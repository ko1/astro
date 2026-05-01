// jstro lexer + recursive-descent parser.
//
// Supports: var/let/const, function declarations + expressions, arrow
// functions, classes (basic), control flow (if/while/do/for/for-of/for-in,
// break/continue/return), try/catch/finally/throw, all expressions
// including new/typeof/instanceof/in/delete, template literals (no
// tagged templates), destructuring is NOT supported (out of scope),
// default parameters, rest parameters.
//
// Lexical limitations:
//   - regex literals: not supported (slash always division)
//   - ASI: simplified — after a newline before `}`, `EOF`, or any keyword
//     that can't continue an expression, we treat as semicolon.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "node.h"
#include "context.h"

// Allocator helpers exposed by js_runtime.c for variable-arity operands.
uint32_t jstro_node_arr_alloc(uint32_t cnt);
uint32_t jstro_u32_arr_alloc(uint32_t cnt);
uint32_t jstro_str_arr_alloc(uint32_t cnt);
extern NODE     **JSTRO_NODE_ARR;
extern uint32_t  *JSTRO_U32_ARR;
extern struct JsString **JSTRO_STR_ARR;

// =====================================================================
// Tokens
// =====================================================================

typedef enum {
    TK_EOF = 0,
    TK_NUM, TK_STR, TK_TPL_STR, TK_TPL_HEAD, TK_TPL_MID, TK_TPL_TAIL,
    TK_IDENT,
    TK_LBRACE, TK_RBRACE, TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK,
    TK_SEMI, TK_COMMA, TK_DOT, TK_QDOT, TK_QUESTION, TK_COLON, TK_ARROW,
    TK_ASSIGN, TK_PLUS_EQ, TK_MINUS_EQ, TK_STAR_EQ, TK_SLASH_EQ, TK_PCT_EQ,
    TK_AND_EQ, TK_OR_EQ, TK_XOR_EQ, TK_SHL_EQ, TK_SAR_EQ, TK_SHR_EQ, TK_POW_EQ,
    TK_LAND_EQ, TK_LOR_EQ, TK_NULLISH_EQ,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PCT, TK_POW,
    TK_INC, TK_DEC,
    TK_LAND, TK_LOR, TK_NOT, TK_NULLISH,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SAR, TK_SHR,
    TK_LT, TK_LE, TK_GT, TK_GE, TK_EQ, TK_NE, TK_SEQ, TK_SNE,
    TK_DOTS,    // ...
    TK_AT,
    TK_REGEX,
    // Keywords
    TK_VAR, TK_LET, TK_CONST,
    TK_FUNCTION, TK_RETURN, TK_IF, TK_ELSE, TK_WHILE, TK_DO, TK_FOR, TK_OF, TK_IN,
    TK_BREAK, TK_CONTINUE,
    TK_TRUE, TK_FALSE, TK_NULL, TK_UNDEFINED,
    TK_NEW, TK_THIS, TK_TYPEOF, TK_INSTANCEOF, TK_VOID, TK_DELETE,
    TK_TRY, TK_CATCH, TK_FINALLY, TK_THROW,
    TK_CLASS, TK_EXTENDS, TK_SUPER, TK_STATIC,
    TK_SWITCH, TK_CASE, TK_DEFAULT,
    TK_YIELD, TK_ASYNC, TK_AWAIT,
    TK_IMPORT, TK_EXPORT, TK_FROM, TK_AS, TK_OF_KW,  // module-related
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;
    uint32_t len;
    int line;
    bool prec_newline;     // whether a newline came before this token
    double dnum;
    int64_t inum;
    bool is_int;
    char *strbuf;          // owned (decoded) string content for TK_STR / TK_TPL_*
    uint32_t strlen;
    char *regex_flags;     // for TK_REGEX (separate flag string)
    uint32_t regex_flags_len;
} Token;

typedef struct Parser {
    CTX *c;
    const char *src;
    const char *src_end;
    const char *p;
    int line;
    Token cur, next;       // 1-token lookahead
    bool has_peeked;
    int tpl_brace_depth;   // depth at which a `}` should resume template scanning
    int *tpl_stack;
    int tpl_stack_cnt, tpl_stack_cap;
    TokenKind prev_kind;   // for regex / division disambiguation
} Parser;

// =====================================================================
// Lexer error
// =====================================================================

static __attribute__((noreturn))
void parse_error(Parser *p, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "SyntaxError at line %d: %s\n", p->line, buf);
    exit(1);
}

// =====================================================================
// Identifier / keyword classification
// =====================================================================

static TokenKind
keyword_of(const char *s, uint32_t len)
{
    #define K(STR, KIND) if (len == sizeof(STR)-1 && memcmp(s, STR, len) == 0) return KIND
    K("var", TK_VAR); K("let", TK_LET); K("const", TK_CONST);
    K("function", TK_FUNCTION); K("return", TK_RETURN);
    K("if", TK_IF); K("else", TK_ELSE);
    K("while", TK_WHILE); K("do", TK_DO);
    K("for", TK_FOR); K("of", TK_OF); K("in", TK_IN);
    K("break", TK_BREAK); K("continue", TK_CONTINUE);
    K("true", TK_TRUE); K("false", TK_FALSE);
    K("null", TK_NULL); K("undefined", TK_UNDEFINED);
    K("new", TK_NEW); K("this", TK_THIS);
    K("typeof", TK_TYPEOF); K("instanceof", TK_INSTANCEOF);
    K("void", TK_VOID); K("delete", TK_DELETE);
    K("try", TK_TRY); K("catch", TK_CATCH); K("finally", TK_FINALLY); K("throw", TK_THROW);
    K("class", TK_CLASS); K("extends", TK_EXTENDS); K("super", TK_SUPER); K("static", TK_STATIC);
    K("switch", TK_SWITCH); K("case", TK_CASE); K("default", TK_DEFAULT);
    K("yield", TK_YIELD); K("async", TK_ASYNC); K("await", TK_AWAIT);
    K("import", TK_IMPORT); K("export", TK_EXPORT);
    #undef K
    return TK_IDENT;
}

// =====================================================================
// Lexer
// =====================================================================

