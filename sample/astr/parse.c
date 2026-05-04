// astr parser — recursive-descent + Pratt expression parser.
//
// Statement separators: both `;` and newline at brace/paren depth 0
// terminate a statement.  At positive depth, newlines are whitespace.
//
// Functions: `name <- function(args) body` is recognized at statement
// scope and lowers to a `node_def` directly.  All other `<-` / `=`
// uses are local-variable assignments (the LHS is the bound name).
//
// Variables vs function names: a NAME followed by `(` is a call site;
// otherwise it's a local variable lookup.  Locals are slotted into a
// per-function frame in left-to-right first-mention order.
//
// Variadic calls: arguments beyond the 3-arg arity-specialised forms
// are packed into ASTR_NODE_TABLE (a parser-managed flat array of
// NODE pointers) and the call site stores (idx, argc) instead of
// inline operand fields.

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "context.h"
#include "node.h"

// ---------------------------------------------------------------------------
// Side tables.  The tokenizer pulls ASTR_NODE_TABLE from a growable
// array; the runtime sees only the pointer + length so no per-call
// realloc is observable.
// ---------------------------------------------------------------------------

NODE     **ASTR_NODE_TABLE     = NULL;
uint32_t   ASTR_NODE_TABLE_LEN = 0;
static uint32_t astr_node_table_capa = 0;

static uint32_t
astr_node_table_push_n(NODE **items, uint32_t n)
{
    if (ASTR_NODE_TABLE_LEN + n > astr_node_table_capa) {
        uint32_t capa = astr_node_table_capa ? astr_node_table_capa * 2 : 16;
        while (capa < ASTR_NODE_TABLE_LEN + n) capa *= 2;
        ASTR_NODE_TABLE = (NODE **)realloc(ASTR_NODE_TABLE, sizeof(NODE *) * capa);
        astr_node_table_capa = capa;
    }
    uint32_t base = ASTR_NODE_TABLE_LEN;
    for (uint32_t i = 0; i < n; i++) ASTR_NODE_TABLE[base + i] = items[i];
    ASTR_NODE_TABLE_LEN += n;
    return base;
}

// ---------------------------------------------------------------------------
// Tokenizer.
// ---------------------------------------------------------------------------

typedef enum {
    TK_EOF, TK_NL, TK_SEMI,
    TK_INT, TK_FLOAT, TK_NAME, TK_STRING,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK,
    TK_COMMA,
    TK_ASSIGN_LEFT,         // <-
    TK_ASSIGN_EQ,           // =
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_CARET,
    TK_MOD,                 // %%
    TK_IDIV,                // %/%
    TK_COLON,
    TK_LT, TK_LE, TK_GT, TK_GE, TK_EQ, TK_NE,
    TK_NOT, TK_AND, TK_OR,
    TK_KW_IF, TK_KW_ELSE, TK_KW_WHILE, TK_KW_FOR, TK_KW_IN,
    TK_KW_FUNCTION, TK_KW_RETURN, TK_KW_TRUE, TK_KW_FALSE,
    TK_KW_NULL, TK_KW_NA,
} TokKind;

typedef struct {
    TokKind kind;
    const char *start;
    int len;
    int64_t  inum;          // valid when kind == TK_INT
    double   fnum;          // valid when kind == TK_FLOAT
    char    *str;           // valid when kind == TK_STRING (heap, NUL-terminated)
    int      str_len;
    int      line;
} Token;

static const char *src;
static const char *p;
static int paren_depth;
static int line_no = 1;

static struct {
    const char *kw;
    TokKind kind;
} keywords[] = {
    { "if",       TK_KW_IF },
    { "else",     TK_KW_ELSE },
    { "while",    TK_KW_WHILE },
    { "for",      TK_KW_FOR },
    { "in",       TK_KW_IN },
    { "function", TK_KW_FUNCTION },
    { "return",   TK_KW_RETURN },
    { "TRUE",     TK_KW_TRUE },
    { "T",        TK_KW_TRUE },
    { "FALSE",    TK_KW_FALSE },
    { "F",        TK_KW_FALSE },
    { "NULL",     TK_KW_NULL },
    { "NA",       TK_KW_NA },
    { NULL, 0 }
};

