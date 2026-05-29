// Recursive-descent parser for AnPy (ChocoPy §4 grammar).
//
// Builds the AST (ALLOC_node_*) plus the static descriptors the checker
// and runtime need: top-level vardefs/funcdefs/classes, per-function
// locals/nested-funcs/global-nonlocal decls, and per-class attrs/methods.
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <gc.h>
#include "parse.h"

// Descriptor arrays consumed by node.c::anpy_install_globals.
anpy_func     **anpy_top_funcs = NULL;  int anpy_n_top_funcs = 0;
struct var_decl *anpy_top_vars = NULL;  int anpy_n_top_vars = 0;

extern const struct NodeKind kind_node_name;
extern const struct NodeKind kind_node_attr;
extern const struct NodeKind kind_node_index;

typedef struct {
    Token *t; int ntok, pos;
    jmp_buf jb; int err;
    Program *prog;
} P;

static NODE *NIL(void) { static NODE *n = NULL; if (!n) n = ALLOC_node_nil(); return n; }

static void fail(P *p, const char *msg) {
    fprintf(stderr, "anpy: syntax error at line %d: %s\n", p->t[p->pos].line, msg);
    p->err = 1; longjmp(p->jb, 1);
}

static Token *cur(P *p) { return &p->t[p->pos]; }
static enum tok_type pk(P *p) { return p->t[p->pos].type; }
static enum tok_type pk2(P *p) { return p->t[p->pos + 1].type; }
static Token *adv(P *p) { Token *k = &p->t[p->pos]; if (k->type != TK_EOF) p->pos++; return k; }
static int accept(P *p, enum tok_type t) { if (pk(p) == t) { adv(p); return 1; } return 0; }
static Token *expect(P *p, enum tok_type t, const char *m) { if (pk(p) != t) fail(p, m); return adv(p); }
static int is_kw(P *p, const char *kw) { return pk(p) == TK_NAME && strcmp(cur(p)->text, kw) == 0; }

// dynamic array helper
#define PUSH(arr, cnt, cap, val) do { \
    if ((cnt) == (cap)) { (cap) = (cap) ? (cap)*2 : 8; (arr) = GC_REALLOC((arr), sizeof(*(arr))*(cap)); } \
    (arr)[(cnt)++] = (val); } while (0)

static NODE *parse_expr(P *p);
static NODE *parse_suite(P *p, anpy_func *fn);   // : NEWLINE INDENT stmt+ DEDENT (fn for context only)
static NODE *parse_stmt(P *p);

// ---- types -----------------------------------------------------------

static Type *parse_type(P *p) {
    if (accept(p, TK_LB)) { Type *e = parse_type(p); expect(p, TK_RB, "expected ']' in type"); return type_list(e); }
    if (pk(p) == TK_NAME)   { return type_class(adv(p)->text); }
    if (pk(p) == TK_STRING) { return type_class(adv(p)->text); }   // quoted class name
    fail(p, "expected a type"); return NULL;
}

// ---- literals (for vardef/attr initialisers) ------------------------

static NODE *parse_literal(P *p) {
    if (is_kw(p, "None"))  { adv(p); return ALLOC_node_none(); }
    if (is_kw(p, "True"))  { adv(p); return ALLOC_node_bool(1); }
    if (is_kw(p, "False")) { adv(p); return ALLOC_node_bool(0); }
    if (pk(p) == TK_INT)    return ALLOC_node_int((int32_t)adv(p)->ival);
    if (pk(p) == TK_STRING)  return ALLOC_node_strlit(adv(p)->text);
    fail(p, "expected a literal"); return NULL;
}

// ---- expressions -----------------------------------------------------

static NODE *parse_atom(P *p);

static NODE *parse_postfix(P *p) {
    NODE *e = parse_atom(p);
    for (;;) {
        if (accept(p, TK_DOT)) {
            Token *id = expect(p, TK_NAME, "expected attribute name");
            if (pk(p) == TK_LP) {
                adv(p);
                NODE *args = NIL();
                NODE *list[256]; int n = 0;
                if (pk(p) != TK_RP) { do { list[n++] = parse_expr(p); } while (accept(p, TK_COMMA) && n < 256); }
                expect(p, TK_RP, "expected ')'");
                for (int i = n - 1; i >= 0; i--) args = ALLOC_node_elt(list[i], args);
                e = ALLOC_node_method(e, id->text, args);
            } else {
                e = ALLOC_node_attr(e, id->text);
            }
        } else if (accept(p, TK_LB)) {
            NODE *idx = parse_expr(p);
            expect(p, TK_RB, "expected ']'");
            e = ALLOC_node_index(e, idx);
        } else break;
    }
    return e;
}

