// arawk parser — lexer + recursive-descent / Pratt parser for a POSIX
// awk subset.  Produces a single `arawk_node_program(begin, main, end)` root.
//
// Phase 0+1 subset:
//   program  := item*
//   item     := 'BEGIN' block | 'END' block | block
//   block    := '{' stmt_list '}'
//   stmt     := print | if | while | next | exit [expr] | break | continue
//             | block | expr_stmt
//
// Pattern-action items with pattern expressions are accepted (Phase 1)
// but only one main action is recorded; multiple BEGIN / END blocks
// are concatenated.  Regex pattern matching lands in Phase 2.

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "context.h"
#include "node.h"

// ---------------------------------------------------------------------------
// ARAWK_NODE_TABLE — flat NODE pointer table for variadic-arity nodes
// (arawk_node_print).  Same convention as astr / pystro.
// ---------------------------------------------------------------------------

NODE     **ARAWK_NODE_TABLE     = NULL;
uint32_t   ARAWK_NODE_TABLE_LEN = 0;
static uint32_t arawk_node_table_capa = 0;

static uint32_t
arawk_node_table_push_n(NODE **items, uint32_t n)
{
    if (ARAWK_NODE_TABLE_LEN + n > arawk_node_table_capa) {
        uint32_t capa = arawk_node_table_capa ? arawk_node_table_capa * 2 : 16;
        while (capa < ARAWK_NODE_TABLE_LEN + n) capa *= 2;
        ARAWK_NODE_TABLE = (NODE **)realloc(ARAWK_NODE_TABLE, sizeof(NODE *) * capa);
        arawk_node_table_capa = capa;
    }
    uint32_t base = ARAWK_NODE_TABLE_LEN;
    for (uint32_t i = 0; i < n; i++) ARAWK_NODE_TABLE[base + i] = items[i];
    ARAWK_NODE_TABLE_LEN += n;
    return base;
}

// ---------------------------------------------------------------------------
// Tokenizer.
// ---------------------------------------------------------------------------

typedef enum {
    TK_EOF, TK_NL, TK_SEMI,
    TK_INT, TK_FLOAT, TK_NAME, TK_STRING,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK,
    TK_COMMA, TK_DOLLAR,
    TK_ASSIGN,                        // =
    TK_PLUS_ASSIGN, TK_MINUS_ASSIGN, TK_STAR_ASSIGN,
    TK_SLASH_ASSIGN, TK_PERCENT_ASSIGN, TK_CARET_ASSIGN,
    TK_INC, TK_DEC,                   // ++  --
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_CARET,
    TK_LT, TK_LE, TK_GT, TK_GE, TK_EQ, TK_NE,
    TK_NOT, TK_AND, TK_OR,
    TK_PIPE,                          // single `|` — print redirect
    TK_APPEND,                        // >> — append redirect
    TK_QUESTION, TK_COLON,            // ? :
    TK_KW_BEGIN, TK_KW_END,
    TK_KW_IF, TK_KW_ELSE, TK_KW_WHILE, TK_KW_FOR, TK_KW_IN,
    TK_KW_PRINT, TK_KW_PRINTF, TK_KW_DELETE,
    TK_KW_FUNCTION, TK_KW_RETURN,
    TK_KW_NEXT, TK_KW_EXIT, TK_KW_BREAK, TK_KW_CONTINUE,
    TK_KW_NR, TK_KW_NF,
    // Builtin function names (treated as keywords; user-defined
    // functions, Phase 1.8, will use plain TK_NAME with `(` peek).
    TK_KW_LENGTH, TK_KW_SPRINTF, TK_KW_SUBSTR, TK_KW_INDEX, TK_KW_SPLIT,
    TK_KW_TOLOWER, TK_KW_TOUPPER, TK_KW_INT_FN,
    TK_KW_SIN, TK_KW_COS, TK_KW_SQRT, TK_KW_EXP, TK_KW_LOG, TK_KW_ATAN2,
    TK_KW_RAND, TK_KW_SRAND,
    TK_KW_CLOSE, TK_KW_FFLUSH, TK_KW_SYSTEM, TK_KW_GETLINE,
} TokKind;

typedef struct {
    TokKind kind;
    const char *start;
    int len;
    int64_t  inum;
    double   fnum;
    char    *str;
    int      str_len;
    int      line;
} Token;

static const char *src;
static const char *p;
static int paren_depth;
static int line_no = 1;

static const struct { const char *kw; TokKind kind; } keywords[] = {
    { "BEGIN",    TK_KW_BEGIN },
    { "END",      TK_KW_END },
    { "if",       TK_KW_IF },
    { "else",     TK_KW_ELSE },
    { "while",    TK_KW_WHILE },
    { "for",      TK_KW_FOR },
    { "in",       TK_KW_IN },
    { "delete",   TK_KW_DELETE },
    { "function", TK_KW_FUNCTION },
    { "func",     TK_KW_FUNCTION },
    { "return",   TK_KW_RETURN },
    { "print",    TK_KW_PRINT },
    { "printf",   TK_KW_PRINTF },
    { "next",     TK_KW_NEXT },
    { "exit",     TK_KW_EXIT },
    { "break",    TK_KW_BREAK },
    { "continue", TK_KW_CONTINUE },
    { "length",   TK_KW_LENGTH },
    { "sprintf",  TK_KW_SPRINTF },
    { "substr",   TK_KW_SUBSTR },
    { "index",    TK_KW_INDEX },
    { "split",    TK_KW_SPLIT },
    { "tolower",  TK_KW_TOLOWER },
    { "toupper",  TK_KW_TOUPPER },
    { "int",      TK_KW_INT_FN },
    { "sin",      TK_KW_SIN },
    { "cos",      TK_KW_COS },
    { "sqrt",     TK_KW_SQRT },
    { "exp",      TK_KW_EXP },
    { "log",      TK_KW_LOG },
    { "atan2",    TK_KW_ATAN2 },
    { "rand",     TK_KW_RAND },
    { "srand",    TK_KW_SRAND },
    { "close",    TK_KW_CLOSE },
    { "fflush",   TK_KW_FFLUSH },
    { "system",   TK_KW_SYSTEM },
    { "getline",  TK_KW_GETLINE },
    // NR / NF intentionally NOT in the keyword table — they resolve
    // to the pre-reserved globals_intern slot via the TK_NAME path,
    // which makes `NF = 5` flow through the regular assignment
    // machinery (where arawk_node_gset has the AWK_GLOB_NF hook).
    { NULL, 0 }
};

static Token next_tok;
static bool have_lookahead = false;
// Pushback stack: storing already-lexed tokens so unread_tok works
// when peeking has advanced the source pointer past them.  Depth 2
// is enough for the `$ INT op=` and `NAME [` lookaheads we use.
#define PUSHBACK_MAX 4
static Token pushback_buf[PUSHBACK_MAX];
static int   pushback_cnt = 0;
// (Legacy single-slot variables are gone — the pushback stack
// subsumes them.)

static __attribute__((noreturn,format(printf,1,2))) void
parse_error(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "arawk: parse error on line %d: ", line_no);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static char *
intern_string(const char *s, int len)
{
    char *r = (char *)malloc((size_t)len + 1);
    memcpy(r, s, (size_t)len);
    r[len] = '\0';
    return r;
}

static void
skip_ws_and_comments(void)
{
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        // Line continuation: `\` immediately before NL eats the newline.
        if (*p == '\\' && p[1] == '\n') { p += 2; line_no++; continue; }
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }
        break;
    }
}