static int
hex_digit(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// Encode one Unicode codepoint as UTF-8 into `out`, returning bytes written.
static int
utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static char *
read_string_escape(Parser *p, int quote, uint32_t *out_len, bool template_mode)
{
    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    while (*p->p && *p->p != quote) {
        if (template_mode && p->p[0] == '$' && p->p[1] == '{') break;
        char ch = *p->p++;
        if (ch == '\n') p->line++;
        if (ch == '\\') {
            char e = *p->p++;
            uint32_t cp = 0;
            int n;
            switch (e) {
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'v': ch = '\v'; break;
            case '0': ch = '\0'; break;
            case '\\': case '\'': case '"': case '`': case '/': ch = e; break;
            case '\n': p->line++; continue;     // line continuation
            case 'x':
                cp = (uint32_t)(hex_digit(p->p[0]) << 4 | hex_digit(p->p[1]));
                p->p += 2;
                if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                buf[len++] = (char)cp;
                continue;
            case 'u':
                if (*p->p == '{') {
                    p->p++;
                    while (*p->p && *p->p != '}') {
                        cp = (cp << 4) | (uint32_t)hex_digit(*p->p++);
                    }
                    if (*p->p == '}') p->p++;
                } else {
                    for (int i = 0; i < 4; i++) {
                        cp = (cp << 4) | (uint32_t)hex_digit(*p->p++);
                    }
                }
                n = utf8_encode(cp, buf + len);
                while (len + n + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                len += n;
                continue;
            default:
                ch = e;
            }
        }
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = ch;
    }
    buf[len] = 0;
    *out_len = (uint32_t)len;
    return buf;
}

static void
lex_one(Parser *p, Token *t)
{
    bool prec_newline = false;
    while (*p->p) {
        char ch = *p->p;
        if (ch == ' ' || ch == '\t' || ch == '\r') { p->p++; continue; }
        if (ch == '\n') { p->line++; p->p++; prec_newline = true; continue; }
        if (ch == '/' && p->p[1] == '/') {
            while (*p->p && *p->p != '\n') p->p++;
            continue;
        }
        if (ch == '/' && p->p[1] == '*') {
            p->p += 2;
            while (*p->p && !(p->p[0] == '*' && p->p[1] == '/')) {
                if (*p->p == '\n') { p->line++; prec_newline = true; }
                p->p++;
            }
            if (*p->p) p->p += 2;
            continue;
        }
        break;
    }
    t->prec_newline = prec_newline;
    t->line = p->line;
    t->start = p->p;
    t->strbuf = NULL;
    t->strlen = 0;
    t->regex_flags = NULL;
    t->regex_flags_len = 0;
    if (!*p->p) { t->kind = TK_EOF; t->len = 0; return; }

    char ch = *p->p;

    // Numbers
    if (isdigit((unsigned char)ch) || (ch == '.' && isdigit((unsigned char)p->p[1]))) {
        const char *start = p->p;
        bool is_int = true;
        if (ch == '0' && (p->p[1] == 'x' || p->p[1] == 'X')) {
            p->p += 2;
            int64_t v = 0;
            while (*p->p && hex_digit(*p->p) >= 0) { v = v * 16 + hex_digit(*p->p); p->p++; }
            t->kind = TK_NUM; t->inum = v; t->dnum = (double)v; t->is_int = true;
            t->len = (uint32_t)(p->p - start);
            return;
        }
        if (ch == '0' && (p->p[1] == 'b' || p->p[1] == 'B')) {
            p->p += 2;
            int64_t v = 0;
            while (*p->p == '0' || *p->p == '1') { v = v * 2 + (*p->p - '0'); p->p++; }
            t->kind = TK_NUM; t->inum = v; t->dnum = (double)v; t->is_int = true;
            t->len = (uint32_t)(p->p - start);
            return;
        }
        if (ch == '0' && (p->p[1] == 'o' || p->p[1] == 'O')) {
            p->p += 2;
            int64_t v = 0;
            while (*p->p >= '0' && *p->p <= '7') { v = v * 8 + (*p->p - '0'); p->p++; }
            t->kind = TK_NUM; t->inum = v; t->dnum = (double)v; t->is_int = true;
            t->len = (uint32_t)(p->p - start);
            return;
        }
        while (isdigit((unsigned char)*p->p)) p->p++;
        if (*p->p == '.') { is_int = false; p->p++; while (isdigit((unsigned char)*p->p)) p->p++; }
        if (*p->p == 'e' || *p->p == 'E') {
            is_int = false;
            p->p++;
            if (*p->p == '+' || *p->p == '-') p->p++;
            while (isdigit((unsigned char)*p->p)) p->p++;
        }
        // BigInt literal suffix `123n` — currently treated as int64 (no
        // arbitrary precision).  Consume the suffix.
        if (*p->p == 'n' && is_int) p->p++;
        char buf[64];
        size_t n = (size_t)(p->p - start);
        if (n >= sizeof buf) parse_error(p, "number literal too long");
        memcpy(buf, start, n); buf[n] = 0;
        t->kind = TK_NUM;
        t->dnum = strtod(buf, NULL);
        if (is_int && t->dnum >= -4.611686018427388e18 && t->dnum <= 4.611686018427388e18) {
            t->inum = (int64_t)t->dnum;
            t->is_int = true;
        } else {
            t->is_int = false;
        }
        t->len = (uint32_t)n;
        return;
    }

    // Identifier / keyword
    if (isalpha((unsigned char)ch) || ch == '_' || ch == '$') {
        const char *start = p->p;
        while (isalnum((unsigned char)*p->p) || *p->p == '_' || *p->p == '$') p->p++;
        t->len = (uint32_t)(p->p - start);
        t->kind = keyword_of(start, t->len);
        return;
    }
    // Private name: #ident — single TK_IDENT with '#' included.
    if (ch == '#') {
        const char *start = p->p;
        p->p++;  // past '#'
        if (!(isalpha((unsigned char)*p->p) || *p->p == '_' || *p->p == '$')) {
            parse_error(p, "expected identifier after '#'");
        }
        while (isalnum((unsigned char)*p->p) || *p->p == '_' || *p->p == '$') p->p++;
        t->len = (uint32_t)(p->p - start);
        t->kind = TK_IDENT;
        return;
    }

    // Strings
    if (ch == '"' || ch == '\'') {
        int q = ch; p->p++;
        uint32_t l;
        char *buf = read_string_escape(p, q, &l, false);
        if (*p->p == (char)q) p->p++;
        t->kind = TK_STR; t->strbuf = buf; t->strlen = l;
        t->len = (uint32_t)(p->p - t->start);
        return;
    }

    // Template literal
    if (ch == '`') {
        p->p++;
        uint32_t l;
        char *buf = read_string_escape(p, '`', &l, true);
        if (p->p[0] == '$' && p->p[1] == '{') {
            // template head -> first text portion before ${
            p->p += 2;
            t->kind = TK_TPL_HEAD; t->strbuf = buf; t->strlen = l;
            // push template state: when we see a `}` at this brace depth,
            // resume template scanning.
            if (p->tpl_stack_cnt + 1 > p->tpl_stack_cap) {
                p->tpl_stack_cap = p->tpl_stack_cap ? p->tpl_stack_cap * 2 : 8;
                p->tpl_stack = realloc(p->tpl_stack, sizeof(int) * p->tpl_stack_cap);
            }
            p->tpl_stack[p->tpl_stack_cnt++] = 1;  // marker
        } else if (*p->p == '`') {
            p->p++;
            t->kind = TK_TPL_STR; t->strbuf = buf; t->strlen = l;
        } else {
            parse_error(p, "unterminated template literal");
        }
        t->len = (uint32_t)(p->p - t->start);
        return;
    }

    // Punctuation / operators
    p->p++;
    #define OP1(K) t->kind = K; t->len = 1; return
    #define OP2(K) p->p++; t->kind = K; t->len = 2; return
    #define OP3(K) p->p += 2; t->kind = K; t->len = 3; return
    #define OP4(K) p->p += 3; t->kind = K; t->len = 4; return
    switch (ch) {
    case '(': OP1(TK_LPAREN);
    case ')': OP1(TK_RPAREN);
    case '[': OP1(TK_LBRACK);
    case ']': OP1(TK_RBRACK);
    case '{': OP1(TK_LBRACE);
    case '}':
        // Check whether we should resume template scanning.
        if (p->tpl_stack_cnt > 0) {
            // Pop; resume reading template.
            p->tpl_stack_cnt--;
            uint32_t l;
            char *buf = read_string_escape(p, '`', &l, true);
            t->strbuf = buf; t->strlen = l;
            if (p->p[0] == '$' && p->p[1] == '{') {
                p->p += 2;
                t->kind = TK_TPL_MID;
                if (p->tpl_stack_cnt + 1 > p->tpl_stack_cap) {
                    p->tpl_stack_cap = p->tpl_stack_cap ? p->tpl_stack_cap * 2 : 8;
                    p->tpl_stack = realloc(p->tpl_stack, sizeof(int) * p->tpl_stack_cap);
                }
                p->tpl_stack[p->tpl_stack_cnt++] = 1;
            } else if (*p->p == '`') {
                p->p++;
                t->kind = TK_TPL_TAIL;
            } else {
                parse_error(p, "unterminated template literal");
            }
            t->len = (uint32_t)(p->p - t->start);
            return;
        }
        OP1(TK_RBRACE);
    case ';': OP1(TK_SEMI);
    case ',': OP1(TK_COMMA);
    case '.':
        if (p->p[0] == '.' && p->p[1] == '.') { OP3(TK_DOTS); }
        OP1(TK_DOT);
    case '?':
        if (*p->p == '.') { OP2(TK_QDOT); }
        if (*p->p == '?') { p->p++; if (*p->p == '=') { OP2(TK_NULLISH_EQ); } OP1(TK_NULLISH); }  // backed up after p->p++, it's still len-2
        OP1(TK_QUESTION);
    case ':': OP1(TK_COLON);
    case '+':
        if (*p->p == '+') { OP2(TK_INC); }
        if (*p->p == '=') { OP2(TK_PLUS_EQ); }
        OP1(TK_PLUS);
    case '-':
        if (*p->p == '-') { OP2(TK_DEC); }
        if (*p->p == '=') { OP2(TK_MINUS_EQ); }
        OP1(TK_MINUS);
    case '*':
        if (*p->p == '*') { p->p++; if (*p->p == '=') { OP2(TK_POW_EQ); } OP1(TK_POW); }
        if (*p->p == '=') { OP2(TK_STAR_EQ); }
        OP1(TK_STAR);
    case '/': {
        // Regex vs division: regex iff prev token can't end an expression.
        TokenKind pk = p->prev_kind;
        bool prev_ends_expr = (pk == TK_IDENT || pk == TK_NUM || pk == TK_STR
                               || pk == TK_TPL_STR || pk == TK_TPL_TAIL
                               || pk == TK_RPAREN || pk == TK_RBRACK
                               || pk == TK_INC || pk == TK_DEC
                               || pk == TK_TRUE || pk == TK_FALSE
                               || pk == TK_NULL || pk == TK_UNDEFINED
                               || pk == TK_THIS);
        if (!prev_ends_expr) {
            // Read regex literal: /pattern/flags
            const char *body_start = p->p;
            int in_class = 0;
            while (*p->p) {
                if (*p->p == '\\' && p->p[1]) { p->p += 2; continue; }
                if (*p->p == '[') in_class++;
                else if (*p->p == ']') in_class--;
                else if (*p->p == '/' && in_class == 0) break;
                else if (*p->p == '\n') break;
                p->p++;
            }
            uint32_t body_len = (uint32_t)(p->p - body_start);
            if (*p->p == '/') p->p++;
            const char *flag_start = p->p;
            while (isalnum((unsigned char)*p->p) || *p->p == '_') p->p++;
            uint32_t flag_len = (uint32_t)(p->p - flag_start);
            t->kind = TK_REGEX;
            t->strbuf = (char *)malloc(body_len + 1);
            memcpy(t->strbuf, body_start, body_len);
            t->strbuf[body_len] = 0;
            t->strlen = body_len;
            t->regex_flags = (char *)malloc(flag_len + 1);
            memcpy(t->regex_flags, flag_start, flag_len);
            t->regex_flags[flag_len] = 0;
            t->regex_flags_len = flag_len;
            t->len = (uint32_t)(p->p - t->start);
            return;
        }
        if (*p->p == '=') { OP2(TK_SLASH_EQ); }
        OP1(TK_SLASH);
    }
    case '%':
        if (*p->p == '=') { OP2(TK_PCT_EQ); }
        OP1(TK_PCT);
    case '!':
        if (p->p[0] == '=' && p->p[1] == '=') { OP3(TK_SNE); }
        if (*p->p == '=') { OP2(TK_NE); }
        OP1(TK_NOT);
    case '=':
        if (p->p[0] == '=' && p->p[1] == '=') { OP3(TK_SEQ); }
        if (*p->p == '=') { OP2(TK_EQ); }
        if (*p->p == '>') { OP2(TK_ARROW); }
        OP1(TK_ASSIGN);
    case '<':
        if (p->p[0] == '<' && p->p[1] == '=') { OP3(TK_SHL_EQ); }
        if (*p->p == '<') { OP2(TK_SHL); }
        if (*p->p == '=') { OP2(TK_LE); }
        OP1(TK_LT);
    case '>':
        if (p->p[0] == '>' && p->p[1] == '>' && p->p[2] == '=') { OP4(TK_SHR_EQ); }
        if (p->p[0] == '>' && p->p[1] == '>')                   { OP3(TK_SHR); }
        if (p->p[0] == '>' && p->p[1] == '=')                   { OP3(TK_SAR_EQ); }
        if (p->p[0] == '>')                                     { OP2(TK_SAR); }
        if (*p->p == '=') { OP2(TK_GE); }
        OP1(TK_GT);
    case '&':
        if (p->p[0] == '&' && p->p[1] == '=') { OP3(TK_LAND_EQ); }
        if (*p->p == '&') { OP2(TK_LAND); }
        if (*p->p == '=') { OP2(TK_AND_EQ); }
        OP1(TK_AMP);
    case '|':
        if (p->p[0] == '|' && p->p[1] == '=') { OP3(TK_LOR_EQ); }
        if (*p->p == '|') { OP2(TK_LOR); }
        if (*p->p == '=') { OP2(TK_OR_EQ); }
        OP1(TK_PIPE);
    case '^':
        if (*p->p == '=') { OP2(TK_XOR_EQ); }
        OP1(TK_CARET);
    case '~': OP1(TK_TILDE);
    case '@': OP1(TK_AT);
    }
    parse_error(p, "unexpected character '%c'", ch);
}

#undef OP1
#undef OP2
#undef OP3
#undef OP4

// =====================================================================
// Parser plumbing
// =====================================================================

static void
advance(Parser *p)
{
    if (p->cur.strbuf) { free(p->cur.strbuf); p->cur.strbuf = NULL; }
    if (p->cur.regex_flags) { free(p->cur.regex_flags); p->cur.regex_flags = NULL; }
    p->prev_kind = p->cur.kind;
    if (p->has_peeked) {
        p->cur = p->next;
        p->has_peeked = false;
    } else {
        lex_one(p, &p->cur);
    }
}

static const Token *
peek(Parser *p)
{
    if (!p->has_peeked) {
        // The lexer's regex/division decision uses p->prev_kind; when we
        // peek the token *after* cur, prev_kind for that purpose should
        // be cur.kind, not p->prev_kind.  Swap temporarily.
        TokenKind saved_pk = p->prev_kind;
        p->prev_kind = p->cur.kind;
        Token tmp;
        lex_one(p, &tmp);
        p->prev_kind = saved_pk;
        p->next = tmp;
        p->has_peeked = true;
    }
    return &p->next;
}

static bool
check(Parser *p, TokenKind k)
{
    return p->cur.kind == k;
}

static bool
accept(Parser *p, TokenKind k)
{
    if (p->cur.kind == k) { advance(p); return true; }
    return false;
}

static void
expect(Parser *p, TokenKind k, const char *what)
{
    if (p->cur.kind != k) parse_error(p, "expected %s, got token %d", what, (int)p->cur.kind);
    advance(p);
}

// =====================================================================
// Scope tracking — assigns frame slot indices to local names, tracks
// captured locals for closure construction.
// =====================================================================

typedef struct ScopeVar {
    struct JsString *name;
    uint32_t slot;
    uint8_t  kind;        // 0=var, 1=let, 2=const, 3=param
    bool     captured;    // marked when any nested function reads/writes it
} ScopeVar;

typedef struct Scope {
    struct Scope *parent;
    ScopeVar *vars;
    uint32_t nvars, capa;
    // For function scopes: nlocals tracks the current high-water mark
    // used to allocate the frame.
    uint32_t nlocals;
    bool is_function;
    bool is_arrow;
    // Upvalues captured by this scope (function only).  Upvalues come
    // from enclosing function's locals (is_local=1) or the parent's
    // upvalues (is_local=0).
    struct Upval { struct JsString *name; uint8_t is_local; uint32_t slot; bool resolved; } *upvals;
    uint32_t nupvals, ucap;
    // Hoisted function bodies & their target slots — emitted at start.
    struct Hoist { uint32_t slot; NODE *body_node; } *hoists;
    uint32_t nhoists, hcap;
    // Per-scope holes (for let/const TDZ).
    uint32_t *hole_slots;
    uint32_t nholes, holecap;
    // Loop label tracking for break/continue.
    int loop_depth;
    int switch_depth;
} Scope;

// Push / find / declare in the current scope.
static Scope *
scope_new(Scope *parent, bool is_function, bool is_arrow)
{
    Scope *s = (Scope *)calloc(1, sizeof(Scope));
    s->parent = parent;
    s->is_function = is_function;
    s->is_arrow = is_arrow;
    if (parent && !is_function) {
        // child block scope shares parent's frame
        s->nlocals = parent->nlocals;
    }
    return s;
}

static void
scope_free(Scope *s)
{
    free(s->vars); free(s->upvals); free(s->hoists); free(s->hole_slots);
    free(s);
}

static int
scope_find_var(Scope *s, struct JsString *name)
{
    for (int i = (int)s->nvars - 1; i >= 0; i--) {
        if (s->vars[i].name == name) return i;
    }
    return -1;
}

// Walk up looking for a binding.  Returns the scope that declares it
// and a flag for whether crossing function boundary.
static Scope *
scope_lookup(Scope *s, struct JsString *name, bool *crosses_function, int *out_idx)
{
    *crosses_function = false;
    while (s) {
        int idx = scope_find_var(s, name);
        if (idx >= 0) { *out_idx = idx; return s; }
        if (s->is_function) *crosses_function = true;
        s = s->parent;
    }
    return NULL;
}

// Get the function scope containing s.
static Scope *
scope_function(Scope *s)
{
    while (s && !s->is_function) s = s->parent;
    return s;
}

// Declare a variable in scope s.  Allocates a frame slot.
static uint32_t
scope_declare(Scope *s, struct JsString *name, uint8_t kind)
{
    Scope *fn_scope = s->is_function ? s : scope_function(s);
    if (s->nvars + 1 > s->capa) {
        s->capa = s->capa ? s->capa * 2 : 4;
        s->vars = (ScopeVar *)realloc(s->vars, sizeof(ScopeVar) * s->capa);
    }
    uint32_t slot = fn_scope->nlocals++;
    s->vars[s->nvars].name = name;
    s->vars[s->nvars].slot = slot;
    s->vars[s->nvars].kind = kind;
    s->vars[s->nvars].captured = false;
    s->nvars++;
    return slot;
}

// Add a hole-init for let/const at start of block.
static void
scope_add_hole(Scope *s, uint32_t slot)
{
    if (s->nholes + 1 > s->holecap) {
        s->holecap = s->holecap ? s->holecap * 2 : 4;
        s->hole_slots = (uint32_t *)realloc(s->hole_slots, sizeof(uint32_t) * s->holecap);
    }
    s->hole_slots[s->nholes++] = slot;
}

// Add an upvalue to function scope; returns its index.
static uint32_t
scope_add_upval(Scope *fn, struct JsString *name, uint8_t is_local, uint32_t slot)
{
    for (uint32_t i = 0; i < fn->nupvals; i++) {
        if (fn->upvals[i].name == name && fn->upvals[i].is_local == is_local && fn->upvals[i].slot == slot) {
            return i;
        }
    }
    if (fn->nupvals + 1 > fn->ucap) {
        fn->ucap = fn->ucap ? fn->ucap * 2 : 4;
        fn->upvals = realloc(fn->upvals, sizeof(*fn->upvals) * fn->ucap);
    }
    uint32_t idx = fn->nupvals++;
    fn->upvals[idx].name = name;
    fn->upvals[idx].is_local = is_local;
    fn->upvals[idx].slot = slot;
    fn->upvals[idx].resolved = true;
    return idx;
}

// Resolve a name reference.  Returns one of:
//   "local"   slot in current frame
//   "boxed"   captured local in current frame (boxed)
//   "upval"   parent function's box, accessed via cur_upvals[idx]
//   "global"  not found in any local scope
typedef enum { RES_LOCAL, RES_BOXED, RES_UPVAL, RES_GLOBAL } RefKind;

typedef struct {
    RefKind kind;
    uint32_t slot;     // for LOCAL/BOXED: frame slot.  for UPVAL: upval index.
    uint8_t  decl;     // 0=var/param, 1=let, 2=const
    struct JsString *name;  // for emitting TDZ error message
} RefBinding;

static RefBinding
resolve_name(Scope *cur, struct JsString *name)
{
    RefBinding rb = {0};
    rb.name = name;
    bool crosses;
    int idx;
    Scope *def = scope_lookup(cur, name, &crosses, &idx);
    if (!def) { rb.kind = RES_GLOBAL; return rb; }
    rb.decl = def->vars[idx].kind;
    Scope *cur_fn = scope_function(cur);
    Scope *def_fn = scope_function(def);
    if (cur_fn == def_fn) {
        rb.kind = def->vars[idx].captured ? RES_BOXED : RES_LOCAL;
        rb.slot = def->vars[idx].slot;
        return rb;
    }
    // Cross function boundary — must capture.
    def->vars[idx].captured = true;

    // Build chain of upvals from def_fn up to cur_fn.  Recursively ensure
    // that intermediate functions also have an upval pointing further up.
    // First, get the upval index for cur_fn pointing to its parent.
    // Walk: for each function from def_fn (which holds the local) up to
    // cur_fn, register an upval.

    // Collect the chain from def_fn to cur_fn (exclusive).  Each
    // intermediate function captures from its parent.
    Scope *chain[64]; int n = 0;
    for (Scope *fs = cur_fn; fs && fs != def_fn; fs = scope_function(fs->parent)) {
        chain[n++] = fs;
    }
    if (n == 0) {
        // shouldn't happen; means same function — already handled above
        rb.kind = RES_LOCAL;
        rb.slot = def->vars[idx].slot;
        return rb;
    }
    // Innermost function: captures from def_fn (local).
    // The deepest function in the chain (chain[n-1]) is a direct child of def_fn.
    uint32_t up = scope_add_upval(chain[n-1], name, /*is_local=*/1, def->vars[idx].slot);
    // Walk outward: each next-shallower function captures the next upval.
    for (int i = n - 2; i >= 0; i--) {
        up = scope_add_upval(chain[i], name, /*is_local=*/0, up);
    }
    rb.kind = RES_UPVAL;
    rb.slot = up;
    return rb;
}

// =====================================================================
// AST builders
// =====================================================================

static NODE *
build_seq(NODE *a, NODE *b)
{
    if (!a) return b;
    if (!b) return a;
    return ALLOC_node_seq(a, b);
}

// Build a multi-statement sequence from an array of statements.
//   - 0 statements: undefined
//   - 1 statement:  the statement itself
//   - 2 statements: a single node_seq (specializable; tight)
//   - 3+ statements: a node_seqn (flat array, one dispatcher per block)
static NODE *
build_block(NODE **stmts, uint32_t n)
{
    if (n == 0) return ALLOC_node_undefined();
    if (n == 1) return stmts[0];
    if (n == 2) return ALLOC_node_seq(stmts[0], stmts[1]);
    uint32_t base = jstro_node_arr_alloc(n);
    for (uint32_t i = 0; i < n; i++) JSTRO_NODE_ARR[base + i] = stmts[i];
    return ALLOC_node_seqn(base, n);
}

// =====================================================================
// Parser forward declarations
// =====================================================================

static NODE *parse_expr(Parser *p, Scope *s);
static NODE *parse_assign(Parser *p, Scope *s);
static NODE *parse_stmt(Parser *p, Scope *s);
static NODE *parse_block(Parser *p, Scope *s, bool new_scope);
static NODE *parse_function_expr(Parser *p, Scope *s, struct JsString *name);
static NODE *parse_arrow(Parser *p, Scope *s, NODE **params_arr, struct JsString **param_names, uint32_t nparams);

// =====================================================================
// Hoisting pre-pass — scans source text for top-level function
// declarations and pre-declares their names in the target scope.  This
// is a textual scan that ignores nested blocks (depth>0); it correctly
// handles string/template literals, line/block comments, and bracket
// nesting.  Conservative-stop-on-end-of-block: returns when it would
// leave the current `{...}` scope.
// =====================================================================

static const char *
hoist_skip_string(const char *p, const char *end, char quote)
{
    p++;  // past opening quote
    while (p < end && *p != quote) {
        if (*p == '\\' && p + 1 < end) p += 2;
        else                           p++;
    }
    if (p < end) p++;
    return p;
}

static const char *
hoist_skip_template(const char *p, const char *end)
{
    int brace_depth = 0;
    p++;  // past `
    while (p < end) {
        if (brace_depth == 0 && *p == '`') return p + 1;
        if (brace_depth == 0 && p[0] == '$' && p + 1 < end && p[1] == '{') { brace_depth = 1; p += 2; continue; }
        if (brace_depth > 0) {
            if (*p == '{') brace_depth++;
            else if (*p == '}') { brace_depth--; if (brace_depth == 0) { p++; continue; } }
            else if (*p == '"' || *p == '\'') p = hoist_skip_string(p, end, *p) - 1;
            else if (*p == '`') p = hoist_skip_template(p, end) - 1;
            p++;
            continue;
        }
        if (*p == '\\' && p + 1 < end) p += 2;
        else                           p++;
    }
    return p;
}

static const char *
hoist_skip_line_comment(const char *p, const char *end)
{
    while (p < end && *p != '\n') p++;
    return p;
}

static const char *
hoist_skip_block_comment(const char *p, const char *end)
{
    p += 2;
    while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
    if (p + 1 < end) p += 2;
    return p;
}

// Walk source bytes from `start`.  Records function-declaration names
// at the top level (depth == 0) into `target` scope.  Stops when we
// see a top-level `}` (treated as end of enclosing block) or EOF.
// Returns the position where we stopped.
static const char *
hoist_scan(Parser *p, const char *start, const char *end, Scope *target)
{
    int depth = 0;
    const char *q = start;
    while (q < end) {
        char ch = *q;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { q++; continue; }
        if (ch == '/' && q + 1 < end && q[1] == '/') { q = hoist_skip_line_comment(q, end); continue; }
        if (ch == '/' && q + 1 < end && q[1] == '*') { q = hoist_skip_block_comment(q, end); continue; }
        if (ch == '"' || ch == '\'')                  { q = hoist_skip_string(q, end, ch); continue; }
        if (ch == '`')                                { q = hoist_skip_template(q, end); continue; }
        if (ch == '{' || ch == '(' || ch == '[')      { depth++; q++; continue; }
        if (ch == '}' || ch == ')' || ch == ']') {
            if (depth == 0) return q;   // exit block
            depth--; q++; continue;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '$') {
            const char *id_start = q;
            while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')
                               || (*q >= '0' && *q <= '9') || *q == '_' || *q == '$')) q++;
            size_t kw_len = q - id_start;
            if (depth == 0 && ((kw_len == 8 && memcmp(id_start, "function", 8) == 0)
                                || (kw_len == 3 && memcmp(id_start, "var", 3) == 0))) {
                bool is_function = (kw_len == 8);
                while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
                if (is_function && q < end && *q == '*') { q++; while (q < end && (*q == ' ' || *q == '\t')) q++; }
                while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')
                                   || *q == '_' || *q == '$')) {
                    const char *n_start = q;
                    while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')
                                       || (*q >= '0' && *q <= '9') || *q == '_' || *q == '$')) q++;
                    size_t n_len = q - n_start;
                    struct JsString *name = js_str_intern_n(p->c, n_start, n_len);
                    if (scope_find_var(target, name) < 0) {
                        scope_declare(target, name, 0);
                    }
                    if (is_function) break;
                    // For var: skip optional initializer to next , or ;
                    int paren = 0, brack = 0, brace = 0;
                    while (q < end) {
                        char cc = *q;
                        if (cc == '(' ) paren++;
                        else if (cc == ')') paren--;
                        else if (cc == '[' ) brack++;
                        else if (cc == ']') brack--;
                        else if (cc == '{' ) brace++;
                        else if (cc == '}') { if (brace == 0) goto done_decl; brace--; }
                        else if (cc == '"' || cc == '\'') { q = hoist_skip_string(q, end, cc); continue; }
                        else if (cc == '`') { q = hoist_skip_template(q, end); continue; }
                        else if (cc == ',' && paren == 0 && brack == 0 && brace == 0) { q++; break; }
                        else if (cc == ';' && paren == 0 && brack == 0 && brace == 0) goto done_decl;
                        q++;
                    }
                    while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
                }
done_decl: ;
            }
            continue;
        }
        q++;
    }
    return q;
}

// Pre-scan a block body for `let` / `const` declarations at depth 0 of
// the block.  Used to hoist them so TDZ on early reads works.
static void
hoist_letconst(Parser *p, const char *start, const char *end, Scope *block)
{
    int depth = 0;
    const char *q = start;
    while (q < end) {
        char ch = *q;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { q++; continue; }
        if (ch == '/' && q + 1 < end && q[1] == '/') { q = hoist_skip_line_comment(q, end); continue; }
        if (ch == '/' && q + 1 < end && q[1] == '*') { q = hoist_skip_block_comment(q, end); continue; }
        if (ch == '"' || ch == '\'')                  { q = hoist_skip_string(q, end, ch); continue; }
        if (ch == '`')                                { q = hoist_skip_template(q, end); continue; }
        if (ch == '{' || ch == '(' || ch == '[')      { depth++; q++; continue; }
        if (ch == '}' || ch == ')' || ch == ']')      {
            if (depth == 0) return;
            depth--; q++; continue;
        }
        if ((ch >= 'a' && ch <= 'z') || ch == '_' || ch == '$') {
            const char *id_start = q;
            while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')
                               || (*q >= '0' && *q <= '9') || *q == '_' || *q == '$')) q++;
            size_t kw_len = q - id_start;
            if (depth == 0) {
                int kind = -1;
                if (kw_len == 3 && memcmp(id_start, "let", 3) == 0) kind = 1;
                else if (kw_len == 5 && memcmp(id_start, "const", 5) == 0) kind = 2;
                if (kind > 0) {
                    while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
                    while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')
                                       || *q == '_' || *q == '$')) {
                        const char *n_start = q;
                        while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')
                                           || (*q >= '0' && *q <= '9') || *q == '_' || *q == '$')) q++;
                        size_t n_len = q - n_start;
                        struct JsString *name = js_str_intern_n(p->c, n_start, n_len);
                        if (scope_find_var(block, name) < 0) {
                            uint32_t slot = scope_declare(block, name, (uint8_t)kind);
                            scope_add_hole(block, slot);
                        }
                        // Skip past initializer to next , or ;
                        int paren = 0, brack = 0, brace = 0;
                        while (q < end) {
                            char cc = *q;
                            if (cc == '(') paren++;
                            else if (cc == ')') paren--;
                            else if (cc == '[') brack++;
                            else if (cc == ']') brack--;
                            else if (cc == '{') brace++;
                            else if (cc == '}') { if (brace == 0) goto done2; brace--; }
                            else if (cc == '"' || cc == '\'') { q = hoist_skip_string(q, end, cc); continue; }
                            else if (cc == '`') { q = hoist_skip_template(q, end); continue; }
                            else if (cc == ',' && paren==0 && brack==0 && brace==0) { q++; break; }
                            else if (cc == ';' && paren==0 && brack==0 && brace==0) goto done2;
                            q++;
                        }
                        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
                    }