static NODE *parse_unary(P *p) {
    if (accept(p, TK_MINUS)) return ALLOC_node_neg(parse_unary(p));
    return parse_postfix(p);
}

static NODE *parse_muldiv(P *p) {
    NODE *l = parse_unary(p);
    for (;;) {
        if (accept(p, TK_STAR))        l = ALLOC_node_mul(l, parse_unary(p));
        else if (accept(p, TK_FSLASH)) l = ALLOC_node_fdiv(l, parse_unary(p));
        else if (accept(p, TK_PCT))    l = ALLOC_node_mod(l, parse_unary(p));
        else break;
    }
    return l;
}

static NODE *parse_addsub(P *p) {
    NODE *l = parse_muldiv(p);
    for (;;) {
        if (accept(p, TK_PLUS))       l = ALLOC_node_add(l, parse_muldiv(p));
        else if (accept(p, TK_MINUS)) l = ALLOC_node_sub(l, parse_muldiv(p));
        else break;
    }
    return l;
}

// comparisons + `is` are non-associative (one only).
static NODE *parse_compare(P *p) {
    NODE *l = parse_addsub(p);
    enum tok_type t = pk(p);
    if (t == TK_LT)  { adv(p); return ALLOC_node_lt(l, parse_addsub(p)); }
    if (t == TK_LE)  { adv(p); return ALLOC_node_le(l, parse_addsub(p)); }
    if (t == TK_GT)  { adv(p); return ALLOC_node_gt(l, parse_addsub(p)); }
    if (t == TK_GE)  { adv(p); return ALLOC_node_ge(l, parse_addsub(p)); }
    if (t == TK_EQEQ){ adv(p); return ALLOC_node_eq(l, parse_addsub(p)); }
    if (t == TK_NE)  { adv(p); return ALLOC_node_ne(l, parse_addsub(p)); }
    if (is_kw(p, "is")) { adv(p); return ALLOC_node_is(l, parse_addsub(p)); }
    return l;
}

static NODE *parse_not(P *p) {
    if (is_kw(p, "not")) { adv(p); return ALLOC_node_not(parse_not(p)); }
    return parse_compare(p);
}

static NODE *parse_and(P *p) {
    NODE *l = parse_not(p);
    while (is_kw(p, "and")) { adv(p); l = ALLOC_node_and(l, parse_not(p)); }
    return l;
}

static NODE *parse_or(P *p) {
    NODE *l = parse_and(p);
    while (is_kw(p, "or")) { adv(p); l = ALLOC_node_or(l, parse_and(p)); }
    return l;
}

// ternary: a if c else b   (lowest precedence, right-assoc on else)
static NODE *parse_expr(P *p) {
    NODE *e = parse_or(p);
    if (is_kw(p, "if")) {
        adv(p);
        NODE *cond = parse_or(p);
        if (!is_kw(p, "else")) fail(p, "expected 'else' in conditional expression");
        adv(p);
        NODE *els = parse_expr(p);
        return ALLOC_node_cond(e, cond, els);
    }
    return e;
}