static Token next_tok;
static bool have_lookahead = false;
static Token saved_tok;
static bool have_saved = false;

static __attribute__((noreturn,format(printf,1,2))) void
parse_error(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "astr: parse error at line %d: ", line_no);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static void
skip_to_eol(void)
{
    while (*p && *p != '\n') p++;
}

static char *
lex_string(char quote, int *out_len)
{
    p++;        // skip opening quote
    char *buf = (char *)malloc(64);
    size_t cap = 64, len = 0;
    while (*p && *p != quote) {
        if (len + 4 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        if (*p == '\\' && p[1]) {
            p++;
            char esc = *p++;
            switch (esc) {
              case 'n': buf[len++] = '\n'; break;
              case 't': buf[len++] = '\t'; break;
              case 'r': buf[len++] = '\r'; break;
              case '\\': buf[len++] = '\\'; break;
              case '"':  buf[len++] = '"';  break;
              case '\'': buf[len++] = '\''; break;
              case '0':  buf[len++] = '\0'; break;
              default:
                buf[len++] = '\\';
                buf[len++] = esc;
                break;
            }
            continue;
        }
        if (*p == '\n') line_no++;
        buf[len++] = *p++;
    }
    if (*p == quote) p++;
    else parse_error("unterminated string literal");
    buf[len] = '\0';
    *out_len = (int)len;
    return buf;
}

static Token
read_token_inner(void)
{
    Token t;
    memset(&t, 0, sizeof(t));

    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }
        if (*p == '#') { skip_to_eol(); continue; }
        if (*p == '\\' && p[1] == '\n') { p += 2; line_no++; continue; }
        if (*p == '\n') {
            if (paren_depth > 0) { p++; line_no++; continue; }
            t.kind = TK_NL; t.start = p; t.len = 1; t.line = line_no;
            p++; line_no++;
            return t;
        }
        break;
    }

    t.start = p;
    t.line = line_no;

    if (*p == '\0') { t.kind = TK_EOF; t.len = 0; return t; }

    if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
        // Decide integer vs float by peeking: if we encounter `.`, `e`,
        // or `E` before a non-digit, treat as float.  R itself defaults
        // to double for `1`, but our fixnum fast-path saves enough on
        // benchmarks that integer literals are integer-typed (with the
        // `L` suffix being a no-op as in R).
        const char *s = p;
        bool is_float = false;
        while (isdigit((unsigned char)*p)) p++;
        if (*p == '.') { is_float = true; p++; while (isdigit((unsigned char)*p)) p++; }
        if (*p == 'e' || *p == 'E') {
            is_float = true;
            p++;
            if (*p == '+' || *p == '-') p++;
            while (isdigit((unsigned char)*p)) p++;
        }
        char saved_p = *p;
        bool int_suffix = (saved_p == 'L');
        bool complex_suffix = (saved_p == 'i');
        if (int_suffix || complex_suffix) p++;

        if (is_float || (!int_suffix && (saved_p == '.' /* unreachable but defensive */))) {
            char buf[64];
            size_t n = (size_t)(p - s) - (int_suffix || complex_suffix ? 1 : 0);
            if (n >= sizeof(buf)) parse_error("numeric literal too long");
            memcpy(buf, s, n);
            buf[n] = '\0';
            t.fnum = strtod(buf, NULL);
            t.kind = TK_FLOAT;
        }
        else {
            // Parse as int64 — overflow ⇒ fall back to float.
            int64_t v = 0;
            bool overflow = false;
            for (const char *q = s; q < p; q++) {
                if (!isdigit((unsigned char)*q)) break;
                int d = *q - '0';
                if (v > (INT64_MAX - d) / 10) { overflow = true; break; }
                v = v * 10 + d;
            }
            if (overflow) {
                char buf[64];
                size_t n = (size_t)(p - s) - (int_suffix ? 1 : 0);
                memcpy(buf, s, n);
                buf[n] = '\0';
                t.fnum = strtod(buf, NULL);
                t.kind = TK_FLOAT;
            }
            else {
                t.inum = v;
                t.kind = TK_INT;
            }
        }
        t.len = (int)(p - s);
        return t;
    }

    if (isalpha((unsigned char)*p) || *p == '_' || *p == '.') {
        const char *s = p;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '.') p++;
        t.len = (int)(p - s);
        for (int i = 0; keywords[i].kw; i++) {
            if ((int)strlen(keywords[i].kw) == t.len &&
                strncmp(s, keywords[i].kw, t.len) == 0) {
                t.kind = keywords[i].kind;
                return t;
            }
        }
        t.kind = TK_NAME;
        return t;
    }

    char c = *p;
    switch (c) {
      case '(':  p++; paren_depth++; t.kind = TK_LPAREN; t.len = 1; return t;
      case ')':  p++; if (paren_depth > 0) paren_depth--; t.kind = TK_RPAREN; t.len = 1; return t;
      case '{':  p++; t.kind = TK_LBRACE; t.len = 1; return t;
      case '}':  p++; t.kind = TK_RBRACE; t.len = 1; return t;
      case '[':  p++; paren_depth++; t.kind = TK_LBRACK; t.len = 1; return t;
      case ']':  p++; if (paren_depth > 0) paren_depth--; t.kind = TK_RBRACK; t.len = 1; return t;
      case ',':  p++; t.kind = TK_COMMA; t.len = 1; return t;
      case ';':  p++; t.kind = TK_SEMI;  t.len = 1; return t;
      case '+':  p++; t.kind = TK_PLUS;  t.len = 1; return t;
      case '-':
        if (p[1] == '>') parse_error("'->' not supported");
        p++; t.kind = TK_MINUS; t.len = 1; return t;
      case '*':  p++; t.kind = TK_STAR;  t.len = 1; return t;
      case '/':  p++; t.kind = TK_SLASH; t.len = 1; return t;
      case '^':  p++; t.kind = TK_CARET; t.len = 1; return t;
      case ':':  p++; t.kind = TK_COLON; t.len = 1; return t;
      case '<':
        if (p[1] == '-') { p += 2; t.kind = TK_ASSIGN_LEFT; t.len = 2; return t; }
        if (p[1] == '=') { p += 2; t.kind = TK_LE; t.len = 2; return t; }
        p++; t.kind = TK_LT; t.len = 1; return t;
      case '>':
        if (p[1] == '=') { p += 2; t.kind = TK_GE; t.len = 2; return t; }
        p++; t.kind = TK_GT; t.len = 1; return t;
      case '=':
        if (p[1] == '=') { p += 2; t.kind = TK_EQ; t.len = 2; return t; }
        p++; t.kind = TK_ASSIGN_EQ; t.len = 1; return t;
      case '!':
        if (p[1] == '=') { p += 2; t.kind = TK_NE; t.len = 2; return t; }
        p++; t.kind = TK_NOT; t.len = 1; return t;
      case '&':
        if (p[1] == '&') { p += 2; t.kind = TK_AND; t.len = 2; return t; }
        p++; t.kind = TK_AND; t.len = 1; return t;
      case '|':
        if (p[1] == '|') { p += 2; t.kind = TK_OR; t.len = 2; return t; }
        p++; t.kind = TK_OR; t.len = 1; return t;
      case '%':
        if (p[1] == '%') { p += 2; t.kind = TK_MOD; t.len = 2; return t; }
        if (p[1] == '/' && p[2] == '%') { p += 3; t.kind = TK_IDIV; t.len = 3; return t; }
        parse_error("unsupported %% operator form");
      case '"': case '\'': {
        t.str = lex_string(c, &t.str_len);
        t.kind = TK_STRING;
        t.len = 0;
        return t;
      }
    }

    parse_error("unexpected character: '%c' (0x%02x)", c, (unsigned char)c);
    (void)src;
}

