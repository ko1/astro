// asom: SOM lexer + parser. Recursive-descent over the grammar in
// SOM/specification/SOM.g4, producing ASTro nodes via ALLOC_*.
//
// Scope discipline: variable references are resolved at parse time:
//   - locals (= method args + method locals + nested block args/locals)
//     -> node_local_get / node_local_set with (scope, index)
//     scope=0 is the innermost frame; each outer block adds 1.
//   - instance fields -> node_field_get / node_field_set
//   - everything else -> node_global_get
//
// The parser keeps a stack of "scopes" while it walks a method/class.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "context.h"
#include "node.h"
#include "asom_runtime.h"
#include "asom_parse.h"

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

typedef enum {
    T_EOF = 0,
    T_IDENT,        // identifier
    T_PRIMITIVE,    // 'primitive' keyword
    T_KEYWORD,      // identifier ':'
    T_INTEGER,
    T_DOUBLE,
    T_STRING,
    T_LPAREN,       // (
    T_RPAREN,       // )
    T_LBRACK,       // [
    T_RBRACK,       // ]
    T_PIPE,         // |
    T_POUND,        // #
    T_CARET,        // ^
    T_PERIOD,       // .
    T_ASSIGN,       // :=
    T_COLON,        // :
    T_EQUAL,        // =
    T_SEPARATOR,    // ----
    T_OPSEQ,        // operator sequence (~ & | * / \ + = > < , @ % - and combos)
    T_MINUS,        // -  (we keep separate so unary minus on numbers is easy)
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;   // for diagnostics
    size_t len;
    intptr_t ival;       // T_INTEGER
    double  dval;        // T_DOUBLE
    char *strval;        // T_STRING / T_IDENT / T_KEYWORD / T_OPSEQ (interned for ident/keyword)
    bool   ws_before;    // true if whitespace/comment preceded this token
} Token;

typedef struct {
    const char *src;
    const char *cur;
    const char *file;
    int line;
    Token tok;
} Lexer;