static NODE *parse_atom(P *p) {
    Token *t = cur(p);
    switch (t->type) {
      case TK_INT:    adv(p); return ALLOC_node_int((int32_t)t->ival);
      case TK_STRING: adv(p); return ALLOC_node_strlit(t->text);
      case TK_LP: {
        adv(p); NODE *e = parse_expr(p); expect(p, TK_RP, "expected ')'"); return e;
      }
      case TK_LB: {
        adv(p);
        NODE *items = NIL();
        NODE *list[1024]; int n = 0;
        if (pk(p) != TK_RB) { do { list[n++] = parse_expr(p); } while (accept(p, TK_COMMA) && n < 1024); }
        expect(p, TK_RB, "expected ']'");
        for (int i = n - 1; i >= 0; i--) items = ALLOC_node_elt(list[i], items);
        return ALLOC_node_listexpr(items);
      }
      case TK_NAME: {
        const char *name = t->text;
        if (strcmp(name, "None") == 0)  { adv(p); return ALLOC_node_none(); }
        if (strcmp(name, "True") == 0)  { adv(p); return ALLOC_node_bool(1); }
        if (strcmp(name, "False") == 0) { adv(p); return ALLOC_node_bool(0); }
        adv(p);
        if (pk(p) == TK_LP) {       // call: ID(args)
            adv(p);
            NODE *args = NIL();
            NODE *list[256]; int n = 0;
            if (pk(p) != TK_RP) { do { list[n++] = parse_expr(p); } while (accept(p, TK_COMMA) && n < 256); }
            expect(p, TK_RP, "expected ')'");
            for (int i = n - 1; i >= 0; i--) args = ALLOC_node_elt(list[i], args);
            return ALLOC_node_call(name, args);
        }
        return ALLOC_node_name(name);
      }
      default: fail(p, "expected an expression"); return NULL;
    }
}

// ---- targets / assignment -------------------------------------------

static NODE *make_assign_single(P *p, NODE *tgt, NODE *rhs) {
    if (tgt->head.kind == &kind_node_name)
        return ALLOC_node_assign_var(tgt->u.node_name.name, rhs);
    if (tgt->head.kind == &kind_node_attr)
        return ALLOC_node_assign_attr(tgt->u.node_attr.obj, tgt->u.node_attr.name, rhs);
    if (tgt->head.kind == &kind_node_index)
        return ALLOC_node_assign_index(tgt->u.node_index.seq, tgt->u.node_index.idx, rhs);
    fail(p, "invalid assignment target"); return NULL;
}

static NODE *make_tgt(P *p, NODE *tgt, NODE *next) {
    if (tgt->head.kind == &kind_node_name)
        return ALLOC_node_tgt(0, tgt->u.node_name.name, NIL(), NIL(), next);
    if (tgt->head.kind == &kind_node_attr)
        return ALLOC_node_tgt(1, tgt->u.node_attr.name, tgt->u.node_attr.obj, NIL(), next);
    if (tgt->head.kind == &kind_node_index)
        return ALLOC_node_tgt(2, "", tgt->u.node_index.seq, tgt->u.node_index.idx, next);
    fail(p, "invalid assignment target"); return NULL;
}

// ---- simple statements ----------------------------------------------

static NODE *parse_simple(P *p) {
    if (is_kw(p, "pass")) { adv(p); expect(p, TK_NEWLINE, "expected newline"); return NIL(); }
    if (is_kw(p, "return")) {
        adv(p);
        if (pk(p) == TK_NEWLINE) { adv(p); return ALLOC_node_return_none(); }
        NODE *e = parse_expr(p);
        expect(p, TK_NEWLINE, "expected newline");
        return ALLOC_node_return(e);
    }
    // expr or [target =]+ expr
    NODE *exprs[64]; int ne = 0;
    exprs[ne++] = parse_expr(p);
    while (accept(p, TK_ASSIGN)) exprs[ne++] = parse_expr(p);
    expect(p, TK_NEWLINE, "expected newline");
    if (ne == 1) return ALLOC_node_exprstmt(exprs[0]);
    NODE *rhs = exprs[ne - 1];
    if (ne == 2) return make_assign_single(p, exprs[0], rhs);
    NODE *chain = NIL();
    for (int i = ne - 2; i >= 0; i--) chain = make_tgt(p, exprs[i], chain);
    return ALLOC_node_massign(rhs, chain);
}

// stmt list until DEDENT/EOF -> right-nested seq
static NODE *parse_stmt_list(P *p) {
    NODE *list[8192]; int n = 0;
    while (pk(p) != TK_DEDENT && pk(p) != TK_EOF) list[n++] = parse_stmt(p);
    if (n == 0) return NIL();
    NODE *seq = list[n - 1];
    for (int i = n - 2; i >= 0; i--) seq = ALLOC_node_seq(list[i], seq);
    return seq;
}

