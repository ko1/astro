// Lox tokenizer for anlox.
//
// Lox lexical syntax (Crafting Interpreters): numbers `[0-9]+("."[0-9]+)?`
// (no leading/trailing dot, no exponent), double-quoted strings with no
// escape sequences (newlines allowed inside), identifiers `[A-Za-z_]\w*`,
// the 16 keywords, and the single-/double-char operators.  Comments are
// `//` to end of line.
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "parse.h"

const char *lox_src;
int     lox_src_pos;
int     lox_src_line;
int     lox_tok;
char    lox_tok_str[256];
int     lox_tok_len;
double  lox_tok_num;

void
lox_init_lexer(const char *text)
{
    lox_src = text;
    lox_src_pos = 0;
    lox_src_line = 1;
    lox_tok = TK_EOF;
}

__attribute__((noreturn, format(printf, 1, 2)))
void
lox_parse_fail(const char *fmt, ...)
{
    fflush(stdout);
    fprintf(stderr, "[line %d] Error: ", lox_src_line);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(65);   // Lox uses exit code 65 for compile errors
}

static int cur(void)     { return (unsigned char)lox_src[lox_src_pos]; }
static int peek1(void)   { return (unsigned char)lox_src[lox_src_pos + 1]; }

static void
skip_ws(void)
{
    for (;;) {
        int ch = cur();
        if (ch == '\n') { lox_src_line++; lox_src_pos++; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\r') { lox_src_pos++; continue; }
        if (ch == '/' && peek1() == '/') {
            lox_src_pos += 2;
            while (cur() != '\n' && cur() != '\0') lox_src_pos++;
            continue;
        }
        break;
    }
}

static int is_idstart(int c) { return isalpha(c) || c == '_'; }
static int is_idcont(int c)  { return isalnum(c) || c == '_'; }

static int
keyword(const char *s)
{
    if (!strcmp(s, "and"))    return TK_AND;
    if (!strcmp(s, "class"))  return TK_CLASS;
    if (!strcmp(s, "else"))   return TK_ELSE;
    if (!strcmp(s, "false"))  return TK_FALSE;
    if (!strcmp(s, "for"))    return TK_FOR;
    if (!strcmp(s, "fun"))    return TK_FUN;
    if (!strcmp(s, "if"))     return TK_IF;
    if (!strcmp(s, "nil"))    return TK_NIL;
    if (!strcmp(s, "or"))     return TK_OR;
    if (!strcmp(s, "print"))  return TK_PRINT;
    if (!strcmp(s, "return")) return TK_RETURN;
    if (!strcmp(s, "super"))  return TK_SUPER;
    if (!strcmp(s, "this"))   return TK_THIS;
    if (!strcmp(s, "true"))   return TK_TRUE;
    if (!strcmp(s, "var"))    return TK_VAR;
    if (!strcmp(s, "while"))  return TK_WHILE;
    return TK_IDENT;
}

void
lox_next_token(void)
{
    skip_ws();
    int ch = cur();
    if (ch == '\0') { lox_tok = TK_EOF; return; }

    // number
    if (isdigit(ch)) {
        int start = lox_src_pos;
        while (isdigit(cur())) lox_src_pos++;
        if (cur() == '.' && isdigit(peek1())) {
            lox_src_pos++;
            while (isdigit(cur())) lox_src_pos++;
        }
        int len = lox_src_pos - start;
        char buf[64];
        if (len >= (int)sizeof(buf)) lox_parse_fail("number literal too long");
        memcpy(buf, lox_src + start, len); buf[len] = '\0';
        lox_tok = TK_NUMBER; lox_tok_num = strtod(buf, NULL);
        return;
    }

    // identifier / keyword
    if (is_idstart(ch)) {
        int start = lox_src_pos;
        while (is_idcont(cur())) lox_src_pos++;
        int len = lox_src_pos - start;
        if (len >= (int)sizeof(lox_tok_str)) lox_parse_fail("identifier too long");
        memcpy(lox_tok_str, lox_src + start, len); lox_tok_str[len] = '\0';
        lox_tok_len = len;
        lox_tok = keyword(lox_tok_str);
        return;
    }

    // string (no escapes; may span lines)
    if (ch == '"') {
        lox_src_pos++;
        int start = lox_src_pos;
        while (cur() != '"' && cur() != '\0') { if (cur() == '\n') lox_src_line++; lox_src_pos++; }
        if (cur() == '\0') lox_parse_fail("Unterminated string.");
        int len = lox_src_pos - start;
        if (len >= (int)sizeof(lox_tok_str)) lox_parse_fail("string literal too long");
        memcpy(lox_tok_str, lox_src + start, len); lox_tok_str[len] = '\0';
        lox_tok_len = len;
        lox_src_pos++;  // closing quote
        lox_tok = TK_STRING;
        return;
    }

    // operators / punctuation
    lox_src_pos++;
    switch (ch) {
      case '(': lox_tok = TK_LPAREN; return;
      case ')': lox_tok = TK_RPAREN; return;
      case '{': lox_tok = TK_LBRACE; return;
      case '}': lox_tok = TK_RBRACE; return;
      case ',': lox_tok = TK_COMMA;  return;
      case '.': lox_tok = TK_DOT;    return;
      case ';': lox_tok = TK_SEMI;   return;
      case '+': lox_tok = TK_PLUS;   return;
      case '-': lox_tok = TK_MINUS;  return;
      case '*': lox_tok = TK_STAR;   return;
      case '/': lox_tok = TK_SLASH;  return;
      case '!': if (cur() == '=') { lox_src_pos++; lox_tok = TK_BANG_EQ; } else lox_tok = TK_BANG; return;
      case '=': if (cur() == '=') { lox_src_pos++; lox_tok = TK_EQ_EQ; }  else lox_tok = TK_EQ;   return;
      case '>': if (cur() == '=') { lox_src_pos++; lox_tok = TK_GE; }     else lox_tok = TK_GT;   return;
      case '<': if (cur() == '=') { lox_src_pos++; lox_tok = TK_LE; }     else lox_tok = TK_LT;   return;
      default:
        lox_parse_fail("Unexpected character.");
    }
}
