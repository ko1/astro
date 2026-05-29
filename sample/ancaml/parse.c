// Recursive-descent parser for ancaml (MinCaml).
//
// Produces the ASTro AST directly.  Two things happen during parsing that a
// MinCaml compiler would do in later passes:
//
//   * Variable resolution.  The parser keeps a compile-time stack of scope
//     frames (mirroring the runtime `ac_frame` chain) and resolves every
//     identifier to a (depth, idx) coordinate → `node_lref`.  Free names are
//     looked up in the external table → `node_gref`; an unknown name is a
//     parse error.
//
//   * Comparison desugaring.  Exactly as MinCaml's parser, `<> < > >=` are
//     rewritten in terms of `=`, `<=` and `not`, so the evaluator only has
//     `node_eq` / `node_le` / `node_not`.
//
// Variable-arity children (tuple elements, >4-ary application arguments) are
// stored in the AC_TUPLE_ITEMS / AC_CALL_ARGS side-tables and reached at
// runtime through a dispatcher read, so they (and every function body) are
// registered as code-store entries — see ac_entries.
#include <stdlib.h>
#include <string.h>
#include "parse.h"
#include "type.h"

// ---- side-tables & entry list (defined here, declared in node.h) -----

NODE **AC_CALL_ARGS   = NULL;
NODE **AC_TUPLE_ITEMS = NULL;
NODE **ac_entries     = NULL;
int    ac_n_entries   = 0;
NODE **ac_tail_roots   = NULL;
int    ac_n_tail_roots = 0;

// Count of node_fun built so far — used to decide a function body's
// `is_leaf` (no nested function was created while parsing it).
static int g_nfun_built = 0;

// A small growable NODE* vector.
typedef struct { NODE **v; int n, cap; } NodeVec;
static int
nv_push(NodeVec *a, NODE *n)
{
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->v = realloc(a->v, sizeof(NODE *) * a->cap); }
    a->v[a->n] = n;
    return a->n++;
}

static NodeVec g_call_args, g_tuple_items, g_entries, g_tail_roots;

static void add_entry(NODE *n) { nv_push(&g_entries, n); }

// ---- compile-time scope stack ---------------------------------------

#define MAX_DEPTH 256
typedef struct { char **names; int n, cap; } ScopeFrame;
static ScopeFrame g_scope[MAX_DEPTH];
static int g_scope_top = -1;     // index of innermost frame

static void push_scope(void) {
    if (++g_scope_top >= MAX_DEPTH) ac_parse_fail("scope nesting too deep");
    ScopeFrame *f = &g_scope[g_scope_top];
    f->n = 0; f->cap = 0; f->names = NULL;
}
static void pop_scope(void) { g_scope_top--; }
static int scope_add(ScopeFrame *f, const char *name) {
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 8; f->names = realloc(f->names, sizeof(char *) * f->cap); }
    f->names[f->n] = strdup(name);
    return f->n++;
}

// ---- node construction helpers --------------------------------------

static NODE *ln(NODE *n) { n->head.line = ac_src_line; return n; }

static void
expect(int t, const char *what)
{
    if (ac_tok != t) ac_parse_fail("expected %s", what);
    ac_next_token();
}

static bool starts_simple(int t) {
    return t == TK_LPAREN || t == TK_INT || t == TK_FLOAT ||
           t == TK_TRUE   || t == TK_FALSE || t == TK_IDENT;
}

extern const struct NodeKind kind_node_get, kind_node_float;

// ---- forward decls ---------------------------------------------------

static NODE *parse_exp(void);
static NODE *parse_exp1(void);
static NODE *parse_put(void);
static NODE *parse_tuple(void);
static NODE *parse_cmp(void);
static NODE *parse_add(void);
static NODE *parse_mul(void);
static NODE *parse_unary(void);
static NODE *parse_app(void);
static NODE *parse_dot(void);
static NODE *parse_atom(void);

// ---- identifier resolution ------------------------------------------

static NODE *
resolve_ident(const char *name)
{
    for (int d = 0; g_scope_top - d >= 0; d++) {
        ScopeFrame *f = &g_scope[g_scope_top - d];
        for (int i = 0; i < f->n; i++)
            if (strcmp(f->names[i], name) == 0)
                return ln(ALLOC_node_lref((uint32_t)d, (uint32_t)i));
    }
    if (ac_external_type(name))
        return ln(ALLOC_node_gref(strdup(name)));
    ac_parse_fail("unbound variable: %s", name);
}

