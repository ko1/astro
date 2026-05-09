// astocaml — lexer.
//
// Source-text → token stream.  Plain mutable state (`src` / `src_pos` /
// `src_line` / `tok` / `tok_str` / `tok_int` / `tok_dbl`) shared with
// the parser proper in main.c via parse.h, since the parser is a
// recursive-descent reader that drives the lexer one token at a time
// and occasionally save/restores it for speculative lookahead.

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "parse.h"

const char *src;
int   src_pos;
int   src_line;

int      tok;
char     tok_str[1024];
int64_t  tok_int;
double   tok_dbl;

void
parse_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "astocaml: parse error at line %d: ", src_line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

void
skip_ws_and_comments(void)
{
    for (;;) {
        while (isspace((unsigned char)src[src_pos])) {
            if (src[src_pos] == '\n') src_line++;
            src_pos++;
        }
        if (src[src_pos] == '(' && src[src_pos+1] == '*') {
            src_pos += 2;
            int depth = 1;
            while (depth > 0 && src[src_pos]) {
                if (src[src_pos] == '(' && src[src_pos+1] == '*') { depth++; src_pos += 2; }
                else if (src[src_pos] == '*' && src[src_pos+1] == ')') { depth--; src_pos += 2; }
                else { if (src[src_pos] == '\n') src_line++; src_pos++; }
            }
            continue;
        }
        break;
    }
}

// OCaml operator characters.  When a sequence of these appears between
// `(` and `)` (with no whitespace inside), it forms a custom infix-op
// identifier — `(+!)`, `(<*>)`, `(:=)` etc.
bool
is_op_char(char c)
{
    return c && strchr("!$%&*+-/.:<=>?@^|~", c) != NULL;
}

