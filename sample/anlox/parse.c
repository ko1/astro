// Recursive-descent + precedence-climbing parser for anlox (Lox), with an
// integrated resolver: as it parses, it maintains a stack of lexical scopes
// and resolves every identifier to either a local (depth, slot) frame
// coordinate or a late-bound global (by name).  This is the same information
// Crafting Interpreters computes in its separate Resolver pass.
//
// `for` is desugared to a scoped block around a `while`.  Variable-arity
// children (block/stmt lists, call arguments) and function descriptors live
// in side-tables (LOX_BLOCK_STMTS / LOX_CALL_ARGS / LOX_FUNDEFS).  Every node
// reached at runtime through a dispatcher read — function bodies and all
// side-table children — is registered as a code-store entry.
#include <stdlib.h>
#include <string.h>
#include <gc.h>
#include "parse.h"

// The side-tables hold NODE* / fundef* that the runtime reaches by index.
// They must be GC-allocated (not libc malloc) so the collector can trace the
// AST through them — the static/global pointers below are GC roots.  (A GC
// run while the AST was only reachable via malloc'd arrays would free live
// nodes.)  Vector backing therefore uses GC_REALLOC and is never free()d.

// ---- side-tables & entries (declared in node.h) ----------------------

NODE **LOX_CALL_ARGS   = NULL;
NODE **LOX_BLOCK_STMTS = NULL;
struct lox_fundef **LOX_FUNDEFS = NULL;
uint32_t *LOX_CLASS_METHODS = NULL;
NODE **lox_entries     = NULL;
int    lox_n_entries   = 0;

typedef struct { NODE **v; int n, cap; } NodeVec;
static int nv_push(NodeVec *a, NODE *x) {
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 32; a->v = GC_REALLOC(a->v, sizeof(NODE *) * a->cap); }
    a->v[a->n] = x; return a->n++;
}
typedef struct { struct lox_fundef **v; int n, cap; } FunVec;
static int fv_push(FunVec *a, struct lox_fundef *x) {
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->v = GC_REALLOC(a->v, sizeof(void *) * a->cap); }
    a->v[a->n] = x; return a->n++;
}

typedef struct { uint32_t *v; int n, cap; } U32Vec;
static int u32_push(U32Vec *a, uint32_t x) {
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->v = GC_REALLOC(a->v, sizeof(uint32_t) * a->cap); }
    a->v[a->n] = x; return a->n++;
}

static NodeVec g_call_args, g_block_stmts, g_entries;
static FunVec  g_fundefs;
static U32Vec  g_class_methods;

static void add_entry(NODE *x) { nv_push(&g_entries, x); }

// Append a finished statement list to LOX_BLOCK_STMTS contiguously, register
// each as an entry, and return its start index.
static int commit_stmts(NodeVec *tmp) {
    int idx0 = g_block_stmts.n;
    for (int i = 0; i < tmp->n; i++) { nv_push(&g_block_stmts, tmp->v[i]); add_entry(tmp->v[i]); }
    return idx0;
}
static int commit_args(NodeVec *tmp) {
    int idx0 = g_call_args.n;
    for (int i = 0; i < tmp->n; i++) { nv_push(&g_call_args, tmp->v[i]); add_entry(tmp->v[i]); }
    return idx0;
}

// ---- scope stack (resolver) ------------------------------------------

#define MAX_SCOPES 256
typedef struct { char **names; char *defined; int n, cap; } Scope;
static Scope g_scopes[MAX_SCOPES];
static int   g_nscopes;       // 0 == global scope (no frame)
static int   g_in_function;   // depth of function nesting (for `return` checks)
static int   g_in_class;      // class-body nesting (for `this`)
static int   g_class_has_super; // current class has a superclass (for `super`)

static void begin_scope(void) {
    if (g_nscopes >= MAX_SCOPES) lox_parse_fail("Too much nesting.");
    Scope *s = &g_scopes[g_nscopes++];
    s->n = 0; s->cap = 0; s->names = NULL; s->defined = NULL;
}
static int end_scope(void) { return g_scopes[--g_nscopes].n; }   // returns slot count