// ---- grammar ---------------------------------------------------------

// exp : exp1 (';' exp1)*
static NODE *
parse_exp(void)
{
    NODE *e = parse_exp1();
    while (ac_tok == TK_SEMI) {
        ac_next_token();
        NODE *e2 = parse_exp1();
        e = ln(ALLOC_node_seq(e, e2));
    }
    return e;
}

// `let rec f a b ... = funbody in body`
static NODE *
parse_letrec(void)
{
    ac_next_token();                          // consume `rec`
    if (ac_tok != TK_IDENT) ac_parse_fail("expected function name after `let rec`");
    char fname[256]; strcpy(fname, ac_tok_str);
    ac_next_token();

    char params[64][256]; int nparams = 0;
    while (ac_tok == TK_IDENT) {
        if (nparams >= 64) ac_parse_fail("too many parameters");
        strcpy(params[nparams++], ac_tok_str);
        ac_next_token();
    }
    if (nparams == 0) ac_parse_fail("`let rec %s` needs at least one parameter", fname);
    expect(TK_EQ, "`=`");

    push_scope();                              // frame holding f
    scope_add(&g_scope[g_scope_top], fname);
    push_scope();                              // frame holding the parameters
    for (int i = 0; i < nparams; i++) scope_add(&g_scope[g_scope_top], params[i]);
    int nfun_before = g_nfun_built;
    NODE *funbody = parse_exp();
    pop_scope();                               // params

    // Leaf iff no nested function (closure) was created while parsing the body.
    int is_leaf = (g_nfun_built == nfun_before);
    add_entry(funbody);
    nv_push(&g_tail_roots, funbody);           // body is a tail-position root
    NODE *fun = ln(ALLOC_node_fun((uint32_t)nparams, funbody, (uint32_t)is_leaf));
    g_nfun_built++;

    expect(TK_IN, "`in`");
    NODE *body = parse_exp();
    pop_scope();                               // f
    return ln(ALLOC_node_letrec(fun, body));
}

// `let (a, b, ...) = value in body`
static NODE *
parse_lettuple(void)
{
    ac_next_token();                          // consume `(`
    char names[64][256]; int n = 0;
    for (;;) {
        if (ac_tok != TK_IDENT) ac_parse_fail("expected identifier in tuple pattern");
        if (n >= 64) ac_parse_fail("tuple pattern too wide");
        strcpy(names[n++], ac_tok_str);
        ac_next_token();
        if (ac_tok == TK_COMMA) { ac_next_token(); continue; }
        break;
    }
    expect(TK_RPAREN, "`)`");
    if (n < 2) ac_parse_fail("tuple pattern needs at least two names");
    expect(TK_EQ, "`=`");
    NODE *value = parse_exp();
    expect(TK_IN, "`in`");
    push_scope();
    for (int i = 0; i < n; i++) scope_add(&g_scope[g_scope_top], names[i]);
    NODE *body = parse_exp();
    pop_scope();
    return ln(ALLOC_node_lettuple(value, (uint32_t)n, body));
}

// `let x = value in body`
static NODE *
parse_let_plain(void)
{
    char name[256]; strcpy(name, ac_tok_str);
    ac_next_token();
    expect(TK_EQ, "`=`");
    NODE *value = parse_exp();
    expect(TK_IN, "`in`");
    push_scope();
    scope_add(&g_scope[g_scope_top], name);
    NODE *body = parse_exp();
    pop_scope();
    return ln(ALLOC_node_let(value, body));
}

// exp1 : let-forms | if-then-else | parse_put
static NODE *
parse_exp1(void)
{
    if (ac_tok == TK_LET) {
        ac_next_token();
        if (ac_tok == TK_REC)    return parse_letrec();
        if (ac_tok == TK_LPAREN) return parse_lettuple();
        if (ac_tok == TK_IDENT)  return parse_let_plain();
        ac_parse_fail("expected name, `rec`, or `(` after `let`");
    }
    if (ac_tok == TK_IF) {
        ac_next_token();
        NODE *cond = parse_exp1();
        expect(TK_THEN, "`then`");
        NODE *thn = parse_exp1();
        expect(TK_ELSE, "`else`");
        NODE *els = parse_exp1();
        return ln(ALLOC_node_if(cond, thn, els));
    }
    return parse_put();
}

