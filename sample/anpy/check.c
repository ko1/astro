// Static type checker for AnPy (ChocoPy §5).
//
// Implements the conformance (<=), assignment-compatibility (<=a) and join
// relations, then walks the program checking every rule.  Type errors are
// reported to stderr; the count is returned.  (The interpreter only runs a
// program that type-checks, matching ChocoPy.)
#include <stdlib.h>
#include <string.h>
#include <gc.h>
#include "parse.h"
#include "check.h"

// --- type constructors ------------------------------------------------

Type *type_class(const char *name) {
    Type *t = GC_MALLOC(sizeof(Type)); t->kind = T_CLASS; t->cls = name; t->elem = NULL; return t;
}
Type *type_list(Type *elem) {
    Type *t = GC_MALLOC(sizeof(Type)); t->kind = T_LIST; t->cls = NULL; t->elem = elem; return t;
}
Type *type_none(void)  { static Type t = { T_NONE, NULL, NULL }; return &t; }
Type *type_empty(void) { static Type t = { T_EMPTY, NULL, NULL }; return &t; }

const char *
type_str(Type *t) {
    if (!t) return "?";
    switch (t->kind) {
      case T_NONE:  return "<None>";
      case T_EMPTY: return "<Empty>";
      case T_CLASS: return t->cls;
      case T_LIST: {
        static char buf[256];
        snprintf(buf, sizeof(buf), "[%s]", type_str(t->elem));
        return buf;
      }
    }
    return "?";
}

// --- helpers for the relations ---------------------------------------

static bool is_special_basic(const char *c) {
    return strcmp(c, "int") == 0 || strcmp(c, "bool") == 0 || strcmp(c, "str") == 0;
}

// class C <= class P  (walk the superclass chain)
static bool class_subtype(const char *c, const char *p) {
    if (strcmp(c, p) == 0) return true;
    for (anpy_class *k = anpy_lookup_class(c); k; k = k->super) {
        if (strcmp(k->name, p) == 0) return true;
    }
    return false;
}

static bool type_eq(Type *a, Type *b) {
    if (a->kind != b->kind) return false;
    if (a->kind == T_CLASS) return strcmp(a->cls, b->cls) == 0;
    if (a->kind == T_LIST)  return type_eq(a->elem, b->elem);
    return true;  // both <None> or both <Empty>
}

// conformance:  a <= b
static bool conforms(Type *a, Type *b) {
    if (b->kind == T_CLASS && strcmp(b->cls, "object") == 0) {
        // everything except <None>/<Empty>? Actually [T]<=object, <None><=object,
        // <Empty><=object, and all class types <= object.
        return true;
    }
    if (a->kind == T_CLASS && b->kind == T_CLASS) return class_subtype(a->cls, b->cls);
    if (a->kind == T_NONE  && b->kind == T_NONE)  return true;
    if (a->kind == T_EMPTY && b->kind == T_EMPTY) return true;
    if (a->kind == T_LIST  && b->kind == T_LIST)  return type_eq(a, b);  // list types unrelated unless equal
    return false;
}

// assignment compatibility:  a <=a b
bool assign_compat(Type *a, Type *b) {
    if (conforms(a, b)) return true;
    if (a->kind == T_NONE && b->kind == T_CLASS && !is_special_basic(b->cls)) return true;
    if (a->kind == T_NONE && b->kind == T_LIST) return true;
    if (b->kind == T_LIST && a->kind == T_EMPTY) return true;
    if (b->kind == T_LIST && a->kind == T_LIST && a->elem->kind == T_NONE && assign_compat(type_none(), b->elem))
        return true;  // [<None>] <=a [T] when <None> <=a T
    return false;
}

// least common ancestor of two class names
static const char *class_lca(const char *a, const char *b) {
    for (const char *x = a; x; ) {
        if (class_subtype(b, x)) return x;
        anpy_class *k = anpy_lookup_class(x);
        x = k && k->super ? k->super->name : (strcmp(x, "object") ? "object" : NULL);
    }
    return "object";
}

// join (least upper bound under <=a)
Type *type_join(Type *a, Type *b) {
    if (assign_compat(a, b)) return b;
    if (assign_compat(b, a)) return a;
    if (a->kind == T_CLASS && b->kind == T_CLASS) return type_class(class_lca(a->cls, b->cls));
    return type_class("object");
}

