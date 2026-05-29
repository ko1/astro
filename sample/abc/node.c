#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include <gc.h>
#include "node.h"
#include "context.h"

// =====================================================================
// abc runtime: GC + GMP wiring, the global symbol table (scalars,
// arrays, functions), function calls with bc's dynamic save/restore
// scoping, runtime-error unwinding, and the EVAL/OPTIMIZE/INIT glue.
// =====================================================================

// --- AST node allocation (permanent; not GC, never freed) ------------
// AST nodes only reference C string literals and other nodes, never
// bcnum values, so they live outside the GC heap.

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (n == NULL) { fprintf(stderr, "Memory allocation failed\n"); exit(EXIT_FAILURE); }
    return n;
}

// --- GMP allocators routed through the GC ----------------------------
// GMP limbs hold no pointers, so they are atomic GC objects; the bcnum
// struct that owns the mpz (and thus the limb pointer) is scanned.

static void *gmp_alloc(size_t n)                       { return GC_MALLOC_ATOMIC(n); }
static void *gmp_realloc(void *p, size_t o, size_t n)  { (void)o; return GC_REALLOC(p, n); }
static void  gmp_free(void *p, size_t n)               { (void)p; (void)n; /* GC reclaims */ }

// --- runtime error unwinding -----------------------------------------

static jmp_buf bc_toplevel_jmp;
static volatile int bc_jmp_active = 0;

jmp_buf *bc_get_jmp(void) { return &bc_toplevel_jmp; }
void bc_set_jmp_active(int v) { bc_jmp_active = v; }

void
bc_runtime_error(CTX *c, const char *fmt, ...)
{
    fflush(stdout);
    fputs("Runtime error: ", stderr);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (c) { c->flow = FLOW_NORMAL; c->out_col = 0; }
    if (bc_jmp_active) longjmp(bc_toplevel_jmp, 1);
    exit(1);
}

// --- common runtime helpers (HASH / DUMP / hash funcs) ---------------
#include "astro_node.c"
// --- code store (specialization / AOT) -------------------------------
#include "astro_code_store.c"
// --- build orchestrator (astro_build_*) ------------------------------
#include "astro_build.c"

// =====================================================================
// Symbol table: scalars, arrays and functions share one name space of
// slots (bc keeps separate name spaces, but a single slot with three
// fields models that without collisions).
// =====================================================================

struct bc_array {
    bcnum **e;     // index -> value (NULL == 0)
    long cap;
};

struct var_slot {
    const char *name;
    bcnum *scalar;
    struct bc_array *arr;
    struct bc_func *func;
    struct var_slot *next;
};

#define BC_NBUCKETS 509
struct bc_symtab {
    struct var_slot *buckets[BC_NBUCKETS];
};

static size_t
name_hash(const char *s)
{
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h % BC_NBUCKETS;
}

static struct var_slot *
slot_find(CTX *c, const char *name)
{
    for (struct var_slot *s = c->vars->buckets[name_hash(name)]; s; s = s->next)
        if (strcmp(s->name, name) == 0) return s;
    return NULL;
}

static struct var_slot *
slot_intern(CTX *c, const char *name)
{
    const size_t b = name_hash(name);
    for (struct var_slot *s = c->vars->buckets[b]; s; s = s->next)
        if (strcmp(s->name, name) == 0) return s;
    struct var_slot *s = (struct var_slot *)GC_MALLOC(sizeof(*s));
    s->name = GC_STRDUP(name);
    s->next = c->vars->buckets[b];
    c->vars->buckets[b] = s;
    return s;
}

// --- special variables (scale / ibase / obase / last) ----------------

enum { SP_NONE = 0, SP_SCALE, SP_IBASE, SP_OBASE, SP_LAST };

