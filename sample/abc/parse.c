// Recursive-descent parser + tokenizer for abc (a bc dialect).
//
// bc treats newlines as statement separators, except inside ( ) and [ ]
// where they are insignificant; the tokenizer suppresses newlines while
// bracket depth > 0 and honours backslash-newline continuations.  The
// parser uses a token array with an index so it can backtrack (needed
// for the dangling `else`).
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <gc.h>
#include "context.h"
#include "node.h"
#include "parse.h"

// Node kinds we need to recognise structurally (lvalues / assignments).
extern const struct NodeKind kind_node_var;
extern const struct NodeKind kind_node_aref;

// ---- tokens ----------------------------------------------------------

enum tok_type {
    T_EOF, T_NL, T_NUM, T_ID, T_STR,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT, T_CARET,
    T_LP, T_RP, T_LB, T_RB, T_LC, T_RC,
    T_SEMI, T_COMMA,
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PCTEQ, T_CARETEQ,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_AND, T_OR, T_NOT,
    T_INCR, T_DECR,
    T_DOT,
};

typedef struct {
    enum tok_type type;
    char *text;   // for NUM / ID / STR
    int line;
} Token;

// ---- parser state ----------------------------------------------------

typedef struct {
    Token *toks;
    int ntok, cap;
    int pos;            // current token index
    CTX *c;
    int error;          // set on syntax error
    jmp_buf errjmp;
    int line;
} Parser;

static char *
xstrndup(const char *s, size_t n)
{
    char *r = (char *)malloc(n + 1);
    if (!r) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(r, s, n); r[n] = '\0';
    return r;
}
static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

static void
parse_fail(Parser *p, const char *msg)
{
    fprintf(stderr, "abc: syntax error near line %d: %s\n", p->line, msg);
    p->error = 1;
    longjmp(p->errjmp, 1);
}

// ---- tokenizer -------------------------------------------------------

static void
push_tok(Parser *p, enum tok_type t, char *text, int line)
{
    if (p->ntok == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 128;
        p->toks = (Token *)realloc(p->toks, sizeof(Token) * p->cap);
    }
    p->toks[p->ntok].type = t;
    p->toks[p->ntok].text = text;
    p->toks[p->ntok].line = line;
    p->ntok++;
}

static int is_num_char(int ch) { return isdigit(ch) || (ch >= 'A' && ch <= 'Z') || ch == '.'; }