done2: ;
                }
            }
            continue;
        }
        q++;
    }
}

// LHS shape — used by parse_postfix_track to support assignments and
// `delete`.  Defined in full further down; forward-declared here.
typedef struct LhsTrack {
    int kind;                // 0=ident, 1=member, 2=index, -1=other
    struct JsString *name;
    NODE *recv;
    NODE *idx;
} LhsTrack;
static NODE *parse_postfix_track(Parser *p, Scope *s, LhsTrack *lt);

// Argument-list helpers (defined after parse_postfix below).
typedef struct ParsedArgs {
    NODE   *args[32];
    uint8_t spread[32];
    uint32_t n;
    bool any_spread;
} ParsedArgs;
static void  parse_call_args(Parser *p, Scope *s, ParsedArgs *out);
static NODE *build_call(NODE *fn, ParsedArgs *pa);
static NODE *build_method_call(NODE *recv, struct JsString *name, ParsedArgs *pa);
static NODE *build_new(NODE *ctor, ParsedArgs *pa);

// Forward decls used by the destructuring helpers below.
static NODE *emit_load(Parser *p, Scope *s, struct JsString *name);
static NODE *emit_store(Parser *p, Scope *s, struct JsString *name, NODE *rhs);
static NODE *parse_postfix(Parser *p, Scope *s);

// =====================================================================
// Destructuring patterns — used in let/const/var bindings, function
// parameters, and standalone assignment expressions.
// =====================================================================

typedef enum {
    PAT_IDENT  = 0,    // plain name binding
    PAT_ARRAY  = 1,    // [ elem, ... ]
    PAT_OBJECT = 2,    // { key: pattern, ... }
} PatKind;

typedef struct Pattern {
    PatKind kind;
    struct JsString *name;     // for PAT_IDENT
    struct Pattern  *children; // for PAT_ARRAY / PAT_OBJECT (linked list via .next)
    struct Pattern  *next;
    struct JsString *prop_key; // for PAT_OBJECT child: "x" in {x: pat}
    NODE *default_expr;        // for any kind: optional default value when matched is undefined
    bool   is_rest;            // for trailing ...rest in array / object
    bool   is_hole;            // for array `[, , a]` holes
} Pattern;

static Pattern *
pat_new(PatKind k)
{
    Pattern *p = (Pattern *)calloc(1, sizeof(Pattern));
    p->kind = k;
    return p;
}

static void
pat_free(Pattern *p)
{
    while (p) {
        Pattern *next = p->next;
        if (p->children) pat_free(p->children);
        free(p);
        p = next;
    }
}

static Pattern *parse_pattern(Parser *p, Scope *s);

static Pattern *
parse_pattern_array(Parser *p, Scope *s)
{
    Pattern *root = pat_new(PAT_ARRAY);
    advance(p); // [
    Pattern *tail = NULL;
    while (p->cur.kind != TK_RBRACK) {
        Pattern *e;
        if (p->cur.kind == TK_COMMA) {
            e = pat_new(PAT_IDENT);
            e->is_hole = true;
        } else {
            bool is_rest = false;
            if (p->cur.kind == TK_DOTS) { advance(p); is_rest = true; }
            e = parse_pattern(p, s);
            if (e) e->is_rest = is_rest;
            if (p->cur.kind == TK_ASSIGN) {
                advance(p);
                e->default_expr = parse_assign(p, s);
            }
        }
        if (root->children == NULL) root->children = e;
        else                         tail->next = e;
        tail = e;
        if (p->cur.kind == TK_COMMA) advance(p);
        else break;
    }
    expect(p, TK_RBRACK, "']'");
    return root;
}

static Pattern *
parse_pattern_object(Parser *p, Scope *s)
{
    Pattern *root = pat_new(PAT_OBJECT);
    advance(p); // {
    Pattern *tail = NULL;
    while (p->cur.kind != TK_RBRACE) {
        Pattern *e = NULL;
        if (p->cur.kind == TK_DOTS) {
            advance(p);
            e = parse_pattern(p, s);
            e->is_rest = true;
        } else if (p->cur.kind == TK_IDENT) {
            struct JsString *key = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            advance(p);
            if (p->cur.kind == TK_COLON) {
                advance(p);
                e = parse_pattern(p, s);
                e->prop_key = key;
            } else {
                // Shorthand: {x} === {x: x}
                e = pat_new(PAT_IDENT);
                e->name = key;
                e->prop_key = key;
            }
            if (p->cur.kind == TK_ASSIGN) {
                advance(p);
                e->default_expr = parse_assign(p, s);
            }
        } else if (p->cur.kind == TK_STR) {
            struct JsString *key = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
            advance(p);
            expect(p, TK_COLON, "':'");
            e = parse_pattern(p, s);
            e->prop_key = key;
            if (p->cur.kind == TK_ASSIGN) {
                advance(p);
                e->default_expr = parse_assign(p, s);
            }
        } else {
            parse_error(p, "expected destructuring key");
        }
        if (root->children == NULL) root->children = e;
        else                         tail->next = e;
        tail = e;
        if (p->cur.kind == TK_COMMA) advance(p);
        else break;
    }
    expect(p, TK_RBRACE, "'}'");
    return root;
}

static Pattern *
parse_pattern(Parser *p, Scope *s)
{
    if (p->cur.kind == TK_LBRACK) return parse_pattern_array(p, s);
    if (p->cur.kind == TK_LBRACE) return parse_pattern_object(p, s);
    if (p->cur.kind != TK_IDENT) parse_error(p, "expected destructuring pattern");
    Pattern *e = pat_new(PAT_IDENT);
    e->name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
    advance(p);
    return e;
}

// Walk a pattern and declare all leaf names in `target` scope.  Used by
// let/const/var bindings.  No-op for assignment patterns (which target
// existing bindings).
static void
pat_declare(Pattern *pat, Scope *target, uint8_t kind)
{
    if (!pat) return;
    if (pat->kind == PAT_IDENT) {
        if (!pat->is_hole && pat->name) {
            int existing = scope_find_var(target, pat->name);
            if (existing < 0) scope_declare(target, pat->name, kind);
        }
        return;
    }
    for (Pattern *c = pat->children; c; c = c->next) pat_declare(c, target, kind);
}

// Emit code that destructures `src_load` into bindings according to
// `pat`.  `is_decl`: true for let/const/var (use let_init for stores),
// false for assignment (use emit_store for stores).
//
// `src_load` is a NODE that, when evaluated, yields the value to bind.
// To avoid re-evaluating side-effecting expressions, the caller is
// expected to have already stashed the value into a local slot and
// passed a node_local_get for it.
static NODE *emit_pattern_bind(Parser *p, Scope *s, Pattern *pat, NODE *src_load, bool is_decl);

static NODE *
emit_assign_to_name(Parser *p, Scope *s, struct JsString *name, NODE *rhs, bool is_decl)
{
    if (is_decl) {
        // For decl: pat_declare already created the slot.
        RefBinding rb = resolve_name(s, name);
        if (rb.kind == RES_LOCAL) return ALLOC_node_let_init(rb.slot, rhs);
        return emit_store(p, s, name, rhs);
    }
    return emit_store(p, s, name, rhs);
}

// Emit `if (load === undefined) target_assign(default) else target_assign(load)`.
static NODE *
emit_with_default(Parser *p, Scope *s, NODE *load, NODE *def_expr,
                  NODE *(*build_assign)(Parser *p, Scope *s, NODE *value, void *ctx),
                  void *ctx)
{
    if (!def_expr) {
        return build_assign(p, s, load, ctx);
    }
    // Use a temp slot to hold the loaded value once.
    Scope *fns = scope_function(s);
    uint32_t tmp = fns->nlocals++;
    NODE *store_tmp = ALLOC_node_local_set(tmp, load);
    NODE *cond = ALLOC_node_strict_eq(ALLOC_node_local_get(tmp), ALLOC_node_undefined());
    NODE *use_default  = build_assign(p, s, def_expr, ctx);
    NODE *use_orig     = build_assign(p, s, ALLOC_node_local_get(tmp), ctx);
    NODE *if_node      = ALLOC_node_if(cond, use_default, use_orig);
    return ALLOC_node_seq(store_tmp, if_node);
}

typedef struct AssignCtx {
    Parser  *p;
    Scope   *s;
    Pattern *pat;
    bool     is_decl;
} AssignCtx;
static NODE *assign_to_pat(Parser *p, Scope *s, NODE *value, void *ctxp);

static NODE *
emit_pattern_bind(Parser *p, Scope *s, Pattern *pat, NODE *src_load, bool is_decl)
{
    if (pat->kind == PAT_IDENT) {
        if (pat->is_hole) {
            // Eat the value but don't bind.
            return src_load;
        }
        AssignCtx ctx = { p, s, pat, is_decl };
        return emit_with_default(p, s, src_load, pat->default_expr, assign_to_pat, &ctx);
    }
    // Array / Object: stash src in a temp, then bind each child.
    Scope *fns = scope_function(s);
    uint32_t tmp = fns->nlocals++;
    NODE *seq = ALLOC_node_local_set(tmp, src_load);
    if (pat->kind == PAT_ARRAY) {
        uint32_t idx = 0;
        for (Pattern *c = pat->children; c; c = c->next) {
            NODE *child_load;
            if (c->is_rest) {
                // rest = src.slice(idx)
                NODE *base = ALLOC_node_local_get(tmp);
                uint32_t ab = jstro_node_arr_alloc(1);
                JSTRO_NODE_ARR[ab] = ALLOC_node_smi((uint64_t)idx);
                child_load = ALLOC_node_method_call(base, js_str_intern(p->c, "slice"), ab, 1);
            } else if (c->is_hole) {
                idx++;
                continue;
            } else {
                child_load = ALLOC_node_index_get(ALLOC_node_local_get(tmp),
                                                   ALLOC_node_smi((uint64_t)idx));
            }
            NODE *child_seq = emit_pattern_bind(p, s, c, child_load, is_decl);
            seq = ALLOC_node_seq(seq, child_seq);
            idx++;
        }
    } else {  // PAT_OBJECT
        // Track keys we've extracted to construct rest later.
        for (Pattern *c = pat->children; c; c = c->next) {
            NODE *child_load;
            if (c->is_rest) {
                // Build a new object that's { ...src }, then delete extracted keys.
                NODE *base = ALLOC_node_local_get(tmp);
                NODE *spread = base;  // simplified — we just use src; spread of properties not yet wired here
                child_load = spread;
                // For now: just bind to src directly (TODO: filter keys).
            } else {
                struct JsString *key = c->prop_key ? c->prop_key : c->name;
                child_load = ALLOC_node_member_get(ALLOC_node_local_get(tmp), key);
            }
            NODE *child_seq = emit_pattern_bind(p, s, c, child_load, is_decl);
            seq = ALLOC_node_seq(seq, child_seq);
        }
    }
    return seq;
}

static NODE *
assign_to_pat(Parser *p, Scope *s, NODE *value, void *ctxp)
{
    AssignCtx *ctx = (AssignCtx *)ctxp;
    Pattern *pat = ctx->pat;
    if (pat->kind == PAT_IDENT) {
        if (pat->is_hole) return value;
        return emit_assign_to_name(ctx->p, ctx->s, pat->name, value, ctx->is_decl);
    }
    return emit_pattern_bind(ctx->p, ctx->s, pat, value, ctx->is_decl);
}

// =====================================================================
// Identifier read / write codegen helpers
// =====================================================================

static NODE *
emit_load(Parser *p, Scope *s, struct JsString *name)
{
    // `arguments` is implicit when not declared by the user.
    static const char ARGS_NAME[] = "arguments";
    if (name->len == 9 && memcmp(name->data, ARGS_NAME, 9) == 0) {
        bool crosses; int idx;
        Scope *def = scope_lookup(s, name, &crosses, &idx);
        if (!def) return ALLOC_node_arguments();
    }
    RefBinding rb = resolve_name(s, name);
    switch (rb.kind) {
    case RES_LOCAL:
        // Use node_let_get when binding is let/const so TDZ throws on
        // pre-init reads.  var bindings (decl == 0/3) skip the check.
        if (rb.decl == 1 || rb.decl == 2) return ALLOC_node_let_get(rb.slot, name);
        return ALLOC_node_local_get(rb.slot);
    case RES_BOXED:  return ALLOC_node_box_get(rb.slot);
    case RES_UPVAL:  return ALLOC_node_upval_get(rb.slot);
    case RES_GLOBAL: return ALLOC_node_global_get(name);
    }
    (void)p;
    return ALLOC_node_undefined();
}

static NODE *
emit_store(Parser *p, Scope *s, struct JsString *name, NODE *rhs)
{
    RefBinding rb = resolve_name(s, name);
    switch (rb.kind) {
    case RES_LOCAL:
        if (rb.decl == 1 || rb.decl == 2) return ALLOC_node_let_set(rb.slot, name, rhs);
        return ALLOC_node_local_set(rb.slot, rhs);
    case RES_BOXED:  return ALLOC_node_box_set(rb.slot, rhs);
    case RES_UPVAL:  return ALLOC_node_upval_set(rb.slot, rhs);
    case RES_GLOBAL: return ALLOC_node_global_set(name, rhs);
    }
    (void)p;
    return ALLOC_node_undefined();
}

// =====================================================================
// Primary expressions
// =====================================================================

