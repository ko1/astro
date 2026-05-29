// Runtime value model for ancaml (MinCaml): heap allocation, environment
// frames, function application, the external (library) functions, and
// polymorphic equality / ordering.  Heap objects are GC-managed (libgc).
#include <gc.h>
#include <math.h>
#include <stdarg.h>
#include "node.h"

// ---- heap allocation -------------------------------------------------

struct ac_obj *
ac_alloc(int type)
{
    struct ac_obj *o = (struct ac_obj *)GC_MALLOC(sizeof(struct ac_obj));
    o->type = type;
    return o;
}

VALUE
ac_make_float(double d)
{
    struct ac_obj *o = ac_alloc(AC_FLOAT);
    o->u.dbl = d;
    return AC_OBJ(o);
}

VALUE
ac_make_closure(NODE *body, struct ac_frame *env, int nparams, int is_leaf)
{
    struct ac_obj *o = ac_alloc(AC_CLOSURE);
    o->u.closure.body = body;
    o->u.closure.env = env;
    o->u.closure.nparams = nparams;
    o->u.closure.is_leaf = is_leaf;
    return AC_OBJ(o);
}

VALUE
ac_make_tuple(int n, const VALUE *items)
{
    struct ac_obj *o = ac_alloc(AC_TUPLE);
    o->u.tup.n = n;
    o->u.tup.items = (VALUE *)GC_MALLOC(sizeof(VALUE) * (n ? n : 1));
    for (int i = 0; i < n; i++) o->u.tup.items[i] = items[i];
    return AC_OBJ(o);
}

VALUE
ac_make_array(int n, VALUE init)
{
    struct ac_obj *o = ac_alloc(AC_ARRAY);
    o->u.arr.n = n;
    o->u.arr.items = n ? (VALUE *)GC_MALLOC(sizeof(VALUE) * n) : NULL;
    for (int i = 0; i < n; i++) o->u.arr.items[i] = init;
    return AC_OBJ(o);
}

VALUE
ac_make_prim(const char *name, ac_prim_fn fn, int arity)
{
    struct ac_obj *o = ac_alloc(AC_PRIM);
    o->u.prim.name = name;
    o->u.prim.fn = fn;
    o->u.prim.arity = arity;
    return AC_OBJ(o);
}

struct ac_frame *
ac_new_frame(struct ac_frame *parent, int nslots)
{
    struct ac_frame *f = (struct ac_frame *)GC_MALLOC(sizeof(struct ac_frame) + sizeof(VALUE) * nslots);
    f->parent = parent;
    f->nslots = nslots;
    return f;
}

// ---- coercion / application -----------------------------------------

double
ac_get_float(CTX *c, VALUE v)
{
    if (LIKELY(AC_IS_PTR(v) && AC_PTR(v)->type == AC_FLOAT)) return AC_PTR(v)->u.dbl;
    ac_runtime_error(c, "expected a float");
}

VALUE
ac_apply(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    // Trampoline: a tail call inside the body sets c->tail_pending and stores
    // the next (fn, argv) in the CTX; we loop instead of recursing, so a tail
    // loop runs in O(1) C stack.  Frames here are heap (GC) frames — never
    // alloca in this loop, or a long tail loop would grow the stack.
    for (;;) {
        if (UNLIKELY(!AC_IS_PTR(fn))) ac_runtime_error(c, "applied a non-function value");
        struct ac_obj *o = AC_PTR(fn);
        if (o->type == AC_PRIM) {
            if (UNLIKELY(o->u.prim.arity != argc))
                ac_runtime_error(c, "%s applied to %d argument(s), expected %d", o->u.prim.name, argc, o->u.prim.arity);
            return o->u.prim.fn(c, argc, argv);
        }
        if (UNLIKELY(o->type != AC_CLOSURE))
            ac_runtime_error(c, "applied a non-function value");
        if (UNLIKELY(o->u.closure.nparams != argc))
            ac_runtime_error(c, "function applied to %d argument(s), expected %d", argc, o->u.closure.nparams);

        struct ac_frame *f = ac_new_frame(o->u.closure.env, argc);
        for (int i = 0; i < argc; i++) f->slots[i] = argv[i];
        struct ac_frame *saved = c->env;
        c->env = f;
        NODE *body = o->u.closure.body;
        VALUE r = (*body->head.dispatcher)(c, body);
        c->env = saved;

        if (LIKELY(!c->tail_pending)) return r;
        c->tail_pending = 0;
        fn = c->tc_fn; argc = c->tc_argc; argv = c->tc_argv;   // re-enter with the tail target
    }
}