static int
special_var(const char *name)
{
    if (name[0] == 's' && strcmp(name, "scale") == 0) return SP_SCALE;
    if (name[0] == 'i' && strcmp(name, "ibase") == 0) return SP_IBASE;
    if (name[0] == 'o' && strcmp(name, "obase") == 0) return SP_OBASE;
    if (name[0] == 'l' && strcmp(name, "last")  == 0) return SP_LAST;
    return SP_NONE;
}

VALUE
bc_var_get(CTX *c, const char *name)
{
    switch (special_var(name)) {
      case SP_SCALE: return bc_from_long(c->scale);
      case SP_IBASE: return bc_from_long(c->ibase);
      case SP_OBASE: return bc_from_long(c->obase);
      case SP_LAST:  return c->last ? c->last : bc_alloc();
    }
    struct var_slot *s = slot_find(c, name);
    return (s && s->scalar) ? s->scalar : bc_alloc();
}

VALUE
bc_var_set(CTX *c, const char *name, VALUE val)
{
    switch (special_var(name)) {
      case SP_SCALE: {
        long v = bc_to_long(val); if (v < 0) v = 0;
        c->scale = v; return bc_from_long(v);
      }
      case SP_IBASE: {
        long v = bc_to_long(val); if (v < 2) v = 2; if (v > 16) v = 16;
        c->ibase = v; return bc_from_long(v);
      }
      case SP_OBASE: {
        long v = bc_to_long(val); if (v < 2) v = 2; if (v > BC_OBASE_MAX) v = BC_OBASE_MAX;
        c->obase = v; return bc_from_long(v);
      }
      case SP_LAST: c->last = val; return val;
    }
    struct var_slot *s = slot_intern(c, name);
    s->scalar = val;
    return val;
}

#define BC_ARRAY_MAX 16777216   // 16M elements cap (guards runaway index)

VALUE
bc_arr_get(CTX *c, const char *name, long idx)
{
    if (idx < 0) { bc_runtime_error(c, "Negative array index"); return bc_alloc(); }
    struct var_slot *s = slot_find(c, name);
    if (!s || !s->arr || idx >= s->arr->cap || !s->arr->e[idx]) return bc_alloc();
    return s->arr->e[idx];
}

VALUE
bc_arr_set(CTX *c, const char *name, long idx, VALUE val)
{
    if (idx < 0) { bc_runtime_error(c, "Negative array index"); return val; }
    if (idx >= BC_ARRAY_MAX) { bc_runtime_error(c, "Array index too large"); return val; }
    struct var_slot *s = slot_intern(c, name);
    if (!s->arr) { s->arr = (struct bc_array *)GC_MALLOC(sizeof(struct bc_array)); s->arr->cap = 0; s->arr->e = NULL; }
    if (idx >= s->arr->cap) {
        long ncap = s->arr->cap ? s->arr->cap : 8;
        while (ncap <= idx) ncap *= 2;
        bcnum **ne = (bcnum **)GC_MALLOC(sizeof(bcnum *) * (size_t)ncap);
        for (long i = 0; i < s->arr->cap; i++) ne[i] = s->arr->e[i];
        s->arr->e = ne;
        s->arr->cap = ncap;
    }
    s->arr->e[idx] = val;
    return val;
}

// --- functions -------------------------------------------------------

void
bc_register_func(CTX *c, struct bc_func *f)
{
    struct var_slot *s = slot_intern(c, f->name);
    s->func = f;   // redefinition simply replaces (bc allows it)
}

struct bc_func *
bc_lookup_func(CTX *c, const char *name)
{
    struct var_slot *s = slot_find(c, name);
    return s ? s->func : NULL;
}

// Walk the node_arg cons list of a call site.
extern const struct NodeKind kind_node_arg;