static NODE *parse_suite(P *p, anpy_func *fn) {
    (void)fn;
    expect(p, TK_COLON, "expected ':'");
    expect(p, TK_NEWLINE, "expected newline before block");
    expect(p, TK_INDENT, "expected indented block");
    NODE *body = parse_stmt_list(p);
    expect(p, TK_DEDENT, "expected dedent");
    return body;
}

static NODE *parse_stmt(P *p) {
    if (is_kw(p, "if")) {
        adv(p);
        NODE *cond = parse_expr(p);
        NODE *then = parse_suite(p, NULL);
        NODE *els = NIL();
        if (is_kw(p, "elif")) { els = parse_stmt(p); /* recurse as if */ return ALLOC_node_if(cond, then, els); }
        if (is_kw(p, "else")) { adv(p); els = parse_suite(p, NULL); }
        return ALLOC_node_if(cond, then, els);
    }
    if (is_kw(p, "elif")) {   // reached via recursion from `if`
        adv(p);
        NODE *cond = parse_expr(p);
        NODE *then = parse_suite(p, NULL);
        NODE *els = NIL();
        if (is_kw(p, "elif")) els = parse_stmt(p);
        else if (is_kw(p, "else")) { adv(p); els = parse_suite(p, NULL); }
        return ALLOC_node_if(cond, then, els);
    }
    if (is_kw(p, "while")) {
        adv(p);
        NODE *cond = parse_expr(p);
        NODE *body = parse_suite(p, NULL);
        return ALLOC_node_while(cond, body);
    }
    if (is_kw(p, "for")) {
        adv(p);
        Token *id = expect(p, TK_NAME, "expected loop variable");
        if (!is_kw(p, "in")) fail(p, "expected 'in'");
        adv(p);
        NODE *it = parse_expr(p);
        NODE *body = parse_suite(p, NULL);
        return ALLOC_node_for(id->text, it, body);
    }
    return parse_simple(p);
}

// ---- declarations ----------------------------------------------------

static int looks_like_vardef(P *p) { return pk(p) == TK_NAME && pk2(p) == TK_COLON; }

static struct var_decl parse_vardef(P *p) {
    struct var_decl d; memset(&d, 0, sizeof(d));
    d.name = expect(p, TK_NAME, "expected variable name")->text;
    expect(p, TK_COLON, "expected ':'");
    d.type = parse_type(p);
    expect(p, TK_ASSIGN, "expected '=' in definition");
    d.init = parse_literal(p);
    expect(p, TK_NEWLINE, "expected newline");
    return d;
}

static anpy_func *parse_funcdef(P *p, struct anpy_class *cls);

// funcbody: [globaldecl|nonlocaldecl|vardef|funcdef]* stmt+
static void parse_funcbody(P *p, anpy_func *fn) {
    int cap_v=0, cap_n=0, cap_g=0, cap_l=0;
    for (;;) {
        if (is_kw(p, "global")) {
            adv(p); const char *nm = expect(p, TK_NAME, "expected name")->text; expect(p, TK_NEWLINE, "newline");
            PUSH(fn->globals, fn->nglobals, cap_g, nm);
        } else if (is_kw(p, "nonlocal")) {
            adv(p); const char *nm = expect(p, TK_NAME, "expected name")->text; expect(p, TK_NEWLINE, "newline");
            PUSH(fn->nonlocals, fn->nnonlocals, cap_l, nm);
        } else if (is_kw(p, "def")) {
            anpy_func *nested = parse_funcdef(p, NULL);
            PUSH(fn->nested, fn->nnested, cap_n, nested);
        } else if (looks_like_vardef(p)) {
            struct var_decl d = parse_vardef(p);
            PUSH(fn->vars, fn->nvars, cap_v, d);
        } else break;
    }
    fn->body = parse_stmt_list(p);
}