static Token
read_token(void)
{
    Token t = { 0 };
    skip_ws_and_comments();
    t.line = line_no;
    t.start = p;

    if (*p == '\0') { t.kind = TK_EOF; return t; }

    if (*p == '\n') {
        line_no++;
        p++;
        t.kind = (paren_depth > 0) ? read_token().kind : TK_NL;
        // Re-evaluate when in paren: continue reading.
        if (paren_depth > 0) return read_token();
        t.len = 1;
        return t;
    }

    // Numbers.
    if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
        const char *start = p;
        bool is_float = false;
        while (isdigit((unsigned char)*p)) p++;
        if (*p == '.') { is_float = true; p++; while (isdigit((unsigned char)*p)) p++; }
        if (*p == 'e' || *p == 'E') {
            is_float = true; p++;
            if (*p == '+' || *p == '-') p++;
            while (isdigit((unsigned char)*p)) p++;
        }
        t.len = (int)(p - start);
        if (is_float) {
            t.kind = TK_FLOAT;
            t.fnum = strtod(start, NULL);
        } else {
            t.kind = TK_INT;
            t.inum = strtoll(start, NULL, 10);
        }
        return t;
    }

    // Identifiers / keywords.
    if (isalpha((unsigned char)*p) || *p == '_') {
        const char *start = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        t.len = (int)(p - start);
        for (int i = 0; keywords[i].kw; i++) {
            if ((int)strlen(keywords[i].kw) == t.len &&
                memcmp(start, keywords[i].kw, t.len) == 0) {
                t.kind = keywords[i].kind;
                return t;
            }
        }
        t.kind = TK_NAME;
        return t;
    }

    // String literal.
    if (*p == '"') {
        p++;
        const char *start = p;
        char *buf = (char *)malloc(256);
        int buflen = 0, bufcapa = 256;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                char esc;
                switch (p[1]) {
                  case 'n':  esc = '\n'; break;
                  case 't':  esc = '\t'; break;
                  case 'r':  esc = '\r'; break;
                  case '\\': esc = '\\'; break;
                  case '"':  esc = '"';  break;
                  case '/':  esc = '/';  break;
                  case '0':  esc = '\0'; break;
                  default:   esc = p[1]; break;
                }
                if (buflen + 1 >= bufcapa) { bufcapa *= 2; buf = (char *)realloc(buf, bufcapa); }
                buf[buflen++] = esc;
                p += 2;
            } else {
                if (*p == '\n') line_no++;
                if (buflen + 1 >= bufcapa) { bufcapa *= 2; buf = (char *)realloc(buf, bufcapa); }
                buf[buflen++] = *p++;
            }
        }
        if (*p != '"') parse_error("unterminated string literal");
        p++;
        buf[buflen] = '\0';
        t.kind = TK_STRING;
        t.str  = buf;
        t.str_len = buflen;
        t.start = start;
        t.len = (int)(p - start - 1);
        return t;
    }

    // Punctuation / operators.
    char c = *p;
    switch (c) {
      case '(': p++; paren_depth++; t.kind = TK_LPAREN; t.len = 1; return t;
      case ')': p++; if (paren_depth > 0) paren_depth--; t.kind = TK_RPAREN; t.len = 1; return t;
      case '{': p++; t.kind = TK_LBRACE; t.len = 1; return t;
      case '}': p++; t.kind = TK_RBRACE; t.len = 1; return t;
      case '[': p++; t.kind = TK_LBRACK; t.len = 1; return t;
      case ']': p++; t.kind = TK_RBRACK; t.len = 1; return t;
      case ',': p++; t.kind = TK_COMMA;  t.len = 1; return t;
      case ';': p++; t.kind = TK_SEMI;   t.len = 1; return t;
      case '$': p++; t.kind = TK_DOLLAR; t.len = 1; return t;
      case '?': p++; t.kind = TK_QUESTION; t.len = 1; return t;
      case ':': p++; t.kind = TK_COLON; t.len = 1; return t;
      case '+':
        if (p[1] == '+') { p += 2; t.kind = TK_INC;          t.len = 2; return t; }
        if (p[1] == '=') { p += 2; t.kind = TK_PLUS_ASSIGN;  t.len = 2; return t; }
        p++; t.kind = TK_PLUS; t.len = 1; return t;
      case '-':
        if (p[1] == '-') { p += 2; t.kind = TK_DEC;          t.len = 2; return t; }
        if (p[1] == '=') { p += 2; t.kind = TK_MINUS_ASSIGN; t.len = 2; return t; }
        p++; t.kind = TK_MINUS; t.len = 1; return t;
      case '*':
        if (p[1] == '=') { p += 2; t.kind = TK_STAR_ASSIGN;  t.len = 2; return t; }
        p++; t.kind = TK_STAR; t.len = 1; return t;
      case '/':
        if (p[1] == '=') { p += 2; t.kind = TK_SLASH_ASSIGN; t.len = 2; return t; }
        p++; t.kind = TK_SLASH; t.len = 1; return t;
      case '%':
        if (p[1] == '=') { p += 2; t.kind = TK_PERCENT_ASSIGN; t.len = 2; return t; }
        p++; t.kind = TK_PERCENT; t.len = 1; return t;
      case '^':
        if (p[1] == '=') { p += 2; t.kind = TK_CARET_ASSIGN; t.len = 2; return t; }
        p++; t.kind = TK_CARET; t.len = 1; return t;
      case '<':
        if (p[1] == '=') { p += 2; t.kind = TK_LE; t.len = 2; return t; }
        p++; t.kind = TK_LT; t.len = 1; return t;
      case '>':
        if (p[1] == '=') { p += 2; t.kind = TK_GE; t.len = 2; return t; }
        if (p[1] == '>') { p += 2; t.kind = TK_APPEND; t.len = 2; return t; }
        p++; t.kind = TK_GT; t.len = 1; return t;
      case '=':
        if (p[1] == '=') { p += 2; t.kind = TK_EQ; t.len = 2; return t; }
        p++; t.kind = TK_ASSIGN; t.len = 1; return t;
      case '!':
        if (p[1] == '=') { p += 2; t.kind = TK_NE; t.len = 2; return t; }
        p++; t.kind = TK_NOT; t.len = 1; return t;
      case '&':
        if (p[1] == '&') { p += 2; t.kind = TK_AND; t.len = 2; return t; }
        parse_error("unexpected `&`");
      case '|':
        if (p[1] == '|') { p += 2; t.kind = TK_OR; t.len = 2; return t; }
        p++; t.kind = TK_PIPE; t.len = 1; return t;
      default:
        parse_error("unexpected character `%c`", c);
    }
}

static Token
peek_tok(void)
{
    if (!have_lookahead) {
        if (pushback_cnt > 0) {
            next_tok = pushback_buf[--pushback_cnt];
        } else {
            next_tok = read_token();
        }
        have_lookahead = true;
    }
    return next_tok;
}

static Token
take_tok(void)
{
    if (have_lookahead) { have_lookahead = false; return next_tok; }
    if (pushback_cnt > 0) return pushback_buf[--pushback_cnt];
    return read_token();
}

// Multi-level pushback.  Putting back `t` when there's already a
// lookahead means we have to remember the lookahead too — the source
// pointer is past it and a fresh read would skip it.  Pushback stack
// is LIFO; legal usage stays within PUSHBACK_MAX.
static void
unread_tok(Token t)
{
    if (have_lookahead) {
        if (pushback_cnt >= PUSHBACK_MAX) parse_error("pushback stack overflow");
        pushback_buf[pushback_cnt++] = next_tok;
        have_lookahead = false;
    }
    if (pushback_cnt >= PUSHBACK_MAX) parse_error("pushback stack overflow");
    pushback_buf[pushback_cnt++] = t;
}

static void
expect(TokKind k, const char *what)
{
    Token t = take_tok();
    if (t.kind != k) {
        parse_error("expected %s, got token kind %d (\"%.*s\")", what, t.kind, t.len, t.start);
    }
}