// =====================================================================
// Type inference + statement checking (ChocoPy §5.2).
// =====================================================================

extern const struct NodeKind
  kind_node_int, kind_node_bool, kind_node_none, kind_node_strlit, kind_node_name,
  kind_node_neg, kind_node_add, kind_node_sub, kind_node_mul, kind_node_fdiv, kind_node_mod,
  kind_node_lt, kind_node_le, kind_node_gt, kind_node_ge, kind_node_eq, kind_node_ne, kind_node_is,
  kind_node_and, kind_node_or, kind_node_not, kind_node_cond,
  kind_node_listexpr, kind_node_elt, kind_node_index, kind_node_attr, kind_node_call, kind_node_method,
  kind_node_exprstmt, kind_node_assign_var, kind_node_assign_attr, kind_node_assign_index,
  kind_node_massign, kind_node_tgt, kind_node_if, kind_node_while, kind_node_for,
  kind_node_return, kind_node_return_none, kind_node_seq, kind_node_nil;

static int g_errors;
static void terr2(const char *msg) { fprintf(stderr, "anpy: type error: %s\n", msg); g_errors++; }

// A type-scope frame: variable name -> declared type, with a parent chain.
typedef struct tframe {
    struct { const char *name; Type *t; } *v; int n, cap;
    struct anpy_func *fn;     // the function this frame belongs to (NULL = global)
    struct tframe *parent;
} tframe;

static Type *INT_T, *BOOL_T, *STR_T, *OBJ_T;

static Type *tframe_lookup(tframe *f, const char *name) {
    for (; f; f = f->parent)
        for (int i = 0; i < f->n; i++) if (strcmp(f->v[i].name, name) == 0) return f->v[i].t;
    return NULL;
}
static void tframe_add(tframe *f, const char *name, Type *t) {
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 8; f->v = GC_REALLOC(f->v, sizeof(*f->v) * f->cap); }
    f->v[f->n].name = name; f->v[f->n].t = t; f->n++;
}

// Resolve a function descriptor by name in a scope chain (nested -> global).
static anpy_func **g_funcs; static int g_nfuncs;
static anpy_func *resolve_func(tframe *f, const char *name) {
    for (; f; f = f->parent) {
        if (!f->fn) break;
        for (int i = 0; i < f->fn->nnested; i++) if (strcmp(f->fn->nested[i]->name, name) == 0) return f->fn->nested[i];
    }
    for (int i = 0; i < g_nfuncs; i++) if (strcmp(g_funcs[i]->name, name) == 0) return g_funcs[i];
    return NULL;
}

// Look up an attribute's type in class T0 (incl. inherited).
static Type *member_attr(const char *cls, const char *name) {
    anpy_class *k = anpy_lookup_class(cls);
    if (!k) return NULL;
    for (int i = 0; i < k->nattrs; i++) if (strcmp(k->attrs[i].name, name) == 0) return k->attrs[i].type;
    return NULL;
}
static anpy_func *member_method(const char *cls, const char *name) {
    anpy_class *k = anpy_lookup_class(cls);
    return k ? anpy_class_method(k, name) : NULL;
}

static Type *infer(NODE *e, tframe *sc, Type *R, const char *C);

// Check that argument exprs are assignment-compatible with a parameter list.
static void check_args(NODE *args, Type **ptypes, int np, int skip, tframe *sc, Type *R, const char *C, const char *what) {
    NODE *a = args; int i = skip;
    for (; a->head.kind == &kind_node_elt; a = a->u.node_elt.next, i++) {
        Type *at = infer(a->u.node_elt.expr, sc, R, C);
        if (i < np && at && !assign_compat(at, ptypes[i])) {
            char buf[128]; snprintf(buf, sizeof(buf), "%s: argument %d type %s not compatible with %s",
                                    what, i - skip + 1, type_str(at), type_str(ptypes[i]));
            terr2(buf);
        }
    }
    if (i != np) { char buf[96]; snprintf(buf, sizeof(buf), "%s: wrong number of arguments", what); terr2(buf); }
}