void
next_token(void)
{
    skip_ws_and_comments();
    char ch = src[src_pos];
    if (ch == '\0') { tok = TK_EOF; return; }

    // String literal.
    if (ch == '"') {
        src_pos++;
        int idx = 0;
        while (src[src_pos] && src[src_pos] != '"') {
            char c = src[src_pos++];
            if (c == '\\') {
                char e = src[src_pos++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                case '\'': c = '\''; break;
                case '0': c = '\0'; break;
                default: c = e; break;
                }
            }
            if (idx < (int)sizeof(tok_str) - 1) tok_str[idx++] = c;
        }
        tok_str[idx] = '\0';
        if (src[src_pos] == '"') src_pos++;
        tok = TK_STRING;
        return;
    }

    // Polymorphic variant tag: `\`Foo`, `\`bar`.  We emit TK_IDENT with
    // the backtick prefixed in-place so the parser can detect via
    // is_uppercase_ident().
    if (ch == '`') {
        src_pos++;
        int idx = 0;
        tok_str[idx++] = '`';
        while (isalnum((unsigned char)src[src_pos]) || src[src_pos] == '_' || src[src_pos] == '\'') {
            if (idx < (int)sizeof(tok_str) - 1) tok_str[idx++] = src[src_pos];
            src_pos++;
        }
        tok_str[idx] = '\0';
        tok = TK_IDENT;
        return;
    }

    // Type variable: `'a`, `'b`, etc. (only inside type declarations,
    // which we skip).  Detected as `'` followed by a lowercase letter and
    // NOT a char-literal pattern.  Yield a TK_IDENT carrying the bare
    // name without the apostrophe.
    if (ch == '\'' && isalpha((unsigned char)src[src_pos+1]) && islower((unsigned char)src[src_pos+1])) {
        // Check it isn't a char literal: char literal has closing `'` at
        // pos+2 (or pos+3 for escapes).  Type vars don't.
        bool is_char_lit = (src[src_pos+2] == '\'') ||
                           (src[src_pos+1] == '\\' && src[src_pos+3] == '\'');
        if (!is_char_lit) {
            src_pos++;       // consume `'`
            int idx = 0;
            while (isalnum((unsigned char)src[src_pos]) || src[src_pos] == '_' || src[src_pos] == '\'') {
                if (idx < (int)sizeof(tok_str) - 1) tok_str[idx++] = src[src_pos];
                src_pos++;
            }
            tok_str[idx] = '\0';
            tok = TK_IDENT;
            return;
        }
    }

    // Char literal: 'a' or '\n' or '\\' etc.  In OCaml chars are int8.
    if (ch == '\'' && src[src_pos+1] && src[src_pos+1] != '\'' &&
        ((src[src_pos+1] == '\\' && src[src_pos+3] == '\'') ||
         src[src_pos+2] == '\'')) {
        src_pos++;
        char c;
        if (src[src_pos] == '\\') {
            src_pos++;
            switch (src[src_pos++]) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '"': c = '"'; break;
            case '0': c = '\0'; break;
            default: c = src[src_pos-1]; break;
            }
        }
        else {
            c = src[src_pos++];
        }
        if (src[src_pos] == '\'') src_pos++;
        tok_int = (unsigned char)c;
        tok = TK_CHAR;
        return;
    }

    // Float literal starting with `.` (e.g. `.5`).  Distinct from `..`
    // range and from `.field` access.
    if (ch == '.' && isdigit((unsigned char)src[src_pos+1])) {
        int start = src_pos;
        src_pos++;
        while (isdigit((unsigned char)src[src_pos])) src_pos++;
        if (src[src_pos] == 'e' || src[src_pos] == 'E') {
            src_pos++;
            if (src[src_pos] == '+' || src[src_pos] == '-') src_pos++;
            while (isdigit((unsigned char)src[src_pos])) src_pos++;
        }
        char buf[64];
        int len = src_pos - start;
        if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, src + start, len);
        buf[len] = '\0';
        tok_dbl = strtod(buf, NULL);
        tok = TK_FLOAT_TOK;
        return;
    }

    // Numeric literal — int or float.
    if (isdigit((unsigned char)ch)) {
        int start = src_pos;
        while (isdigit((unsigned char)src[src_pos])) src_pos++;
        bool is_float = false;
        if (src[src_pos] == '.' && src[src_pos+1] != '.' /* not range op */) {
            is_float = true;
            src_pos++;
            while (isdigit((unsigned char)src[src_pos])) src_pos++;
        }
        if (src[src_pos] == 'e' || src[src_pos] == 'E') {
            is_float = true;
            src_pos++;
            if (src[src_pos] == '+' || src[src_pos] == '-') src_pos++;
            while (isdigit((unsigned char)src[src_pos])) src_pos++;
        }
        char buf[64];
        int len = src_pos - start;
        if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, src + start, len);
        buf[len] = '\0';
        if (is_float) {
            tok_dbl = strtod(buf, NULL);
            tok = TK_FLOAT_TOK;
        }
        else {
            tok_int = strtoll(buf, NULL, 10);
            tok = TK_INT;
        }
        return;
    }

    // Identifier / keyword.
    if (isalpha((unsigned char)ch) || ch == '_') {
        int idx = 0;
        while (isalnum((unsigned char)src[src_pos]) || src[src_pos] == '_' || src[src_pos] == '\'') {
            if (idx < (int)sizeof(tok_str) - 1) tok_str[idx++] = src[src_pos];
            src_pos++;
        }
        tok_str[idx] = '\0';
        if (idx == 1 && tok_str[0] == '_') { tok = TK_UNDER; return; }
        struct kw { const char *s; int t; };
        static const struct kw kws[] = {
            {"let", TK_LET},   {"rec", TK_REC},     {"in", TK_IN},
            {"if", TK_IF},     {"then", TK_THEN},   {"else", TK_ELSE},
            {"fun", TK_FUN},   {"function", TK_FUNCTION},
            {"match", TK_MATCH}, {"with", TK_WITH}, {"try", TK_TRY},
            {"true", TK_TRUE}, {"false", TK_FALSE},
            {"mod", TK_MOD},   {"not", TK_NOT},
            {"begin", TK_BEGIN}, {"end", TK_END},
            {"and", TK_KW_AND},
            {"when", TK_WHEN}, {"as", TK_AS},
            {"type", TK_TYPE}, {"of", TK_OF},
            {"exception", TK_EXCEPTION_KW},
            {"open", TK_OPEN}, {"module", TK_MODULE}, {"include", TK_INCLUDE},
            {"sig", TK_SIG},   {"struct", TK_STRUCT},
            {"do", TK_DO},     {"done", TK_DONE},
            {"for", TK_FOR},   {"to", TK_TO},       {"downto", TK_DOWNTO},
            {"while", TK_WHILE},
            {"lsl", TK_LSL},   {"lsr", TK_LSR},     {"asr", TK_ASR},
            {"land", TK_LAND_KW}, {"lor", TK_LOR_KW}, {"lxor", TK_LXOR_KW},
            {"or", TK_OR_KW},
            {"lazy", TK_LAZY},
            {"class", TK_CLASS}, {"object", TK_OBJECT}, {"method", TK_METHOD},
            {"val", TK_VAL}, {"inherit", TK_INHERIT}, {"private", TK_PRIVATE},
            {"mutable", TK_MUTABLE}, {"new", TK_NEW}, {"initializer", TK_INITIALIZER},
            {"functor", TK_FUNCTOR},
            {NULL, 0}
        };
        for (const struct kw *k = kws; k->s; k++) {
            if (!strcmp(tok_str, k->s)) { tok = k->t; return; }
        }
        tok = TK_IDENT;
        return;
    }

    // Operator-character sequences.  Read all consecutive op chars and
    // map known sequences to their dedicated tokens; anything else is a
    // custom infix operator emitted as TK_IDENT.
    if (is_op_char(ch)) {
        char obuf[64]; int idx = 0;
        // ch is at src[src_pos]; consume it and any continuation.
        while (is_op_char(src[src_pos]) && idx < 63) obuf[idx++] = src[src_pos++];
        obuf[idx] = '\0';
        // Special: `[]` was tokenized via the `[` case below; here we only
        // see `[` as standalone bracket (handled separately).
        // Map well-known operator strings to their tokens.
        if      (!strcmp(obuf, "+"))   { tok = TK_PLUS;     return; }
        else if (!strcmp(obuf, "-"))   { tok = TK_MINUS;    return; }
        else if (!strcmp(obuf, "*"))   { tok = TK_STAR;     return; }
        else if (!strcmp(obuf, "/"))   { tok = TK_SLASH;    return; }
        else if (!strcmp(obuf, "+."))  { tok = TK_FPLUS;    return; }
        else if (!strcmp(obuf, "-."))  { tok = TK_FMINUS;   return; }
        else if (!strcmp(obuf, "*."))  { tok = TK_FSTAR;    return; }
        else if (!strcmp(obuf, "/."))  { tok = TK_FSLASH;   return; }
        else if (!strcmp(obuf, "<"))   { tok = TK_LT;       return; }
        else if (!strcmp(obuf, ">"))   { tok = TK_GT;       return; }
        else if (!strcmp(obuf, "<="))  { tok = TK_LE;       return; }
        else if (!strcmp(obuf, ">="))  { tok = TK_GE;       return; }
        else if (!strcmp(obuf, "="))   { tok = TK_EQ;       return; }
        else if (!strcmp(obuf, "<>"))  { tok = TK_NE;       return; }
        else if (!strcmp(obuf, "=="))  { tok = TK_PEQ;      return; }
        else if (!strcmp(obuf, "!="))  { tok = TK_PNE;      return; }
        else if (!strcmp(obuf, "->"))  { tok = TK_ARROW;    return; }
        else if (!strcmp(obuf, "|") && src[src_pos] == ']') { src_pos++; tok = TK_BARRBRACK; return; }
        else if (!strcmp(obuf, "|"))   { tok = TK_BAR;      return; }
        else if (!strcmp(obuf, "||"))  { tok = TK_PIPEPIPE; return; }
        else if (!strcmp(obuf, "&&"))  { tok = TK_AMPAMP;   return; }
        else if (!strcmp(obuf, "::"))  { tok = TK_CONS;     return; }
        else if (!strcmp(obuf, ":="))  { tok = TK_ASSIGN;   return; }
        else if (!strcmp(obuf, "<-"))  { tok = TK_ASSIGN;   return; }
        else if (!strcmp(obuf, "!"))   { tok = TK_BANG;     return; }
        else if (!strcmp(obuf, "~"))   { tok = TK_TILDE;    return; }
        else if (!strcmp(obuf, "?"))   { tok = TK_QMARK;    return; }
        else if (!strcmp(obuf, ":"))   { tok = TK_COLON;    return; }
        else if (!strcmp(obuf, "."))   { tok = TK_DOT;      return; }
        else if (!strcmp(obuf, ".."))  { tok = TK_DOTDOT;   return; }
        else if (!strcmp(obuf, "^"))   { tok = TK_CONCAT;   return; }
        else if (!strcmp(obuf, "@"))   { tok = TK_CONCAT;   return; }   // simplified: '@' aliased to '^'
        // Custom operator — emit as identifier.
        strcpy(tok_str, obuf);
        tok = TK_IDENT;
        return;
    }

    src_pos++;
    switch (ch) {
    case ',': tok = TK_COMMA; return;
    case '(': tok = TK_LPAREN; return;
    case ')': tok = TK_RPAREN; return;
    case '{': tok = TK_LBRACE; return;
    case '}': tok = TK_RBRACE; return;
    case '[':
        if (src[src_pos] == ']') { src_pos++; tok_str[0] = '\0'; tok = TK_LBRACK; tok_int = 1; return; }
        if (src[src_pos] == '|') { src_pos++; tok = TK_LBRACKBAR; return; }
        tok = TK_LBRACK; tok_int = 0; return;
    case ']': tok = TK_RBRACK; return;
    case ';':
        if (src[src_pos] == ';') { src_pos++; tok = TK_DSEMI; return; }
        tok = TK_SEMI; return;
    case '#': tok = TK_HASH; return;
    case '&':
        // Stand-alone `&` (without other op chars following) — fall through
        // to old behavior: only valid as `&&` (which is_op_char captured).
        parse_fail("expected '&&'");
    }
    parse_fail("unexpected character '%c' (0x%02x)", ch, (unsigned char)ch);
}

void
expect(int t, const char *what)
{
    if (tok != t) parse_fail("expected %s (got tok=%d)", what, tok);
    next_token();
}

void
init_lexer(const char *text)
{
    src = text; src_pos = 0; src_line = 1;
    next_token();
}