static void
skip_terminators(void)
{
    for (;;) {
        Token t = peek_tok();
        if (t.kind == TK_NL || t.kind == TK_SEMI) { (void)take_tok(); continue; }
        return;
    }
}

// ---------------------------------------------------------------------------
// Scope: maps global variable names to env slots.  Phase 0+1 — single
// global scope; specials reserved 0..AWK_GLOB_RESERVED-1.
// ---------------------------------------------------------------------------

#define MAX_GLOBALS 256

typedef struct {
    const char *names[MAX_GLOBALS];
    uint32_t    count;
} GlobalScope;

static GlobalScope globals;

static void
globals_init(void)
{
    // Pre-reserve special slots so they don't collide with user vars.
    for (uint32_t i = 0; i < AWK_GLOB_RESERVED; i++) globals.names[i] = NULL;
    globals.names[AWK_GLOB_NR]       = "NR";
    globals.names[AWK_GLOB_NF]       = "NF";
    globals.names[AWK_GLOB_FS]       = "FS";
    globals.names[AWK_GLOB_OFS]      = "OFS";
    globals.names[AWK_GLOB_ORS]      = "ORS";
    globals.names[AWK_GLOB_RS]       = "RS";
    globals.names[AWK_GLOB_FILENAME] = "FILENAME";
    globals.names[AWK_GLOB_FNR]      = "FNR";
    globals.names[AWK_GLOB_SUBSEP]   = "SUBSEP";
    globals.names[AWK_GLOB_CONVFMT]  = "CONVFMT";
    globals.names[AWK_GLOB_OFMT]     = "OFMT";
    globals.names[AWK_GLOB_RSTART]   = "RSTART";
    globals.names[AWK_GLOB_RLENGTH]  = "RLENGTH";
    globals.names[AWK_GLOB_ENVIRON]  = "ENVIRON";
    globals.names[AWK_GLOB_ARGC]     = "ARGC";
    globals.names[AWK_GLOB_ARGV]     = "ARGV";
    globals.count = AWK_GLOB_RESERVED;
}

static uint32_t
globals_intern(const char *name)
{
    for (uint32_t i = 0; i < globals.count; i++) {
        if (globals.names[i] && strcmp(globals.names[i], name) == 0) return i;
    }
    if (globals.count >= MAX_GLOBALS) parse_error("too many globals");
    uint32_t slot = globals.count++;
    globals.names[slot] = name;
    return slot;
}

uint32_t
arawk_globals_count(void)
{
    return globals.count;
}

const char *
arawk_globals_name(uint32_t slot)
{
    if (slot >= globals.count) return NULL;
    return globals.names[slot];
}

// ---------------------------------------------------------------------------
// Function-local scope (Phase 1.8).  When parsing a function body,
// cur_scope is non-NULL; identifiers look up here first and fall
// through to the global table.  Locals occupy frame slots 0..N-1 in
// the callee's VLA `F[]`.
// ---------------------------------------------------------------------------

#define LOCAL_MAX 64

typedef struct LocalScope {
    const char *names[LOCAL_MAX];
    uint32_t count;
    uint32_t params_cnt;       // number of formals (rest are pure locals)
} LocalScope;

static LocalScope *cur_scope = NULL;

// Resolved name reference.  `is_local` selects between frame-based
// (lget / aget / postinc_l ...) and env-based (gget / aget_g / ...)
// emission helpers below.
typedef struct {
    bool     is_local;
    uint32_t slot;
} Var;

static Var
resolve_name(const char *name)
{
    if (cur_scope) {
        for (uint32_t i = 0; i < cur_scope->count; i++) {
            if (strcmp(cur_scope->names[i], name) == 0) {
                return (Var){ .is_local = true, .slot = i };
            }
        }
    }
    return (Var){ .is_local = false, .slot = globals_intern(name) };
}

static uint32_t
scope_add_local(LocalScope *s, const char *name)
{
    if (s->count >= LOCAL_MAX) parse_error("too many locals (max %d)", LOCAL_MAX);
    s->names[s->count] = name;
    return s->count++;
}

// Emission helpers — pick local- vs global-flavoured node based on
// `v.is_local`.  Centralises the scope dispatch; the parser body just
// calls these and never picks the node kind directly.
static NODE *emit_var_get(Var v)
{
    return v.is_local ? ALLOC_arawk_node_lget(v.slot) : ALLOC_arawk_node_gget(v.slot);
}
static NODE *emit_var_set(Var v, NODE *rhs)
{
    return v.is_local ? ALLOC_arawk_node_lset(v.slot, rhs) : ALLOC_arawk_node_gset(v.slot, rhs);
}
static NODE *emit_arr_get(Var v, NODE *key)
{
    return v.is_local ? ALLOC_arawk_node_aget(v.slot, key) : ALLOC_arawk_node_aget_g(v.slot, key);
}
static NODE *emit_arr_set(Var v, NODE *key, NODE *rhs)
{
    return v.is_local ? ALLOC_arawk_node_aset(v.slot, key, rhs) : ALLOC_arawk_node_aset_g(v.slot, key, rhs);
}
static NODE *emit_postinc(Var v)
{
    return v.is_local ? ALLOC_arawk_node_postinc_l(v.slot) : ALLOC_arawk_node_postinc_g(v.slot);
}
static NODE *emit_postdec(Var v)
{
    return v.is_local ? ALLOC_arawk_node_postdec_l(v.slot) : ALLOC_arawk_node_postdec_g(v.slot);
}
static NODE *emit_preinc(Var v)
{
    return v.is_local ? ALLOC_arawk_node_preinc_l(v.slot) : ALLOC_arawk_node_preinc_g(v.slot);
}
static NODE *emit_predec(Var v)
{
    return v.is_local ? ALLOC_arawk_node_predec_l(v.slot) : ALLOC_arawk_node_predec_g(v.slot);
}
static NODE *emit_in_arr(NODE *key, Var v)
{
    return v.is_local ? ALLOC_arawk_node_in_arr(key, v.slot) : ALLOC_arawk_node_in_arr_g(key, v.slot);
}
static NODE *emit_for_in(Var key_v, Var arr_v, NODE *body)
{
    if (key_v.is_local && arr_v.is_local)
        return ALLOC_arawk_node_for_in(key_v.slot, arr_v.slot, body);
    if (!key_v.is_local && !arr_v.is_local)
        return ALLOC_arawk_node_for_in_g(key_v.slot, arr_v.slot, body);
    parse_error("for (k in arr): mixed local/global key+array not supported");
}
static NODE *emit_delete(Var v, NODE *key)
{
    return v.is_local ? ALLOC_arawk_node_delete(v.slot, key) : ALLOC_arawk_node_delete_g(v.slot, key);
}
static NODE *emit_delete_all(Var v)
{
    return v.is_local ? ALLOC_arawk_node_delete_all(v.slot) : ALLOC_arawk_node_delete_all_g(v.slot);
}

// ---------------------------------------------------------------------------
// Pratt-style expression parser.  Awk precedence (low → high):
//   ||
//   &&
//   == != < <= > >=
//   <CONCAT>            (implicit, juxtaposition)
//   + -
//   * / %
//   unary ! - +
//   ^                   (right-associative)
//   $expr
//   primary
// ---------------------------------------------------------------------------

static NODE *parse_expr(void);
static NODE *parse_unary(void);
static NODE *parse_primary(void);
static NODE *parse_stmt(void);
static NODE *parse_block(void);
static NODE *parse_array_key(void);