static void
tokenize(Parser *p, const char *src)
{
    const char *s = src;
    int depth = 0;     // ( ) [ ] nesting
    int line = 1;
    while (*s) {
        char ch = *s;
        if (ch == '\\' && s[1] == '\n') { s += 2; line++; continue; }   // continuation
        if (ch == '\n') { if (depth <= 0) push_tok(p, T_NL, NULL, line); line++; s++; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\r') { s++; continue; }
        if (ch == '#') { while (*s && *s != '\n') s++; continue; }      // line comment
        if (ch == '/' && s[1] == '*') {                                  // block comment
            s += 2;
            while (*s && !(*s == '*' && s[1] == '/')) { if (*s == '\n') line++; s++; }
            if (*s) s += 2;
            continue;
        }
        if (ch == '"') {                                                 // string (stored raw)
            // bc strings are stored verbatim; escapes are interpreted only
            // by `print` (a bare string statement outputs them literally).
            // There is no \" escape — the first '"' ends the string.
            s++;
            const char *start = s;
            while (*s && *s != '"') { if (*s == '\n') line++; s++; }
            char *text = xstrndup(start, s - start);
            if (*s == '"') s++;
            push_tok(p, T_STR, text, line);
            continue;
        }
        if (isdigit((unsigned char)ch) || (ch >= 'A' && ch <= 'Z')) {    // number
            const char *start = s;
            while (is_num_char((unsigned char)*s)) s++;
            push_tok(p, T_NUM, xstrndup(start, s - start), line);
            continue;
        }
        if (ch == '.') {
            if (is_num_char((unsigned char)s[1]) && s[1] != '.') {       // .5 etc
                const char *start = s; s++;
                while (is_num_char((unsigned char)*s)) s++;
                push_tok(p, T_NUM, xstrndup(start, s - start), line);
            }
            else { push_tok(p, T_DOT, NULL, line); s++; }               // last value
            continue;
        }
        if (islower((unsigned char)ch)) {                                // identifier / keyword
            const char *start = s;
            while (islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '_') s++;
            push_tok(p, T_ID, xstrndup(start, s - start), line);
            continue;
        }
        // operators (maximal munch)
        switch (ch) {
          case '+': if (s[1]=='+'){push_tok(p,T_INCR,0,line);s+=2;} else if (s[1]=='='){push_tok(p,T_PLUSEQ,0,line);s+=2;} else {push_tok(p,T_PLUS,0,line);s++;} break;
          case '-': if (s[1]=='-'){push_tok(p,T_DECR,0,line);s+=2;} else if (s[1]=='='){push_tok(p,T_MINUSEQ,0,line);s+=2;} else {push_tok(p,T_MINUS,0,line);s++;} break;
          case '*': if (s[1]=='='){push_tok(p,T_STAREQ,0,line);s+=2;} else {push_tok(p,T_STAR,0,line);s++;} break;
          case '/': if (s[1]=='='){push_tok(p,T_SLASHEQ,0,line);s+=2;} else {push_tok(p,T_SLASH,0,line);s++;} break;
          case '%': if (s[1]=='='){push_tok(p,T_PCTEQ,0,line);s+=2;} else {push_tok(p,T_PCT,0,line);s++;} break;
          case '^': if (s[1]=='='){push_tok(p,T_CARETEQ,0,line);s+=2;} else {push_tok(p,T_CARET,0,line);s++;} break;
          case '=': if (s[1]=='='){push_tok(p,T_EQ,0,line);s+=2;} else {push_tok(p,T_ASSIGN,0,line);s++;} break;
          case '!': if (s[1]=='='){push_tok(p,T_NE,0,line);s+=2;} else {push_tok(p,T_NOT,0,line);s++;} break;
          case '<': if (s[1]=='='){push_tok(p,T_LE,0,line);s+=2;} else {push_tok(p,T_LT,0,line);s++;} break;
          case '>': if (s[1]=='='){push_tok(p,T_GE,0,line);s+=2;} else {push_tok(p,T_GT,0,line);s++;} break;
          case '&': if (s[1]=='&'){push_tok(p,T_AND,0,line);s+=2;} else {fprintf(stderr,"abc: stray '&' on line %d\n",line);s++;} break;
          case '|': if (s[1]=='|'){push_tok(p,T_OR,0,line);s+=2;} else {fprintf(stderr,"abc: stray '|' on line %d\n",line);s++;} break;
          case '(': push_tok(p,T_LP,0,line); depth++; s++; break;
          case ')': push_tok(p,T_RP,0,line); if(depth>0)depth--; s++; break;
          case '[': push_tok(p,T_LB,0,line); depth++; s++; break;
          case ']': push_tok(p,T_RB,0,line); if(depth>0)depth--; s++; break;
          case '{': push_tok(p,T_LC,0,line); s++; break;
          case '}': push_tok(p,T_RC,0,line); s++; break;
          case ';': push_tok(p,T_SEMI,0,line); s++; break;
          case ',': push_tok(p,T_COMMA,0,line); s++; break;
          default: fprintf(stderr,"abc: illegal character '%c' on line %d\n", ch, line); s++; break;
        }
    }
    push_tok(p, T_EOF, NULL, line);
}

// ---- token cursor helpers -------------------------------------------

static Token *cur(Parser *p) { return &p->toks[p->pos]; }
static enum tok_type peek(Parser *p) { return p->toks[p->pos].type; }
static Token *advance(Parser *p) { Token *t = &p->toks[p->pos]; if (p->toks[p->pos].type != T_EOF) p->pos++; p->line = t->line; return t; }
static int accept(Parser *p, enum tok_type t) { if (peek(p) == t) { advance(p); return 1; } return 0; }
static void expect(Parser *p, enum tok_type t, const char *what) { if (!accept(p, t)) parse_fail(p, what); }
static void skip_nl(Parser *p) { while (peek(p) == T_NL) advance(p); }

static int id_is(Parser *p, const char *kw) { return peek(p) == T_ID && strcmp(cur(p)->text, kw) == 0; }

// Shared sentinel node for empty branches / list ends.
static NODE *
nil_node(void)
{
    static NODE *n = NULL;
    if (!n) n = ALLOC_node_nil();
    return n;
}

// ---- expression parsing ---------------------------------------------

static NODE *parse_expr(Parser *p);
static NODE *parse_stmt(Parser *p);

static int g_is_assign;   // did the most recent parse_expr build a top-level assignment?

static int is_lvalue(NODE *n) { return n->head.kind == &kind_node_var || n->head.kind == &kind_node_aref; }

static NODE *
make_incdec(Parser *p, NODE *lv, int pre, int dec)
{
    const int32_t flags = (pre ? 1 : 0) | (dec ? 2 : 0);
    if (lv->head.kind == &kind_node_var)
        return ALLOC_node_incdec(lv->u.node_var.name, flags);
    if (lv->head.kind == &kind_node_aref)
        return ALLOC_node_aincdec(lv->u.node_aref.name, lv->u.node_aref.idx, flags);
    parse_fail(p, "++/-- requires a variable");
    return nil_node();
}

static NODE *
parse_primary(Parser *p)
{
    g_is_assign = 0;
    Token *t = cur(p);
    switch (t->type) {
      case T_NUM: advance(p); return ALLOC_node_num(t->text);
      case T_DOT: advance(p); return ALLOC_node_var(xstrdup("last"));
      case T_LP: {
        advance(p);
        NODE *e = parse_expr(p);
        expect(p, T_RP, "expected ')'");
        g_is_assign = 0;   // parenthesised: never a top-level assignment
        return e;
      }
      case T_ID: {
        char *name = t->text;
        advance(p);
        if (peek(p) == T_LP) {              // call or builtin
            advance(p);
            if (strcmp(name, "sqrt") == 0)   { NODE *e = parse_expr(p); expect(p, T_RP, "expected ')'"); return ALLOC_node_sqrt(e); }
            if (strcmp(name, "length") == 0) { NODE *e = parse_expr(p); expect(p, T_RP, "expected ')'"); return ALLOC_node_length(e); }
            if (strcmp(name, "scale") == 0)  { NODE *e = parse_expr(p); expect(p, T_RP, "expected ')'"); return ALLOC_node_scale_of(e); }
            // user function call: build the arg cons-list
            NODE *args = nil_node();
            if (peek(p) != T_RP) {
                NODE *list[256]; int n = 0;
                do { list[n++] = parse_expr(p); } while (accept(p, T_COMMA) && n < 256);
                for (int i = n - 1; i >= 0; i--) args = ALLOC_node_arg(list[i], args);
            }
            expect(p, T_RP, "expected ')'");
            return ALLOC_node_call(name, args);
        }
        if (peek(p) == T_LB) {              // array reference
            advance(p);
            NODE *idx = parse_expr(p);
            expect(p, T_RB, "expected ']'");
            return ALLOC_node_aref(name, idx);
        }
        return ALLOC_node_var(name);        // scalar / special variable
      }
      default:
        parse_fail(p, "expected expression");
        return nil_node();
    }
}

static NODE *
parse_postfix(Parser *p)
{
    NODE *e = parse_primary(p);
    while (peek(p) == T_INCR || peek(p) == T_DECR) {
        const int dec = (peek(p) == T_DECR);
        if (!is_lvalue(e)) break;           // e.g. `f()++` — leave for caller/error
        advance(p);
        e = make_incdec(p, e, 0, dec);
        g_is_assign = 0;
    }
    return e;
}

static NODE *
parse_unary(Parser *p)
{
    if (peek(p) == T_MINUS) { advance(p); NODE *e = parse_unary(p); g_is_assign = 0; return ALLOC_node_uminus(e); }
    if (peek(p) == T_INCR || peek(p) == T_DECR) {
        const int dec = (peek(p) == T_DECR);
        advance(p);
        NODE *e = parse_unary(p);
        g_is_assign = 0;
        return make_incdec(p, e, 1, dec);
    }
    return parse_postfix(p);
}

static NODE *
parse_pow(Parser *p)
{
    NODE *base = parse_unary(p);
    if (peek(p) == T_CARET) { advance(p); NODE *exp = parse_pow(p); g_is_assign = 0; return ALLOC_node_pow(base, exp); }
    return base;
}

static NODE *
parse_mul(Parser *p)
{
    NODE *l = parse_pow(p);
    for (;;) {
        enum tok_type t = peek(p);
        if (t == T_STAR)      { advance(p); l = ALLOC_node_mul(l, parse_pow(p)); }
        else if (t == T_SLASH){ advance(p); l = ALLOC_node_div(l, parse_pow(p)); }
        else if (t == T_PCT)  { advance(p); l = ALLOC_node_mod(l, parse_pow(p)); }
        else break;
        g_is_assign = 0;
    }
    return l;
}

static NODE *
parse_add(Parser *p)
{
    NODE *l = parse_mul(p);
    for (;;) {
        enum tok_type t = peek(p);
        if (t == T_PLUS)       { advance(p); l = ALLOC_node_add(l, parse_mul(p)); }
        else if (t == T_MINUS) { advance(p); l = ALLOC_node_sub(l, parse_mul(p)); }
        else break;
        g_is_assign = 0;
    }
    return l;
}

static NODE *
parse_rel(Parser *p)
{
    NODE *l = parse_add(p);
    for (;;) {
        enum tok_type t = peek(p);
        if (t == T_LT)      { advance(p); l = ALLOC_node_lt(l, parse_add(p)); }
        else if (t == T_LE) { advance(p); l = ALLOC_node_le(l, parse_add(p)); }
        else if (t == T_GT) { advance(p); l = ALLOC_node_gt(l, parse_add(p)); }
        else if (t == T_GE) { advance(p); l = ALLOC_node_ge(l, parse_add(p)); }
        else if (t == T_EQ) { advance(p); l = ALLOC_node_eq(l, parse_add(p)); }
        else if (t == T_NE) { advance(p); l = ALLOC_node_ne(l, parse_add(p)); }
        else break;
        g_is_assign = 0;
    }
    return l;
}

static NODE *
parse_not(Parser *p)
{
    if (peek(p) == T_NOT) { advance(p); NODE *e = parse_not(p); g_is_assign = 0; return ALLOC_node_lnot(e); }
    return parse_rel(p);
}

static NODE *
parse_and(Parser *p)
{
    NODE *l = parse_not(p);
    while (peek(p) == T_AND) { advance(p); l = ALLOC_node_land(l, parse_not(p)); g_is_assign = 0; }
    return l;
}

static NODE *
parse_or(Parser *p)
{
    NODE *l = parse_and(p);
    while (peek(p) == T_OR) { advance(p); l = ALLOC_node_lor(l, parse_and(p)); g_is_assign = 0; }
    return l;
}

// assignment ops map to a desugared `lvalue = lvalue OP rhs`.
static NODE *
build_binop(enum tok_type op, NODE *l, NODE *r)
{
    switch (op) {
      case T_PLUSEQ:  return ALLOC_node_add(l, r);
      case T_MINUSEQ: return ALLOC_node_sub(l, r);
      case T_STAREQ:  return ALLOC_node_mul(l, r);
      case T_SLASHEQ: return ALLOC_node_div(l, r);
      case T_PCTEQ:   return ALLOC_node_mod(l, r);
      case T_CARETEQ: return ALLOC_node_pow(l, r);
      default:        return r;
    }
}

static NODE *
parse_expr(Parser *p)
{
    NODE *lhs = parse_or(p);
    enum tok_type op = peek(p);
    if (op == T_ASSIGN || op == T_PLUSEQ || op == T_MINUSEQ || op == T_STAREQ ||
        op == T_SLASHEQ || op == T_PCTEQ || op == T_CARETEQ) {
        if (!is_lvalue(lhs)) parse_fail(p, "assignment to non-variable");
        advance(p);
        NODE *rhs = parse_expr(p);                 // right-associative
        NODE *value = (op == T_ASSIGN) ? rhs : build_binop(op, lhs, rhs);
        NODE *result;
        if (lhs->head.kind == &kind_node_var)
            result = ALLOC_node_assign(lhs->u.node_var.name, value);
        else
            result = ALLOC_node_aset(lhs->u.node_aref.name, lhs->u.node_aref.idx, value);
        g_is_assign = 1;
        return result;
    }
    g_is_assign = 0;
    return lhs;
}

// ---- statement parsing ----------------------------------------------

// Parse a brace-delimited or single statement, returning a seq chain.
static NODE *
parse_stmt_list_until(Parser *p, enum tok_type closer)
{
    NODE *stmts[4096]; int n = 0;
    for (;;) {
        while (peek(p) == T_NL || peek(p) == T_SEMI) advance(p);
        if (peek(p) == closer || peek(p) == T_EOF) break;
        if (n >= 4096) parse_fail(p, "block too large");
        stmts[n++] = parse_stmt(p);
    }
    if (n == 0) return nil_node();
    NODE *seq = stmts[n - 1];
    for (int i = n - 2; i >= 0; i--) seq = ALLOC_node_seq(stmts[i], seq);
    return seq;
}

static NODE *
parse_block_or_stmt(Parser *p)
{
    skip_nl(p);
    if (peek(p) == T_LC) {
        advance(p);
        NODE *body = parse_stmt_list_until(p, T_RC);
        expect(p, T_RC, "expected '}'");
        return body;
    }
    return parse_stmt(p);
}

static void
parse_define(Parser *p)
{
    advance(p);                       // 'define'
    if (id_is(p, "void")) advance(p); // GNU void functions: accept, treat as normal
    if (peek(p) != T_ID) parse_fail(p, "expected function name");
    char *fname = cur(p)->text; advance(p);
    expect(p, T_LP, "expected '(' after function name");

    char *params[256]; int param_arr[256]; int nparams = 0;
    if (peek(p) != T_RP) {
        do {
            if (peek(p) != T_ID) parse_fail(p, "expected parameter name");
            params[nparams] = cur(p)->text; advance(p);
            param_arr[nparams] = 0;
            if (accept(p, T_LB)) { expect(p, T_RB, "expected ']'"); param_arr[nparams] = 1; }
            nparams++;
        } while (accept(p, T_COMMA) && nparams < 256);
    }
    expect(p, T_RP, "expected ')'");
    skip_nl(p);
    expect(p, T_LC, "expected '{' to open function body");
    skip_nl(p);

    // auto declarations
    char *autos[256]; int auto_arr[256]; int nautos = 0;
    while (id_is(p, "auto")) {
        advance(p);
        do {
            if (peek(p) != T_ID) parse_fail(p, "expected auto variable name");
            autos[nautos] = cur(p)->text; advance(p);
            auto_arr[nautos] = 0;
            if (accept(p, T_LB)) { expect(p, T_RB, "expected ']'"); auto_arr[nautos] = 1; }
            nautos++;
        } while (accept(p, T_COMMA) && nautos < 256);
        while (peek(p) == T_NL || peek(p) == T_SEMI) advance(p);
    }

    NODE *body = parse_stmt_list_until(p, T_RC);
    expect(p, T_RC, "expected '}' to close function body");

    struct bc_func *f = (struct bc_func *)GC_MALLOC(sizeof(struct bc_func));
    f->name = fname;
    f->nparams = nparams;
    f->params = (const char **)malloc(sizeof(char *) * (nparams ? nparams : 1));
    f->param_isarray = (int *)malloc(sizeof(int) * (nparams ? nparams : 1));
    for (int i = 0; i < nparams; i++) { f->params[i] = params[i]; f->param_isarray[i] = param_arr[i]; }
    f->nautos = nautos;
    f->autos = (const char **)malloc(sizeof(char *) * (nautos ? nautos : 1));
    f->auto_isarray = (int *)malloc(sizeof(int) * (nautos ? nautos : 1));
    for (int i = 0; i < nautos; i++) { f->autos[i] = autos[i]; f->auto_isarray[i] = auto_arr[i]; }
    f->body = body;
    bc_register_func(p->c, f);
}

static NODE *
parse_print(Parser *p)
{
    advance(p);   // 'print'
    NODE *items[1024]; int is_str[1024]; char *strs[1024]; int n = 0;
    do {
        if (peek(p) == T_STR) { is_str[n] = 1; strs[n] = cur(p)->text; items[n] = nil_node(); advance(p); }
        else                  { is_str[n] = 0; strs[n] = (char *)""; items[n] = parse_expr(p); }
        n++;
    } while (accept(p, T_COMMA) && n < 1024);

    NODE *chain = nil_node();
    for (int i = n - 1; i >= 0; i--)
        chain = ALLOC_node_print_item(is_str[i], strs[i], items[i], chain);
    return chain;
}

static NODE *
parse_stmt(Parser *p)
{
    Token *t = cur(p);

    if (t->type == T_LC) {            // { ... }
        advance(p);
        NODE *body = parse_stmt_list_until(p, T_RC);
        expect(p, T_RC, "expected '}'");
        return body;
    }
    if (t->type == T_STR) {           // bare string: print without newline
        advance(p);
        return ALLOC_node_str(t->text);
    }
    if (t->type == T_ID) {
        const char *kw = t->text;
        if (strcmp(kw, "if") == 0) {
            advance(p); expect(p, T_LP, "expected '(' after if");
            NODE *cond = parse_expr(p); expect(p, T_RP, "expected ')'");
            NODE *then = parse_block_or_stmt(p);
            NODE *els = nil_node();
            int save = p->pos;        // allow optional newlines before else, else backtrack
            skip_nl(p);
            if (id_is(p, "else")) { advance(p); els = parse_block_or_stmt(p); }
            else p->pos = save;
            return ALLOC_node_if(cond, then, els);
        }
        if (strcmp(kw, "while") == 0) {
            advance(p); expect(p, T_LP, "expected '(' after while");
            NODE *cond = parse_expr(p); expect(p, T_RP, "expected ')'");
            NODE *body = parse_block_or_stmt(p);
            return ALLOC_node_while(cond, body);
        }
        if (strcmp(kw, "for") == 0) {
            advance(p); expect(p, T_LP, "expected '(' after for");
            NODE *init = (peek(p) == T_SEMI) ? nil_node() : parse_expr(p);
            expect(p, T_SEMI, "expected ';' in for");
            NODE *cond = (peek(p) == T_SEMI) ? ALLOC_node_num(xstrdup("1")) : parse_expr(p);
            expect(p, T_SEMI, "expected ';' in for");
            NODE *step = (peek(p) == T_RP) ? nil_node() : parse_expr(p);
            expect(p, T_RP, "expected ')'");
            NODE *body = parse_block_or_stmt(p);
            return ALLOC_node_for(init, cond, step, body);
        }
        if (strcmp(kw, "break") == 0)    { advance(p); return ALLOC_node_break(); }
        if (strcmp(kw, "continue") == 0) { advance(p); return ALLOC_node_continue(); }
        if (strcmp(kw, "halt") == 0 || strcmp(kw, "quit") == 0) { advance(p); return ALLOC_node_halt(); }
        if (strcmp(kw, "return") == 0) {
            advance(p);
            NODE *e = nil_node();
            if (peek(p) == T_LP) {            // return ( expr )  or  return ()
                advance(p);
                if (peek(p) != T_RP) e = parse_expr(p);
                expect(p, T_RP, "expected ')'");
            }
            else if (peek(p) != T_NL && peek(p) != T_SEMI && peek(p) != T_RC && peek(p) != T_EOF) {
                e = parse_expr(p);
            }
            return ALLOC_node_return(e);
        }
        if (strcmp(kw, "print") == 0) return parse_print(p);
        if (strcmp(kw, "define") == 0) { parse_define(p); return nil_node(); }
        // otherwise: fall through to expression statement
    }

    // expression statement
    NODE *e = parse_expr(p);
    if (g_is_assign) return e;             // bare assignment: no auto-print
    return ALLOC_node_autoprint(e);
}

// ---- entry points ----------------------------------------------------

Program
parse_program(CTX *c, const char *src)
{
    Parser p; memset(&p, 0, sizeof(p));
    p.c = c; p.line = 1;
    Program prog = { NULL, 0 };

    if (setjmp(p.errjmp)) { free(p.toks); prog.count = -1; return prog; }

    tokenize(&p, src);

    int cap = 0;
    for (;;) {
        while (peek(&p) == T_NL || peek(&p) == T_SEMI) advance(&p);
        if (peek(&p) == T_EOF) break;
        NODE *st = parse_stmt(&p);
        if (prog.count == cap) { cap = cap ? cap * 2 : 64; prog.stmts = (NODE **)realloc(prog.stmts, sizeof(NODE *) * cap); }
        prog.stmts[prog.count++] = st;
    }
    free(p.toks);
    return prog;
}

NODE *
program_to_root(const Program *p)
{
    if (p->count <= 0) return nil_node();
    NODE *seq = p->stmts[p->count - 1];
    for (int i = p->count - 2; i >= 0; i--) seq = ALLOC_node_seq(p->stmts[i], seq);
    return seq;
}