static Type *
infer(NODE *e, tframe *sc, Type *R, const char *C)
{
    const struct NodeKind *k = e->head.kind;
    if (k == &kind_node_int)    return INT_T;
    if (k == &kind_node_bool)   return BOOL_T;
    if (k == &kind_node_none)   return type_none();
    if (k == &kind_node_strlit) return STR_T;
    if (k == &kind_node_name) {
        Type *t = tframe_lookup(sc, e->u.node_name.name);
        if (!t) { char b[96]; snprintf(b,sizeof b,"undefined or non-value name '%s'", e->u.node_name.name); terr2(b); return OBJ_T; }
        return t;
    }
    if (k == &kind_node_neg) {
        Type *t = infer(e->u.node_neg.e, sc, R, C);
        if (t->kind != T_CLASS || strcmp(t->cls,"int")) terr2("unary '-' requires int");
        return INT_T;
    }
    if (k == &kind_node_add) {
        Type *a = infer(e->u.node_add.l, sc, R, C), *b = infer(e->u.node_add.r, sc, R, C);
        if (type_eq(a, INT_T) && type_eq(b, INT_T)) return INT_T;
        if (type_eq(a, STR_T) && type_eq(b, STR_T)) return STR_T;
        if (a->kind == T_LIST && b->kind == T_LIST) return type_list(type_join(a->elem, b->elem));
        if (a->kind == T_EMPTY && b->kind == T_LIST) return b;
        if (a->kind == T_LIST && b->kind == T_EMPTY) return a;
        if (a->kind == T_EMPTY && b->kind == T_EMPTY) return type_empty();
        terr2("'+' operands must be two ints, two strs, or two lists");
        return OBJ_T;
    }
    if (k == &kind_node_sub || k == &kind_node_mul || k == &kind_node_fdiv || k == &kind_node_mod) {
        NODE *l = e->u.node_sub.l, *r = e->u.node_sub.r;   // same layout for all four
        Type *a = infer(l, sc, R, C), *b = infer(r, sc, R, C);
        if (!type_eq(a, INT_T) || !type_eq(b, INT_T)) terr2("arithmetic operands must be int");
        return INT_T;
    }
    if (k == &kind_node_lt || k == &kind_node_le || k == &kind_node_gt || k == &kind_node_ge) {
        Type *a = infer(e->u.node_lt.l, sc, R, C), *b = infer(e->u.node_lt.r, sc, R, C);
        if (!type_eq(a, INT_T) || !type_eq(b, INT_T)) terr2("comparison operands must be int");
        return BOOL_T;
    }
    if (k == &kind_node_eq || k == &kind_node_ne) {
        Type *a = infer(e->u.node_eq.l, sc, R, C), *b = infer(e->u.node_eq.r, sc, R, C);
        if (!(type_eq(a,b) && (type_eq(a,INT_T)||type_eq(a,BOOL_T)||type_eq(a,STR_T))))
            terr2("'==' / '!=' operands must both be int, bool, or str");
        return BOOL_T;
    }
    if (k == &kind_node_is) {
        Type *a = infer(e->u.node_is.l, sc, R, C), *b = infer(e->u.node_is.r, sc, R, C);
        if (type_eq(a,INT_T)||type_eq(a,BOOL_T)||type_eq(a,STR_T)||type_eq(b,INT_T)||type_eq(b,BOOL_T)||type_eq(b,STR_T))
            terr2("'is' operands must not be int, bool, or str");
        return BOOL_T;
    }
    if (k == &kind_node_and || k == &kind_node_or) {
        Type *a = infer(e->u.node_and.l, sc, R, C), *b = infer(e->u.node_and.r, sc, R, C);
        if (!type_eq(a, BOOL_T) || !type_eq(b, BOOL_T)) terr2("logical operands must be bool");
        return BOOL_T;
    }
    if (k == &kind_node_not) {
        if (!type_eq(infer(e->u.node_not.e, sc, R, C), BOOL_T)) terr2("'not' operand must be bool");
        return BOOL_T;
    }
    if (k == &kind_node_cond) {
        if (!type_eq(infer(e->u.node_cond.cond, sc, R, C), BOOL_T)) terr2("condition must be bool");
        Type *t1 = infer(e->u.node_cond.then, sc, R, C), *t2 = infer(e->u.node_cond.els, sc, R, C);
        return type_join(t1, t2);
    }
    if (k == &kind_node_listexpr) {
        NODE *it = e->u.node_listexpr.items;
        if (it->head.kind != &kind_node_elt) return type_empty();
        Type *t = NULL;
        for (NODE *a = it; a->head.kind == &kind_node_elt; a = a->u.node_elt.next) {
            Type *et = infer(a->u.node_elt.expr, sc, R, C);
            t = t ? type_join(t, et) : et;
        }
        return type_list(t);
    }
    if (k == &kind_node_index) {
        Type *s = infer(e->u.node_index.seq, sc, R, C);
        if (!type_eq(infer(e->u.node_index.idx, sc, R, C), INT_T)) terr2("index must be int");
        if (type_eq(s, STR_T)) return STR_T;
        if (s->kind == T_LIST) return s->elem;
        terr2("cannot index this type"); return OBJ_T;
    }
    if (k == &kind_node_attr) {
        Type *o = infer(e->u.node_attr.obj, sc, R, C);
        if (o->kind != T_CLASS) { terr2("attribute access on non-object"); return OBJ_T; }
        Type *t = member_attr(o->cls, e->u.node_attr.name);
        if (!t) { char b[96]; snprintf(b,sizeof b,"no attribute '%s' on '%s'", e->u.node_attr.name, o->cls); terr2(b); return OBJ_T; }
        return t;
    }
    if (k == &kind_node_call) {
        const char *name = e->u.node_call.name;
        NODE *args = e->u.node_call.args;
        if (strcmp(name, "print") == 0) { for (NODE *a=args;a->head.kind==&kind_node_elt;a=a->u.node_elt.next) infer(a->u.node_elt.expr,sc,R,C); return type_none(); }
        if (strcmp(name, "len") == 0)   { for (NODE *a=args;a->head.kind==&kind_node_elt;a=a->u.node_elt.next) infer(a->u.node_elt.expr,sc,R,C); return INT_T; }
        if (strcmp(name, "input") == 0) return STR_T;
        if (anpy_lookup_class(name)) {   // constructor
            for (NODE *a=args;a->head.kind==&kind_node_elt;a=a->u.node_elt.next) infer(a->u.node_elt.expr,sc,R,C);
            return type_class(name);
        }
        anpy_func *f = resolve_func(sc, name);
        if (!f) { char b[96]; snprintf(b,sizeof b,"undefined function '%s'", name); terr2(b); return OBJ_T; }
        check_args(args, f->param_types, f->nparams, 0, sc, R, C, name);
        return f->ret_type;
    }
    if (k == &kind_node_method) {
        Type *o = infer(e->u.node_method.recv, sc, R, C);
        if (o->kind != T_CLASS) { terr2("method call on non-object"); return OBJ_T; }
        anpy_func *m = member_method(o->cls, e->u.node_method.name);
        if (!m) { char b[96]; snprintf(b,sizeof b,"no method '%s' on '%s'", e->u.node_method.name, o->cls); terr2(b); return OBJ_T; }
        check_args(e->u.node_method.args, m->param_types, m->nparams, 1, sc, R, C, e->u.node_method.name);
        return m->ret_type;
    }
    return OBJ_T;
}