static bool
is_primary_start(TokKind k)
{
    switch (k) {
      case TK_INT: case TK_FLOAT: case TK_STRING: case TK_NAME:
      case TK_DOLLAR: case TK_LPAREN:
      case TK_KW_LENGTH:  case TK_KW_SPRINTF: case TK_KW_SUBSTR:
      case TK_KW_INDEX:   case TK_KW_SPLIT:
      case TK_KW_TOLOWER: case TK_KW_TOUPPER: case TK_KW_INT_FN:
      case TK_KW_SIN: case TK_KW_COS: case TK_KW_SQRT: case TK_KW_EXP:
      case TK_KW_LOG: case TK_KW_ATAN2:
      case TK_KW_RAND: case TK_KW_SRAND:
      case TK_KW_CLOSE: case TK_KW_FFLUSH: case TK_KW_SYSTEM:
      case TK_KW_GETLINE:
        return true;
      default:
        return false;
    }
}

// Precedence chain (low → high), with ternary at the top of the
// expression syntax (assignment is handled inside parse_expr).  Each
// level is split into a top entry `parse_X_full()` and a continuation
// `parse_X_continue(lhs)` so an externally-pre-parsed lhs (e.g. an
// `a[k]` reference recognised by parse_expr) can climb the chain
// without re-tokenising.

static NODE *parse_ternary_continue(NODE *lhs);
static NODE *parse_or_continue(NODE *lhs);
static NODE *parse_and_continue(NODE *lhs);
static NODE *parse_in_continue(NODE *lhs);
static NODE *parse_rel_continue(NODE *lhs);
static NODE *parse_concat_continue(NODE *lhs);
static NODE *parse_add_continue(NODE *lhs);
static NODE *parse_mul_continue(NODE *lhs);
static NODE *parse_pow_continue(NODE *lhs);

static NODE *parse_mul_full(void)     { return parse_mul_continue(parse_unary()); }
static NODE *parse_add_full(void)     { return parse_add_continue(parse_mul_full()); }
static NODE *parse_concat_full(void)  { return parse_concat_continue(parse_add_full()); }
static NODE *parse_rel_full(void)     { return parse_rel_continue(parse_concat_full()); }
static NODE *parse_in_full(void)      { return parse_in_continue(parse_rel_full()); }
static NODE *parse_and_full(void)     { return parse_and_continue(parse_in_full()); }
static NODE *parse_or_full(void)      { return parse_or_continue(parse_and_full()); }
static NODE *parse_ternary_full(void) { return parse_ternary_continue(parse_or_full()); }

// Ternary: `cond ? then : else`, right-associative.
static NODE *
parse_ternary_continue(NODE *cond)
{
    if (peek_tok().kind == TK_QUESTION) {
        (void)take_tok();
        NODE *then_e = parse_ternary_full();
        expect(TK_COLON, ":");
        NODE *else_e = parse_ternary_full();
        return ALLOC_arawk_node_ternary(cond, then_e, else_e);
    }
    return cond;
}

static NODE *
parse_or_continue(NODE *lhs)
{
    while (peek_tok().kind == TK_OR) {
        (void)take_tok();
        lhs = ALLOC_arawk_node_or(lhs, parse_and_full());
    }
    return lhs;
}

static NODE *
parse_and_continue(NODE *lhs)
{
    while (peek_tok().kind == TK_AND) {
        (void)take_tok();
        lhs = ALLOC_arawk_node_and(lhs, parse_in_full());
    }
    return lhs;
}

// `k in arr` — arr must be a NAME (array slot).  Between `&&` and
// relops in POSIX.
static NODE *
parse_in_continue(NODE *lhs)
{
    while (peek_tok().kind == TK_KW_IN) {
        (void)take_tok();
        Token nx = take_tok();
        if (nx.kind != TK_NAME)
            parse_error("expected array name after `in`");
        char *name = intern_string(nx.start, nx.len);
        lhs = emit_in_arr(lhs, resolve_name(name));
    }
    return lhs;
}

static NODE *
parse_rel_continue(NODE *lhs)
{
    TokKind k = peek_tok().kind;
    if (k == TK_LT || k == TK_LE || k == TK_GT || k == TK_GE || k == TK_EQ || k == TK_NE) {
        (void)take_tok();
        NODE *rhs = parse_concat_full();
        switch (k) {
          case TK_LT: return ALLOC_arawk_node_lt(lhs, rhs);
          case TK_LE: return ALLOC_arawk_node_le(lhs, rhs);
          case TK_GT: return ALLOC_arawk_node_gt(lhs, rhs);
          case TK_GE: return ALLOC_arawk_node_ge(lhs, rhs);
          case TK_EQ: return ALLOC_arawk_node_eq(lhs, rhs);
          case TK_NE: return ALLOC_arawk_node_ne(lhs, rhs);
          default: break;
        }
    }
    return lhs;
}

static NODE *
parse_concat_continue(NODE *lhs)
{
    while (is_primary_start(peek_tok().kind)) {
        lhs = ALLOC_arawk_node_concat(lhs, parse_add_full());
    }
    // `cmd | getline [NAME]` — the only place an expression-level
    // `|` appears.  Pipe-to-command for print is parsed inside the
    // print/printf statement and never reaches here.
    if (peek_tok().kind == TK_PIPE) {
        Token p = take_tok();
        if (peek_tok().kind != TK_KW_GETLINE) {
            unread_tok(p);
            return lhs;
        }
        (void)take_tok();    // getline
        if (peek_tok().kind == TK_NAME) {
            Token nx = take_tok();
            char *vname = intern_string(nx.start, nx.len);
            Var v = resolve_name(vname);
            return v.is_local ? ALLOC_arawk_node_getline_cmd_l(v.slot, lhs)
                              : ALLOC_arawk_node_getline_cmd_g(v.slot, lhs);
        }
        return ALLOC_arawk_node_getline_cmd(lhs);
    }
    return lhs;
}

static NODE *
parse_add_continue(NODE *lhs)
{
    for (;;) {
        TokKind k = peek_tok().kind;
        if (k != TK_PLUS && k != TK_MINUS) break;
        (void)take_tok();
        NODE *rhs = parse_mul_full();
        lhs = (k == TK_PLUS) ? ALLOC_arawk_node_add(lhs, rhs) : ALLOC_arawk_node_sub(lhs, rhs);
    }
    return lhs;
}

static NODE *
parse_mul_continue(NODE *lhs)
{
    for (;;) {
        TokKind k = peek_tok().kind;
        if (k != TK_STAR && k != TK_SLASH && k != TK_PERCENT) break;
        (void)take_tok();
        NODE *rhs = parse_unary();
        switch (k) {
          case TK_STAR:    lhs = ALLOC_arawk_node_mul(lhs, rhs); break;
          case TK_SLASH:   lhs = ALLOC_arawk_node_div(lhs, rhs); break;
          case TK_PERCENT: lhs = ALLOC_arawk_node_mod(lhs, rhs); break;
          default: break;
        }
    }
    return lhs;
}

// Unary parsing — replaces the parse_unary entry point.  Returns a
// NODE that climbs through parse_pow_continue if appropriate (handled
// inside parse_unary itself for non-prefix cases by routing through
// parse_pow_continue(parse_primary())).
static NODE *
parse_unary(void)
{
    TokKind k = peek_tok().kind;
    if (k == TK_NOT)   { (void)take_tok(); NODE *x = parse_unary(); return ALLOC_arawk_node_not(x); }
    if (k == TK_MINUS) { (void)take_tok(); NODE *x = parse_unary(); return ALLOC_arawk_node_neg(x); }
    if (k == TK_PLUS)  { (void)take_tok(); NODE *x = parse_unary(); return ALLOC_arawk_node_unaryplus(x); }
    if (k == TK_INC || k == TK_DEC) {
        TokKind op = k;
        (void)take_tok();
        Token nx = take_tok();
        if (nx.kind != TK_NAME)
            parse_error("++/-- requires an lvalue (got token kind %d)", nx.kind);
        char *name = intern_string(nx.start, nx.len);
        Var v = resolve_name(name);
        return op == TK_INC ? emit_preinc(v) : emit_predec(v);
    }
    return parse_pow_continue(parse_primary());
}