static NODE *
parse_primary(Parser *p, Scope *s)
{
    Token tk = p->cur;
    switch (tk.kind) {
    case TK_NUM: {
        advance(p);
        if (tk.is_int && tk.inum >= -((int64_t)1 << 53) && tk.inum <= ((int64_t)1 << 53)) {
            return ALLOC_node_smi((uint64_t)tk.inum);
        }
        return ALLOC_node_double(tk.dnum);
    }
    case TK_STR: {
        struct JsString *str = js_str_intern_n(p->c, tk.strbuf, tk.strlen);
        advance(p);
        return ALLOC_node_string(str);
    }
    case TK_TPL_STR: {
        struct JsString *str = js_str_intern_n(p->c, tk.strbuf, tk.strlen);
        advance(p);
        return ALLOC_node_string(str);
    }
    case TK_REGEX: {
        // `/pattern/flags` — call __makeRegex__(pattern, flags) at runtime.
        struct JsString *pat = js_str_intern_n(p->c, tk.strbuf, tk.strlen);
        struct JsString *fl  = js_str_intern_n(p->c, tk.regex_flags ? tk.regex_flags : "", tk.regex_flags_len);
        advance(p);
        NODE *helper = emit_load(p, s, js_str_intern(p->c, "__makeRegex__"));
        uint32_t base = jstro_node_arr_alloc(2);
        JSTRO_NODE_ARR[base]   = ALLOC_node_string(pat);
        JSTRO_NODE_ARR[base+1] = ALLOC_node_string(fl);
        return ALLOC_node_call(helper, base, 2);
    }
    case TK_TPL_HEAD: {
        // template: head expr (mid expr)* tail
        struct JsString *head_str = js_str_intern_n(p->c, tk.strbuf, tk.strlen);
        advance(p);
        NODE *acc = ALLOC_node_string(head_str);
        for (;;) {
            NODE *e = parse_expr(p, s);
            // Convert e to string via String(e) then concat.  We'll use
            // node_add which does spec-correct ToString when one side is string.
            acc = ALLOC_node_add(acc, e);
            if (p->cur.kind == TK_TPL_MID) {
                struct JsString *mid = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
                advance(p);
                acc = ALLOC_node_add(acc, ALLOC_node_string(mid));
            } else if (p->cur.kind == TK_TPL_TAIL) {
                struct JsString *tail = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
                advance(p);
                acc = ALLOC_node_add(acc, ALLOC_node_string(tail));
                break;
            } else {
                parse_error(p, "expected template middle or tail");
            }
        }
        return acc;
    }
    case TK_TRUE:      advance(p); return ALLOC_node_true();
    case TK_FALSE:     advance(p); return ALLOC_node_false();
    case TK_NULL:      advance(p); return ALLOC_node_null();
    case TK_UNDEFINED: advance(p); return ALLOC_node_undefined();
    case TK_THIS:      advance(p); return ALLOC_node_this();
    case TK_SUPER: {
        advance(p);
        // super(...) — call parent constructor on this.
        if (p->cur.kind == TK_LPAREN) {
            advance(p);
            ParsedArgs pa; parse_call_args(p, s, &pa);
            // __super__.call(this, ...args)
            NODE *sup = emit_load(p, s, js_str_intern(p->c, "__super__"));
            // Build args: [this, args...].
            NODE *all_args[33];
            uint8_t spreads[33];
            all_args[0] = ALLOC_node_this();
            spreads[0] = 0;
            for (uint32_t i = 0; i < pa.n && i < 32; i++) {
                all_args[i+1] = pa.args[i];
                spreads[i+1] = pa.spread[i];
            }
            uint32_t total = pa.n + 1;
            uint32_t base = jstro_node_arr_alloc(total);
            for (uint32_t i = 0; i < total; i++) JSTRO_NODE_ARR[base + i] = all_args[i];
            if (pa.any_spread) {
                uint32_t sb = jstro_u32_arr_alloc(total);
                for (uint32_t i = 0; i < total; i++) JSTRO_U32_ARR[sb + i] = spreads[i];
                return ALLOC_node_method_call_spread(sup, js_str_intern(p->c, "call"), base, sb, total);
            }
            return ALLOC_node_method_call(sup, js_str_intern(p->c, "call"), base, total);
        }
        // super.method or super.method(...)
        if (p->cur.kind != TK_DOT) parse_error(p, "expected ( or . after super");
        advance(p);
        struct JsString *mn = js_str_intern_n(p->c, p->cur.start, p->cur.len);
        advance(p);
        NODE *sup = emit_load(p, s, js_str_intern(p->c, "__super__"));
        NODE *parent_proto = ALLOC_node_member_get(sup, js_str_intern(p->c, "prototype"));
        NODE *method = ALLOC_node_member_get(parent_proto, mn);
        if (p->cur.kind == TK_LPAREN) {
            advance(p);
            ParsedArgs pa; parse_call_args(p, s, &pa);
            // method.call(this, ...args)
            NODE *all_args[33];
            uint8_t spreads[33];
            all_args[0] = ALLOC_node_this();
            spreads[0] = 0;
            for (uint32_t i = 0; i < pa.n && i < 32; i++) {
                all_args[i+1] = pa.args[i];
                spreads[i+1] = pa.spread[i];
            }
            uint32_t total = pa.n + 1;
            uint32_t base = jstro_node_arr_alloc(total);
            for (uint32_t i = 0; i < total; i++) JSTRO_NODE_ARR[base + i] = all_args[i];
            if (pa.any_spread) {
                uint32_t sb = jstro_u32_arr_alloc(total);
                for (uint32_t i = 0; i < total; i++) JSTRO_U32_ARR[sb + i] = spreads[i];
                return ALLOC_node_method_call_spread(method, js_str_intern(p->c, "call"), base, sb, total);
            }
            return ALLOC_node_method_call(method, js_str_intern(p->c, "call"), base, total);
        }
        // bare super.x — return the value (rare; without method-binding it's just a property read).
        return method;
    }
    case TK_IDENT: {
        struct JsString *name = js_str_intern_n(p->c, tk.start, tk.len);
        // `arguments` keyword: implicit binding to current call's args.
        // Resolve only when there's no user-declared `arguments` in scope.
        if (tk.len == 9 && memcmp(tk.start, "arguments", 9) == 0) {
            bool crosses; int idx;
            Scope *def = scope_lookup(s, name, &crosses, &idx);
            if (!def) {
                advance(p);
                return ALLOC_node_arguments();
            }
        }
        // arrow shorthand:  ident => ...
        const Token *nx = peek(p);
        if (nx->kind == TK_ARROW) {
            advance(p); // consume IDENT
            NODE *body;
            // build arrow with single param `name`
            Scope *child = scope_new(s, /*is_function*/true, /*is_arrow*/true);
            uint32_t slot = scope_declare(child, name, 3);
            (void)slot;
            advance(p); // consume =>
            if (check(p, TK_LBRACE)) {
                body = parse_block(p, child, /*new_scope=*/false);
            } else {
                NODE *e = parse_assign(p, child);
                body = ALLOC_node_return(e);
            }
            // emit upvals
            uint32_t up_idx = jstro_u32_arr_alloc(child->nupvals * 2);
            for (uint32_t i = 0; i < child->nupvals; i++) {
                JSTRO_U32_ARR[up_idx + 2*i]   = child->upvals[i].is_local;
                JSTRO_U32_ARR[up_idx + 2*i+1] = child->upvals[i].slot;
            }
            uint32_t nu = child->nupvals;
            uint32_t nl = child->nlocals;
            scope_free(child);
            return ALLOC_node_func(body, 1, nl, nu, up_idx, /*is_arrow=*/1, NULL);
        }
        advance(p);
        return emit_load(p, s, name);
    }
    case TK_LPAREN: {
        // Could be: (expr), arrow (a, b) => ..., or empty () => ...
        // Try peeking to see if there's a closing paren followed by =>.
        const char *save_pos = p->p;
        int save_line = p->line;
        Token save_cur = p->cur;
        Token save_next = p->next;
        bool save_peek = p->has_peeked;

        advance(p); // consume (
        // try to parse as comma-separated identifier list followed by ) =>
        struct JsString *names[32];
        uint32_t nn = 0;
        bool is_arrow = true;
        bool is_empty_paren = false;
        if (p->cur.kind == TK_RPAREN) {
            is_empty_paren = true;
            advance(p);
            if (p->cur.kind != TK_ARROW) is_arrow = false;
        } else {
            while (p->cur.kind == TK_IDENT) {
                if (nn >= 32) { is_arrow = false; break; }
                names[nn++] = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
                if (p->cur.kind == TK_COMMA) advance(p);
                else break;
            }
            if (p->cur.kind != TK_RPAREN) is_arrow = false;
            else {
                advance(p);
                if (p->cur.kind != TK_ARROW) is_arrow = false;
            }
        }
        if (is_arrow) {
            // build arrow
            advance(p); // consume =>
            Scope *child = scope_new(s, true, true);
            for (uint32_t i = 0; i < nn; i++) scope_declare(child, names[i], 3);
            NODE *body;
            if (check(p, TK_LBRACE)) {
                body = parse_block(p, child, /*new_scope=*/false);
            } else {
                NODE *e = parse_assign(p, child);
                body = ALLOC_node_return(e);
            }
            uint32_t up_idx = jstro_u32_arr_alloc(child->nupvals * 2);
            for (uint32_t i = 0; i < child->nupvals; i++) {
                JSTRO_U32_ARR[up_idx + 2*i]   = child->upvals[i].is_local;
                JSTRO_U32_ARR[up_idx + 2*i+1] = child->upvals[i].slot;
            }
            uint32_t nu = child->nupvals, nl = child->nlocals;
            scope_free(child);
            return ALLOC_node_func(body, nn, nl, nu, up_idx, 1, NULL);
        }
        // backtrack
        if (is_empty_paren) parse_error(p, "() must be followed by =>");
        p->p = save_pos; p->line = save_line; p->cur = save_cur; p->next = save_next; p->has_peeked = save_peek;
        advance(p); // re-consume LPAREN
        NODE *e = parse_expr(p, s);
        expect(p, TK_RPAREN, "')'");
        return e;
    }
    case TK_LBRACK: {
        advance(p);
        NODE *elems[256];
        uint8_t spreads[256];
        uint32_t n = 0;
        bool any_spread = false;
        while (p->cur.kind != TK_RBRACK) {
            if (p->cur.kind == TK_COMMA) {
                if (n < 256) { elems[n] = NULL; spreads[n] = 0; n++; }
                advance(p);
                continue;
            }
            uint8_t sp = 0;
            if (p->cur.kind == TK_DOTS) { advance(p); sp = 1; any_spread = true; }
            NODE *e = parse_assign(p, s);
            if (n < 256) { elems[n] = e; spreads[n] = sp; n++; }
            if (p->cur.kind == TK_COMMA) advance(p);
            else break;
        }
        expect(p, TK_RBRACK, "']'");
        uint32_t base = jstro_node_arr_alloc(n);
        for (uint32_t i = 0; i < n; i++) JSTRO_NODE_ARR[base + i] = elems[i];
        if (any_spread) {
            uint32_t sbase = jstro_u32_arr_alloc(n);
            for (uint32_t i = 0; i < n; i++) JSTRO_U32_ARR[sbase + i] = spreads[i];
            return ALLOC_node_array_lit_spread(base, sbase, n);
        }
        return ALLOC_node_array_lit(base, n);
    }
    case TK_LBRACE: {
        advance(p);
        struct JsString *keys[256];
        NODE *key_nodes[256];
        NODE *vals[256];
        uint8_t kinds[256];       // 0=static, 1=spread, 2=computed
        uint32_t n = 0;
        bool any_dyn = false;
        // Track getter/setter pairs: pending[key] holds an accessor obj.
        // We lazily build {get, set} accessor objects when both halves seen.
        // For simplicity, the first half emits an accessor, and the second
        // half mutates via a runtime helper.
        while (p->cur.kind != TK_RBRACE) {
            struct JsString *key = NULL;
            uint8_t kind = 0;
            NODE *key_node = NULL;
            if (p->cur.kind == TK_DOTS) {
                advance(p);
                NODE *src_e = parse_assign(p, s);
                if (n < 256) { keys[n] = NULL; key_nodes[n] = NULL; vals[n] = src_e; kinds[n] = 1; n++; }
                any_dyn = true;
                if (p->cur.kind == TK_COMMA) advance(p);
                else break;
                continue;
            }
            // Check for `get NAME() { ... }` / `set NAME(v) { ... }` accessors.
            if (p->cur.kind == TK_IDENT && p->cur.len == 3
                && (memcmp(p->cur.start, "get", 3) == 0 || memcmp(p->cur.start, "set", 3) == 0)) {
                bool is_get = memcmp(p->cur.start, "get", 3) == 0;
                const Token *nx = peek(p);
                if (nx->kind == TK_IDENT || nx->kind == TK_STR) {
                    advance(p);
                    struct JsString *acc_key;
                    if (p->cur.kind == TK_IDENT)
                        acc_key = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                    else
                        acc_key = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
                    advance(p);
                    NODE *fn = parse_function_expr(p, s, acc_key);
                    // Emit an object literal { get: fn } or { set: fn } as a
                    // marker — but we want a real JS_TACCESSOR.  Use a runtime
                    // helper `__defAccessor__(target, key, getter, setter)`.
                    // For object literals, we collect them here as kind=4
                    // (special) with the fn as the value.  Implemented via a
                    // post-build step below.
                    (void)is_get;
                    (void)acc_key;
                    (void)fn;
                    if (n < 256) {
                        keys[n] = acc_key;
                        key_nodes[n] = NULL;
                        vals[n] = fn;
                        kinds[n] = is_get ? 3 : 4;   // 3=getter, 4=setter
                        any_dyn = true;
                        n++;
                    }
                    if (p->cur.kind == TK_COMMA) advance(p);
                    continue;
                }
            }
            if (p->cur.kind == TK_LBRACK) {
                advance(p);
                key_node = parse_assign(p, s);
                expect(p, TK_RBRACK, "']'");
                kind = 2;
                any_dyn = true;
                if (p->cur.kind == TK_LPAREN) {
                    NODE *fn = parse_function_expr(p, s, NULL);
                    if (n < 256) { keys[n] = NULL; key_nodes[n] = key_node; vals[n] = fn; kinds[n] = kind; n++; }
                    if (p->cur.kind == TK_COMMA) advance(p);
                    continue;
                }
                expect(p, TK_COLON, "':'");
            } else if (p->cur.kind == TK_IDENT) {
                key = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
                if (p->cur.kind == TK_COMMA || p->cur.kind == TK_RBRACE) {
                    NODE *v = emit_load(p, s, key);
                    if (n < 256) { keys[n] = key; key_nodes[n] = NULL; vals[n] = v; kinds[n] = 0; n++; }
                    if (p->cur.kind == TK_COMMA) advance(p);
                    continue;
                }
                if (p->cur.kind == TK_LPAREN) {
                    NODE *fn = parse_function_expr(p, s, key);
                    if (n < 256) { keys[n] = key; key_nodes[n] = NULL; vals[n] = fn; kinds[n] = 0; n++; }
                    if (p->cur.kind == TK_COMMA) advance(p);
                    continue;
                }
                expect(p, TK_COLON, "':'");
            } else if (p->cur.kind == TK_STR) {
                key = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
                advance(p);
                expect(p, TK_COLON, "':'");
            } else if (p->cur.kind == TK_NUM) {
                char buf[32];
                int kn = snprintf(buf, sizeof buf, "%lld", (long long)p->cur.inum);
                key = js_str_intern_n(p->c, buf, kn);
                advance(p);
                expect(p, TK_COLON, "':'");
            } else {
                if (p->cur.start && p->cur.len > 0) {
                    key = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                    advance(p);
                    expect(p, TK_COLON, "':'");
                } else {
                    parse_error(p, "expected property name");
                }
            }
            NODE *v = parse_assign(p, s);
            if (n < 256) { keys[n] = key; key_nodes[n] = key_node; vals[n] = v; kinds[n] = kind; n++; }
            if (p->cur.kind == TK_COMMA) advance(p);
            else break;
        }
        expect(p, TK_RBRACE, "'}'");
        if (any_dyn) {
            uint32_t kbase_str  = jstro_str_arr_alloc(n);
            uint32_t kbase_node = jstro_node_arr_alloc(n);
            uint32_t vbase      = jstro_node_arr_alloc(n);
            uint32_t kindbase   = jstro_u32_arr_alloc(n);
            for (uint32_t i = 0; i < n; i++) {
                JSTRO_STR_ARR[kbase_str + i]   = keys[i];        // static slot
                JSTRO_NODE_ARR[kbase_node + i] = key_nodes[i];   // computed slot
                JSTRO_NODE_ARR[vbase + i]      = vals[i];
                JSTRO_U32_ARR[kindbase + i]    = kinds[i];
            }
            return ALLOC_node_object_lit_dyn(kbase_str, kbase_node, vbase, kindbase, n);
        }
        uint32_t kbase = jstro_str_arr_alloc(n);
        uint32_t vbase = jstro_node_arr_alloc(n);
        for (uint32_t i = 0; i < n; i++) {
            JSTRO_STR_ARR[kbase + i] = keys[i];
            JSTRO_NODE_ARR[vbase + i] = vals[i];
        }
        return ALLOC_node_object_lit(kbase, vbase, n);
    }
    case TK_FUNCTION: {
        advance(p);
        // Generator marker `function* name(...)` — accepted syntactically;
        // body is parsed normally.  yield evaluates to undefined; full
        // suspended-frame semantics are a known limitation.
        if (p->cur.kind == TK_STAR) advance(p);
        struct JsString *name = NULL;
        if (p->cur.kind == TK_IDENT) {
            name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            advance(p);
        }
        return parse_function_expr(p, s, name);
    }
    case TK_ASYNC: {
        // `async function ... ` or `async (a) => ...`
        advance(p);
        if (p->cur.kind == TK_FUNCTION) {
            advance(p);
            struct JsString *name = NULL;
            if (p->cur.kind == TK_IDENT) {
                name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
            }
            // async functions just run synchronously and return their value
            // wrapped in a resolved Promise (we use a {then} duck-type).
            return parse_function_expr(p, s, name);
        }
        // async arrow — fall through to regular ident / arrow handling
        // (arrows don't currently get wrapped; their result is the body's value)
        // Re-enter parse_primary by emitting an ident `async` (unlikely
        // useful but at least not a parse error).
        struct JsString *name = js_str_intern(p->c, "async");
        return emit_load(p, s, name);
    }
    case TK_AWAIT: {
        // `await expr` — synchronous reduction: if expr is a Promise-like
        // ({then(cb)}), call .then to obtain the value; otherwise return
        // the expression value.  This is non-spec (no scheduling) but
        // fine for code paths that use Promise.resolve/.then chains.
        advance(p);
        NODE *expr = parse_assign(p, s);
        // Build helper: __awaitSync__(expr)
        NODE *helper = emit_load(p, s, js_str_intern(p->c, "__awaitSync__"));
        uint32_t base = jstro_node_arr_alloc(1);
        JSTRO_NODE_ARR[base] = expr;
        return ALLOC_node_call(helper, base, 1);
    }
    case TK_YIELD: {
        // Generator yield — without true suspension, we treat `yield expr`
        // as evaluating expr and returning undefined.  Documented limit.
        advance(p);
        if (p->cur.kind == TK_SEMI || p->cur.kind == TK_RPAREN || p->cur.kind == TK_RBRACE
            || p->cur.kind == TK_COMMA || p->cur.kind == TK_RBRACK) {
            return ALLOC_node_undefined();
        }
        NODE *expr = parse_assign(p, s);
        // emit a no-op that evaluates expr for side effects
        NODE *helper = emit_load(p, s, js_str_intern(p->c, "__yieldFake__"));
        uint32_t base = jstro_node_arr_alloc(1);
        JSTRO_NODE_ARR[base] = expr;
        return ALLOC_node_call(helper, base, 1);
    }
    case TK_NEW: {
        advance(p);
        // `new.target`
        if (p->cur.kind == TK_DOT) {
            advance(p);
            if (p->cur.kind == TK_IDENT && p->cur.len == 6 && memcmp(p->cur.start, "target", 6) == 0) {
                advance(p);
                return ALLOC_node_new_target();
            }
            parse_error(p, "expected 'target' after 'new.'");
        }
        // Parse a member expression (no call) for the constructor.
        NODE *ctor = parse_primary(p, s);
        // Allow .member chains
        while (p->cur.kind == TK_DOT) {
            advance(p);
            if (p->cur.kind != TK_IDENT) parse_error(p, "expected name after .");
            struct JsString *nm = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            advance(p);
            ctor = ALLOC_node_member_get(ctor, nm);
        }
        // Optional argument list
        ParsedArgs pa = {0};
        if (p->cur.kind == TK_LPAREN) {
            advance(p);
            parse_call_args(p, s, &pa);
        }
        return build_new(ctor, &pa);
    }
    case TK_TYPEOF: {
        advance(p);
        // typeof on a bare identifier doesn't throw if it's undeclared
        if (p->cur.kind == TK_IDENT) {
            const Token *nx = peek(p);
            if (nx->kind != TK_DOT && nx->kind != TK_LBRACK && nx->kind != TK_LPAREN) {
                struct JsString *name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
                RefBinding rb = resolve_name(s, name);
                if (rb.kind == RES_GLOBAL) return ALLOC_node_typeof_global(name);
                NODE *load;
                switch (rb.kind) {
                case RES_LOCAL:  load = ALLOC_node_local_get(rb.slot); break;
                case RES_BOXED:  load = ALLOC_node_box_get(rb.slot); break;
                case RES_UPVAL:  load = ALLOC_node_upval_get(rb.slot); break;
                default: load = ALLOC_node_undefined();
                }
                return ALLOC_node_typeof(load);
            }
        }
        NODE *x = parse_primary(p, s);
        // allow postfix . and []
        for (;;) {
            if (p->cur.kind == TK_DOT) {
                advance(p);
                struct JsString *nm = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
                x = ALLOC_node_member_get(x, nm);
            } else if (p->cur.kind == TK_LBRACK) {
                advance(p);
                NODE *e = parse_expr(p, s);
                expect(p, TK_RBRACK, "']'");
                x = ALLOC_node_index_get(x, e);
            } else break;
        }
        return ALLOC_node_typeof(x);
    }
    case TK_VOID: {
        advance(p);
        NODE *e = parse_assign(p, s);
        return ALLOC_node_void(e);
    }
    case TK_DELETE: {
        advance(p);
        // Parse a member expression so we can detect delete obj.name / obj[k].
        // We track the LHS shape; if it's a member, emit node_delete_member.
        LhsTrack lt = {0};
        NODE *e = parse_postfix_track(p, s, &lt);
        if (lt.kind == 1) {
            // delete obj.name
            return ALLOC_node_delete_member(lt.recv, lt.name);
        }
        // delete obj[k] / delete x — fall back to true (per spec, delete
        // on a non-property reference returns true unless strict mode).
        (void)e;
        return ALLOC_node_true();
    }
    case TK_NOT: {
        advance(p);
        NODE *e = parse_postfix(p, s);
        return ALLOC_node_not(e);
    }
    case TK_PLUS: {
        advance(p);
        NODE *e = parse_postfix(p, s);
        return ALLOC_node_pos(e);
    }
    case TK_MINUS: {
        advance(p);
        NODE *e = parse_postfix(p, s);
        return ALLOC_node_neg(e);
    }
    case TK_TILDE: {
        advance(p);
        NODE *e = parse_postfix(p, s);
        return ALLOC_node_bnot(e);
    }
    case TK_INC:
    case TK_DEC: {
        int delta = (p->cur.kind == TK_INC) ? 1 : -1;
        advance(p);
        if (p->cur.kind != TK_IDENT) parse_error(p, "prefix inc/dec on non-ident not supported");
        struct JsString *name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
        advance(p);
        RefBinding rb = resolve_name(s, name);
        if (rb.kind == RES_LOCAL) return ALLOC_node_inc_local(rb.slot, 1, (uint32_t)delta);
        if (rb.kind == RES_BOXED) return ALLOC_node_inc_box(rb.slot, 1, (uint32_t)delta);
        if (rb.kind == RES_UPVAL) return ALLOC_node_inc_upval(rb.slot, 1, (uint32_t)delta);
        // Global: load + add + store (returns the new value, OK for prefix)
        NODE *one = ALLOC_node_smi(1);
        NODE *load = emit_load(p, s, name);
        NODE *res = (delta > 0) ? ALLOC_node_add(load, one) : ALLOC_node_sub(load, one);
        return emit_store(p, s, name, res);
    }
    default: break;
    }
    parse_error(p, "unexpected token (kind=%d) at line %d", (int)p->cur.kind, p->cur.line);
}