// Declare a name in the current (innermost) local scope; returns its slot.
// At global scope (g_nscopes == 0) declarations are global → slot -1.
static int declare_local(const char *name) {
    if (g_nscopes == 0) return -1;
    Scope *s = &g_scopes[g_nscopes - 1];
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->names[i], name) == 0) lox_parse_fail("Already a variable with this name in this scope.");
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->names = realloc(s->names, sizeof(char *) * s->cap);
        s->defined = realloc(s->defined, s->cap);
    }
    s->names[s->n] = strdup(name);
    s->defined[s->n] = 0;
    return s->n++;
}
static void mark_defined(int slot) {
    if (g_nscopes == 0 || slot < 0) return;
    g_scopes[g_nscopes - 1].defined[slot] = 1;
}

// Resolve a name to a local: returns true and sets *depth/*slot, else false
// (caller treats it as a global).
static bool resolve_local(const char *name, uint32_t *depth, uint32_t *slot) {
    for (int d = 0; d < g_nscopes; d++) {
        Scope *s = &g_scopes[g_nscopes - 1 - d];
        for (int i = s->n - 1; i >= 0; i--) {
            if (strcmp(s->names[i], name) == 0) {
                if (d == 0 && !s->defined[i])
                    lox_parse_fail("Can't read local variable in its own initializer.");
                *depth = (uint32_t)d; *slot = (uint32_t)i;
                return true;
            }
        }
    }
    return false;
}

// ---- node helpers ----------------------------------------------------

static NODE *ln(NODE *n) { n->head.line = lox_src_line; return n; }

extern const struct NodeKind kind_node_local, kind_node_global, kind_node_get;

static void expect(int t, const char *what) {
    if (lox_tok != t) lox_parse_fail("Expect %s.", what);
    lox_next_token();
}
static bool match(int t) { if (lox_tok == t) { lox_next_token(); return true; } return false; }

// Emit a read of `name` (local or global).
static NODE *var_get(const char *name) {
    uint32_t depth, slot;
    if (resolve_local(name, &depth, &slot)) return ln(ALLOC_node_local(depth, slot));
    return ln(ALLOC_node_global(strdup(name)));
}

// ---- forward decls ---------------------------------------------------

static NODE *parse_declaration(void);
static NODE *parse_statement(void);
static NODE *parse_expression(void);
static NODE *parse_assignment(void);
static NODE *parse_or(void);
static NODE *parse_and(void);
static NODE *parse_equality(void);
static NODE *parse_comparison(void);
static NODE *parse_term(void);
static NODE *parse_factor(void);
static NODE *parse_unary(void);
static NODE *parse_call(void);
static NODE *parse_primary(void);
static NODE *parse_block_node(void);   // expects current tok == '{'
static NODE *parse_fun_closure(const char *name);
static int   parse_fundef(const char *name, bool is_init);
static NODE *parse_class(void);

// ---- expressions -----------------------------------------------------

static NODE *parse_expression(void) { return parse_assignment(); }

static NODE *
parse_assignment(void)
{
    NODE *expr = parse_or();
    if (lox_tok == TK_EQ) {
        lox_next_token();
        NODE *value = parse_assignment();
        if (expr->head.kind == &kind_node_local)
            return ln(ALLOC_node_assign_local(expr->u.node_local.depth, expr->u.node_local.slot, value));
        if (expr->head.kind == &kind_node_global)
            return ln(ALLOC_node_assign_global(strdup(expr->u.node_global.name), value));
        if (expr->head.kind == &kind_node_get)
            return ln(ALLOC_node_set(expr->u.node_get.obj, strdup(expr->u.node_get.name), value));
        lox_parse_fail("Invalid assignment target.");
    }
    return expr;
}