// parse_pow_continue handles postfix ++/-- (only when the primary
// captured an lvalue) and the right-assoc ^ operator.  Concat uses
// parse_unary not parse_pow, so prefix ! / - / unary+ / pre-inc can
// sit inside concat operands.
enum primary_lv_kind { PLV_NONE, PLV_LGET, PLV_GGET, PLV_AGET, PLV_AGET_G, PLV_DOLLAR_CONST };
enum primary_lv_kind primary_lv = PLV_NONE;
uint32_t primary_lget_slot = 0;
int32_t  primary_dollar_n = 0;
NODE    *primary_aget_key = NULL;
// (Field name kept for compatibility with the prefix-++ pre-parser
// branch which still calls it `primary_was_lget`.)
bool primary_was_lget = false;

static NODE *
parse_pow_continue(NODE *lhs)
{
    TokKind k = peek_tok().kind;
    if (k == TK_INC || k == TK_DEC) {
        if (primary_lv == PLV_LGET || primary_lv == PLV_GGET) {
            (void)take_tok();
            Var v = { .is_local = (primary_lv == PLV_LGET), .slot = primary_lget_slot };
            primary_lv = PLV_NONE; primary_was_lget = false;
            return k == TK_INC ? emit_postinc(v) : emit_postdec(v);
        }
        if (primary_lv == PLV_AGET || primary_lv == PLV_AGET_G) {
            (void)take_tok();
            bool is_local = (primary_lv == PLV_AGET);
            uint32_t slot = primary_lget_slot;
            NODE *key = primary_aget_key;
            primary_lv = PLV_NONE; primary_was_lget = false; primary_aget_key = NULL;
            if (k == TK_INC) {
                return is_local ? ALLOC_arawk_node_postinc_a(slot, key)
                                : ALLOC_arawk_node_postinc_ag(slot, key);
            } else {
                return is_local ? ALLOC_arawk_node_postdec_a(slot, key)
                                : ALLOC_arawk_node_postdec_ag(slot, key);
            }
        }
        if (primary_lv == PLV_DOLLAR_CONST) {
            (void)take_tok();
            primary_lv = PLV_NONE;
            return k == TK_INC ? ALLOC_arawk_node_dollar_postinc(primary_dollar_n)
                               : ALLOC_arawk_node_dollar_postdec(primary_dollar_n);
        }
    }
    primary_lv = PLV_NONE; primary_was_lget = false; primary_aget_key = NULL;
    if (peek_tok().kind == TK_CARET) {
        (void)take_tok();
        NODE *rhs = parse_unary();   // right-associative
        return ALLOC_arawk_node_pow(lhs, rhs);
    }
    return lhs;
}

