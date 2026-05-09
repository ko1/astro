#ifndef PASCALAST_PARSE_H
#define PASCALAST_PARSE_H 1

#include <stdint.h>
#include <stdbool.h>

/* Token kinds for the Pascal subset.  Shared with the parser proper
 * in main.c via `tk` (current token) and `next_token` / `expect` /
 * `accept` / `init_lexer` below. */
enum {
    TK_EOF = 0,
    TK_INT, TK_RNUM, TK_ID, TK_STR,
    TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK,
    TK_SEMI, TK_COMMA, TK_COLON, TK_DOT, TK_DOTDOT, TK_ASSIGN,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    /* keywords */
    TK_PROGRAM, TK_VAR, TK_BEGIN, TK_END, TK_PROCEDURE, TK_FUNCTION,
    TK_IF, TK_THEN, TK_ELSE, TK_WHILE, TK_DO, TK_FOR, TK_TO, TK_DOWNTO,
    TK_REPEAT, TK_UNTIL, TK_INTEGER, TK_BOOLEAN, TK_REAL, TK_ARRAY, TK_OF,
    TK_TRUE, TK_FALSE, TK_AND, TK_OR, TK_NOT, TK_DIV, TK_MOD,
    TK_CONST, TK_NIL,
    TK_CASE, TK_FORWARD,
    TK_BREAK, TK_CONTINUE, TK_EXIT, TK_TYPE,
    TK_RECORD, TK_WITH,
    TK_SET, TK_IN,
    TK_STRING,
    TK_HAT,
    TK_AT,
    TK_TEXT, TK_FILE,
    TK_TRY, TK_EXCEPT, TK_FINALLY, TK_RAISE,
    TK_PACKED, TK_GOTO, TK_LABEL,
    TK_UNIT, TK_USES, TK_INTERFACE, TK_IMPLEMENTATION,
    TK_CLASS, TK_CONSTRUCTOR, TK_DESTRUCTOR, TK_VIRTUAL, TK_OVERRIDE, TK_INHERITED, TK_SELF,
    TK_PROPERTY, TK_PRIVATE, TK_PUBLIC, TK_PROTECTED, TK_PUBLISHED,
    TK_IS, TK_AS, TK_ABSTRACT,
};

/* Lexer state.  Read directly by the parser proper for speculative
 * lookahead and unit-file save/restore (parse_unit_file in main.c). */
extern const char *src;
extern int line_no;
extern int tk;
extern int64_t tk_int;
extern double  tk_real;
extern char tk_id[256];
extern char tk_str[1024];

/* `{$R+}` / `{$R-}` directive state — read by emit_assign in main.c
 * to decide whether to wrap a write in node_range_check. */
extern bool range_check_enabled;

/* Lexer entries. */
void init_lexer(const char *text);
void next_token(void);
void expect(int t, const char *what);
bool accept(int t);
const char *tk_name(int t);

#endif /* PASCALAST_PARSE_H */