static Token peek_tok(void)
{
    if (have_saved) return saved_tok;
    if (!have_lookahead) {
        next_tok = read_token_inner();
        have_lookahead = true;
    }
    return next_tok;
}

static Token take_tok(void)
{
    if (have_saved) { have_saved = false; return saved_tok; }
    if (have_lookahead) { have_lookahead = false; return next_tok; }
    return read_token_inner();
}

static void unread_tok(Token t) { saved_tok = t; have_saved = true; }

static void
skip_terminators(void)
{
    for (;;) {
        Token t = peek_tok();
        if (t.kind == TK_NL || t.kind == TK_SEMI) (void)take_tok();
        else break;
    }
}

static void
expect(TokKind k, const char *what)
{
    Token t = take_tok();
    if (t.kind != k) parse_error("expected %s", what);
}

// ---------------------------------------------------------------------------
// Local-variable scope tracking.
// ---------------------------------------------------------------------------

#define MAX_LOCALS 256

typedef struct LocalScope {
    const char *names[MAX_LOCALS];
    uint32_t count;
    struct LocalScope *parent;
} LocalScope;

static LocalScope *cur_scope = NULL;

static int
scope_lookup(LocalScope *s, const char *name, int len)
{
    if (!s) return -1;
    for (uint32_t i = 0; i < s->count; i++) {
        if ((int)strlen(s->names[i]) == len &&
            strncmp(s->names[i], name, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static char *
intern_string(const char *s, int len)
{
    char *copy = (char *)malloc(len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

static uint32_t
scope_intern_str(LocalScope *s, const char *name)
{
    int len = (int)strlen(name);
    int idx = scope_lookup(s, name, len);
    if (idx >= 0) return (uint32_t)idx;
    if (s->count >= MAX_LOCALS) parse_error("too many locals (max %d)", MAX_LOCALS);
    s->names[s->count] = name;
    return s->count++;
}

// ---------------------------------------------------------------------------
// Builtin registry.  Built-ins come in two flavours: arity-specialised
// (0..3 args inline) and variadic (>= 4 args via ASTR_NODE_TABLE).
// astr_builtin_arity == -1 means variadic.
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    void *func;
    int arity;          // -1 ⇒ variadic (VALUE *args, size_t n)
} BuiltinDecl;

extern const BuiltinDecl *astr_builtins;
extern unsigned int       astr_builtin_count;

static const BuiltinDecl *
lookup_builtin(const char *name, int len)
{
    for (unsigned int i = 0; i < astr_builtin_count; i++) {
        if ((int)strlen(astr_builtins[i].name) == len &&
            strncmp(astr_builtins[i].name, name, len) == 0) {
            return &astr_builtins[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Pratt expression parser.
// ---------------------------------------------------------------------------

typedef enum {
    P_NONE = 0,
    P_OR,
    P_AND,
    P_CMP,
    P_RANGE,            // : (used inside expressions; produces an int vector)
    P_ADD,
    P_MUL,
    P_POW,
    P_UNARY,
    P_INDEX,
} Prec;

static Prec
infix_prec(TokKind k)
{
    switch (k) {
      case TK_OR:        return P_OR;
      case TK_AND:       return P_AND;
      case TK_LT: case TK_LE: case TK_GT: case TK_GE:
      case TK_EQ: case TK_NE: return P_CMP;
      case TK_COLON:     return P_RANGE;
      case TK_PLUS: case TK_MINUS: return P_ADD;
      case TK_STAR: case TK_SLASH: case TK_MOD: case TK_IDIV: return P_MUL;
      case TK_CARET:     return P_POW;
      case TK_LBRACK:    return P_INDEX;
      default:           return P_NONE;
    }
}

static NODE *
allocate_int(int64_t v)
{
    if (v >= INT32_MIN && v <= INT32_MAX) return ALLOC_node_int((int32_t)v);
    return ALLOC_node_int64((uint64_t)v);
}

static NODE *
allocate_float(double d)
{
    union { uint64_t u; double dv; } pun = { .dv = d };
    return ALLOC_node_float(pun.u);
}

static NODE *parse_stmt(void);
static NODE *parse_expr(void);
static NODE *parse_block(void);
static NODE *parse_function_lit(const char *name);
static NODE *parse_expr_prec(Prec min_prec);

static bool
right_assoc(TokKind k)
{
    return k == TK_CARET;
}

static NODE *
make_binop(TokKind k, NODE *l, NODE *r)
{
    switch (k) {
      case TK_PLUS:  return ALLOC_node_add(l, r);
      case TK_MINUS: return ALLOC_node_sub(l, r);
      case TK_STAR:  return ALLOC_node_mul(l, r);
      case TK_SLASH: return ALLOC_node_div(l, r);
      case TK_MOD:   return ALLOC_node_mod(l, r);
      case TK_IDIV:  return ALLOC_node_idiv(l, r);
      case TK_CARET: return ALLOC_node_pow(l, r);
      case TK_COLON: return ALLOC_node_range(l, r);
      case TK_LT:    return ALLOC_node_lt(l, r);
      case TK_LE:    return ALLOC_node_le(l, r);
      case TK_GT:    return ALLOC_node_gt(l, r);
      case TK_GE:    return ALLOC_node_ge(l, r);
      case TK_EQ:    return ALLOC_node_eq(l, r);
      case TK_NE:    return ALLOC_node_neq(l, r);
      case TK_AND:   return ALLOC_node_and(l, r);
      case TK_OR:    return ALLOC_node_or(l, r);
      default: parse_error("internal: unhandled binop");
    }
}

static NODE *
parse_call_args_and_make_call(const char *name)
{
    expect(TK_LPAREN, "(");
    NODE *args[64];
    int argc = 0;
    if (peek_tok().kind != TK_RPAREN) {
        for (;;) {
            if (argc >= 64) parse_error("too many call args (>64)");
            // Tolerate `name = expr` keyword args by discarding the name —
            // R's keyword args aren't supported in v0; we just take the
            // expr in positional order so common signatures still parse.
            Token la = peek_tok();
            if (la.kind == TK_NAME) {
                Token name_tok = take_tok();
                if (peek_tok().kind == TK_ASSIGN_EQ) {
                    (void)take_tok();
                    args[argc++] = parse_expr();
                }
                else {
                    unread_tok(name_tok);
                    args[argc++] = parse_expr();
                }
            }
            else {
                args[argc++] = parse_expr();
            }
            Token sep = peek_tok();
            if (sep.kind == TK_COMMA) { (void)take_tok(); continue; }
            break;
        }
    }
    expect(TK_RPAREN, ") after call args");

    const BuiltinDecl *bi = lookup_builtin(name, (int)strlen(name));
    if (bi) {
        if (bi->arity == -1) {
            // variadic builtin — pack into ASTR_NODE_TABLE
            uint32_t base = astr_node_table_push_n(args, (uint32_t)argc);
            return ALLOC_node_call_builtin_n(name, bi->func, base, (uint32_t)argc);
        }
        if (bi->arity != argc) {
            parse_error("builtin '%s' expects %d args, got %d", name, bi->arity, argc);
        }
        switch (argc) {
          case 0: return ALLOC_node_call_builtin0(name, bi->func);
          case 1: return ALLOC_node_call_builtin1(name, bi->func, args[0]);
          case 2: return ALLOC_node_call_builtin2(name, bi->func, args[0], args[1]);
          case 3: return ALLOC_node_call_builtin3(name, bi->func, args[0], args[1], args[2]);
          default: parse_error("builtin arity > 3 must be variadic");
        }
    }

    switch (argc) {
      case 0: return ALLOC_node_call_0(name);
      case 1: return ALLOC_node_call_1(name, args[0]);
      case 2: return ALLOC_node_call_2(name, args[0], args[1]);
      case 3: return ALLOC_node_call_3(name, args[0], args[1], args[2]);
      default: {
        uint32_t base = astr_node_table_push_n(args, (uint32_t)argc);
        return ALLOC_node_call_n(name, base, (uint32_t)argc);
      }
    }
}

static NODE *
parse_primary(void)
{
    Token t = take_tok();
    switch (t.kind) {
      case TK_INT:        return allocate_int(t.inum);
      case TK_FLOAT:      return allocate_float(t.fnum);
      case TK_STRING:     return ALLOC_node_str(t.str);
      case TK_KW_TRUE:    return allocate_int(1);
      case TK_KW_FALSE:   return allocate_int(0);
      case TK_KW_NULL:    return ALLOC_node_null();
      case TK_KW_NA:      return ALLOC_node_na();

      case TK_LPAREN: {
        NODE *e = parse_expr();
        expect(TK_RPAREN, ")");
        return e;
      }
      case TK_LBRACE: {
        unread_tok(t);
        return parse_block();
      }
      case TK_MINUS:      return ALLOC_node_neg(parse_expr_prec(P_UNARY));
      case TK_PLUS:       return parse_expr_prec(P_UNARY);
      case TK_NOT:        return ALLOC_node_not(parse_expr_prec(P_UNARY));

      case TK_KW_IF: {
        expect(TK_LPAREN, "( after `if`");
        NODE *cond = parse_expr();
        expect(TK_RPAREN, ") after if condition");
        skip_terminators();
        NODE *then_n = parse_stmt();
        skip_terminators();
        NODE *else_n;
        if (peek_tok().kind == TK_KW_ELSE) {
            (void)take_tok();
            skip_terminators();
            else_n = parse_stmt();
        }
        else {
            else_n = allocate_int(0);
        }
        return ALLOC_node_if(cond, then_n, else_n);
      }
      case TK_KW_WHILE: {
        expect(TK_LPAREN, "( after `while`");
        NODE *cond = parse_expr();
        expect(TK_RPAREN, ") after while condition");
        skip_terminators();
        NODE *body = parse_stmt();
        return ALLOC_node_while(cond, body);
      }
      case TK_KW_FOR: {
        expect(TK_LPAREN, "( after `for`");
        Token name = take_tok();
        if (name.kind != TK_NAME) parse_error("expected loop variable name");
        expect(TK_KW_IN, "`in`");
        char *iter_name = intern_string(name.start, name.len);
        // Reserve the loop variable's slot up-front so the body sees it.
        uint32_t slot = scope_intern_str(cur_scope, iter_name);
        // Try fast path: `for (i in start:stop)` — peek past `start`'s
        // expression to see if a colon follows.  Otherwise route through
        // the generic vector iterator.
        NODE *first = parse_expr_prec(P_ADD + 1);   // stop before `:`
        if (peek_tok().kind == TK_COLON) {
            (void)take_tok();
            NODE *stop = parse_expr_prec(P_ADD + 1);
            expect(TK_RPAREN, ") after for header");
            skip_terminators();
            NODE *body = parse_stmt();
            return ALLOC_node_for_range(slot, first, stop, body);
        }
        // Generic form: keep parsing the iterable expression at full
        // expression precedence (the partial `first` already covers
        // the `+`/`-` fragment, but we may have stopped early).  Easiest
        // path: if no operator follows, `first` is the whole iterable.
        NODE *iter = first;
        // It's possible we stopped at an op below P_ADD+1 (e.g. `<`,
        // `&&`).  Re-enter the Pratt loop so we pick those up too.
        for (;;) {
            Token op = peek_tok();
            Prec prec = infix_prec(op.kind);
            if (prec == P_NONE || op.kind == TK_COLON) break;
            (void)take_tok();
            Prec next_min = right_assoc(op.kind) ? prec : (Prec)(prec + 1);
            NODE *rhs = parse_expr_prec(next_min);
            iter = make_binop(op.kind, iter, rhs);
        }
        expect(TK_RPAREN, ") after for header");
        skip_terminators();
        NODE *body = parse_stmt();
        return ALLOC_node_for_iter(slot, iter, body);
      }
      case TK_KW_RETURN: {
        NODE *val;
        if (peek_tok().kind == TK_LPAREN) {
            (void)take_tok();
            if (peek_tok().kind == TK_RPAREN) {
                (void)take_tok();
                val = allocate_int(0);
            }
            else {
                val = parse_expr();
                expect(TK_RPAREN, ") after return value");
            }
        }
        else {
            val = allocate_int(0);
        }
        return ALLOC_node_return(val);
      }
      case TK_KW_FUNCTION:
        parse_error("`function` only allowed as RHS of assignment");
      case TK_NAME: {
        char *name = intern_string(t.start, t.len);
        if (peek_tok().kind == TK_LPAREN) {
            return parse_call_args_and_make_call(name);
        }
        int slot = scope_lookup(cur_scope, name, (int)strlen(name));
        if (slot < 0) {
            parse_error("undefined variable `%s`", name);
        }
        return ALLOC_node_lget((uint32_t)slot);
      }
      default:
        parse_error("unexpected token");
    }
}

static NODE *
parse_expr_prec(Prec min_prec)
{
    NODE *lhs = parse_primary();
    for (;;) {
        Token t = peek_tok();
        Prec prec = infix_prec(t.kind);
        if (prec == P_NONE || prec < min_prec) break;

        if (t.kind == TK_LBRACK) {
            // Subscript: `expr[idx]`.
            (void)take_tok();
            NODE *idx = parse_expr();
            expect(TK_RBRACK, "] after subscript");
            lhs = ALLOC_node_index_get(lhs, idx);
            continue;
        }

        TokKind op = t.kind;
        (void)take_tok();
        Prec next_min = right_assoc(op) ? prec : (Prec)(prec + 1);
        NODE *rhs = parse_expr_prec(next_min);
        lhs = make_binop(op, lhs, rhs);
    }
    return lhs;
}

static NODE *
parse_expr(void)
{
    return parse_expr_prec(P_OR);
}

static NODE *
parse_function_lit(const char *name)
{
    expect(TK_LPAREN, "( after `function`");
    LocalScope inner = { .parent = cur_scope, .count = 0 };
    LocalScope *saved = cur_scope;
    cur_scope = &inner;

    uint32_t params_cnt = 0;
    if (peek_tok().kind != TK_RPAREN) {
        for (;;) {
            Token name_tok = take_tok();
            if (name_tok.kind != TK_NAME) parse_error("expected parameter name");
            char *param_name = intern_string(name_tok.start, name_tok.len);
            (void)scope_intern_str(cur_scope, param_name);
            params_cnt++;
            if (peek_tok().kind == TK_ASSIGN_EQ) {
                // R defaults: parse-and-discard for v0.
                (void)take_tok();
                (void)parse_expr();
            }
            if (peek_tok().kind == TK_COMMA) { (void)take_tok(); continue; }
            break;
        }
    }
    expect(TK_RPAREN, ") after function params");
    skip_terminators();

    NODE *body = parse_stmt();
    uint32_t locals_cnt = cur_scope->count;
    cur_scope = saved;

    NODE *def = ALLOC_node_def(name, body, params_cnt, locals_cnt);
    code_repo_add(name, body, true);
    return def;
}

static NODE *
parse_block(void)
{
    expect(TK_LBRACE, "{");
    skip_terminators();
    if (peek_tok().kind == TK_RBRACE) {
        (void)take_tok();
        return allocate_int(0);
    }
    NODE *acc = parse_stmt();
    for (;;) {
        skip_terminators();
        Token t = peek_tok();
        if (t.kind == TK_RBRACE) { (void)take_tok(); break; }
        if (t.kind == TK_EOF) parse_error("unexpected EOF in block");
        NODE *next = parse_stmt();
        acc = ALLOC_node_seq(acc, next);
    }
    return acc;
}

// `name <- expr` / `name = expr` / `name[idx] <- expr` are the assignment
// forms recognised at statement scope.  Anything else is an expression.
static NODE *
parse_stmt(void)
{
    skip_terminators();
    Token la = peek_tok();
    if (la.kind == TK_NAME) {
        Token name_tok = take_tok();
        Token op = peek_tok();
        if (op.kind == TK_ASSIGN_LEFT || op.kind == TK_ASSIGN_EQ) {
            (void)take_tok();
            char *name = intern_string(name_tok.start, name_tok.len);
            if (peek_tok().kind == TK_KW_FUNCTION) {
                (void)take_tok();
                return parse_function_lit(name);
            }
            uint32_t slot = scope_intern_str(cur_scope, name);
            NODE *rhs = parse_expr();
            return ALLOC_node_lset(slot, rhs);
        }
        if (op.kind == TK_LBRACK) {
            // Could be `name[idx] <- val` or just `name[idx]` as an
            // expression.  Peek past the `]` to disambiguate.
            (void)take_tok();
            NODE *idx = parse_expr();
            expect(TK_RBRACK, "] after subscript");
            if (peek_tok().kind == TK_ASSIGN_LEFT || peek_tok().kind == TK_ASSIGN_EQ) {
                (void)take_tok();
                char *name = intern_string(name_tok.start, name_tok.len);
                int slot = scope_lookup(cur_scope, name, (int)strlen(name));
                if (slot < 0) parse_error("undefined variable `%s` in indexed assign", name);
                NODE *val = parse_expr();
                return ALLOC_node_index_set_local((uint32_t)slot, idx, val);
            }
            // Continuation expression: re-build the `name[idx]` get and
            // continue parsing infix operators.
            char *name = intern_string(name_tok.start, name_tok.len);
            int slot = scope_lookup(cur_scope, name, (int)strlen(name));
            if (slot < 0) parse_error("undefined variable `%s`", name);
            NODE *base = ALLOC_node_lget((uint32_t)slot);
            NODE *lhs = ALLOC_node_index_get(base, idx);
            for (;;) {
                Token nx = peek_tok();
                Prec prec = infix_prec(nx.kind);
                if (prec == P_NONE) break;
                if (nx.kind == TK_LBRACK) {
                    (void)take_tok();
                    NODE *i2 = parse_expr();
                    expect(TK_RBRACK, "] after subscript");
                    lhs = ALLOC_node_index_get(lhs, i2);
                    continue;
                }
                (void)take_tok();
                Prec next_min = right_assoc(nx.kind) ? prec : (Prec)(prec + 1);
                NODE *rhs = parse_expr_prec(next_min);
                lhs = make_binop(nx.kind, lhs, rhs);
            }
            return lhs;
        }
        unread_tok(name_tok);
    }
    return parse_expr();
}

NODE *
PARSE_SOURCE(const char *source)
{
    src = source;
    p = source;
    paren_depth = 0;
    line_no = 1;
    have_lookahead = false;
    have_saved = false;

    LocalScope top = { .parent = NULL, .count = 0 };
    cur_scope = &top;

    skip_terminators();
    if (peek_tok().kind == TK_EOF) {
        return allocate_int(0);
    }

    NODE *acc = parse_stmt();
    for (;;) {
        skip_terminators();
        if (peek_tok().kind == TK_EOF) break;
        NODE *next = parse_stmt();
        acc = ALLOC_node_seq(acc, next);
    }

    NODE *root = ALLOC_node_scope(top.count, acc);
    return root;
}