NODE *
parse_primary(void)
{
    Token t = take_tok();
    switch (t.kind) {
      case TK_INT: {
        if (t.inum >= INT32_MIN && t.inum <= INT32_MAX) {
            return ALLOC_arawk_node_int((int32_t)t.inum);
        }
        return ALLOC_arawk_node_int64((uint64_t)t.inum);
      }
      case TK_FLOAT: {
        union { double d; uint64_t u; } pun = { .d = t.fnum };
        return ALLOC_arawk_node_float(pun.u);
      }
      case TK_STRING:
        return ALLOC_arawk_node_str(t.str);
      case TK_LPAREN: {
        NODE *e = parse_expr();
        expect(TK_RPAREN, ")");
        return e;
      }
      case TK_DOLLAR: {
        // `$N` literal-int fast path; else generic.
        Token nx = peek_tok();
        if (nx.kind == TK_INT && nx.inum >= 0 && nx.inum <= 1024) {
            (void)take_tok();
            primary_lv = PLV_DOLLAR_CONST;
            primary_dollar_n = (int32_t)nx.inum;
            return ALLOC_arawk_node_dollar_const((int32_t)nx.inum);
        }
        NODE *idx = parse_primary();
        return ALLOC_arawk_node_dollar(idx);
      }
      case TK_NAME: {
        char *name = intern_string(t.start, t.len);
        // `NAME ( args )` — user-defined function call.  Builtin
        // function names were caught as their own TK_KW_* tokens
        // earlier, so any TK_NAME LPAREN here must be a user call.
        if (peek_tok().kind == TK_LPAREN) {
            (void)take_tok();
            NODE *args[ARAWK_FRAME_MAX]; uint32_t argc = 0;
            if (peek_tok().kind != TK_RPAREN) {
                args[argc++] = parse_expr();
                while (peek_tok().kind == TK_COMMA) {
                    (void)take_tok();
                    if (argc >= ARAWK_FRAME_MAX) parse_error("too many call args");
                    args[argc++] = parse_expr();
                }
            }
            expect(TK_RPAREN, ")");
            uint32_t base = argc ? arawk_node_table_push_n(args, argc) : 0;
            return ALLOC_arawk_node_call_user(name, base, argc);
        }
        Var v = resolve_name(name);
        // `NAME [ key ]` — array reference (rvalue).
        if (peek_tok().kind == TK_LBRACK) {
            (void)take_tok();
            NODE *key = parse_array_key();
            expect(TK_RBRACK, "]");
            // Track the lvalue shape so parse_pow_continue can emit
            // postfix ++/-- on this aget if the next token is one.
            primary_lv = v.is_local ? PLV_AGET : PLV_AGET_G;
            primary_lget_slot = v.slot;
            primary_aget_key = key;
            return emit_arr_get(v, key);
        }
        primary_was_lget = v.is_local;
        primary_lget_slot = v.slot;
        primary_lv = v.is_local ? PLV_LGET : PLV_GGET;
        return emit_var_get(v);
      }
      case TK_KW_LENGTH: {
        if (peek_tok().kind == TK_LPAREN) {
            (void)take_tok();
            if (peek_tok().kind == TK_RPAREN) {
                (void)take_tok();
                return ALLOC_arawk_node_length0();
            }
            NODE *arg = parse_expr();
            expect(TK_RPAREN, ")");
            return ALLOC_arawk_node_length1(arg);
        }
        return ALLOC_arawk_node_length0();
      }
      case TK_KW_SUBSTR: {
        expect(TK_LPAREN, "(");
        NODE *s = parse_expr();
        expect(TK_COMMA, ",");
        NODE *pos = parse_expr();
        if (peek_tok().kind == TK_COMMA) {
            (void)take_tok();
            NODE *len = parse_expr();
            expect(TK_RPAREN, ")");
            return ALLOC_arawk_node_substr3(s, pos, len);
        }
        expect(TK_RPAREN, ")");
        return ALLOC_arawk_node_substr2(s, pos);
      }
      case TK_KW_INDEX: {
        expect(TK_LPAREN, "(");
        NODE *h = parse_expr();
        expect(TK_COMMA, ",");
        NODE *n2 = parse_expr();
        expect(TK_RPAREN, ")");
        return ALLOC_arawk_node_index_b(h, n2);
      }
      case TK_KW_SPLIT: {
        expect(TK_LPAREN, "(");
        NODE *s = parse_expr();
        expect(TK_COMMA, ",");
        Token an = take_tok();
        if (an.kind != TK_NAME) parse_error("split: 2nd arg must be array name");
        char *aname = intern_string(an.start, an.len);
        // split: only global arrays for Phase 1.8 (rare to split into
        // a function-local).
        uint32_t aslot = resolve_name(aname).is_local
                       ? parse_error("split: local-array target not supported"), 0u
                       : globals_intern(aname);
        if (peek_tok().kind == TK_COMMA) {
            (void)take_tok();
            NODE *sep = parse_expr();
            expect(TK_RPAREN, ")");
            return ALLOC_arawk_node_split3(s, aslot, sep);
        }
        expect(TK_RPAREN, ")");
        return ALLOC_arawk_node_split2(s, aslot);
      }
      case TK_KW_TOLOWER: case TK_KW_TOUPPER:
      case TK_KW_INT_FN:
      case TK_KW_SIN: case TK_KW_COS: case TK_KW_SQRT:
      case TK_KW_EXP: case TK_KW_LOG: {
        expect(TK_LPAREN, "(");
        NODE *a = parse_expr();
        expect(TK_RPAREN, ")");
        switch (t.kind) {
          case TK_KW_TOLOWER: return ALLOC_arawk_node_tolower(a);
          case TK_KW_TOUPPER: return ALLOC_arawk_node_toupper(a);
          case TK_KW_INT_FN:  return ALLOC_arawk_node_int_fn(a);
          case TK_KW_SIN:     return ALLOC_arawk_node_sin(a);
          case TK_KW_COS:     return ALLOC_arawk_node_cos(a);
          case TK_KW_SQRT:    return ALLOC_arawk_node_sqrt(a);
          case TK_KW_EXP:     return ALLOC_arawk_node_exp(a);
          case TK_KW_LOG:     return ALLOC_arawk_node_log(a);
          default: break;
        }
        parse_error("internal: unary builtin dispatch fell through");
      }
      case TK_KW_ATAN2: {
        expect(TK_LPAREN, "(");
        NODE *a = parse_expr();
        expect(TK_COMMA, ",");
        NODE *b = parse_expr();
        expect(TK_RPAREN, ")");
        return ALLOC_arawk_node_atan2(a, b);
      }
      case TK_KW_RAND: {
        if (peek_tok().kind == TK_LPAREN) { (void)take_tok(); expect(TK_RPAREN, ")"); }
        return ALLOC_arawk_node_rand();
      }
      case TK_KW_SRAND: {
        if (peek_tok().kind != TK_LPAREN) return ALLOC_arawk_node_srand0();
        (void)take_tok();
        if (peek_tok().kind == TK_RPAREN) { (void)take_tok(); return ALLOC_arawk_node_srand0(); }
        NODE *a = parse_expr();
        expect(TK_RPAREN, ")");
        return ALLOC_arawk_node_srand1(a);
      }
      case TK_KW_SPRINTF: {
        expect(TK_LPAREN, "(");
        NODE *fmt = parse_expr();
        NODE *items[64]; uint32_t n = 0;
        while (peek_tok().kind == TK_COMMA) {
            (void)take_tok();
            if (n >= 64) parse_error("too many sprintf arguments");
            items[n++] = parse_expr();
        }
        expect(TK_RPAREN, ")");
        uint32_t base = n ? arawk_node_table_push_n(items, n) : 0;
        return ALLOC_arawk_node_sprintf(fmt, base, n);
      }
      case TK_KW_CLOSE: {
        expect(TK_LPAREN, "(");
        NODE *a = parse_expr();
        expect(TK_RPAREN, ")");
        return ALLOC_arawk_node_close(a);
      }
      case TK_KW_FFLUSH: {
        if (peek_tok().kind != TK_LPAREN) return ALLOC_arawk_node_fflush_0();
        (void)take_tok();
        if (peek_tok().kind == TK_RPAREN) { (void)take_tok(); return ALLOC_arawk_node_fflush_0(); }
        NODE *a = parse_expr();
        expect(TK_RPAREN, ")");
        return ALLOC_arawk_node_fflush_1(a);
      }
      case TK_KW_SYSTEM: {
        expect(TK_LPAREN, "(");
        NODE *a = parse_expr();
        expect(TK_RPAREN, ")");
        return ALLOC_arawk_node_system(a);
      }
      case TK_KW_GETLINE: {
        // Four leading forms (the two `cmd | getline ...` forms are
        // recognised in parse_concat_continue):
        //   getline           → from current input → $0      (+NR/FNR/NF)
        //   getline NAME      → from current input → NAME    (+NR/FNR)
        //   getline < expr    → from file → $0   (no NR bump)
        //   getline NAME < expr → from file → NAME           (no side effects)
        Token la = peek_tok();
        if (la.kind == TK_NAME) {
            Token nx = take_tok();
            char *vname = intern_string(nx.start, nx.len);
            Var v = resolve_name(vname);
            if (peek_tok().kind == TK_LT) {
                (void)take_tok();
                NODE *file = parse_unary();
                return v.is_local ? ALLOC_arawk_node_getline_file_l(v.slot, file)
                                  : ALLOC_arawk_node_getline_file_g(v.slot, file);
            }
            return v.is_local ? ALLOC_arawk_node_getline_cur_l(v.slot)
                              : ALLOC_arawk_node_getline_cur_g(v.slot);
        }
        if (la.kind == TK_LT) {
            (void)take_tok();
            NODE *file = parse_unary();
            return ALLOC_arawk_node_getline_file(file);
        }
        return ALLOC_arawk_node_getline_cur();
      }
      default:
        parse_error("unexpected token in primary (kind %d, \"%.*s\")", t.kind, t.len, t.start);
    }
}

// Assignment is the lowest-precedence operator and is handled here
// in parse_expr instead of in the Pratt chain.  Supported lvalues:
// scalar NAME, array NAME[key], and field $expr / $N.  Compound forms
// (`+=`, etc.) desugar to `lhs = lhs OP rhs`.
//
// `NAME[key]` references that are NOT followed by `=` fall through to
// parse_primary which emits arawk_node_aget — so `a[k] + 1` works naturally.

static bool
is_assign_op(TokKind k)
{
    return k == TK_ASSIGN
        || k == TK_PLUS_ASSIGN    || k == TK_MINUS_ASSIGN
        || k == TK_STAR_ASSIGN    || k == TK_SLASH_ASSIGN
        || k == TK_PERCENT_ASSIGN || k == TK_CARET_ASSIGN;
}

static NODE *
combine_binop(TokKind compound_op, NODE *lhs, NODE *rhs)
{
    switch (compound_op) {
      case TK_PLUS_ASSIGN:    return ALLOC_arawk_node_add(lhs, rhs);
      case TK_MINUS_ASSIGN:   return ALLOC_arawk_node_sub(lhs, rhs);
      case TK_STAR_ASSIGN:    return ALLOC_arawk_node_mul(lhs, rhs);
      case TK_SLASH_ASSIGN:   return ALLOC_arawk_node_div(lhs, rhs);
      case TK_PERCENT_ASSIGN: return ALLOC_arawk_node_mod(lhs, rhs);
      case TK_CARET_ASSIGN:   return ALLOC_arawk_node_pow(lhs, rhs);
      default: parse_error("internal: bad compound op kind %d", compound_op);
    }
}