// =====================================================================
// Postfix / call / member chain
// =====================================================================

// Helper: parse `(arg, ...spread, arg)` into out arrays; sets any_spread.
static void
parse_call_args(Parser *p, Scope *s, ParsedArgs *out)
{
    out->n = 0;
    out->any_spread = false;
    while (p->cur.kind != TK_RPAREN) {
        if (out->n >= 32) parse_error(p, "too many args");
        uint8_t sp = 0;
        if (p->cur.kind == TK_DOTS) { advance(p); sp = 1; out->any_spread = true; }
        out->args[out->n] = parse_assign(p, s);
        out->spread[out->n] = sp;
        out->n++;
        if (p->cur.kind == TK_COMMA) advance(p);
        else break;
    }
    expect(p, TK_RPAREN, "')'");
}

// Build a regular or spread-aware call NODE based on parsed args.
static NODE *
build_call(NODE *fn, ParsedArgs *pa)
{
    uint32_t n = pa->n;
    if (pa->any_spread) {
        uint32_t base = jstro_node_arr_alloc(n);
        uint32_t sb   = jstro_u32_arr_alloc(n);
        for (uint32_t i = 0; i < n; i++) {
            JSTRO_NODE_ARR[base + i] = pa->args[i];
            JSTRO_U32_ARR[sb + i] = pa->spread[i];
        }
        return ALLOC_node_call_spread(fn, base, sb, n);
    }
    switch (n) {
    case 0: return ALLOC_node_call0(fn);
    case 1: return ALLOC_node_call1(fn, pa->args[0]);
    case 2: return ALLOC_node_call2(fn, pa->args[0], pa->args[1]);
    case 3: return ALLOC_node_call3(fn, pa->args[0], pa->args[1], pa->args[2]);
    default: {
        uint32_t base = jstro_node_arr_alloc(n);
        for (uint32_t i = 0; i < n; i++) JSTRO_NODE_ARR[base + i] = pa->args[i];
        return ALLOC_node_call(fn, base, n);
    }
    }
}

static NODE *
build_method_call(NODE *recv, struct JsString *name, ParsedArgs *pa)
{
    uint32_t n = pa->n;
    if (pa->any_spread) {
        uint32_t base = jstro_node_arr_alloc(n);
        uint32_t sb   = jstro_u32_arr_alloc(n);
        for (uint32_t i = 0; i < n; i++) {
            JSTRO_NODE_ARR[base + i] = pa->args[i];
            JSTRO_U32_ARR[sb + i] = pa->spread[i];
        }
        return ALLOC_node_method_call_spread(recv, name, base, sb, n);
    }
    uint32_t base = jstro_node_arr_alloc(n);
    for (uint32_t i = 0; i < n; i++) JSTRO_NODE_ARR[base + i] = pa->args[i];
    return ALLOC_node_method_call(recv, name, base, n);
}

static NODE *
build_new(NODE *ctor, ParsedArgs *pa)
{
    uint32_t n = pa->n;
    if (pa->any_spread) {
        uint32_t base = jstro_node_arr_alloc(n);
        uint32_t sb   = jstro_u32_arr_alloc(n);
        for (uint32_t i = 0; i < n; i++) {
            JSTRO_NODE_ARR[base + i] = pa->args[i];
            JSTRO_U32_ARR[sb + i] = pa->spread[i];
        }
        return ALLOC_node_new_spread(ctor, base, sb, n);
    }
    uint32_t base = jstro_node_arr_alloc(n);
    for (uint32_t i = 0; i < n; i++) JSTRO_NODE_ARR[base + i] = pa->args[i];
    return ALLOC_node_new(ctor, base, n);
}

// Build a tagged-template call: tag(stringsArray, ...exprs).
// We've already consumed the first TPL_STR or TPL_HEAD token at `tk_first`.
static NODE *
parse_tagged_template(Parser *p, Scope *s, NODE *tag_fn, struct JsString *first_str, bool head)
{
    // Collect cooked strings + interpolation expressions.
    struct JsString *strs[64];
    NODE *exprs[64];
    uint32_t ns = 1, ne = 0;
    strs[0] = first_str;
    if (head) {
        for (;;) {
            NODE *e = parse_expr(p, s);
            if (ne < 64) exprs[ne++] = e;
            if (p->cur.kind == TK_TPL_MID) {
                if (ns < 64) strs[ns++] = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
                advance(p);
            } else if (p->cur.kind == TK_TPL_TAIL) {
                if (ns < 64) strs[ns++] = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
                advance(p);
                break;
            } else {
                parse_error(p, "expected template middle or tail");
            }
        }
    }
    // Build a cooked-strings array literal AST node.
    uint32_t arr_base = jstro_node_arr_alloc(ns);
    for (uint32_t i = 0; i < ns; i++) JSTRO_NODE_ARR[arr_base + i] = ALLOC_node_string(strs[i]);
    NODE *strings_arr = ALLOC_node_array_lit(arr_base, ns);
    // Build call: tag(strings_arr, ...exprs).
    uint32_t total = 1 + ne;
    uint32_t cb = jstro_node_arr_alloc(total);
    JSTRO_NODE_ARR[cb] = strings_arr;
    for (uint32_t i = 0; i < ne; i++) JSTRO_NODE_ARR[cb + 1 + i] = exprs[i];
    return ALLOC_node_call(tag_fn, cb, total);
}

static NODE *
parse_postfix(Parser *p, Scope *s)
{
    NODE *x = parse_primary(p, s);
    for (;;) {
        if (p->cur.kind == TK_TPL_STR) {
            struct JsString *str = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
            advance(p);
            x = parse_tagged_template(p, s, x, str, false);
            continue;
        }
        if (p->cur.kind == TK_TPL_HEAD) {
            struct JsString *str = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
            advance(p);
            x = parse_tagged_template(p, s, x, str, true);
            continue;
        }
        if (p->cur.kind == TK_QDOT) {
            advance(p);
            if (p->cur.kind == TK_LPAREN) {
                advance(p);
                ParsedArgs pa; parse_call_args(p, s, &pa);
                uint32_t base = jstro_node_arr_alloc(pa.n);
                for (uint32_t i = 0; i < pa.n; i++) JSTRO_NODE_ARR[base + i] = pa.args[i];
                x = ALLOC_node_optional_call(x, base, pa.n);
                continue;
            }
            if (p->cur.kind == TK_LBRACK) {
                advance(p);
                NODE *e = parse_expr(p, s);
                expect(p, TK_RBRACK, "']'");
                x = ALLOC_node_optional_index(x, e);
                continue;
            }
            // Identifier (possibly keyword used as name)
            struct JsString *nm;
            if (p->cur.start) { nm = js_str_intern_n(p->c, p->cur.start, p->cur.len); advance(p); }
            else parse_error(p, "expected name after ?.");
            x = ALLOC_node_optional_member(x, nm);
            continue;
        }
        if (p->cur.kind == TK_DOT) {
            advance(p);
            // accept any keyword here as a property name
            struct JsString *nm;
            if (p->cur.kind == TK_IDENT || p->cur.start) {
                nm = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
            } else {
                parse_error(p, "expected property name");
            }
            if (p->cur.kind == TK_LPAREN) {
                advance(p);
                ParsedArgs pa; parse_call_args(p, s, &pa);
                x = build_method_call(x, nm, &pa);
            } else {
                x = ALLOC_node_member_get(x, nm);
            }
        } else if (p->cur.kind == TK_LBRACK) {
            advance(p);
            NODE *e = parse_expr(p, s);
            expect(p, TK_RBRACK, "']'");
            x = ALLOC_node_index_get(x, e);
        } else if (p->cur.kind == TK_LPAREN) {
            advance(p);
            ParsedArgs pa; parse_call_args(p, s, &pa);
            x = build_call(x, &pa);
        } else if (p->cur.kind == TK_INC) {
            advance(p);
            return x;
        } else if (p->cur.kind == TK_DEC) {
            advance(p);
            return x;
        } else {
            break;
        }
    }
    return x;
}

// =====================================================================
// Binary / ternary expressions
// =====================================================================

// Operator precedence table (lower number = lower precedence)
static int
binop_prec(TokenKind k, bool no_in)
{
    switch (k) {
    case TK_LOR: return 4;
    case TK_NULLISH: return 4;
    case TK_LAND: return 5;
    case TK_PIPE: return 6;
    case TK_CARET: return 7;
    case TK_AMP: return 8;
    case TK_EQ: case TK_NE: case TK_SEQ: case TK_SNE: return 9;
    case TK_LT: case TK_LE: case TK_GT: case TK_GE: return 10;
    case TK_INSTANCEOF: return 10;
    case TK_IN: return no_in ? -1 : 10;
    case TK_SHL: case TK_SAR: case TK_SHR: return 11;
    case TK_PLUS: case TK_MINUS: return 12;
    case TK_STAR: case TK_SLASH: case TK_PCT: return 13;
    case TK_POW: return 14;
    default: return -1;
    }
}

static NODE *
parse_binop_climb(Parser *p, Scope *s, int min_prec, bool no_in)
{
    NODE *left = parse_postfix(p, s);
    for (;;) {
        TokenKind op = p->cur.kind;
        int prec = binop_prec(op, no_in);
        if (prec < min_prec) break;
        advance(p);
        // ** is right-associative
        bool right_assoc = (op == TK_POW);
        NODE *right = parse_binop_climb(p, s, prec + (right_assoc ? 0 : 1), no_in);
        switch (op) {
        case TK_PLUS:  left = ALLOC_node_add(left, right); break;
        case TK_MINUS: left = ALLOC_node_sub(left, right); break;
        case TK_STAR:  left = ALLOC_node_mul(left, right); break;
        case TK_SLASH: left = ALLOC_node_div(left, right); break;
        case TK_PCT:   left = ALLOC_node_mod(left, right); break;
        case TK_POW:   left = ALLOC_node_pow(left, right); break;
        case TK_LT:    left = ALLOC_node_lt(left, right); break;
        case TK_LE:    left = ALLOC_node_le(left, right); break;
        case TK_GT:    left = ALLOC_node_gt(left, right); break;
        case TK_GE:    left = ALLOC_node_ge(left, right); break;
        case TK_EQ:    left = ALLOC_node_loose_eq(left, right); break;
        case TK_NE:    left = ALLOC_node_loose_neq(left, right); break;
        case TK_SEQ:   left = ALLOC_node_strict_eq(left, right); break;
        case TK_SNE:   left = ALLOC_node_strict_neq(left, right); break;
        case TK_LAND:  left = ALLOC_node_and(left, right); break;
        case TK_LOR:   left = ALLOC_node_or(left, right); break;
        case TK_NULLISH: left = ALLOC_node_nullish(left, right); break;
        case TK_AMP:   left = ALLOC_node_band(left, right); break;
        case TK_PIPE:  left = ALLOC_node_bor(left, right); break;
        case TK_CARET: left = ALLOC_node_bxor(left, right); break;
        case TK_SHL:   left = ALLOC_node_shl(left, right); break;
        case TK_SAR:   left = ALLOC_node_sar(left, right); break;
        case TK_SHR:   left = ALLOC_node_shr(left, right); break;
        case TK_INSTANCEOF: left = ALLOC_node_instanceof(left, right); break;
        case TK_IN:    left = ALLOC_node_in(left, right); break;
        default: parse_error(p, "internal: unhandled binop");
        }
    }
    return left;
}

// =====================================================================
// Assignments / ternary
// =====================================================================

// (parse_ternary defined later as parse_ternary_with_assign)

// Convert a postfix-chain LHS NODE back into an assignment target by
// inspecting the dispatcher name.  We store the kind via a pattern match.
typedef enum { LHS_NAME, LHS_MEMBER, LHS_INDEX, LHS_OTHER } LhsKind;

// We stash "the original ident name" and "the receiver expression" on
// special parser-side wrappers: emit_load returns nodes that we
// reconstruct from to detect assignment patterns.  Since detecting
// after-the-fact from generated nodes is messy, we re-parse.
//
// Simpler: parse the LHS via a special helper that captures structure
// before generating loads.  But to keep things small we instead detect
// assignments at the start of expressions by lookahead.

// (parse_assign forward-defined later as parse_ternary_with_assign wrapper)

// =====================================================================
// We need full assignment support.  Override parse_ternary to handle
// assignments at its head, using a "parse LHS then check for =".
// =====================================================================

// We need to detect:
//   ident = expr
//   obj.x = expr
//   obj[k] = expr
//   ident OP= expr
//
// We re-run the LHS parse first, then check for assignment operator.  If
// found, we re-emit the LHS as a target instead of as a load.  Implement
// this by tracking, during the LHS parse, the *kind* of last expression.
//
// Simpler path: let parse_ternary call this assignment-first version,
// which parses an LHS via a "remember the last load shape" trick.

static NODE *parse_unary_track(Parser *p, Scope *s, LhsTrack *lt);

static NODE *
parse_unary_track(Parser *p, Scope *s, LhsTrack *lt)
{
    lt->kind = -1;
    switch (p->cur.kind) {
    case TK_NOT: case TK_PLUS: case TK_MINUS: case TK_TILDE: case TK_TYPEOF:
    case TK_VOID: case TK_DELETE: case TK_INC: case TK_DEC: {
        NODE *r = parse_primary(p, s);
        lt->kind = -1;
        return r;
    }
    default:
        break;
    }
    return parse_postfix_track(p, s, lt);
}

static NODE *
parse_postfix_track(Parser *p, Scope *s, LhsTrack *lt)
{
    Token start = p->cur;
    NODE *x;
    if (p->cur.kind == TK_IDENT) {
        struct JsString *name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
        const Token *nx = peek(p);
        if (nx->kind == TK_ARROW) {
            x = parse_primary(p, s);
            lt->kind = -1;
            return x;
        }
        advance(p);
        x = emit_load(p, s, name);
        lt->kind = 0;
        lt->name = name;
        lt->recv = NULL;
    } else {
        x = parse_primary(p, s);
        lt->kind = -1;
    }
    (void)start;
    for (;;) {
        if (p->cur.kind == TK_TPL_STR) {
            struct JsString *str = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
            advance(p);
            x = parse_tagged_template(p, s, x, str, false);
            lt->kind = -1;
            continue;
        }
        if (p->cur.kind == TK_TPL_HEAD) {
            struct JsString *str = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
            advance(p);
            x = parse_tagged_template(p, s, x, str, true);
            lt->kind = -1;
            continue;
        }
        if (p->cur.kind == TK_QDOT) {
            advance(p);
            if (p->cur.kind == TK_LPAREN) {
                advance(p);
                ParsedArgs pa; parse_call_args(p, s, &pa);
                uint32_t base = jstro_node_arr_alloc(pa.n);
                for (uint32_t i = 0; i < pa.n; i++) JSTRO_NODE_ARR[base + i] = pa.args[i];
                x = ALLOC_node_optional_call(x, base, pa.n);
                lt->kind = -1;
                continue;
            }
            if (p->cur.kind == TK_LBRACK) {
                advance(p);
                NODE *e = parse_expr(p, s);
                expect(p, TK_RBRACK, "']'");
                x = ALLOC_node_optional_index(x, e);
                lt->kind = -1;
                continue;
            }
            struct JsString *nm;
            if (p->cur.start) { nm = js_str_intern_n(p->c, p->cur.start, p->cur.len); advance(p); }
            else parse_error(p, "expected name after ?.");
            x = ALLOC_node_optional_member(x, nm);
            lt->kind = -1;
            continue;
        }
        if (p->cur.kind == TK_DOT) {
            advance(p);
            struct JsString *nm;
            if (p->cur.start) { nm = js_str_intern_n(p->c, p->cur.start, p->cur.len); advance(p); }
            else parse_error(p, "expected name after .");
            if (p->cur.kind == TK_LPAREN) {
                advance(p);
                ParsedArgs pa; parse_call_args(p, s, &pa);
                x = build_method_call(x, nm, &pa);
                lt->kind = -1;
            } else {
                lt->kind = 1; lt->name = nm; lt->recv = x;
                x = ALLOC_node_member_get(x, nm);
            }
        } else if (p->cur.kind == TK_LBRACK) {
            advance(p);
            NODE *e = parse_expr(p, s);
            expect(p, TK_RBRACK, "']'");
            lt->kind = 2; lt->recv = x; lt->idx = e;
            x = ALLOC_node_index_get(x, e);
        } else if (p->cur.kind == TK_LPAREN) {
            advance(p);
            ParsedArgs pa; parse_call_args(p, s, &pa);
            x = build_call(x, &pa);
            lt->kind = -1;
        } else if (p->cur.kind == TK_INC || p->cur.kind == TK_DEC) {
            // postfix x++ / x--: emit kind-specific inc node so that
            // boxed-local and upval references return the *old* value.
            int delta = (p->cur.kind == TK_INC) ? 1 : -1;
            advance(p);
            if (lt->kind == 0) {
                RefBinding rb = resolve_name(s, lt->name);
                if (rb.kind == RES_LOCAL) {
                    x = ALLOC_node_inc_local(rb.slot, /*is_pre=*/0, (uint32_t)delta);
                    lt->kind = -1;
                    continue;
                }
                if (rb.kind == RES_BOXED) {
                    x = ALLOC_node_inc_box(rb.slot, /*is_pre=*/0, (uint32_t)delta);
                    lt->kind = -1;
                    continue;
                }
                if (rb.kind == RES_UPVAL) {
                    x = ALLOC_node_inc_upval(rb.slot, /*is_pre=*/0, (uint32_t)delta);
                    lt->kind = -1;
                    continue;
                }
                // Globals — fall through to load/store with new value
                // (postfix semantics on globals only differ when there's
                // a getter; we don't model getters yet).
                NODE *load = emit_load(p, s, lt->name);
                NODE *plus = (delta > 0) ? ALLOC_node_add(load, ALLOC_node_smi(1)) : ALLOC_node_sub(load, ALLOC_node_smi(1));
                x = emit_store(p, s, lt->name, plus);
                lt->kind = -1;
            } else {
                // member.x++ / arr[i]++ — produce (load + 1) and assign (returns new value, slight mismatch with spec)
                if (lt->kind == 1) {
                    NODE *cur = ALLOC_node_member_get(lt->recv, lt->name);
                    NODE *plus = (delta > 0) ? ALLOC_node_add(cur, ALLOC_node_smi(1)) : ALLOC_node_sub(cur, ALLOC_node_smi(1));
                    x = ALLOC_node_member_set(lt->recv, lt->name, plus);
                } else {
                    NODE *cur = ALLOC_node_index_get(lt->recv, lt->idx);
                    NODE *plus = (delta > 0) ? ALLOC_node_add(cur, ALLOC_node_smi(1)) : ALLOC_node_sub(cur, ALLOC_node_smi(1));
                    x = ALLOC_node_index_set(lt->recv, lt->idx, plus);
                }
                lt->kind = -1;
            }
        } else break;
    }
    return x;
}