static NODE *parse_or(void) {
    NODE *e = parse_and();
    while (lox_tok == TK_OR) { lox_next_token(); e = ln(ALLOC_node_or(e, parse_and())); }
    return e;
}
static NODE *parse_and(void) {
    NODE *e = parse_equality();
    while (lox_tok == TK_AND) { lox_next_token(); e = ln(ALLOC_node_and(e, parse_equality())); }
    return e;
}
static NODE *parse_equality(void) {
    NODE *e = parse_comparison();
    for (;;) {
        if (lox_tok == TK_EQ_EQ)       { lox_next_token(); e = ln(ALLOC_node_eq(e, parse_comparison())); }
        else if (lox_tok == TK_BANG_EQ){ lox_next_token(); e = ln(ALLOC_node_neq(e, parse_comparison())); }
        else return e;
    }
}
static NODE *parse_comparison(void) {
    NODE *e = parse_term();
    for (;;) {
        if (lox_tok == TK_LT)      { lox_next_token(); e = ln(ALLOC_node_lt(e, parse_term())); }
        else if (lox_tok == TK_LE) { lox_next_token(); e = ln(ALLOC_node_le(e, parse_term())); }
        else if (lox_tok == TK_GT) { lox_next_token(); e = ln(ALLOC_node_gt(e, parse_term())); }
        else if (lox_tok == TK_GE) { lox_next_token(); e = ln(ALLOC_node_ge(e, parse_term())); }
        else return e;
    }
}
static NODE *parse_term(void) {
    NODE *e = parse_factor();
    for (;;) {
        if (lox_tok == TK_PLUS)       { lox_next_token(); e = ln(ALLOC_node_add(e, parse_factor())); }
        else if (lox_tok == TK_MINUS) { lox_next_token(); e = ln(ALLOC_node_sub(e, parse_factor())); }
        else return e;
    }
}
static NODE *parse_factor(void) {
    NODE *e = parse_unary();
    for (;;) {
        if (lox_tok == TK_STAR)        { lox_next_token(); e = ln(ALLOC_node_mul(e, parse_unary())); }
        else if (lox_tok == TK_SLASH)  { lox_next_token(); e = ln(ALLOC_node_div(e, parse_unary())); }
        else return e;
    }
}
static NODE *parse_unary(void) {
    if (lox_tok == TK_BANG)  { lox_next_token(); return ln(ALLOC_node_not(parse_unary())); }
    if (lox_tok == TK_MINUS) { lox_next_token(); return ln(ALLOC_node_neg(parse_unary())); }
    return parse_call();
}

static NODE *
parse_call(void)
{
    NODE *e = parse_primary();
    for (;;) {
        if (lox_tok == TK_LPAREN) {
            lox_next_token();
            NodeVec args = {0};
            if (lox_tok != TK_RPAREN) {
                do {
                    if (args.n >= 255) lox_parse_fail("Can't have more than 255 arguments.");
                    nv_push(&args, parse_expression());
                } while (match(TK_COMMA));
            }
            expect(TK_RPAREN, "')' after arguments");
            int idx = commit_args(&args);
            int cnt = args.n;
            e = ln(ALLOC_node_call(e, (uint32_t)idx, (uint32_t)cnt));
        } else if (lox_tok == TK_DOT) {
            lox_next_token();
            if (lox_tok != TK_IDENT) lox_parse_fail("Expect property name after '.'.");
            char *name = strdup(lox_tok_str);
            lox_next_token();
            e = ln(ALLOC_node_get(e, name));
        } else {
            return e;
        }
    }
}