NODE *
parse_expr(void)
{
    Token la = peek_tok();
    // `$N = rhs` and `$N op= rhs` (literal N fast path).  Generic
    // `$expr = rhs` falls back to the slow path through parse_primary
    // + parse-then-rewrite (TODO).
    if (la.kind == TK_DOLLAR) {
        Token dol = take_tok();
        Token next = peek_tok();
        if (next.kind == TK_INT) {
            Token nx = take_tok();
            Token op = peek_tok();
            if (is_assign_op(op.kind)) {
                (void)take_tok();
                NODE *rhs = parse_expr();
                int32_t fi = (int32_t)nx.inum;
                if (op.kind == TK_ASSIGN) return ALLOC_arawk_node_dollar_const_set(fi, rhs);
                NODE *get = ALLOC_arawk_node_dollar_const(fi);
                return ALLOC_arawk_node_dollar_const_set(fi, combine_binop(op.kind, get, rhs));
            }
            unread_tok(nx);
            unread_tok(dol);
        }
        else {
            unread_tok(dol);
        }
    }
    if (la.kind == TK_NAME) {
        Token n = take_tok();
        Token op = peek_tok();
        if (is_assign_op(op.kind)) {
            (void)take_tok();
            char *name = intern_string(n.start, n.len);
            Var v = resolve_name(name);
            NODE *rhs = parse_expr();
            if (op.kind == TK_ASSIGN) return emit_var_set(v, rhs);
            return emit_var_set(v, combine_binop(op.kind, emit_var_get(v), rhs));
        }
        if (op.kind == TK_LBRACK) {
            (void)take_tok();
            NODE *key = parse_array_key();
            expect(TK_RBRACK, "]");
            TokKind op2 = peek_tok().kind;
            if (is_assign_op(op2)) {
                (void)take_tok();
                char *name = intern_string(n.start, n.len);
                Var v = resolve_name(name);
                NODE *rhs = parse_expr();
                if (op2 == TK_ASSIGN) return emit_arr_set(v, key, rhs);
                return emit_arr_set(v, key, combine_binop(op2, emit_arr_get(v, key), rhs));
            }
            // Not an assignment: this `NAME[key]` is the start of a
            // larger expression.  Build the aget primary and climb
            // the Pratt chain from there.  Stash the aget shape in
            // primary_lv so a trailing `++` / `--` can promote into
            // a postinc_a / postdec_a in parse_pow_continue.
            char *name = intern_string(n.start, n.len);
            Var v = resolve_name(name);
            NODE *primary = emit_arr_get(v, key);
            primary_lv = v.is_local ? PLV_AGET : PLV_AGET_G;
            primary_lget_slot = v.slot;
            primary_aget_key = key;
            primary_was_lget = false;
            NODE *lhs = parse_pow_continue(primary);
            lhs = parse_mul_continue(lhs);
            lhs = parse_add_continue(lhs);
            lhs = parse_concat_continue(lhs);
            lhs = parse_rel_continue(lhs);
            lhs = parse_in_continue(lhs);
            lhs = parse_and_continue(lhs);
            lhs = parse_or_continue(lhs);
            lhs = parse_ternary_continue(lhs);
            return lhs;
        }
        unread_tok(n);
    }
    return parse_ternary_full();
}