VALUE
bc_call(CTX *c, const char *name, NODE *args)
{
    struct bc_func *f = bc_lookup_func(c, name);
    if (!f) { bc_runtime_error(c, "Function %s not defined", name); return bc_alloc(); }

    // Evaluate arguments in the caller's scope (left to right).
    bcnum *argv[256];
    int argc = 0;
    for (NODE *a = args; a->head.kind == &kind_node_arg; a = a->u.node_arg.next) {
        if (argc >= 256) { bc_runtime_error(c, "Too many arguments"); return bc_alloc(); }
        argv[argc++] = EVAL(c, a->u.node_arg.expr);
    }
    if (argc != f->nparams)
        bc_runtime_error(c, "Function %s expects %d arguments, got %d", name, f->nparams, argc);

    // Save current values of params + autos, then bind (dynamic scoping).
    const int nsave = f->nparams + f->nautos;
    bcnum  *saved_scalar[256];
    struct bc_array *saved_arr[256];
    struct var_slot *slots[256];

    for (int i = 0; i < f->nparams; i++) {
        struct var_slot *s = slot_intern(c, f->params[i]);
        slots[i] = s; saved_scalar[i] = s->scalar; saved_arr[i] = s->arr;
        s->scalar = argv[i];   // bcnum is immutable: share, no copy needed
        s->arr = NULL;
    }
    for (int j = 0; j < f->nautos; j++) {
        struct var_slot *s = slot_intern(c, f->autos[j]);
        const int i = f->nparams + j;
        slots[i] = s; saved_scalar[i] = s->scalar; saved_arr[i] = s->arr;
        s->scalar = bc_alloc();         // autos start at 0 / empty array
        s->arr = NULL;
    }

    // Execute the body; a `return` lands here as FLOW_RETURN.
    EVAL(c, f->body);
    bcnum *result = (c->flow == FLOW_RETURN && c->retval) ? c->retval : bc_alloc();
    c->flow = FLOW_NORMAL;

    // Restore caller's bindings.
    for (int i = 0; i < nsave; i++) { slots[i]->scalar = saved_scalar[i]; slots[i]->arr = saved_arr[i]; }
    return result;
}

// =====================================================================
// Context construction
// =====================================================================

CTX *
bc_make_context(void)
{
    CTX *c = (CTX *)GC_MALLOC(sizeof(CTX));
    c->ibase = 10;
    c->obase = 10;
    c->scale = OPTION.math_lib ? 20 : 0;
    c->last = bc_alloc();
    c->flow = FLOW_NORMAL;
    c->retval = NULL;
    c->out_col = 0;
    c->vars = (struct bc_symtab *)GC_MALLOC(sizeof(struct bc_symtab));
    c->interactive = false;
    return c;
}

// =====================================================================
// EVAL / OPTIMIZE / code repo / INIT
// =====================================================================

VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

NODE *
OPTIMIZE(NODE *n)
{
    if (!OPTION.no_compiled_code) astro_cs_load(n, NULL);
    return n;
}

void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}

// --- generated code ---------------------------------------------------
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

void
INIT(void)
{
    GC_INIT();
    mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
    astro_cs_init("code_store", ".", 0);
}

// AOT-specialize a parsed program (the top-level statements plus every
// registered function body, since bodies are dispatched at runtime via
// EVAL(c, f->body) and so must each be their own SD entry).  Builds the
// code store, reloads it, and patches the entry dispatchers in place.
void
bc_aot_specialize(CTX *c, NODE **stmts, int n)
{
    for (int i = 0; i < n; i++) astro_cs_compile(stmts[i], NULL);
    for (size_t b = 0; b < BC_NBUCKETS; b++)
        for (struct var_slot *s = c->vars->buckets[b]; s; s = s->next)
            if (s->func) astro_cs_compile(s->func->body, NULL);

    astro_cs_build(NULL);
    astro_cs_reload();

    for (int i = 0; i < n; i++) astro_cs_load(stmts[i], NULL);
    for (size_t b = 0; b < BC_NBUCKETS; b++)
        for (struct var_slot *s = c->vars->buckets[b]; s; s = s->next)
            if (s->func) astro_cs_load(s->func->body, NULL);
}
