// Indentation-aware tokenizer for AnPy (ChocoPy §3).
//
// Emits NEWLINE / INDENT / DEDENT per the Python-style indentation stack.
// Newlines inside ( ) and [ ] are insignificant (implicit line joining,
// matching Python).  Blank lines and comments produce no tokens.
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "parse.h"

typedef struct {
    Token *toks; int n, cap;
    int indent[256]; int isp;     // indentation stack
    int depth;                    // ( [ nesting
    int err;
} Lex;

static void
emit(Lex *L, enum tok_type t, char *text, long ival, int line)
{
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 256; L->toks = realloc(L->toks, sizeof(Token) * L->cap); }
    Token *k = &L->toks[L->n++];
    k->type = t; k->text = text; k->ival = ival; k->is_idstr = 0; k->line = line;
}

static char *dupn(const char *s, size_t n) { char *r = malloc(n + 1); memcpy(r, s, n); r[n] = 0; return r; }

static int is_idstart(int c) { return isalpha(c) || c == '_'; }
static int is_idcont(int c)  { return isalnum(c) || c == '_'; }

Token *
anpy_tokenize(const char *src, int *ntok, int *errline)
{
    Lex L; memset(&L, 0, sizeof(L));
    L.indent[L.isp = 0] = 0;
    const char *s = src;
    int line = 1;
    int at_line_start = 1;
    int line_has_token = 0;

    while (*s) {
        if (at_line_start && L.depth == 0) {
            // measure indentation
            int col = 0; const char *p = s;
            for (;;) {
                if (*p == ' ') { col++; p++; }
                else if (*p == '\t') { col += 8 - (col % 8); p++; }
                else break;
            }
            // blank line / comment-only line?
            if (*p == '\n' || *p == '\r' || *p == '#' || *p == '\0') {
                while (*p && *p != '\n') p++;       // skip comment to EOL
                if (*p == '\n') { p++; line++; }
                s = p; continue;
            }
            s = p;
            if (col > L.indent[L.isp]) { L.indent[++L.isp] = col; emit(&L, TK_INDENT, 0, 0, line); }
            else while (col < L.indent[L.isp]) { L.isp--; emit(&L, TK_DEDENT, 0, 0, line); }
            at_line_start = 0;
            line_has_token = 0;
        }

        char ch = *s;
        if (ch == '#') { while (*s && *s != '\n') s++; continue; }
        if (ch == '\n') {
            line++;
            if (L.depth == 0) {
                if (line_has_token) emit(&L, TK_NEWLINE, 0, 0, line - 1);
                at_line_start = 1;
            }
            s++; continue;
        }
        if (ch == '\r' || ch == ' ' || ch == '\t') { s++; continue; }

        // identifiers / keywords
        if (is_idstart((unsigned char)ch)) {
            const char *start = s; while (is_idcont((unsigned char)*s)) s++;
            emit(&L, TK_NAME, dupn(start, s - start), 0, line); line_has_token = 1; continue;
        }
        // integer literals (no leading zeros except "0")
        if (isdigit((unsigned char)ch)) {
            const char *start = s; while (isdigit((unsigned char)*s)) s++;
            char *t = dupn(start, s - start);
            int over = 0; long v = 0;
            for (const char *q = t; *q; q++) { v = v * 10 + (*q - '0'); if (v > 2147483647L) over = 1; }
            free(t);
            if (over) { L.err = line; break; }
            emit(&L, TK_INT, 0, v, line); line_has_token = 1; continue;
        }
        // string literals
        if (ch == '"') {
            s++; char buf[8192]; int bi = 0; int ok = 1;
            while (*s && *s != '"') {
                unsigned char e = (unsigned char)*s;
                if (e == '\\') {
                    char nx = s[1];
                    if (nx == '"') e = '"'; else if (nx == '\\') e = '\\';
                    else if (nx == 't') e = '\t'; else if (nx == 'n') e = '\n';
                    else { ok = 0; break; }
                    s += 2;
                }
                else if (e < 32 || e > 126) { ok = 0; break; }
                else s++;
                if (bi < (int)sizeof(buf) - 1) buf[bi++] = (char)e;
            }
            if (!ok || *s != '"') { L.err = line; break; }
            s++; buf[bi] = 0;
            char *text = dupn(buf, bi);
            int idstr = bi > 0 && is_idstart((unsigned char)buf[0]);
            for (int i = 1; idstr && i < bi; i++) if (!is_idcont((unsigned char)buf[i])) idstr = 0;
            emit(&L, TK_STRING, text, 0, line); L.toks[L.n - 1].is_idstr = idstr; line_has_token = 1; continue;
        }
        // operators / delimiters (maximal munch)
        switch (ch) {
          case '+': emit(&L,TK_PLUS,0,0,line); s++; break;
          case '-': if (s[1]=='>'){emit(&L,TK_ARROW,0,0,line);s+=2;} else {emit(&L,TK_MINUS,0,0,line);s++;} break;
          case '*': emit(&L,TK_STAR,0,0,line); s++; break;
          case '/': if (s[1]=='/'){emit(&L,TK_FSLASH,0,0,line);s+=2;} else {L.err=line;} break;
          case '%': emit(&L,TK_PCT,0,0,line); s++; break;
          case '<': if (s[1]=='='){emit(&L,TK_LE,0,0,line);s+=2;} else {emit(&L,TK_LT,0,0,line);s++;} break;
          case '>': if (s[1]=='='){emit(&L,TK_GE,0,0,line);s+=2;} else {emit(&L,TK_GT,0,0,line);s++;} break;
          case '=': if (s[1]=='='){emit(&L,TK_EQEQ,0,0,line);s+=2;} else {emit(&L,TK_ASSIGN,0,0,line);s++;} break;
          case '!': if (s[1]=='='){emit(&L,TK_NE,0,0,line);s+=2;} else {L.err=line;} break;
          case '(': emit(&L,TK_LP,0,0,line); L.depth++; s++; break;
          case ')': emit(&L,TK_RP,0,0,line); if(L.depth>0)L.depth--; s++; break;
          case '[': emit(&L,TK_LB,0,0,line); L.depth++; s++; break;
          case ']': emit(&L,TK_RB,0,0,line); if(L.depth>0)L.depth--; s++; break;
          case ',': emit(&L,TK_COMMA,0,0,line); s++; break;
          case ':': emit(&L,TK_COLON,0,0,line); s++; break;
          case '.': emit(&L,TK_DOT,0,0,line); s++; break;
          default: L.err = line; break;
        }
        if (L.err) break;
        line_has_token = 1;
    }

    if (L.err) { if (errline) *errline = L.err; free(L.toks); return NULL; }

    if (line_has_token && L.n > 0 && L.toks[L.n - 1].type != TK_NEWLINE) emit(&L, TK_NEWLINE, 0, 0, line);
    while (L.isp > 0) { L.isp--; emit(&L, TK_DEDENT, 0, 0, line); }
    emit(&L, TK_EOF, 0, 0, line);

    *ntok = L.n;
    return L.toks;
}