static NODE *
parse_primary(void)
{
    switch (lox_tok) {
      case TK_NIL:    lox_next_token(); return ln(ALLOC_node_nil());
      case TK_TRUE:   lox_next_token(); return ln(ALLOC_node_true());
      case TK_FALSE:  lox_next_token(); return ln(ALLOC_node_false());
      case TK_NUMBER: { double v = lox_tok_num; lox_next_token(); return ln(ALLOC_node_number(v)); }
      case TK_STRING: { char *s = strndup(lox_tok_str, (size_t)lox_tok_len); lox_next_token(); NODE *r = ln(ALLOC_node_string(s)); return r; }
      case TK_IDENT:  { char nm[256]; strcpy(nm, lox_tok_str); lox_next_token(); return var_get(nm); }
      case TK_LPAREN: { lox_next_token(); NODE *e = parse_expression(); expect(TK_RPAREN, "')' after expression"); return e; }
      case TK_THIS:
        if (!g_in_class) lox_parse_fail("Can't use 'this' outside of a class.");
        lox_next_token();
        return var_get("this");
      case TK_SUPER: {
        if (!g_in_class) lox_parse_fail("Can't use 'super' outside of a class.");
        if (!g_class_has_super) lox_parse_fail("Can't use 'super' in a class with no superclass.");
        lox_next_token();
        expect(TK_DOT, "'.' after 'super'");
        if (lox_tok != TK_IDENT) lox_parse_fail("Expect superclass method name.");
        char *method = strdup(lox_tok_str);
        lox_next_token();
        uint32_t depth, slot;
        resolve_local("super", &depth, &slot);   // declared in the class's super scope
        return ln(ALLOC_node_super_get(depth, method));
      }
      default:        lox_parse_fail("Expect expression.");
    }
}

// ---- functions -------------------------------------------------------

// Parses `( params ) { body }` and registers a fundef; returns its index in
// LOX_FUNDEFS.  `name` is for display; the caller has already declared the
// binding (and, for methods, the enclosing this/super scopes).
static int
parse_fundef(const char *name, bool is_init)
{
    expect(TK_LPAREN, "'(' after function name");
    begin_scope();
    int arity = 0;
    if (lox_tok != TK_RPAREN) {
        do {
            if (arity >= 255) lox_parse_fail("Can't have more than 255 parameters.");
            if (lox_tok != TK_IDENT) lox_parse_fail("Expect parameter name.");
            int slot = declare_local(lox_tok_str); mark_defined(slot);
            arity++;
            lox_next_token();
        } while (match(TK_COMMA));
    }
    expect(TK_RPAREN, "')' after parameters");
    expect(TK_LBRACE, "'{' before function body");

    g_in_function++;
    NodeVec body = {0};
    while (lox_tok != TK_RBRACE && lox_tok != TK_EOF) nv_push(&body, parse_declaration());
    expect(TK_RBRACE, "'}' after function body");
    g_in_function--;

    int nslots = end_scope();
    int idx = commit_stmts(&body);
    int cnt = body.n;
    NODE *bodynode = ln(ALLOC_node_stmts((uint32_t)idx, (uint32_t)cnt));
    add_entry(bodynode);

    struct lox_fundef *fn = GC_MALLOC(sizeof(struct lox_fundef));
    fn->name = strdup(name); fn->arity = arity; fn->nslots = nslots;
    fn->body = bodynode; fn->is_init = is_init;
    return fv_push(&g_fundefs, fn);
}

static NODE *parse_fun_closure(const char *name) { return ln(ALLOC_node_closure((uint32_t)parse_fundef(name, false))); }

// ---- statements ------------------------------------------------------

// `{ ... }` as a scoped block statement.
static NODE *
parse_block_node(void)
{
    expect(TK_LBRACE, "'{'");
    begin_scope();
    NodeVec stmts = {0};
    while (lox_tok != TK_RBRACE && lox_tok != TK_EOF) nv_push(&stmts, parse_declaration());
    expect(TK_RBRACE, "'}' after block");
    int nslots = end_scope();
    int idx = commit_stmts(&stmts);
    int cnt = stmts.n;
    return ln(ALLOC_node_block((uint32_t)nslots, (uint32_t)idx, (uint32_t)cnt));
}

// var name ("=" expr)? ";"
static NODE *
parse_var_decl(void)
{
    if (lox_tok != TK_IDENT) lox_parse_fail("Expect variable name.");
    char name[256]; strcpy(name, lox_tok_str);
    lox_next_token();

    bool global = (g_nscopes == 0);
    int slot = global ? -1 : declare_local(name);   // local: declared, not yet defined

    NODE *init;
    if (match(TK_EQ)) init = parse_expression();
    else              init = ln(ALLOC_node_nil());
    expect(TK_SEMI, "';' after variable declaration");

    if (global) return ln(ALLOC_node_define_global(strdup(name), init));
    mark_defined(slot);
    return ln(ALLOC_node_define_local((uint32_t)slot, init));
}