// `a[i, j]` keys: comma-separated parts are joined with SUBSEP
// (matching gawk / POSIX semantics).  Single-key path is the
// fast path.
static NODE *
parse_array_key(void)
{
    NODE *first = parse_ternary_full();
    if (peek_tok().kind != TK_COMMA) return first;
    NODE *acc = first;
    while (peek_tok().kind == TK_COMMA) {
        (void)take_tok();
        NODE *next = parse_ternary_full();
        NODE *sep  = ALLOC_arawk_node_gget(AWK_GLOB_SUBSEP);
        acc = ALLOC_arawk_node_concat(ALLOC_arawk_node_concat(acc, sep), next);
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Statements.
// ---------------------------------------------------------------------------

static NODE *
parse_print_args(void)
{
    // print with no args → arawk_node_print(base=0, cnt=0): runtime prints $0.
    Token la = peek_tok();
    if (la.kind == TK_SEMI || la.kind == TK_NL || la.kind == TK_RBRACE || la.kind == TK_EOF) {
        return ALLOC_arawk_node_print(0, 0);
    }
    NODE *items[64];
    uint32_t n = 0;
    if (la.kind != TK_PIPE && la.kind != TK_APPEND && la.kind != TK_GT) {
        items[n++] = parse_concat_full();
        while (peek_tok().kind == TK_COMMA) {
            (void)take_tok();
            if (n >= 64) parse_error("too many print arguments");
            items[n++] = parse_concat_full();
        }
    }
    TokKind redir = peek_tok().kind;
    if (redir == TK_PIPE || redir == TK_GT || redir == TK_APPEND) {
        (void)take_tok();
        NODE *dest = parse_concat_full();
        int32_t mode = redir == TK_PIPE   ? 'w'
                     : redir == TK_APPEND ? 'a'
                     :                      'o';
        uint32_t base = n ? arawk_node_table_push_n(items, n) : 0;
        return ALLOC_arawk_node_print_to(base, n, dest, mode);
    }
    uint32_t base = arawk_node_table_push_n(items, n);
    return ALLOC_arawk_node_print(base, n);
}

NODE *
parse_block(void)
{
    expect(TK_LBRACE, "{");
    skip_terminators();
    if (peek_tok().kind == TK_RBRACE) {
        (void)take_tok();
        return ALLOC_arawk_node_int(0);     // empty block
    }
    NODE *acc = parse_stmt();
    for (;;) {
        skip_terminators();
        if (peek_tok().kind == TK_RBRACE) break;
        NODE *next = parse_stmt();
        acc = ALLOC_arawk_node_seq(acc, next);
    }
    expect(TK_RBRACE, "}");
    return acc;
}

NODE *
parse_stmt(void)
{
    skip_terminators();
    Token la = peek_tok();
    switch (la.kind) {
      case TK_KW_PRINT:
        (void)take_tok();
        return parse_print_args();
      case TK_KW_PRINTF: {
        (void)take_tok();
        // `printf fmt, args...` — the format is mandatory; args list
        // may be empty.  Parentheses around the whole arg list are
        // optional (POSIX).
        bool has_paren = peek_tok().kind == TK_LPAREN;
        if (has_paren) (void)take_tok();
        NODE *fmt = parse_concat_full();
        NODE *items[64]; uint32_t n = 0;
        while (peek_tok().kind == TK_COMMA) {
            (void)take_tok();
            if (n >= 64) parse_error("too many printf arguments");
            items[n++] = parse_concat_full();
        }
        if (has_paren) expect(TK_RPAREN, ")");
        TokKind redir = peek_tok().kind;
        if (redir == TK_PIPE || redir == TK_GT || redir == TK_APPEND) {
            (void)take_tok();
            NODE *dest = parse_concat_full();
            int32_t mode = redir == TK_PIPE   ? 'w'
                         : redir == TK_APPEND ? 'a'
                         :                      'o';
            uint32_t base = n ? arawk_node_table_push_n(items, n) : 0;
            return ALLOC_arawk_node_printf_to(fmt, base, n, dest, mode);
        }
        uint32_t base = n ? arawk_node_table_push_n(items, n) : 0;
        return ALLOC_arawk_node_printf(fmt, base, n);
      }
      case TK_KW_IF: {
        (void)take_tok();
        expect(TK_LPAREN, "(");
        NODE *cond = parse_expr();
        expect(TK_RPAREN, ")");
        skip_terminators();
        NODE *then_s = parse_stmt();
        NODE *else_s = ALLOC_arawk_node_int(0);
        skip_terminators();
        if (peek_tok().kind == TK_KW_ELSE) {
            (void)take_tok();
            skip_terminators();
            else_s = parse_stmt();
        }
        return ALLOC_arawk_node_if(cond, then_s, else_s);
      }
      case TK_KW_WHILE: {
        (void)take_tok();
        expect(TK_LPAREN, "(");
        NODE *cond = parse_expr();
        expect(TK_RPAREN, ")");
        skip_terminators();
        NODE *body = parse_stmt();
        return ALLOC_arawk_node_while(cond, body);
      }
      case TK_KW_FOR: {
        (void)take_tok();
        expect(TK_LPAREN, "(");
        // Distinguish `for (k in arr)` from C-style `for (e; e; e)`.
        // Two-token peek: NAME followed by `in` is the array form.
        if (peek_tok().kind == TK_NAME) {
            Token nk = take_tok();
            if (peek_tok().kind == TK_KW_IN) {
                (void)take_tok();
                Token na = take_tok();
                if (na.kind != TK_NAME)
                    parse_error("for (k in arr): expected array name");
                expect(TK_RPAREN, ")");
                char *k_name = intern_string(nk.start, nk.len);
                char *a_name = intern_string(na.start, na.len);
                Var k_var = resolve_name(k_name);
                Var a_var = resolve_name(a_name);
                skip_terminators();
                NODE *body = parse_stmt();
                return emit_for_in(k_var, a_var, body);
            }
            unread_tok(nk);
        }
        NODE *init = (peek_tok().kind == TK_SEMI) ? ALLOC_arawk_node_noop() : parse_expr();
        expect(TK_SEMI, ";");
        NODE *cond;
        uint32_t has_cond = 1;
        if (peek_tok().kind == TK_SEMI) { cond = ALLOC_arawk_node_noop(); has_cond = 0; }
        else                            { cond = parse_expr(); }
        expect(TK_SEMI, ";");
        NODE *step = (peek_tok().kind == TK_RPAREN) ? ALLOC_arawk_node_noop() : parse_expr();
        expect(TK_RPAREN, ")");
        skip_terminators();
        NODE *body = parse_stmt();
        return ALLOC_arawk_node_for(init, cond, step, body, has_cond);
      }
      case TK_KW_DELETE: {
        (void)take_tok();
        Token nm = take_tok();
        if (nm.kind != TK_NAME) parse_error("delete: expected array name");
        char *name = intern_string(nm.start, nm.len);
        Var v = resolve_name(name);
        if (peek_tok().kind == TK_LBRACK) {
            (void)take_tok();
            NODE *key = parse_array_key();
            expect(TK_RBRACK, "]");
            return emit_delete(v, key);
        }
        return emit_delete_all(v);
      }
      case TK_KW_NEXT:     (void)take_tok(); return ALLOC_arawk_node_next();
      case TK_KW_BREAK:    (void)take_tok(); return ALLOC_arawk_node_break();
      case TK_KW_CONTINUE: (void)take_tok(); return ALLOC_arawk_node_continue();
      case TK_KW_RETURN: {
        (void)take_tok();
        Token nx = peek_tok();
        // Optional return value: stops at `;` / `}` / NL / EOF.
        if (nx.kind == TK_SEMI || nx.kind == TK_NL || nx.kind == TK_RBRACE || nx.kind == TK_EOF) {
            return ALLOC_arawk_node_return(ALLOC_arawk_node_int(0));
        }
        return ALLOC_arawk_node_return(parse_expr());
      }
      case TK_KW_EXIT: {
        (void)take_tok();
        Token nx = peek_tok();
        NODE *arg = NULL;
        if (nx.kind != TK_SEMI && nx.kind != TK_NL && nx.kind != TK_RBRACE && nx.kind != TK_EOF) {
            arg = parse_expr();
        }
        return ALLOC_arawk_node_exit(arg);
      }
      case TK_LBRACE:
        return parse_block();
      default:
        return parse_expr();
    }
}

// ---------------------------------------------------------------------------
// Program-level: items are BEGIN / END / pattern-action.
// ---------------------------------------------------------------------------

NODE *
PARSE_SOURCE(const char *source)
{
    src = source;
    p = source;
    paren_depth = 0;
    line_no = 1;
    have_lookahead = false;
    pushback_cnt = 0;

    globals_init();

    NODE *defs_acc  = NULL;     // user `function f(...) { ... }` defs
    NODE *begin_acc = NULL;
    NODE *main_acc  = NULL;
    NODE *end_acc   = NULL;

    skip_terminators();
    while (peek_tok().kind != TK_EOF) {
        Token la = peek_tok();
        if (la.kind == TK_KW_BEGIN) {
            (void)take_tok();
            NODE *b = parse_block();
            begin_acc = begin_acc ? ALLOC_arawk_node_seq(begin_acc, b) : b;
        }
        else if (la.kind == TK_KW_END) {
            (void)take_tok();
            NODE *b = parse_block();
            end_acc = end_acc ? ALLOC_arawk_node_seq(end_acc, b) : b;
        }
        else if (la.kind == TK_KW_FUNCTION) {
            // `function NAME ( params ) { body }` — parse with a
            // fresh LocalScope.  Params occupy slots 0..params-1;
            // any further locals declared by use inside the body
            // grow the same scope.
            (void)take_tok();
            Token nm = take_tok();
            if (nm.kind != TK_NAME) parse_error("function: expected name");
            char *name = intern_string(nm.start, nm.len);
            expect(TK_LPAREN, "(");
            LocalScope scope = { .count = 0, .params_cnt = 0 };
            LocalScope *prev = cur_scope;
            cur_scope = &scope;
            if (peek_tok().kind != TK_RPAREN) {
                for (;;) {
                    Token p = take_tok();
                    if (p.kind != TK_NAME) parse_error("function: expected parameter name");
                    char *pname = intern_string(p.start, p.len);
                    scope_add_local(&scope, pname);
                    if (peek_tok().kind != TK_COMMA) break;
                    (void)take_tok();
                }
            }
            scope.params_cnt = scope.count;
            expect(TK_RPAREN, ")");
            skip_terminators();
            NODE *body = parse_block();
            cur_scope = prev;
            NODE *def = ALLOC_arawk_node_def(name, body, scope.count, scope.params_cnt);
            defs_acc = defs_acc ? ALLOC_arawk_node_seq(defs_acc, def) : def;
        }
        else if (la.kind == TK_LBRACE) {
            // Patternless action.
            NODE *b = parse_block();
            main_acc = main_acc ? ALLOC_arawk_node_seq(main_acc, b) : b;
        }
        else {
            // Pattern-action: expr [block].  No-block form `expr`
            // means default action = `{ print }`.  Phase 1.
            NODE *pat = parse_expr();
            skip_terminators();
            NODE *body;
            if (peek_tok().kind == TK_LBRACE) {
                body = parse_block();
            }
            else {
                body = ALLOC_arawk_node_print(0, 0);
            }
            NODE *guarded = ALLOC_arawk_node_if(pat, body, ALLOC_arawk_node_int(0));
            main_acc = main_acc ? ALLOC_arawk_node_seq(main_acc, guarded) : guarded;
        }
        skip_terminators();
    }

    // Function defs run before BEGIN — they merely register the body
    // in c->func_set, so the order is essentially "register all, then
    // start program proper".  Splice into the begin branch.
    if (defs_acc) {
        if (begin_acc) begin_acc = ALLOC_arawk_node_seq(defs_acc, begin_acc);
        else           begin_acc = defs_acc;
    }

    // The framework pre-fetches `head.dispatcher` for every NODE *
    // operand, so we hand `arawk_node_program` non-null branches and use
    // `arawk_node_noop` as the Null object for absent parts.  The main
    // action is wrapped in `arawk_node_main_loop` to drive the input loop;
    // BEGIN-only / END-only programs pass `arawk_node_noop` and so never
    // touch stdin.
    NODE *begin_part = begin_acc ? begin_acc : ALLOC_arawk_node_noop();
    NODE *main_part  = main_acc  ? ALLOC_arawk_node_main_loop(main_acc) : ALLOC_arawk_node_noop();
    NODE *end_part   = end_acc   ? end_acc   : ALLOC_arawk_node_noop();
    return ALLOC_arawk_node_program(begin_part, main_part, end_part);
}