// put : tuple ('<-' exp)?
static NODE *
parse_put(void)
{
    NODE *e = parse_tuple();
    if (ac_tok == TK_LARROW) {
        ac_next_token();
        NODE *rhs = parse_exp1();   // `<-` binds tighter than `;`, so stop before it
        if (e->head.kind != &kind_node_get)
            ac_parse_fail("left of `<-` must be an array element `a.(i)`");
        NODE *arr = e->u.node_get.arr;
        NODE *idx = e->u.node_get.idx;
        return ln(ALLOC_node_put(arr, idx, rhs));
    }
    return e;
}

// tuple : cmp (',' cmp)*
static NODE *
parse_tuple(void)
{
    NODE *first = parse_cmp();
    if (ac_tok != TK_COMMA) return first;
    NodeVec elems = {0};
    nv_push(&elems, first);
    while (ac_tok == TK_COMMA) { ac_next_token(); nv_push(&elems, parse_cmp()); }
    int idx0 = g_tuple_items.n;
    for (int i = 0; i < elems.n; i++) { nv_push(&g_tuple_items, elems.v[i]); add_entry(elems.v[i]); }
    int cnt = elems.n;
    free(elems.v);
    return ln(ALLOC_node_tuple((uint32_t)idx0, (uint32_t)cnt));
}

// cmp : add (('='|'<>'|'<'|'>'|'<='|'>=') add)*   — desugared to eq/le/not
static NODE *
parse_cmp(void)
{
    NODE *e = parse_add();
    for (;;) {
        int op = ac_tok;
        switch (op) {
          case TK_EQ:  ac_next_token(); e = ln(ALLOC_node_eq(e, parse_add())); break;
          case TK_NEQ: ac_next_token(); e = ln(ALLOC_node_not(ln(ALLOC_node_eq(e, parse_add())))); break;
          case TK_LE:  ac_next_token(); e = ln(ALLOC_node_le(e, parse_add())); break;
          case TK_GE:  ac_next_token(); { NODE *r = parse_add(); e = ln(ALLOC_node_le(r, e)); } break;
          case TK_LT:  ac_next_token(); { NODE *r = parse_add(); e = ln(ALLOC_node_not(ln(ALLOC_node_le(r, e)))); } break;
          case TK_GT:  ac_next_token(); { NODE *r = parse_add(); e = ln(ALLOC_node_not(ln(ALLOC_node_le(e, r)))); } break;
          default: return e;
        }
    }
}

// add : mul (('+'|'-'|'+.'|'-.') mul)*
static NODE *
parse_add(void)
{
    NODE *e = parse_mul();
    for (;;) {
        int op = ac_tok;
        if      (op == TK_PLUS)   { ac_next_token(); e = ln(ALLOC_node_add(e, parse_mul())); }
        else if (op == TK_MINUS)  { ac_next_token(); e = ln(ALLOC_node_sub(e, parse_mul())); }
        else if (op == TK_FPLUS)  { ac_next_token(); e = ln(ALLOC_node_fadd(e, parse_mul())); }
        else if (op == TK_FMINUS) { ac_next_token(); e = ln(ALLOC_node_fsub(e, parse_mul())); }
        else return e;
    }
}

// mul : unary (('*.'|'/.') unary)*
static NODE *
parse_mul(void)
{
    NODE *e = parse_unary();
    for (;;) {
        int op = ac_tok;
        if      (op == TK_FSTAR)  { ac_next_token(); e = ln(ALLOC_node_fmul(e, parse_unary())); }
        else if (op == TK_FSLASH) { ac_next_token(); e = ln(ALLOC_node_fdiv(e, parse_unary())); }
        else return e;
    }
}

// unary : '-' unary | '-.' unary | 'not' unary | app
static NODE *
parse_unary(void)
{
    if (ac_tok == TK_MINUS) {
        ac_next_token();
        NODE *e = parse_unary();
        if (e->head.kind == &kind_node_float)        // MinCaml folds  -<float literal>
            return ln(ALLOC_node_float(-e->u.node_float.v));
        return ln(ALLOC_node_neg(e));
    }
    if (ac_tok == TK_FMINUS) { ac_next_token(); return ln(ALLOC_node_fneg(parse_unary())); }
    if (ac_tok == TK_NOT)    { ac_next_token(); return ln(ALLOC_node_not(parse_unary())); }
    return parse_app();
}