// fun name ( ... ) { ... }  — a named function declaration / binding.
static NODE *
parse_fun_decl(void)
{
    if (lox_tok != TK_IDENT) lox_parse_fail("Expect function name.");
    char name[256]; strcpy(name, lox_tok_str);
    lox_next_token();

    bool global = (g_nscopes == 0);
    int slot = -1;
    if (!global) { slot = declare_local(name); mark_defined(slot); }  // defined before body → recursion

    NODE *closure = parse_fun_closure(name);
    if (global) return ln(ALLOC_node_define_global(strdup(name), closure));
    return ln(ALLOC_node_define_local((uint32_t)slot, closure));
}

// class Name ( "<" Super )? "{" method* "}"
static NODE *
parse_class(void)
{
    if (lox_tok != TK_IDENT) lox_parse_fail("Expect class name.");
    char name[256]; strcpy(name, lox_tok_str);
    lox_next_token();

    bool global = (g_nscopes == 0);
    int slot = -1;
    if (!global) { slot = declare_local(name); mark_defined(slot); }

    NODE *superNode;
    bool has_super = false;
    if (match(TK_LT)) {
        if (lox_tok != TK_IDENT) lox_parse_fail("Expect superclass name.");
        if (strcmp(lox_tok_str, name) == 0) lox_parse_fail("A class can't inherit from itself.");
        superNode = var_get(lox_tok_str);
        lox_next_token();
        has_super = true;
    } else {
        superNode = ln(ALLOC_node_nil());
    }

    int saved_has_super = g_class_has_super;
    g_class_has_super = has_super;
    if (has_super) { begin_scope(); mark_defined(declare_local("super")); }
    begin_scope(); mark_defined(declare_local("this"));
    g_in_class++;

    expect(TK_LBRACE, "'{' before class body");
    U32Vec methods = {0};
    while (lox_tok != TK_RBRACE && lox_tok != TK_EOF) {
        if (lox_tok != TK_IDENT) lox_parse_fail("Expect method name.");
        char mname[256]; strcpy(mname, lox_tok_str);
        lox_next_token();
        u32_push(&methods, (uint32_t)parse_fundef(mname, strcmp(mname, "init") == 0));
    }
    expect(TK_RBRACE, "'}' after class body");

    g_in_class--;
    end_scope();                      // this
    if (has_super) end_scope();       // super
    g_class_has_super = saved_has_super;

    int midx = g_class_methods.n;
    for (int i = 0; i < methods.n; i++) u32_push(&g_class_methods, methods.v[i]);
    int mcnt = methods.n;

    NODE *classexpr = ln(ALLOC_node_classexpr(strdup(name), superNode, (uint32_t)midx, (uint32_t)mcnt));
    if (global) return ln(ALLOC_node_define_global(strdup(name), classexpr));
    return ln(ALLOC_node_define_local((uint32_t)slot, classexpr));
}

// for ( init ; cond ; incr ) body   — desugared to a scoped while loop.
static NODE *
parse_for(void)
{
    expect(TK_LPAREN, "'(' after 'for'");
    begin_scope();   // the for's own scope (holds a `var` initializer)

    NODE *init = NULL;
    if (match(TK_SEMI))            init = NULL;
    else if (lox_tok == TK_VAR)  { lox_next_token(); init = parse_var_decl(); }
    else { NODE *e = parse_expression(); expect(TK_SEMI, "';' after loop initializer"); init = ln(ALLOC_node_exprstmt(e)); }

    NODE *cond;
    if (lox_tok != TK_SEMI) cond = parse_expression(); else cond = ln(ALLOC_node_true());
    expect(TK_SEMI, "';' after loop condition");

    NODE *incr = NULL;
    if (lox_tok != TK_RPAREN) incr = parse_expression();
    expect(TK_RPAREN, "')' after for clauses");

    NODE *body = parse_statement();

    // while-body = body (+ incr), run in the for scope without a new frame.
    NODE *wbody;
    if (incr) {
        NodeVec wv = {0};
        nv_push(&wv, body);
        nv_push(&wv, ln(ALLOC_node_exprstmt(incr)));
        int idx = commit_stmts(&wv); int cnt = wv.n;
        wbody = ln(ALLOC_node_stmts((uint32_t)idx, (uint32_t)cnt));
        add_entry(wbody);
    } else {
        wbody = body;
    }
    NODE *loop = ln(ALLOC_node_while(cond, wbody));

    // wrap: { init?; while(cond) wbody }
    NodeVec outer = {0};
    if (init) nv_push(&outer, init);
    nv_push(&outer, loop);
    int nslots = end_scope();
    int idx = commit_stmts(&outer); int cnt = outer.n;
    return ln(ALLOC_node_block((uint32_t)nslots, (uint32_t)idx, (uint32_t)cnt));
}

