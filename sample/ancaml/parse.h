#ifndef ANCAML_PARSE_H
#define ANCAML_PARSE_H 1

#include "node.h"

// Token kinds for the MinCaml lexer (lexer.c) and parser (parse.c).
enum tok_type {
    TK_EOF,
    TK_INT, TK_FLOAT, TK_IDENT,
    TK_TRUE, TK_FALSE,
    TK_LET, TK_REC, TK_IN, TK_IF, TK_THEN, TK_ELSE, TK_NOT,
    TK_ARRAY_MAKE,                 // `Array.create` / `Array.make`
    TK_PLUS, TK_MINUS,             // int  + -
    TK_FPLUS, TK_FMINUS, TK_FSTAR, TK_FSLASH,   // float  +. -. *. /.
    TK_EQ, TK_LT, TK_GT, TK_LE, TK_GE, TK_NEQ,  // = < > <= >= <>
    TK_LPAREN, TK_RPAREN, TK_COMMA, TK_SEMI, TK_DOT, TK_LARROW,  // ( ) , ; . <-
};

// Streaming lexer state — read directly by the parser.
extern const char *ac_src;
extern int     ac_src_pos;
extern int     ac_src_line;
extern int     ac_tok;
extern char    ac_tok_str[256];
extern int64_t ac_tok_int;
extern double  ac_tok_dbl;

void ac_init_lexer(const char *text);
void ac_next_token(void);

__attribute__((noreturn, format(printf, 1, 2)))
void ac_parse_fail(const char *fmt, ...);

// The parsed program: a single top-level expression.
typedef struct Program {
    NODE *body;
    int   ok;        // 0 on syntax error
} Program;

Program ac_parse_program(const char *src);

#endif // ANCAML_PARSE_H