// app : 'Array.create' dot dot | dot dot*
static NODE *
parse_app(void)
{
    if (ac_tok == TK_ARRAY_MAKE) {
        ac_next_token();
        NODE *sz = parse_dot();
        NODE *init = parse_dot();
        return ln(ALLOC_node_array(sz, init));
    }
    NODE *f = parse_dot();
    if (!starts_simple(ac_tok)) return f;

    NodeVec args = {0};
    while (starts_simple(ac_tok)) nv_push(&args, parse_dot());

    NODE *r;
    switch (args.n) {
      case 1: r = ln(ALLOC_node_app1(f, args.v[0])); break;
      case 2: r = ln(ALLOC_node_app2(f, args.v[0], args.v[1])); break;
      case 3: r = ln(ALLOC_node_app3(f, args.v[0], args.v[1], args.v[2])); break;
      case 4: r = ln(ALLOC_node_app4(f, args.v[0], args.v[1], args.v[2], args.v[3])); break;
      default: {
        int idx0 = g_call_args.n;
        for (int i = 0; i < args.n; i++) { nv_push(&g_call_args, args.v[i]); add_entry(args.v[i]); }
        r = ln(ALLOC_node_appn(f, (uint32_t)idx0, (uint32_t)args.n));
      }
    }
    free(args.v);
    return r;
}

// dot : atom ('.(' exp ')')*
static NODE *
parse_dot(void)
{
    NODE *e = parse_atom();
    while (ac_tok == TK_DOT) {
        ac_next_token();
        expect(TK_LPAREN, "`(` after `.`");
        NODE *idx = parse_exp();
        expect(TK_RPAREN, "`)`");
        e = ln(ALLOC_node_get(e, idx));
    }
    return e;
}

static NODE *
parse_atom(void)
{
    switch (ac_tok) {
      case TK_INT:   {
        int64_t v = ac_tok_int; ac_next_token();
        if (v >= INT32_MIN && v <= INT32_MAX) return ln(ALLOC_node_int((int32_t)v));
        return ln(ALLOC_node_int64((uint64_t)v));
      }
      case TK_FLOAT: { double d = ac_tok_dbl;  ac_next_token(); return ln(ALLOC_node_float(d)); }
      case TK_TRUE:  ac_next_token(); return ln(ALLOC_node_bool(1));
      case TK_FALSE: ac_next_token(); return ln(ALLOC_node_bool(0));
      case TK_IDENT: { char nm[256]; strcpy(nm, ac_tok_str); ac_next_token(); return resolve_ident(nm); }
      case TK_LPAREN:
        ac_next_token();
        if (ac_tok == TK_RPAREN) { ac_next_token(); return ln(ALLOC_node_unit()); }
        else {
            NODE *e = parse_exp();
            expect(TK_RPAREN, "`)`");
            return e;
        }
      default:
        ac_parse_fail("unexpected token in expression");
    }
}

// ---- entry point -----------------------------------------------------

Program
ac_parse_program(const char *src)
{
    Program prog = { NULL, 0 };
    g_call_args = (NodeVec){0};
    g_tuple_items = (NodeVec){0};
    g_entries = (NodeVec){0};
    g_tail_roots = (NodeVec){0};
    g_scope_top = -1;
    g_nfun_built = 0;

    ac_init_lexer(src);
    ac_next_token();
    NODE *body = parse_exp();
    if (ac_tok != TK_EOF) ac_parse_fail("trailing tokens after program");

    add_entry(body);

    // publish the final (post-realloc) side-table / entry pointers
    AC_CALL_ARGS   = g_call_args.v;
    AC_TUPLE_ITEMS = g_tuple_items.v;
    ac_entries     = g_entries.v;
    ac_n_entries   = g_entries.n;
    ac_tail_roots   = g_tail_roots.v;
    ac_n_tail_roots = g_tail_roots.n;

    prog.body = body;
    prog.ok = 1;
    return prog;
}