static void
lex_error(Lexer *L, const char *fmt, ...)
{
    fprintf(stderr, "asom: %s:%d: ", L->file, L->line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void lex_advance(Lexer *L);

static void
lex_init(Lexer *L, const char *src, const char *file)
{
    L->src = L->cur = src;
    L->file = file;
    L->line = 1;
    lex_advance(L);
}

static int
is_ident_start(int c) { return isalpha(c) || c == '_'; }
static int
is_ident_cont(int c) { return isalnum(c) || c == '_'; }

static int
is_op_char(int c)
{
    switch (c) {
    case '~': case '&': case '|': case '*': case '/': case '\\':
    case '+': case '=': case '>': case '<': case ',': case '@':
    case '%': case '-':
        return 1;
    default: return 0;
    }
}

static void
lex_skip_ws(Lexer *L)
{
    for (;;) {
        char c = *L->cur;
        if (c == ' ' || c == '\t' || c == '\r') { L->cur++; continue; }
        if (c == '\n') { L->line++; L->cur++; continue; }
        if (c == '"') {
            L->cur++;
            while (*L->cur && *L->cur != '"') {
                if (*L->cur == '\n') L->line++;
                L->cur++;
            }
            if (*L->cur == '"') L->cur++;
            continue;
        }
        return;
    }
}

static char *
lex_intern_range(const char *start, size_t len)
{
    char buf[1024];
    if (len + 1 > sizeof(buf)) {
        char *heap = malloc(len + 1);
        memcpy(heap, start, len);
        heap[len] = '\0';
        const char *interned = asom_intern_cstr(heap);
        free(heap);
        return (char *)interned;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return (char *)asom_intern_cstr(buf);
}

static void
lex_advance(Lexer *L)
{
    const char *before = L->cur;
    lex_skip_ws(L);
    Token *t = &L->tok;
    t->ws_before = (L->cur > before);
    t->start = L->cur;
    char c = *L->cur;

    if (c == '\0') { t->kind = T_EOF; t->len = 0; return; }

    // ---- (4+ hyphens form a separator)
    if (c == '-' && L->cur[1] == '-' && L->cur[2] == '-' && L->cur[3] == '-') {
        const char *p = L->cur;
        while (*p == '-') p++;
        L->cur = p;
        t->kind = T_SEPARATOR; t->len = (size_t)(p - t->start);
        return;
    }

    if (isdigit((unsigned char)c)) {
        const char *p = L->cur;
        while (isdigit((unsigned char)*p)) p++;
        if (*p == '.' && isdigit((unsigned char)p[1])) {
            p++;
            while (isdigit((unsigned char)*p)) p++;
            t->dval = strtod(L->cur, NULL);
            t->kind = T_DOUBLE;
        } else {
            t->ival = strtoll(L->cur, NULL, 10);
            t->kind = T_INTEGER;
        }
        t->len = (size_t)(p - L->cur);
        L->cur = p;
        return;
    }

    if (is_ident_start((unsigned char)c)) {
        const char *p = L->cur + 1;
        while (is_ident_cont((unsigned char)*p)) p++;
        bool is_kw = (*p == ':');
        // strval excludes the trailing ':' for keyword tokens; the parser
        // re-appends it when building the full selector.
        size_t name_len = (size_t)(p - L->cur);
        t->strval = lex_intern_range(L->cur, name_len);
        t->len = name_len;
        if (is_kw) { t->kind = T_KEYWORD; L->cur = p + 1; return; }
        L->cur = p;
        if (strcmp(t->strval, "primitive") == 0) t->kind = T_PRIMITIVE;
        else t->kind = T_IDENT;
        return;
    }

    if (c == '\'') {
        // String literal: read raw bytes (including escapes that may produce
        // NUL bytes) and stash as a heap buffer. We keep the explicit length
        // separate from the buffer so embedded NULs round-trip correctly,
        // unlike asom_intern_cstr which would truncate at strlen().
        L->cur++;
        char *buf = malloc(64); size_t cap = 64, len = 0;
        while (*L->cur && *L->cur != '\'') {
            char ch = *L->cur++;
            if (ch == '\\') {
                char esc = *L->cur++;
                switch (esc) {
                case 't': ch = '\t'; break;
                case 'b': ch = '\b'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 'f': ch = '\f'; break;
                case '0': ch = '\0'; break;
                case '\'': ch = '\''; break;
                case '\\': ch = '\\'; break;
                default: ch = esc; break;
                }
            }
            if (ch == '\n') L->line++;
            if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = ch;
        }
        if (*L->cur == '\'') L->cur++;
        else lex_error(L, "unterminated string");
        buf[len] = '\0';
        // Build the runtime asom_string at parse time so the (possibly NUL-
        // containing) length is preserved; the parser stores it as the
        // operand of node_string_val.
        t->strval = buf;          // ownership stays with the lexer / parser
        t->len = len;
        t->kind = T_STRING;
        return;
    }

    switch (c) {
    case '(': L->cur++; t->kind = T_LPAREN; t->len = 1; return;
    case ')': L->cur++; t->kind = T_RPAREN; t->len = 1; return;
    case '[': L->cur++; t->kind = T_LBRACK; t->len = 1; return;
    case ']': L->cur++; t->kind = T_RBRACK; t->len = 1; return;
    case '#': L->cur++; t->kind = T_POUND;  t->len = 1; return;
    case '^': L->cur++; t->kind = T_CARET;  t->len = 1; return;
    case '.': L->cur++; t->kind = T_PERIOD; t->len = 1; return;
    case ':':
        if (L->cur[1] == '=') { L->cur += 2; t->kind = T_ASSIGN; t->len = 2; return; }
        L->cur++; t->kind = T_COLON; t->len = 1; return;
    case '|':
        // `|` is normally a single-character delimiter for variable-block
        // boundaries (`| a b |`), but when it stands at the start of a
        // longer operator sequence (`||`, `|&`, …) it's an OPSEQ token.
        if (is_op_char((unsigned char)L->cur[1])) break; // fall through to op-seq logic
        L->cur++; t->kind = T_PIPE; t->len = 1; return;
    }

    // Operator sequence (one or more op chars; '-' alone is T_OPSEQ too)
    if (is_op_char((unsigned char)c)) {
        const char *p = L->cur;
        while (is_op_char((unsigned char)*p)) p++;
        size_t len = (size_t)(p - L->cur);
        t->strval = lex_intern_range(L->cur, len);
        L->cur = p;
        t->len = len;
        // '=' alone has special role (method def "=") so distinguish it
        if (len == 1 && t->strval[0] == '=') { t->kind = T_EQUAL; return; }
        t->kind = T_OPSEQ;
        return;
    }

    lex_error(L, "unexpected character: %c (0x%02x)", c, (unsigned)c);
}

// ---------------------------------------------------------------------------
// Symbol table (per method, with nested block scopes).
// ---------------------------------------------------------------------------

#define ASOM_MAX_LOCALS_PER_SCOPE 64
#define ASOM_MAX_FIELDS 64
#define ASOM_MAX_SCOPE_DEPTH 16

typedef struct asom_scope {
    const char *names[ASOM_MAX_LOCALS_PER_SCOPE];
    uint32_t cnt;
} Scope;

typedef struct {
    Lexer *L;
    CTX *ctx;        // needed for runtime VALUE construction in literal arrays

    // class state
    const char *class_name;
    const char *fields[ASOM_MAX_FIELDS];
    uint32_t fields_cnt;
    const char *class_fields[ASOM_MAX_FIELDS];
    uint32_t class_fields_cnt;
    bool on_class_side; // currently parsing class-side methods

    // method state (stack of nested block/method scopes)
    Scope scopes[ASOM_MAX_SCOPE_DEPTH];
    uint32_t scope_depth; // number of active scopes
} Parser;

static void
parser_error(Parser *P, const char *fmt, ...)
{
    fprintf(stderr, "asom: %s:%d: ", P->L->file, P->L->line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static Token *peek(Parser *P) { return &P->L->tok; }

static void
expect(Parser *P, TokenKind k)
{
    if (P->L->tok.kind != k) parser_error(P, "expected token %d, got %d", k, P->L->tok.kind);
    lex_advance(P->L);
}

static bool
accept(Parser *P, TokenKind k)
{
    if (P->L->tok.kind == k) { lex_advance(P->L); return true; }
    return false;
}

static void
push_scope(Parser *P)
{
    if (P->scope_depth >= ASOM_MAX_SCOPE_DEPTH) parser_error(P, "scope nesting too deep");
    P->scopes[P->scope_depth++].cnt = 0;
}

static void
pop_scope(Parser *P)
{
    if (P->scope_depth == 0) parser_error(P, "scope underflow");
    P->scope_depth--;
}

static uint32_t
add_local(Parser *P, const char *name)
{
    Scope *s = &P->scopes[P->scope_depth - 1];
    if (s->cnt >= ASOM_MAX_LOCALS_PER_SCOPE) parser_error(P, "too many locals in scope");
    s->names[s->cnt] = name;
    return s->cnt++;
}

static bool
lookup_local(Parser *P, const char *name, uint32_t *out_scope, uint32_t *out_idx)
{
    for (int d = P->scope_depth - 1; d >= 0; d--) {
        Scope *s = &P->scopes[d];
        for (uint32_t i = 0; i < s->cnt; i++) {
            if (s->names[i] == name) {
                *out_scope = (uint32_t)((P->scope_depth - 1) - d);
                *out_idx = i;
                return true;
            }
        }
    }
    return false;
}

static bool
lookup_field(Parser *P, const char *name, uint32_t *out_idx)
{
    const char **arr = P->on_class_side ? P->class_fields : P->fields;
    uint32_t cnt = P->on_class_side ? P->class_fields_cnt : P->fields_cnt;
    for (uint32_t i = 0; i < cnt; i++) {
        if (arr[i] == name) { *out_idx = i; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// AST helpers.
// ---------------------------------------------------------------------------

static struct asom_callcache *
new_cc(void) { return calloc(1, sizeof(struct asom_callcache)); }

static NODE *
make_var_get(Parser *P, const char *name)
{
    uint32_t scope, idx;
    if (lookup_local(P, name, &scope, &idx)) return ALLOC_node_local_get(scope, idx);
    if (lookup_field(P, name, &idx)) {
        return P->on_class_side ? ALLOC_node_class_field_get(idx)
                                : ALLOC_node_field_get(idx);
    }
    if (strcmp(name, "self") == 0)           return ALLOC_node_self();
    if (strcmp(name, "nil")  == 0)           return ALLOC_node_nil();
    if (strcmp(name, "true") == 0)           return ALLOC_node_true();
    if (strcmp(name, "false")== 0)           return ALLOC_node_false();
    return ALLOC_node_global_get(asom_intern_cstr(name));
}

static NODE *
make_var_set(Parser *P, const char *name, NODE *rhs)
{
    uint32_t scope, idx;
    if (lookup_local(P, name, &scope, &idx)) return ALLOC_node_local_set(scope, idx, rhs);
    if (lookup_field(P, name, &idx)) {
        return P->on_class_side ? ALLOC_node_class_field_set(idx, rhs)
                                : ALLOC_node_field_set(idx, rhs);
    }
    parser_error(P, "cannot assign to '%s' (not a local or field)", name);
    return NULL;
}

static NODE *
make_send(Parser *P, NODE *recv, const char *sel, NODE **args, uint32_t nargs)
{
    (void)P;
    sel = asom_intern_cstr(sel);
    switch (nargs) {
    case 0: return ALLOC_node_send0(recv, sel, new_cc());
    case 1: return ALLOC_node_send1(recv, args[0], sel, new_cc());
    case 2: return ALLOC_node_send2(recv, args[0], args[1], sel, new_cc());
    case 3: return ALLOC_node_send3(recv, args[0], args[1], args[2], sel, new_cc());
    case 4: return ALLOC_node_send4(recv, args[0], args[1], args[2], args[3], sel, new_cc());
    case 5: return ALLOC_node_send5(recv, args[0], args[1], args[2], args[3], args[4], sel, new_cc());
    case 6: return ALLOC_node_send6(recv, args[0], args[1], args[2], args[3], args[4], args[5], sel, new_cc());
    case 7: return ALLOC_node_send7(recv, args[0], args[1], args[2], args[3], args[4], args[5], args[6], sel, new_cc());
    case 8: return ALLOC_node_send8(recv, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], sel, new_cc());
    default:
        fprintf(stderr, "asom: sends with %u args not yet supported\n", nargs);
        exit(1);
    }
}

static NODE *
make_super_send(Parser *P, const char *sel, NODE **args, uint32_t nargs)
{
    (void)P;
    sel = asom_intern_cstr(sel);
    switch (nargs) {
    case 0: return ALLOC_node_super_send0(sel, new_cc());
    case 1: return ALLOC_node_super_send1(args[0], sel, new_cc());
    case 2: return ALLOC_node_super_send2(args[0], args[1], sel, new_cc());
    case 3: return ALLOC_node_super_send3(args[0], args[1], args[2], sel, new_cc());
    case 4: return ALLOC_node_super_send4(args[0], args[1], args[2], args[3], sel, new_cc());
    case 5: return ALLOC_node_super_send5(args[0], args[1], args[2], args[3], args[4], sel, new_cc());
    case 6: return ALLOC_node_super_send6(args[0], args[1], args[2], args[3], args[4], args[5], sel, new_cc());
    case 7: return ALLOC_node_super_send7(args[0], args[1], args[2], args[3], args[4], args[5], args[6], sel, new_cc());
    case 8: return ALLOC_node_super_send8(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], sel, new_cc());
    default:
        fprintf(stderr, "asom: super sends with %u args not yet supported\n", nargs);
        exit(1);
    }
}

// ---------------------------------------------------------------------------
// Expression parser
// ---------------------------------------------------------------------------

static NODE *parse_expression(Parser *P, bool in_block);
static NODE *parse_block_body(Parser *P);
static NODE *parse_unary_chain(Parser *P, NODE *recv);
static NODE *parse_binary_chain(Parser *P, NODE *recv);
static VALUE parse_literal_array_value(Parser *P);

// Parse one element of a literal array: another literal, with the
// added wrinkle that bare identifiers and negative numbers are allowed.
static VALUE
parse_literal_array_elem(Parser *P)
{
    Token *t = peek(P);
    switch (t->kind) {
    case T_INTEGER: { intptr_t v = t->ival; lex_advance(P->L); return ASOM_INT2VAL(v); }
    case T_DOUBLE:  { double v = t->dval; lex_advance(P->L); return asom_double_new(P->ctx, v); }
    case T_STRING:  { VALUE v = asom_string_new(P->ctx, t->strval, t->len); lex_advance(P->L); return v; }
    case T_IDENT:   { const char *s = t->strval; lex_advance(P->L); return asom_intern_symbol(P->ctx, s); }
    case T_KEYWORD: {
        // build a symbol composed of one-or-more keywords
        char buf[256]; buf[0] = '\0';
        while (peek(P)->kind == T_KEYWORD) {
            strncat(buf, peek(P)->strval, sizeof(buf) - strlen(buf) - 1);
            strncat(buf, ":",             sizeof(buf) - strlen(buf) - 1);
            lex_advance(P->L);
        }
        return asom_intern_symbol(P->ctx, buf);
    }
    case T_POUND:   {  // nested #symbol or #(...) inside a literal array
        lex_advance(P->L);
        if (peek(P)->kind == T_LPAREN) {
            lex_advance(P->L);
            return parse_literal_array_value(P);
        }
        if (peek(P)->kind == T_STRING) { const char *s = peek(P)->strval; lex_advance(P->L); return asom_intern_symbol(P->ctx, s); }
        if (peek(P)->kind == T_IDENT)  { const char *s = peek(P)->strval; lex_advance(P->L); return asom_intern_symbol(P->ctx, s); }
        if (peek(P)->kind == T_KEYWORD) {
            char buf[256]; buf[0] = '\0';
            while (peek(P)->kind == T_KEYWORD) {
                strncat(buf, peek(P)->strval, sizeof(buf) - strlen(buf) - 1);
                strncat(buf, ":",             sizeof(buf) - strlen(buf) - 1);
                lex_advance(P->L);
            }
            return asom_intern_symbol(P->ctx, buf);
        }
        parser_error(P, "expected symbol after '#' in literal array");
        return 0;
    }
    case T_OPSEQ: {
        // Negative numeric literals inside #(...)
        if (t->strval && t->strval[0] == '-' && t->strval[1] == '\0') {
            lex_advance(P->L);
            if (peek(P)->kind == T_INTEGER) { intptr_t v = -peek(P)->ival; lex_advance(P->L); return ASOM_INT2VAL(v); }
            if (peek(P)->kind == T_DOUBLE)  { double v = -peek(P)->dval; lex_advance(P->L); return asom_double_new(P->ctx, v); }
        }
        parser_error(P, "unexpected operator in literal array");
        return 0;
    }
    case T_LPAREN:  {  // SOM allows nested (...) without a leading #
        lex_advance(P->L);
        return parse_literal_array_value(P);
    }
    default:
        parser_error(P, "unexpected token in literal array (kind=%d)", t->kind);
        return 0;
    }
}

// Caller already consumed `#(` (or `(`); we parse elements until ')'.
static VALUE
parse_literal_array_value(Parser *P)
{
    VALUE buf[64];
    uint32_t cnt = 0;
    while (peek(P)->kind != T_RPAREN && peek(P)->kind != T_EOF) {
        if (cnt >= 64) parser_error(P, "literal array too long (max 64)");
        buf[cnt++] = parse_literal_array_elem(P);
    }
    expect(P, T_RPAREN);
    VALUE arr = asom_array_new(P->ctx, cnt);
    for (uint32_t i = 0; i < cnt; i++) ((struct asom_array *)ASOM_VAL2OBJ(arr))->data[i] = buf[i];
    return arr;
}

static NODE *
parse_primary(Parser *P)
{
    Token *t = peek(P);
    switch (t->kind) {
    case T_IDENT: {
        const char *name = t->strval;
        lex_advance(P->L);
        // `super` consumes the immediately-following message and emits a
        // super_send instead of a normal send. Subsequent chained messages
        // (e.g. `super foo bar` — `bar` is a normal unary send to the
        // super-send result) fall through to the surrounding chain
        // builders, which is exactly what we want.
        if (strcmp(name, "super") == 0) {
            Token *m = peek(P);
            if (m->kind == T_IDENT) {
                const char *sel = m->strval;
                lex_advance(P->L);
                return make_super_send(P, sel, NULL, 0);
            }
            if (m->kind == T_KEYWORD) {
                char selbuf[256]; selbuf[0] = '\0';
                NODE *args[8]; uint32_t nargs = 0;
                while (peek(P)->kind == T_KEYWORD) {
                    strncat(selbuf, peek(P)->strval, sizeof(selbuf) - strlen(selbuf) - 1);
                    strncat(selbuf, ":", sizeof(selbuf) - strlen(selbuf) - 1);
                    lex_advance(P->L);
                    NODE *arg = parse_primary(P);
                    arg = parse_unary_chain(P, arg);
                    arg = parse_binary_chain(P, arg);
                    if (nargs >= 8) parser_error(P, "too many keyword args in super send");
                    args[nargs++] = arg;
                }
                return make_super_send(P, asom_intern_cstr(selbuf), args, nargs);
            }
            if (m->kind == T_OPSEQ || m->kind == T_PIPE
                || m->kind == T_EQUAL || m->kind == T_MINUS) {
                const char *sel = (m->kind == T_PIPE)  ? "|"
                                : (m->kind == T_EQUAL) ? "="
                                : (m->kind == T_MINUS) ? "-"
                                : m->strval;
                lex_advance(P->L);
                NODE *arg = parse_primary(P);
                arg = parse_unary_chain(P, arg);
                NODE *args[1] = { arg };
                return make_super_send(P, asom_intern_cstr(sel), args, 1);
            }
            // Bare `super` — uncommon but valid; fall through to self.
            return ALLOC_node_self();
        }
        return make_var_get(P, name);
    }
    case T_INTEGER: {
        intptr_t v = t->ival;
        lex_advance(P->L);
        return ALLOC_node_int_lit((int32_t)v);
    }
    case T_DOUBLE: {
        double v = t->dval;
        lex_advance(P->L);
        return ALLOC_node_double_lit(v);
    }
    case T_STRING: {
        // Build the runtime asom_string at parse time so embedded NUL bytes
        // and the explicit byte length round-trip correctly.
        VALUE v = asom_string_new(P->ctx, t->strval, t->len);
        lex_advance(P->L);
        return ALLOC_node_string_val((uint64_t)(uintptr_t)v);
    }
    case T_POUND: {
        // #symbol | #'string' | #binarySelector | #keyword:keyword: | #(literalArray)
        lex_advance(P->L);
        char buf[256]; buf[0] = '\0';
        if (peek(P)->kind == T_LPAREN) {
            lex_advance(P->L);
            VALUE arr = parse_literal_array_value(P);
            return ALLOC_node_array_lit((uint64_t)(uintptr_t)arr);
        }
        if (peek(P)->kind == T_STRING) {
            const char *s = peek(P)->strval;
            lex_advance(P->L);
            return ALLOC_node_symbol_lit(s);
        }
        if (peek(P)->kind == T_IDENT) {
            strncat(buf, peek(P)->strval, sizeof(buf) - strlen(buf) - 1);
            lex_advance(P->L);
            return ALLOC_node_symbol_lit(asom_intern_cstr(buf));
        }
        if (peek(P)->kind == T_KEYWORD) {
            // Match SOM/CSOM lexing: a literal symbol's keyword sequence
            // groups *contiguous* (whitespace-free) keywords. The next
            // keyword preceded by whitespace belongs to the surrounding
            // message expression.
            bool first = true;
            while (peek(P)->kind == T_KEYWORD) {
                if (!first && peek(P)->ws_before) break;
                strncat(buf, peek(P)->strval, sizeof(buf) - strlen(buf) - 1);
                strncat(buf, ":", sizeof(buf) - strlen(buf) - 1);
                lex_advance(P->L);
                first = false;
            }
            return ALLOC_node_symbol_lit(asom_intern_cstr(buf));
        }
        if (peek(P)->kind == T_OPSEQ || peek(P)->kind == T_EQUAL
            || peek(P)->kind == T_PIPE || peek(P)->kind == T_COLON) {
            // `#~`, `#&`, `#|`, `#:` etc. — single-char operator symbols.
            const char *s = (peek(P)->kind == T_PIPE)  ? "|"
                          : (peek(P)->kind == T_EQUAL) ? "="
                          : (peek(P)->kind == T_COLON) ? ":"
                          :  peek(P)->strval;
            lex_advance(P->L);
            return ALLOC_node_symbol_lit(asom_intern_cstr(s));
        }
        parser_error(P, "expected symbol literal after '#'");
        return NULL;
    }
    case T_LPAREN: {
        lex_advance(P->L);
        NODE *inner = parse_expression(P, false);
        expect(P, T_RPAREN);
        return inner;
    }
    case T_LBRACK: {
        lex_advance(P->L);
        push_scope(P);
        // optional block arguments: `[ :a :b | ... ]`
        uint32_t num_params = 0;
        while (peek(P)->kind == T_COLON) {
            lex_advance(P->L);
            if (peek(P)->kind != T_IDENT) parser_error(P, "expected block argument name");
            add_local(P, peek(P)->strval);
            num_params++;
            lex_advance(P->L);
        }
        if (num_params > 0) expect(P, T_PIPE);
        // optional locals: `| a b |`
        if (peek(P)->kind == T_PIPE) {
            lex_advance(P->L);
            while (peek(P)->kind == T_IDENT) {
                add_local(P, peek(P)->strval);
                lex_advance(P->L);
            }
            expect(P, T_PIPE);
        }
        NODE *body = parse_block_body(P);
        expect(P, T_RBRACK);
        Scope *s = &P->scopes[P->scope_depth - 1];
        uint32_t num_locals = s->cnt - num_params;
        NODE *block_body = ALLOC_node_block_body(body, num_params, num_locals);
        pop_scope(P);
        return ALLOC_node_block(block_body, num_params, num_locals);
    }
    case T_OPSEQ: {
        // Unary minus on a numeric literal — `-1`, `-3.14` — appears as
        // OPSEQ "-" followed by INTEGER/DOUBLE in keyword-arg / binary-arg
        // positions. Anywhere else, leading operators in primary are an
        // error.
        if (t->strval && t->strval[0] == '-' && t->strval[1] == '\0') {
            lex_advance(P->L);
            if (peek(P)->kind == T_INTEGER) {
                intptr_t v = -peek(P)->ival;
                lex_advance(P->L);
                return ALLOC_node_int_lit((int32_t)v);
            }
            if (peek(P)->kind == T_DOUBLE) {
                double v = -peek(P)->dval;
                lex_advance(P->L);
                return ALLOC_node_double_lit(v);
            }
            parser_error(P, "expected number after '-'");
        }
        parser_error(P, "unexpected operator token in primary: '%s'", t->strval ? t->strval : "?");
        return NULL;
    }
    default:
        parser_error(P, "unexpected token in primary (kind=%d)", t->kind);
        return NULL;
    }
}

static NODE *
parse_unary_chain(Parser *P, NODE *recv)
{
    while (peek(P)->kind == T_IDENT) {
        const char *sel = peek(P)->strval;
        lex_advance(P->L);
        recv = make_send(P, recv, sel, NULL, 0);
    }
    return recv;
}

static NODE *
parse_binary_chain(Parser *P, NODE *recv)
{
    while (peek(P)->kind == T_OPSEQ
           || peek(P)->kind == T_PIPE
           || peek(P)->kind == T_EQUAL) {
        const char *sel;
        if (peek(P)->kind == T_PIPE) sel = "|";
        else if (peek(P)->kind == T_EQUAL) sel = "=";
        else sel = peek(P)->strval;
        sel = asom_intern_cstr(sel);
        lex_advance(P->L);
        NODE *arg = parse_primary(P);
        arg = parse_unary_chain(P, arg);
        NODE *args[1] = { arg };
        recv = make_send(P, recv, sel, args, 1);
    }
    return recv;
}

static NODE *
parse_keyword_chain(Parser *P, NODE *recv)
{
    if (peek(P)->kind != T_KEYWORD) return recv;
    char selbuf[256]; selbuf[0] = '\0';
    NODE *args[8]; uint32_t nargs = 0;
    while (peek(P)->kind == T_KEYWORD) {
        strncat(selbuf, peek(P)->strval, sizeof(selbuf) - strlen(selbuf) - 1);
        strncat(selbuf, ":", sizeof(selbuf) - strlen(selbuf) - 1);
        lex_advance(P->L);
        NODE *arg = parse_primary(P);
        arg = parse_unary_chain(P, arg);
        arg = parse_binary_chain(P, arg);
        if (nargs >= 8) parser_error(P, "too many keyword args");
        args[nargs++] = arg;
    }
    return make_send(P, recv, asom_intern_cstr(selbuf), args, nargs);
}

static NODE *
parse_evaluation(Parser *P, bool in_block)
{
    (void)in_block;
    NODE *e = parse_primary(P);
    e = parse_unary_chain(P, e);
    e = parse_binary_chain(P, e);
    e = parse_keyword_chain(P, e);
    return e;
}

static NODE *
parse_expression(Parser *P, bool in_block)
{
    // Detect chained assignments: `a := b := <eval>`.
    // We need to look ahead for IDENT followed by ':='.
    // To keep parsing simple, handle one assignment at a time.
    if (peek(P)->kind == T_IDENT) {
        // peek next token by saving lexer state
        Token saved = P->L->tok;
        const char *saved_cur = P->L->cur;
        int saved_line = P->L->line;
        const char *name = peek(P)->strval;
        lex_advance(P->L);
        if (peek(P)->kind == T_ASSIGN) {
            lex_advance(P->L);
            NODE *rhs = parse_expression(P, in_block);
            return make_var_set(P, name, rhs);
        }
        // restore
        P->L->tok = saved;
        P->L->cur = saved_cur;
        P->L->line = saved_line;
    }
    return parse_evaluation(P, in_block);
}

static NODE *
parse_block_body(Parser *P)
{
    // blockBody : Exit result | expression ( Period blockBody? )?
    NODE *result = NULL;
    while (peek(P)->kind != T_RBRACK
           && peek(P)->kind != T_RPAREN
           && peek(P)->kind != T_EOF) {
        NODE *stmt;
        if (peek(P)->kind == T_CARET) {
            lex_advance(P->L);
            NODE *e = parse_expression(P, true);
            stmt = ALLOC_node_return_nlr(e);
            // optional trailing period
            accept(P, T_PERIOD);
            result = result ? ALLOC_node_seq(result, stmt) : stmt;
            break;
        }
        stmt = parse_expression(P, true);
        result = result ? ALLOC_node_seq(result, stmt) : stmt;
        if (!accept(P, T_PERIOD)) break;
    }
    if (!result) result = ALLOC_node_self();
    return result;
}

// Method body — like block body but ^expr is a local return, and an implicit
// `^self` is appended when the body falls through.
static NODE *
parse_method_body_stmts(Parser *P)
{
    NODE *result = NULL;
    bool returned = false;
    while (peek(P)->kind != T_RPAREN && peek(P)->kind != T_EOF) {
        NODE *stmt;
        if (peek(P)->kind == T_CARET) {
            lex_advance(P->L);
            NODE *e = parse_expression(P, false);
            stmt = ALLOC_node_return_local(e);
            accept(P, T_PERIOD);
            result = result ? ALLOC_node_seq(result, stmt) : stmt;
            returned = true;
            break;
        }
        stmt = parse_expression(P, false);
        result = result ? ALLOC_node_seq(result, stmt) : stmt;
        if (!accept(P, T_PERIOD)) break;
    }
    if (!result) result = ALLOC_node_self();
    if (!returned) {
        NODE *implicit = ALLOC_node_return_local(ALLOC_node_self());
        result = ALLOC_node_seq(result, implicit);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Method definition
// ---------------------------------------------------------------------------

#define ParsedMethod ASOM_PARSED_METHOD

static void
parse_method(Parser *P, ParsedMethod *out)
{
    char selbuf[256]; selbuf[0] = '\0';
    Token *t = peek(P);
    push_scope(P);
    uint32_t num_params = 0;

    if (t->kind == T_IDENT) {
        // unaryPattern
        strncat(selbuf, t->strval, sizeof(selbuf) - strlen(selbuf) - 1);
        lex_advance(P->L);
    } else if (t->kind == T_OPSEQ || t->kind == T_PIPE
               || t->kind == T_EQUAL || t->kind == T_MINUS) {
        // binaryPattern
        const char *sel = (t->kind == T_PIPE) ? "|"
                        : (t->kind == T_EQUAL) ? "="
                        : (t->kind == T_MINUS) ? "-"
                        : t->strval;
        strncat(selbuf, sel, sizeof(selbuf) - strlen(selbuf) - 1);
        lex_advance(P->L);
        if (peek(P)->kind != T_IDENT) parser_error(P, "expected binary argument");
        add_local(P, peek(P)->strval);
        num_params++;
        lex_advance(P->L);
    } else if (t->kind == T_KEYWORD) {
        // keywordPattern
        while (peek(P)->kind == T_KEYWORD) {
            strncat(selbuf, peek(P)->strval, sizeof(selbuf) - strlen(selbuf) - 1);
            strncat(selbuf, ":", sizeof(selbuf) - strlen(selbuf) - 1);
            lex_advance(P->L);
            if (peek(P)->kind != T_IDENT) parser_error(P, "expected keyword argument");
            add_local(P, peek(P)->strval);
            num_params++;
            lex_advance(P->L);
        }
    } else {
        parser_error(P, "expected method pattern (got kind=%d)", t->kind);
    }

    expect(P, T_EQUAL);

    if (peek(P)->kind == T_PRIMITIVE) {
        lex_advance(P->L);
        out->selector = asom_intern_cstr(selbuf);
        out->num_params = num_params;
        out->num_locals = 0;
        out->body = NULL;
        out->is_primitive = true;
        pop_scope(P);
        return;
    }

    expect(P, T_LPAREN);

    // optional locals: | a b c |
    if (peek(P)->kind == T_PIPE) {
        lex_advance(P->L);
        while (peek(P)->kind == T_IDENT) {
            add_local(P, peek(P)->strval);
            lex_advance(P->L);
        }
        expect(P, T_PIPE);
    }

    NODE *body = parse_method_body_stmts(P);
    expect(P, T_RPAREN);

    Scope *s = &P->scopes[P->scope_depth - 1];
    out->selector = asom_intern_cstr(selbuf);
    out->num_params = num_params;
    out->num_locals = s->cnt - num_params;
    // Store the bare body (sequence / return / send / ...) — NOT wrapped
    // in node_method_body — so that astro_cs_compile sees a specializable
    // root. node_method_body is @noinline and emits no SD_, which would
    // make AOT bake a no-op. Keeping the wrapper out lets the specialiser
    // start with the actual statements.
    out->body = body;
    out->is_primitive = false;
    pop_scope(P);
}

// ---------------------------------------------------------------------------
// Class definition
// ---------------------------------------------------------------------------

static void
parse_field_list(Parser *P, const char **arr, uint32_t *cnt)
{
    if (peek(P)->kind != T_PIPE) return;
    lex_advance(P->L);
    while (peek(P)->kind == T_IDENT) {
        if (*cnt >= ASOM_MAX_FIELDS) parser_error(P, "too many fields");
        arr[(*cnt)++] = asom_intern_cstr(peek(P)->strval);
        lex_advance(P->L);
    }
    expect(P, T_PIPE);
}

ASOM_PARSED_CLASS *
asom_parse_class_str(CTX *c, const char *src, const char *file)
{
    Lexer L;
    lex_init(&L, src, file);
    Parser P = { .L = &L, .ctx = c };

    // Identifier "=" superclass-spec
    if (peek(&P)->kind != T_IDENT) parser_error(&P, "expected class name");
    P.class_name = asom_intern_cstr(peek(&P)->strval);
    lex_advance(P.L);
    expect(&P, T_EQUAL);

    const char *super_name = NULL;
    if (peek(&P)->kind == T_IDENT) {
        super_name = asom_intern_cstr(peek(&P)->strval);
        lex_advance(P.L);
    }
    expect(&P, T_LPAREN);

    // Pull in inherited field names so that references to ancestor instance
    // variables resolve at the correct slot indices. We load the superclass
    // here (recursively, if needed) before parsing our own methods. The
    // resulting layout is [super fields..., own fields...].
    if (super_name && c && strcmp(super_name, "nil") != 0) {
        struct asom_class *super = asom_load_class(c, super_name);
        if (super) {
            for (uint32_t i = 0; i < super->num_instance_fields && P.fields_cnt < ASOM_MAX_FIELDS; i++) {
                P.fields[P.fields_cnt++] = super->field_names[i];
            }
            for (uint32_t i = 0; i < super->num_class_side_fields && P.class_fields_cnt < ASOM_MAX_FIELDS; i++) {
                P.class_fields[P.class_fields_cnt++] = super->class_side_field_names[i];
            }
        }
    }

    // instance fields (own)
    parse_field_list(&P, P.fields, &P.fields_cnt);

    ASOM_PARSED_CLASS *pc = calloc(1, sizeof(*pc));
    pc->name = P.class_name;
    pc->superclass_name = super_name;

    // instance-side methods
    while (peek(&P)->kind != T_RPAREN
           && peek(&P)->kind != T_SEPARATOR
           && peek(&P)->kind != T_EOF) {
        ParsedMethod *pm = calloc(1, sizeof(*pm));
        parse_method(&P, pm);
        if (pc->methods_cnt == pc->methods_cap) {
            pc->methods_cap = pc->methods_cap ? pc->methods_cap * 2 : 8;
            pc->methods = realloc(pc->methods, pc->methods_cap * sizeof(*pc->methods));
        }
        pc->methods[pc->methods_cnt++] = pm;
    }

    if (accept(&P, T_SEPARATOR)) {
        P.on_class_side = true;
        // class-side fields
        parse_field_list(&P, P.class_fields, &P.class_fields_cnt);
        while (peek(&P)->kind != T_RPAREN && peek(&P)->kind != T_EOF) {
            ParsedMethod *pm = calloc(1, sizeof(*pm));
            parse_method(&P, pm);
            if (pc->class_methods_cnt == pc->class_methods_cap) {
                pc->class_methods_cap = pc->class_methods_cap ? pc->class_methods_cap * 2 : 8;
                pc->class_methods = realloc(pc->class_methods, pc->class_methods_cap * sizeof(*pc->class_methods));
            }
            pc->class_methods[pc->class_methods_cnt++] = pm;
        }
    }

    expect(&P, T_RPAREN);

    // copy field names into pc (parser scope is about to disappear)
    pc->fields_cnt = P.fields_cnt;
    pc->fields = malloc(pc->fields_cnt * sizeof(*pc->fields));
    memcpy(pc->fields, P.fields, pc->fields_cnt * sizeof(*pc->fields));
    pc->class_fields_cnt = P.class_fields_cnt;
    pc->class_fields = malloc(pc->class_fields_cnt * sizeof(*pc->class_fields));
    memcpy(pc->class_fields, P.class_fields, pc->class_fields_cnt * sizeof(*pc->class_fields));

    return pc;
}

ASOM_PARSED_CLASS *
asom_parse_class_file(CTX *c, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "asom: cannot open %s\n", path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (fread(buf, 1, sz, fp) != (size_t)sz) {
        fprintf(stderr, "asom: short read on %s\n", path);
        fclose(fp); free(buf); return NULL;
    }
    buf[sz] = '\0';
    fclose(fp);
    ASOM_PARSED_CLASS *pc = asom_parse_class_str(c, buf, path);
    // buf is leaked deliberately: interned/parsed strings may reference into it
    return pc;
}
