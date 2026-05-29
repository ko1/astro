// Static type checker for ancaml — MinCaml's monomorphic Hindley–Milner
// inference (Typing module).  Destructive unification over TY_VAR cells, no
// generalization (let is monomorphic).  An unresolved variable left after
// inference would default to int in a MinCaml compiler; since the
// interpreter never consults types, we only default them for display.
//
// The first type error aborts inference (via longjmp) and is reported with
// its source line — matching MinCaml, which stops at the first mismatch.
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "node.h"
#include "parse.h"
#include "type.h"

// ---- constructors ----------------------------------------------------

static Type *mk(int kind) { Type *t = calloc(1, sizeof(Type)); t->kind = kind; return t; }

Type *ty_unit(void)  { return mk(TY_UNIT); }
Type *ty_bool(void)  { return mk(TY_BOOL); }
Type *ty_int(void)   { return mk(TY_INT); }
Type *ty_float(void) { return mk(TY_FLOAT); }

static int g_var_id = 0;
Type *ty_var(void) { Type *t = mk(TY_VAR); t->content = NULL; t->id = ++g_var_id; return t; }

Type *ty_fun(Type **args, int nargs, Type *ret) {
    Type *t = mk(TY_FUN); t->args = args; t->nargs = nargs; t->ret = ret; return t;
}
Type *ty_tuple(Type **elems, int nelems) {
    Type *t = mk(TY_TUPLE); t->elems = elems; t->nelems = nelems; return t;
}
Type *ty_array(Type *elem) { Type *t = mk(TY_ARRAY); t->elem = elem; return t; }

static Type *deref(Type *t) {
    while (t->kind == TY_VAR && t->content) t = t->content;
    return t;
}

// Recursive writer into a bounded buffer (so nested types don't clobber a
// shared static buffer).  Returns chars written (clamped).
static int
ty_write(char *out, size_t cap, Type *t)
{
    if (cap == 0) return 0;
    t = deref(t);
    switch (t->kind) {
      case TY_UNIT:  return snprintf(out, cap, "unit");
      case TY_BOOL:  return snprintf(out, cap, "bool");
      case TY_INT:   return snprintf(out, cap, "int");
      case TY_FLOAT: return snprintf(out, cap, "float");
      case TY_VAR:   return snprintf(out, cap, "int");   // default unresolved vars to int (MinCaml)
      case TY_ARRAY: {
        int w = ty_write(out, cap, t->elem);
        if (w < (int)cap) w += snprintf(out + w, cap - w, " array");
        return w;
      }
      case TY_TUPLE: {
        int w = snprintf(out, cap, "(");
        for (int i = 0; i < t->nelems && w < (int)cap; i++) {
            if (i) w += snprintf(out + w, cap - w, " * ");
            if (w < (int)cap) w += ty_write(out + w, cap - w, t->elems[i]);
        }
        if (w < (int)cap) w += snprintf(out + w, cap - w, ")");
        return w;
      }
      case TY_FUN: {
        int w = snprintf(out, cap, "(");
        for (int i = 0; i < t->nargs && w < (int)cap; i++) {
            if (i) w += snprintf(out + w, cap - w, " -> ");
            if (w < (int)cap) w += ty_write(out + w, cap - w, t->args[i]);
        }
        if (w < (int)cap) w += snprintf(out + w, cap - w, " -> ");
        if (w < (int)cap) w += ty_write(out + w, cap - w, t->ret);
        if (w < (int)cap) w += snprintf(out + w, cap - w, ")");
        return w;
      }
    }
    return snprintf(out, cap, "?");
}

const char *
ty_str(Type *t)
{
    static char buf[1024];
    ty_write(buf, sizeof(buf), t);
    return buf;
}

// ---- external (library) signatures -----------------------------------

static Type *fun1(Type *a, Type *r) { Type **args = malloc(sizeof(Type *)); args[0] = a; return ty_fun(args, 1, r); }