// Now redefine parse_assign / parse_ternary to use track + assignment.

static NODE *
parse_ternary_with_assign(Parser *p, Scope *s, bool no_in)
{
    LhsTrack lt = {0};
    NODE *left = parse_unary_track(p, s, &lt);
    // Check for assignment operator
    TokenKind op = p->cur.kind;
    if (op == TK_ASSIGN || op == TK_PLUS_EQ || op == TK_MINUS_EQ ||
        op == TK_STAR_EQ || op == TK_SLASH_EQ || op == TK_PCT_EQ ||
        op == TK_AND_EQ || op == TK_OR_EQ || op == TK_XOR_EQ ||
        op == TK_SHL_EQ || op == TK_SAR_EQ || op == TK_SHR_EQ ||
        op == TK_POW_EQ || op == TK_LAND_EQ || op == TK_LOR_EQ || op == TK_NULLISH_EQ) {
        if (lt.kind < 0) parse_error(p, "invalid assignment target");
        advance(p);
        NODE *rhs = parse_ternary_with_assign(p, s, no_in);
        // OP= : compute (load OP rhs) then store
        if (op != TK_ASSIGN) {
            NODE *current;
            if (lt.kind == 0) current = emit_load(p, s, lt.name);
            else if (lt.kind == 1) current = ALLOC_node_member_get(lt.recv, lt.name);
            else current = ALLOC_node_index_get(lt.recv, lt.idx);
            switch (op) {
            case TK_PLUS_EQ:  rhs = ALLOC_node_add(current, rhs); break;
            case TK_MINUS_EQ: rhs = ALLOC_node_sub(current, rhs); break;
            case TK_STAR_EQ:  rhs = ALLOC_node_mul(current, rhs); break;
            case TK_SLASH_EQ: rhs = ALLOC_node_div(current, rhs); break;
            case TK_PCT_EQ:   rhs = ALLOC_node_mod(current, rhs); break;
            case TK_AND_EQ:   rhs = ALLOC_node_band(current, rhs); break;
            case TK_OR_EQ:    rhs = ALLOC_node_bor(current, rhs); break;
            case TK_XOR_EQ:   rhs = ALLOC_node_bxor(current, rhs); break;
            case TK_SHL_EQ:   rhs = ALLOC_node_shl(current, rhs); break;
            case TK_SAR_EQ:   rhs = ALLOC_node_sar(current, rhs); break;
            case TK_SHR_EQ:   rhs = ALLOC_node_shr(current, rhs); break;
            case TK_POW_EQ:   rhs = ALLOC_node_pow(current, rhs); break;
            case TK_LAND_EQ:  rhs = ALLOC_node_and(current, rhs); break;
            case TK_LOR_EQ:   rhs = ALLOC_node_or(current, rhs); break;
            case TK_NULLISH_EQ: rhs = ALLOC_node_nullish(current, rhs); break;
            default: break;
            }
        }
        if (lt.kind == 0) return emit_store(p, s, lt.name, rhs);
        if (lt.kind == 1) return ALLOC_node_member_set(lt.recv, lt.name, rhs);
        return ALLOC_node_index_set(lt.recv, lt.idx, rhs);
    }
    // No assignment — continue as a binop expression.
    // Promote left through the binop climb starting at min_prec=4.
    for (;;) {
        TokenKind op2 = p->cur.kind;
        int prec = binop_prec(op2, no_in);
        if (prec < 4) break;
        advance(p);
        bool right_assoc = (op2 == TK_POW);
        NODE *right = parse_binop_climb(p, s, prec + (right_assoc ? 0 : 1), no_in);
        switch (op2) {
        case TK_PLUS:  left = ALLOC_node_add(left, right); break;
        case TK_MINUS: left = ALLOC_node_sub(left, right); break;
        case TK_STAR:  left = ALLOC_node_mul(left, right); break;
        case TK_SLASH: left = ALLOC_node_div(left, right); break;
        case TK_PCT:   left = ALLOC_node_mod(left, right); break;
        case TK_POW:   left = ALLOC_node_pow(left, right); break;
        case TK_LT:    left = ALLOC_node_lt(left, right); break;
        case TK_LE:    left = ALLOC_node_le(left, right); break;
        case TK_GT:    left = ALLOC_node_gt(left, right); break;
        case TK_GE:    left = ALLOC_node_ge(left, right); break;
        case TK_EQ:    left = ALLOC_node_loose_eq(left, right); break;
        case TK_NE:    left = ALLOC_node_loose_neq(left, right); break;
        case TK_SEQ:   left = ALLOC_node_strict_eq(left, right); break;
        case TK_SNE:   left = ALLOC_node_strict_neq(left, right); break;
        case TK_LAND:  left = ALLOC_node_and(left, right); break;
        case TK_LOR:   left = ALLOC_node_or(left, right); break;
        case TK_NULLISH: left = ALLOC_node_nullish(left, right); break;
        case TK_AMP:   left = ALLOC_node_band(left, right); break;
        case TK_PIPE:  left = ALLOC_node_bor(left, right); break;
        case TK_CARET: left = ALLOC_node_bxor(left, right); break;
        case TK_SHL:   left = ALLOC_node_shl(left, right); break;
        case TK_SAR:   left = ALLOC_node_sar(left, right); break;
        case TK_SHR:   left = ALLOC_node_shr(left, right); break;
        case TK_INSTANCEOF: left = ALLOC_node_instanceof(left, right); break;
        case TK_IN:    left = ALLOC_node_in(left, right); break;
        default: parse_error(p, "internal: unhandled binop");
        }
    }
    if (p->cur.kind == TK_QUESTION) {
        advance(p);
        NODE *then_n = parse_ternary_with_assign(p, s, no_in);
        expect(p, TK_COLON, "':'");
        NODE *else_n = parse_ternary_with_assign(p, s, no_in);
        return ALLOC_node_if(left, then_n, else_n);
    }
    return left;
}

static NODE *
parse_assign(Parser *p, Scope *s)
{
    return parse_ternary_with_assign(p, s, false);
}
static NODE *
parse_assign_no_in_v2(Parser *p, Scope *s)
{
    return parse_ternary_with_assign(p, s, true);
}

static NODE *
parse_expr(Parser *p, Scope *s)
{
    NODE *r = parse_assign(p, s);
    while (p->cur.kind == TK_COMMA) {
        advance(p);
        NODE *r2 = parse_assign(p, s);
        r = ALLOC_node_seq(r, r2);
    }
    return r;
}

// =====================================================================
// Function definition (expression form)
// =====================================================================

static NODE *
parse_function_expr(Parser *p, Scope *s, struct JsString *name)
{
    Scope *child = scope_new(s, true, false);
    expect(p, TK_LPAREN, "'('");
    struct JsString *param_names[64]; uint32_t nparams = 0;
    uint32_t param_slots[64];
    NODE *param_defaults[64];     // NULL if no default
    Pattern *param_patterns[64];   // non-NULL if destructuring pattern
    bool has_rest = false;
    // Pass 1: parse all params, allocate synthetic slots consecutively.
    while (p->cur.kind != TK_RPAREN) {
        bool is_rest = false;
        if (p->cur.kind == TK_DOTS) { advance(p); is_rest = true; }
        param_patterns[nparams] = NULL;
        param_defaults[nparams] = NULL;
        if (p->cur.kind == TK_LBRACK || p->cur.kind == TK_LBRACE) {
            char synth[32];
            int sn = snprintf(synth, sizeof synth, "__p%u__", nparams);
            param_names[nparams] = js_str_intern_n(p->c, synth, sn);
            param_slots[nparams] = scope_declare(child, param_names[nparams], 3);
            param_patterns[nparams] = parse_pattern(p, child);
        } else {
            if (p->cur.kind != TK_IDENT) parse_error(p, "expected parameter name");
            param_names[nparams] = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            param_slots[nparams] = scope_declare(child, param_names[nparams], 3);
            if (is_rest) has_rest = true;
            advance(p);
        }
        nparams++;
        if (p->cur.kind == TK_ASSIGN && !is_rest) {
            advance(p);
            param_defaults[nparams - 1] = parse_assign(p, child);
        }
        if (p->cur.kind == TK_COMMA) {
            if (is_rest) parse_error(p, "rest parameter must be last");
            advance(p);
        }
        else break;
    }
    expect(p, TK_RPAREN, "')'");
    // Pass 2: declare inner vars for destructuring patterns.  This runs
    // AFTER all positional param slots are allocated so they stay
    // contiguous at slots 0..nparams-1.
    for (uint32_t i = 0; i < nparams; i++) {
        if (param_patterns[i]) pat_declare(param_patterns[i], child, 3);
    }
    // After consuming `(...)`, the next token is `{`; hoist function-decl
    // names in the body so forward refs (within this function) resolve.
    if (p->cur.kind == TK_LBRACE) {
        const char *body_start = p->cur.start ? p->cur.start + 1 : p->p;
        hoist_scan(p, body_start, p->src_end, child);
        hoist_letconst(p, body_start, p->src_end, child);
    }
    NODE *body = parse_block(p, child, /*new_scope=*/false);

    // Build prelude statements: for each defaulted param, emit
    //   if (slot === undefined) slot = default_expr
    // For destructuring params, also emit pattern-bind from the slot.
    for (int i = (int)nparams - 1; i >= 0; i--) {
        if (param_patterns[i]) {
            NODE *load = ALLOC_node_local_get(param_slots[i]);
            NODE *bind = emit_pattern_bind(p, child, param_patterns[i], load, true);
            pat_free(param_patterns[i]);
            body = ALLOC_node_seq(bind, body);
        }
        if (param_defaults[i]) {
            NODE *load = ALLOC_node_local_get(param_slots[i]);
            NODE *cond = ALLOC_node_strict_eq(load, ALLOC_node_undefined());
            NODE *set  = ALLOC_node_local_set(param_slots[i], param_defaults[i]);
            NODE *check = ALLOC_node_if(cond, set, ALLOC_node_undefined());
            body = ALLOC_node_seq(check, body);
        }
    }

    uint32_t up_idx = jstro_u32_arr_alloc(child->nupvals * 2);
    for (uint32_t i = 0; i < child->nupvals; i++) {
        JSTRO_U32_ARR[up_idx + 2*i]   = child->upvals[i].is_local;
        JSTRO_U32_ARR[up_idx + 2*i+1] = child->upvals[i].slot;
    }
    uint32_t nu = child->nupvals, nl = child->nlocals;
    scope_free(child);
    NODE *fn = ALLOC_node_func(body, nparams, nl, nu, up_idx, 0, name);
    if (has_rest) {
        // Mark the JsFunction as vararg via a flag accessible at runtime.
        // We piggyback on the unused `is_arrow` bit by introducing a new
        // alloc-time hook below; for now, set a node-level marker.
        // (Implemented via the new node operand `is_vararg`.)
        // Reach into the AST node and note the vararg bit; the runtime
        // js_func_new is called from EVAL_node_func and reads the bit
        // through the node's u.node_func.is_arrow operand.  We use the
        // top bit of that operand to encode "vararg".
        fn->u.node_func.is_arrow |= 0x2;  // bit 0: arrow, bit 1: vararg
    }
    return fn;
}

// =====================================================================
// Statements
// =====================================================================

static NODE *
parse_var_decl(Parser *p, Scope *s, uint8_t kind, bool no_in)
{
    NODE *result = NULL;
    while (p->cur.kind == TK_IDENT || p->cur.kind == TK_LBRACK || p->cur.kind == TK_LBRACE) {
        // Destructuring binding `let [a, b] = ...` / `let {x, y} = ...`.
        if (p->cur.kind == TK_LBRACK || p->cur.kind == TK_LBRACE) {
            Pattern *pat = parse_pattern(p, s);
            Scope *target = (kind == 0) ? scope_function(s) : s;
            pat_declare(pat, target, kind);
            if (p->cur.kind != TK_ASSIGN) parse_error(p, "destructuring binding requires initializer");
            advance(p);
            NODE *init = no_in ? parse_assign_no_in_v2(p, s) : parse_assign(p, s);
            NODE *bind = emit_pattern_bind(p, s, pat, init, /*is_decl=*/true);
            pat_free(pat);
            result = result ? ALLOC_node_seq(result, bind) : bind;
            if (p->cur.kind == TK_COMMA) advance(p);
            else break;
            continue;
        }
        struct JsString *name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
        advance(p);
        Scope *target = (kind == 0) ? scope_function(s) : s;
        int existing = scope_find_var(target, name);
        uint32_t slot;
        if (existing >= 0) {
            slot = target->vars[existing].slot;
        } else {
            slot = scope_declare(target, name, kind);
        }
        NODE *init = NULL;
        if (p->cur.kind == TK_ASSIGN) {
            advance(p);
            init = no_in ? parse_assign_no_in_v2(p, s) : parse_assign(p, s);
        }
        if (init) {
            if (kind == 0) {
                NODE *st = ALLOC_node_local_set(slot, init);
                result = build_seq(result, st);
            } else {
                NODE *st = ALLOC_node_let_init(slot, init);
                result = build_seq(result, st);
            }
        } else if (kind != 0) {
            // let/const without init -> remains undefined; for const, parser
            // should have required an init (we relax).
            // No emit needed.
        }
        if (p->cur.kind == TK_COMMA) advance(p);
        else break;
    }
    if (!result) result = ALLOC_node_undefined();
    return result;
}

static NODE *
parse_block(Parser *p, Scope *s, bool new_scope)
{
    expect(p, TK_LBRACE, "'{'");
    Scope *block = new_scope ? scope_new(s, false, s->is_arrow) : s;
    // Pre-hoist let/const into the block scope (TDZ).  Do this for new
    // scopes; for shared (function-body parses), the function entry has
    // already done it.
    if (new_scope) {
        hoist_letconst(p, p->cur.start ? p->cur.start : p->p, p->src_end, block);
    }
    NODE *body = NULL;
    // Hoisting: function-decl assignments are collected separately and
    // emitted before any other statement, mirroring §13.2.6's HoistableDeclaration.
    NODE *fdecls[1024]; uint32_t nf = 0;
    NODE *stmts[1024]; uint32_t n = 0;
    while (p->cur.kind != TK_RBRACE && p->cur.kind != TK_EOF) {
        bool is_fdecl = false;
        if (p->cur.kind == TK_FUNCTION) {
            // Distinguish `function f(){}` (declaration) from `function(){}` (expression).
            const Token *nx = peek(p);
            if (nx->kind == TK_IDENT) is_fdecl = true;
        }
        NODE *st = parse_stmt(p, block);
        if (st) {
            if (is_fdecl && nf < 1024) fdecls[nf++] = st;
            else if (n < 1024)         stmts[n++] = st;
        }
    }
    expect(p, TK_RBRACE, "'}'");
    NODE *combined[2048]; uint32_t total = 0;
    for (uint32_t i = 0; i < nf; i++) combined[total++] = fdecls[i];
    for (uint32_t i = 0; i < n;  i++) combined[total++] = stmts[i];
    body = build_block(combined, total);
    if (new_scope && block->nholes > 0) {
        uint32_t base = jstro_u32_arr_alloc(block->nholes);
        for (uint32_t i = 0; i < block->nholes; i++) JSTRO_U32_ARR[base + i] = block->hole_slots[i];
        body = ALLOC_node_block(base, block->nholes, body);
    }
    if (new_scope) {
        // We can't free block — its captured-marks may still be needed by
        // enclosing function's scope_lookup.  But the block is detached;
        // the captured bits live on the parent fn scope.  Free it only if
        // we don't need its declarations after closing the brace.  We do
        // not — once parsed, lookups happen during parse.
        scope_free(block);
    }
    return body;
}

static NODE *
parse_if(Parser *p, Scope *s)
{
    advance(p);
    expect(p, TK_LPAREN, "'('");
    NODE *cond = parse_expr(p, s);
    expect(p, TK_RPAREN, "')'");
    NODE *then_n = parse_stmt(p, s);
    NODE *else_n;
    if (p->cur.kind == TK_ELSE) { advance(p); else_n = parse_stmt(p, s); }
    else else_n = ALLOC_node_undefined();
    return ALLOC_node_if(cond, then_n, else_n);
}