// ---- polymorphic equality / ordering --------------------------------

bool
ac_structural_eq(CTX *c, VALUE a, VALUE b)
{
    if (a == b) return true;
    if (AC_IS_INT(a) || AC_IS_INT(b)) return false;
    if (AC_IS_PTR(a) && AC_IS_PTR(b)) {
        struct ac_obj *oa = AC_PTR(a), *ob = AC_PTR(b);
        if (oa->type != ob->type) return false;
        switch (oa->type) {
          case AC_FLOAT: return oa->u.dbl == ob->u.dbl;
          case AC_TUPLE:
            if (oa->u.tup.n != ob->u.tup.n) return false;
            for (int i = 0; i < oa->u.tup.n; i++)
                if (!ac_structural_eq(c, oa->u.tup.items[i], ob->u.tup.items[i])) return false;
            return true;
          case AC_ARRAY:
            if (oa->u.arr.n != ob->u.arr.n) return false;
            for (int i = 0; i < oa->u.arr.n; i++)
                if (!ac_structural_eq(c, oa->u.arr.items[i], ob->u.arr.items[i])) return false;
            return true;
          default:
            ac_runtime_error(c, "equality on a functional value");
        }
    }
    return false;
}

int
ac_compare(CTX *c, VALUE a, VALUE b)
{
    if (AC_IS_INT(a) && AC_IS_INT(b)) {
        int64_t x = AC_INT_VAL(a), y = AC_INT_VAL(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (AC_IS_PTR(a) && AC_IS_PTR(b)) {
        struct ac_obj *oa = AC_PTR(a), *ob = AC_PTR(b);
        if (oa->type == AC_FLOAT && ob->type == AC_FLOAT) {
            double x = oa->u.dbl, y = ob->u.dbl;
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        if (oa->type == AC_TUPLE && ob->type == AC_TUPLE) {
            int n = oa->u.tup.n < ob->u.tup.n ? oa->u.tup.n : ob->u.tup.n;
            for (int i = 0; i < n; i++) {
                int r = ac_compare(c, oa->u.tup.items[i], ob->u.tup.items[i]);
                if (r) return r;
            }
            return oa->u.tup.n - ob->u.tup.n;
        }
        if (oa->type == AC_ARRAY && ob->type == AC_ARRAY) {
            int n = oa->u.arr.n < ob->u.arr.n ? oa->u.arr.n : ob->u.arr.n;
            for (int i = 0; i < n; i++) {
                int r = ac_compare(c, oa->u.arr.items[i], ob->u.arr.items[i]);
                if (r) return r;
            }
            return oa->u.arr.n - ob->u.arr.n;
        }
        ac_runtime_error(c, "comparison on a functional value");
    }
    // unit / bool constants (and any leftover): compare the raw word
    return a < b ? -1 : (a > b ? 1 : 0);
}

// ---- external (library) functions -----------------------------------

static VALUE prim_print_int(CTX *c, int argc, VALUE *a)     { (void)c;(void)argc; printf("%lld", (long long)AC_INT_VAL(a[0])); return AC_UNIT; }
static VALUE prim_print_char(CTX *c, int argc, VALUE *a)    { (void)c;(void)argc; putchar((int)AC_INT_VAL(a[0])); return AC_UNIT; }
static VALUE prim_print_newline(CTX *c, int argc, VALUE *a) { (void)c;(void)argc;(void)a; putchar('\n'); return AC_UNIT; }

static VALUE
prim_print_float(CTX *c, int argc, VALUE *a)
{
    (void)argc;
    double d = ac_get_float(c, a[0]);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%.12g", d);
    if (!strpbrk(tmp, ".eEnN")) strncat(tmp, ".", sizeof(tmp) - strlen(tmp) - 1);  // OCaml string_of_float
    fputs(tmp, stdout);
    return AC_UNIT;
}

static VALUE prim_read_int(CTX *c, int argc, VALUE *a)   { (void)c;(void)argc;(void)a; long long v = 0; if (scanf("%lld", &v) != 1) v = 0; return AC_INT(v); }
static VALUE prim_read_float(CTX *c, int argc, VALUE *a) { (void)c;(void)argc;(void)a; double v = 0; if (scanf("%lf", &v) != 1) v = 0; return ac_make_float(v); }

static VALUE prim_float_of_int(CTX *c, int argc, VALUE *a) { (void)c;(void)argc; return ac_make_float((double)AC_INT_VAL(a[0])); }
static VALUE prim_int_of_float(CTX *c, int argc, VALUE *a) { (void)argc; return AC_INT((int64_t)ac_get_float(c, a[0])); }
static VALUE prim_abs_float(CTX *c, int argc, VALUE *a)    { (void)argc; return ac_make_float(fabs(ac_get_float(c, a[0]))); }
static VALUE prim_sqrt(CTX *c, int argc, VALUE *a)         { (void)argc; return ac_make_float(sqrt(ac_get_float(c, a[0]))); }
static VALUE prim_sin(CTX *c, int argc, VALUE *a)          { (void)argc; return ac_make_float(sin(ac_get_float(c, a[0]))); }
static VALUE prim_cos(CTX *c, int argc, VALUE *a)          { (void)argc; return ac_make_float(cos(ac_get_float(c, a[0]))); }
static VALUE prim_atan(CTX *c, int argc, VALUE *a)         { (void)argc; return ac_make_float(atan(ac_get_float(c, a[0]))); }
static VALUE prim_floor(CTX *c, int argc, VALUE *a)        { (void)argc; return ac_make_float(floor(ac_get_float(c, a[0]))); }

static struct { const char *name; ac_prim_fn fn; int arity; VALUE val; } g_externals[] = {
    { "print_int",     prim_print_int,     1, 0 },
    { "print_char",    prim_print_char,    1, 0 },
    { "print_newline", prim_print_newline, 1, 0 },
    { "print_float",   prim_print_float,   1, 0 },
    { "read_int",      prim_read_int,      1, 0 },
    { "read_float",    prim_read_float,    1, 0 },
    { "float_of_int",  prim_float_of_int,  1, 0 },
    { "int_of_float",  prim_int_of_float,  1, 0 },
    { "truncate",      prim_int_of_float,  1, 0 },
    { "abs_float",     prim_abs_float,     1, 0 },
    { "sqrt",          prim_sqrt,          1, 0 },
    { "sin",           prim_sin,           1, 0 },
    { "cos",           prim_cos,           1, 0 },
    { "atan",          prim_atan,          1, 0 },
    { "floor",         prim_floor,         1, 0 },
};
static const int g_n_externals = (int)(sizeof(g_externals) / sizeof(g_externals[0]));

void
ac_register_externals(void)
{
    for (int i = 0; i < g_n_externals; i++)
        g_externals[i].val = ac_make_prim(g_externals[i].name, g_externals[i].fn, g_externals[i].arity);
}

VALUE
ac_lookup_external(const char *name)
{
    for (int i = 0; i < g_n_externals; i++)
        if (strcmp(g_externals[i].name, name) == 0) return g_externals[i].val;
    return 0;
}

// ---- display (REPL / --dump debugging) ------------------------------

void
ac_display(FILE *fp, VALUE v)
{
    if (v == AC_UNIT)  { fputs("()", fp); return; }
    if (v == AC_TRUE)  { fputs("true", fp); return; }
    if (v == AC_FALSE) { fputs("false", fp); return; }
    if (AC_IS_INT(v))  { fprintf(fp, "%lld", (long long)AC_INT_VAL(v)); return; }
    struct ac_obj *o = AC_PTR(v);
    switch (o->type) {
      case AC_FLOAT:   fprintf(fp, "%g", o->u.dbl); return;
      case AC_CLOSURE:
      case AC_PRIM:    fputs("<fun>", fp); return;
      case AC_TUPLE:
        fputc('(', fp);
        for (int i = 0; i < o->u.tup.n; i++) { if (i) fputs(", ", fp); ac_display(fp, o->u.tup.items[i]); }
        fputc(')', fp);
        return;
      case AC_ARRAY:
        fputs("[|", fp);
        for (int i = 0; i < o->u.arr.n; i++) { if (i) fputs("; ", fp); ac_display(fp, o->u.arr.items[i]); }
        fputs("|]", fp);
        return;
    }
}

// ---- runtime errors --------------------------------------------------

void
ac_runtime_error(CTX *c, const char *fmt, ...)
{
    fflush(stdout);
    va_list ap; va_start(ap, fmt);
    fputs("ancaml: runtime error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (c && c->err_active) longjmp(c->err_jmp, 1);
    exit(1);
}

CTX *
ac_make_context(void)
{
    CTX *c = (CTX *)GC_MALLOC(sizeof(CTX));
    c->env = NULL;
    c->err_active = 0;
    return c;
}