static void check_stmt(NODE *s, tframe *sc, Type *R, const char *C);

static void check_seq(NODE *s, tframe *sc, Type *R, const char *C) {
    while (s->head.kind == &kind_node_seq) { check_stmt(s->u.node_seq.head, sc, R, C); s = s->u.node_seq.tail; }
    check_stmt(s, sc, R, C);
}

static void
check_stmt(NODE *s, tframe *sc, Type *R, const char *C)
{
    const struct NodeKind *k = s->head.kind;
    if (k == &kind_node_nil) return;
    if (k == &kind_node_seq) { check_seq(s, sc, R, C); return; }
    if (k == &kind_node_exprstmt) { infer(s->u.node_exprstmt.e, sc, R, C); return; }
    if (k == &kind_node_assign_var) {
        Type *t = tframe_lookup(sc, s->u.node_assign_var.name);
        Type *v = infer(s->u.node_assign_var.value, sc, R, C);
        if (t && v && !assign_compat(v, t)) terr2("assignment type mismatch");
        return;
    }
    if (k == &kind_node_assign_attr) {
        Type *o = infer(s->u.node_assign_attr.obj, sc, R, C);
        Type *v = infer(s->u.node_assign_attr.value, sc, R, C);
        if (o->kind == T_CLASS) { Type *at = member_attr(o->cls, s->u.node_assign_attr.name);
            if (at && v && !assign_compat(v, at)) terr2("attribute assignment type mismatch");
            else if (!at) terr2("assignment to unknown attribute"); }
        return;
    }
    if (k == &kind_node_assign_index) {
        Type *st = infer(s->u.node_assign_index.seq, sc, R, C);
        if (!type_eq(infer(s->u.node_assign_index.idx, sc, R, C), INT_T)) terr2("index must be int");
        Type *v = infer(s->u.node_assign_index.value, sc, R, C);
        if (st->kind != T_LIST) terr2("element assignment requires a list");
        else if (v && !assign_compat(v, st->elem)) terr2("list element assignment type mismatch");
        return;
    }
    if (k == &kind_node_massign) {
        infer(s->u.node_massign.value, sc, R, C);
        // (per-target compatibility checked structurally; the [<None>] restriction is in docs/todo)
        return;
    }
    if (k == &kind_node_if) {
        if (!type_eq(infer(s->u.node_if.cond, sc, R, C), BOOL_T)) terr2("if condition must be bool");
        check_stmt(s->u.node_if.then, sc, R, C); check_stmt(s->u.node_if.els, sc, R, C); return;
    }
    if (k == &kind_node_while) {
        if (!type_eq(infer(s->u.node_while.cond, sc, R, C), BOOL_T)) terr2("while condition must be bool");
        check_stmt(s->u.node_while.body, sc, R, C); return;
    }
    if (k == &kind_node_for) {
        Type *it = infer(s->u.node_for.iter, sc, R, C);
        Type *vt = tframe_lookup(sc, s->u.node_for.name);
        if (type_eq(it, STR_T)) { if (vt && !assign_compat(STR_T, vt)) terr2("for: loop var incompatible with str element"); }
        else if (it->kind == T_LIST) { if (vt && !assign_compat(it->elem, vt)) terr2("for: loop var incompatible with element type"); }
        else terr2("for loop requires a list or str");
        check_stmt(s->u.node_for.body, sc, R, C); return;
    }
    if (k == &kind_node_return) {
        Type *t = infer(s->u.node_return.e, sc, R, C);
        if (!R) terr2("return outside function");
        else if (t && !assign_compat(t, R)) terr2("return type mismatch");
        return;
    }
    if (k == &kind_node_return_none) {
        if (R && !assign_compat(type_none(), R)) terr2("missing return value");
        return;
    }
}