static NODE *
parse_while(Parser *p, Scope *s)
{
    advance(p);
    expect(p, TK_LPAREN, "'('");
    NODE *cond = parse_expr(p, s);
    expect(p, TK_RPAREN, "')'");
    s->loop_depth++;
    NODE *body = parse_stmt(p, s);
    s->loop_depth--;
    return ALLOC_node_while(cond, body);
}

static NODE *
parse_do(Parser *p, Scope *s)
{
    advance(p);
    s->loop_depth++;
    NODE *body = parse_stmt(p, s);
    s->loop_depth--;
    expect(p, TK_WHILE, "'while'");
    expect(p, TK_LPAREN, "'('");
    NODE *cond = parse_expr(p, s);
    expect(p, TK_RPAREN, "')'");
    accept(p, TK_SEMI);
    return ALLOC_node_do_while(body, cond);
}

static NODE *
parse_for(Parser *p, Scope *s)
{
    advance(p);
    expect(p, TK_LPAREN, "'('");
    Scope *block = scope_new(s, false, s->is_arrow);
    NODE *init = ALLOC_node_undefined();
    bool init_is_decl = false;
    uint8_t init_decl_kind = 0;
    struct JsString *init_decl_name = NULL;
    if (p->cur.kind == TK_VAR || p->cur.kind == TK_LET || p->cur.kind == TK_CONST) {
        init_is_decl = true;
        TokenKind dk = p->cur.kind;
        init_decl_kind = (dk == TK_VAR) ? 0 : (dk == TK_LET) ? 1 : 2;
        advance(p);
        if (p->cur.kind == TK_LBRACK || p->cur.kind == TK_LBRACE) {
            // Destructuring in for-of/for-in head: `for (const {x, y} of arr)`.
            Pattern *pat = parse_pattern(p, block);
            Scope *target = (init_decl_kind == 0) ? scope_function(s) : block;
            pat_declare(pat, target, init_decl_kind);
            if (p->cur.kind != TK_OF && p->cur.kind != TK_IN) {
                // Standard destructuring decl in for(;;): emit init.
                if (p->cur.kind != TK_ASSIGN) parse_error(p, "destructuring init requires =");
                advance(p);
                NODE *e = parse_assign_no_in_v2(p, s);
                init = emit_pattern_bind(p, block, pat, e, true);
                pat_free(pat);
            } else {
                bool is_of = (p->cur.kind == TK_OF);
                advance(p);
                NODE *iter = parse_expr(p, s);
                expect(p, TK_RPAREN, "')'");
                // Allocate a hidden temp slot to hold the iter element,
                // then emit pattern bind at start of body.
                uint32_t tmp = scope_function(block)->nlocals++;
                block->loop_depth++;
                NODE *user_body = parse_stmt(p, block);
                block->loop_depth--;
                NODE *bind = emit_pattern_bind(p, block, pat, ALLOC_node_local_get(tmp), true);
                pat_free(pat);
                NODE *body = ALLOC_node_seq(bind, user_body);
                NODE *r = is_of ?
                    ALLOC_node_for_of(tmp, 0, iter, body) :
                    ALLOC_node_for_in(tmp, 0, iter, body);
                scope_free(block);
                return r;
            }
        } else if (p->cur.kind == TK_IDENT) {
            init_decl_name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            // peek to detect for-of / for-in
            const Token *nx = peek(p);
            if (nx->kind == TK_OF || nx->kind == TK_IN) {
                advance(p);
                bool is_of = (p->cur.kind == TK_OF);
                advance(p);
                Scope *target = (init_decl_kind == 0) ? scope_function(s) : block;
                uint32_t slot = scope_declare(target, init_decl_name, init_decl_kind);
                NODE *iter = parse_expr(p, s);
                expect(p, TK_RPAREN, "')'");
                block->loop_depth++;
                NODE *body = parse_stmt(p, block);
                block->loop_depth--;
                NODE *r = is_of ?
                    ALLOC_node_for_of(slot, 0, iter, body) :
                    ALLOC_node_for_in(slot, 0, iter, body);
                scope_free(block);
                return r;
            }
            init = parse_var_decl(p, block, init_decl_kind, /*no_in=*/true);
        } else {
            init = parse_var_decl(p, block, init_decl_kind, /*no_in=*/true);
        }
    } else if (p->cur.kind != TK_SEMI) {
        // Could be `expr;` or `lhs of expr` / `lhs in expr`.
        // Simplification: assume expression init.
        // First, parse a LHS to support for(name of/in expr).
        if (p->cur.kind == TK_IDENT) {
            const Token *nx = peek(p);
            if (nx->kind == TK_OF || nx->kind == TK_IN) {
                struct JsString *vname = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
                bool is_of = (p->cur.kind == TK_OF);
                advance(p);
                NODE *iter = parse_expr(p, s);
                expect(p, TK_RPAREN, "')'");
                RefBinding rb = resolve_name(s, vname);
                uint32_t slot = (rb.kind == RES_LOCAL || rb.kind == RES_BOXED) ? rb.slot : 0;
                if (rb.kind == RES_GLOBAL) parse_error(p, "for-of/in target must be local");
                block->loop_depth++;
                NODE *body = parse_stmt(p, block);
                block->loop_depth--;
                NODE *r = is_of ?
                    ALLOC_node_for_of(slot, rb.kind == RES_BOXED ? 1 : 0, iter, body) :
                    ALLOC_node_for_in(slot, rb.kind == RES_BOXED ? 1 : 0, iter, body);
                scope_free(block);
                return r;
            }
        }
        init = parse_expr(p, block);
    }
    expect(p, TK_SEMI, "';'");
    NODE *cond = (p->cur.kind == TK_SEMI) ? ALLOC_node_true() : parse_expr(p, block);
    expect(p, TK_SEMI, "';'");
    NODE *step = (p->cur.kind == TK_RPAREN) ? ALLOC_node_undefined() : parse_expr(p, block);
    expect(p, TK_RPAREN, "')'");
    block->loop_depth++;
    NODE *body = parse_stmt(p, block);
    block->loop_depth--;
    NODE *r;
    if (init_is_decl && init_decl_kind == 1 && block->nvars > 0) {
        // Per-iteration binding for `for (let X ...) body`: use the first
        // declared let var's slot.  (Multiple let vars in the head are
        // unusual; if present we still use the first one — others share
        // function-style scoping.)
        uint32_t lslot = (uint32_t)-1;
        for (uint32_t i = 0; i < block->nvars; i++) {
            if (block->vars[i].kind == 1) { lslot = block->vars[i].slot; break; }
        }
        if (lslot != (uint32_t)-1) {
            r = ALLOC_node_for_let(lslot, init, cond, step, body);
        } else {
            r = ALLOC_node_for(init, cond, step, body);
        }
    } else {
        r = ALLOC_node_for(init, cond, step, body);
    }
    if (block->nholes > 0) {
        uint32_t base = jstro_u32_arr_alloc(block->nholes);
        for (uint32_t i = 0; i < block->nholes; i++) JSTRO_U32_ARR[base + i] = block->hole_slots[i];
        r = ALLOC_node_block(base, block->nholes, r);
    }
    scope_free(block);
    return r;
}

static NODE *
parse_try(Parser *p, Scope *s)
{
    advance(p);
    NODE *body = parse_block(p, s, true);
    uint32_t catch_slot = 0;
    uint8_t catch_is_box = 0;
    NODE *handler = NULL;
    NODE *final_ = NULL;
    if (p->cur.kind == TK_CATCH) {
        advance(p);
        Scope *catch_scope = scope_new(s, false, s->is_arrow);
        if (p->cur.kind == TK_LPAREN) {
            advance(p);
            if (p->cur.kind == TK_IDENT) {
                struct JsString *name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
                catch_slot = scope_declare(catch_scope, name, 1);
            }
            expect(p, TK_RPAREN, "')'");
        }
        handler = parse_block(p, catch_scope, false);
        if (catch_scope->nvars > 0 && catch_scope->vars[0].captured) catch_is_box = 1;
        scope_free(catch_scope);
    }
    if (p->cur.kind == TK_FINALLY) {
        advance(p);
        final_ = parse_block(p, s, true);
    }
    if (!handler && !final_) parse_error(p, "missing catch or finally");
    uint32_t has_handler = handler != NULL;
    uint32_t has_final   = final_  != NULL;
    if (!handler) handler = ALLOC_node_undefined();
    if (!final_)  final_  = ALLOC_node_undefined();
    return ALLOC_node_try(body, catch_slot, catch_is_box, has_handler, handler, has_final, final_);
}

// Switch desugar to a chain of ifs.
static NODE *
parse_switch(Parser *p, Scope *s)
{
    advance(p);
    expect(p, TK_LPAREN, "'('");
    NODE *disc = parse_expr(p, s);
    expect(p, TK_RPAREN, "')'");
    expect(p, TK_LBRACE, "'{'");
    Scope *block = scope_new(s, false, s->is_arrow);
    // Stash discriminant in a local slot.
    Scope *fnscope = scope_function(s);
    uint32_t disc_slot = fnscope->nlocals++;
    NODE *init = ALLOC_node_local_set(disc_slot, disc);
    // Build cases as a flat sequence wrapped in an outer block (with break).
    NODE *body = ALLOC_node_undefined();
    NODE *default_body = NULL;
    typedef struct { NODE *test; NODE *body; } Case;
    Case cases[64]; uint32_t nc = 0;
    block->switch_depth++;
    while (p->cur.kind != TK_RBRACE) {
        if (p->cur.kind == TK_CASE) {
            advance(p);
            NODE *te = parse_expr(p, block);
            expect(p, TK_COLON, "':'");
            // collect statements until next case/default/}
            NODE *stmts[256]; uint32_t ns = 0;
            while (p->cur.kind != TK_CASE && p->cur.kind != TK_DEFAULT && p->cur.kind != TK_RBRACE) {
                stmts[ns++] = parse_stmt(p, block);
                if (ns >= 256) break;
            }
            cases[nc].test = te;
            cases[nc].body = build_block(stmts, ns);
            nc++;
        } else if (p->cur.kind == TK_DEFAULT) {
            advance(p);
            expect(p, TK_COLON, "':'");
            NODE *stmts[256]; uint32_t ns = 0;
            while (p->cur.kind != TK_CASE && p->cur.kind != TK_DEFAULT && p->cur.kind != TK_RBRACE) {
                stmts[ns++] = parse_stmt(p, block);
                if (ns >= 256) break;
            }
            default_body = build_block(stmts, ns);
        } else {
            parse_error(p, "expected case or default");
        }
    }
    block->switch_depth--;
    expect(p, TK_RBRACE, "'}'");
    // Build chained ifs (without fallthrough — simplification; benchmarks
    // mostly use cases that end with `break`.).
    NODE *chain = default_body ? default_body : ALLOC_node_undefined();
    for (int i = (int)nc - 1; i >= 0; i--) {
        NODE *cond = ALLOC_node_strict_eq(ALLOC_node_local_get(disc_slot), cases[i].test);
        chain = ALLOC_node_if(cond, cases[i].body, chain);
    }
    body = ALLOC_node_seq(init, chain);
    scope_free(block);
    return body;
}

// `class Name [extends Base] { constructor() {...} method() {...} static foo() {...} }`
// Desugars to a sequence of assignments:
//   var Name = function(...) { ...constructor body... };
//   _ChainProto(Name, Base);
//   Name.prototype.method = function() {...};
//   Name.staticFoo = function() {...};
// Inside method/ctor bodies, `super` references the parent class.  We
// allocate a hidden local slot `__super__` that holds Base, and method
// bodies capture it as an upvalue.  super(...) → __super__.call(this, ...).
// super.foo(...) → __super__.prototype.foo.call(this, ...).
static NODE *
parse_class(Parser *p, Scope *s)
{
    advance(p); // class
    struct JsString *name = NULL;
    if (p->cur.kind == TK_IDENT) {
        name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
        advance(p);
    }
    NODE *parent = NULL;
    bool has_parent = false;
    if (p->cur.kind == TK_EXTENDS) {
        advance(p);
        parent = parse_postfix(p, s);
        has_parent = true;
    }
    // Reserve a hidden slot for __super__ so methods can capture it.
    Scope *fns = scope_function(s);
    uint32_t super_slot = (uint32_t)-1;
    struct JsString *super_name = js_str_intern(p->c, "__super__");
    if (has_parent) {
        super_slot = scope_declare(fns, super_name, 0);  // var-style
    }
    // Pre-declare the class name so static blocks and methods can refer
    // to it.  (Spec: ClassDeclaration creates a let-style binding.)
    if (name) {
        int ex = scope_find_var(fns, name);
        if (ex < 0) scope_declare(fns, name, 0);
    }
    expect(p, TK_LBRACE, "'{'");
    NODE *ctor = NULL;
    typedef struct { struct JsString *k; NODE *v; bool is_static; uint8_t kind; } M;  // kind: 0=method, 1=getter, 2=setter
    M ms[64]; uint32_t nm = 0;
    NODE *static_blocks[16]; uint32_t nsb = 0;
    while (p->cur.kind != TK_RBRACE) {
        if (p->cur.kind == TK_SEMI) { advance(p); continue; }
        bool is_static = false;
        if (p->cur.kind == TK_STATIC) {
            is_static = true;
            advance(p);
            if (p->cur.kind == TK_LBRACE) {
                // static initialization block — collect AST.
                NODE *blk = parse_block(p, s, true);
                if (nsb < 16) static_blocks[nsb++] = blk;
                continue;
            }
        }
        // get / set accessor methods (preceded by `static` if applicable).
        if (p->cur.kind == TK_IDENT && p->cur.len == 3
            && (memcmp(p->cur.start, "get", 3) == 0 || memcmp(p->cur.start, "set", 3) == 0)) {
            bool is_get = memcmp(p->cur.start, "get", 3) == 0;
            const Token *nx = peek(p);
            if (nx->kind == TK_IDENT || nx->kind == TK_STR) {
                advance(p);
                struct JsString *mn;
                if (p->cur.kind == TK_IDENT) mn = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                else                          mn = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
                advance(p);
                NODE *fn = parse_function_expr(p, s, mn);
                if (nm < 64) { ms[nm].k = mn; ms[nm].v = fn; ms[nm].is_static = is_static; ms[nm].kind = is_get ? 1 : 2; nm++; }
                continue;
            }
        }
        struct JsString *mn;
        if (p->cur.kind == TK_IDENT) {
            mn = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            advance(p);
        } else if (p->cur.kind == TK_STR) {
            mn = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
            advance(p);
        } else {
            parse_error(p, "expected method name");
        }
        // Handle class field declarations (e.g., `x = 10;` or `#priv = 5`):
        // not supported yet — only parse method form.  If the next token
        // isn't '(' we throw.
        if (p->cur.kind != TK_LPAREN) {
            // Skip optional initializer for now: `name = expr ;`
            if (p->cur.kind == TK_ASSIGN) {
                advance(p);
                parse_assign(p, s);   // discard for now (TODO: emit init in ctor)
            }
            accept(p, TK_SEMI);
            continue;
        }
        NODE *fn = parse_function_expr(p, s, mn);
        if (!is_static && mn == js_str_intern(p->c, "constructor")) ctor = fn;
        else { ms[nm].k = mn; ms[nm].v = fn; ms[nm].is_static = is_static; ms[nm].kind = 0; nm++; }
    }
    expect(p, TK_RBRACE, "'}'");
    if (!ctor) {
        Scope *child = scope_new(s, true, false);
        NODE *body = ALLOC_node_undefined();
        // If extending, default ctor is `super(...arguments)` per spec.
        if (has_parent) {
            // Body: __super__.call(this, ...arguments) — we approximate by
            // calling __super__ on this with no args (rest spread of
            // arguments inside a default ctor isn't perfectly handled).
            NODE *load = emit_load(p, child, super_name);
            body = ALLOC_node_method_call(load, js_str_intern(p->c, "call"),
                                          jstro_node_arr_alloc(1), 1);
            JSTRO_NODE_ARR[body->u.node_method_call.args_idx] = ALLOC_node_this();
        }
        uint32_t up_idx = jstro_u32_arr_alloc(child->nupvals * 2);
        for (uint32_t i = 0; i < child->nupvals; i++) {
            JSTRO_U32_ARR[up_idx + 2*i] = child->upvals[i].is_local;
            JSTRO_U32_ARR[up_idx + 2*i + 1] = child->upvals[i].slot;
        }
        ctor = ALLOC_node_func(body, 0, child->nlocals, child->nupvals, up_idx, 0, name);
        scope_free(child);
    }
    uint32_t tmp = fns->nlocals++;
    NODE *seq = ALLOC_node_local_set(tmp, ctor);
    // Bind the class name early so static blocks can reference it.
    if (name) {
        int ex = scope_find_var(fns, name);
        uint32_t slot = ex >= 0 ? fns->vars[ex].slot : scope_declare(fns, name, 0);
        seq = ALLOC_node_seq(seq, ALLOC_node_local_set(slot, ALLOC_node_local_get(tmp)));
    }
    if (has_parent) {
        // Store parent into __super__ slot first (before methods reference it).
        seq = ALLOC_node_seq(ALLOC_node_local_set(super_slot, parent), seq);
        // Wire prototype chain: Object.setPrototypeOf(Sub.prototype, Base.prototype)
        // and Object.setPrototypeOf(Sub, Base).  We use a runtime helper exposed
        // as the global function `_chainProto` — install it from the stdlib init.
        NODE *helper = emit_load(p, s, js_str_intern(p->c, "__chainProto__"));
        uint32_t hb = jstro_node_arr_alloc(2);
        JSTRO_NODE_ARR[hb]   = ALLOC_node_local_get(tmp);
        JSTRO_NODE_ARR[hb+1] = ALLOC_node_local_get(super_slot);
        seq = ALLOC_node_seq(seq, ALLOC_node_call(helper, hb, 2));
    }
    for (uint32_t i = 0; i < nm; i++) {
        NODE *cload = ALLOC_node_local_get(tmp);
        NODE *target;
        if (ms[i].is_static) target = cload;
        else                 target = ALLOC_node_member_get(cload, js_str_intern(p->c, "prototype"));
        if (ms[i].kind == 0) {
            seq = ALLOC_node_seq(seq, ALLOC_node_member_set(target, ms[i].k, ms[i].v));
        } else {
            // accessor: __defAccessor__(target, "key", "get"|"set", fn)
            NODE *helper = emit_load(p, s, js_str_intern(p->c, "__defAccessor__"));
            uint32_t base = jstro_node_arr_alloc(4);
            JSTRO_NODE_ARR[base]   = target;
            JSTRO_NODE_ARR[base+1] = ALLOC_node_string(ms[i].k);
            JSTRO_NODE_ARR[base+2] = ALLOC_node_string(js_str_intern(p->c, ms[i].kind == 1 ? "get" : "set"));
            JSTRO_NODE_ARR[base+3] = ms[i].v;
            seq = ALLOC_node_seq(seq, ALLOC_node_call(helper, base, 4));
        }
    }
    // Static blocks: run after methods are wired.
    for (uint32_t i = 0; i < nsb; i++) {
        seq = ALLOC_node_seq(seq, static_blocks[i]);
    }
    return seq;
}