Type *
ac_external_type(const char *name)
{
    if (!strcmp(name, "print_int"))    return fun1(ty_int(),   ty_unit());
    if (!strcmp(name, "print_char"))   return fun1(ty_int(),   ty_unit());
    if (!strcmp(name, "print_newline"))return fun1(ty_unit(),  ty_unit());
    if (!strcmp(name, "print_float"))  return fun1(ty_float(), ty_unit());
    if (!strcmp(name, "read_int"))     return fun1(ty_unit(),  ty_int());
    if (!strcmp(name, "read_float"))   return fun1(ty_unit(),  ty_float());
    if (!strcmp(name, "float_of_int")) return fun1(ty_int(),   ty_float());
    if (!strcmp(name, "int_of_float")) return fun1(ty_float(), ty_int());
    if (!strcmp(name, "truncate"))     return fun1(ty_float(), ty_int());
    if (!strcmp(name, "abs_float"))    return fun1(ty_float(), ty_float());
    if (!strcmp(name, "sqrt"))         return fun1(ty_float(), ty_float());
    if (!strcmp(name, "sin"))          return fun1(ty_float(), ty_float());
    if (!strcmp(name, "cos"))          return fun1(ty_float(), ty_float());
    if (!strcmp(name, "atan"))         return fun1(ty_float(), ty_float());
    if (!strcmp(name, "floor"))        return fun1(ty_float(), ty_float());
    return NULL;
}

// ---- unification -----------------------------------------------------

static jmp_buf g_type_jmp;
static char    g_type_err[256];
static int     g_err_line;

__attribute__((noreturn))
static void type_fail(const char *what, Type *a, Type *b) {
    char sa[110], sb[110];
    strncpy(sa, ty_str(a), sizeof(sa) - 1); sa[sizeof(sa)-1] = 0;
    strncpy(sb, ty_str(b), sizeof(sb) - 1); sb[sizeof(sb)-1] = 0;
    snprintf(g_type_err, sizeof(g_type_err), "%s: %s vs %s", what, sa, sb);
    longjmp(g_type_jmp, 1);
}

static bool occurs(Type *var, Type *t) {
    t = deref(t);
    if (t == var) return true;
    if (t->kind == TY_FUN) {
        for (int i = 0; i < t->nargs; i++) if (occurs(var, t->args[i])) return true;
        return occurs(var, t->ret);
    }
    if (t->kind == TY_TUPLE) {
        for (int i = 0; i < t->nelems; i++) if (occurs(var, t->elems[i])) return true;
        return false;
    }
    if (t->kind == TY_ARRAY) return occurs(var, t->elem);
    return false;
}

static void
unify(Type *a, Type *b)
{
    a = deref(a); b = deref(b);
    if (a == b) return;
    if (a->kind == TY_VAR) { if (occurs(a, b)) type_fail("recursive type", a, b); a->content = b; return; }
    if (b->kind == TY_VAR) { if (occurs(b, a)) type_fail("recursive type", b, a); b->content = a; return; }
    if (a->kind != b->kind) type_fail("type mismatch", a, b);
    switch (a->kind) {
      case TY_FUN:
        if (a->nargs != b->nargs) type_fail("arity mismatch", a, b);
        for (int i = 0; i < a->nargs; i++) unify(a->args[i], b->args[i]);
        unify(a->ret, b->ret);
        return;
      case TY_TUPLE:
        if (a->nelems != b->nelems) type_fail("tuple width mismatch", a, b);
        for (int i = 0; i < a->nelems; i++) unify(a->elems[i], b->elems[i]);
        return;
      case TY_ARRAY:
        unify(a->elem, b->elem);
        return;
      default:
        return;   // both same base kind
    }
}

// ---- inference -------------------------------------------------------

typedef struct TFrame { Type **types; int n; struct TFrame *parent; } TFrame;