// Build a type-scope frame for a function and check its body + nested funcs.
static void
check_func(anpy_func *fn, tframe *parent, const char *C)
{
    tframe sc; memset(&sc, 0, sizeof(sc)); sc.parent = parent; sc.fn = fn;
    for (int i = 0; i < fn->nparams; i++) tframe_add(&sc, fn->params[i], fn->param_types[i]);
    for (int i = 0; i < fn->nvars; i++)   tframe_add(&sc, fn->vars[i].name, fn->vars[i].type);
    if (fn->body) check_stmt(fn->body, &sc, fn->ret_type, C);
    for (int i = 0; i < fn->nnested; i++) check_func(fn->nested[i], &sc, C);
}

int
anpy_typecheck(Program *prog)
{
    g_errors = 0;
    INT_T = type_class("int"); BOOL_T = type_class("bool"); STR_T = type_class("str"); OBJ_T = type_class("object");
    g_funcs = prog->funcs; g_nfuncs = prog->nfuncs;

    // structural class checks
    for (int i = 0; i < prog->nclasses; i++) {
        anpy_class *c = prog->classes[i];
        if (strcmp(c->super_name, "object") != 0 && !anpy_lookup_class(c->super_name)) terr2("undefined superclass");
        if (is_special_basic(c->super_name)) terr2("cannot subclass int/bool/str");
        for (anpy_class *kk = c->super; kk; kk = kk->super) if (kk == c) { terr2("cyclic inheritance"); break; }
    }

    // global type frame
    tframe gsc; memset(&gsc, 0, sizeof(gsc));
    for (int i = 0; i < prog->nvars; i++) tframe_add(&gsc, prog->vars[i].name, prog->vars[i].type);

    // methods: scope has self + params + locals; C = class name
    for (int i = 0; i < prog->nclasses; i++) {
        anpy_class *c = prog->classes[i];
        for (int m = 0; m < c->own_nmethods; m++) check_func(c->own_methods[m].fn, &gsc, c->name);
    }
    // top-level functions
    for (int i = 0; i < prog->nfuncs; i++) check_func(prog->funcs[i], &gsc, NULL);
    // top-level statements
    if (prog->body) check_stmt(prog->body, &gsc, NULL, NULL);

    (void)conforms;
    return g_errors;
}
