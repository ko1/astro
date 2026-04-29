#ifndef LUASTRO_LUA_TOKEN_H
#define LUASTRO_LUA_TOKEN_H

// Shared between tokenizer and parser.

typedef enum {
    LT_EOF = 0,

    // Punctuation
    LT_LPAREN, LT_RPAREN,
    LT_LBRACE, LT_RBRACE,
    LT_LBRACK, LT_RBRACK,
    LT_COMMA,  LT_SEMI,
    LT_DOT,    LT_DOTDOT,    LT_ELLIPSIS,
    LT_COLON,  LT_DBLCOLON,
    LT_ASSIGN,
    LT_EQ, LT_NEQ, LT_LT, LT_LE, LT_GT, LT_GE,
    LT_PLUS, LT_MINUS, LT_STAR, LT_SLASH, LT_DSLASH, LT_PERCENT, LT_CARET,
    LT_HASH, LT_AMP, LT_PIPE, LT_TILDE, LT_LSHIFT, LT_RSHIFT,

    // Keywords
    LT_AND, LT_BREAK, LT_DO, LT_ELSE, LT_ELSEIF, LT_END, LT_FALSE, LT_FOR,
    LT_FUNCTION, LT_GOTO, LT_IF, LT_IN, LT_LOCAL, LT_NIL, LT_NOT, LT_OR,
    LT_REPEAT, LT_RETURN, LT_THEN, LT_TRUE, LT_UNTIL, LT_WHILE,

    // Literals
    LT_INT, LT_FLOAT, LT_STRING, LT_IDENT,
} ltok_t;

typedef struct Token {
    ltok_t  kind;
    const char *start;
    size_t      len;

    int64_t int_value;
    double  float_value;

    char   *str_value;
    size_t  str_len;

    int line;
    int col;
} Token;

void           lua_tok_init(const char *src, const char *filename);
void           lua_tok_next(void);
const Token   *lua_tok_cur(void);
__attribute__((noreturn)) void lua_tok_error(const char *msg);

#endif
