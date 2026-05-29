#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <gc.h>
#include "node.h"

// =====================================================================
// AnPy runtime: GC wiring, lexical environments, class/function
// registries, function calls / object construction / dynamic dispatch,
// and the EVAL/OPTIMIZE/INIT glue.
// =====================================================================

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (!n) { fprintf(stderr, "out of memory\n"); exit(1); }
    return n;
}

// --- runtime-error unwinding -----------------------------------------
static jmp_buf anpy_jmp;
static volatile int anpy_jmp_active = 0;
jmp_buf *anpy_get_jmp(void) { return &anpy_jmp; }
void anpy_set_jmp_active(int v) { anpy_jmp_active = v; }
int anpy_jmp_is_active(void) { return anpy_jmp_active; }

#include "astro_node.c"
#include "astro_code_store.c"
#include "astro_build.c"

// --- lexical environments --------------------------------------------

#define ENV_NB 32

anpy_env *
env_new(anpy_env *parent)
{
    anpy_env *e = (anpy_env *)GC_MALLOC(sizeof(anpy_env));
    e->parent = parent;
    e->nbuckets = ENV_NB;
    e->buckets = (struct env_entry **)GC_MALLOC(sizeof(struct env_entry *) * ENV_NB);
    return e;
}

static size_t
str_hash(const char *s)
{
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static anpy_cell *
env_lookup_local(anpy_env *e, const char *name)
{
    for (struct env_entry *p = e->buckets[str_hash(name) % e->nbuckets]; p; p = p->next)
        if (strcmp(p->name, name) == 0) return p->cell;
    return NULL;
}

anpy_cell *
env_lookup(anpy_env *e, const char *name)
{
    for (; e; e = e->parent) {
        anpy_cell *c = env_lookup_local(e, name);
        if (c) return c;
    }
    return NULL;
}

void
env_bind(anpy_env *e, const char *name, anpy_cell *cell)
{
    const size_t b = str_hash(name) % e->nbuckets;
    for (struct env_entry *p = e->buckets[b]; p; p = p->next)
        if (strcmp(p->name, name) == 0) { p->cell = cell; return; }
    struct env_entry *ent = (struct env_entry *)GC_MALLOC(sizeof(*ent));
    ent->name = name; ent->cell = cell; ent->next = e->buckets[b];
    e->buckets[b] = ent;
}

anpy_cell *
env_define(anpy_env *e, const char *name)
{
    anpy_cell *cell = (anpy_cell *)GC_MALLOC(sizeof(anpy_cell));
    cell->v = ANPY_NONE;
    env_bind(e, name, cell);
    return cell;
}

// --- class registry ---------------------------------------------------

#define MAX_CLASSES 4096
static anpy_class *classes[MAX_CLASSES];
static int nclasses = 0;

void
anpy_register_class(anpy_class *cls)
{
    if (nclasses < MAX_CLASSES) classes[nclasses++] = cls;
}

anpy_class *
anpy_lookup_class(const char *name)
{
    for (int i = 0; i < nclasses; i++)
        if (strcmp(classes[i]->name, name) == 0) return classes[i];
    return NULL;
}

// Flatten attributes/methods over the inheritance chain (slots, overrides).
static void
finalize_one(anpy_class *cls)
{
    if (cls->finalized) return;
    cls->finalized = true;
    if (cls->super_name && !cls->super) cls->super = anpy_lookup_class(cls->super_name);
    if (cls->super) finalize_one(cls->super);

    int base_attrs = cls->super ? cls->super->nattrs : 0;
    int base_meths = cls->super ? cls->super->nmethods : 0;

    cls->nattrs = base_attrs + cls->own_nattrs;
    cls->attrs = (struct var_decl *)GC_MALLOC(sizeof(struct var_decl) * (cls->nattrs ? cls->nattrs : 1));
    for (int i = 0; i < base_attrs; i++) cls->attrs[i] = cls->super->attrs[i];
    for (int i = 0; i < cls->own_nattrs; i++) {
        cls->attrs[base_attrs + i] = cls->own_attrs[i];
        cls->attrs[base_attrs + i].slot = base_attrs + i;
    }

    // methods: start from super's, then apply own (override by name or append).
    struct method_ent *tmp = (struct method_ent *)GC_MALLOC(sizeof(struct method_ent) * (base_meths + cls->own_nmethods + 1));
    int n = 0;
    for (int i = 0; i < base_meths; i++) tmp[n++] = cls->super->methods[i];
    for (int i = 0; i < cls->own_nmethods; i++) {
        int found = -1;
        for (int j = 0; j < n; j++) if (strcmp(tmp[j].name, cls->own_methods[i].name) == 0) { found = j; break; }
        if (found >= 0) tmp[found] = cls->own_methods[i];
        else tmp[n++] = cls->own_methods[i];
    }
    cls->nmethods = n;
    cls->methods = tmp;
}

void
anpy_finalize_classes(void)
{
    for (int i = 0; i < nclasses; i++) finalize_one(classes[i]);
}

anpy_func *
anpy_class_method(anpy_class *cls, const char *name)
{
    for (int i = 0; i < cls->nmethods; i++)
        if (strcmp(cls->methods[i].name, name) == 0) return cls->methods[i].fn;
    return NULL;
}

static int
class_attr_slot(anpy_class *cls, const char *name)
{
    for (int i = 0; i < cls->nattrs; i++)
        if (strcmp(cls->attrs[i].name, name) == 0) return cls->attrs[i].slot;
    return -1;
}

// --- closures / classvals ---------------------------------------------

static anpy_closure *
make_closure(anpy_func *fn, anpy_env *env)
{
    anpy_closure *clo = (anpy_closure *)GC_MALLOC(sizeof(anpy_closure));
    clo->hdr.kind = K_FUNC; clo->fn = fn; clo->env = env; clo->bound_self = ANPY_NONE;
    return clo;
}

static anpy_classval *
make_classval(anpy_class *cls)
{
    anpy_classval *cv = (anpy_classval *)GC_MALLOC(sizeof(anpy_classval));
    cv->hdr.kind = K_CLASS; cv->cls = cls;
    return cv;
}

// --- the core function invoker ---------------------------------------

static VALUE anpy_invoke(CTX *c, anpy_func *fn, anpy_env *parent, VALUE *args, int nargs);

VALUE
anpy_call_closure(CTX *c, anpy_closure *clo, VALUE *args, int nargs)
{
    return anpy_invoke(c, clo->fn, clo->env, args, nargs);
}

static VALUE
anpy_invoke(CTX *c, anpy_func *fn, anpy_env *parent, VALUE *args, int nargs)
{
    (void)nargs;
    anpy_env *frame = env_new(parent);

    // params
    for (int i = 0; i < fn->nparams; i++) env_define(frame, fn->params[i])->v = args[i];
    // global declarations: alias the global cell (create if needed)
    for (int i = 0; i < fn->nglobals; i++) {
        anpy_cell *g = env_lookup_local(c->global, fn->globals[i]);
        if (!g) g = env_define(c->global, fn->globals[i]);
        env_bind(frame, fn->globals[i], g);
    }
    // nonlocal declarations: alias the nearest enclosing cell
    for (int i = 0; i < fn->nnonlocals; i++) {
        anpy_cell *cell = env_lookup(parent, fn->nonlocals[i]);
        if (cell) env_bind(frame, fn->nonlocals[i], cell);
    }
    // local variables (literal initialisers)
    anpy_env *saved = c->env;
    c->env = frame;
    for (int i = 0; i < fn->nvars; i++) {
        anpy_cell *cell = env_define(frame, fn->vars[i].name);
        cell->v = fn->vars[i].init ? EVAL(c, fn->vars[i].init) : ANPY_NONE;
    }
    // nested functions (capture this frame)
    for (int i = 0; i < fn->nnested; i++)
        env_define(frame, fn->nested[i]->name)->v = (VALUE)make_closure(fn->nested[i], frame);

    bool saved_ret = c->returning;
    VALUE saved_rv = c->retval;
    c->returning = false;

    if (fn->body) EVAL(c, fn->body);
    VALUE result = c->returning ? c->retval : ANPY_NONE;

    c->returning = saved_ret;
    c->retval = saved_rv;
    c->env = saved;
    return result;
}

// --- object construction ---------------------------------------------

VALUE
anpy_construct(CTX *c, anpy_class *cls)
{
    anpy_inst *o = (anpy_inst *)GC_MALLOC(sizeof(anpy_inst) + sizeof(VALUE) * (size_t)(cls->nattrs ? cls->nattrs : 1));
    o->hdr.kind = K_OBJ;
    o->cls = cls;
    // initialise attributes (literal initialisers evaluated in global scope)
    anpy_env *saved = c->env; c->env = c->global;
    for (int i = 0; i < cls->nattrs; i++)
        o->attrs[cls->attrs[i].slot] = cls->attrs[i].init ? EVAL(c, cls->attrs[i].init) : ANPY_NONE;
    c->env = saved;
    // invoke __init__ via dispatch
    anpy_func *init = anpy_class_method(cls, "__init__");
    if (init) { VALUE a[1] = { (VALUE)o }; anpy_invoke(c, init, c->global, a, 1); }
    return (VALUE)o;
}

// --- attribute access -------------------------------------------------

VALUE
anpy_getattr(CTX *c, VALUE obj, const char *name)
{
    if (!is_inst(obj)) { anpy_runtime_error(c, "Operation on None"); return ANPY_NONE; }
    anpy_inst *o = (anpy_inst *)obj;
    int slot = class_attr_slot(o->cls, name);
    if (slot < 0) { anpy_runtime_error(c, "Operation on None"); return ANPY_NONE; }
    return o->attrs[slot];
}

void
anpy_setattr(CTX *c, VALUE obj, const char *name, VALUE v)
{
    if (!is_inst(obj)) { anpy_runtime_error(c, "Operation on None"); return; }
    anpy_inst *o = (anpy_inst *)obj;
    int slot = class_attr_slot(o->cls, name);
    if (slot < 0) { anpy_runtime_error(c, "Operation on None"); return; }
    o->attrs[slot] = v;
}

// --- argument evaluation ---------------------------------------------

extern const struct NodeKind kind_node_elt;

static int
eval_args(CTX *c, NODE *chain, VALUE *out, int max)
{
    int n = 0;
    for (NODE *a = chain; a->head.kind == &kind_node_elt && n < max; a = a->u.node_elt.next)
        out[n++] = EVAL(c, a->u.node_elt.expr);
    return n;
}

VALUE
anpy_make_list(CTX *c, NODE *chain)
{
    VALUE tmp[1024]; int n = eval_args(c, chain, tmp, 1024);
    anpy_list *l = anpy_list_new(n);
    for (int i = 0; i < n; i++) l->elems[i] = tmp[i];
    return (VALUE)l;
}

// --- calls / dispatch -------------------------------------------------

VALUE
anpy_do_call(CTX *c, const char *name, NODE *args)
{
    // predefined functions
    if (strcmp(name, "print") == 0) { VALUE a[1] = { ANPY_NONE }; eval_args(c, args, a, 1); anpy_print(c, a[0]); return ANPY_NONE; }
    if (strcmp(name, "len")   == 0) { VALUE a[1] = { ANPY_NONE }; eval_args(c, args, a, 1); return anpy_len(c, a[0]); }
    if (strcmp(name, "input") == 0) { (void)args; return anpy_input(c); }

    anpy_cell *cell = env_lookup(c->env, name);
    if (!cell) { anpy_runtime_error(c, "Undefined: %s", name); return ANPY_NONE; }
    VALUE callee = cell->v;
    if (IS_PTR(callee) && obj_kind(callee) == K_CLASS)
        return anpy_construct(c, ((anpy_classval *)callee)->cls);
    if (IS_PTR(callee) && obj_kind(callee) == K_FUNC) {
        VALUE a[256]; int n = eval_args(c, args, a, 256);
        return anpy_call_closure(c, (anpy_closure *)callee, a, n);
    }
    anpy_runtime_error(c, "Not callable: %s", name);
    return ANPY_NONE;
}

VALUE
anpy_do_method(CTX *c, VALUE recv, const char *name, NODE *args)
{
    if (!is_inst(recv)) { anpy_runtime_error(c, "Operation on None"); return ANPY_NONE; }
    anpy_inst *o = (anpy_inst *)recv;
    anpy_func *m = anpy_class_method(o->cls, name);
    if (!m) { anpy_runtime_error(c, "Operation on None"); return ANPY_NONE; }
    VALUE a[256];
    a[0] = recv;
    int n = eval_args(c, args, a + 1, 255);
    return anpy_invoke(c, m, c->global, a, n + 1);
}

// --- multiple assignment ---------------------------------------------

extern const struct NodeKind kind_node_tgt;

void
anpy_massign(CTX *c, NODE *targets, VALUE v)
{
    for (NODE *t = targets; t->head.kind == &kind_node_tgt; t = t->u.node_tgt.next) {
        switch (t->u.node_tgt.kind) {
          case 0: {  // variable
            anpy_cell *cell = env_lookup(c->env, t->u.node_tgt.name);
            if (cell) cell->v = v;
            break;
          }
          case 1: {  // attribute
            VALUE o = EVAL(c, t->u.node_tgt.obj);
            anpy_setattr(c, o, t->u.node_tgt.name, v);
            break;
          }
          case 2: {  // index
            VALUE s = EVAL(c, t->u.node_tgt.obj);
            VALUE i = EVAL(c, t->u.node_tgt.idx);
            anpy_index_set(c, s, i, v);
            break;
          }
        }
    }
}

// --- global install (top-level defs) ---------------------------------

// Filled by the parser: top-level function and class descriptors, and
// global variable defs.
extern anpy_func **anpy_top_funcs; extern int anpy_n_top_funcs;
extern struct var_decl *anpy_top_vars; extern int anpy_n_top_vars;

void
anpy_install_globals(CTX *c)
{
    anpy_finalize_classes();
    // classes -> classval cells
    for (int i = 0; i < nclasses; i++)
        if (classes[i]->builtin == 0)   // builtin classes (object/int/...) not user-callable as ctors here
            env_define(c->global, classes[i]->name)->v = (VALUE)make_classval(classes[i]);
    // also expose object as constructor (object())
    // top-level functions -> closures capturing global env
    for (int i = 0; i < anpy_n_top_funcs; i++)
        env_define(c->global, anpy_top_funcs[i]->name)->v = (VALUE)make_closure(anpy_top_funcs[i], c->global);
    // global variables -> evaluate literal initialisers
    c->env = c->global;
    for (int i = 0; i < anpy_n_top_vars; i++)
        env_define(c->global, anpy_top_vars[i].name)->v =
            anpy_top_vars[i].init ? EVAL(c, anpy_top_vars[i].init) : ANPY_NONE;
}

// --- EVAL / OPTIMIZE / INIT ------------------------------------------

VALUE EVAL(CTX *c, NODE *n) { return (*n->head.dispatcher)(c, n); }

NODE *
OPTIMIZE(NODE *n)
{
    if (!OPTION.no_compiled_code) astro_cs_load(n, NULL);
    return n;
}

void code_repo_add(const char *name, NODE *body, bool force) { (void)name; (void)body; (void)force; }

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#if defined(__has_include) && __has_include("node_emit_ast.c")
#include "node_emit_ast.c"
#endif
#include "node_alloc.c"

CTX *
anpy_make_context(void)
{
    CTX *c = (CTX *)GC_MALLOC(sizeof(CTX));
    c->global = env_new(NULL);
    c->env = c->global;
    c->returning = false;
    c->retval = ANPY_NONE;
    return c;
}

void
INIT(void)
{
    GC_INIT();
    astro_cs_init("code_store", ".", 0);
}

// AOT: every function/method body is dispatched at runtime via
// EVAL(c, fn->body), so each must be its own SD entry.
static void compile_func(anpy_func *fn) {
    if (fn->body) astro_cs_compile(fn->body, NULL);
    for (int i = 0; i < fn->nnested; i++) compile_func(fn->nested[i]);
}
static void load_func(anpy_func *fn) {
    if (fn->body) astro_cs_load(fn->body, NULL);
    for (int i = 0; i < fn->nnested; i++) load_func(fn->nested[i]);
}

void
anpy_aot_specialize(NODE *body, anpy_func **funcs, int nfuncs)
{
    if (body) astro_cs_compile(body, NULL);
    for (int i = 0; i < nfuncs; i++) compile_func(funcs[i]);
    for (int i = 0; i < nclasses; i++)
        for (int m = 0; m < classes[i]->own_nmethods; m++) compile_func(classes[i]->own_methods[m].fn);

    astro_cs_build(NULL);
    astro_cs_reload();

    if (body) astro_cs_load(body, NULL);
    for (int i = 0; i < nfuncs; i++) load_func(funcs[i]);
    for (int i = 0; i < nclasses; i++)
        for (int m = 0; m < classes[i]->own_nmethods; m++) load_func(classes[i]->own_methods[m].fn);
}
