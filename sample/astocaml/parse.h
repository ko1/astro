#ifndef ASTOCAML_PARSE_H
#define ASTOCAML_PARSE_H 1

#include <stdint.h>
#include <stdbool.h>

/* Token kinds.  Shared between lexer (parse.c) and parser (main.c).
 * Adding / reordering values requires updating tok_op_name in main.c. */
enum tok {
    TK_EOF,
    TK_INT, TK_FLOAT_TOK, TK_IDENT, TK_STRING, TK_CHAR,
    TK_LET, TK_REC, TK_IN, TK_IF, TK_THEN, TK_ELSE, TK_FUN, TK_FUNCTION,
    TK_MATCH, TK_WITH, TK_TRY, TK_TRUE, TK_FALSE, TK_MOD, TK_NOT,
    TK_BEGIN, TK_END, TK_KW_AND, TK_OR_KW, TK_WHEN, TK_AS, TK_REF_KW,
    TK_TYPE, TK_OF, TK_EXCEPTION_KW, TK_OPEN, TK_MODULE, TK_INCLUDE,
    TK_SIG, TK_STRUCT, TK_DO, TK_DONE, TK_FOR, TK_TO, TK_DOWNTO,
    TK_WHILE, TK_LSL, TK_LSR, TK_ASR, TK_LAND_KW, TK_LOR_KW, TK_LXOR_KW,
    TK_LAZY, TK_CLASS, TK_OBJECT, TK_METHOD, TK_VAL, TK_INHERIT,
    TK_PRIVATE, TK_MUTABLE, TK_NEW, TK_INITIALIZER, TK_FUNCTOR,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH,
    TK_FPLUS, TK_FMINUS, TK_FSTAR, TK_FSLASH,
    TK_LT, TK_GT, TK_LE, TK_GE, TK_EQ, TK_NE, TK_PEQ, TK_PNE,
    TK_ARROW, TK_BAR, TK_SEMI, TK_DSEMI, TK_CONS,
    TK_AMPAMP, TK_PIPEPIPE, TK_CONCAT, TK_UNDER, TK_COMMA,
    TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK, TK_LBRACE, TK_RBRACE,
    TK_LBRACKBAR, TK_BARRBRACK,
    TK_DOT, TK_DOTDOT, TK_BANG, TK_ASSIGN, TK_TILDE, TK_QMARK,
    TK_COLON, TK_HASH,
};

/* Lexer state.  Read directly by the parser proper for save/restore /
 * speculative lookahead — kept extern so parser-side helpers can stash
 * (src_pos, src_line, tok) without going through accessors. */
extern const char *src;
extern int   src_pos;
extern int   src_line;
extern int      tok;
extern char     tok_str[1024];
extern int64_t  tok_int;
extern double   tok_dbl;

/* Lexer entries. */
void init_lexer(const char *text);
void next_token(void);
void skip_ws_and_comments(void);
void expect(int t, const char *what);

/* Reports the current source line and exits.  Used by lexer and parser. */
__attribute__((noreturn,format(printf,1,2)))
void parse_fail(const char *fmt, ...);

/* True iff `c` is one of the OCaml infix-operator characters
 * (`!$%&*+-/.:<=>?@^|~`).  Defined alongside the lexer because the
 * tokenizer uses it; the parser also calls it when scanning custom
 * operator names inside `(...)`. */
bool is_op_char(char c);

#endif /* ASTOCAML_PARSE_H */