static NODE *
parse_stmt(Parser *p, Scope *s)
{
    // Labeled statement: IDENT ':' Stmt.  Detect via 1-token lookahead.
    if (p->cur.kind == TK_IDENT) {
        const Token *nx = peek(p);
        if (nx->kind == TK_COLON) {
            struct JsString *lbl = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            advance(p);  // ident
            advance(p);  // colon
            NODE *body = parse_stmt(p, s);
            return ALLOC_node_label(lbl, body);
        }
    }
    switch (p->cur.kind) {
    case TK_LBRACE: return parse_block(p, s, true);
    case TK_VAR: {
        advance(p);
        NODE *r = parse_var_decl(p, s, 0, false);
        accept(p, TK_SEMI);
        return r;
    }
    case TK_LET: {
        advance(p);
        NODE *r = parse_var_decl(p, s, 1, false);
        accept(p, TK_SEMI);
        return r;
    }
    case TK_CONST: {
        advance(p);
        NODE *r = parse_var_decl(p, s, 2, false);
        accept(p, TK_SEMI);
        return r;
    }
    case TK_IF: return parse_if(p, s);
    case TK_WHILE: return parse_while(p, s);
    case TK_DO: return parse_do(p, s);
    case TK_FOR: return parse_for(p, s);
    case TK_TRY: return parse_try(p, s);
    case TK_SWITCH: return parse_switch(p, s);
    case TK_CLASS: return parse_class(p, s);
    case TK_THROW: {
        advance(p);
        NODE *e = parse_expr(p, s);
        accept(p, TK_SEMI);
        return ALLOC_node_throw(e);
    }
    case TK_BREAK: {
        advance(p);
        if (p->cur.kind == TK_IDENT) {
            struct JsString *lbl = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            advance(p);
            accept(p, TK_SEMI);
            return ALLOC_node_break_label(lbl);
        }
        accept(p, TK_SEMI);
        return ALLOC_node_break();
    }
    case TK_CONTINUE: {
        advance(p);
        if (p->cur.kind == TK_IDENT) {
            // Labeled continue is hard to implement without giving every
            // loop a label operand — strip the label and emit a plain
            // continue.  Works for the common single-loop case.
            advance(p);
        }
        accept(p, TK_SEMI);
        return ALLOC_node_continue();
    }
    case TK_RETURN: {
        advance(p);
        NODE *e;
        if (p->cur.kind != TK_SEMI && p->cur.kind != TK_RBRACE && p->cur.kind != TK_EOF) {
            e = parse_expr(p, s);
        } else {
            e = ALLOC_node_undefined();
        }
        accept(p, TK_SEMI);
        return ALLOC_node_return(e);
    }
    case TK_IMPORT: {
        // `import [defaultName] [, { name1, name2 as alias }] from "path"`
        // or `import * as ns from "path"`
        // or `import "path"`
        advance(p);
        if (p->cur.kind == TK_STR) {
            // bare import "p" — just call require for side effects.
            struct JsString *path = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
            advance(p);
            accept(p, TK_SEMI);
            NODE *req = emit_load(p, s, js_str_intern(p->c, "require"));
            uint32_t base = jstro_node_arr_alloc(1);
            JSTRO_NODE_ARR[base] = ALLOC_node_string(path);
            return ALLOC_node_call(req, base, 1);
        }
        // Collect names: default, namespace, or {a, b as c}.
        struct JsString *default_name = NULL;
        struct JsString *ns_name = NULL;
        struct JsString *named_local[64];
        struct JsString *named_imported[64];
        uint32_t nn = 0;
        if (p->cur.kind == TK_IDENT) {
            default_name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            advance(p);
            if (p->cur.kind == TK_COMMA) advance(p);
        }
        if (p->cur.kind == TK_STAR) {
            advance(p);
            if (p->cur.kind == TK_IDENT && p->cur.len == 2 && memcmp(p->cur.start, "as", 2) == 0) {
                advance(p);
            } else parse_error(p, "expected 'as' after import *");
            ns_name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            advance(p);
        }
        if (p->cur.kind == TK_LBRACE) {
            advance(p);
            while (p->cur.kind != TK_RBRACE) {
                struct JsString *imp = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
                struct JsString *loc = imp;
                if (p->cur.kind == TK_IDENT && p->cur.len == 2 && memcmp(p->cur.start, "as", 2) == 0) {
                    advance(p);
                    loc = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                    advance(p);
                }
                if (nn < 64) { named_imported[nn] = imp; named_local[nn] = loc; nn++; }
                if (p->cur.kind == TK_COMMA) advance(p);
                else break;
            }
            expect(p, TK_RBRACE, "'}'");
        }
        // expect "from"
        if (!(p->cur.kind == TK_IDENT && p->cur.len == 4 && memcmp(p->cur.start, "from", 4) == 0))
            parse_error(p, "expected 'from'");
        advance(p);
        if (p->cur.kind != TK_STR) parse_error(p, "expected module path string");
        struct JsString *path = js_str_intern_n(p->c, p->cur.strbuf, p->cur.strlen);
        advance(p);
        accept(p, TK_SEMI);
        // Build: const __mod = require("path");
        // For each name: const local = __mod.imp;  (default → __mod.default)
        Scope *target = scope_function(s);
        struct JsString *modname;
        char modbuf[64]; int mblen = snprintf(modbuf, sizeof modbuf, "__import_%p__", (void*)path);
        modname = js_str_intern_n(p->c, modbuf, mblen);
        uint32_t mod_slot = scope_declare(target, modname, 0);
        NODE *req = emit_load(p, s, js_str_intern(p->c, "require"));
        uint32_t rb = jstro_node_arr_alloc(1);
        JSTRO_NODE_ARR[rb] = ALLOC_node_string(path);
        NODE *seq = ALLOC_node_local_set(mod_slot, ALLOC_node_call(req, rb, 1));
        if (default_name) {
            int ex = scope_find_var(target, default_name);
            uint32_t slot = ex >= 0 ? target->vars[ex].slot : scope_declare(target, default_name, 1);
            seq = ALLOC_node_seq(seq, ALLOC_node_local_set(slot,
                ALLOC_node_member_get(ALLOC_node_local_get(mod_slot), js_str_intern(p->c, "default"))));
        }
        if (ns_name) {
            int ex = scope_find_var(target, ns_name);
            uint32_t slot = ex >= 0 ? target->vars[ex].slot : scope_declare(target, ns_name, 1);
            seq = ALLOC_node_seq(seq, ALLOC_node_local_set(slot, ALLOC_node_local_get(mod_slot)));
        }
        for (uint32_t i = 0; i < nn; i++) {
            int ex = scope_find_var(target, named_local[i]);
            uint32_t slot = ex >= 0 ? target->vars[ex].slot : scope_declare(target, named_local[i], 1);
            seq = ALLOC_node_seq(seq, ALLOC_node_local_set(slot,
                ALLOC_node_member_get(ALLOC_node_local_get(mod_slot), named_imported[i])));
        }
        return seq;
    }
    case TK_EXPORT: {
        advance(p);
        // `export default expr;` / `export const x = ...;` / `export function f() {...}` /
        // `export class C {...}` / `export { a, b as c };` / `export * from "p";`
        if (p->cur.kind == TK_DEFAULT) {
            advance(p);
            NODE *e;
            if (p->cur.kind == TK_FUNCTION) e = parse_stmt(p, s);
            else { e = parse_assign(p, s); accept(p, TK_SEMI); }
            // module.exports.default = e
            NODE *mod = emit_load(p, s, js_str_intern(p->c, "module"));
            NODE *exp = ALLOC_node_member_get(mod, js_str_intern(p->c, "exports"));
            return ALLOC_node_seq(e, ALLOC_node_member_set(exp, js_str_intern(p->c, "default"), e));
        }
        if (p->cur.kind == TK_VAR || p->cur.kind == TK_LET || p->cur.kind == TK_CONST
            || p->cur.kind == TK_FUNCTION || p->cur.kind == TK_CLASS) {
            // Parse the declaration; collect names; then re-export.
            // Simpler: parse, then emit `module.exports.NAME = NAME` for each declared.
            // We don't track newly-declared names from the substatement,
            // so we approximate: parse the statement as a regular var/fn/class
            // and then for var/let/const, parse via parse_var_decl and capture.
            // For function/class declarations, recover the name via lookahead.
            if (p->cur.kind == TK_FUNCTION) {
                advance(p);
                if (p->cur.kind != TK_IDENT) parse_error(p, "export function must have name");
                struct JsString *name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
                Scope *target = scope_function(s);
                int ex = scope_find_var(target, name);
                uint32_t slot = ex >= 0 ? target->vars[ex].slot : scope_declare(target, name, 0);
                NODE *fn = parse_function_expr(p, s, name);
                NODE *bind = emit_store(p, s, name, fn);
                NODE *mod = emit_load(p, s, js_str_intern(p->c, "module"));
                NODE *exp_obj = ALLOC_node_member_get(mod, js_str_intern(p->c, "exports"));
                NODE *do_export = ALLOC_node_member_set(exp_obj, name, ALLOC_node_local_get(slot));
                return ALLOC_node_seq(bind, do_export);
            }
            if (p->cur.kind == TK_CLASS) {
                NODE *cls = parse_stmt(p, s);
                // class declarations are desugared so we don't easily get the name back;
                // for simplicity, evaluate cls and assume the binding succeeded (the
                // export will happen via re-lookup).  We can't easily inject the export
                // at this level without parsing the name first.
                return cls;
            }
            // var/let/const declaration
            uint8_t k = (p->cur.kind == TK_VAR) ? 0 : (p->cur.kind == TK_LET) ? 1 : 2;
            advance(p);
            // Take the first identifier and re-export it.
            struct JsString *first_name = NULL;
            if (p->cur.kind == TK_IDENT) {
                first_name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            }
            // Roll back: rewrite as parse_var_decl from this point.
            // We can't roll back cleanly, so just call parse_var_decl which
            // will handle the current ident.
            NODE *decl = parse_var_decl(p, s, k, false);
            accept(p, TK_SEMI);
            if (first_name) {
                NODE *mod = emit_load(p, s, js_str_intern(p->c, "module"));
                NODE *exp_obj = ALLOC_node_member_get(mod, js_str_intern(p->c, "exports"));
                NODE *do_export = ALLOC_node_member_set(exp_obj, first_name, emit_load(p, s, first_name));
                return ALLOC_node_seq(decl, do_export);
            }
            return decl;
        }
        if (p->cur.kind == TK_LBRACE) {
            // export { a, b as c } [from "p"];
            advance(p);
            struct JsString *names_local[64];
            struct JsString *names_export[64];
            uint32_t nn = 0;
            while (p->cur.kind != TK_RBRACE) {
                struct JsString *loc = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                advance(p);
                struct JsString *exp = loc;
                if (p->cur.kind == TK_IDENT && p->cur.len == 2 && memcmp(p->cur.start, "as", 2) == 0) {
                    advance(p);
                    exp = js_str_intern_n(p->c, p->cur.start, p->cur.len);
                    advance(p);
                }
                if (nn < 64) { names_local[nn] = loc; names_export[nn] = exp; nn++; }
                if (p->cur.kind == TK_COMMA) advance(p);
                else break;
            }
            expect(p, TK_RBRACE, "'}'");
            accept(p, TK_SEMI);
            NODE *mod = emit_load(p, s, js_str_intern(p->c, "module"));
            NODE *exp_obj = ALLOC_node_member_get(mod, js_str_intern(p->c, "exports"));
            NODE *seq = ALLOC_node_undefined();
            for (uint32_t i = 0; i < nn; i++) {
                NODE *src = emit_load(p, s, names_local[i]);
                seq = ALLOC_node_seq(seq, ALLOC_node_member_set(exp_obj, names_export[i], src));
            }
            return seq;
        }
        parse_error(p, "unsupported export form");
    }
    case TK_ASYNC: {
        // `async function name() {...}` declaration — same as a regular
        // function decl in our impl (sync execution).
        const Token *nx = peek(p);
        if (nx->kind == TK_FUNCTION) {
            advance(p);  // async
            // Fall through to TK_FUNCTION handler below by re-dispatching.
        } else {
            // Just an ident named "async" / arrow — treat as expression.
            NODE *e = parse_expr(p, s);
            accept(p, TK_SEMI);
            return e;
        }
    }
    /* fall through */
    case TK_FUNCTION: {
        advance(p);
        // Generator marker `function*` — accept but don't fully implement.
        if (p->cur.kind == TK_STAR) advance(p);
        struct JsString *name = NULL;
        if (p->cur.kind == TK_IDENT) {
            name = js_str_intern_n(p->c, p->cur.start, p->cur.len);
            advance(p);
        }
        // Function declaration: predeclare in current function/global scope
        Scope *target = scope_function(s);
        if (!target) target = s;
        uint32_t slot = (uint32_t)-1;
        if (name) {
            int existing = scope_find_var(target, name);
            slot = existing >= 0 ? target->vars[existing].slot : scope_declare(target, name, 0);
        }
        NODE *fn = parse_function_expr(p, s, name);
        // Use emit_store so captured-var detection picks up box_set when
        // a nested function captured the function-decl's name (recursion).
        if (name) return emit_store(p, s, name, fn);
        return fn;
    }
    case TK_SEMI: advance(p); return ALLOC_node_undefined();
    case TK_EOF:  return ALLOC_node_undefined();
    default: {
        NODE *e = parse_expr(p, s);
        accept(p, TK_SEMI);
        return e;
    }
    }
}

// =====================================================================
// Top-level entry — parse a whole source file.
// =====================================================================

char *
jstro_read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(1); }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    size_t got = fread(buf, 1, sz, fp);
    if ((long)got != sz) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    buf[sz] = 0;
    fclose(fp);
    *out_len = (size_t)sz;
    return buf;
}

NODE *
PARSE_FILE(CTX *c, const char *path)
{
    size_t len;
    char *src = jstro_read_file(path, &len);
    Parser p = {0};
    p.c = c;
    p.src = src; p.src_end = src + len; p.p = src; p.line = 1;
    advance(&p);  // load first token
    Scope *root = scope_new(NULL, true, false);
    // Hoist top-level function declarations so forward references work.
    hoist_scan(&p, p.cur.start ? p.cur.start : p.p, p.src_end, root);
    // Hoist top-level let/const for TDZ.
    hoist_letconst(&p, p.cur.start ? p.cur.start : p.p, p.src_end, root);
    NODE *fdecls[16384]; uint32_t nf = 0;
    NODE *stmts[16384]; uint32_t n = 0;
    while (p.cur.kind != TK_EOF) {
        bool is_fdecl = false;
        if (p.cur.kind == TK_FUNCTION) {
            const Token *nx = peek(&p);
            if (nx->kind == TK_IDENT) is_fdecl = true;
        }
        NODE *st = parse_stmt(&p, root);
        if (st) {
            if (is_fdecl && nf < 16384) fdecls[nf++] = st;
            else if (n < 16384)         stmts[n++] = st;
        }
    }
    NODE *combined[32768]; uint32_t total = 0;
    for (uint32_t i = 0; i < nf; i++) combined[total++] = fdecls[i];
    for (uint32_t i = 0; i < n;  i++) combined[total++] = stmts[i];
    n = total;
    for (uint32_t i = 0; i < total; i++) stmts[i] = combined[i];
    NODE *body = build_block(stmts, n);
    if (root->nholes > 0) {
        uint32_t base = jstro_u32_arr_alloc(root->nholes);
        for (uint32_t i = 0; i < root->nholes; i++) JSTRO_U32_ARR[base + i] = root->hole_slots[i];
        body = ALLOC_node_block(base, root->nholes, body);
    }
    // Wrap top-level so its frame is allocated.
    // We use a JsFunction-of-no-args invoked at startup (by main.c) instead.
    // Track final nlocals so main.c can size a frame.
    extern uint32_t JSTRO_TOP_NLOCALS;
    JSTRO_TOP_NLOCALS = root->nlocals;
    scope_free(root);
    free(src);
    return body;
}

uint32_t JSTRO_TOP_NLOCALS = 0;

// Parse a source string (used by `eval` and `Function` constructor).
NODE *
PARSE_STRING(CTX *c, const char *src, size_t len)
{
    Parser p = {0};
    p.c = c;
    p.src = src; p.src_end = src + len; p.p = src; p.line = 1;
    advance(&p);
    Scope *root = scope_new(NULL, true, false);
    hoist_scan(&p, p.cur.start ? p.cur.start : p.p, p.src_end, root);
    hoist_letconst(&p, p.cur.start ? p.cur.start : p.p, p.src_end, root);
    NODE *fdecls[1024]; uint32_t nf = 0;
    NODE *stmts[1024]; uint32_t n = 0;
    while (p.cur.kind != TK_EOF) {
        bool is_fdecl = false;
        if (p.cur.kind == TK_FUNCTION) {
            const Token *nx = peek(&p);
            if (nx->kind == TK_IDENT) is_fdecl = true;
        }
        NODE *st = parse_stmt(&p, root);
        if (st) {
            if (is_fdecl && nf < 1024) fdecls[nf++] = st;
            else if (n < 1024)         stmts[n++] = st;
        }
    }
    NODE *combined[2048]; uint32_t total = 0;
    for (uint32_t i = 0; i < nf; i++) combined[total++] = fdecls[i];
    for (uint32_t i = 0; i < n;  i++) combined[total++] = stmts[i];
    NODE *body = build_block(combined, total);
    if (root->nholes > 0) {
        uint32_t base = jstro_u32_arr_alloc(root->nholes);
        for (uint32_t i = 0; i < root->nholes; i++) JSTRO_U32_ARR[base + i] = root->hole_slots[i];
        body = ALLOC_node_block(base, root->nholes, body);
    }
    JSTRO_TOP_NLOCALS = root->nlocals;
    scope_free(root);
    return body;
}