static NODE *
parse_statement(void)
{
    switch (lox_tok) {
      case TK_PRINT: {
        lox_next_token();
        NODE *e = parse_expression();
        expect(TK_SEMI, "';' after value");
        return ln(ALLOC_node_print(e));
      }
      case TK_LBRACE: return parse_block_node();
      case TK_IF: {
        lox_next_token();
        expect(TK_LPAREN, "'(' after 'if'");
        NODE *cond = parse_expression();
        expect(TK_RPAREN, "')' after if condition");
        NODE *thn = parse_statement();
        NODE *els = match(TK_ELSE) ? parse_statement() : ln(ALLOC_node_nil());
        return ln(ALLOC_node_if(cond, thn, els));
      }
      case TK_WHILE: {
        lox_next_token();
        expect(TK_LPAREN, "'(' after 'while'");
        NODE *cond = parse_expression();
        expect(TK_RPAREN, "')' after condition");
        NODE *body = parse_statement();
        return ln(ALLOC_node_while(cond, body));
      }
      case TK_FOR: lox_next_token(); return parse_for();
      case TK_RETURN: {
        lox_next_token();
        if (g_in_function == 0) lox_parse_fail("Can't return from top-level code.");
        NODE *value = (lox_tok == TK_SEMI) ? ln(ALLOC_node_nil()) : parse_expression();
        expect(TK_SEMI, "';' after return value");
        return ln(ALLOC_node_return(value));
      }
      default: {
        NODE *e = parse_expression();
        expect(TK_SEMI, "';' after expression");
        return ln(ALLOC_node_exprstmt(e));
      }
    }
}

static NODE *
parse_declaration(void)
{
    if (lox_tok == TK_VAR) { lox_next_token(); return parse_var_decl(); }
    if (lox_tok == TK_FUN) { lox_next_token(); return parse_fun_decl(); }
    if (lox_tok == TK_CLASS) { lox_next_token(); return parse_class(); }
    return parse_statement();
}

// ---- entry point -----------------------------------------------------

Program
lox_parse_program(const char *src)
{
    Program prog = { NULL, 0 };
    g_call_args = (NodeVec){0}; g_block_stmts = (NodeVec){0}; g_entries = (NodeVec){0};
    g_fundefs = (FunVec){0}; g_class_methods = (U32Vec){0};
    g_nscopes = 0; g_in_function = 0; g_in_class = 0; g_class_has_super = 0;

    lox_init_lexer(src);
    lox_next_token();

    NodeVec top = {0};
    while (lox_tok != TK_EOF) nv_push(&top, parse_declaration());
    int idx = commit_stmts(&top);
    int cnt = top.n;
    NODE *root = ln(ALLOC_node_stmts((uint32_t)idx, (uint32_t)cnt));
    add_entry(root);

    LOX_CALL_ARGS = g_call_args.v;
    LOX_BLOCK_STMTS = g_block_stmts.v;
    LOX_FUNDEFS = g_fundefs.v;
    LOX_CLASS_METHODS = g_class_methods.v;
    lox_entries = g_entries.v;
    lox_n_entries = g_entries.n;

    prog.body = root;
    prog.ok = 1;
    return prog;
}
