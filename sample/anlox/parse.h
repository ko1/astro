#ifndef ANLOX_PARSE_H
#define ANLOX_PARSE_H 1

#include "node.h"

enum tok_type {
    TK_EOF,
    TK_NUMBER, TK_STRING, TK_IDENT,
    // keywords
    TK_AND, TK_CLASS, TK_ELSE, TK_FALSE, TK_FOR, TK_FUN, TK_IF, TK_NIL,
    TK_OR, TK_PRINT, TK_RETURN, TK_SUPER, TK_THIS, TK_TRUE, TK_VAR, TK_WHILE,
    // punctuation
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_COMMA, TK_DOT, TK_SEMI,
    // operators
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH,
    TK_BANG, TK_BANG_EQ, TK_EQ, TK_EQ_EQ, TK_GT, TK_GE, TK_LT, TK_LE,
};

// Streaming lexer state — read directly by the parser.
extern const char *lox_src;
extern int     lox_src_pos;
extern int     lox_src_line;
extern int     lox_tok;
extern char    lox_tok_str[256];   // TK_IDENT / TK_STRING text
extern int     lox_tok_len;        // length of TK_STRING (may contain anything but NUL handling)
extern double  lox_tok_num;

void lox_init_lexer(const char *text);
void lox_next_token(void);

__attribute__((noreturn, format(printf, 1, 2)))
void lox_parse_fail(const char *fmt, ...);

typedef struct Program {
    NODE *body;     // node_stmts of the top-level program
    int   ok;
} Program;

Program lox_parse_program(const char *src);

#endif // ANLOX_PARSE_H
