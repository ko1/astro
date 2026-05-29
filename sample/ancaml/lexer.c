// MinCaml tokenizer for ancaml.
//
// MinCaml lexical syntax: ints (`digit+`), floats (`digit+('.'digit*)?
// (['e''E']['+''-']?digit+)?` — anything with a '.' or exponent),
// lowercase-ish identifiers, the keywords let/rec/in/if/then/else/
// true/false/not, the special token `Array.create` / `Array.make`, and
// the operators `+ - +. -. *. /. = < > <= >= <> ( ) , ; . <-`.  Comments
// are `(* ... *)` and nest.
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "parse.h"

const char *ac_src;
int     ac_src_pos;
int     ac_src_line;
int     ac_tok;
char    ac_tok_str[256];
int64_t ac_tok_int;
double  ac_tok_dbl;

void
ac_init_lexer(const char *text)
{
    ac_src = text;
    ac_src_pos = 0;
    ac_src_line = 1;
    ac_tok = TK_EOF;
}

__attribute__((noreturn, format(printf, 1, 2)))
void
ac_parse_fail(const char *fmt, ...)
{
    fflush(stdout);
    fprintf(stderr, "ancaml: parse error (line %d): ", ac_src_line);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int peek(int off) { return (unsigned char)ac_src[ac_src_pos + off]; }
static int cur(void)     { return (unsigned char)ac_src[ac_src_pos]; }

static void
skip_ws_and_comments(void)
{
    for (;;) {
        int ch = cur();
        if (ch == '\n') { ac_src_line++; ac_src_pos++; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\r') { ac_src_pos++; continue; }
        if (ch == '(' && peek(1) == '*') {
            // nested block comment
            int depth = 1;
            ac_src_pos += 2;
            while (depth > 0 && cur() != '\0') {
                if (cur() == '(' && peek(1) == '*') { depth++; ac_src_pos += 2; }
                else if (cur() == '*' && peek(1) == ')') { depth--; ac_src_pos += 2; }
                else { if (cur() == '\n') ac_src_line++; ac_src_pos++; }
            }
            continue;
        }
        break;
    }
}

static int is_idstart(int c) { return isalpha(c) || c == '_'; }
static int is_idcont(int c)  { return isalnum(c) || c == '_' || c == '\''; }

static int
keyword(const char *s)
{
    if (!strcmp(s, "let"))   return TK_LET;
    if (!strcmp(s, "rec"))   return TK_REC;
    if (!strcmp(s, "in"))    return TK_IN;
    if (!strcmp(s, "if"))    return TK_IF;
    if (!strcmp(s, "then"))  return TK_THEN;
    if (!strcmp(s, "else"))  return TK_ELSE;
    if (!strcmp(s, "true"))  return TK_TRUE;
    if (!strcmp(s, "false")) return TK_FALSE;
    if (!strcmp(s, "not"))   return TK_NOT;
    return TK_IDENT;
}

void
ac_next_token(void)
{
    skip_ws_and_comments();
    int ch = cur();

    if (ch == '\0') { ac_tok = TK_EOF; return; }

    // numbers
    if (isdigit(ch)) {
        int start = ac_src_pos;
        while (isdigit(cur())) ac_src_pos++;
        bool is_float = false;
        if (cur() == '.') {                       // fractional part
            is_float = true;
            ac_src_pos++;
            while (isdigit(cur())) ac_src_pos++;
        }
        if (cur() == 'e' || cur() == 'E') {        // exponent
            int save = ac_src_pos;
            ac_src_pos++;
            if (cur() == '+' || cur() == '-') ac_src_pos++;
            if (isdigit(cur())) { is_float = true; while (isdigit(cur())) ac_src_pos++; }
            else ac_src_pos = save;               // not an exponent after all
        }
        int len = ac_src_pos - start;
        char buf[256];
        if (len >= (int)sizeof(buf)) ac_parse_fail("numeric literal too long");
        memcpy(buf, ac_src + start, len); buf[len] = '\0';
        if (is_float) { ac_tok = TK_FLOAT; ac_tok_dbl = strtod(buf, NULL); }
        else          { ac_tok = TK_INT;   ac_tok_int = strtoll(buf, NULL, 10); }
        return;
    }

    // identifiers / keywords / Array.create|make
    if (is_idstart(ch)) {
        int start = ac_src_pos;
        while (is_idcont(cur())) ac_src_pos++;
        int len = ac_src_pos - start;
        if (len >= (int)sizeof(ac_tok_str)) ac_parse_fail("identifier too long");
        memcpy(ac_tok_str, ac_src + start, len); ac_tok_str[len] = '\0';

        if (!strcmp(ac_tok_str, "Array")) {
            if (!strncmp(ac_src + ac_src_pos, ".create", 7)) { ac_src_pos += 7; ac_tok = TK_ARRAY_MAKE; return; }
            if (!strncmp(ac_src + ac_src_pos, ".make",   5)) { ac_src_pos += 5; ac_tok = TK_ARRAY_MAKE; return; }
        }
        ac_tok = keyword(ac_tok_str);
        return;
    }

    // operators / punctuation
    ac_src_pos++;
    switch (ch) {
      case '+': if (cur() == '.') { ac_src_pos++; ac_tok = TK_FPLUS; } else ac_tok = TK_PLUS; return;
      case '-': if (cur() == '.') { ac_src_pos++; ac_tok = TK_FMINUS; } else ac_tok = TK_MINUS; return;
      case '*': if (cur() == '.') { ac_src_pos++; ac_tok = TK_FSTAR; return; }
                ac_parse_fail("unexpected '*' (MinCaml has no integer multiplication; use '*.' on floats)");
      case '/': if (cur() == '.') { ac_src_pos++; ac_tok = TK_FSLASH; return; }
                ac_parse_fail("unexpected '/' (MinCaml has no integer division; use '/.' on floats)");
      case '=': ac_tok = TK_EQ; return;
      case '<':
        if (cur() == '=') { ac_src_pos++; ac_tok = TK_LE; return; }
        if (cur() == '>') { ac_src_pos++; ac_tok = TK_NEQ; return; }
        if (cur() == '-') { ac_src_pos++; ac_tok = TK_LARROW; return; }
        ac_tok = TK_LT; return;
      case '>': if (cur() == '=') { ac_src_pos++; ac_tok = TK_GE; return; } ac_tok = TK_GT; return;
      case '(': ac_tok = TK_LPAREN; return;
      case ')': ac_tok = TK_RPAREN; return;
      case ',': ac_tok = TK_COMMA;  return;
      case ';': ac_tok = TK_SEMI;   return;
      case '.': ac_tok = TK_DOT;    return;
      default:
        ac_parse_fail("unexpected character '%c' (0x%02x)", ch, ch);
    }
}