extern const struct NodeKind
  kind_node_unit, kind_node_bool, kind_node_int, kind_node_int64, kind_node_float,
  kind_node_lref, kind_node_gref, kind_node_if, kind_node_seq,
  kind_node_let, kind_node_letrec, kind_node_lettuple, kind_node_fun,
  kind_node_app1, kind_node_app2, kind_node_app3, kind_node_app4, kind_node_appn,
  kind_node_add, kind_node_sub, kind_node_neg,
  kind_node_fadd, kind_node_fsub, kind_node_fmul, kind_node_fdiv, kind_node_fneg,
  kind_node_eq, kind_node_le, kind_node_not,
  kind_node_tuple, kind_node_array, kind_node_get, kind_node_put;

static Type *infer(NODE *e, TFrame *env);

// Build the (expected) function type for an application and return its
// result, unifying with the callee.
static Type *
infer_app(NODE *fn, NODE **args, int argc, TFrame *env)
{
    Type *ft = infer(fn, env);
    Type **atys = malloc(sizeof(Type *) * (argc ? argc : 1));
    for (int i = 0; i < argc; i++) atys[i] = infer(args[i], env);
    Type *ret = ty_var();
    unify(ft, ty_fun(atys, argc, ret));
    return ret;
}

static Type *
infer(NODE *e, TFrame *env)
{
    const struct NodeKind *k = e->head.kind;
    g_err_line = e->head.line;

    if (k == &kind_node_unit)  return ty_unit();
    if (k == &kind_node_bool)  return ty_bool();
    if (k == &kind_node_int)   return ty_int();
    if (k == &kind_node_int64) return ty_int();
    if (k == &kind_node_float) return ty_float();

    if (k == &kind_node_lref) {
        uint32_t depth = e->u.node_lref.depth, idx = e->u.node_lref.idx;
        TFrame *f = env;
        for (uint32_t i = 0; i < depth; i++) f = f->parent;
        return f->types[idx];
    }
    if (k == &kind_node_gref) {
        Type *t = ac_external_type(e->u.node_gref.name);
        return t ? t : ty_var();
    }

    if (k == &kind_node_if) {
        unify(infer(e->u.node_if.cond, env), ty_bool());
        Type *t = infer(e->u.node_if.thn, env);
        Type *f = infer(e->u.node_if.els, env);
        unify(t, f);
        return t;
    }
    if (k == &kind_node_seq) {
        unify(infer(e->u.node_seq.first, env), ty_unit());
        return infer(e->u.node_seq.rest, env);
    }
    if (k == &kind_node_let) {
        Type *vt = infer(e->u.node_let.value, env);
        Type *slot[1] = { vt };
        TFrame f = { slot, 1, env };
        return infer(e->u.node_let.body, &f);
    }
    if (k == &kind_node_letrec) {
        Type *ft = ty_var();
        Type *slot[1] = { ft };
        TFrame f = { slot, 1, env };
        Type *vt = infer(e->u.node_letrec.value, &f);   // node_fun
        unify(ft, vt);
        return infer(e->u.node_letrec.body, &f);
    }
    if (k == &kind_node_fun) {
        int np = (int)e->u.node_fun.nparams;
        Type **ptys = malloc(sizeof(Type *) * (np ? np : 1));
        for (int i = 0; i < np; i++) ptys[i] = ty_var();
        TFrame f = { ptys, np, env };
        Type *rt = infer(e->u.node_fun.body, &f);
        return ty_fun(ptys, np, rt);
    }
    if (k == &kind_node_lettuple) {
        int n = (int)e->u.node_lettuple.cnt;
        Type *vt = infer(e->u.node_lettuple.value, env);
        Type **etys = malloc(sizeof(Type *) * (n ? n : 1));
        for (int i = 0; i < n; i++) etys[i] = ty_var();
        unify(vt, ty_tuple(etys, n));
        TFrame f = { etys, n, env };
        return infer(e->u.node_lettuple.body, &f);
    }

    if (k == &kind_node_app1) { NODE *a[1] = { e->u.node_app1.a0 }; return infer_app(e->u.node_app1.fn, a, 1, env); }
    if (k == &kind_node_app2) { NODE *a[2] = { e->u.node_app2.a0, e->u.node_app2.a1 }; return infer_app(e->u.node_app2.fn, a, 2, env); }
    if (k == &kind_node_app3) { NODE *a[3] = { e->u.node_app3.a0, e->u.node_app3.a1, e->u.node_app3.a2 }; return infer_app(e->u.node_app3.fn, a, 3, env); }
    if (k == &kind_node_app4) { NODE *a[4] = { e->u.node_app4.a0, e->u.node_app4.a1, e->u.node_app4.a2, e->u.node_app4.a3 }; return infer_app(e->u.node_app4.fn, a, 4, env); }
    if (k == &kind_node_appn) {
        uint32_t idx = e->u.node_appn.args_idx, argc = e->u.node_appn.argc;
        return infer_app(e->u.node_appn.fn, &AC_CALL_ARGS[idx], (int)argc, env);
    }

    if (k == &kind_node_add || k == &kind_node_sub) {
        unify(infer(e->u.node_add.a, env), ty_int());
        unify(infer(e->u.node_add.b, env), ty_int());
        return ty_int();
    }
    if (k == &kind_node_neg) { unify(infer(e->u.node_neg.e, env), ty_int()); return ty_int(); }

    if (k == &kind_node_fadd || k == &kind_node_fsub || k == &kind_node_fmul || k == &kind_node_fdiv) {
        unify(infer(e->u.node_fadd.a, env), ty_float());
        unify(infer(e->u.node_fadd.b, env), ty_float());
        return ty_float();
    }
    if (k == &kind_node_fneg) { unify(infer(e->u.node_fneg.e, env), ty_float()); return ty_float(); }

    if (k == &kind_node_eq) { unify(infer(e->u.node_eq.a, env), infer(e->u.node_eq.b, env)); return ty_bool(); }
    if (k == &kind_node_le) { unify(infer(e->u.node_le.a, env), infer(e->u.node_le.b, env)); return ty_bool(); }
    if (k == &kind_node_not) { unify(infer(e->u.node_not.e, env), ty_bool()); return ty_bool(); }

    if (k == &kind_node_tuple) {
        int n = (int)e->u.node_tuple.cnt;
        uint32_t idx = e->u.node_tuple.items_idx;
        Type **etys = malloc(sizeof(Type *) * (n ? n : 1));
        for (int i = 0; i < n; i++) etys[i] = infer(AC_TUPLE_ITEMS[idx + i], env);
        return ty_tuple(etys, n);
    }
    if (k == &kind_node_array) {
        unify(infer(e->u.node_array.size, env), ty_int());
        return ty_array(infer(e->u.node_array.init, env));
    }
    if (k == &kind_node_get) {
        unify(infer(e->u.node_get.idx, env), ty_int());
        Type *ev = ty_var();
        unify(infer(e->u.node_get.arr, env), ty_array(ev));
        return ev;
    }
    if (k == &kind_node_put) {
        unify(infer(e->u.node_put.idx, env), ty_int());
        Type *vt = infer(e->u.node_put.val, env);
        unify(infer(e->u.node_put.arr, env), ty_array(vt));
        return ty_unit();
    }

    // Should be unreachable.
    return ty_var();
}

int
ac_typecheck(Program *prog)
{
    if (setjmp(g_type_jmp) != 0) {
        fprintf(stderr, "ancaml: type error (line %d): %s\n", g_err_line, g_type_err);
        return 1;
    }
    Type *t = infer(prog->body, NULL);
    if (OPTION.dump_types) printf("- : %s\n", ty_str(t));
    return 0;
}
