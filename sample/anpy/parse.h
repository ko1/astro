#ifndef PARSE_H
#define PARSE_H 1

#include "node.h"
#include "check.h"

enum tok_type {
    TK_EOF, TK_NEWLINE, TK_INDENT, TK_DEDENT,
    TK_NAME, TK_INT, TK_STRING,
    TK_PLUS, TK_MINUS, TK_STAR, TK_FSLASH /* // */, TK_PCT,
    TK_LT, TK_GT, TK_LE, TK_GE, TK_EQEQ, TK_NE, TK_ASSIGN,
    TK_LP, TK_RP, TK_LB, TK_RB, TK_COMMA, TK_COLON, TK_DOT, TK_ARROW,
};

typedef struct {
    enum tok_type type;
    char *text;        // TK_NAME / TK_STRING
    long  ival;        // TK_INT
    int   is_idstr;    // TK_STRING whose content is a valid identifier
    int   line;
} Token;

// Tokenize `src` into a NUL(EOF)-terminated array.  Returns NULL and sets
// *errline on a lexical error.
Token *anpy_tokenize(const char *src, int *ntok, int *errline);

// The whole parsed program (for the type checker + global install).
typedef struct Program {
    struct var_decl *vars;  int nvars;     // top-level global variable defs
    anpy_func     **funcs;  int nfuncs;    // top-level function defs
    anpy_class    **classes; int nclasses; // class defs (in definition order)
    NODE           *body;                  // top-level statement sequence
    int             ok;                    // 0 on syntax error
} Program;

Program parse_program(const char *src);

#endif // PARSE_H