static anpy_func *parse_funcdef(P *p, struct anpy_class *cls) {
    expect(p, TK_NAME, "expected 'def'");   // 'def' already matched by caller via is_kw; consume
    anpy_func *fn = GC_MALLOC(sizeof(anpy_func));
    memset(fn, 0, sizeof(*fn));
    fn->name = expect(p, TK_NAME, "expected function name")->text;
    fn->is_method = (cls != NULL); fn->cls = cls;
    expect(p, TK_LP, "expected '('");
    int capp = 0;
    if (pk(p) != TK_RP) {
        do {
            const char *pn = expect(p, TK_NAME, "expected parameter name")->text;
            expect(p, TK_COLON, "expected ':'");
            Type *pt = parse_type(p);
            PUSH(fn->params, fn->nparams, capp, pn);
            // store param type in a parallel array
            fn->param_types = GC_REALLOC(fn->param_types, sizeof(Type*) * fn->nparams);
            fn->param_types[fn->nparams - 1] = pt;
        } while (accept(p, TK_COMMA));
    }
    expect(p, TK_RP, "expected ')'");
    fn->ret_type = type_none();
    if (accept(p, TK_ARROW)) fn->ret_type = parse_type(p);
    expect(p, TK_COLON, "expected ':'");
    expect(p, TK_NEWLINE, "expected newline");
    expect(p, TK_INDENT, "expected indented body");
    parse_funcbody(p, fn);
    expect(p, TK_DEDENT, "expected dedent");
    return fn;
}

static anpy_class *parse_classdef(P *p) {
    adv(p);   // 'class'
    anpy_class *cls = GC_MALLOC(sizeof(anpy_class));
    memset(cls, 0, sizeof(*cls));
    cls->name = expect(p, TK_NAME, "expected class name")->text;
    expect(p, TK_LP, "expected '('");
    cls->super_name = expect(p, TK_NAME, "expected superclass")->text;
    expect(p, TK_RP, "expected ')'");
    expect(p, TK_COLON, "expected ':'");
    expect(p, TK_NEWLINE, "expected newline");
    expect(p, TK_INDENT, "expected indented class body");
    int cap_a = 0, cap_m = 0;
    if (is_kw(p, "pass")) { adv(p); expect(p, TK_NEWLINE, "newline"); }
    else {
        while (pk(p) != TK_DEDENT && pk(p) != TK_EOF) {
            if (is_kw(p, "def")) {
                anpy_func *m = parse_funcdef(p, cls);
                struct method_ent e = { m->name, m };
                PUSH(cls->own_methods, cls->own_nmethods, cap_m, e);
            } else if (looks_like_vardef(p)) {
                struct var_decl d = parse_vardef(p);
                PUSH(cls->own_attrs, cls->own_nattrs, cap_a, d);
            } else fail(p, "expected attribute or method in class body");
        }
    }
    expect(p, TK_DEDENT, "expected dedent");
    return cls;
}

// ---- program ---------------------------------------------------------

Program
parse_program(const char *src)
{
    Program prog; memset(&prog, 0, sizeof(prog));
    P p; memset(&p, 0, sizeof(p));
    p.prog = &prog;

    int ntok, errline = 0;
    Token *toks = anpy_tokenize(src, &ntok, &errline);
    if (!toks) { fprintf(stderr, "anpy: lexical error at line %d\n", errline); prog.ok = 0; return prog; }
    p.t = toks; p.ntok = ntok; p.pos = 0;

    if (setjmp(p.jb)) { prog.ok = 0; return prog; }

    // reset global descriptor arrays
    anpy_top_funcs = NULL; anpy_n_top_funcs = 0;
    anpy_top_vars = NULL;  anpy_n_top_vars = 0;
    int cap_f = 0, cap_v = 0, cap_c = 0;

    // declarations* (vardef | funcdef | classdef)
    for (;;) {
        if (is_kw(&p, "class")) {
            anpy_class *cls = parse_classdef(&p);
            anpy_register_class(cls);
            PUSH(prog.classes, prog.nclasses, cap_c, cls);
        } else if (is_kw(&p, "def")) {
            anpy_func *fn = parse_funcdef(&p, NULL);
            PUSH(anpy_top_funcs, anpy_n_top_funcs, cap_f, fn);
        } else if (looks_like_vardef(&p)) {
            struct var_decl d = parse_vardef(&p);
            PUSH(anpy_top_vars, anpy_n_top_vars, cap_v, d);
        } else break;
    }
    // top-level statements
    prog.body = parse_stmt_list(&p);
    prog.vars = anpy_top_vars;   prog.nvars = anpy_n_top_vars;
    prog.funcs = anpy_top_funcs; prog.nfuncs = anpy_n_top_funcs;
    prog.ok = 1;
    return prog;
}
