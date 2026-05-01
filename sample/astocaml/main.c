// astocaml — small OCaml interpreter on ASTro.
//
// Parser: hand-written recursive descent, operator precedence per OCaml.
// Runtime: tagged-int VALUE, malloc-backed heap, struct oframe lexical envs.
// Interpreter: tree-walking via ASTroGen-generated dispatchers.

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>
#include <gc.h>
#include "context.h"
#include "node.h"
#include "astro_code_store.h"

// Route every runtime allocation in this TU through Boehm GC.  AST
// nodes (in node.c via node_allocate) stay on plain malloc — they
// outlive the program anyway, no point GC-tracking them.  Boehm's
// conservative scan still picks up NODE * pointers stored inside
// GC objects (closures, frames, globals) and treats them as opaque,
// which is harmless: NODE storage is never freed.
#define malloc(n)        GC_malloc((n))
#define calloc(n, s)     GC_malloc((n) * (s))
#define realloc(p, n)    GC_realloc((p), (n))
#define free(p)          ((void)(p))

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

struct astocaml_option OPTION;

// AOT entry registry (definitions live near the parser); forward-decl
// here so `maybe_aot_compile` can iterate them.  Populated by
// `aot_add_entry` from the `make_fun` parser wrapper.
extern NODE **AOT_ENTRIES;
extern size_t AOT_ENTRIES_LEN;

// AOT compile + reload helper.  Called before EVAL on top-level
// expressions when --compile is set.  No-ops if already specialized.
//
// Beyond compiling the form itself, we also compile every closure body
// registered via `aot_add_entry` (`make_fun`) — without this the AST
// inside `let f x = ...` is never specialized, since
// `SPECIALIZE_node_fun` is empty.  After build+reload, we patch every
// registered entry's dispatcher (not just the form's), so future
// `oc_apply` calls into those closures dispatch through the SD_*.
//
// Repeat invocations are cheap: `astro_cs_compile` skips files that
// already exist; `make` only rebuilds .c files newer than their .o;
// `astro_cs_load` short-circuits if the dispatcher is already SD_*.
extern void astro_cs_init_dispatcher(NODE *n);
static size_t AOT_COMPILED = 0;     // index of next entry to compile

static void
maybe_aot_compile(NODE *n)
{
    if (!OPTION.compile || !n) return;
    if (n->head.flags.is_specialized) return;
    // Disable ccache so make can write into its cache dir even on a
    // sandboxed / read-only FS — ccache becomes a pass-through to gcc.
    setenv("CCACHE_DISABLE", "1", 1);
    // Trim per-call alloca cost: stack-clash protection emits a probe
    // loop on every variable-size alloca which adds ~10 dead
    // instructions per fib-style recursive call.  The fast path's
    // alloca is small (24 bytes) so the probe is unnecessary.
    // LTO + relaxed inline limits let gcc fold child SDs (lref / cmp /
    // const) into the recursive-call SD across .o boundaries.
    setenv("ASTRO_EXTRA_CFLAGS",
           "-fno-stack-clash-protection -fno-stack-protector "
           "-flto -finline-functions -finline-small-functions "
           "-finline-limit=10000 --param max-inline-insns-auto=400 "
           "--param max-inline-insns-single=400 --param inline-unit-growth=300", 1);
    setenv("ASTRO_EXTRA_LDFLAGS", "-flto", 1);

    // Compile any closure bodies registered since last call.
    for (; AOT_COMPILED < AOT_ENTRIES_LEN; AOT_COMPILED++) {
        NODE *e = AOT_ENTRIES[AOT_COMPILED];
        if (e && !e->head.flags.is_specialized) astro_cs_compile(e, NULL);
    }
    astro_cs_compile(n, NULL);
    astro_cs_build(NULL);
    astro_cs_reload();
    // Patch every body's dispatcher, then the form itself.
    size_t patched = 0;
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++) {
        if (AOT_ENTRIES[i] && astro_cs_load(AOT_ENTRIES[i], NULL)) patched++;
    }
    if (astro_cs_load(n, NULL)) patched++;
    if (getenv("ASTOCAML_AOT_VERBOSE"))
        fprintf(stderr, "astocaml: AOT patched %zu / %zu entries\n",
                patched, AOT_ENTRIES_LEN + 1);
}

// ---------------------------------------------------------------------------
// Singleton oobj's.
// ---------------------------------------------------------------------------

struct oobj OC_UNIT_OBJ  = { .type = OOBJ_UNIT };
struct oobj OC_TRUE_OBJ  = { .type = OOBJ_BOOL, .b = true };
struct oobj OC_FALSE_OBJ = { .type = OOBJ_BOOL, .b = false };
struct oobj OC_NIL_OBJ   = { .type = OOBJ_NIL };

// ---------------------------------------------------------------------------
// Allocation.
// ---------------------------------------------------------------------------

struct oobj *
oc_alloc(int type)
{
    // malloc + manual type-tag init: caller will overwrite the union
    // member it cares about, so zero-initing the whole ~64-byte oobj
    // (calloc) is wasted memset traffic on a hot path.
    struct oobj *o = (struct oobj *)malloc(sizeof(struct oobj));
    if (!o) { fprintf(stderr, "astocaml: oom\n"); exit(1); }
    o->type = type;
    return o;
}

// One `GC_malloc` per cons cell — properly collectable.  Earlier
// versions bump-allocated from a 4 MB chunk for raw speed, but
// Boehm sees the chunk as one opaque object: a single live cell
// pinned the entire 4 MB.  Per-cell allocation costs ~30 ns of
// Boehm small-object fast path per cons, but the cells get
// collected so RSS stays flat across long-running list-heavy code.
//
// Allocate just `{type, head, tail}` (24 bytes); reads of other
// oobj union members are guarded by `OC_IS_CONS` so the truncated
// allocation is safe.
VALUE
oc_cons(VALUE h, VALUE t)
{
    struct oobj *o = (struct oobj *)malloc(offsetof(struct oobj, cons) + sizeof(o->cons));
    if (!o) { fprintf(stderr, "astocaml: oom\n"); exit(1); }
    o->type = OOBJ_CONS;
    o->cons.head = h;
    o->cons.tail = t;
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_string(const char *s, size_t len)
{
    struct oobj *o = oc_alloc(OOBJ_STRING);
    o->str.chars = (char *)malloc(len + 1);
    memcpy(o->str.chars, s, len);
    o->str.chars[len] = '\0';
    o->str.len = len;
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_closure(struct Node *body, struct oframe *env, int nparams)
{
    struct oobj *o = oc_alloc(OOBJ_CLOSURE);
    o->closure.body = body;
    o->closure.env = env;
    o->closure.nparams = nparams;
    o->closure.is_leaf = false;
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_closure_ex(struct Node *body, struct oframe *env, int nparams, bool is_leaf)
{
    struct oobj *o = oc_alloc(OOBJ_CLOSURE);
    o->closure.body = body;
    o->closure.env = env;
    o->closure.nparams = nparams;
    o->closure.is_leaf = is_leaf;
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_prim(const char *name, oc_prim_fn fn, int min_argc, int max_argc)
{
    struct oobj *o = oc_alloc(OOBJ_PRIM);
    o->prim.name = name;
    o->prim.fn = fn;
    o->prim.min_argc = min_argc;
    o->prim.max_argc = max_argc;
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_tuple(int n, VALUE *items)
{
    struct oobj *o = oc_alloc(OOBJ_TUPLE);
    o->tup.n = n;
    o->tup.items = (VALUE *)malloc(sizeof(VALUE) * (n ? n : 1));
    for (int i = 0; i < n; i++) o->tup.items[i] = items[i];
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_ref(VALUE init)
{
    struct oobj *o = oc_alloc(OOBJ_REF);
    o->refval = init;
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_float(double d)
{
    struct oobj *o = oc_alloc(OOBJ_FLOAT);
    o->dbl = d;
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_variant(const char *name, int n, VALUE *items)
{
    struct oobj *o = oc_alloc(OOBJ_VARIANT);
    o->var.name = name;     // expected interned (strdup'd) elsewhere
    o->var.n = n;
    if (n > 0) {
        o->var.items = (VALUE *)malloc(sizeof(VALUE) * n);
        for (int i = 0; i < n; i++) o->var.items[i] = items[i];
    }
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_exn(const char *name, int n, VALUE *items)
{
    return oc_make_variant(name, n, items);     // unified as variant
}

VALUE
oc_make_record(int n, const char **fields, VALUE *items)
{
    struct oobj *o = oc_alloc(OOBJ_RECORD);
    o->rec.n = n;
    o->rec.fields = (const char **)malloc(sizeof(char *) * (n ? n : 1));
    o->rec.items  = (VALUE *)malloc(sizeof(VALUE) * (n ? n : 1));
    for (int i = 0; i < n; i++) {
        o->rec.fields[i] = fields[i];
        o->rec.items[i]  = items[i];
    }
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_array(int n, VALUE *items)
{
    struct oobj *o = oc_alloc(OOBJ_ARRAY);
    o->arr.n = n;
    o->arr.items = (VALUE *)malloc(sizeof(VALUE) * (n ? n : 1));
    for (int i = 0; i < n; i++) o->arr.items[i] = items[i];
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_lazy(struct Node *body, struct oframe *env)
{
    struct oobj *o = oc_alloc(OOBJ_LAZY);
    o->lazy.forced = false;
    o->lazy.body = body;
    o->lazy.env = env;
    return OC_OBJ_VAL(o);
}

VALUE
oc_force_lazy(CTX *c, VALUE v)
{
    if (!OC_IS_LAZY(v)) return v;
    struct oobj *o = OC_PTR(v);
    if (o->lazy.forced) return o->lazy.value;
    struct oframe *saved = c->env;
    c->env = o->lazy.env;
    VALUE r = EVAL(c, o->lazy.body);
    c->env = saved;
    o->lazy.forced = true;
    o->lazy.value = r;
    return r;
}

VALUE
oc_make_bytes(size_t len, char fill)
{
    struct oobj *o = oc_alloc(OOBJ_BYTES);
    o->bytes.len = len;
    o->bytes.bytes = (char *)malloc(len + 1);
    memset(o->bytes.bytes, fill, len);
    o->bytes.bytes[len] = '\0';
    return OC_OBJ_VAL(o);
}

VALUE
oc_make_object(int n_methods, const char **method_names, VALUE *closures,
               int n_fields,  const char **field_names, VALUE *field_init)
{
    struct oobj *o = oc_alloc(OOBJ_OBJECT);
    o->obj.n_methods = n_methods;
    o->obj.method_names = (const char **)malloc(sizeof(char *) * (n_methods ? n_methods : 1));
    o->obj.method_closures = (VALUE *)malloc(sizeof(VALUE) * (n_methods ? n_methods : 1));
    for (int i = 0; i < n_methods; i++) {
        o->obj.method_names[i] = method_names[i];
        o->obj.method_closures[i] = closures[i];
    }
    o->obj.n_fields = n_fields;
    o->obj.field_names = (const char **)malloc(sizeof(char *) * (n_fields ? n_fields : 1));
    o->obj.field_values = (VALUE *)malloc(sizeof(VALUE) * (n_fields ? n_fields : 1));
    for (int i = 0; i < n_fields; i++) {
        o->obj.field_names[i] = field_names[i];
        o->obj.field_values[i] = field_init[i];
    }
    return OC_OBJ_VAL(o);
}

VALUE
oc_object_get_field(VALUE obj, const char *field)
{
    if (!OC_IS_OBJECT(obj)) return OC_UNIT;
    struct oobj *o = OC_PTR(obj);
    for (int i = 0; i < o->obj.n_fields; i++) {
        if (strcmp(o->obj.field_names[i], field) == 0)
            return o->obj.field_values[i];
    }
    return OC_UNIT;
}

void
oc_object_set_field(VALUE obj, const char *field, VALUE v)
{
    if (!OC_IS_OBJECT(obj)) return;
    struct oobj *o = OC_PTR(obj);
    for (int i = 0; i < o->obj.n_fields; i++) {
        if (strcmp(o->obj.field_names[i], field) == 0) {
            o->obj.field_values[i] = v;
            return;
        }
    }
}

// Pure lookup — returns the method closure VALUE, or 0 (an invalid VALUE
// since no oobj lives at address 0) when the name is not a method on this
// object.  Used by the `node_send` IC fast path to avoid both the strcmp
// loop and the field-fallback machinery on the hot side.
VALUE
oc_object_lookup_method(VALUE obj, const char *method)
{
    if (!OC_IS_OBJECT(obj)) return 0;
    struct oobj *o = OC_PTR(obj);
    for (int i = 0; i < o->obj.n_methods; i++) {
        if (strcmp(o->obj.method_names[i], method) == 0)
            return o->obj.method_closures[i];
    }
    return 0;
}

VALUE
oc_object_send(CTX *c, VALUE obj, const char *method, int argc, VALUE *argv)
{
    if (!OC_IS_OBJECT(obj))
        oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("not an object", 13)}));
    struct oobj *o = OC_PTR(obj);
    // 1. Try methods (with self prepended).
    for (int i = 0; i < o->obj.n_methods; i++) {
        if (strcmp(o->obj.method_names[i], method) == 0) {
            VALUE *all_args = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            all_args[0] = obj;
            for (int j = 0; j < argc; j++) all_args[j + 1] = argv[j];
            return oc_apply(c, o->obj.method_closures[i], argc + 1, all_args);
        }
    }
    // 2. Fallback: field read (0 args).
    if (argc == 0) {
        for (int i = 0; i < o->obj.n_fields; i++) {
            if (strcmp(o->obj.field_names[i], method) == 0)
                return o->obj.field_values[i];
        }
    }
    // 3. Fallback: `set_X v` writes field X (1 arg).
    if (argc == 1 && strncmp(method, "set_", 4) == 0) {
        const char *fname = method + 4;
        for (int i = 0; i < o->obj.n_fields; i++) {
            if (strcmp(o->obj.field_names[i], fname) == 0) {
                o->obj.field_values[i] = argv[0];
                return OC_UNIT;
            }
        }
    }
    char buf[128];
    snprintf(buf, sizeof buf, "no such method: %s", method);
    oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string(buf, strlen(buf))}));
}

double
oc_get_float(VALUE v)
{
    if (OC_IS_INT(v)) return (double)OC_INT_VAL(v);
    if (OC_IS_FLOAT(v)) return OC_PTR(v)->dbl;
    return 0.0;     // OCaml would type-error; we treat as 0
}

VALUE
oc_string_concat(VALUE a, VALUE b)
{
    if (!OC_IS_STRING(a) || !OC_IS_STRING(b)) return oc_make_string("", 0);
    struct oobj *as = OC_PTR(a);
    struct oobj *bs = OC_PTR(b);
    size_t n = as->str.len + bs->str.len;
    struct oobj *o = oc_alloc(OOBJ_STRING);
    o->str.chars = (char *)malloc(n + 1);
    memcpy(o->str.chars, as->str.chars, as->str.len);
    memcpy(o->str.chars + as->str.len, bs->str.chars, bs->str.len);
    o->str.chars[n] = '\0';
    o->str.len = n;
    return OC_OBJ_VAL(o);
}

bool
oc_structural_eq(VALUE a, VALUE b)
{
    if (a == b) return true;
    if (OC_IS_INT(a) || OC_IS_INT(b)) return false;     // ints are encoded inline
    if (!OC_IS_PTR(a) || !OC_IS_PTR(b)) return false;
    struct oobj *ao = OC_PTR(a), *bo = OC_PTR(b);
    if (ao->type != bo->type) {
        // int (already returned above) vs float — OCaml-style coercion
        // would be a type error, but both forms can come from generic code;
        // be lenient on float equality.
        if ((ao->type == OOBJ_FLOAT) && (bo->type == OOBJ_FLOAT))
            return ao->dbl == bo->dbl;
        return false;
    }
    switch (ao->type) {
    case OOBJ_CONS:
        return oc_structural_eq(ao->cons.head, bo->cons.head) &&
               oc_structural_eq(ao->cons.tail, bo->cons.tail);
    case OOBJ_STRING:
        return ao->str.len == bo->str.len &&
               memcmp(ao->str.chars, bo->str.chars, ao->str.len) == 0;
    case OOBJ_TUPLE:
        if (ao->tup.n != bo->tup.n) return false;
        for (int i = 0; i < ao->tup.n; i++)
            if (!oc_structural_eq(ao->tup.items[i], bo->tup.items[i])) return false;
        return true;
    case OOBJ_FLOAT:
        return ao->dbl == bo->dbl;
    case OOBJ_REF:
        return oc_structural_eq(ao->refval, bo->refval);
    case OOBJ_VARIANT:
        if (strcmp(ao->var.name, bo->var.name) != 0) return false;
        if (ao->var.n != bo->var.n) return false;
        for (int i = 0; i < ao->var.n; i++)
            if (!oc_structural_eq(ao->var.items[i], bo->var.items[i])) return false;
        return true;
    case OOBJ_RECORD:
        if (ao->rec.n != bo->rec.n) return false;
        for (int i = 0; i < ao->rec.n; i++) {
            if (strcmp(ao->rec.fields[i], bo->rec.fields[i]) != 0) return false;
            if (!oc_structural_eq(ao->rec.items[i], bo->rec.items[i])) return false;
        }
        return true;
    case OOBJ_ARRAY:
        if (ao->arr.n != bo->arr.n) return false;
        for (int i = 0; i < ao->arr.n; i++)
            if (!oc_structural_eq(ao->arr.items[i], bo->arr.items[i])) return false;
        return true;
    default:
        return false;
    }
}

int
oc_compare(VALUE a, VALUE b)
{
    if (a == b) return 0;
    // Both ints — fast.
    if (OC_IS_INT(a) && OC_IS_INT(b)) {
        int64_t ai = OC_INT_VAL(a), bi = OC_INT_VAL(b);
        return (ai < bi) ? -1 : (ai > bi) ? 1 : 0;
    }
    // Mixed numeric (int / float) — coerce both to double.
    if ((OC_IS_INT(a) || OC_IS_FLOAT(a)) && (OC_IS_INT(b) || OC_IS_FLOAT(b))) {
        double ad = oc_get_float(a), bd = oc_get_float(b);
        return (ad < bd) ? -1 : (ad > bd) ? 1 : 0;
    }
    // Bool / unit / nil singletons compare via address.
    if (!OC_IS_PTR(a) || !OC_IS_PTR(b)) {
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    struct oobj *ao = OC_PTR(a), *bo = OC_PTR(b);
    // Different types — order by type tag (matches OCaml's "incomparable" loosely).
    if (ao->type != bo->type) return (ao->type < bo->type) ? -1 : 1;
    switch (ao->type) {
    case OOBJ_FLOAT: {
        double ad = ao->dbl, bd = bo->dbl;
        return (ad < bd) ? -1 : (ad > bd) ? 1 : 0;
    }
    case OOBJ_STRING: {
        size_t n = ao->str.len < bo->str.len ? ao->str.len : bo->str.len;
        int c = memcmp(ao->str.chars, bo->str.chars, n);
        if (c != 0) return c < 0 ? -1 : 1;
        return (ao->str.len < bo->str.len) ? -1 : (ao->str.len > bo->str.len) ? 1 : 0;
    }
    case OOBJ_CONS: {
        int c = oc_compare(ao->cons.head, bo->cons.head);
        if (c != 0) return c;
        return oc_compare(ao->cons.tail, bo->cons.tail);
    }
    case OOBJ_TUPLE: {
        int n = ao->tup.n < bo->tup.n ? ao->tup.n : bo->tup.n;
        for (int i = 0; i < n; i++) {
            int c = oc_compare(ao->tup.items[i], bo->tup.items[i]);
            if (c != 0) return c;
        }
        return (ao->tup.n < bo->tup.n) ? -1 : (ao->tup.n > bo->tup.n) ? 1 : 0;
    }
    case OOBJ_VARIANT: {
        int c = strcmp(ao->var.name, bo->var.name);
        if (c != 0) return c < 0 ? -1 : 1;
        int n = ao->var.n < bo->var.n ? ao->var.n : bo->var.n;
        for (int i = 0; i < n; i++) {
            int cc = oc_compare(ao->var.items[i], bo->var.items[i]);
            if (cc != 0) return cc;
        }
        return (ao->var.n < bo->var.n) ? -1 : (ao->var.n > bo->var.n) ? 1 : 0;
    }
    case OOBJ_RECORD: {
        int n = ao->rec.n < bo->rec.n ? ao->rec.n : bo->rec.n;
        for (int i = 0; i < n; i++) {
            int c = oc_compare(ao->rec.items[i], bo->rec.items[i]);
            if (c != 0) return c;
        }
        return (ao->rec.n < bo->rec.n) ? -1 : (ao->rec.n > bo->rec.n) ? 1 : 0;
    }
    case OOBJ_ARRAY: {
        int n = ao->arr.n < bo->arr.n ? ao->arr.n : bo->arr.n;
        for (int i = 0; i < n; i++) {
            int c = oc_compare(ao->arr.items[i], bo->arr.items[i]);
            if (c != 0) return c;
        }
        return (ao->arr.n < bo->arr.n) ? -1 : (ao->arr.n > bo->arr.n) ? 1 : 0;
    }
    case OOBJ_REF:
        return oc_compare(ao->refval, bo->refval);
    default:
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
}

// ---------------------------------------------------------------------------
// Frames.
// ---------------------------------------------------------------------------

struct oframe *
oc_new_frame(struct oframe *parent, int nslots)
{
    size_t bytes = sizeof(struct oframe) + sizeof(VALUE) * (nslots ? nslots : 1);
    struct oframe *f = (struct oframe *)malloc(bytes);
    if (!f) { fprintf(stderr, "astocaml: oom\n"); exit(1); }
    f->parent = parent;
    f->nslots = nslots;
    return f;
}

// ---------------------------------------------------------------------------
// Apply.
// ---------------------------------------------------------------------------

struct partial_state {
    VALUE fn;
    int captured;
    int nparams;
    VALUE *args;
};

VALUE
oc_apply(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    VALUE local_argv[16];   // only used after a tail re-entry
    bool first_iter = true; // false after the first tail-trampoline goto loop;
                            // controls whether closure-leaf alloca is safe
loop:
    if (OC_IS_PRIM(fn)) {
        struct oobj *p = OC_PTR(fn);
        // Partial-application sentinel: fn==NULL, name carries partial_state.
        if (p->prim.fn == NULL) {
            struct partial_state *ps = (struct partial_state *)p->prim.name;
            int total = ps->captured + argc;
            VALUE *combined = (VALUE *)alloca(sizeof(VALUE) * (total ? total : 1));
            for (int i = 0; i < ps->captured; i++) combined[i] = ps->args[i];
            for (int i = 0; i < argc;         i++) combined[ps->captured + i] = argv[i];
            fn = ps->fn;
            argc = total;
            argv = combined;
            goto loop;
        }
        if (argc < p->prim.min_argc ||
            (p->prim.max_argc >= 0 && argc > p->prim.max_argc)) {
            oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){
                oc_make_string("primitive arity mismatch", 24)}));
        }
        return p->prim.fn(c, argc, argv);
    }
    if (!OC_IS_CLOSURE(fn)) {
        oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){
            oc_make_string("applying a non-function value", 29)}));
    }
    {
    struct oobj *cl = OC_PTR(fn);
    int np = cl->closure.nparams;
    if (argc < np) {
        // Partial application: build a wrapper closure that captures the
        // supplied args.  Implemented as an OOBJ_PRIM sentinel with
        // fn==NULL and partial_state stashed in `name`.
        VALUE *capt = (VALUE *)malloc(sizeof(VALUE) * argc);
        for (int i = 0; i < argc; i++) capt[i] = argv[i];
        struct oobj *p = oc_alloc(OOBJ_PRIM);
        struct partial_state *ps = (struct partial_state *)malloc(sizeof *ps);
        ps->fn = fn;
        ps->captured = argc;
        ps->nparams = np;
        ps->args = capt;
        p->prim.fn = NULL;
        p->prim.name = (const char *)ps;
        p->prim.min_argc = np - argc;
        p->prim.max_argc = -1;
        return OC_OBJ_VAL(p);
    }
    struct oframe *f;
    if (LIKELY(cl->closure.is_leaf && first_iter)) {
        // Leaf closure on first oc_apply iteration: frame can live in our
        // C stack — body cannot capture it (no inner closure / lazy).
        // On tail-trampoline re-entry (goto loop) `first_iter` is false,
        // so we revert to malloc to prevent stack growth across tail iters.
        size_t bytes = sizeof(struct oframe) + sizeof(VALUE) * (np ? np : 1);
        f = (struct oframe *)alloca(bytes);
        f->parent = cl->closure.env;
        f->nslots = np;
    }
    else {
        f = oc_new_frame(cl->closure.env, np);
    }
    for (int i = 0; i < np; i++) f->slots[i] = argv[i];
    struct oframe *saved = c->env;
    c->env = f;
    VALUE r = EVAL(c, cl->closure.body);
    c->env = saved;
    if (UNLIKELY(c->tail_call_pending)) {
        c->tail_call_pending = 0;
        fn = c->tc_fn;
        argc = c->tc_argc;
        if (argc > 16) return oc_apply(c, fn, argc, c->tc_argv);
        for (int i = 0; i < argc; i++) local_argv[i] = c->tc_argv[i];
        argv = local_argv;
        first_iter = false;
        goto loop;
    }
    if (argc == np) return r;
    return oc_apply(c, r, argc - np, argv + np);
    }
}

// (partial_state is defined ahead of oc_apply now.)

static VALUE
oc_apply_partial(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    struct partial_state *ps = (struct partial_state *)OC_PTR(fn)->prim.name;
    int total = ps->captured + argc;
    VALUE *combined = (VALUE *)alloca(sizeof(VALUE) * total);
    for (int i = 0; i < ps->captured; i++) combined[i] = ps->args[i];
    for (int i = 0; i < argc;         i++) combined[ps->captured + i] = argv[i];
    return oc_apply(c, ps->fn, total, combined);
}

// ---------------------------------------------------------------------------
// Globals.
// ---------------------------------------------------------------------------

void
oc_global_define(CTX *c, const char *name, VALUE v)
{
    c->globals_serial++;
    for (size_t i = 0; i < c->globals_size; i++) {
        if (strcmp(c->globals[i].name, name) == 0) {
            c->globals[i].value = v;
            return;
        }
    }
    if (c->globals_size == c->globals_capa) {
        size_t cap = c->globals_capa ? c->globals_capa * 2 : 64;
        c->globals = (struct gentry *)realloc(c->globals, cap * sizeof(struct gentry));
        c->globals_capa = cap;
    }
    c->globals[c->globals_size].name = strdup(name);
    c->globals[c->globals_size].value = v;
    c->globals_size++;
}

VALUE
oc_global_ref(CTX *c, const char *name)
{
    for (size_t i = 0; i < c->globals_size; i++) {
        if (strcmp(c->globals[i].name, name) == 0)
            return c->globals[i].value;
    }
    oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){
        oc_make_string(name, strlen(name))}));
}

VALUE
oc_global_ref2(CTX *c, const char *first, const char *second)
{
    for (size_t i = 0; i < c->globals_size; i++) {
        if (strcmp(c->globals[i].name, first) == 0)
            return c->globals[i].value;
    }
    for (size_t i = 0; i < c->globals_size; i++) {
        if (strcmp(c->globals[i].name, second) == 0)
            return c->globals[i].value;
    }
    oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){
        oc_make_string(second, strlen(second))}));
}

// ---------------------------------------------------------------------------
// Error / raise.
// ---------------------------------------------------------------------------

void
oc_error(CTX *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err_msg, sizeof(c->err_msg), fmt, ap);
    va_end(ap);
    if (c->err_jmp_active) longjmp(c->err_jmp, 1);
    fprintf(stderr, "astocaml: %s\n", c->err_msg);
    exit(2);
}

VALUE
oc_run_try(CTX *c, struct Node *body, struct Node *handler)
{
    if (c->handlers_top + 1 >= OC_HANDLER_MAX_DEPTH) oc_error(c, "handler stack overflow");
    int top = ++c->handlers_top;
    struct oc_handler *h = &c->handlers[top];
    h->saved_env = c->env;
    if (setjmp(h->buf) == 0) {
        VALUE r = EVAL(c, body);
        c->handlers_top--;
        return r;
    }
    VALUE exn = h->exn;
    c->env = h->saved_env;
    c->handlers_top--;
    struct oframe *f = oc_new_frame(c->env, 1);
    f->slots[0] = exn;
    struct oframe *saved = c->env;
    c->env = f;
    VALUE r = EVAL(c, handler);
    c->env = saved;
    return r;
}

void
oc_type_error(CTX *c, const char *op, const char *expected)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s: expected %s", op, expected);
    oc_raise(c, oc_make_variant("Type_error", 1, (VALUE[]){oc_make_string(buf, strlen(buf))}));
}

void
oc_raise(CTX *c, VALUE exn)
{
    if (c->handlers_top < 0) {
        if (OC_IS_VARIANT(exn)) {
            const char *name = OC_PTR(exn)->var.name;
            if (OC_PTR(exn)->var.n > 0 && OC_IS_STRING(OC_PTR(exn)->var.items[0])) {
                snprintf(c->err_msg, sizeof c->err_msg, "Uncaught exception: %s(%s)",
                         name, OC_PTR(OC_PTR(exn)->var.items[0])->str.chars);
            }
            else {
                snprintf(c->err_msg, sizeof c->err_msg, "Uncaught exception: %s", name);
            }
        }
        else {
            snprintf(c->err_msg, sizeof c->err_msg, "Uncaught exception (non-variant value)");
        }
        if (c->err_jmp_active) longjmp(c->err_jmp, 1);
        fprintf(stderr, "astocaml: %s\n", c->err_msg);
        exit(2);
    }
    struct oc_handler *h = &c->handlers[c->handlers_top];
    h->exn = exn;
    longjmp(h->buf, 1);
}

// ---------------------------------------------------------------------------
// Display.
// ---------------------------------------------------------------------------

void oc_display_inner(FILE *fp, VALUE v, bool quote_strings);

void
oc_display(FILE *fp, VALUE v)
{
    oc_display_inner(fp, v, false);
}

void
oc_display_inner(FILE *fp, VALUE v, bool quote_strings)
{
    if (OC_IS_INT(v)) {
        fprintf(fp, "%lld", (long long)OC_INT_VAL(v));
        return;
    }
    if (v == OC_UNIT)  { fputs("()",     fp); return; }
    if (v == OC_TRUE)  { fputs("true",   fp); return; }
    if (v == OC_FALSE) { fputs("false",  fp); return; }
    if (v == OC_NIL)   { fputs("[]",     fp); return; }
    struct oobj *o = OC_PTR(v);
    switch (o->type) {
    case OOBJ_CONS: {
        fputc('[', fp);
        oc_display_inner(fp, o->cons.head, true);
        VALUE t = o->cons.tail;
        while (OC_IS_CONS(t)) {
            fputs("; ", fp);
            oc_display_inner(fp, OC_PTR(t)->cons.head, true);
            t = OC_PTR(t)->cons.tail;
        }
        if (t != OC_NIL) {
            fputs(" :: ", fp);
            oc_display_inner(fp, t, true);
        }
        fputc(']', fp);
        return;
    }
    case OOBJ_STRING:
        if (quote_strings) {
            fputc('"', fp);
            fwrite(o->str.chars, 1, o->str.len, fp);
            fputc('"', fp);
        }
        else {
            fwrite(o->str.chars, 1, o->str.len, fp);
        }
        return;
    case OOBJ_FLOAT:
        // OCaml uses a precision that round-trips; %.17g is safe.
        fprintf(fp, "%g", o->dbl);
        return;
    case OOBJ_TUPLE: {
        fputc('(', fp);
        for (int i = 0; i < o->tup.n; i++) {
            if (i) fputs(", ", fp);
            oc_display_inner(fp, o->tup.items[i], true);
        }
        fputc(')', fp);
        return;
    }
    case OOBJ_REF: {
        fputs("{contents = ", fp);
        oc_display_inner(fp, o->refval, true);
        fputc('}', fp);
        return;
    }
    case OOBJ_VARIANT:
        fputs(o->var.name, fp);
        if (o->var.n > 0) {
            fputs(" (", fp);
            for (int i = 0; i < o->var.n; i++) {
                if (i) fputs(", ", fp);
                oc_display_inner(fp, o->var.items[i], true);
            }
            fputc(')', fp);
        }
        return;
    case OOBJ_RECORD:
        fputs("{", fp);
        for (int i = 0; i < o->rec.n; i++) {
            if (i) fputs("; ", fp);
            fprintf(fp, "%s = ", o->rec.fields[i]);
            oc_display_inner(fp, o->rec.items[i], true);
        }
        fputs("}", fp);
        return;
    case OOBJ_ARRAY:
        fputs("[|", fp);
        for (int i = 0; i < o->arr.n; i++) {
            if (i) fputs("; ", fp);
            oc_display_inner(fp, o->arr.items[i], true);
        }
        fputs("|]", fp);
        return;
    case OOBJ_CLOSURE: fputs("<fun>",  fp); return;
    case OOBJ_PRIM:    fputs("<prim>", fp); return;
    default:           fputs("<?>",    fp); return;
    }
}

// ---------------------------------------------------------------------------
// Type system (HM-lite).
//
// A best-effort static type inferencer that catches the obvious errors
// (`"a" + 1`, `if 5 then ... else ...`, `f arg` where f isn't a function).
// Things the inferencer doesn't know (gref to globals, variants, records,
// objects, modules, lazy, etc.) come out as `TY_ANY` and skip checking
// for that subexpression — the runtime checks added to `node_add` / etc.
// catch what slips through.
// ---------------------------------------------------------------------------

extern struct Node **OC_CALL_ARGS;       // forward (defined alongside other parser tables)

enum ty_kind {
    TY_VAR, TY_INT, TY_BOOL, TY_UNIT, TY_STRING, TY_FLOAT, TY_CHAR,
    TY_FUN, TY_LIST, TY_TUPLE, TY_REF, TY_ARRAY, TY_ANY
};

struct ty {
    enum ty_kind kind;
    int var_id;             // TY_VAR
    struct ty *bound;       // TY_VAR — points to bound type when unified
    int var_level;          // TY_VAR
    struct ty *fun_arg;     // TY_FUN
    struct ty *fun_ret;     // TY_FUN
    struct ty *elem;        // TY_LIST / TY_REF / TY_ARRAY
    int tup_n;              // TY_TUPLE
    struct ty **tup_items;  // TY_TUPLE
};

static struct ty TY_INT_VAL    = { .kind = TY_INT };
static struct ty TY_BOOL_VAL   = { .kind = TY_BOOL };
static struct ty TY_UNIT_VAL   = { .kind = TY_UNIT };
static struct ty TY_STRING_VAL = { .kind = TY_STRING };
static struct ty TY_FLOAT_VAL  = { .kind = TY_FLOAT };
static struct ty TY_CHAR_VAL   = { .kind = TY_CHAR };
static struct ty TY_ANY_VAL    = { .kind = TY_ANY };
#define ty_int    (&TY_INT_VAL)
#define ty_bool   (&TY_BOOL_VAL)
#define ty_unit   (&TY_UNIT_VAL)
#define ty_string (&TY_STRING_VAL)
#define ty_float  (&TY_FLOAT_VAL)
#define ty_char   (&TY_CHAR_VAL)
#define ty_any    (&TY_ANY_VAL)

static int ty_next_var_id = 0;

static struct ty *ty_alloc(enum ty_kind k) {
    struct ty *t = (struct ty *)calloc(1, sizeof *t);
    t->kind = k;
    return t;
}
static struct ty *ty_new_var(int level) {
    struct ty *t = ty_alloc(TY_VAR);
    t->var_id = ty_next_var_id++;
    t->var_level = level;
    return t;
}
static struct ty *ty_fun(struct ty *a, struct ty *r) {
    struct ty *t = ty_alloc(TY_FUN);
    t->fun_arg = a; t->fun_ret = r;
    return t;
}
static struct ty *ty_list(struct ty *e) {
    struct ty *t = ty_alloc(TY_LIST);
    t->elem = e;
    return t;
}
static struct ty *ty_ref_of(struct ty *e) {
    struct ty *t = ty_alloc(TY_REF);
    t->elem = e;
    return t;
}
static struct ty *ty_array_of(struct ty *e) {
    struct ty *t = ty_alloc(TY_ARRAY);
    t->elem = e;
    return t;
}
static struct ty *ty_tuple(int n, struct ty **items) {
    struct ty *t = ty_alloc(TY_TUPLE);
    t->tup_n = n;
    t->tup_items = (struct ty **)malloc(sizeof(struct ty *) * (n ? n : 1));
    for (int i = 0; i < n; i++) t->tup_items[i] = items[i];
    return t;
}

// Resolve TY_VAR chain.
static struct ty *ty_walk(struct ty *t) {
    while (t->kind == TY_VAR && t->bound) t = t->bound;
    return t;
}

static void
ty_print(FILE *fp, struct ty *t)
{
    t = ty_walk(t);
    switch (t->kind) {
    case TY_INT:    fputs("int", fp);    return;
    case TY_BOOL:   fputs("bool", fp);   return;
    case TY_UNIT:   fputs("unit", fp);   return;
    case TY_STRING: fputs("string", fp); return;
    case TY_FLOAT:  fputs("float", fp);  return;
    case TY_CHAR:   fputs("char", fp);   return;
    case TY_ANY:    fputs("?", fp);      return;
    case TY_VAR:    fprintf(fp, "'%c%d", 'a' + (t->var_id % 26), t->var_id / 26); return;
    case TY_FUN:    fputc('(', fp); ty_print(fp, t->fun_arg); fputs(" -> ", fp);
                    ty_print(fp, t->fun_ret); fputc(')', fp); return;
    case TY_LIST:   ty_print(fp, t->elem); fputs(" list", fp); return;
    case TY_REF:    ty_print(fp, t->elem); fputs(" ref", fp); return;
    case TY_ARRAY:  ty_print(fp, t->elem); fputs(" array", fp); return;
    case TY_TUPLE:  fputc('(', fp);
                    for (int i = 0; i < t->tup_n; i++) {
                        if (i) fputs(" * ", fp);
                        ty_print(fp, t->tup_items[i]);
                    }
                    fputc(')', fp); return;
    }
}

// Naive occurs check.
static bool
ty_occurs(int var_id, struct ty *t)
{
    t = ty_walk(t);
    if (t->kind == TY_VAR) return t->var_id == var_id;
    if (t->kind == TY_FUN) return ty_occurs(var_id, t->fun_arg) || ty_occurs(var_id, t->fun_ret);
    if (t->kind == TY_LIST || t->kind == TY_REF || t->kind == TY_ARRAY)
        return ty_occurs(var_id, t->elem);
    if (t->kind == TY_TUPLE) {
        for (int i = 0; i < t->tup_n; i++) if (ty_occurs(var_id, t->tup_items[i])) return true;
    }
    return false;
}

// Unify a and b.  Returns 0 on success, -1 on failure.
static int
ty_unify(struct ty *a, struct ty *b)
{
    a = ty_walk(a); b = ty_walk(b);
    if (a == b) return 0;
    if (a->kind == TY_ANY || b->kind == TY_ANY) return 0;
    if (a->kind == TY_VAR) {
        if (ty_occurs(a->var_id, b)) return -1;
        a->bound = b; return 0;
    }
    if (b->kind == TY_VAR) {
        if (ty_occurs(b->var_id, a)) return -1;
        b->bound = a; return 0;
    }
    if (a->kind != b->kind) return -1;
    switch (a->kind) {
    case TY_FUN:
        if (ty_unify(a->fun_arg, b->fun_arg) < 0) return -1;
        return ty_unify(a->fun_ret, b->fun_ret);
    case TY_LIST: case TY_REF: case TY_ARRAY:
        return ty_unify(a->elem, b->elem);
    case TY_TUPLE:
        if (a->tup_n != b->tup_n) return -1;
        for (int i = 0; i < a->tup_n; i++) {
            if (ty_unify(a->tup_items[i], b->tup_items[i]) < 0) return -1;
        }
        return 0;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Type schemes — for let-polymorphism (HM generalize/instantiate).
// ---------------------------------------------------------------------------

struct ty_scheme {
    int n_quants;
    int *quants;        // var ids that are quantified
    struct ty *body;
};

static struct ty_scheme *
ty_scheme_mono(struct ty *t)
{
    struct ty_scheme *s = (struct ty_scheme *)calloc(1, sizeof *s);
    s->body = t;
    s->n_quants = 0;
    return s;
}

// Walk a type and collect free type vars whose level > the given level
// (they are "free in the type but not bound by the current env").
static void
ty_collect_free_vars(struct ty *t, int level, int *out, int *n_out, int max)
{
    t = ty_walk(t);
    if (t->kind == TY_VAR) {
        if (t->var_level > level) {
            for (int i = 0; i < *n_out; i++) if (out[i] == t->var_id) return;
            if (*n_out < max) out[(*n_out)++] = t->var_id;
        }
        return;
    }
    if (t->kind == TY_FUN) {
        ty_collect_free_vars(t->fun_arg, level, out, n_out, max);
        ty_collect_free_vars(t->fun_ret, level, out, n_out, max);
    }
    else if (t->kind == TY_LIST || t->kind == TY_REF || t->kind == TY_ARRAY) {
        ty_collect_free_vars(t->elem, level, out, n_out, max);
    }
    else if (t->kind == TY_TUPLE) {
        for (int i = 0; i < t->tup_n; i++)
            ty_collect_free_vars(t->tup_items[i], level, out, n_out, max);
    }
}

static struct ty_scheme *
ty_generalize(struct ty *t, int level)
{
    struct ty_scheme *s = (struct ty_scheme *)calloc(1, sizeof *s);
    int quants[64]; int n = 0;
    ty_collect_free_vars(t, level, quants, &n, 64);
    s->n_quants = n;
    if (n) {
        s->quants = (int *)malloc(sizeof(int) * n);
        for (int i = 0; i < n; i++) s->quants[i] = quants[i];
    }
    s->body = t;
    return s;
}

// Substitute: walk type, replace var_id matches with new_vars.
static struct ty *
ty_subst(struct ty *t, int *old_ids, struct ty **new_vars, int n)
{
    t = ty_walk(t);
    if (t->kind == TY_VAR) {
        for (int i = 0; i < n; i++) if (old_ids[i] == t->var_id) return new_vars[i];
        return t;
    }
    if (t->kind == TY_FUN) {
        return ty_fun(ty_subst(t->fun_arg, old_ids, new_vars, n),
                      ty_subst(t->fun_ret, old_ids, new_vars, n));
    }
    if (t->kind == TY_LIST)  return ty_list(ty_subst(t->elem, old_ids, new_vars, n));
    if (t->kind == TY_REF)   return ty_ref_of(ty_subst(t->elem, old_ids, new_vars, n));
    if (t->kind == TY_ARRAY) return ty_array_of(ty_subst(t->elem, old_ids, new_vars, n));
    if (t->kind == TY_TUPLE) {
        struct ty **items = (struct ty **)malloc(sizeof(struct ty *) * t->tup_n);
        for (int i = 0; i < t->tup_n; i++) items[i] = ty_subst(t->tup_items[i], old_ids, new_vars, n);
        struct ty *r = ty_tuple(t->tup_n, items);
        free(items);
        return r;
    }
    return t;
}

static struct ty *
ty_instantiate(struct ty_scheme *s, int level)
{
    if (!s) return ty_any;
    if (s->n_quants == 0) return s->body;
    struct ty **new_vars = (struct ty **)malloc(sizeof(struct ty *) * s->n_quants);
    for (int i = 0; i < s->n_quants; i++) new_vars[i] = ty_new_var(level);
    struct ty *r = ty_subst(s->body, s->quants, new_vars, s->n_quants);
    free(new_vars);
    return r;
}

// Type environment: linked list of scopes parallel to the parser's scope.
struct ty_env {
    struct ty_env *parent;
    int n;
    struct ty_scheme **schemes;     // each entry is a (possibly-quantified) scheme
};

static struct ty_env *
ty_env_push(struct ty_env *parent, int n, struct ty **types)
{
    struct ty_env *e = (struct ty_env *)malloc(sizeof *e);
    e->parent = parent;
    e->n = n;
    e->schemes = (struct ty_scheme **)malloc(sizeof(struct ty_scheme *) * (n ? n : 1));
    for (int i = 0; i < n; i++) e->schemes[i] = ty_scheme_mono(types[i]);
    return e;
}

static struct ty_env *
ty_env_push_schemes(struct ty_env *parent, int n, struct ty_scheme **schemes)
{
    struct ty_env *e = (struct ty_env *)malloc(sizeof *e);
    e->parent = parent;
    e->n = n;
    e->schemes = (struct ty_scheme **)malloc(sizeof(struct ty_scheme *) * (n ? n : 1));
    for (int i = 0; i < n; i++) e->schemes[i] = schemes[i];
    return e;
}

static struct ty *
ty_env_lookup(struct ty_env *env, int depth, int slot, int level)
{
    struct ty_env *e = env;
    while (depth > 0 && e) { e = e->parent; depth--; }
    if (!e || slot < 0 || slot >= e->n) return ty_any;
    return ty_instantiate(e->schemes[slot], level);
}

// ---------------------------------------------------------------------------
// Global type registry — top-level let bindings register their type
// scheme so subsequent gref references can instantiate cleanly.
// ---------------------------------------------------------------------------

struct gtype_entry {
    const char *name;
    struct ty_scheme *scheme;
};
static struct gtype_entry *g_gtypes = NULL;
static size_t g_gtypes_n = 0, g_gtypes_capa = 0;

static void
gtype_define(const char *name, struct ty_scheme *s)
{
    for (size_t i = 0; i < g_gtypes_n; i++) {
        if (strcmp(g_gtypes[i].name, name) == 0) { g_gtypes[i].scheme = s; return; }
    }
    if (g_gtypes_n == g_gtypes_capa) {
        size_t cap = g_gtypes_capa ? g_gtypes_capa * 2 : 64;
        g_gtypes = (struct gtype_entry *)realloc(g_gtypes, cap * sizeof(struct gtype_entry));
        g_gtypes_capa = cap;
    }
    g_gtypes[g_gtypes_n].name = strdup(name);
    g_gtypes[g_gtypes_n].scheme = s;
    g_gtypes_n++;
}

static struct ty *
gtype_lookup(const char *name, int level)
{
    for (size_t i = 0; i < g_gtypes_n; i++) {
        if (strcmp(g_gtypes[i].name, name) == 0)
            return ty_instantiate(g_gtypes[i].scheme, level);
    }
    return ty_any;
}

// Type-error counter.  We collect errors and print all at once after
// each top-level expression, so a single typo doesn't drown the rest.
static int ty_error_count = 0;

static void
ty_err(NODE *n, const char *fmt, ...)
{
    (void)n;
    va_list ap;
    va_start(ap, fmt);
    fputs("astocaml: type error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    ty_error_count++;
}

static struct ty *infer(NODE *n, struct ty_env *env, int level);

// Match dispatcher_name for a given node-kind name (e.g. "node_add").
static bool
ty_node_is(NODE *n, const char *kind_name)
{
    if (!n || !n->head.kind || !n->head.kind->default_dispatcher_name) return false;
    const char *d = n->head.kind->default_dispatcher_name;
    // Skip "DISPATCH_" prefix.
    if (strncmp(d, "DISPATCH_", 9) != 0) return false;
    return strcmp(d + 9, kind_name) == 0;
}

extern bool oc_node_to_int(NODE *n);
extern bool oc_node_to_neg_int(NODE *n);
extern bool oc_node_to_if_bool(NODE *n);
extern bool oc_node_to_logic_bool(NODE *n);

// Helper for binary int ops: infers operands, unifies with int, returns int.
// On success, also swaps the parent node's dispatcher / kind to its
// `_int` specialization — both operands are guaranteed `int` by the
// unify, so the runtime type-tag check in the generic op is dead code.
static struct ty *
infer_arith_int(NODE *parent, NODE *a, NODE *b, struct ty_env *env, int level, const char *op)
{
    struct ty *ta = infer(a, env, level);
    struct ty *tb = infer(b, env, level);
    bool a_ok = (ty_unify(ta, ty_int) >= 0);
    bool b_ok = (ty_unify(tb, ty_int) >= 0);
    if (!a_ok) { ty_err(a, "%s: left operand must be int, got ", op);  ty_print(stderr, ta); fputc('\n', stderr); }
    if (!b_ok) { ty_err(b, "%s: right operand must be int, got ", op); ty_print(stderr, tb); fputc('\n', stderr); }
    if (a_ok && b_ok) oc_node_to_int(parent);
    return ty_int;
}

static struct ty *
infer_arith_float(NODE *a, NODE *b, struct ty_env *env, int level, const char *op)
{
    struct ty *ta = infer(a, env, level);
    struct ty *tb = infer(b, env, level);
    if (ty_unify(ta, ty_float) < 0) {
        ty_err(a, "%s: left operand must be float, got ", op);
        ty_print(stderr, ta); fputc('\n', stderr);
    }
    if (ty_unify(tb, ty_float) < 0) {
        ty_err(b, "%s: right operand must be float, got ", op);
        ty_print(stderr, tb); fputc('\n', stderr);
    }
    return ty_float;
}

extern bool oc_node_to_logic_bool(NODE *n);

static struct ty *
infer_bool_op(NODE *parent, NODE *a, NODE *b, struct ty_env *env, int level, const char *op)
{
    struct ty *ta = infer(a, env, level);
    struct ty *tb = infer(b, env, level);
    bool a_ok = (ty_unify(ta, ty_bool) >= 0);
    bool b_ok = (ty_unify(tb, ty_bool) >= 0);
    if (!a_ok) { ty_err(a, "%s: left operand must be bool, got ", op);  ty_print(stderr, ta); fputc('\n', stderr); }
    if (!b_ok) { ty_err(b, "%s: right operand must be bool, got ", op); ty_print(stderr, tb); fputc('\n', stderr); }
    if (a_ok && b_ok) oc_node_to_logic_bool(parent);
    return ty_bool;
}

static struct ty *
infer(NODE *n, struct ty_env *env, int level)
{
    if (!n) return ty_any;

    // Constants.
    if (ty_node_is(n, "node_const_int") || ty_node_is(n, "node_const_int64")) return ty_int;
    if (ty_node_is(n, "node_const_bool")) return ty_bool;
    if (ty_node_is(n, "node_const_unit")) return ty_unit;
    if (ty_node_is(n, "node_const_str"))  return ty_string;
    if (ty_node_is(n, "node_const_float")) return ty_float;
    if (ty_node_is(n, "node_const_char")) return ty_int;     // chars are ints
    if (ty_node_is(n, "node_const_nil")) {
        return ty_list(ty_new_var(level));
    }

    // Variable references.
    if (ty_node_is(n, "node_lref")) {
        return ty_env_lookup(env, n->u.node_lref.depth, n->u.node_lref.idx, level);
    }
    if (ty_node_is(n, "node_gref")) {
        return gtype_lookup(n->u.node_gref.name, level);
    }
    if (ty_node_is(n, "node_gref_q")) {
        // Try qualified first then bare.
        struct ty *t = gtype_lookup(n->u.node_gref_q.qualified, level);
        if (t->kind != TY_ANY) return t;
        return gtype_lookup(n->u.node_gref_q.bare, level);
    }

    // Arithmetic.
    if (ty_node_is(n, "node_add"))  return infer_arith_int(n, n->u.node_add.a, n->u.node_add.b, env, level, "(+)");
    if (ty_node_is(n, "node_sub"))  return infer_arith_int(n, n->u.node_sub.a, n->u.node_sub.b, env, level, "(-)");
    if (ty_node_is(n, "node_mul"))  return infer_arith_int(n, n->u.node_mul.a, n->u.node_mul.b, env, level, "( * )");
    if (ty_node_is(n, "node_div"))  return infer_arith_int(n, n->u.node_div.a, n->u.node_div.b, env, level, "(/)");
    if (ty_node_is(n, "node_mod"))  return infer_arith_int(n, n->u.node_mod.a, n->u.node_mod.b, env, level, "mod");
    if (ty_node_is(n, "node_neg")) {
        struct ty *t = infer(n->u.node_neg.e, env, level);
        bool ok = (ty_unify(t, ty_int) >= 0);
        if (!ok) {
            ty_err(n->u.node_neg.e, "(unary -): operand must be int, got ");
            ty_print(stderr, t); fputc('\n', stderr);
        }
        if (ok) oc_node_to_neg_int(n);
        return ty_int;
    }
    if (ty_node_is(n, "node_fadd")) return infer_arith_float(n->u.node_fadd.a, n->u.node_fadd.b, env, level, "(+.)");
    if (ty_node_is(n, "node_fsub")) return infer_arith_float(n->u.node_fsub.a, n->u.node_fsub.b, env, level, "(-.)");
    if (ty_node_is(n, "node_fmul")) return infer_arith_float(n->u.node_fmul.a, n->u.node_fmul.b, env, level, "( *. )");
    if (ty_node_is(n, "node_fdiv")) return infer_arith_float(n->u.node_fdiv.a, n->u.node_fdiv.b, env, level, "(/.)");
    if (ty_node_is(n, "node_fneg")) {
        struct ty *t = infer(n->u.node_fneg.e, env, level);
        if (ty_unify(t, ty_float) < 0) {
            ty_err(n->u.node_fneg.e, "(unary -.): operand must be float, got ");
            ty_print(stderr, t); fputc('\n', stderr);
        }
        return ty_float;
    }

    // Boolean.
    if (ty_node_is(n, "node_and"))  return infer_bool_op(n, n->u.node_and.a, n->u.node_and.b, env, level, "(&&)");
    if (ty_node_is(n, "node_or"))   return infer_bool_op(n, n->u.node_or.a,  n->u.node_or.b,  env, level, "(||)");
    if (ty_node_is(n, "node_not")) {
        struct ty *t = infer(n->u.node_not.e, env, level);
        if (ty_unify(t, ty_bool) < 0) {
            ty_err(n->u.node_not.e, "not: operand must be bool, got ");
            ty_print(stderr, t); fputc('\n', stderr);
        }
        return ty_bool;
    }

    // Comparison — polymorphic but operands must unify.
    if (ty_node_is(n, "node_lt") || ty_node_is(n, "node_le") || ty_node_is(n, "node_gt") ||
        ty_node_is(n, "node_ge") || ty_node_is(n, "node_eq") || ty_node_is(n, "node_ne") ||
        ty_node_is(n, "node_phys_eq") || ty_node_is(n, "node_phys_ne")) {
        struct ty *ta = infer(n->u.node_lt.a, env, level);
        struct ty *tb = infer(n->u.node_lt.b, env, level);
        if (ty_unify(ta, tb) < 0) {
            ty_err(n, "comparison operands must have the same type: ");
            ty_print(stderr, ta); fputs(" vs ", stderr); ty_print(stderr, tb); fputc('\n', stderr);
        }
        // If both operands prove to be `int`, swap to the `_int`
        // variant: skips both the IS_INT fast-path branch and the
        // polymorphic `oc_compare` fallback in the generic dispatcher.
        // Note: phys_eq / phys_ne already do a single-cmp on raw
        // VALUEs, so no mapping for them.
        if (ty_unify(ta, ty_int) >= 0 && ty_unify(tb, ty_int) >= 0)
            oc_node_to_int(n);
        return ty_bool;
    }

    // String concat.
    if (ty_node_is(n, "node_concat")) {
        struct ty *ta = infer(n->u.node_concat.a, env, level);
        struct ty *tb = infer(n->u.node_concat.b, env, level);
        if (ty_unify(ta, ty_string) < 0) {
            ty_err(n->u.node_concat.a, "(^): operand must be string, got ");
            ty_print(stderr, ta); fputc('\n', stderr);
        }
        if (ty_unify(tb, ty_string) < 0) {
            ty_err(n->u.node_concat.b, "(^): operand must be string, got ");
            ty_print(stderr, tb); fputc('\n', stderr);
        }
        return ty_string;
    }

    // List operations.
    if (ty_node_is(n, "node_cons")) {
        struct ty *th = infer(n->u.node_cons.hd, env, level);
        struct ty *tt = infer(n->u.node_cons.tl, env, level);
        struct ty *list_t = ty_list(th);
        if (ty_unify(tt, list_t) < 0) {
            ty_err(n->u.node_cons.tl, "(::): tail must be list of head's type");
        }
        return list_t;
    }
    if (ty_node_is(n, "node_cons_head")) {
        struct ty *t = infer(n->u.node_cons_head.e, env, level);
        struct ty *elem = ty_new_var(level);
        if (ty_unify(t, ty_list(elem)) < 0) ty_err(n, "expected list");
        return elem;
    }
    if (ty_node_is(n, "node_cons_tail")) {
        struct ty *t = infer(n->u.node_cons_tail.e, env, level);
        struct ty *elem = ty_new_var(level);
        struct ty *lt = ty_list(elem);
        if (ty_unify(t, lt) < 0) ty_err(n, "expected list");
        return lt;
    }
    if (ty_node_is(n, "node_is_cons") || ty_node_is(n, "node_is_nil")) {
        infer(n->u.node_is_cons.e, env, level);
        return ty_bool;
    }

    // References.
    if (ty_node_is(n, "node_ref")) {
        return ty_ref_of(infer(n->u.node_ref.e, env, level));
    }
    if (ty_node_is(n, "node_deref")) {
        struct ty *t = infer(n->u.node_deref.e, env, level);
        struct ty *elem = ty_new_var(level);
        if (ty_unify(t, ty_ref_of(elem)) < 0) ty_err(n, "(!): operand must be ref");
        return elem;
    }
    if (ty_node_is(n, "node_assign")) {
        struct ty *tl = infer(n->u.node_assign.lhs, env, level);
        struct ty *tr = infer(n->u.node_assign.rhs, env, level);
        struct ty *elem = ty_new_var(level);
        if (ty_unify(tl, ty_ref_of(elem)) < 0) ty_err(n, "(:=): lhs must be ref");
        if (ty_unify(elem, tr) < 0) ty_err(n, "(:=): assigned value type mismatch");
        return ty_unit;
    }

    // Control flow.
    if (ty_node_is(n, "node_if")) {
        struct ty *tc = infer(n->u.node_if.cond, env, level);
        bool cond_ok = (ty_unify(tc, ty_bool) >= 0);
        if (!cond_ok) {
            ty_err(n->u.node_if.cond, "if: condition must be bool, got ");
            ty_print(stderr, tc); fputc('\n', stderr);
        }
        if (cond_ok) oc_node_to_if_bool(n);
        struct ty *tt = infer(n->u.node_if.thn, env, level);
        struct ty *te = infer(n->u.node_if.els, env, level);
        if (ty_unify(tt, te) < 0) {
            ty_err(n, "if: then/else types differ: ");
            ty_print(stderr, tt); fputs(" vs ", stderr); ty_print(stderr, te); fputc('\n', stderr);
        }
        return tt;
    }
    if (ty_node_is(n, "node_seq")) {
        infer(n->u.node_seq.first, env, level);
        return infer(n->u.node_seq.rest, env, level);
    }
    if (ty_node_is(n, "node_let")) {
        struct ty *tv = infer(n->u.node_let.value, env, level + 1);
        struct ty_scheme *s = ty_generalize(tv, level);
        struct ty_env *e2 = ty_env_push_schemes(env, 1, &s);
        return infer(n->u.node_let.body, e2, level);
    }
    if (ty_node_is(n, "node_letrec")) {
        struct ty *self = ty_new_var(level + 1);
        struct ty *types[1] = { self };
        struct ty_env *e1 = ty_env_push(env, 1, types);
        struct ty *tv = infer(n->u.node_letrec.value, e1, level + 1);
        if (ty_unify(self, tv) < 0) ty_err(n, "let rec: value type mismatch");
        struct ty_scheme *s = ty_generalize(tv, level);
        struct ty_env *e2 = ty_env_push_schemes(env, 1, &s);
        return infer(n->u.node_letrec.body, e2, level);
    }
    if (ty_node_is(n, "node_letrec_n")) {
        uint32_t nb = n->u.node_letrec_n.nbindings;
        uint32_t vidx = n->u.node_letrec_n.values_idx;
        extern NODE **OC_LETREC_VALUES;
        struct ty **types = (struct ty **)malloc(sizeof(struct ty *) * (nb ? nb : 1));
        for (uint32_t i = 0; i < nb; i++) types[i] = ty_new_var(level + 1);
        struct ty_env *e2 = ty_env_push(env, (int)nb, types);
        for (uint32_t i = 0; i < nb; i++) {
            struct ty *tv = infer(OC_LETREC_VALUES[vidx + i], e2, level + 1);
            if (ty_unify(types[i], tv) < 0) ty_err(n, "let rec ... and: value type mismatch");
        }
        struct ty *tb = infer(n->u.node_letrec_n.body, e2, level);
        free(types);
        return tb;
    }
    if (ty_node_is(n, "node_let_pat")) {
        infer(n->u.node_let_pat.value, env, level);
        uint32_t ar = n->u.node_let_pat.arity;
        struct ty **types = (struct ty **)malloc(sizeof(struct ty *) * (ar ? ar : 1));
        for (uint32_t i = 0; i < ar; i++) types[i] = ty_new_var(level);
        struct ty_env *e2 = ty_env_push(env, (int)ar, types);
        struct ty *tb = infer(n->u.node_let_pat.body, e2, level);
        free(types);
        return tb;
    }
    if (ty_node_is(n, "node_match_arm")) {
        struct ty *tt = infer(n->u.node_match_arm.test, env, level);
        if (ty_unify(tt, ty_bool) < 0) ty_err(n, "match arm: test must be bool");
        uint32_t ar = n->u.node_match_arm.arity;
        struct ty **types = (struct ty **)malloc(sizeof(struct ty *) * (ar ? ar : 1));
        for (uint32_t i = 0; i < ar; i++) types[i] = ty_new_var(level);
        struct ty_env *e2 = ty_env_push(env, (int)ar, types);
        struct ty *tb = infer(n->u.node_match_arm.body, e2, level);
        struct ty *tf = infer(n->u.node_match_arm.failure, env, level);
        if (ty_unify(tb, tf) < 0) {
            ty_err(n, "match arm: arm and fall-through types differ: ");
            ty_print(stderr, tb); fputs(" vs ", stderr); ty_print(stderr, tf); fputc('\n', stderr);
        }
        free(types);
        return tb;
    }
    if (ty_node_is(n, "node_fun")) {
        // Build a curried fun type with fresh vars for each param.
        int np = n->u.node_fun.nparams;
        struct ty **params = (struct ty **)malloc(sizeof(struct ty *) * (np ? np : 1));
        for (int i = 0; i < np; i++) params[i] = ty_new_var(level + 1);
        struct ty_env *e2 = ty_env_push(env, np, params);
        struct ty *tb = infer(n->u.node_fun.body, e2, level);
        // Build chained fun type: t1 -> t2 -> ... -> tb (right-assoc)
        struct ty *result = tb;
        for (int i = np - 1; i >= 0; i--) result = ty_fun(params[i], result);
        free(params);
        return result;
    }

    // Function application.
    const char *app_kinds[][2] = {
        {"node_app0", NULL}, {"node_app1", NULL}, {"node_app2", NULL},
        {"node_app3", NULL}, {"node_app4", NULL},
        {"node_tail_app0", NULL}, {"node_tail_app1", NULL}, {"node_tail_app2", NULL},
        {"node_tail_app3", NULL}, {"node_tail_app4", NULL},
    };
    for (int i = 0; i < (int)(sizeof app_kinds / sizeof app_kinds[0]); i++) {
        if (ty_node_is(n, app_kinds[i][0])) {
            // Args are at u.node_appK.fn / .a0 / .a1 / ... with the same
            // layout for the tail variant.  Use union punning via
            // node_app4 since it's the largest.
            struct ty *tf = infer(n->u.node_app4.fn, env, level);
            int argc = (i % 5);
            NODE *args[4] = {0};
            if (argc >= 1) args[0] = n->u.node_app4.a0;
            if (argc >= 2) args[1] = n->u.node_app4.a1;
            if (argc >= 3) args[2] = n->u.node_app4.a2;
            if (argc >= 4) args[3] = n->u.node_app4.a3;
            // Apply args one at a time.
            struct ty *cur = tf;
            for (int j = 0; j < argc; j++) {
                struct ty *ta = infer(args[j], env, level);
                struct ty *tr = ty_new_var(level);
                if (ty_unify(cur, ty_fun(ta, tr)) < 0) {
                    ty_err(n, "applying non-function value (got ");
                    ty_print(stderr, cur); fputs(")\n", stderr);
                    return ty_any;
                }
                cur = ty_walk(tr);
            }
            return cur;
        }
    }
    if (ty_node_is(n, "node_appn")) {
        // Variable arity — too complex; trust runtime.
        return ty_any;
    }

    // Tuples.
    if (ty_node_is(n, "node_tuple_n")) {
        uint32_t idx = n->u.node_tuple_n.args_idx;
        uint32_t argc = n->u.node_tuple_n.argc;
        struct ty **items = (struct ty **)malloc(sizeof(struct ty *) * (argc ? argc : 1));
        for (uint32_t i = 0; i < argc; i++) {
            items[i] = infer(OC_CALL_ARGS[idx + i], env, level);
        }
        struct ty *r = ty_tuple((int)argc, items);
        free(items);
        return r;
    }
    if (ty_node_is(n, "node_tuple_get")) {
        struct ty *t = infer(n->u.node_tuple_get.e, env, level);
        // We don't know the arity from a get; if it's a tuple, return the slot type.
        struct ty *tw = ty_walk(t);
        if (tw->kind == TY_TUPLE) {
            uint32_t i = n->u.node_tuple_get.idx;
            if (i < (uint32_t)tw->tup_n) return tw->tup_items[i];
        }
        return ty_any;
    }
    if (ty_node_is(n, "node_is_tuple")) {
        infer(n->u.node_is_tuple.e, env, level);
        return ty_bool;
    }

    // Try / raise.
    if (ty_node_is(n, "node_try")) {
        struct ty *tb = infer(n->u.node_try.body, env, level);
        struct ty *th = infer(n->u.node_try.handler, env, level);
        if (ty_unify(tb, th) < 0) ty_err(n, "try: body and handler types differ");
        return tb;
    }
    if (ty_node_is(n, "node_raise")) {
        infer(n->u.node_raise.e, env, level);
        return ty_new_var(level);   // raise has any type (never returns)
    }

    // Variants / records / objects / lazy / send / field_assign — give up.
    return ty_any;
}

// Top-level type-check entry.  Reports errors but doesn't abort.
static void
type_check_top(NODE *n)
{
    if (!OPTION.type_check) return;
    int saved = ty_error_count;
    infer(n, NULL, 0);
    if (ty_error_count > saved) {
        // Errors already printed.
    }
}

// Same as type_check_top, but also registers the inferred type as the
// global named `name` (called from parse_program for top-level lets).
static void
type_check_top_define(NODE *n, const char *name)
{
    if (!OPTION.type_check) { return; }
    int saved = ty_error_count;
    struct ty *t = infer(n, NULL, 0);
    if (ty_error_count > saved) return;
    if (name && strcmp(name, "_") != 0) {
        gtype_define(name, ty_generalize(t, -1));   // -1 → all free vars are quantified
    }
}

// ---------------------------------------------------------------------------
// Lexer.
// ---------------------------------------------------------------------------

enum tok {
    TK_EOF,
    TK_INT, TK_FLOAT_TOK, TK_IDENT, TK_STRING, TK_CHAR,
    TK_LET, TK_REC, TK_IN, TK_IF, TK_THEN, TK_ELSE, TK_FUN, TK_FUNCTION,
    TK_MATCH, TK_WITH, TK_TRY, TK_TRUE, TK_FALSE, TK_MOD, TK_NOT,
    TK_BEGIN, TK_END, TK_KW_AND, TK_OR_KW, TK_WHEN, TK_AS, TK_REF_KW,
    TK_TYPE, TK_OF, TK_EXCEPTION_KW, TK_OPEN, TK_MODULE, TK_INCLUDE,
    TK_SIG, TK_STRUCT, TK_DO, TK_DONE, TK_FOR, TK_TO, TK_DOWNTO,
    TK_WHILE, TK_LSL, TK_LSR, TK_ASR, TK_LAND_KW, TK_LOR_KW, TK_LXOR_KW,
    TK_LAZY, TK_CLASS, TK_OBJECT, TK_METHOD, TK_VAL, TK_INHERIT,
    TK_PRIVATE, TK_MUTABLE, TK_NEW, TK_INITIALIZER, TK_FUNCTOR,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH,
    TK_FPLUS, TK_FMINUS, TK_FSTAR, TK_FSLASH,
    TK_LT, TK_GT, TK_LE, TK_GE, TK_EQ, TK_NE, TK_PEQ, TK_PNE,
    TK_ARROW, TK_BAR, TK_SEMI, TK_DSEMI, TK_CONS,
    TK_AMPAMP, TK_PIPEPIPE, TK_CONCAT, TK_UNDER, TK_COMMA,
    TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK, TK_LBRACE, TK_RBRACE,
    TK_LBRACKBAR, TK_BARRBRACK,
    TK_DOT, TK_DOTDOT, TK_BANG, TK_ASSIGN, TK_TILDE, TK_QMARK,
    TK_COLON, TK_HASH,
};

static const char *src;
static int   src_pos;
static int   src_line;

static int      tok;
static char     tok_str[1024];
static int64_t  tok_int;
static double   tok_dbl;

static bool is_op_char(char c);

__attribute__((noreturn,format(printf,1,2)))
static void
parse_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "astocaml: parse error at line %d: ", src_line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void
skip_ws_and_comments(void)
{
    for (;;) {
        while (isspace((unsigned char)src[src_pos])) {
            if (src[src_pos] == '\n') src_line++;
            src_pos++;
        }
        if (src[src_pos] == '(' && src[src_pos+1] == '*') {
            src_pos += 2;
            int depth = 1;
            while (depth > 0 && src[src_pos]) {
                if (src[src_pos] == '(' && src[src_pos+1] == '*') { depth++; src_pos += 2; }
                else if (src[src_pos] == '*' && src[src_pos+1] == ')') { depth--; src_pos += 2; }
                else { if (src[src_pos] == '\n') src_line++; src_pos++; }
            }
            continue;
        }
        break;
    }
}

static void
next_token(void)
{
    skip_ws_and_comments();
    char ch = src[src_pos];
    if (ch == '\0') { tok = TK_EOF; return; }

    // String literal.
    if (ch == '"') {
        src_pos++;
        int idx = 0;
        while (src[src_pos] && src[src_pos] != '"') {
            char c = src[src_pos++];
            if (c == '\\') {
                char e = src[src_pos++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                case '\'': c = '\''; break;
                case '0': c = '\0'; break;
                default: c = e; break;
                }
            }
            if (idx < (int)sizeof(tok_str) - 1) tok_str[idx++] = c;
        }
        tok_str[idx] = '\0';
        if (src[src_pos] == '"') src_pos++;
        tok = TK_STRING;
        return;
    }

    // Polymorphic variant tag: `\`Foo`, `\`bar`.  We emit TK_IDENT with
    // the backtick prefixed in-place so the parser can detect via
    // is_uppercase_ident().
    if (ch == '`') {
        src_pos++;
        int idx = 0;
        tok_str[idx++] = '`';
        while (isalnum((unsigned char)src[src_pos]) || src[src_pos] == '_' || src[src_pos] == '\'') {
            if (idx < (int)sizeof(tok_str) - 1) tok_str[idx++] = src[src_pos];
            src_pos++;
        }
        tok_str[idx] = '\0';
        tok = TK_IDENT;
        return;
    }

    // Type variable: `'a`, `'b`, etc. (only inside type declarations,
    // which we skip).  Detected as `'` followed by a lowercase letter and
    // NOT a char-literal pattern.  Yield a TK_IDENT carrying the bare
    // name without the apostrophe.
    if (ch == '\'' && isalpha((unsigned char)src[src_pos+1]) && islower((unsigned char)src[src_pos+1])) {
        // Check it isn't a char literal: char literal has closing `'` at
        // pos+2 (or pos+3 for escapes).  Type vars don't.
        bool is_char_lit = (src[src_pos+2] == '\'') ||
                           (src[src_pos+1] == '\\' && src[src_pos+3] == '\'');
        if (!is_char_lit) {
            src_pos++;       // consume `'`
            int idx = 0;
            while (isalnum((unsigned char)src[src_pos]) || src[src_pos] == '_' || src[src_pos] == '\'') {
                if (idx < (int)sizeof(tok_str) - 1) tok_str[idx++] = src[src_pos];
                src_pos++;
            }
            tok_str[idx] = '\0';
            tok = TK_IDENT;
            return;
        }
    }

    // Char literal: 'a' or '\n' or '\\' etc.  In OCaml chars are int8.
    if (ch == '\'' && src[src_pos+1] && src[src_pos+1] != '\'' &&
        ((src[src_pos+1] == '\\' && src[src_pos+3] == '\'') ||
         src[src_pos+2] == '\'')) {
        src_pos++;
        char c;
        if (src[src_pos] == '\\') {
            src_pos++;
            switch (src[src_pos++]) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '"': c = '"'; break;
            case '0': c = '\0'; break;
            default: c = src[src_pos-1]; break;
            }
        }
        else {
            c = src[src_pos++];
        }
        if (src[src_pos] == '\'') src_pos++;
        tok_int = (unsigned char)c;
        tok = TK_CHAR;
        return;
    }

    // Float literal starting with `.` (e.g. `.5`).  Distinct from `..`
    // range and from `.field` access.
    if (ch == '.' && isdigit((unsigned char)src[src_pos+1])) {
        int start = src_pos;
        src_pos++;
        while (isdigit((unsigned char)src[src_pos])) src_pos++;
        if (src[src_pos] == 'e' || src[src_pos] == 'E') {
            src_pos++;
            if (src[src_pos] == '+' || src[src_pos] == '-') src_pos++;
            while (isdigit((unsigned char)src[src_pos])) src_pos++;
        }
        char buf[64];
        int len = src_pos - start;
        if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, src + start, len);
        buf[len] = '\0';
        tok_dbl = strtod(buf, NULL);
        tok = TK_FLOAT_TOK;
        return;
    }

    // Numeric literal — int or float.
    if (isdigit((unsigned char)ch)) {
        int start = src_pos;
        while (isdigit((unsigned char)src[src_pos])) src_pos++;
        bool is_float = false;
        if (src[src_pos] == '.' && src[src_pos+1] != '.' /* not range op */) {
            is_float = true;
            src_pos++;
            while (isdigit((unsigned char)src[src_pos])) src_pos++;
        }
        if (src[src_pos] == 'e' || src[src_pos] == 'E') {
            is_float = true;
            src_pos++;
            if (src[src_pos] == '+' || src[src_pos] == '-') src_pos++;
            while (isdigit((unsigned char)src[src_pos])) src_pos++;
        }
        char buf[64];
        int len = src_pos - start;
        if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, src + start, len);
        buf[len] = '\0';
        if (is_float) {
            tok_dbl = strtod(buf, NULL);
            tok = TK_FLOAT_TOK;
        }
        else {
            tok_int = strtoll(buf, NULL, 10);
            tok = TK_INT;
        }
        return;
    }

    // Identifier / keyword.
    if (isalpha((unsigned char)ch) || ch == '_') {
        int idx = 0;
        while (isalnum((unsigned char)src[src_pos]) || src[src_pos] == '_' || src[src_pos] == '\'') {
            if (idx < (int)sizeof(tok_str) - 1) tok_str[idx++] = src[src_pos];
            src_pos++;
        }
        tok_str[idx] = '\0';
        if (idx == 1 && tok_str[0] == '_') { tok = TK_UNDER; return; }
        struct kw { const char *s; int t; };
        static const struct kw kws[] = {
            {"let", TK_LET},   {"rec", TK_REC},     {"in", TK_IN},
            {"if", TK_IF},     {"then", TK_THEN},   {"else", TK_ELSE},
            {"fun", TK_FUN},   {"function", TK_FUNCTION},
            {"match", TK_MATCH}, {"with", TK_WITH}, {"try", TK_TRY},
            {"true", TK_TRUE}, {"false", TK_FALSE},
            {"mod", TK_MOD},   {"not", TK_NOT},
            {"begin", TK_BEGIN}, {"end", TK_END},
            {"and", TK_KW_AND},
            {"when", TK_WHEN}, {"as", TK_AS},
            {"type", TK_TYPE}, {"of", TK_OF},
            {"exception", TK_EXCEPTION_KW},
            {"open", TK_OPEN}, {"module", TK_MODULE}, {"include", TK_INCLUDE},
            {"sig", TK_SIG},   {"struct", TK_STRUCT},
            {"do", TK_DO},     {"done", TK_DONE},
            {"for", TK_FOR},   {"to", TK_TO},       {"downto", TK_DOWNTO},
            {"while", TK_WHILE},
            {"lsl", TK_LSL},   {"lsr", TK_LSR},     {"asr", TK_ASR},
            {"land", TK_LAND_KW}, {"lor", TK_LOR_KW}, {"lxor", TK_LXOR_KW},
            {"or", TK_OR_KW},
            {"lazy", TK_LAZY},
            {"class", TK_CLASS}, {"object", TK_OBJECT}, {"method", TK_METHOD},
            {"val", TK_VAL}, {"inherit", TK_INHERIT}, {"private", TK_PRIVATE},
            {"mutable", TK_MUTABLE}, {"new", TK_NEW}, {"initializer", TK_INITIALIZER},
            {"functor", TK_FUNCTOR},
            {NULL, 0}
        };
        for (const struct kw *k = kws; k->s; k++) {
            if (!strcmp(tok_str, k->s)) { tok = k->t; return; }
        }
        tok = TK_IDENT;
        return;
    }

    // Operator-character sequences.  Read all consecutive op chars and
    // map known sequences to their dedicated tokens; anything else is a
    // custom infix operator emitted as TK_IDENT.
    if (is_op_char(ch)) {
        char obuf[64]; int idx = 0;
        // ch is at src[src_pos]; consume it and any continuation.
        while (is_op_char(src[src_pos]) && idx < 63) obuf[idx++] = src[src_pos++];
        obuf[idx] = '\0';
        // Special: `[]` was tokenized via the `[` case below; here we only
        // see `[` as standalone bracket (handled separately).
        // Map well-known operator strings to their tokens.
        if      (!strcmp(obuf, "+"))   { tok = TK_PLUS;     return; }
        else if (!strcmp(obuf, "-"))   { tok = TK_MINUS;    return; }
        else if (!strcmp(obuf, "*"))   { tok = TK_STAR;     return; }
        else if (!strcmp(obuf, "/"))   { tok = TK_SLASH;    return; }
        else if (!strcmp(obuf, "+."))  { tok = TK_FPLUS;    return; }
        else if (!strcmp(obuf, "-."))  { tok = TK_FMINUS;   return; }
        else if (!strcmp(obuf, "*."))  { tok = TK_FSTAR;    return; }
        else if (!strcmp(obuf, "/."))  { tok = TK_FSLASH;   return; }
        else if (!strcmp(obuf, "<"))   { tok = TK_LT;       return; }
        else if (!strcmp(obuf, ">"))   { tok = TK_GT;       return; }
        else if (!strcmp(obuf, "<="))  { tok = TK_LE;       return; }
        else if (!strcmp(obuf, ">="))  { tok = TK_GE;       return; }
        else if (!strcmp(obuf, "="))   { tok = TK_EQ;       return; }
        else if (!strcmp(obuf, "<>"))  { tok = TK_NE;       return; }
        else if (!strcmp(obuf, "=="))  { tok = TK_PEQ;      return; }
        else if (!strcmp(obuf, "!="))  { tok = TK_PNE;      return; }
        else if (!strcmp(obuf, "->"))  { tok = TK_ARROW;    return; }
        else if (!strcmp(obuf, "|") && src[src_pos] == ']') { src_pos++; tok = TK_BARRBRACK; return; }
        else if (!strcmp(obuf, "|"))   { tok = TK_BAR;      return; }
        else if (!strcmp(obuf, "||"))  { tok = TK_PIPEPIPE; return; }
        else if (!strcmp(obuf, "&&"))  { tok = TK_AMPAMP;   return; }
        else if (!strcmp(obuf, "::"))  { tok = TK_CONS;     return; }
        else if (!strcmp(obuf, ":="))  { tok = TK_ASSIGN;   return; }
        else if (!strcmp(obuf, "<-"))  { tok = TK_ASSIGN;   return; }
        else if (!strcmp(obuf, "!"))   { tok = TK_BANG;     return; }
        else if (!strcmp(obuf, "~"))   { tok = TK_TILDE;    return; }
        else if (!strcmp(obuf, "?"))   { tok = TK_QMARK;    return; }
        else if (!strcmp(obuf, ":"))   { tok = TK_COLON;    return; }
        else if (!strcmp(obuf, "."))   { tok = TK_DOT;      return; }
        else if (!strcmp(obuf, ".."))  { tok = TK_DOTDOT;   return; }
        else if (!strcmp(obuf, "^"))   { tok = TK_CONCAT;   return; }
        else if (!strcmp(obuf, "@"))   { tok = TK_CONCAT;   return; }   // simplified: '@' aliased to '^'
        // Custom operator — emit as identifier.
        strcpy(tok_str, obuf);
        tok = TK_IDENT;
        return;
    }

    src_pos++;
    switch (ch) {
    case ',': tok = TK_COMMA; return;
    case '(': tok = TK_LPAREN; return;
    case ')': tok = TK_RPAREN; return;
    case '{': tok = TK_LBRACE; return;
    case '}': tok = TK_RBRACE; return;
    case '[':
        if (src[src_pos] == ']') { src_pos++; tok_str[0] = '\0'; tok = TK_LBRACK; tok_int = 1; return; }
        if (src[src_pos] == '|') { src_pos++; tok = TK_LBRACKBAR; return; }
        tok = TK_LBRACK; tok_int = 0; return;
    case ']': tok = TK_RBRACK; return;
    case ';':
        if (src[src_pos] == ';') { src_pos++; tok = TK_DSEMI; return; }
        tok = TK_SEMI; return;
    case '#': tok = TK_HASH; return;
    case '&':
        // Stand-alone `&` (without other op chars following) — fall through
        // to old behavior: only valid as `&&` (which is_op_char captured).
        parse_fail("expected '&&'");
    }
    parse_fail("unexpected character '%c' (0x%02x)", ch, (unsigned char)ch);
}

static void
expect(int t, const char *what)
{
    if (tok != t) parse_fail("expected %s (got tok=%d)", what, tok);
    next_token();
}

// ---------------------------------------------------------------------------
// Parser scopes.
// ---------------------------------------------------------------------------

#define MAX_SCOPE_SLOTS 64
struct scope {
    struct scope *parent;
    int nslots;
    const char *names[MAX_SCOPE_SLOTS];
};

static struct scope *cur_scope;

// Class context for bare field access inside method bodies.  When set,
// `resolve_var` returns `self#<name>` (a method-style send that falls
// back to field read in `oc_object_send`) for any name that matches
// one of these fields.
static const char **cur_class_fields = NULL;
static int          cur_class_fields_n = 0;
static const char **cur_class_methods = NULL;
static int          cur_class_methods_n = 0;

// Tail-position tracking — currently unused (tail-call rewriting is
// done in a post-pass `mark_tail_calls` instead of a parser flag).
// Kept for future use.
__attribute__((unused)) static bool cur_tail = false;

// Functor registry — saved (param-name, body-source-range) for each
// `module F = functor (X : S) -> struct ... end` (or sugar form
// `module F (X : S) = struct ... end`).  Instantiation later replays
// the body with the parameter aliased to the actual module argument.
struct functor_entry {
    char name[128];
    char param[128];
    int body_start_pos, body_start_line;
    int body_end_pos;
};
static struct functor_entry g_functors[64];
static int g_functors_n = 0;

static struct functor_entry *
functor_lookup(const char *name)
{
    for (int i = 0; i < g_functors_n; i++) {
        if (strcmp(g_functors[i].name, name) == 0) return &g_functors[i];
    }
    return NULL;
}

// Current module path, e.g. `"M"` / `"M.N"` / `""` (top level).  When set,
// top-level `let X = ...` defines a global `<prefix>.X` and `let rec`
// pre-binds `<prefix>.X`.  Nested modules append with a dot.
static char cur_module_prefix[256] = "";

static const char *
make_global_name(const char *name)
{
    if (cur_module_prefix[0] == '\0') return strdup(name);
    char buf[512];
    snprintf(buf, sizeof buf, "%s.%s", cur_module_prefix, name);
    return strdup(buf);
}

// Build a gref node aware of the current module prefix.  When inside a
// module, references to `name` may resolve as either `<prefix>.<name>`
// (a sibling defined within the same module) or as `name` (a top-level
// name from outside).  We emit a node_gref_q that tries the qualified
// form first, falling back to the bare name.
static NODE *
make_gref(const char *name)
{
    if (cur_module_prefix[0] == '\0') return ALLOC_node_gref(strdup(name));
    char buf[512];
    snprintf(buf, sizeof buf, "%s.%s", cur_module_prefix, name);
    return ALLOC_node_gref_q(strdup(buf), strdup(name));
}

static struct scope *
push_scope_n(int nslots, const char **names)
{
    struct scope *s = (struct scope *)malloc(sizeof(struct scope));
    s->parent = cur_scope;
    s->nslots = nslots;
    for (int i = 0; i < nslots; i++) s->names[i] = strdup(names[i]);
    cur_scope = s;
    return s;
}

static void
pop_scope(void)
{
    struct scope *s = cur_scope;
    cur_scope = s->parent;
    free(s);
}

static bool
is_uppercase_ident(const char *s)
{
    return (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '`';
}

// True iff name looks like a user-defined infix operator (starts with
// an op char and isn't one of our reserved tokens like `+` / `<` etc.
// — those wouldn't reach this code path because they're tokenized as
// dedicated tokens, not TK_IDENT).
static bool
is_custom_op_starting_with(const char *name, const char *firsts)
{
    if (!name || !name[0]) return false;
    return strchr(firsts, name[0]) != NULL;
}

// OCaml operator characters.  When a sequence of these appears between
// `(` and `)` (with no whitespace inside), it forms a custom infix-op
// identifier — `(+!)`, `(<*>)`, `(:=)` etc.
static bool
is_op_char(char c)
{
    return c && strchr("!$%&*+-/.:<=>?@^|~", c) != NULL;
}

// Map an operator token to its OCaml identifier name (for `(+)`, `(-)` etc.).
static const char *
tok_op_name(int t)
{
    switch (t) {
    case TK_PLUS:    return "+";
    case TK_MINUS:   return "-";
    case TK_STAR:    return "*";
    case TK_SLASH:   return "/";
    case TK_FPLUS:   return "+.";
    case TK_FMINUS:  return "-.";
    case TK_FSTAR:   return "*.";
    case TK_FSLASH:  return "/.";
    case TK_LT:      return "<";
    case TK_GT:      return ">";
    case TK_LE:      return "<=";
    case TK_GE:      return ">=";
    case TK_EQ:      return "=";
    case TK_NE:      return "<>";
    case TK_PEQ:     return "==";
    case TK_AMPAMP:  return "&&";
    case TK_PIPEPIPE:return "||";
    case TK_CONCAT:  return "^";
    case TK_CONS:    return "::";
    case TK_ASSIGN:  return ":=";
    case TK_BANG:    return "!";
    }
    return NULL;
}

static NODE *
resolve_var(const char *name)
{
    int depth = 0;
    for (struct scope *s = cur_scope; s; s = s->parent) {
        for (int i = 0; i < s->nslots; i++) {
            if (strcmp(s->names[i], name) == 0)
                return ALLOC_node_lref((uint32_t)depth, (uint32_t)i);
        }
        depth++;
    }
    // Inside a method body, bare field/method names resolve as `self#name`
    // (object_send falls back to field read for 0-arg lookups).
    if (cur_class_fields) {
        for (int i = 0; i < cur_class_fields_n; i++) {
            if (strcmp(cur_class_fields[i], name) == 0) {
                NODE *self_node = NULL;
                int d = 0;
                for (struct scope *s = cur_scope; s; s = s->parent) {
                    for (int j = 0; j < s->nslots; j++) {
                        if (strcmp(s->names[j], "self") == 0) {
                            self_node = ALLOC_node_lref((uint32_t)d, (uint32_t)j);
                            goto self_found;
                        }
                    }
                    d++;
                }
self_found:
                if (self_node) {
                    return ALLOC_node_send(self_node, strdup(name), 0, 0);
                }
            }
        }
    }
    if (cur_class_methods) {
        for (int i = 0; i < cur_class_methods_n; i++) {
            if (strcmp(cur_class_methods[i], name) == 0) {
                // Bare method reference inside class body — resolve as a
                // closure that sends to self.  Most uses are actually
                // method calls (`name args`), where parse_app will
                // package them; for now we just treat the bare name as
                // a 0-arg send (returns the field-or-method value).
                NODE *self_node = NULL;
                int d = 0;
                for (struct scope *s = cur_scope; s; s = s->parent) {
                    for (int j = 0; j < s->nslots; j++) {
                        if (strcmp(s->names[j], "self") == 0) {
                            self_node = ALLOC_node_lref((uint32_t)d, (uint32_t)j);
                            goto m_self_found;
                        }
                    }
                    d++;
                }
m_self_found:
                if (self_node) {
                    return ALLOC_node_send(self_node, strdup(name), 0, 0);
                }
            }
        }
    }
    return make_gref(name);
}

// ---------------------------------------------------------------------------
// Global node-arg tables (used by node_appn / node_tuple_n / node_letrec_n
// / node_match_arm / etc).
// ---------------------------------------------------------------------------

NODE **OC_CALL_ARGS = NULL;
static size_t oc_call_args_size = 0;
static size_t oc_call_args_capa = 0;

NODE **OC_LETREC_VALUES = NULL;
static size_t oc_letrec_values_size = 0;
static size_t oc_letrec_values_capa = 0;

NODE **OC_EXTRACT_NODES = NULL;
static size_t oc_extract_nodes_size = 0;
static size_t oc_extract_nodes_capa = 0;

const char **OC_RECORD_FIELDS = NULL;
static size_t oc_record_fields_size = 0;
static size_t oc_record_fields_capa = 0;

static uint32_t
stash_call_args(NODE **args, int n)
{
    if (oc_call_args_size + n > oc_call_args_capa) {
        size_t cap = oc_call_args_capa ? oc_call_args_capa * 2 : 64;
        while (cap < oc_call_args_size + n) cap *= 2;
        OC_CALL_ARGS = (NODE **)realloc(OC_CALL_ARGS, cap * sizeof(NODE *));
        oc_call_args_capa = cap;
    }
    uint32_t idx = (uint32_t)oc_call_args_size;
    for (int i = 0; i < n; i++) OC_CALL_ARGS[idx + i] = args[i];
    oc_call_args_size += n;
    return idx;
}

static uint32_t
stash_letrec_values(NODE **vs, int n)
{
    if (oc_letrec_values_size + n > oc_letrec_values_capa) {
        size_t cap = oc_letrec_values_capa ? oc_letrec_values_capa * 2 : 32;
        while (cap < oc_letrec_values_size + n) cap *= 2;
        OC_LETREC_VALUES = (NODE **)realloc(OC_LETREC_VALUES, cap * sizeof(NODE *));
        oc_letrec_values_capa = cap;
    }
    uint32_t idx = (uint32_t)oc_letrec_values_size;
    for (int i = 0; i < n; i++) OC_LETREC_VALUES[idx + i] = vs[i];
    oc_letrec_values_size += n;
    return idx;
}

static uint32_t
stash_extract_nodes(NODE **es, int n)
{
    if (oc_extract_nodes_size + n > oc_extract_nodes_capa) {
        size_t cap = oc_extract_nodes_capa ? oc_extract_nodes_capa * 2 : 32;
        while (cap < oc_extract_nodes_size + n) cap *= 2;
        OC_EXTRACT_NODES = (NODE **)realloc(OC_EXTRACT_NODES, cap * sizeof(NODE *));
        oc_extract_nodes_capa = cap;
    }
    uint32_t idx = (uint32_t)oc_extract_nodes_size;
    for (int i = 0; i < n; i++) OC_EXTRACT_NODES[idx + i] = es[i];
    oc_extract_nodes_size += n;
    return idx;
}

static uint32_t
stash_record_fields(const char **fs, int n)
{
    if (oc_record_fields_size + n > oc_record_fields_capa) {
        size_t cap = oc_record_fields_capa ? oc_record_fields_capa * 2 : 32;
        while (cap < oc_record_fields_size + n) cap *= 2;
        OC_RECORD_FIELDS = (const char **)realloc(OC_RECORD_FIELDS, cap * sizeof(char *));
        oc_record_fields_capa = cap;
    }
    uint32_t idx = (uint32_t)oc_record_fields_size;
    for (int i = 0; i < n; i++) OC_RECORD_FIELDS[idx + i] = fs[i];
    oc_record_fields_size += n;
    return idx;
}

// ---------------------------------------------------------------------------
// Patterns (recursive struct).
// ---------------------------------------------------------------------------

enum pat_kind {
    PAT_WILD, PAT_VAR, PAT_INT, PAT_BOOL, PAT_UNIT, PAT_NIL, PAT_STRING,
    PAT_CONS, PAT_TUPLE, PAT_VARIANT, PAT_RECORD, PAT_AS, PAT_OR, PAT_RANGE,
};

struct pat {
    enum pat_kind kind;
    union {
        const char *var_name;
        int32_t int_val;
        bool bool_val;
        const char *str_val;
        struct { struct pat *head; struct pat *tail; } cons;
        struct { int n; struct pat **items; } tup;
        struct {
            const char *name;
            struct pat *arg;            // nullable
        } variant;
        struct {
            int n;
            const char **fields;
            struct pat **items;
        } rec;
        struct { struct pat *p; const char *name; } as_;
        struct { int n; struct pat **alts; } or_;
        struct { int32_t lo, hi; } range;
    };
    NODE *guard;        // optional `when` expression, only set on outer arm
};

static struct pat *
pat_new(enum pat_kind k)
{
    struct pat *p = (struct pat *)calloc(1, sizeof *p);
    p->kind = k;
    return p;
}

// Walk pattern collecting variable names in pre-order.
static void
pat_collect_vars(struct pat *p, const char **out, int *idx)
{
    switch (p->kind) {
    case PAT_VAR: out[(*idx)++] = p->var_name; break;
    case PAT_AS:
        pat_collect_vars(p->as_.p, out, idx);
        out[(*idx)++] = p->as_.name;
        break;
    case PAT_CONS:
        pat_collect_vars(p->cons.head, out, idx);
        pat_collect_vars(p->cons.tail, out, idx);
        break;
    case PAT_TUPLE:
        for (int i = 0; i < p->tup.n; i++) pat_collect_vars(p->tup.items[i], out, idx);
        break;
    case PAT_VARIANT:
        if (p->variant.arg) pat_collect_vars(p->variant.arg, out, idx);
        break;
    case PAT_RECORD:
        for (int i = 0; i < p->rec.n; i++) pat_collect_vars(p->rec.items[i], out, idx);
        break;
    case PAT_OR:
        // For OR-patterns we require all branches to bind the same vars
        // — collect from the first branch only.
        if (p->or_.n > 0) pat_collect_vars(p->or_.alts[0], out, idx);
        break;
    default: break;
    }
}

// Generate a NODE expressing the scrut access; reusable since each call
// allocates a fresh tree (no sharing).  `scrut_lref(d, s)` returns
// `lref(d, s)` — used as the base; sub-accessors append nodes.
static NODE *
scrut_lref(int depth, int slot)
{
    return ALLOC_node_lref((uint32_t)depth, (uint32_t)slot);
}

// gen_test: emit a boolean expression that is TRUE iff scrut matches `p`
// (ignoring `p`'s guard, which is handled at the arm level).  scrut_fac
// is a function that produces a fresh access expression each call.
typedef NODE *(*scrut_fac_fn)(void *ctx);

struct scrut_lref_ctx { int depth, slot; };
static NODE *scrut_fac_lref(void *ctx) {
    struct scrut_lref_ctx *c = ctx;
    return ALLOC_node_lref((uint32_t)c->depth, (uint32_t)c->slot);
}

struct scrut_proj_ctx {
    scrut_fac_fn parent_fac;
    void *parent_ctx;
    enum { PROJ_HEAD, PROJ_TAIL, PROJ_TUPLE_GET, PROJ_VAR_GET, PROJ_REC_GET } kind;
    int idx;
    const char *field;
};
static NODE *scrut_fac_proj(void *ctx) {
    struct scrut_proj_ctx *c = ctx;
    NODE *p = c->parent_fac(c->parent_ctx);
    switch (c->kind) {
    case PROJ_HEAD:      return ALLOC_node_cons_head(p);
    case PROJ_TAIL:      return ALLOC_node_cons_tail(p);
    case PROJ_TUPLE_GET: return ALLOC_node_tuple_get(p, (uint32_t)c->idx);
    case PROJ_VAR_GET:   return ALLOC_node_variant_get(p, (uint32_t)c->idx);
    case PROJ_REC_GET:   return ALLOC_node_record_get(p, c->field);
    }
    return p;
}

static NODE *pat_gen_test(struct pat *p, scrut_fac_fn fac, void *ctx);
static void  pat_gen_extracts(struct pat *p, scrut_fac_fn fac, void *ctx, NODE **out, int *idx);

static NODE *
pat_gen_test(struct pat *p, scrut_fac_fn fac, void *ctx)
{
    switch (p->kind) {
    case PAT_WILD:
    case PAT_VAR:
    case PAT_UNIT:
        return ALLOC_node_const_bool(1);
    case PAT_INT:
        return ALLOC_node_eq(fac(ctx), ALLOC_node_const_int(p->int_val));
    case PAT_RANGE: {
        NODE *test_lo = ALLOC_node_le(ALLOC_node_const_int(p->range.lo), fac(ctx));
        NODE *test_hi = ALLOC_node_le(fac(ctx), ALLOC_node_const_int(p->range.hi));
        return ALLOC_node_and(test_lo, test_hi);
    }
    case PAT_STRING:
        return ALLOC_node_eq(fac(ctx), ALLOC_node_const_str(strdup(p->str_val)));
    case PAT_BOOL:
        return p->bool_val ? fac(ctx) : ALLOC_node_not(fac(ctx));
    case PAT_NIL:
        return ALLOC_node_is_nil(fac(ctx));
    case PAT_AS:
        return pat_gen_test(p->as_.p, fac, ctx);
    case PAT_CONS: {
        NODE *test_cons = ALLOC_node_is_cons(fac(ctx));
        struct scrut_proj_ctx hctx = {.parent_fac = fac, .parent_ctx = ctx, .kind = PROJ_HEAD};
        struct scrut_proj_ctx *hctx_p = malloc(sizeof hctx); *hctx_p = hctx;
        struct scrut_proj_ctx tctx = {.parent_fac = fac, .parent_ctx = ctx, .kind = PROJ_TAIL};
        struct scrut_proj_ctx *tctx_p = malloc(sizeof tctx); *tctx_p = tctx;
        NODE *head_test = pat_gen_test(p->cons.head, scrut_fac_proj, hctx_p);
        NODE *tail_test = pat_gen_test(p->cons.tail, scrut_fac_proj, tctx_p);
        return ALLOC_node_and(test_cons, ALLOC_node_and(head_test, tail_test));
    }
    case PAT_TUPLE: {
        NODE *test = ALLOC_node_is_tuple(fac(ctx), (uint32_t)p->tup.n);
        for (int i = 0; i < p->tup.n; i++) {
            struct scrut_proj_ctx *pc = malloc(sizeof *pc);
            pc->parent_fac = fac; pc->parent_ctx = ctx; pc->kind = PROJ_TUPLE_GET; pc->idx = i;
            test = ALLOC_node_and(test, pat_gen_test(p->tup.items[i], scrut_fac_proj, pc));
        }
        return test;
    }
    case PAT_VARIANT: {
        NODE *test = ALLOC_node_is_variant(fac(ctx), strdup(p->variant.name));
        if (p->variant.arg) {
            struct scrut_proj_ctx *pc = malloc(sizeof *pc);
            pc->parent_fac = fac; pc->parent_ctx = ctx; pc->kind = PROJ_VAR_GET; pc->idx = 0;
            NODE *sub = pat_gen_test(p->variant.arg, scrut_fac_proj, pc);
            test = ALLOC_node_and(test, sub);
        }
        return test;
    }
    case PAT_RECORD: {
        NODE *test = ALLOC_node_const_bool(1);
        for (int i = 0; i < p->rec.n; i++) {
            struct scrut_proj_ctx *pc = malloc(sizeof *pc);
            pc->parent_fac = fac; pc->parent_ctx = ctx;
            pc->kind = PROJ_REC_GET; pc->field = p->rec.fields[i];
            NODE *sub = pat_gen_test(p->rec.items[i], scrut_fac_proj, pc);
            test = ALLOC_node_and(test, sub);
        }
        return test;
    }
    case PAT_OR: {
        // Test is the OR of all branch tests.  Variable bindings come
        // from the matched branch (we use the first branch's structure
        // for extraction — caller must ensure branches bind same vars
        // in same order; or-patterns with bindings are tricky but we
        // settle for this).
        NODE *test = ALLOC_node_const_bool(0);
        for (int i = 0; i < p->or_.n; i++) {
            test = ALLOC_node_or(test, pat_gen_test(p->or_.alts[i], fac, ctx));
        }
        return test;
    }
    }
    return ALLOC_node_const_bool(0);
}

static void
pat_gen_extracts(struct pat *p, scrut_fac_fn fac, void *ctx, NODE **out, int *idx)
{
    switch (p->kind) {
    case PAT_VAR: out[(*idx)++] = fac(ctx); break;
    case PAT_AS:
        pat_gen_extracts(p->as_.p, fac, ctx, out, idx);
        out[(*idx)++] = fac(ctx);
        break;
    case PAT_CONS: {
        struct scrut_proj_ctx *hctx = malloc(sizeof *hctx);
        hctx->parent_fac = fac; hctx->parent_ctx = ctx; hctx->kind = PROJ_HEAD;
        pat_gen_extracts(p->cons.head, scrut_fac_proj, hctx, out, idx);
        struct scrut_proj_ctx *tctx = malloc(sizeof *tctx);
        tctx->parent_fac = fac; tctx->parent_ctx = ctx; tctx->kind = PROJ_TAIL;
        pat_gen_extracts(p->cons.tail, scrut_fac_proj, tctx, out, idx);
        break;
    }
    case PAT_TUPLE:
        for (int i = 0; i < p->tup.n; i++) {
            struct scrut_proj_ctx *pc = malloc(sizeof *pc);
            pc->parent_fac = fac; pc->parent_ctx = ctx; pc->kind = PROJ_TUPLE_GET; pc->idx = i;
            pat_gen_extracts(p->tup.items[i], scrut_fac_proj, pc, out, idx);
        }
        break;
    case PAT_VARIANT:
        if (p->variant.arg) {
            struct scrut_proj_ctx *pc = malloc(sizeof *pc);
            pc->parent_fac = fac; pc->parent_ctx = ctx; pc->kind = PROJ_VAR_GET; pc->idx = 0;
            pat_gen_extracts(p->variant.arg, scrut_fac_proj, pc, out, idx);
        }
        break;
    case PAT_RECORD:
        for (int i = 0; i < p->rec.n; i++) {
            struct scrut_proj_ctx *pc = malloc(sizeof *pc);
            pc->parent_fac = fac; pc->parent_ctx = ctx;
            pc->kind = PROJ_REC_GET; pc->field = p->rec.fields[i];
            pat_gen_extracts(p->rec.items[i], scrut_fac_proj, pc, out, idx);
        }
        break;
    case PAT_OR:
        if (p->or_.n > 0) pat_gen_extracts(p->or_.alts[0], fac, ctx, out, idx);
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Parser forward declarations.
// ---------------------------------------------------------------------------

static NODE *parse_expr(void);
static NODE *parse_expr_no_seq(void);
static NODE *parse_tuple_expr(void);
static NODE *parse_or_expr(void);
static NODE *parse_and_expr(void);
static NODE *parse_cmp(void);
static NODE *parse_concat(void);
static NODE *parse_cons(void);
static NODE *parse_assign(void);
static NODE *parse_add(void);
static NODE *parse_mul(void);
static NODE *parse_unary(void);
static NODE *parse_app(void);
static NODE *parse_atom(void);
static NODE *parse_atom_basic(void);
static int   atom_starts(int t);
static struct pat *parse_pattern(void);
static struct pat *parse_pattern_no_or(void);
static struct pat *parse_pattern_atom(void);
static void  mark_tail_calls(NODE *n);
static uint32_t node_is_leaf(NODE *body);

// AOT entry registry — every closure body and every top-level form gets
// pushed here.  Under `--compile`, after parsing we hand the entire list
// to `astro_cs_compile` (one SD_<hash>.c each), build all.so once, load,
// then patch every entry's dispatcher.  This is the only way closure
// bodies become AOT-compiled — `SPECIALIZE_node_fun` is empty (the body
// must not inline into its parent SD), so without this registry the AST
// inside `let f x = ...` is never specialized.
NODE **AOT_ENTRIES = NULL;
size_t AOT_ENTRIES_LEN = 0;
static size_t AOT_ENTRIES_CAP = 0;

static void
aot_add_entry(NODE *n)
{
    if (!n) return;
    if (AOT_ENTRIES_LEN == AOT_ENTRIES_CAP) {
        AOT_ENTRIES_CAP = AOT_ENTRIES_CAP ? AOT_ENTRIES_CAP * 2 : 64;
        AOT_ENTRIES = (NODE **)realloc(AOT_ENTRIES, sizeof(NODE *) * AOT_ENTRIES_CAP);
    }
    AOT_ENTRIES[AOT_ENTRIES_LEN++] = n;
}

// Wrap ALLOC_node_fun: register the body for AOT and compute is_leaf.
static inline NODE *
make_fun(uint32_t nparams, NODE *body)
{
    aot_add_entry(body);
    return ALLOC_node_fun(nparams, body, node_is_leaf(body));
}

// Same for `@noinline` nodes whose ASTroGen-generated SPECIALIZE is
// empty: their NODE * children would never become AOT-compiled
// otherwise.  Pattern arms in particular show up *everywhere* in
// list-processing OCaml code, so without these the AOT pass leaves
// most of nqueens / sieve unspecialized.
static inline NODE *
make_match_arm(NODE *test, uint32_t arity, uint32_t exi, NODE *body, NODE *failure)
{
    aot_add_entry(body);
    aot_add_entry(failure);
    return ALLOC_node_match_arm(test, arity, exi, body, failure, node_is_leaf(body));
}

static inline NODE *
make_let_pat(NODE *value, uint32_t arity, uint32_t exi, NODE *body)
{
    aot_add_entry(body);
    return ALLOC_node_let_pat(value, arity, exi, body);
}

static inline NODE *
make_letrec_n(uint32_t nb, uint32_t vidx, NODE *body)
{
    aot_add_entry(body);
    return ALLOC_node_letrec_n(nb, vidx, body);
}

static inline NODE *
make_try(NODE *body, NODE *handler)
{
    aot_add_entry(body);
    aot_add_entry(handler);
    return ALLOC_node_try(body, handler);
}

// ---------------------------------------------------------------------------
// Patterns parser.
// ---------------------------------------------------------------------------

static struct pat *
parse_pattern_atom(void)
{
    if (tok == TK_UNDER)        { next_token(); return pat_new(PAT_WILD); }
    if (tok == TK_INT) {
        int32_t v = (int32_t)tok_int; next_token();
        if (tok == TK_DOTDOT) {
            next_token();
            int32_t hi;
            if (tok == TK_INT) { hi = (int32_t)tok_int; next_token(); }
            else if (tok == TK_CHAR) { hi = (int32_t)tok_int; next_token(); }
            else parse_fail("expected int/char after '..' in pattern");
            struct pat *p = pat_new(PAT_RANGE);
            p->range.lo = v; p->range.hi = hi; return p;
        }
        struct pat *p = pat_new(PAT_INT); p->int_val = v; return p;
    }
    if (tok == TK_MINUS) {
        next_token();
        if (tok != TK_INT) parse_fail("expected int after '-' in pattern");
        struct pat *p = pat_new(PAT_INT); p->int_val = (int32_t)-tok_int; next_token(); return p;
    }
    if (tok == TK_TRUE)         { next_token(); struct pat *p = pat_new(PAT_BOOL); p->bool_val = true;  return p; }
    if (tok == TK_FALSE)        { next_token(); struct pat *p = pat_new(PAT_BOOL); p->bool_val = false; return p; }
    if (tok == TK_STRING)       { struct pat *p = pat_new(PAT_STRING); p->str_val = strdup(tok_str); next_token(); return p; }
    if (tok == TK_CHAR) {
        int32_t v = (int32_t)tok_int; next_token();
        if (tok == TK_DOTDOT) {
            next_token();
            int32_t hi;
            if (tok == TK_CHAR) { hi = (int32_t)tok_int; next_token(); }
            else if (tok == TK_INT) { hi = (int32_t)tok_int; next_token(); }
            else parse_fail("expected char/int after '..' in pattern");
            struct pat *p = pat_new(PAT_RANGE);
            p->range.lo = v; p->range.hi = hi; return p;
        }
        struct pat *p = pat_new(PAT_INT); p->int_val = v; return p;
    }
    if (tok == TK_LBRACK) {
        if (tok_int) { next_token(); return pat_new(PAT_NIL); }
        // List pattern `[p1; p2; p3]` desugars to p1 :: p2 :: p3 :: [].
        next_token();
        struct pat *items[256]; int n = 0;
        if (tok != TK_RBRACK) {
            items[n++] = parse_pattern();
            while (tok == TK_SEMI) { next_token(); if (tok == TK_RBRACK) break; items[n++] = parse_pattern(); }
        }
        expect(TK_RBRACK, "']'");
        struct pat *tail = pat_new(PAT_NIL);
        for (int i = n - 1; i >= 0; i--) {
            struct pat *c = pat_new(PAT_CONS);
            c->cons.head = items[i];
            c->cons.tail = tail;
            tail = c;
        }
        return tail;
    }
    if (tok == TK_LPAREN) {
        next_token();
        if (tok == TK_RPAREN) { next_token(); return pat_new(PAT_UNIT); }
        struct pat *first = parse_pattern();
        if (tok == TK_COMMA) {
            // Tuple pattern.
            struct pat *items[64]; int n = 0;
            items[n++] = first;
            while (tok == TK_COMMA) {
                next_token();
                items[n++] = parse_pattern();
            }
            expect(TK_RPAREN, "')'");
            struct pat *p = pat_new(PAT_TUPLE);
            p->tup.n = n;
            p->tup.items = (struct pat **)malloc(sizeof(struct pat *) * n);
            for (int i = 0; i < n; i++) p->tup.items[i] = items[i];
            return p;
        }
        expect(TK_RPAREN, "')'");
        return first;
    }
    if (tok == TK_LBRACE) {
        // Record pattern: { f1 = p1; f2 = p2 }
        next_token();
        const char *fields[64]; struct pat *items[64]; int n = 0;
        while (tok != TK_RBRACE) {
            if (tok != TK_IDENT) parse_fail("expected field name in record pattern");
            fields[n] = strdup(tok_str);
            next_token();
            if (tok == TK_EQ) {
                next_token();
                items[n] = parse_pattern();
            }
            else {
                // Punning: `{ f }` is `{ f = f }`.
                struct pat *vp = pat_new(PAT_VAR);
                vp->var_name = fields[n];
                items[n] = vp;
            }
            n++;
            if (tok == TK_SEMI) next_token();
            else break;
        }
        expect(TK_RBRACE, "'}'");
        struct pat *p = pat_new(PAT_RECORD);
        p->rec.n = n;
        p->rec.fields = malloc(sizeof(char *) * n);
        p->rec.items = malloc(sizeof(struct pat *) * n);
        for (int i = 0; i < n; i++) { p->rec.fields[i] = fields[i]; p->rec.items[i] = items[i]; }
        return p;
    }
    if (tok == TK_IDENT) {
        const char *name = strdup(tok_str);
        next_token();
        if (is_uppercase_ident(name)) {
            // Constructor pattern.  Optional sub-pattern (atom-level).
            struct pat *p = pat_new(PAT_VARIANT);
            p->variant.name = name;
            p->variant.arg = NULL;
            if (tok == TK_UNDER || tok == TK_IDENT || tok == TK_INT || tok == TK_TRUE ||
                tok == TK_FALSE || tok == TK_STRING || tok == TK_CHAR ||
                tok == TK_LPAREN || tok == TK_LBRACK || tok == TK_LBRACE) {
                p->variant.arg = parse_pattern_atom();
            }
            return p;
        }
        struct pat *p = pat_new(PAT_VAR); p->var_name = name; return p;
    }
    parse_fail("expected pattern (got tok=%d)", tok);
}

// `::` is right-associative.
static struct pat *
parse_pattern_cons(void)
{
    struct pat *l = parse_pattern_atom();
    if (tok == TK_CONS) {
        next_token();
        struct pat *r = parse_pattern_cons();
        struct pat *p = pat_new(PAT_CONS);
        p->cons.head = l;
        p->cons.tail = r;
        return p;
    }
    return l;
}

static struct pat *
parse_pattern_as(void)
{
    struct pat *p = parse_pattern_cons();
    while (tok == TK_AS) {
        next_token();
        if (tok != TK_IDENT) parse_fail("expected name after 'as'");
        const char *name = strdup(tok_str); next_token();
        struct pat *as_p = pat_new(PAT_AS);
        as_p->as_.p = p;
        as_p->as_.name = name;
        p = as_p;
    }
    return p;
}

static struct pat *
parse_pattern_no_or(void)
{
    return parse_pattern_as();
}

static struct pat *
parse_pattern(void)
{
    return parse_pattern_no_or();
}

// `parse_arm_pattern` consumes `|`-separated patterns into a single
// or-pattern.  Used in match-arm position (and try-with).  The arm
// separator `|` AFTER `->` is consumed by the outer match loop, not
// here, so this is unambiguous.
static struct pat *
parse_arm_pattern(void)
{
    struct pat *p = parse_pattern_no_or();
    if (tok != TK_BAR) return p;
    struct pat *alts[64];
    int n = 0;
    alts[n++] = p;
    while (tok == TK_BAR) {
        next_token();
        alts[n++] = parse_pattern_no_or();
        if (n >= 64) parse_fail("too many or-pattern alternatives");
    }
    struct pat *o = pat_new(PAT_OR);
    o->or_.n = n;
    o->or_.alts = malloc(sizeof(struct pat *) * n);
    for (int i = 0; i < n; i++) o->or_.alts[i] = alts[i];
    return o;
}

// ---------------------------------------------------------------------------
// Match desugaring.
// ---------------------------------------------------------------------------

// One match arm.
struct match_arm {
    struct pat *pat;
    NODE *guard;        // may be NULL
    NODE *body;
    int n_vars;
};

static NODE *
build_match_arms(struct match_arm *arms, int n_arms)
{
    // Failure of last arm is a runtime match-failure.
    NODE *fail = ALLOC_node_raise(ALLOC_node_variant_0(strdup("Match_failure")));
    for (int i = n_arms - 1; i >= 0; i--) {
        struct match_arm *a = &arms[i];
        struct scrut_lref_ctx *ctx = malloc(sizeof *ctx);
        ctx->depth = 0; ctx->slot = 0;
        NODE *test = pat_gen_test(a->pat, scrut_fac_lref, ctx);
        NODE *body = a->body;
        // Apply guard if present.
        if (a->guard) {
            // The guard is parsed in a scope where pattern vars are
            // bound.  At runtime, guard is evaluated AFTER the arm's
            // frame has been pushed (so vars are visible).
            // Hmm — but node_match_arm evaluates extracts in OUTER env,
            // then pushes frame, then evaluates body.  Guard needs to
            // see the pushed frame.  Solution: wrap body with `if guard
            // then body else <fall-through>`; but fall-through requires
            // not running this arm, which we can't do once we've pushed.
            // Instead: include guard in the test.  But test is evaluated
            // BEFORE the frame push, so vars aren't bound yet.
            // Workaround: the guard test needs the SAME extracts.  We
            // emit guard as: bind pattern vars to a temp let, evaluate
            // guard in that env, but THIS adds depth shifts.
            //
            // Practical solution: handle guarded arm differently —
            // wrap the arm's body in `if guard then real_body else
            // <next arm>`.  The pushed frame stays even though we
            // fall through; that's fine functionally (a small leak)
            // but breaks correctness because the next arm's match is
            // evaluated in pushed-frame env, not the original.  To
            // avoid that, we'd need a way to "abort the push".
            //
            // For simplicity and correctness: don't push frame for
            // guarded arms via node_match_arm; instead emit explicit
            // tests + lets + guard check.
            //
            // Implement via a chain of let-bindings (each pushes 1
            // frame), then `if guard then body else fail`.  This
            // restructures the arm to bypass node_match_arm.

            // Build chained lets for each pattern variable.
            const char *names[MAX_SCOPE_SLOTS]; int nv = 0;
            pat_collect_vars(a->pat, names, &nv);

            // Extracts in pre-order, each at depth 0 of the original
            // bind-frame (we'll re-bind scrut to slot 0 first).
            NODE *extracts[MAX_SCOPE_SLOTS];
            int ei = 0;
            struct scrut_lref_ctx *bctx = malloc(sizeof *bctx);
            bctx->depth = 0; bctx->slot = 0;
            pat_gen_extracts(a->pat, scrut_fac_lref, bctx, extracts, &ei);

            // Build inner: chain of node_let, each pushes 1 slot.
            // The body / guard sees vars at depths reflecting the chain.
            // But the parser bound names as a SINGLE scope (push_scope_n
            // called once with all names).  So body's lref(0, 0..N-1)
            // refers to the single scope.
            //
            // To match: instead of chained lets (which would be N frames),
            // use node_let_pat with arity=N — pushes a single N-slot
            // frame.  Then wrap with `if guard then body else fail`.
            // The if/guard/body all run inside the pushed frame, where
            // vars are at depth 0 slot k.

            uint32_t exi = stash_extract_nodes(extracts, ei);
            NODE *guarded_body = ALLOC_node_if(a->guard, body, fail);
            // node_let_pat: value = scrut_lref(depth=0, slot=0); arity = ei; extracts; body.
            // But node_let_pat itself adds a 1-slot frame for scrut.  We
            // want to skip that since scrut is already in the outer let.
            // Reuse via node_match_arm: its test arg makes the arm
            // conditional, but here we want unconditional bind.  Use
            // node_match_arm with test=true for unconditional path.
            NODE *always = ALLOC_node_const_bool(1);
            NODE *armnode = make_match_arm(always, (uint32_t)ei, exi, guarded_body, fail);
            // Then wrap with the pattern test.
            NODE *armtest = ALLOC_node_if(test, armnode, fail);
            fail = armtest;
            continue;
        }
        // Non-guarded arm: use node_match_arm (test, arity, extract_idx, body, failure).
        const char *names[MAX_SCOPE_SLOTS]; int nv = 0;
        pat_collect_vars(a->pat, names, &nv);
        NODE *extracts[MAX_SCOPE_SLOTS];
        int ei = 0;
        struct scrut_lref_ctx *bctx = malloc(sizeof *bctx);
        bctx->depth = 0; bctx->slot = 0;
        pat_gen_extracts(a->pat, scrut_fac_lref, bctx, extracts, &ei);
        uint32_t exi = stash_extract_nodes(extracts, ei);
        fail = make_match_arm(test, (uint32_t)ei, exi, body, fail);
    }
    return fail;
}

// `match e with ...` — caller has already consumed `match`.  Wraps the
// chain in `let __m = e in ...` so all arms see scrut at lref(0, 0).
static NODE *
parse_match_after_kw(void)
{
    NODE *scrut = parse_expr_no_seq();
    expect(TK_WITH, "'with'");
    if (tok == TK_BAR) next_token();

    enum { MAX_ARMS = 128 };
    struct match_arm arms[MAX_ARMS];
    struct match_arm exn_arms[MAX_ARMS];
    int n = 0;
    int en = 0;

    const char *m_name[1] = { "__m" };
    push_scope_n(1, m_name);

    while (n + en < MAX_ARMS) {
        // `| exception E -> handler` — match arm that catches a raised exn.
        bool is_exn = false;
        if (tok == TK_EXCEPTION_KW) {
            next_token();
            is_exn = true;
        }
        struct pat *p = parse_arm_pattern();
        const char *names[MAX_SCOPE_SLOTS]; int nv = 0;
        pat_collect_vars(p, names, &nv);

        NODE *guard = NULL;
        if (tok == TK_WHEN) {
            next_token();
            if (nv > 0) push_scope_n(nv, names);
            guard = parse_expr_no_seq();
            if (nv > 0) pop_scope();
        }

        expect(TK_ARROW, "'->'");

        if (nv > 0) push_scope_n(nv, names);
        NODE *body = parse_expr();
        if (nv > 0) pop_scope();

        struct match_arm *target = is_exn ? &exn_arms[en++] : &arms[n++];
        target->pat = p;
        target->guard = guard;
        target->body = body;
        target->n_vars = nv;
        if (tok != TK_BAR) break;
        next_token();
    }

    NODE *body = build_match_arms(arms, n);
    pop_scope();
    NODE *let_match = ALLOC_node_let(scrut, body);
    if (en > 0) {
        // `match e with | exception E -> h | p -> b` desugars to
        //
        //     try (let __m = e in match __m with normal-arms)
        //     with E -> h
        //
        // The try MUST cover the scrut evaluation as well (so that
        // exceptions raised by `e` can be caught).
        const char *exn_m[1] = { "__exn" };
        push_scope_n(1, exn_m);
        NODE *fail = ALLOC_node_raise(ALLOC_node_lref(0, 0));
        for (int i = en - 1; i >= 0; i--) {
            struct match_arm *a = &exn_arms[i];
            struct scrut_lref_ctx *ctx = malloc(sizeof *ctx);
            ctx->depth = 0; ctx->slot = 0;
            NODE *test = pat_gen_test(a->pat, scrut_fac_lref, ctx);
            NODE *bd = a->body;
            if (a->guard) bd = ALLOC_node_if(a->guard, bd, fail);
            NODE *extracts[MAX_SCOPE_SLOTS]; int ei = 0;
            struct scrut_lref_ctx *bctx = malloc(sizeof *bctx);
            bctx->depth = 0; bctx->slot = 0;
            pat_gen_extracts(a->pat, scrut_fac_lref, bctx, extracts, &ei);
            uint32_t exi = stash_extract_nodes(extracts, ei);
            if (a->guard) {
                NODE *armnode = make_match_arm(ALLOC_node_const_bool(1), (uint32_t)ei, exi, bd, fail);
                fail = ALLOC_node_if(test, armnode, fail);
            }
            else {
                fail = make_match_arm(test, (uint32_t)ei, exi, bd, fail);
            }
        }
        pop_scope();
        let_match = make_try(let_match, fail);
    }
    return let_match;
}

// `function | p -> e | ...` desugars to `fun __x -> match __x with ...`.
static NODE *
parse_function_after_kw(void)
{
    if (tok == TK_BAR) next_token();

    enum { MAX_ARMS = 128 };
    struct match_arm arms[MAX_ARMS];
    int n = 0;

    // Push a "fake" scope for __m so arms parse correctly.
    const char *m_name[1] = { "__m" };
    push_scope_n(1, m_name);

    while (n < MAX_ARMS) {
        struct pat *p = parse_arm_pattern();
        const char *names[MAX_SCOPE_SLOTS]; int nv = 0;
        pat_collect_vars(p, names, &nv);

        NODE *guard = NULL;
        if (tok == TK_WHEN) {
            next_token();
            if (nv > 0) push_scope_n(nv, names);
            guard = parse_expr_no_seq();
            if (nv > 0) pop_scope();
        }

        expect(TK_ARROW, "'->'");

        if (nv > 0) push_scope_n(nv, names);
        NODE *body = parse_expr();
        if (nv > 0) pop_scope();

        arms[n].pat = p;
        arms[n].guard = guard;
        arms[n].body = body;
        arms[n].n_vars = nv;
        n++;
        if (tok != TK_BAR) break;
        next_token();
    }

    NODE *match_body = build_match_arms(arms, n);
    pop_scope();
    // Wrap in `fun __x -> let __m = __x in match_body`.  Since __m and __x
    // are basically the same, simplify: the fun body is `let __m = lref(0,0) in match_body`.
    // Actually we want `fun __m -> match_body` directly.
    // Re-build with a proper scope: pop __m, push the fun's __m as slot 0.
    const char *fname[1] = { "__m" };
    push_scope_n(1, fname);
    pop_scope();
    NODE *fun_body = ALLOC_node_let(ALLOC_node_lref(0, 0), match_body);
    return (mark_tail_calls(fun_body), make_fun(1, fun_body));
}

// ---------------------------------------------------------------------------
// Expression parser.
// ---------------------------------------------------------------------------

static NODE *
make_app(NODE *fn, NODE **args, int argc)
{
    // Always emit non-tail variants here; a post-parse walker
    // (mark_tail_calls) flips the dispatcher to the tail variant for
    // calls in tail position.
    switch (argc) {
    case 0: return ALLOC_node_app0(fn);
    case 1: return ALLOC_node_app1(fn, args[0]);
    case 2: return ALLOC_node_app2(fn, args[0], args[1]);
    case 3: return ALLOC_node_app3(fn, args[0], args[1], args[2]);
    case 4: return ALLOC_node_app4(fn, args[0], args[1], args[2], args[3]);
    default: {
        uint32_t idx = stash_call_args(args, argc);
        return ALLOC_node_appn(fn, idx, (uint32_t)argc);
    }
    }
}

// Tail-call post-pass.  Visit every node in tail position; if it's an
// `app_K` and a `tail_app_K` exists with the same operand layout, swap
// the dispatcher in-place via helpers in node.c (which has access to
// the static DISPATCH_* function pointers).
extern bool oc_node_is_app(NODE *n);
extern void oc_node_to_tail(NODE *n);

static void
mark_tail_calls(NODE *n)
{
    if (!n) return;
    if (oc_node_is_app(n)) { oc_node_to_tail(n); return; }
    const char *dn = n->head.kind->default_dispatcher_name;
    if (strcmp(dn, "DISPATCH_node_if") == 0) {
        mark_tail_calls(n->u.node_if.thn);
        mark_tail_calls(n->u.node_if.els);
        return;
    }
    if (strcmp(dn, "DISPATCH_node_seq") == 0) {
        mark_tail_calls(n->u.node_seq.rest);
        return;
    }
    if (strcmp(dn, "DISPATCH_node_let") == 0) {
        mark_tail_calls(n->u.node_let.body);
        return;
    }
    if (strcmp(dn, "DISPATCH_node_letrec") == 0) {
        mark_tail_calls(n->u.node_letrec.body);
        return;
    }
    if (strcmp(dn, "DISPATCH_node_letrec_n") == 0) {
        mark_tail_calls(n->u.node_letrec_n.body);
        return;
    }
    if (strcmp(dn, "DISPATCH_node_let_pat") == 0) {
        mark_tail_calls(n->u.node_let_pat.body);
        return;
    }
    if (strcmp(dn, "DISPATCH_node_match_arm") == 0) {
        mark_tail_calls(n->u.node_match_arm.body);
        mark_tail_calls(n->u.node_match_arm.failure);
        return;
    }
    if (strcmp(dn, "DISPATCH_node_try") == 0) {
        // body is NOT tail (need to catch raises); handler IS tail.
        mark_tail_calls(n->u.node_try.handler);
        return;
    }
}

// Closure-leaf detection.  A closure is a "leaf" if its body never
// constructs another closure (`node_fun`) or thunk (`node_lazy`).  For
// such closures, oc_apply may alloca the activation frame instead of
// malloc-ing it, since nothing the body produces can outlive the call
// — a frame escape requires a closure capture.
//
// Implementation: dump the body to a memstream and scan for the
// disqualifying dispatcher names.  Conservative: any literal substring
// that looks like a closure marker also marks non-leaf (rare; the
// emitted form is `(node_fun ` / `(node_lazy ` with leading paren).
//
// Cost is paid once at parse time per `node_fun`, so the simplicity
// matters more than micro-efficiency here.
static uint32_t
node_is_leaf(NODE *body)
{
    if (!body) return 1;
    char *buf = NULL;
    size_t sz = 0;
    FILE *fp = open_memstream(&buf, &sz);
    if (!fp) return 0;
    DUMP(fp, body, /*oneline=*/true);
    fclose(fp);
    bool leaf = (strstr(buf, "(node_fun ") == NULL &&
                 strstr(buf, "(node_lazy ") == NULL);
    free(buf);
    return leaf ? 1u : 0u;
}


// `fun x y z -> body` — collect params, build a single multi-arity closure.
static NODE *
parse_fun_after_kw(void)
{
    const char *params[MAX_SCOPE_SLOTS];
    int n = 0;
    while (tok == TK_IDENT || tok == TK_UNDER || tok == TK_LPAREN ||
           tok == TK_TILDE || tok == TK_QMARK) {
        if (tok == TK_LPAREN) {
            // `()` unit parameter or `(x : ty)` annotated parameter.
            int peek_pos = src_pos, peek_line = src_line, peek_tok = tok;
            char peek_str[1024]; strcpy(peek_str, tok_str);
            next_token();
            if (tok == TK_RPAREN) {
                next_token();
                if (n >= MAX_SCOPE_SLOTS) parse_fail("too many parameters");
                params[n++] = "_";
                continue;
            }
            if (tok == TK_IDENT) {
                const char *pn = strdup(tok_str);
                next_token();
                if (tok == TK_COLON) {
                    // Skip type annotation up to matching `)`.
                    next_token();
                    int d = 0;
                    while (tok != TK_EOF) {
                        if (tok == TK_LPAREN) d++;
                        else if (tok == TK_RPAREN) {
                            if (d == 0) break;
                            d--;
                        }
                        next_token();
                    }
                    if (tok == TK_RPAREN) next_token();
                    if (n >= MAX_SCOPE_SLOTS) parse_fail("too many parameters");
                    params[n++] = pn;
                    continue;
                }
            }
            // Restore.
            src_pos = peek_pos; src_line = peek_line; tok = peek_tok;
            strcpy(tok_str, peek_str);
            break;
        }
        if (tok == TK_TILDE || tok == TK_QMARK) {
            // `~x` or `?x` — labeled / optional, stripped to positional.
            next_token();
            if (tok != TK_IDENT) parse_fail("expected identifier after '~' or '?'");
            if (n >= MAX_SCOPE_SLOTS) parse_fail("too many parameters");
            params[n++] = strdup(tok_str);
            next_token();
            // Optional default `~x:default` or `~(x : ty)` — simplified
            // skip.  We don't handle defaults in this pass.
            if (tok == TK_COLON) {
                // Could be `~x:value` (default) or type annotation; skip
                // until we hit something that ends the param.
                next_token();
                // Skip an atom (likely a default) — best effort.
                if (atom_starts(tok)) parse_atom();
            }
            continue;
        }
        if (n >= MAX_SCOPE_SLOTS) parse_fail("too many parameters");
        params[n++] = (tok == TK_UNDER) ? "_" : strdup(tok_str);
        next_token();
    }
    if (n == 0) parse_fail("'fun' requires at least one parameter");
    expect(TK_ARROW, "'->'");
    push_scope_n(n, params);
    NODE *body = parse_expr();
    pop_scope();
    return (mark_tail_calls(body), make_fun((uint32_t)n, body));
}

// Parse `let X (params) = ... [in ...]`.  When `in_required` is true and
// we hit something other than `in`, error.  When false, we may return
// early to let the caller treat it as a top-level binding.
struct let_binding {
    const char *name;       // NULL if pattern destructure
    struct pat *pat;        // NULL if simple name
    int np;
    const char *params[MAX_SCOPE_SLOTS];
    NODE *value;
};

// Save/restore lexer state for two-pass parsing of mutual `let rec`.
struct lex_state {
    int src_pos, src_line;
    int tok;
    char tok_str[1024];
    int64_t tok_int;
    double tok_dbl;
};

static void lex_save(struct lex_state *s) {
    s->src_pos = src_pos; s->src_line = src_line; s->tok = tok;
    memcpy(s->tok_str, tok_str, sizeof tok_str);
    s->tok_int = tok_int; s->tok_dbl = tok_dbl;
}

static void lex_restore(struct lex_state *s) {
    src_pos = s->src_pos; src_line = s->src_line; tok = s->tok;
    memcpy(tok_str, s->tok_str, sizeof tok_str);
    tok_int = s->tok_int; tok_dbl = s->tok_dbl;
}

// Walk forward past a `let ... = <value>` body without parsing it
// semantically, so we can come back later and parse it once all sibling
// `and` binding names are in scope.  Stops at top-level `and` / `in` /
// `;;` / EOF.  Tracks nesting through (), [], {}, begin/end and nested
// let/in pairs.
static void
skip_let_value(void)
{
    int paren = 0, bracket = 0, brace = 0;
    int begin_d = 0, let_d = 0;
    while (tok != TK_EOF) {
        if (paren == 0 && bracket == 0 && brace == 0 && begin_d == 0 && let_d == 0) {
            if (tok == TK_KW_AND || tok == TK_IN || tok == TK_DSEMI) return;
        }
        switch (tok) {
        case TK_LPAREN:    paren++; break;
        case TK_RPAREN:    if (paren > 0) paren--; break;
        case TK_LBRACK:    if (tok_int == 0) bracket++; break;     // tok_int==1 means `[]` (already balanced)
        case TK_LBRACKBAR: bracket++; break;
        case TK_RBRACK:    if (bracket > 0) bracket--; break;
        case TK_BARRBRACK: if (bracket > 0) bracket--; break;
        case TK_LBRACE:    brace++; break;
        case TK_RBRACE:    if (brace > 0) brace--; break;
        case TK_BEGIN:     begin_d++; break;
        case TK_END:       if (begin_d > 0) begin_d--; break;
        case TK_LET:       let_d++; break;
        case TK_IN:        if (let_d > 0) let_d--; break;
        }
        next_token();
    }
}

static struct let_binding *
parse_let_binding(void)
{
    struct let_binding *lb = (struct let_binding *)calloc(1, sizeof *lb);
    if (tok == TK_LBRACE) {
        // Pattern binding: `let { f1 = a; ... } = ...`.
        lb->pat = parse_pattern_no_or();
        lb->np = 0;
    }
    else if (tok == TK_LPAREN) {
        // `let (op) [params] = ...` — operator-as-identifier (single- or
        // multi-char).  Scan the raw source for op chars + `)`; on
        // failure fall through to pattern binding.
        int p = src_pos;
        while (isspace((unsigned char)src[p])) p++;
        char opbuf[64]; int oi = 0;
        while (is_op_char(src[p]) && oi < 63) opbuf[oi++] = src[p++];
        while (isspace((unsigned char)src[p])) p++;
        if (oi > 0 && src[p] == ')') {
            src_pos = p + 1;
            opbuf[oi] = '\0';
            next_token();
            lb->name = strdup(opbuf);
            goto have_name;
        }
        // Pattern binding fallback — fall through to TK_EQ expect below.
        lb->pat = parse_pattern_no_or();
        lb->np = 0;
    }
    else if (tok == TK_IDENT || tok == TK_UNDER) {
        lb->name = (tok == TK_UNDER) ? "_" : strdup(tok_str);
        next_token();
have_name:;
        // Optional params.
        while (tok == TK_IDENT || tok == TK_UNDER || tok == TK_LPAREN ||
               tok == TK_TILDE || tok == TK_QMARK) {
            if (tok == TK_TILDE || tok == TK_QMARK) {
                next_token();
                if (tok != TK_IDENT) parse_fail("expected identifier after '~' or '?'");
                if (lb->np >= MAX_SCOPE_SLOTS) parse_fail("too many params");
                lb->params[lb->np++] = strdup(tok_str);
                next_token();
                if (tok == TK_COLON) {
                    next_token();
                    if (atom_starts(tok)) parse_atom();
                }
                continue;
            }
            if (tok == TK_LPAREN) {
                // `()` unit parameter — bind to a fresh unused slot.
                int peek_pos = src_pos, peek_line = src_line, peek_tok = tok;
                char peek_str[1024]; strcpy(peek_str, tok_str);
                int64_t peek_int = tok_int; double peek_dbl = tok_dbl;
                next_token();
                if (tok == TK_RPAREN) {
                    next_token();
                    if (lb->np >= MAX_SCOPE_SLOTS) parse_fail("too many params");
                    lb->params[lb->np++] = "_";
                    continue;
                }
                src_pos = peek_pos; src_line = peek_line; tok = peek_tok;
                strcpy(tok_str, peek_str);
                tok_int = peek_int; tok_dbl = peek_dbl;
                (void)peek_pos;
            }
            if (tok == TK_LPAREN) {
                // `(x : ty)` annotated param — skip the type annotation.
                int save = src_pos, saveline = src_line, savetok = tok;
                next_token();
                if (tok == TK_IDENT && src[src_pos] == ' ' /* heuristic */) {
                    // Probably (x : ty) form.
                    const char *p = strdup(tok_str);
                    next_token();
                    if (tok == TK_COLON) {
                        // skip type by walking until matching ')'
                        next_token();
                        int depth = 1;
                        while (depth > 0 && tok != TK_EOF) {
                            if (tok == TK_LPAREN) depth++;
                            else if (tok == TK_RPAREN) { depth--; if (depth == 0) break; }
                            next_token();
                        }
                        expect(TK_RPAREN, "')'");
                        if (lb->np >= MAX_SCOPE_SLOTS) parse_fail("too many params");
                        lb->params[lb->np++] = p;
                        continue;
                    }
                    // Not (x : ty), revert.
                    src_pos = save; src_line = saveline; tok = savetok;
                }
                else {
                    src_pos = save; src_line = saveline; tok = savetok;
                }
                break;
            }
            if (lb->np >= MAX_SCOPE_SLOTS) parse_fail("too many params");
            lb->params[lb->np++] = (tok == TK_UNDER) ? "_" : strdup(tok_str);
            next_token();
        }
    }
    else {
        parse_fail("expected ident or pattern after 'let'");
    }
    // Optional return-type annotation: `: ty`.  Skip it.
    if (tok == TK_COLON) {
        next_token();
        int depth = 0;
        while (tok != TK_EOF && tok != TK_EQ) {
            if (tok == TK_LPAREN) depth++;
            else if (tok == TK_RPAREN) {
                if (depth == 0) break;
                depth--;
            }
            next_token();
        }
    }
    expect(TK_EQ, "'='");
    return lb;
}

static NODE *
parse_expr_no_seq(void)
{
    if (tok == TK_IF) {
        next_token();
        NODE *c = parse_expr_no_seq();
        expect(TK_THEN, "'then'");
        NODE *t = parse_expr_no_seq();
        NODE *e;
        if (tok == TK_ELSE) { next_token(); e = parse_expr_no_seq(); }
        else                 { e = ALLOC_node_const_unit(); }
        return ALLOC_node_if(c, t, e);
    }
    if (tok == TK_FUN) {
        next_token();
        return parse_fun_after_kw();
    }
    if (tok == TK_FUNCTION) {
        next_token();
        return parse_function_after_kw();
    }
    if (tok == TK_TRY) {
        next_token();
        NODE *body = parse_expr();
        expect(TK_WITH, "'with'");
        if (tok == TK_BAR) next_token();
        // Parse arms — same as match.  Re-use match parser by parsing the
        // arms here in place; we won't have an outer let __m wrap because
        // node_try already supplies the exn at slot 0.
        enum { MAX_ARMS = 128 };
        struct match_arm arms[MAX_ARMS];
        int n = 0;

        const char *m_name[1] = { "__exn" };
        push_scope_n(1, m_name);

        while (n < MAX_ARMS) {
            struct pat *p = parse_arm_pattern();
            const char *names[MAX_SCOPE_SLOTS]; int nv = 0;
            pat_collect_vars(p, names, &nv);
            NODE *guard = NULL;
            if (tok == TK_WHEN) {
                next_token();
                if (nv > 0) push_scope_n(nv, names);
                guard = parse_expr_no_seq();
                if (nv > 0) pop_scope();
            }
            expect(TK_ARROW, "'->'");
            if (nv > 0) push_scope_n(nv, names);
            NODE *body_node = parse_expr();
            if (nv > 0) pop_scope();
            arms[n].pat = p; arms[n].guard = guard; arms[n].body = body_node; arms[n].n_vars = nv;
            n++;
            if (tok != TK_BAR) break;
            next_token();
        }
        // Last arm should re-raise instead of match-failure: if no
        // arm matches, propagate the exn.
        NODE *handler;
        {
            NODE *fail = ALLOC_node_raise(ALLOC_node_lref(0, 0));      // re-raise __exn
            for (int i = n - 1; i >= 0; i--) {
                struct match_arm *a = &arms[i];
                struct scrut_lref_ctx *ctx = malloc(sizeof *ctx);
                ctx->depth = 0; ctx->slot = 0;
                NODE *test = pat_gen_test(a->pat, scrut_fac_lref, ctx);
                NODE *bd = a->body;
                if (a->guard) bd = ALLOC_node_if(a->guard, bd, fail);
                const char *names[MAX_SCOPE_SLOTS]; int nv = 0;
                pat_collect_vars(a->pat, names, &nv);
                NODE *extracts[MAX_SCOPE_SLOTS]; int ei = 0;
                struct scrut_lref_ctx *bctx = malloc(sizeof *bctx);
                bctx->depth = 0; bctx->slot = 0;
                pat_gen_extracts(a->pat, scrut_fac_lref, bctx, extracts, &ei);
                uint32_t exi = stash_extract_nodes(extracts, ei);
                if (a->guard) {
                    NODE *armnode = make_match_arm(ALLOC_node_const_bool(1), (uint32_t)ei, exi, bd, fail);
                    fail = ALLOC_node_if(test, armnode, fail);
                }
                else {
                    fail = make_match_arm(test, (uint32_t)ei, exi, bd, fail);
                }
            }
            handler = fail;
        }
        pop_scope();
        return make_try(body, handler);
    }
    if (tok == TK_LET) {
        next_token();
        // `let open M in body` — local open.  We register name aliases at
        // parse time (they persist across this run; a deviation from
        // OCaml's lexical scoping for the sake of simplicity).
        if (tok == TK_OPEN) {
            next_token();
            char modname[256] = "";
            if (tok == TK_IDENT) {
                strncpy(modname, tok_str, sizeof modname - 1);
                next_token();
                while (tok == TK_DOT) {
                    next_token();
                    if (tok == TK_IDENT) {
                        strncat(modname, ".", sizeof modname - strlen(modname) - 1);
                        strncat(modname, tok_str, sizeof modname - strlen(modname) - 1);
                        next_token();
                    }
                }
            }
            // We can't access CTX here; defer alias creation to runtime
            // via a primitive call.  Build:
            //   __open_module modname; body
            NODE *open_call = make_app(ALLOC_node_gref(strdup("__open_module")),
                                      (NODE *[]){ALLOC_node_const_str(strdup(modname))}, 1);
            expect(TK_IN, "'in' after 'let open'");
            NODE *body = parse_expr();
            return ALLOC_node_seq(open_call, body);
        }
        bool is_rec = (tok == TK_REC);
        if (is_rec) next_token();

        struct let_binding *lb = parse_let_binding();
        // Build value.
        // For `let rec`, mutual recursion via `and` requires that all
        // binding names are in scope when each value is parsed.  We
        // implement this by pre-scanning: parse each LHS, save the lexer
        // position at the value's start, skip the value with
        // skip_let_value(), then come back to parse all values once the
        // full name set is known.
        struct let_binding *bindings[64];
        int nb = 0;
        bindings[nb++] = lb;

        struct lex_state value_starts[64];

        if (is_rec) {
            // Save the start of lb->value (we just consumed `=`).
            lex_save(&value_starts[0]);
            skip_let_value();
            while (tok == TK_KW_AND) {
                next_token();
                struct let_binding *lb2 = parse_let_binding();
                bindings[nb] = lb2;
                lex_save(&value_starts[nb]);
                nb++;
                skip_let_value();
            }
            if (tok != TK_IN) parse_fail("expected 'in' after let-rec bindings");
            struct lex_state after_in;
            next_token();
            lex_save(&after_in);
            // Now parse all values with all binding names in scope.
            const char *bnames[64];
            for (int i = 0; i < nb; i++) bnames[i] = bindings[i]->name ? bindings[i]->name : "_";
            push_scope_n(nb, bnames);
            for (int i = 0; i < nb; i++) {
                lex_restore(&value_starts[i]);
                struct let_binding *bi = bindings[i];
                if (bi->np > 0) {
                    push_scope_n(bi->np, bi->params);
                    NODE *body = parse_expr();
                    pop_scope();
                    bi->value = (mark_tail_calls(body), make_fun((uint32_t)bi->np, body));
                }
                else {
                    bi->value = parse_expr();
                }
            }
            pop_scope();
            lex_restore(&after_in);
        }
        else {
            // Non-rec single binding (no `and` allowed).
            NODE *value;
            if (lb->np > 0) {
                push_scope_n(lb->np, lb->params);
                NODE *body = parse_expr();
                pop_scope();
                value = (mark_tail_calls(body), make_fun((uint32_t)lb->np, body));
            }
            else {
                value = parse_expr();
            }
            lb->value = value;
            if (tok != TK_IN) parse_fail("expected 'in' after let-binding");
            next_token();
        }

        // Push body scope with all bindings.
        // For pattern binding (nb == 1, lb->name == NULL, lb->pat set),
        // collect pattern vars and push them as the body scope.  For
        // simple-name bindings, push the names.
        const char *bnames[MAX_SCOPE_SLOTS];
        int n_bnames;
        bool is_pat_bind = (nb == 1 && !bindings[0]->name && bindings[0]->pat);
        if (is_pat_bind) {
            n_bnames = 0;
            pat_collect_vars(bindings[0]->pat, bnames, &n_bnames);
        }
        else {
            n_bnames = nb;
            for (int i = 0; i < nb; i++) bnames[i] = bindings[i]->name ? bindings[i]->name : "_";
        }
        if (n_bnames > 0) push_scope_n(n_bnames, bnames);
        NODE *cont = parse_expr();
        if (n_bnames > 0) pop_scope();

        // Build expr.
        if (nb == 1 && lb->name && !lb->pat) {
            // Single simple binding.
            return is_rec ? ALLOC_node_letrec(lb->value, cont) : ALLOC_node_let(lb->value, cont);
        }
        if (is_pat_bind) {
            // Use node_let_pat: push N-slot frame with extracted values,
            // then run body.
            NODE *extracts[MAX_SCOPE_SLOTS]; int ei = 0;
            struct scrut_lref_ctx *ctx = malloc(sizeof *ctx);
            ctx->depth = 0; ctx->slot = 0;
            pat_gen_extracts(lb->pat, scrut_fac_lref, ctx, extracts, &ei);
            uint32_t exi = stash_extract_nodes(extracts, ei);
            return make_let_pat(lb->value, (uint32_t)ei, exi, cont);
        }
        // Multiple bindings — use letrec_n.
        NODE *vals[64];
        for (int i = 0; i < nb; i++) vals[i] = bindings[i]->value;
        uint32_t vidx = stash_letrec_values(vals, nb);
        return make_letrec_n((uint32_t)nb, vidx, cont);
    }
    if (tok == TK_MATCH) {
        next_token();
        return parse_match_after_kw();
    }
    return parse_tuple_expr();
}

static int
expr_stop_token(int t)
{
    switch (t) {
    case TK_EOF: case TK_DSEMI: case TK_RPAREN: case TK_RBRACK: case TK_RBRACE:
    case TK_BARRBRACK:
    case TK_END: case TK_THEN: case TK_ELSE: case TK_IN: case TK_WITH:
    case TK_BAR: case TK_ARROW: case TK_DONE: case TK_DO: case TK_TO: case TK_DOWNTO:
    case TK_KW_AND: case TK_WHEN:
        return 1;
    }
    return 0;
}

static NODE *
parse_expr(void)
{
    NODE *e = parse_expr_no_seq();
    while (tok == TK_SEMI) {
        next_token();
        if (expr_stop_token(tok)) break;
        NODE *r = parse_expr_no_seq();
        e = ALLOC_node_seq(e, r);
    }
    return e;
}

// Tuple expression: `e, e, e` (comma-separated).
static NODE *
parse_tuple_expr(void)
{
    NODE *first = parse_or_expr();
    if (tok != TK_COMMA) return first;
    NODE *items[64]; int n = 0;
    items[n++] = first;
    while (tok == TK_COMMA) {
        next_token();
        items[n++] = parse_or_expr();
    }
    uint32_t idx = stash_call_args(items, n);
    return ALLOC_node_tuple_n(idx, (uint32_t)n);
}

static NODE *parse_or_expr(void)  { NODE *l = parse_and_expr(); while (tok == TK_PIPEPIPE || tok == TK_OR_KW) { next_token(); l = ALLOC_node_or (l, parse_and_expr()); } return l; }
static NODE *parse_and_expr(void) { NODE *l = parse_cmp(); while (tok == TK_AMPAMP)    { next_token(); l = ALLOC_node_and(l, parse_cmp()); }    return l; }

static NODE *
parse_cmp(void)
{
    NODE *l = parse_concat();
    int op = tok;
    if (op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE ||
        op == TK_EQ || op == TK_NE || op == TK_PEQ || op == TK_PNE) {
        next_token();
        NODE *r = parse_concat();
        switch (op) {
        case TK_LT: return ALLOC_node_lt(l, r);
        case TK_GT: return ALLOC_node_gt(l, r);
        case TK_LE: return ALLOC_node_le(l, r);
        case TK_GE: return ALLOC_node_ge(l, r);
        case TK_EQ: return ALLOC_node_eq(l, r);
        case TK_NE: return ALLOC_node_ne(l, r);
        case TK_PEQ: return ALLOC_node_phys_eq(l, r);
        case TK_PNE: return ALLOC_node_phys_ne(l, r);
        }
    }
    if (tok == TK_IDENT && is_custom_op_starting_with(tok_str, "<>=|&$!")) {
        const char *opname = strdup(tok_str);
        next_token();
        NODE *r = parse_concat();
        NODE *args[2] = { l, r };
        return make_app(ALLOC_node_gref(strdup(opname)), args, 2);
    }
    return l;
}

static NODE *
parse_concat(void)
{
    NODE *l = parse_cons();
    for (;;) {
        if (tok == TK_CONCAT) { next_token(); l = ALLOC_node_concat(l, parse_cons()); }
        else if (tok == TK_IDENT && is_custom_op_starting_with(tok_str, "^@")) {
            const char *op = strdup(tok_str);
            next_token();
            NODE *r = parse_cons();
            NODE *args[2] = { l, r };
            l = make_app(ALLOC_node_gref(strdup(op)), args, 2);
        }
        else break;
    }
    return l;
}

static NODE *
parse_cons(void)
{
    NODE *l = parse_assign();
    if (tok == TK_CONS) { next_token(); NODE *r = parse_cons(); return ALLOC_node_cons(l, r); }
    return l;
}

// `:=` is right-associative-ish (assignments), low precedence.
static NODE *
parse_assign(void)
{
    NODE *l = parse_add();
    if (tok == TK_ASSIGN) {
        next_token();
        NODE *r = parse_assign();
        // If l is a bare-field-style send (`self#name` with 0 args, as
        // emitted by resolve_var for class field names), convert to
        // node_field_assign which mutates the object's field.
        if (l->head.kind &&
            l->head.kind->default_dispatcher_name &&
            !strcmp(l->head.kind->default_dispatcher_name, "DISPATCH_node_send") &&
            l->u.node_send.argc == 0) {
            return ALLOC_node_field_assign(l->u.node_send.obj, l->u.node_send.method, r);
        }
        return ALLOC_node_assign(l, r);
    }
    return l;
}

static NODE *
parse_add(void)
{
    NODE *l = parse_mul();
    for (;;) {
        if (tok == TK_PLUS)   { next_token(); l = ALLOC_node_add(l, parse_mul()); }
        else if (tok == TK_MINUS)  { next_token(); l = ALLOC_node_sub(l, parse_mul()); }
        else if (tok == TK_FPLUS)  { next_token(); l = ALLOC_node_fadd(l, parse_mul()); }
        else if (tok == TK_FMINUS) { next_token(); l = ALLOC_node_fsub(l, parse_mul()); }
        else if (tok == TK_IDENT && is_custom_op_starting_with(tok_str, "+-")) {
            const char *op = strdup(tok_str);
            next_token();
            NODE *r = parse_mul();
            NODE *args[2] = { l, r };
            l = make_app(ALLOC_node_gref(strdup(op)), args, 2);
        }
        else break;
    }
    return l;
}

static NODE *
parse_mul(void)
{
    NODE *l = parse_unary();
    for (;;) {
        if (tok == TK_STAR)   { next_token(); l = ALLOC_node_mul(l, parse_unary()); }
        else if (tok == TK_SLASH)  { next_token(); l = ALLOC_node_div(l, parse_unary()); }
        else if (tok == TK_FSTAR)  { next_token(); l = ALLOC_node_fmul(l, parse_unary()); }
        else if (tok == TK_FSLASH) { next_token(); l = ALLOC_node_fdiv(l, parse_unary()); }
        else if (tok == TK_MOD)    { next_token(); l = ALLOC_node_mod(l, parse_unary()); }
        else if (tok == TK_LAND_KW){ next_token(); l = ALLOC_node_mul(l, parse_unary()); /* placeholder */ }
        else if (tok == TK_LOR_KW) { next_token(); l = ALLOC_node_mul(l, parse_unary()); /* placeholder */ }
        else if (tok == TK_IDENT && is_custom_op_starting_with(tok_str, "*/%")) {
            const char *op = strdup(tok_str);
            next_token();
            NODE *r = parse_unary();
            NODE *args[2] = { l, r };
            l = make_app(ALLOC_node_gref(strdup(op)), args, 2);
        }
        else break;
    }
    return l;
}

static NODE *
parse_unary(void)
{
    if (tok == TK_MINUS)  { next_token(); return ALLOC_node_neg(parse_unary()); }
    if (tok == TK_FMINUS) { next_token(); return ALLOC_node_fneg(parse_unary()); }
    if (tok == TK_NOT)    { next_token(); return ALLOC_node_not(parse_unary()); }
    if (tok == TK_BANG)   { next_token(); return ALLOC_node_deref(parse_unary()); }
    return parse_app();
}

static int
atom_starts(int t)
{
    switch (t) {
    case TK_INT: case TK_FLOAT_TOK: case TK_IDENT: case TK_STRING: case TK_CHAR:
    case TK_TRUE: case TK_FALSE: case TK_LPAREN: case TK_LBRACK: case TK_LBRACE:
    case TK_BEGIN: case TK_LBRACKBAR: case TK_BANG:
    case TK_TILDE: case TK_QMARK: case TK_LAZY:
        return 1;
    }
    return 0;
}

// `!e` (deref) binds tighter than function application — `f !x` is
// `f (!x)`.  parse_simple_expr handles the unary deref before falling
// into parse_atom.  Also handles labeled args at call sites: `~x` /
// `~x:value` / `?x` / `?x:value` — labels are stripped to positional
// values (a simplification: full label-based dispatch is not implemented).
static NODE *
parse_simple_expr(void)
{
    if (tok == TK_BANG) { next_token(); return ALLOC_node_deref(parse_simple_expr()); }
    if (tok == TK_LAZY) { next_token(); return ALLOC_node_lazy(parse_simple_expr()); }
    if (tok == TK_TILDE) {
        next_token();
        if (tok != TK_IDENT) parse_fail("expected identifier after '~'");
        const char *name = strdup(tok_str);
        next_token();
        if (tok == TK_COLON) {
            next_token();
            return parse_simple_expr();
        }
        return resolve_var(name);
    }
    if (tok == TK_QMARK) {
        next_token();
        if (tok != TK_IDENT) parse_fail("expected identifier after '?'");
        const char *name = strdup(tok_str);
        next_token();
        if (tok == TK_COLON) {
            next_token();
            return parse_simple_expr();
        }
        // `?x` alone — the variable, wrapped in `Some`.  We use the
        // bare variable for simplicity (real OCaml: `Some x`).
        return resolve_var(name);
    }
    return parse_atom();
}

static NODE *
parse_app(void)
{
    NODE *fn = parse_simple_expr();
    // Custom infix-op TK_IDENT (e.g. `+!`, `<*>`) must NOT be consumed as
    // an application argument — that's the operator separator between
    // operands.  We let the surrounding precedence parsers (parse_add /
    // parse_mul / parse_cmp / parse_concat) pick it up.
    if (!atom_starts(tok)) return fn;
    if (tok == TK_IDENT && is_op_char(tok_str[0])) return fn;
    NODE *args[16];
    int n = 0;
    while (atom_starts(tok)) {
        if (tok == TK_IDENT && is_op_char(tok_str[0])) break;
        if (n >= 16) parse_fail("too many args in single application");
        args[n++] = parse_simple_expr();
    }
    return make_app(fn, args, n);
}

static NODE *
parse_atom(void)
{
    // `new C args` — `new` is just a marker for class instantiation; we
    // treat classes as constructor functions, so `new` is a no-op.
    if (tok == TK_NEW) next_token();
    NODE *base = parse_atom_basic();
    // Postfix loop: `.field` (record access), `.(idx)` (array access),
    // `#method` (method send).
    while (tok == TK_DOT || tok == TK_HASH) {
        if (tok == TK_HASH) {
            next_token();
            if (tok != TK_IDENT) parse_fail("expected method name after '#'");
            const char *method = strdup(tok_str);
            next_token();
            // Method args: zero-or-more parse_simple_expr's, like function app.
            NODE *args[16]; int n = 0;
            while (atom_starts(tok)) {
                if (n >= 16) parse_fail("too many method args");
                args[n++] = parse_simple_expr();
            }
            uint32_t idx = stash_call_args(args, n);
            base = ALLOC_node_send(base, method, idx, (uint32_t)n);
            continue;
        }
        next_token();
        if (tok == TK_LPAREN) {
            next_token();
            NODE *idx = parse_expr_no_seq();
            expect(TK_RPAREN, "')'");
            NODE *args[2] = { base, idx };
            base = make_app(ALLOC_node_gref(strdup("__array_get")), args, 2);
            continue;
        }
        if (tok != TK_IDENT) parse_fail("expected field name after '.'");
        const char *field = strdup(tok_str);
        next_token();
        // Field set via `r.f <- v` (note: TK_ASSIGN tokenizes both `:=`
        // and `<-`; here we treat it as record/object field assignment).
        if (tok == TK_ASSIGN) {
            next_token();
            NODE *val = parse_expr_no_seq();
            base = ALLOC_node_field_assign(base, field, val);
            continue;
        }
        base = ALLOC_node_record_get(base, field);
    }
    return base;
}

static NODE *
parse_atom_basic(void)
{
    if (tok == TK_INT)      { int64_t v = tok_int; next_token(); return ALLOC_node_const_int((int32_t)v); }
    if (tok == TK_FLOAT_TOK){ double v = tok_dbl; next_token(); return ALLOC_node_const_float(v); }
    if (tok == TK_TRUE)     { next_token(); return ALLOC_node_const_bool(1); }
    if (tok == TK_FALSE)    { next_token(); return ALLOC_node_const_bool(0); }
    if (tok == TK_STRING)   { NODE *n = ALLOC_node_const_str(strdup(tok_str)); next_token(); return n; }
    if (tok == TK_CHAR)     { int64_t v = tok_int; next_token(); return ALLOC_node_const_int((int32_t)v); }
    if (tok == TK_IDENT) {
        char first[256];
        strncpy(first, tok_str, sizeof first - 1); first[sizeof first - 1] = '\0';
        next_token();
        if (is_uppercase_ident(first)) {
            // Module path or constructor.
            char path[1024];
            strcpy(path, first);
            bool path_ended = false;       // ended at lowercase ident
            while (tok == TK_DOT) {
                next_token();
                if (tok != TK_IDENT) parse_fail("expected identifier after '.'");
                strcat(path, ".");
                strcat(path, tok_str);
                bool last_lower = !is_uppercase_ident(tok_str);
                next_token();
                if (last_lower) { path_ended = true; break; }
            }
            if (path_ended) {
                // Module path ending in lower ident — global ref.
                return make_gref(path);
            }
            // All-uppercase path: constructor.
            // Check for argument (atom).
            if (atom_starts(tok)) {
                NODE *arg = parse_atom();
                NODE *args[1] = {arg};
                uint32_t idx = stash_call_args(args, 1);
                return ALLOC_node_variant_n(strdup(path), idx, 1);
            }
            return ALLOC_node_variant_0(strdup(path));
        }
        return resolve_var(first);
    }
    if (tok == TK_LBRACK) {
        if (tok_int) { next_token(); return ALLOC_node_const_nil(); }
        next_token();
        NODE *items[256]; int n = 0;
        if (tok != TK_RBRACK) {
            items[n++] = parse_expr_no_seq();
            while (tok == TK_SEMI) { next_token(); if (tok == TK_RBRACK) break; items[n++] = parse_expr_no_seq(); }
        }
        expect(TK_RBRACK, "']'");
        NODE *tail = ALLOC_node_const_nil();
        for (int i = n - 1; i >= 0; i--) tail = ALLOC_node_cons(items[i], tail);
        return tail;
    }
    if (tok == TK_LBRACKBAR) {
        // Array literal `[| 1; 2; 3 |]` — call __array_make as a single
        // call with all elements (variable arity).
        next_token();
        NODE *items[256]; int n = 0;
        if (tok != TK_BARRBRACK) {
            items[n++] = parse_expr_no_seq();
            while (tok == TK_SEMI) { next_token(); if (tok == TK_BARRBRACK) break; items[n++] = parse_expr_no_seq(); }
        }
        expect(TK_BARRBRACK, "'|]'");
        // Translate to make_array call.
        NODE *args[256];
        for (int i = 0; i < n; i++) args[i] = items[i];
        return make_app(ALLOC_node_gref(strdup("__array_make")), args, n);
    }
    if (tok == TK_LPAREN) {
        // `(<op_chars>)` — operator-as-identifier, single- or multi-char.
        // We scan the source directly (not via the lexer) so we pick up
        // multi-char custom operators like `(+!)`, `(<*>)`.  If the scan
        // fails, we fall through to normal `( ... )` parsing.
        {
            int p = src_pos;
            while (isspace((unsigned char)src[p])) p++;
            char opbuf[64]; int oi = 0;
            while (is_op_char(src[p]) && oi < 63) opbuf[oi++] = src[p++];
            while (isspace((unsigned char)src[p])) p++;
            if (oi > 0 && src[p] == ')') {
                src_pos = p + 1;
                opbuf[oi] = '\0';
                next_token();
                return resolve_var(strdup(opbuf));
            }
        }
        next_token();
        if (tok == TK_RPAREN) { next_token(); return ALLOC_node_const_unit(); }
        NODE *e = parse_expr();
        // Optional type annotation: `(e : ty)`. Skip type.
        if (tok == TK_COLON) {
            next_token();
            int depth = 0;
            while (tok != TK_EOF && tok != TK_RPAREN) {
                if (tok == TK_LPAREN) depth++;
                else if (tok == TK_RPAREN) {
                    if (depth == 0) break;
                    depth--;
                }
                next_token();
            }
        }
        expect(TK_RPAREN, "')'");
        return e;
    }
    if (tok == TK_BEGIN) {
        next_token();
        NODE *e = parse_expr();
        expect(TK_END, "'end'");
        return e;
    }
    if (tok == TK_LBRACE) {
        // Record literal: `{ f1 = e1; f2 = e2 }` or `{ r with f = v; ... }`.
        next_token();
        // Detect `{ <expr> with ...}` form.
        // Save state, try parsing as expr; if next is `with`, it's a
        // record-update form.
        int save_pos = src_pos, save_line = src_line, save_tok = tok;
        char save_str[1024]; strcpy(save_str, tok_str);
        int64_t save_int = tok_int; double save_dbl = tok_dbl;
        // Heuristic: if first token is IDENT and next-next is WITH, it's update.
        // Simpler: try parsing as expr; if next is WITH, handle update.
        NODE *base = NULL;
        if (tok == TK_IDENT) {
            // Look ahead: parse atom and check for WITH.
            int idsave_pos = src_pos, idsave_line = src_line, idsave_tok = tok;
            char idsave_str[1024]; strcpy(idsave_str, tok_str);
            base = parse_atom();
            if (tok == TK_WITH) {
                next_token();
            }
            else {
                // Not a `with` form — restore state and parse as field list.
                src_pos = idsave_pos; src_line = idsave_line; tok = idsave_tok;
                strcpy(tok_str, idsave_str);
                base = NULL;
            }
        }
        (void)save_pos; (void)save_line; (void)save_tok; (void)save_str; (void)save_int; (void)save_dbl;

        const char *fields[64]; NODE *vals[64]; int n = 0;
        while (tok != TK_RBRACE) {
            if (tok != TK_IDENT) parse_fail("expected field name");
            fields[n] = strdup(tok_str);
            next_token();
            if (tok == TK_EQ) {
                next_token();
                vals[n] = parse_expr_no_seq();
            }
            else {
                // Punning
                vals[n] = resolve_var(fields[n]);
            }
            n++;
            if (tok == TK_SEMI) next_token();
            else break;
        }
        expect(TK_RBRACE, "'}'");
        uint32_t fidx = stash_record_fields(fields, n);
        uint32_t vidx = stash_call_args(vals, n);
        if (base) {
            return ALLOC_node_record_with(base, fidx, vidx, (uint32_t)n);
        }
        return ALLOC_node_record_n(fidx, vidx, (uint32_t)n);
    }
    parse_fail("expected atom (got tok=%d)", tok);
}

// ---------------------------------------------------------------------------
// Top-level parser.
// ---------------------------------------------------------------------------

static void
skip_type_decl(void)
{
    // Skip until `;;` or next `let`/`type`/`open`/`module`/etc, balanced.
    int depth = 0;
    while (tok != TK_EOF && tok != TK_DSEMI) {
        if (depth == 0 && (tok == TK_LET || tok == TK_TYPE || tok == TK_OPEN ||
                            tok == TK_MODULE || tok == TK_INCLUDE || tok == TK_EXCEPTION_KW)) {
            break;
        }
        if (tok == TK_LPAREN || tok == TK_LBRACK || tok == TK_LBRACE) depth++;
        else if (tok == TK_RPAREN || tok == TK_RBRACK || tok == TK_RBRACE) {
            if (depth > 0) depth--;
        }
        next_token();
    }
    if (tok == TK_DSEMI) next_token();
}

static void parse_program_until(CTX *c, int stop_tok);

static void
parse_program(CTX *c)
{
    parse_program_until(c, TK_EOF);
}

static void
parse_program_until(CTX *c, int stop_tok)
{
    while (tok != TK_EOF && tok != stop_tok) {
        if (tok == TK_DSEMI) { next_token(); continue; }
        if (tok == TK_TYPE) {
            // Skip type declaration body.
            next_token();
            skip_type_decl();
            continue;
        }
        if (tok == TK_OPEN) {
            // `open M` — best effort: collect aliases `M.name` → `name`
            // for already-defined globals so subsequent lookups succeed
            // without prefix.
            next_token();
            char modname[256] = "";
            if (tok == TK_IDENT) {
                strncpy(modname, tok_str, sizeof modname - 1);
                next_token();
                while (tok == TK_DOT) {
                    next_token();
                    if (tok == TK_IDENT) {
                        strncat(modname, ".", sizeof modname - strlen(modname) - 1);
                        strncat(modname, tok_str, sizeof modname - strlen(modname) - 1);
                        next_token();
                    }
                }
            }
            // Walk globals; for each "modname.X" alias as "X".
            size_t plen = strlen(modname);
            for (size_t i = 0; i < c->globals_size; i++) {
                const char *gn = c->globals[i].name;
                if (strncmp(gn, modname, plen) == 0 && gn[plen] == '.') {
                    oc_global_define(c, gn + plen + 1, c->globals[i].value);
                }
            }
            if (tok == TK_DSEMI) next_token();
            continue;
        }
        if (tok == TK_INCLUDE) {
            // `include M` — same effect as `open` for our purposes.
            next_token();
            skip_type_decl();
            continue;
        }
        if (tok == TK_MODULE) {
            next_token();
            // `module type S = sig ... end` — skip.
            if (tok == TK_TYPE) {
                next_token();
                // Skip until `;;` or top decl, balancing struct/sig/begin.
                int d = 0;
                while (tok != TK_EOF) {
                    if (d == 0 && (tok == TK_DSEMI || tok == TK_LET || tok == TK_TYPE ||
                                   tok == TK_MODULE || tok == TK_OPEN || tok == TK_INCLUDE ||
                                   tok == TK_EXCEPTION_KW)) break;
                    if (tok == TK_STRUCT || tok == TK_SIG || tok == TK_BEGIN) d++;
                    else if (tok == TK_END) { if (d > 0) d--; }
                    next_token();
                }
                if (tok == TK_DSEMI) next_token();
                continue;
            }
            if (tok != TK_IDENT) parse_fail("expected module name");
            char modname[128];
            strncpy(modname, tok_str, sizeof modname - 1); modname[sizeof modname - 1] = '\0';
            next_token();
            // `module F (X : S) = struct ... end` — sugar for
            // `module F = functor (X : S) -> struct ... end`.  We handle
            // the parameter here and arrange for the rest of this branch
            // to see TK_FUNCTOR (effectively).
            char sugar_param[128] = "";
            if (tok == TK_LPAREN) {
                next_token();
                if (tok == TK_IDENT) {
                    strncpy(sugar_param, tok_str, sizeof sugar_param - 1);
                    next_token();
                }
                if (tok == TK_COLON) {
                    next_token();
                    int dd = 0;
                    while (tok != TK_EOF) {
                        if (tok == TK_LPAREN) dd++;
                        else if (tok == TK_RPAREN) { if (dd == 0) break; dd--; }
                        next_token();
                    }
                }
                expect(TK_RPAREN, "')'");
            }
            // Optional `: SIG` — skip until `=`.
            if (tok == TK_COLON) {
                next_token();
                int d = 0;
                while (tok != TK_EOF) {
                    if (tok == TK_EQ && d == 0) break;
                    if (tok == TK_LPAREN || tok == TK_LBRACE || tok == TK_BEGIN || tok == TK_SIG) d++;
                    else if (tok == TK_RPAREN || tok == TK_RBRACE || tok == TK_END) { if (d > 0) d--; }
                    next_token();
                }
            }
            expect(TK_EQ, "'='");
            // `module M = functor (X : S) -> struct ... end` form.
            char functor_param[128] = "";
            if (tok == TK_FUNCTOR) {
                next_token();
                if (tok == TK_LPAREN) {
                    next_token();
                    if (tok == TK_IDENT) {
                        strncpy(functor_param, tok_str, sizeof functor_param - 1);
                        next_token();
                    }
                    if (tok == TK_COLON) {
                        next_token();
                        int dd = 0;
                        while (tok != TK_EOF) {
                            if (tok == TK_LPAREN) dd++;
                            else if (tok == TK_RPAREN) { if (dd == 0) break; dd--; }
                            next_token();
                        }
                    }
                    expect(TK_RPAREN, "')'");
                }
                if (tok == TK_ARROW) next_token();
            }
            // Combine sugar param + functor keyword param (whichever was
            // provided is non-empty).
            const char *param_name = sugar_param[0] ? sugar_param : functor_param[0] ? functor_param : NULL;

            if (tok == TK_STRUCT) {
                // Save src position right AFTER `struct` (before consuming
                // the first body token).  This is what `lex_restore` /
                // `next_token` need for replay.
                int body_pos_before = src_pos;
                int body_line_before = src_line;
                next_token();
                if (param_name) {
                    // Functor: register body src range, skip body.
                    if (g_functors_n >= 64) parse_fail("too many functors");
                    struct functor_entry *fe = &g_functors[g_functors_n++];
                    strncpy(fe->name, modname, sizeof fe->name - 1);
                    fe->name[sizeof fe->name - 1] = '\0';
                    strncpy(fe->param, param_name, sizeof fe->param - 1);
                    fe->param[sizeof fe->param - 1] = '\0';
                    fe->body_start_pos = body_pos_before;
                    fe->body_start_line = body_line_before;
                    // Skip to matching `end`.
                    int d = 0;
                    while (tok != TK_EOF) {
                        if (tok == TK_END && d == 0) break;
                        if (tok == TK_OBJECT || tok == TK_BEGIN || tok == TK_STRUCT || tok == TK_SIG) d++;
                        else if (tok == TK_END) { if (d > 0) d--; }
                        next_token();
                    }
                    fe->body_end_pos = src_pos;
                    expect(TK_END, "'end'");
                    if (tok == TK_DSEMI) next_token();
                    continue;
                }
                char saved_prefix[256];
                strncpy(saved_prefix, cur_module_prefix, sizeof saved_prefix);
                if (cur_module_prefix[0] == '\0') {
                    strncpy(cur_module_prefix, modname, sizeof cur_module_prefix - 1);
                }
                else {
                    size_t l = strlen(cur_module_prefix);
                    snprintf(cur_module_prefix + l, sizeof cur_module_prefix - l, ".%s", modname);
                }
                parse_program_until(c, TK_END);
                expect(TK_END, "'end'");
                strncpy(cur_module_prefix, saved_prefix, sizeof cur_module_prefix);
                if (tok == TK_DSEMI) next_token();
                continue;
            }
            // `module N = F (Arg)` (functor application) or
            // `module N = M`     (alias) or
            // `module N = M.N`   (alias with dotted path)
            if (tok == TK_IDENT) {
                char target[128];
                strncpy(target, tok_str, sizeof target - 1); target[sizeof target - 1] = '\0';
                next_token();
                while (tok == TK_DOT) {
                    next_token();
                    if (tok == TK_IDENT) {
                        strncat(target, ".", sizeof target - strlen(target) - 1);
                        strncat(target, tok_str, sizeof target - strlen(target) - 1);
                        next_token();
                    }
                }
                // Functor application?
                if (tok == TK_LPAREN) {
                    struct functor_entry *fe = functor_lookup(target);
                    if (!fe) parse_fail("unknown functor: %s", target);
                    next_token();
                    char arg_name[128] = "";
                    if (tok == TK_IDENT) {
                        strncpy(arg_name, tok_str, sizeof arg_name - 1);
                        next_token();
                        while (tok == TK_DOT) {
                            next_token();
                            if (tok == TK_IDENT) {
                                strncat(arg_name, ".", sizeof arg_name - strlen(arg_name) - 1);
                                strncat(arg_name, tok_str, sizeof arg_name - strlen(arg_name) - 1);
                                next_token();
                            }
                        }
                    }
                    expect(TK_RPAREN, "')'");
                    // Alias every <arg_name>.x as <param>.x so the body's
                    // references to X.x resolve.
                    size_t alen = strlen(arg_name);
                    size_t pmlen = strlen(fe->param);
                    for (size_t i = 0; i < c->globals_size; i++) {
                        const char *gn = c->globals[i].name;
                        if (strncmp(gn, arg_name, alen) == 0 && gn[alen] == '.') {
                            char buf[256];
                            snprintf(buf, sizeof buf, "%s.%s", fe->param, gn + alen + 1);
                            (void)pmlen;
                            oc_global_define(c, buf, c->globals[i].value);
                        }
                    }
                    // Save lex state, jump to functor body, set prefix to N.
                    struct lex_state saved_lex;
                    lex_save(&saved_lex);
                    char saved_prefix[256];
                    strncpy(saved_prefix, cur_module_prefix, sizeof saved_prefix);
                    if (cur_module_prefix[0] == '\0') {
                        strncpy(cur_module_prefix, modname, sizeof cur_module_prefix - 1);
                    }
                    else {
                        size_t l = strlen(cur_module_prefix);
                        snprintf(cur_module_prefix + l, sizeof cur_module_prefix - l, ".%s", modname);
                    }
                    src_pos = fe->body_start_pos;
                    src_line = fe->body_start_line;
                    next_token();   // refill from new position
                    parse_program_until(c, TK_END);
                    // We expect to be at TK_END.  The body src ended where
                    // we recorded; skip our re-tokenized `end` and restore.
                    strncpy(cur_module_prefix, saved_prefix, sizeof cur_module_prefix);
                    lex_restore(&saved_lex);
                    if (tok == TK_DSEMI) next_token();
                    continue;
                }
                // Plain alias.
                size_t plen = strlen(target);
                for (size_t i = 0; i < c->globals_size; i++) {
                    const char *gn = c->globals[i].name;
                    if (strncmp(gn, target, plen) == 0 && gn[plen] == '.') {
                        char buf[256];
                        snprintf(buf, sizeof buf, "%s.%s", modname, gn + plen + 1);
                        oc_global_define(c, buf, c->globals[i].value);
                    }
                }
                if (tok == TK_DSEMI) next_token();
                continue;
            }
            // Unknown form — skip best-effort.
            skip_type_decl();
            continue;
        }
        if (tok == TK_CLASS) {
            // `class NAME params = object body end`.
            next_token();
            // Optional `virtual` / `[type params]` — skip the rest until name.
            while (tok != TK_IDENT && tok != TK_EOF) next_token();
            if (tok != TK_IDENT) parse_fail("expected class name");
            const char *cname = strdup(tok_str);
            next_token();
            const char *cparams[MAX_SCOPE_SLOTS]; int ncp = 0;
            while (tok == TK_IDENT || tok == TK_LPAREN) {
                if (tok == TK_LPAREN) {
                    int sp = src_pos, sl = src_line, st = tok;
                    char ss[1024]; strcpy(ss, tok_str);
                    next_token();
                    if (tok == TK_RPAREN) { next_token(); cparams[ncp++] = "_"; continue; }
                    if (tok == TK_IDENT) {
                        const char *pn = strdup(tok_str);
                        next_token();
                        if (tok == TK_COLON) {
                            next_token();
                            int d = 0;
                            while (tok != TK_EOF) {
                                if (tok == TK_LPAREN) d++;
                                else if (tok == TK_RPAREN) { if (d == 0) break; d--; }
                                next_token();
                            }
                            if (tok == TK_RPAREN) next_token();
                            cparams[ncp++] = pn; continue;
                        }
                    }
                    src_pos = sp; src_line = sl; tok = st; strcpy(tok_str, ss);
                    break;
                }
                cparams[ncp++] = strdup(tok_str);
                next_token();
            }
            if (tok == TK_COLON) {
                next_token();
                int d = 0;
                while (tok != TK_EOF) {
                    if (tok == TK_EQ && d == 0) break;
                    if (tok == TK_LBRACK || tok == TK_LPAREN) d++;
                    else if (tok == TK_RBRACK || tok == TK_RPAREN) { if (d > 0) d--; }
                    next_token();
                }
            }
            expect(TK_EQ, "'='");
            if (tok == TK_FUN) {
                next_token();
                while (tok != TK_OBJECT && tok != TK_EOF) next_token();
            }
            expect(TK_OBJECT, "'object'");
            // Pre-scan the class body to collect all field and method
            // names — this lets bare field/method references inside one
            // method body resolve to siblings declared later in the class.
            const char *pre_fields[64]; int pre_nf = 0;
            const char *pre_methods[64]; int pre_nm = 0;
            {
                struct lex_state pre_save;
                lex_save(&pre_save);
                int depth = 0;
                while (tok != TK_EOF) {
                    if (depth == 0 && tok == TK_END) break;
                    if (tok == TK_OBJECT || tok == TK_BEGIN || tok == TK_STRUCT || tok == TK_SIG) depth++;
                    else if (tok == TK_END) { if (depth > 0) depth--; }
                    if (depth == 0 && tok == TK_VAL) {
                        next_token();
                        if (tok == TK_MUTABLE) next_token();
                        if (tok == TK_IDENT) {
                            if (pre_nf < 64) pre_fields[pre_nf++] = strdup(tok_str);
                            next_token();
                        }
                        continue;
                    }
                    if (depth == 0 && tok == TK_METHOD) {
                        next_token();
                        if (tok == TK_PRIVATE) next_token();
                        if (tok == TK_IDENT) {
                            if (pre_nm < 64) pre_methods[pre_nm++] = strdup(tok_str);
                            next_token();
                        }
                        continue;
                    }
                    next_token();
                }
                lex_restore(&pre_save);
            }
            const char **saved_class_fields = cur_class_fields;
            int saved_class_fields_n = cur_class_fields_n;
            const char **saved_class_methods = cur_class_methods;
            int saved_class_methods_n = cur_class_methods_n;
            cur_class_fields = pre_fields;
            cur_class_fields_n = pre_nf;
            cur_class_methods = pre_methods;
            cur_class_methods_n = pre_nm;

            const char *self_name = "self";
            if (tok == TK_LPAREN) {
                next_token();
                if (tok == TK_IDENT) { self_name = strdup(tok_str); next_token(); }
                if (tok == TK_COLON) {
                    next_token();
                    int d = 0;
                    while (tok != TK_EOF) {
                        if (tok == TK_LPAREN) d++;
                        else if (tok == TK_RPAREN) { if (d == 0) break; d--; }
                        next_token();
                    }
                }
                expect(TK_RPAREN, "')'");
            }

            const char *m_names[64]; NODE *m_closures[64]; int nm = 0;
            const char *f_names[64]; NODE *f_inits[64]; int nf = 0;
            NODE *parent_expr = NULL;       // first `inherit` clause's expression
            NODE *initializer_body = NULL;  // last `initializer` clause's body
            while (tok != TK_END && tok != TK_EOF) {
                if (tok == TK_VAL) {
                    next_token();
                    if (tok == TK_MUTABLE) next_token();
                    if (tok != TK_IDENT) parse_fail("expected field name after 'val'");
                    const char *fname = strdup(tok_str); next_token();
                    if (tok == TK_COLON) {
                        next_token();
                        int d = 0;
                        while (tok != TK_EQ && tok != TK_EOF) {
                            if (tok == TK_LPAREN) d++;
                            else if (tok == TK_RPAREN) { if (d > 0) d--; else break; }
                            if (tok == TK_EQ && d == 0) break;
                            next_token();
                        }
                    }
                    expect(TK_EQ, "'='");
                    if (ncp > 0) push_scope_n(ncp, cparams);
                    NODE *init = parse_expr();
                    if (ncp > 0) pop_scope();
                    f_names[nf] = fname; f_inits[nf] = init; nf++;
                }
                else if (tok == TK_METHOD) {
                    next_token();
                    if (tok == TK_PRIVATE) next_token();
                    if (tok != TK_IDENT) parse_fail("expected method name");
                    const char *mname = strdup(tok_str); next_token();
                    const char *mparams[MAX_SCOPE_SLOTS];
                    int nmp = 0;
                    mparams[nmp++] = self_name;
                    while (tok == TK_IDENT || tok == TK_UNDER || tok == TK_LPAREN || tok == TK_TILDE || tok == TK_QMARK) {
                        if (tok == TK_LPAREN) {
                            int sp = src_pos, sl = src_line, st = tok;
                            char ss[1024]; strcpy(ss, tok_str);
                            next_token();
                            if (tok == TK_RPAREN) { next_token(); mparams[nmp++] = "_"; continue; }
                            if (tok == TK_IDENT) {
                                const char *pn = strdup(tok_str);
                                next_token();
                                if (tok == TK_COLON) {
                                    next_token();
                                    int d = 0;
                                    while (tok != TK_EOF) {
                                        if (tok == TK_LPAREN) d++;
                                        else if (tok == TK_RPAREN) { if (d == 0) break; d--; }
                                        next_token();
                                    }
                                    if (tok == TK_RPAREN) next_token();
                                    mparams[nmp++] = pn; continue;
                                }
                            }
                            src_pos = sp; src_line = sl; tok = st; strcpy(tok_str, ss);
                            break;
                        }
                        if (tok == TK_TILDE || tok == TK_QMARK) {
                            next_token();
                            if (tok != TK_IDENT) parse_fail("expected name");
                            mparams[nmp++] = strdup(tok_str); next_token();
                            if (tok == TK_COLON) {
                                next_token();
                                if (atom_starts(tok)) parse_atom();
                            }
                            continue;
                        }
                        mparams[nmp++] = (tok == TK_UNDER) ? "_" : strdup(tok_str);
                        next_token();
                    }
                    if (tok == TK_COLON) {
                        next_token();
                        int d = 0;
                        while (tok != TK_EQ && tok != TK_EOF) {
                            if (tok == TK_LPAREN) d++;
                            else if (tok == TK_RPAREN) { if (d > 0) d--; else break; }
                            if (tok == TK_EQ && d == 0) break;
                            next_token();
                        }
                    }
                    expect(TK_EQ, "'='");
                    if (ncp > 0) push_scope_n(ncp, cparams);
                    push_scope_n(nmp, mparams);
                    NODE *body = parse_expr();
                    pop_scope();
                    if (ncp > 0) pop_scope();
                    NODE *closure = (mark_tail_calls(body), make_fun((uint32_t)nmp, body));
                    m_names[nm] = mname; m_closures[nm] = closure; nm++;
                }
                else if (tok == TK_INHERIT) {
                    next_token();
                    // Parse `IDENT [args...]`: a class application.  We
                    // emit it as parse_expr_no_seq so the parent class
                    // construction (typically `Parent x y`) is captured.
                    if (ncp > 0) push_scope_n(ncp, cparams);
                    NODE *pe = parse_expr_no_seq();
                    if (ncp > 0) pop_scope();
                    parent_expr = pe;     // last inherit wins (only one supported)
                }
                else if (tok == TK_INITIALIZER) {
                    next_token();
                    if (ncp > 0) push_scope_n(ncp, cparams);
                    const char *self_only[1] = { self_name };
                    push_scope_n(1, self_only);
                    NODE *ib = parse_expr();
                    pop_scope();
                    if (ncp > 0) pop_scope();
                    initializer_body = ib;
                }
                else if (tok == TK_DSEMI) next_token();
                else parse_fail("unexpected token in class body: %d", tok);
            }
            expect(TK_END, "'end'");
            cur_class_fields = saved_class_fields;
            cur_class_fields_n = saved_class_fields_n;
            cur_class_methods = saved_class_methods;
            cur_class_methods_n = saved_class_methods_n;

            NODE *methods_list = ALLOC_node_const_nil();
            for (int i = nm - 1; i >= 0; i--) {
                NODE *args[2] = { ALLOC_node_const_str(m_names[i]), m_closures[i] };
                uint32_t aidx = stash_call_args(args, 2);
                NODE *pair = ALLOC_node_tuple_n(aidx, 2);
                methods_list = ALLOC_node_cons(pair, methods_list);
            }
            NODE *fields_list = ALLOC_node_const_nil();
            for (int i = nf - 1; i >= 0; i--) {
                NODE *args[2] = { ALLOC_node_const_str(f_names[i]), f_inits[i] };
                uint32_t aidx = stash_call_args(args, 2);
                NODE *pair = ALLOC_node_tuple_n(aidx, 2);
                fields_list = ALLOC_node_cons(pair, fields_list);
            }
            NODE *build;
            if (parent_expr) {
                NODE *call_args[3] = { methods_list, fields_list, parent_expr };
                build = make_app(ALLOC_node_gref(strdup("__class_build_with_parent")), call_args, 3);
            }
            else {
                NODE *call_args[2] = { methods_list, fields_list };
                build = make_app(ALLOC_node_gref(strdup("__class_build")), call_args, 2);
            }
            if (initializer_body) {
                // Wrap as `let self = build in (initializer_body; self)`
                // so init code can reference `self` at lref(0, 0).
                NODE *seq_inner = ALLOC_node_seq(initializer_body, ALLOC_node_lref(0, 0));
                build = ALLOC_node_let(build, seq_inner);
            }
            NODE *ctor;
            if (ncp == 0) ctor = (mark_tail_calls(build), make_fun(1, build));
            else          ctor = (mark_tail_calls(build), make_fun((uint32_t)ncp, build));
            VALUE cv = EVAL(c, ctor);
            oc_global_define(c, make_global_name(cname), cv);
            if (tok == TK_DSEMI) next_token();
            continue;
        }
        if (tok == TK_EXCEPTION_KW) {
            // `exception E [of ty]` — register E as a known variant constructor.
            // For our dynamic typing, we just need the name; nothing else
            // to do because constructors are looked up by name lazily.
            next_token();
            if (tok == TK_IDENT) next_token();
            skip_type_decl();
            continue;
        }
        if (tok == TK_LET) {
            next_token();
            bool is_rec = (tok == TK_REC);
            if (is_rec) next_token();

            // Collect all bindings (handling `and`).
            struct let_binding *bindings[64];
            int nb = 0;
            // Pre-define globals for is_rec so all values can reference each other.
            // First parse all bindings without their values to know names.
            // But values come immediately after each binding's `=`.  So we
            // parse one at a time, registering globals lazily.
            //
            // For simple correctness with mutual recursion at top level:
            //   1. For each binding, pre-define the global name with OC_UNIT.
            //   2. Parse the value (which may reference the global).
            //   3. Update the global.
            // When the second binding's body references the first, gref
            // resolves correctly.

            struct let_binding *lb = parse_let_binding();
            if (lb->name && is_rec) oc_global_define(c, make_global_name(lb->name), OC_UNIT);
            if (lb->np > 0) {
                push_scope_n(lb->np, lb->params);
                NODE *body = parse_expr();
                pop_scope();
                lb->value = (mark_tail_calls(body), make_fun((uint32_t)lb->np, body));
            }
            else {
                lb->value = parse_expr();
            }
            bindings[nb++] = lb;

            while (is_rec && tok == TK_KW_AND) {
                next_token();
                struct let_binding *lb2 = parse_let_binding();
                if (lb2->name) oc_global_define(c, make_global_name(lb2->name), OC_UNIT);
                if (lb2->np > 0) {
                    push_scope_n(lb2->np, lb2->params);
                    NODE *body = parse_expr();
                    pop_scope();
                    lb2->value = (mark_tail_calls(body), make_fun((uint32_t)lb2->np, body));
                }
                else {
                    lb2->value = parse_expr();
                }
                bindings[nb++] = lb2;
            }

            if (tok == TK_IN) {
                // Expression form: `let .. in cont`.
                next_token();
                const char *bnames[64];
                for (int i = 0; i < nb; i++) bnames[i] = bindings[i]->name ? bindings[i]->name : "_";
                push_scope_n(nb, bnames);
                NODE *cont = parse_expr();
                pop_scope();
                NODE *expr;
                if (nb == 1) {
                    expr = is_rec ? ALLOC_node_letrec(bindings[0]->value, cont)
                                   : ALLOC_node_let(bindings[0]->value, cont);
                }
                else {
                    NODE *vals[64];
                    for (int i = 0; i < nb; i++) vals[i] = bindings[i]->value;
                    uint32_t vidx = stash_letrec_values(vals, nb);
                    expr = make_letrec_n((uint32_t)nb, vidx, cont);
                }
                type_check_top(expr); maybe_aot_compile(expr);
                VALUE v = EVAL(c, expr);
                if (!OPTION.quiet) { fputs("- : ", stdout); oc_display(stdout, v); fputc('\n', stdout); }
            }
            else {
                // Top-level binding(s).
                for (int i = 0; i < nb; i++) {
                    struct let_binding *b = bindings[i];
                    if (!b->name) {
                        // Pattern binding at top-level — destructure into globals.
                        VALUE v = EVAL(c, b->value);
                        const char *names[MAX_SCOPE_SLOTS]; int nv = 0;
                        pat_collect_vars(b->pat, names, &nv);
                        // Re-extract: we need to walk pattern and assign vars.
                        // Use a simple recursive helper.
                        // Bind each var to scrut sub-value at runtime.
                        // For now, just re-evaluate via temp let chain at AST level.
                        if (nv > 0) {
                            // Create a temp global "__top_scrut__".
                            oc_global_define(c, "__top_scrut__", v);
                            for (int k = 0; k < nv; k++) {
                                // Build extract for k-th var by re-walking.
                                // Easiest: emit and evaluate.
                                struct scrut_lref_ctx *bctx = malloc(sizeof *bctx);
                                bctx->depth = 0; bctx->slot = 0;
                                NODE *extracts[MAX_SCOPE_SLOTS]; int ei = 0;
                                pat_gen_extracts(b->pat, scrut_fac_lref, bctx, extracts, &ei);
                                // Evaluate inside a temp frame containing v.
                                struct oframe *f = oc_new_frame(c->env, 1);
                                f->slots[0] = v;
                                struct oframe *saved = c->env;
                                c->env = f;
                                VALUE vk = EVAL(c, extracts[k]);
                                c->env = saved;
                                oc_global_define(c, make_global_name(names[k]), vk);
                            }
                        }
                        continue;
                    }
                    type_check_top_define(b->value, b->name); maybe_aot_compile(b->value);
                    VALUE v = EVAL(c, b->value);
                    if (strcmp(b->name, "_") != 0) {
                        oc_global_define(c, make_global_name(b->name), v);
                    }
                    if (!OPTION.quiet) {
                        fprintf(stdout, "val %s = ", b->name); oc_display(stdout, v); fputc('\n', stdout);
                    }
                }
            }
            if (tok == TK_DSEMI) next_token();
            continue;
        }
        // Top-level expression.
        NODE *e = parse_expr();
        type_check_top(e); maybe_aot_compile(e);
        VALUE v = EVAL(c, e);
        if (!OPTION.quiet) { fputs("- : ", stdout); oc_display(stdout, v); fputc('\n', stdout); }
        if (tok == TK_DSEMI) next_token();
    }
}

// ---------------------------------------------------------------------------
// Built-in primitives.
// ---------------------------------------------------------------------------

static VALUE prim_print_int(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    fprintf(stdout, "%lld", (long long)OC_INT_VAL(argv[0]));
    return OC_UNIT;
}
static VALUE prim_print_string(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (OC_IS_STRING(argv[0])) {
        struct oobj *s = OC_PTR(argv[0]);
        fwrite(s->str.chars, 1, s->str.len, stdout);
    }
    return OC_UNIT;
}
static VALUE prim_print_endline(CTX *c, int argc, VALUE *argv) {
    prim_print_string(c, argc, argv);
    fputc('\n', stdout);
    return OC_UNIT;
}
static VALUE prim_print_newline(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    fputc('\n', stdout);
    return OC_UNIT;
}
static VALUE prim_print_char(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    fputc((int)(OC_INT_VAL(argv[0]) & 0xff), stdout);
    return OC_UNIT;
}
static VALUE prim_print_float(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    fprintf(stdout, "%g", oc_get_float(argv[0]));
    return OC_UNIT;
}
static VALUE prim_string_of_int(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)OC_INT_VAL(argv[0]));
    return oc_make_string(buf, (size_t)n);
}
static VALUE prim_int_of_string(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("int_of_string", 13)}));
    return OC_INT(strtoll(OC_PTR(argv[0])->str.chars, NULL, 10));
}
static VALUE prim_string_of_float(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%g", oc_get_float(argv[0]));
    return oc_make_string(buf, (size_t)n);
}
static VALUE prim_float_of_int(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    return oc_make_float((double)OC_INT_VAL(argv[0]));
}
static VALUE prim_int_of_float(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    return OC_INT((int64_t)oc_get_float(argv[0]));
}
static VALUE prim_float_of_string(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("float_of_string", 15)}));
    return oc_make_float(strtod(OC_PTR(argv[0])->str.chars, NULL));
}
static VALUE prim_ignore(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    return OC_UNIT;
}
static VALUE prim_failwith(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    oc_raise(c, oc_make_variant("Failure", 1, argv));
}
static VALUE prim_invalid_arg(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    oc_raise(c, oc_make_variant("Invalid_argument", 1, argv));
}
static VALUE prim_raise(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    oc_raise(c, argv[0]);
}
static VALUE prim_assert(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (argv[0] != OC_TRUE) oc_raise(c, oc_make_variant("Assert_failure", 0, NULL));
    return OC_UNIT;
}

// List builtins.
static VALUE prim_list_length(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    int64_t n = 0; VALUE v = argv[0];
    while (OC_IS_CONS(v)) { n++; v = OC_PTR(v)->cons.tail; }
    return OC_INT(n);
}
static VALUE prim_list_hd(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_CONS(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("hd", 2)}));
    return OC_PTR(argv[0])->cons.head;
}
static VALUE prim_list_tl(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_CONS(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("tl", 2)}));
    return OC_PTR(argv[0])->cons.tail;
}
static VALUE prim_list_rev(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE acc = OC_NIL, v = argv[0];
    while (OC_IS_CONS(v)) { acc = oc_cons(OC_PTR(v)->cons.head, acc); v = OC_PTR(v)->cons.tail; }
    return acc;
}
static VALUE prim_list_append(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE a = argv[0], b = argv[1];
    if (!OC_IS_CONS(a)) return b;
    // Build reversed list from a, then prepend onto b.
    VALUE buf[16384]; int n = 0;
    while (OC_IS_CONS(a) && n < 16384) { buf[n++] = OC_PTR(a)->cons.head; a = OC_PTR(a)->cons.tail; }
    VALUE acc = b;
    for (int i = n - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    return acc;
}
static VALUE prim_list_map(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE v = argv[1];
    VALUE buf[16384]; int n = 0;
    while (OC_IS_CONS(v) && n < 16384) {
        VALUE h = OC_PTR(v)->cons.head;
        buf[n++] = oc_apply(c, fn, 1, &h);
        v = OC_PTR(v)->cons.tail;
    }
    VALUE acc = OC_NIL;
    for (int i = n - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    return acc;
}
static VALUE prim_list_filter(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE v = argv[1];
    VALUE buf[16384]; int n = 0;
    while (OC_IS_CONS(v) && n < 16384) {
        VALUE h = OC_PTR(v)->cons.head;
        VALUE r = oc_apply(c, fn, 1, &h);
        if (r == OC_TRUE) buf[n++] = h;
        v = OC_PTR(v)->cons.tail;
    }
    VALUE acc = OC_NIL;
    for (int i = n - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    return acc;
}
static VALUE prim_list_fold_left(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE acc = argv[1]; VALUE v = argv[2];
    while (OC_IS_CONS(v)) {
        VALUE pair[2] = { acc, OC_PTR(v)->cons.head };
        acc = oc_apply(c, fn, 2, pair);
        v = OC_PTR(v)->cons.tail;
    }
    return acc;
}
static VALUE prim_list_fold_right(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE v = argv[1]; VALUE acc = argv[2];
    VALUE buf[16384]; int n = 0;
    while (OC_IS_CONS(v) && n < 16384) {
        buf[n++] = OC_PTR(v)->cons.head;
        v = OC_PTR(v)->cons.tail;
    }
    for (int i = n - 1; i >= 0; i--) {
        VALUE pair[2] = { buf[i], acc };
        acc = oc_apply(c, fn, 2, pair);
    }
    return acc;
}
static VALUE prim_list_iter(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE v = argv[1];
    while (OC_IS_CONS(v)) {
        VALUE h = OC_PTR(v)->cons.head;
        oc_apply(c, fn, 1, &h);
        v = OC_PTR(v)->cons.tail;
    }
    return OC_UNIT;
}
static VALUE prim_list_nth(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE v = argv[0]; int64_t k = OC_INT_VAL(argv[1]);
    while (k > 0 && OC_IS_CONS(v)) { v = OC_PTR(v)->cons.tail; k--; }
    if (!OC_IS_CONS(v)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("nth", 3)}));
    return OC_PTR(v)->cons.head;
}
static VALUE prim_list_mem(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE x = argv[0]; VALUE v = argv[1];
    while (OC_IS_CONS(v)) {
        if (oc_structural_eq(x, OC_PTR(v)->cons.head)) return OC_TRUE;
        v = OC_PTR(v)->cons.tail;
    }
    return OC_FALSE;
}

// Find a key in an association list of (k, v) tuples.
static VALUE prim_list_assoc(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE k = argv[0]; VALUE v = argv[1];
    while (OC_IS_CONS(v)) {
        VALUE pair = OC_PTR(v)->cons.head;
        if (OC_IS_TUPLE(pair) && OC_PTR(pair)->tup.n >= 2 &&
            oc_structural_eq(OC_PTR(pair)->tup.items[0], k))
            return OC_PTR(pair)->tup.items[1];
        v = OC_PTR(v)->cons.tail;
    }
    oc_raise(c, oc_make_variant("Not_found", 0, NULL));
}
static VALUE prim_list_assoc_opt(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE k = argv[0]; VALUE v = argv[1];
    while (OC_IS_CONS(v)) {
        VALUE pair = OC_PTR(v)->cons.head;
        if (OC_IS_TUPLE(pair) && OC_PTR(pair)->tup.n >= 2 &&
            oc_structural_eq(OC_PTR(pair)->tup.items[0], k))
            return oc_make_variant("Some", 1, (VALUE[]){OC_PTR(pair)->tup.items[1]});
        v = OC_PTR(v)->cons.tail;
    }
    return oc_make_variant("None", 0, NULL);
}
static VALUE prim_list_mem_assoc(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE k = argv[0]; VALUE v = argv[1];
    while (OC_IS_CONS(v)) {
        VALUE pair = OC_PTR(v)->cons.head;
        if (OC_IS_TUPLE(pair) && OC_PTR(pair)->tup.n >= 2 &&
            oc_structural_eq(OC_PTR(pair)->tup.items[0], k))
            return OC_TRUE;
        v = OC_PTR(v)->cons.tail;
    }
    return OC_FALSE;
}
static VALUE prim_list_exists(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE v = argv[1];
    while (OC_IS_CONS(v)) {
        VALUE h = OC_PTR(v)->cons.head;
        if (oc_apply(c, fn, 1, &h) == OC_TRUE) return OC_TRUE;
        v = OC_PTR(v)->cons.tail;
    }
    return OC_FALSE;
}
static VALUE prim_list_for_all(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE v = argv[1];
    while (OC_IS_CONS(v)) {
        VALUE h = OC_PTR(v)->cons.head;
        if (oc_apply(c, fn, 1, &h) != OC_TRUE) return OC_FALSE;
        v = OC_PTR(v)->cons.tail;
    }
    return OC_TRUE;
}
static VALUE prim_list_find(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE v = argv[1];
    while (OC_IS_CONS(v)) {
        VALUE h = OC_PTR(v)->cons.head;
        if (oc_apply(c, fn, 1, &h) == OC_TRUE) return h;
        v = OC_PTR(v)->cons.tail;
    }
    oc_raise(c, oc_make_variant("Not_found", 0, NULL));
}
static VALUE prim_list_find_opt(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE v = argv[1];
    while (OC_IS_CONS(v)) {
        VALUE h = OC_PTR(v)->cons.head;
        if (oc_apply(c, fn, 1, &h) == OC_TRUE)
            return oc_make_variant("Some", 1, &h);
        v = OC_PTR(v)->cons.tail;
    }
    return oc_make_variant("None", 0, NULL);
}
static VALUE prim_list_init(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    int64_t n = OC_INT_VAL(argv[0]);
    VALUE fn = argv[1];
    VALUE buf[16384]; int cnt = 0;
    for (int64_t i = 0; i < n && cnt < 16384; i++) {
        VALUE iv = OC_INT(i);
        buf[cnt++] = oc_apply(c, fn, 1, &iv);
    }
    VALUE acc = OC_NIL;
    for (int i = cnt - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    return acc;
}
static VALUE prim_list_flatten(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE buf[16384]; int n = 0;
    for (VALUE outer = argv[0]; OC_IS_CONS(outer); outer = OC_PTR(outer)->cons.tail) {
        VALUE inner = OC_PTR(outer)->cons.head;
        for (VALUE l = inner; OC_IS_CONS(l) && n < 16384; l = OC_PTR(l)->cons.tail)
            buf[n++] = OC_PTR(l)->cons.head;
    }
    VALUE acc = OC_NIL;
    for (int i = n - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    return acc;
}
static VALUE prim_list_combine(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE a = argv[0]; VALUE b = argv[1];
    VALUE buf[16384]; int n = 0;
    while (OC_IS_CONS(a) && OC_IS_CONS(b) && n < 16384) {
        VALUE pair = oc_make_tuple(2, (VALUE[]){OC_PTR(a)->cons.head, OC_PTR(b)->cons.head});
        buf[n++] = pair;
        a = OC_PTR(a)->cons.tail;
        b = OC_PTR(b)->cons.tail;
    }
    if (a != OC_NIL || b != OC_NIL) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("List.combine", 12)}));
    VALUE acc = OC_NIL;
    for (int i = n - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    return acc;
}
static VALUE prim_list_split(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE buf_a[16384]; VALUE buf_b[16384]; int n = 0;
    for (VALUE l = argv[0]; OC_IS_CONS(l) && n < 16384; l = OC_PTR(l)->cons.tail) {
        VALUE pair = OC_PTR(l)->cons.head;
        if (!OC_IS_TUPLE(pair) || OC_PTR(pair)->tup.n < 2) break;
        buf_a[n] = OC_PTR(pair)->tup.items[0];
        buf_b[n] = OC_PTR(pair)->tup.items[1];
        n++;
    }
    VALUE la = OC_NIL, lb = OC_NIL;
    for (int i = n - 1; i >= 0; i--) { la = oc_cons(buf_a[i], la); lb = oc_cons(buf_b[i], lb); }
    return oc_make_tuple(2, (VALUE[]){la, lb});
}
static VALUE prim_list_partition(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE v = argv[1];
    VALUE buf_t[16384], buf_f[16384]; int nt = 0, nf = 0;
    for (VALUE l = v; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
        VALUE h = OC_PTR(l)->cons.head;
        if (oc_apply(c, fn, 1, &h) == OC_TRUE) { if (nt < 16384) buf_t[nt++] = h; }
        else                                   { if (nf < 16384) buf_f[nf++] = h; }
    }
    VALUE lt = OC_NIL, lf = OC_NIL;
    for (int i = nt - 1; i >= 0; i--) lt = oc_cons(buf_t[i], lt);
    for (int i = nf - 1; i >= 0; i--) lf = oc_cons(buf_f[i], lf);
    return oc_make_tuple(2, (VALUE[]){lt, lf});
}
// merge sort
static void mergesort_impl(VALUE *buf, VALUE *tmp, int lo, int hi, VALUE cmp, CTX *c) {
    if (hi - lo <= 1) return;
    int mid = (lo + hi) / 2;
    mergesort_impl(buf, tmp, lo, mid, cmp, c);
    mergesort_impl(buf, tmp, mid, hi, cmp, c);
    int i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        VALUE av[2] = { buf[i], buf[j] };
        VALUE r = oc_apply(c, cmp, 2, av);
        int sgn = (int)OC_INT_VAL(r);
        if (sgn <= 0) tmp[k++] = buf[i++];
        else          tmp[k++] = buf[j++];
    }
    while (i < mid) tmp[k++] = buf[i++];
    while (j < hi) tmp[k++] = buf[j++];
    for (int q = lo; q < hi; q++) buf[q] = tmp[q];
}
static VALUE prim_list_sort(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE cmp = argv[0]; VALUE v = argv[1];
    int n = 0;
    static VALUE buf[65536], tmp[65536];
    for (VALUE l = v; OC_IS_CONS(l) && n < 65536; l = OC_PTR(l)->cons.tail)
        buf[n++] = OC_PTR(l)->cons.head;
    mergesort_impl(buf, tmp, 0, n, cmp, c);
    VALUE acc = OC_NIL;
    for (int i = n - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    return acc;
}
static VALUE prim_list_iter2(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE a = argv[1]; VALUE b = argv[2];
    while (OC_IS_CONS(a) && OC_IS_CONS(b)) {
        VALUE av[2] = { OC_PTR(a)->cons.head, OC_PTR(b)->cons.head };
        oc_apply(c, fn, 2, av);
        a = OC_PTR(a)->cons.tail;
        b = OC_PTR(b)->cons.tail;
    }
    return OC_UNIT;
}
static VALUE prim_list_map2(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE a = argv[1]; VALUE b = argv[2];
    VALUE buf[16384]; int n = 0;
    while (OC_IS_CONS(a) && OC_IS_CONS(b) && n < 16384) {
        VALUE av[2] = { OC_PTR(a)->cons.head, OC_PTR(b)->cons.head };
        buf[n++] = oc_apply(c, fn, 2, av);
        a = OC_PTR(a)->cons.tail;
        b = OC_PTR(b)->cons.tail;
    }
    VALUE acc = OC_NIL;
    for (int i = n - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    return acc;
}

// String builtins.
static VALUE prim_string_length(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("string_length", 13)}));
    return OC_INT((int64_t)OC_PTR(argv[0])->str.len);
}
static VALUE prim_string_get(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("string_get", 10)}));
    int64_t i = OC_INT_VAL(argv[1]);
    struct oobj *s = OC_PTR(argv[0]);
    if (i < 0 || (size_t)i >= s->str.len) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("index", 5)}));
    return OC_INT((unsigned char)s->str.chars[i]);
}
static VALUE prim_string_sub(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("sub", 3)}));
    int64_t s = OC_INT_VAL(argv[1]);
    int64_t l = OC_INT_VAL(argv[2]);
    struct oobj *o = OC_PTR(argv[0]);
    if (s < 0 || l < 0 || (size_t)(s + l) > o->str.len) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("sub", 3)}));
    return oc_make_string(o->str.chars + s, (size_t)l);
}
static VALUE prim_string_concat(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    // OCaml's String.concat sep list — joins list of strings with sep.
    // Detect: if argv[1] is a list, use sep+list semantics.  Otherwise
    // fall back to two-string concat (legacy `String.concat a b`).
    if (argc == 2 && OC_IS_STRING(argv[0]) && (OC_IS_CONS(argv[1]) || argv[1] == OC_NIL)) {
        const char *sep_chars = OC_PTR(argv[0])->str.chars;
        size_t sep_len = OC_PTR(argv[0])->str.len;
        // Compute total length.
        size_t total = 0; int n = 0;
        for (VALUE l = argv[1]; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
            VALUE s = OC_PTR(l)->cons.head;
            if (!OC_IS_STRING(s)) return oc_make_string("", 0);
            total += OC_PTR(s)->str.len;
            n++;
        }
        if (n > 1) total += sep_len * (n - 1);
        char *out = (char *)malloc(total + 1);
        size_t pos = 0; int idx = 0;
        for (VALUE l = argv[1]; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
            VALUE s = OC_PTR(l)->cons.head;
            if (idx > 0) { memcpy(out + pos, sep_chars, sep_len); pos += sep_len; }
            memcpy(out + pos, OC_PTR(s)->str.chars, OC_PTR(s)->str.len);
            pos += OC_PTR(s)->str.len;
            idx++;
        }
        out[total] = '\0';
        VALUE r = oc_make_string(out, total);
        free(out);
        return r;
    }
    return oc_string_concat(argv[0], argv[1]);
}
static VALUE prim_string_make(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    int64_t n = OC_INT_VAL(argv[0]);
    if (n < 0) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("String.make", 11)}));
    char ch = (char)(OC_INT_VAL(argv[1]) & 0xff);
    char *buf = (char *)malloc((size_t)n + 1);
    memset(buf, ch, n); buf[n] = '\0';
    VALUE r = oc_make_string(buf, (size_t)n);
    free(buf);
    return r;
}
static VALUE prim_string_contains(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_STRING(argv[0])) return OC_FALSE;
    char ch = (char)(OC_INT_VAL(argv[1]) & 0xff);
    struct oobj *s = OC_PTR(argv[0]);
    return memchr(s->str.chars, ch, s->str.len) ? OC_TRUE : OC_FALSE;
}
static VALUE prim_string_index(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Not_found", 0, NULL));
    char ch = (char)(OC_INT_VAL(argv[1]) & 0xff);
    struct oobj *s = OC_PTR(argv[0]);
    char *p = (char *)memchr(s->str.chars, ch, s->str.len);
    if (!p) oc_raise(c, oc_make_variant("Not_found", 0, NULL));
    return OC_INT(p - s->str.chars);
}
static VALUE prim_string_uppercase(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_STRING(argv[0])) return argv[0];
    struct oobj *s = OC_PTR(argv[0]);
    char *buf = (char *)malloc(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    VALUE r = oc_make_string(buf, s->str.len);
    free(buf);
    return r;
}
static VALUE prim_string_lowercase(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_STRING(argv[0])) return argv[0];
    struct oobj *s = OC_PTR(argv[0]);
    char *buf = (char *)malloc(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    VALUE r = oc_make_string(buf, s->str.len);
    free(buf);
    return r;
}
static VALUE prim_bytes_blit(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_BYTES(argv[0]) || !OC_IS_BYTES(argv[2])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Bytes.blit", 10)}));
    int64_t sp = OC_INT_VAL(argv[1]);
    int64_t dp = OC_INT_VAL(argv[3]);
    int64_t n  = OC_INT_VAL(argv[4]);
    struct oobj *src = OC_PTR(argv[0]);
    struct oobj *dst = OC_PTR(argv[2]);
    if (sp < 0 || dp < 0 || n < 0 ||
        (size_t)(sp + n) > src->bytes.len || (size_t)(dp + n) > dst->bytes.len)
        oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("Bytes.blit", 10)}));
    memmove(dst->bytes.bytes + dp, src->bytes.bytes + sp, (size_t)n);
    return OC_UNIT;
}
static VALUE prim_char_code(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    return argv[0];     // chars are ints already
}
static VALUE prim_char_chr(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    return argv[0];
}

// Array builtins.
static VALUE prim_array_make_n(CTX *c, int argc, VALUE *argv) {
    (void)c;
    return oc_make_array(argc, argv);
}
static VALUE prim_array_get(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_ARRAY(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Array.get", 9)}));
    int64_t i = OC_INT_VAL(argv[1]);
    struct oobj *a = OC_PTR(argv[0]);
    if (i < 0 || i >= a->arr.n) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("index", 5)}));
    return a->arr.items[i];
}
static VALUE prim_array_set(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_ARRAY(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Array.set", 9)}));
    int64_t i = OC_INT_VAL(argv[1]);
    struct oobj *a = OC_PTR(argv[0]);
    if (i < 0 || i >= a->arr.n) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("index", 5)}));
    a->arr.items[i] = argv[2];
    return OC_UNIT;
}
static VALUE prim_array_length(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_ARRAY(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("length", 6)}));
    return OC_INT(OC_PTR(argv[0])->arr.n);
}
static VALUE prim_array_make(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    int64_t n = OC_INT_VAL(argv[0]);
    if (n < 0) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("make", 4)}));
    VALUE *items = (VALUE *)malloc(sizeof(VALUE) * (n ? n : 1));
    for (int64_t i = 0; i < n; i++) items[i] = argv[1];
    VALUE r = oc_make_array((int)n, items);
    free(items);
    return r;
}

// Float math.
static VALUE prim_sqrt(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return oc_make_float(__builtin_sqrt(oc_get_float(argv[0]))); }
static VALUE prim_sin(CTX *c, int argc, VALUE *argv)  { (void)c; (void)argc; return oc_make_float(__builtin_sin(oc_get_float(argv[0]))); }
static VALUE prim_cos(CTX *c, int argc, VALUE *argv)  { (void)c; (void)argc; return oc_make_float(__builtin_cos(oc_get_float(argv[0]))); }
static VALUE prim_log(CTX *c, int argc, VALUE *argv)  { (void)c; (void)argc; return oc_make_float(__builtin_log(oc_get_float(argv[0]))); }
static VALUE prim_exp(CTX *c, int argc, VALUE *argv)  { (void)c; (void)argc; return oc_make_float(__builtin_exp(oc_get_float(argv[0]))); }
static VALUE prim_floor(CTX *c, int argc, VALUE *argv){ (void)c; (void)argc; return oc_make_float(__builtin_floor(oc_get_float(argv[0]))); }
static VALUE prim_ceil(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return oc_make_float(__builtin_ceil(oc_get_float(argv[0]))); }

// Compare, abs, min, max.
static VALUE prim_compare(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    return OC_INT(oc_compare(argv[0], argv[1]));
}
static VALUE prim_abs(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    int64_t v = OC_INT_VAL(argv[0]);
    return OC_INT(v < 0 ? -v : v);
}
static VALUE prim_min(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    return oc_compare(argv[0], argv[1]) <= 0 ? argv[0] : argv[1];
}
static VALUE prim_max(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    return oc_compare(argv[0], argv[1]) >= 0 ? argv[0] : argv[1];
}

// Ref operations exposed as functions too (for `ref x` syntax via prim).
static VALUE prim_ref(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    return oc_make_ref(argv[0]);
}

// Lazy.force.
static VALUE prim_lazy_force(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    return oc_force_lazy(c, argv[0]);
}
static VALUE prim_lazy_is_val(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_LAZY(argv[0])) return OC_TRUE;
    return OC_PTR(argv[0])->lazy.forced ? OC_TRUE : OC_FALSE;
}

// Bytes.
static VALUE prim_bytes_create(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    int64_t n = OC_INT_VAL(argv[0]);
    if (n < 0) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("Bytes.create", 12)}));
    return oc_make_bytes((size_t)n, '\0');
}
static VALUE prim_bytes_make(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    int64_t n = OC_INT_VAL(argv[0]);
    if (n < 0) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("Bytes.make", 10)}));
    return oc_make_bytes((size_t)n, (char)(OC_INT_VAL(argv[1]) & 0xff));
}
static VALUE prim_bytes_length(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_BYTES(argv[0])) return OC_INT(0);
    return OC_INT((int64_t)OC_PTR(argv[0])->bytes.len);
}
static VALUE prim_bytes_get(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_BYTES(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Bytes.get", 9)}));
    int64_t i = OC_INT_VAL(argv[1]);
    struct oobj *b = OC_PTR(argv[0]);
    if (i < 0 || (size_t)i >= b->bytes.len) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("index", 5)}));
    return OC_INT((unsigned char)b->bytes.bytes[i]);
}
static VALUE prim_bytes_set(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_BYTES(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Bytes.set", 9)}));
    int64_t i = OC_INT_VAL(argv[1]);
    struct oobj *b = OC_PTR(argv[0]);
    if (i < 0 || (size_t)i >= b->bytes.len) oc_raise(c, oc_make_variant("Invalid_argument", 1, (VALUE[]){oc_make_string("index", 5)}));
    b->bytes.bytes[i] = (char)(OC_INT_VAL(argv[2]) & 0xff);
    return OC_UNIT;
}
static VALUE prim_bytes_to_string(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_BYTES(argv[0])) return oc_make_string("", 0);
    struct oobj *b = OC_PTR(argv[0]);
    return oc_make_string(b->bytes.bytes, b->bytes.len);
}
static VALUE prim_bytes_of_string(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_STRING(argv[0])) return oc_make_bytes(0, '\0');
    struct oobj *s = OC_PTR(argv[0]);
    VALUE r = oc_make_bytes(s->str.len, '\0');
    memcpy(OC_PTR(r)->bytes.bytes, s->str.chars, s->str.len);
    return r;
}

// ---------------------------------------------------------------------------
// Printf — minimal format-string runtime.  Supports %d %s %f %c %b %% and
// width specifiers (%5d, %-5d, %.3f).  No partial application yet — call
// site must supply all args at once: `Printf.printf "..." a b c`.
// ---------------------------------------------------------------------------
static VALUE
oc_printf_impl(CTX *c, FILE *fp, const char *fmt, int argc, VALUE *argv,
               char *out_buf, size_t out_buf_size, size_t *out_used)
{
    int ai = 0;
    char piece[64];
    size_t used = 0;
    while (*fmt) {
        if (*fmt != '%') {
            if (fp) fputc(*fmt, fp);
            else if (used + 1 < out_buf_size) out_buf[used++] = *fmt;
            fmt++;
            continue;
        }
        // Parse % spec.
        const char *spec_start = fmt;
        fmt++;
        // flags + width + precision
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '0' || *fmt == '#') fmt++;
        while (*fmt >= '0' && *fmt <= '9') fmt++;
        if (*fmt == '.') { fmt++; while (*fmt >= '0' && *fmt <= '9') fmt++; }
        char conv = *fmt++;
        size_t spec_len = (size_t)(fmt - spec_start);
        char specbuf[32];
        if (spec_len >= sizeof specbuf) spec_len = sizeof specbuf - 1;
        memcpy(specbuf, spec_start, spec_len);
        specbuf[spec_len] = '\0';
        if (conv == '%') {
            if (fp) fputc('%', fp);
            else if (used + 1 < out_buf_size) out_buf[used++] = '%';
            continue;
        }
        if (ai >= argc) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("printf: not enough args", 23)}));
        VALUE v = argv[ai++];
        int n = 0;
        switch (conv) {
        case 'd': case 'i':
            n = snprintf(piece, sizeof piece, specbuf, (long long)OC_INT_VAL(v));
            break;
        case 'u': case 'x': case 'X': case 'o':
            n = snprintf(piece, sizeof piece, specbuf, (unsigned long long)OC_INT_VAL(v));
            break;
        case 'f': case 'g': case 'e': case 'E': case 'G':
            n = snprintf(piece, sizeof piece, specbuf, oc_get_float(v));
            break;
        case 'c':
            n = snprintf(piece, sizeof piece, "%c", (int)(OC_INT_VAL(v) & 0xff));
            break;
        case 'b':
            n = snprintf(piece, sizeof piece, "%s", (v == OC_TRUE) ? "true" : "false");
            break;
        case 's': {
            const char *s = OC_IS_STRING(v) ? OC_PTR(v)->str.chars : "";
            n = snprintf(piece, sizeof piece, specbuf, s);
            break;
        }
        default:
            n = snprintf(piece, sizeof piece, "<?>");
        }
        if (n < 0) n = 0;
        if (n >= (int)sizeof piece) n = sizeof piece - 1;
        if (fp) fwrite(piece, 1, n, fp);
        else for (int k = 0; k < n && used + 1 < out_buf_size; k++) out_buf[used++] = piece[k];
    }
    if (!fp) {
        if (used < out_buf_size) out_buf[used] = '\0';
        if (out_used) *out_used = used;
    }
    return OC_UNIT;
}

static VALUE prim_printf(CTX *c, int argc, VALUE *argv) {
    if (argc < 1) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("printf: missing format", 22)}));
    if (!OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("printf: format not a string", 27)}));
    return oc_printf_impl(c, stdout, OC_PTR(argv[0])->str.chars, argc - 1, argv + 1, NULL, 0, NULL);
}
static VALUE prim_eprintf(CTX *c, int argc, VALUE *argv) {
    if (argc < 1 || !OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("eprintf", 7)}));
    return oc_printf_impl(c, stderr, OC_PTR(argv[0])->str.chars, argc - 1, argv + 1, NULL, 0, NULL);
}
static VALUE prim_sprintf(CTX *c, int argc, VALUE *argv) {
    if (argc < 1 || !OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("sprintf", 7)}));
    char buf[4096];
    size_t used = 0;
    oc_printf_impl(c, NULL, OC_PTR(argv[0])->str.chars, argc - 1, argv + 1, buf, sizeof buf, &used);
    return oc_make_string(buf, used);
}

// Built-in infix operators as standalone function values (used as e.g.
// `(+) 1 2`).  These mirror the AST node operations.
static VALUE prim_op_add(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return OC_INT(OC_INT_VAL(argv[0]) + OC_INT_VAL(argv[1])); }
static VALUE prim_op_sub(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return OC_INT(OC_INT_VAL(argv[0]) - OC_INT_VAL(argv[1])); }
static VALUE prim_op_mul(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return OC_INT(OC_INT_VAL(argv[0]) * OC_INT_VAL(argv[1])); }
static VALUE prim_op_div(CTX *c, int argc, VALUE *argv) { (void)argc;
    int64_t b = OC_INT_VAL(argv[1]);
    if (b == 0) oc_raise(c, oc_make_variant("Division_by_zero", 0, NULL));
    return OC_INT(OC_INT_VAL(argv[0]) / b);
}
static VALUE prim_op_mod(CTX *c, int argc, VALUE *argv) { (void)argc;
    int64_t b = OC_INT_VAL(argv[1]);
    if (b == 0) oc_raise(c, oc_make_variant("Division_by_zero", 0, NULL));
    return OC_INT(OC_INT_VAL(argv[0]) % b);
}
static VALUE prim_op_lt(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_compare(argv[0], argv[1]) <  0 ? OC_TRUE : OC_FALSE; }
static VALUE prim_op_le(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_compare(argv[0], argv[1]) <= 0 ? OC_TRUE : OC_FALSE; }
static VALUE prim_op_gt(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_compare(argv[0], argv[1]) >  0 ? OC_TRUE : OC_FALSE; }
static VALUE prim_op_ge(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_compare(argv[0], argv[1]) >= 0 ? OC_TRUE : OC_FALSE; }
static VALUE prim_op_eq(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_structural_eq(argv[0], argv[1]) ? OC_TRUE : OC_FALSE; }
static VALUE prim_op_ne(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_structural_eq(argv[0], argv[1]) ? OC_FALSE : OC_TRUE; }
static VALUE prim_op_concat(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_string_concat(argv[0], argv[1]); }
static VALUE prim_op_cons(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_cons(argv[0], argv[1]); }
static VALUE prim_op_fadd(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_make_float(oc_get_float(argv[0]) + oc_get_float(argv[1])); }
static VALUE prim_op_fsub(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_make_float(oc_get_float(argv[0]) - oc_get_float(argv[1])); }
static VALUE prim_op_fmul(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_make_float(oc_get_float(argv[0]) * oc_get_float(argv[1])); }
static VALUE prim_op_fdiv(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc; return oc_make_float(oc_get_float(argv[0]) / oc_get_float(argv[1])); }

// ---------------------------------------------------------------------------
// Hashtbl / Buffer / Stack / Queue / Lists.assoc — simple implementations
// using refs and lists.  Internal representation:
//   Hashtbl: ref to a list of (key, value) tuples
//   Stack:   ref to a list (head = top)
//   Queue:   ref to a list (head = front)
//   Buffer:  ref to a list of strings (concat on contents)
// ---------------------------------------------------------------------------

static VALUE prim_hash_create(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc;(void)argv; return oc_make_ref(OC_NIL); }
static VALUE prim_hash_add(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    VALUE h = argv[0]; VALUE k = argv[1]; VALUE v = argv[2];
    if (!OC_IS_REF(h)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Hashtbl.add", 11)}));
    VALUE pair = oc_make_tuple(2, (VALUE[]){k, v});
    OC_PTR(h)->refval = oc_cons(pair, OC_PTR(h)->refval);
    return OC_UNIT;
}
static VALUE prim_hash_find(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE h = argv[0]; VALUE k = argv[1];
    if (!OC_IS_REF(h)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Hashtbl.find", 12)}));
    for (VALUE l = OC_PTR(h)->refval; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
        VALUE pair = OC_PTR(l)->cons.head;
        if (OC_IS_TUPLE(pair) && OC_PTR(pair)->tup.n == 2 &&
            oc_structural_eq(OC_PTR(pair)->tup.items[0], k))
            return OC_PTR(pair)->tup.items[1];
    }
    oc_raise(c, oc_make_variant("Not_found", 0, NULL));
}
static VALUE prim_hash_mem(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    VALUE h = argv[0]; VALUE k = argv[1];
    if (!OC_IS_REF(h)) return OC_FALSE;
    for (VALUE l = OC_PTR(h)->refval; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
        VALUE pair = OC_PTR(l)->cons.head;
        if (OC_IS_TUPLE(pair) && OC_PTR(pair)->tup.n == 2 &&
            oc_structural_eq(OC_PTR(pair)->tup.items[0], k))
            return OC_TRUE;
    }
    return OC_FALSE;
}
static VALUE prim_hash_remove(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    VALUE h = argv[0]; VALUE k = argv[1];
    if (!OC_IS_REF(h)) return OC_UNIT;
    // Build new list without first match.
    VALUE buf[16384]; int n = 0; bool removed = false;
    for (VALUE l = OC_PTR(h)->refval; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
        VALUE pair = OC_PTR(l)->cons.head;
        if (!removed && OC_IS_TUPLE(pair) && OC_PTR(pair)->tup.n == 2 &&
            oc_structural_eq(OC_PTR(pair)->tup.items[0], k)) {
            removed = true; continue;
        }
        if (n < 16384) buf[n++] = pair;
    }
    VALUE acc = OC_NIL;
    for (int i = n - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    OC_PTR(h)->refval = acc;
    return OC_UNIT;
}
static VALUE prim_hash_length(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    if (!OC_IS_REF(argv[0])) return OC_INT(0);
    int64_t n = 0;
    for (VALUE l = OC_PTR(argv[0])->refval; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) n++;
    return OC_INT(n);
}
static VALUE prim_hash_iter(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE fn = argv[0]; VALUE h = argv[1];
    if (!OC_IS_REF(h)) return OC_UNIT;
    for (VALUE l = OC_PTR(h)->refval; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
        VALUE pair = OC_PTR(l)->cons.head;
        if (OC_IS_TUPLE(pair) && OC_PTR(pair)->tup.n == 2) {
            VALUE av[2] = { OC_PTR(pair)->tup.items[0], OC_PTR(pair)->tup.items[1] };
            oc_apply(c, fn, 2, av);
        }
    }
    return OC_UNIT;
}
static VALUE prim_hash_replace(CTX *c, int argc, VALUE *argv) {
    prim_hash_remove(c, argc, argv);
    prim_hash_add(c, argc, argv);
    return OC_UNIT;
}

// Stack — head-of-list = top.
static VALUE prim_stack_create(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc;(void)argv; return oc_make_ref(OC_NIL); }
static VALUE prim_stack_push(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    VALUE x = argv[0]; VALUE s = argv[1];
    if (!OC_IS_REF(s)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Stack.push", 10)}));
    OC_PTR(s)->refval = oc_cons(x, OC_PTR(s)->refval);
    return OC_UNIT;
}
static VALUE prim_stack_pop(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE s = argv[0];
    if (!OC_IS_REF(s)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Stack.pop", 9)}));
    VALUE l = OC_PTR(s)->refval;
    if (!OC_IS_CONS(l)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Stack.Empty", 11)}));
    VALUE top = OC_PTR(l)->cons.head;
    OC_PTR(s)->refval = OC_PTR(l)->cons.tail;
    return top;
}
static VALUE prim_stack_top(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE s = argv[0];
    if (!OC_IS_REF(s)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Stack.top", 9)}));
    VALUE l = OC_PTR(s)->refval;
    if (!OC_IS_CONS(l)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Stack.Empty", 11)}));
    return OC_PTR(l)->cons.head;
}
static VALUE prim_stack_is_empty(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    if (!OC_IS_REF(argv[0])) return OC_TRUE;
    return OC_IS_CONS(OC_PTR(argv[0])->refval) ? OC_FALSE : OC_TRUE;
}
static VALUE prim_stack_length(CTX *c, int argc, VALUE *argv) {
    return prim_hash_length(c, argc, argv);
}

// Queue — head-of-list = front.
static VALUE prim_queue_create(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc;(void)argv; return oc_make_ref(OC_NIL); }
static VALUE prim_queue_add(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    VALUE x = argv[0]; VALUE q = argv[1];
    if (!OC_IS_REF(q)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Queue.add", 9)}));
    // Append: walk to end, build reversed, then add.
    VALUE buf[16384]; int n = 0;
    for (VALUE l = OC_PTR(q)->refval; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
        if (n < 16384) buf[n++] = OC_PTR(l)->cons.head;
    }
    VALUE acc = oc_cons(x, OC_NIL);
    for (int i = n - 1; i >= 0; i--) acc = oc_cons(buf[i], acc);
    OC_PTR(q)->refval = acc;
    return OC_UNIT;
}
static VALUE prim_queue_pop(CTX *c, int argc, VALUE *argv) {
    return prim_stack_pop(c, argc, argv);
}
static VALUE prim_queue_is_empty(CTX *c, int argc, VALUE *argv) {
    return prim_stack_is_empty(c, argc, argv);
}
static VALUE prim_queue_length(CTX *c, int argc, VALUE *argv) {
    return prim_hash_length(c, argc, argv);
}

// Buffer — bytes-backed mutable buffer.  Internal: ref to bytes plus len.
// Simpler: just use a ref to a list of strings, concatenate on contents.
static VALUE prim_buf_create(CTX *c, int argc, VALUE *argv) { (void)c;(void)argc;(void)argv; return oc_make_ref(OC_NIL); }
static VALUE prim_buf_add_string(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    VALUE b = argv[0]; VALUE s = argv[1];
    if (!OC_IS_REF(b)) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("Buffer.add_string", 17)}));
    OC_PTR(b)->refval = oc_cons(s, OC_PTR(b)->refval);
    return OC_UNIT;
}
static VALUE prim_buf_add_char(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    char ch = (char)(OC_INT_VAL(argv[1]) & 0xff);
    VALUE s = oc_make_string(&ch, 1);
    VALUE av[2] = { argv[0], s };
    return prim_buf_add_string(c, 2, av);
}
static VALUE prim_buf_contents(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    VALUE b = argv[0];
    if (!OC_IS_REF(b)) return oc_make_string("", 0);
    // Reverse the list (we prepended) and concat.
    VALUE buf[16384]; int n = 0;
    for (VALUE l = OC_PTR(b)->refval; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
        if (n < 16384) buf[n++] = OC_PTR(l)->cons.head;
    }
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        if (OC_IS_STRING(buf[i])) total += OC_PTR(buf[i])->str.len;
    }
    char *out = (char *)malloc(total + 1);
    size_t pos = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (OC_IS_STRING(buf[i])) {
            memcpy(out + pos, OC_PTR(buf[i])->str.chars, OC_PTR(buf[i])->str.len);
            pos += OC_PTR(buf[i])->str.len;
        }
    }
    out[total] = '\0';
    VALUE r = oc_make_string(out, total);
    free(out);
    return r;
}
static VALUE prim_buf_length(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    if (!OC_IS_REF(argv[0])) return OC_INT(0);
    int64_t total = 0;
    for (VALUE l = OC_PTR(argv[0])->refval; OC_IS_CONS(l); l = OC_PTR(l)->cons.tail) {
        VALUE s = OC_PTR(l)->cons.head;
        if (OC_IS_STRING(s)) total += OC_PTR(s)->str.len;
    }
    return OC_INT(total);
}
static VALUE prim_buf_clear(CTX *c, int argc, VALUE *argv) {
    (void)c;(void)argc;
    if (!OC_IS_REF(argv[0])) return OC_UNIT;
    OC_PTR(argv[0])->refval = OC_NIL;
    return OC_UNIT;
}

// `__class_build_with_parent`: methods, fields, parent_obj_or_unit.  If
// parent is an object, copy its methods/fields into the new object
// first; subclass entries override (last-write-wins via dedupe).
static VALUE prim_class_build_with_parent(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE m_list = argv[0], f_list = argv[1], parent = argv[2];
    int parent_nm = 0, parent_nf = 0;
    if (OC_IS_OBJECT(parent)) {
        struct oobj *po = OC_PTR(parent);
        parent_nm = po->obj.n_methods;
        parent_nf = po->obj.n_fields;
    }
    int own_nm = 0, own_nf = 0;
    for (VALUE v = m_list; OC_IS_CONS(v); v = OC_PTR(v)->cons.tail) own_nm++;
    for (VALUE v = f_list; OC_IS_CONS(v); v = OC_PTR(v)->cons.tail) own_nf++;
    int total_nm = parent_nm + own_nm;
    int total_nf = parent_nf + own_nf;
    const char **mn = (const char **)malloc(sizeof(char *) * (total_nm ? total_nm : 1));
    VALUE *mc = (VALUE *)malloc(sizeof(VALUE) * (total_nm ? total_nm : 1));
    const char **fn = (const char **)malloc(sizeof(char *) * (total_nf ? total_nf : 1));
    VALUE *fv = (VALUE *)malloc(sizeof(VALUE) * (total_nf ? total_nf : 1));
    int ni = 0, fi = 0;
    if (OC_IS_OBJECT(parent)) {
        struct oobj *po = OC_PTR(parent);
        for (int i = 0; i < parent_nm; i++) { mn[ni] = po->obj.method_names[i]; mc[ni] = po->obj.method_closures[i]; ni++; }
        for (int i = 0; i < parent_nf; i++) { fn[fi] = po->obj.field_names[i]; fv[fi] = po->obj.field_values[i]; fi++; }
    }
    for (VALUE v = m_list; OC_IS_CONS(v); v = OC_PTR(v)->cons.tail) {
        VALUE pair = OC_PTR(v)->cons.head;
        if (!OC_IS_TUPLE(pair) || OC_PTR(pair)->tup.n != 2) continue;
        VALUE name = OC_PTR(pair)->tup.items[0];
        const char *nm_s = OC_IS_STRING(name) ? OC_PTR(name)->str.chars : "?";
        // Override if already present.
        bool overridden = false;
        for (int i = 0; i < ni; i++) {
            if (strcmp(mn[i], nm_s) == 0) { mc[i] = OC_PTR(pair)->tup.items[1]; overridden = true; break; }
        }
        if (!overridden) { mn[ni] = nm_s; mc[ni] = OC_PTR(pair)->tup.items[1]; ni++; }
    }
    for (VALUE v = f_list; OC_IS_CONS(v); v = OC_PTR(v)->cons.tail) {
        VALUE pair = OC_PTR(v)->cons.head;
        if (!OC_IS_TUPLE(pair) || OC_PTR(pair)->tup.n != 2) continue;
        VALUE name = OC_PTR(pair)->tup.items[0];
        const char *nm_s = OC_IS_STRING(name) ? OC_PTR(name)->str.chars : "?";
        bool overridden = false;
        for (int i = 0; i < fi; i++) {
            if (strcmp(fn[i], nm_s) == 0) { fv[i] = OC_PTR(pair)->tup.items[1]; overridden = true; break; }
        }
        if (!overridden) { fn[fi] = nm_s; fv[fi] = OC_PTR(pair)->tup.items[1]; fi++; }
    }
    return oc_make_object(ni, mn, mc, fi, fn, fv);
}

// Class build helper: build OOBJ_OBJECT from a list of (name, closure)
// pairs (methods) and a list of (name, value) pairs (fields).
static VALUE prim_class_build(CTX *c, int argc, VALUE *argv) {
    (void)c;
    if (argc < 2) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("__class_build", 13)}));
    VALUE m_list = argv[0], f_list = argv[1];
    int nm = 0, nf = 0;
    for (VALUE v = m_list; OC_IS_CONS(v); v = OC_PTR(v)->cons.tail) nm++;
    for (VALUE v = f_list; OC_IS_CONS(v); v = OC_PTR(v)->cons.tail) nf++;
    const char **mn = (const char **)malloc(sizeof(char *) * (nm ? nm : 1));
    VALUE *mc = (VALUE *)malloc(sizeof(VALUE) * (nm ? nm : 1));
    const char **fn = (const char **)malloc(sizeof(char *) * (nf ? nf : 1));
    VALUE *fv = (VALUE *)malloc(sizeof(VALUE) * (nf ? nf : 1));
    int i = 0;
    for (VALUE v = m_list; OC_IS_CONS(v); v = OC_PTR(v)->cons.tail) {
        VALUE pair = OC_PTR(v)->cons.head;
        if (!OC_IS_TUPLE(pair) || OC_PTR(pair)->tup.n != 2)
            oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("class: bad method pair", 22)}));
        VALUE name = OC_PTR(pair)->tup.items[0];
        if (OC_IS_STRING(name)) mn[i] = OC_PTR(name)->str.chars;
        else mn[i] = "?";
        mc[i] = OC_PTR(pair)->tup.items[1];
        i++;
    }
    int j = 0;
    for (VALUE v = f_list; OC_IS_CONS(v); v = OC_PTR(v)->cons.tail) {
        VALUE pair = OC_PTR(v)->cons.head;
        if (!OC_IS_TUPLE(pair) || OC_PTR(pair)->tup.n != 2)
            oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("class: bad field pair", 21)}));
        VALUE name = OC_PTR(pair)->tup.items[0];
        if (OC_IS_STRING(name)) fn[j] = OC_PTR(name)->str.chars;
        else fn[j] = "?";
        fv[j] = OC_PTR(pair)->tup.items[1];
        j++;
    }
    return oc_make_object(nm, mn, mc, nf, fn, fv);
}

// Random.
static unsigned int g_random_state = 12345;
static VALUE prim_random_int(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    int64_t bound = OC_INT_VAL(argv[0]);
    if (bound <= 0) return OC_INT(0);
    g_random_state = g_random_state * 1103515245u + 12345u;
    return OC_INT((int64_t)((g_random_state >> 16) % (unsigned)bound));
}
static VALUE prim_random_float(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    double bound = oc_get_float(argv[0]);
    g_random_state = g_random_state * 1103515245u + 12345u;
    double r = ((g_random_state >> 16) & 0x7fff) / 32768.0;
    return oc_make_float(r * bound);
}
static VALUE prim_random_self_init(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    g_random_state = (unsigned int)time(NULL);
    return OC_UNIT;
}
static VALUE prim_random_init(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    g_random_state = (unsigned int)OC_INT_VAL(argv[0]);
    return OC_UNIT;
}

// Sys.
static int g_sys_argc = 0;
static char **g_sys_argv = NULL;
static VALUE prim_sys_argv(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    VALUE *items = (VALUE *)alloca(sizeof(VALUE) * (g_sys_argc ? g_sys_argc : 1));
    for (int i = 0; i < g_sys_argc; i++) items[i] = oc_make_string(g_sys_argv[i], strlen(g_sys_argv[i]));
    return oc_make_array(g_sys_argc, items);
}
static VALUE prim_sys_getenv(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Not_found", 0, NULL));
    char *v = getenv(OC_PTR(argv[0])->str.chars);
    if (!v) oc_raise(c, oc_make_variant("Not_found", 0, NULL));
    return oc_make_string(v, strlen(v));
}
static VALUE prim_sys_time(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return oc_make_float((double)ts.tv_sec + ts.tv_nsec / 1e9);
}
static VALUE prim_sys_command(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    if (!OC_IS_STRING(argv[0])) return OC_INT(-1);
    int r = system(OC_PTR(argv[0])->str.chars);
    return OC_INT(r);
}
static VALUE prim_sys_exit(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    exit((int)OC_INT_VAL(argv[0]));
}

// Format basics — `Format.sprintf` ≈ `Printf.sprintf` (no boxes).
// `print_string` and friends already exist.

// succ / pred / int_of_char / char_of_int.
static VALUE prim_succ(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (UNLIKELY(!OC_IS_INT(argv[0]))) oc_type_error(c, "succ", "int");
    return OC_INT(OC_INT_VAL(argv[0]) + 1);
}
static VALUE prim_pred(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (UNLIKELY(!OC_IS_INT(argv[0]))) oc_type_error(c, "pred", "int");
    return OC_INT(OC_INT_VAL(argv[0]) - 1);
}
static VALUE prim_bool_of_string(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_STRING(argv[0])) oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("bool_of_string", 14)}));
    if (strcmp(OC_PTR(argv[0])->str.chars, "true") == 0) return OC_TRUE;
    if (strcmp(OC_PTR(argv[0])->str.chars, "false") == 0) return OC_FALSE;
    oc_raise(c, oc_make_variant("Failure", 1, (VALUE[]){oc_make_string("bool_of_string", 14)}));
}
static VALUE prim_string_of_bool(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    return oc_make_string(argv[0] == OC_TRUE ? "true" : "false", argv[0] == OC_TRUE ? 4 : 5);
}

// Open a module dynamically — alias every `M.x` in globals as bare `x`.
static VALUE prim_open_module(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!OC_IS_STRING(argv[0])) return OC_UNIT;
    const char *mod = OC_PTR(argv[0])->str.chars;
    size_t plen = strlen(mod);
    // Snapshot current size to avoid iterating new entries we're adding.
    size_t end = c->globals_size;
    for (size_t i = 0; i < end; i++) {
        const char *gn = c->globals[i].name;
        if (strncmp(gn, mod, plen) == 0 && gn[plen] == '.') {
            oc_global_define(c, gn + plen + 1, c->globals[i].value);
        }
    }
    return OC_UNIT;
}

// Pre-defined exception singletons (0-ary constructors).
static VALUE prim_match_failure(CTX *c, int argc, VALUE *argv) { (void)argc; (void)argv; oc_raise(c, oc_make_variant("Match_failure", 0, NULL)); }
static VALUE prim_not_found(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; (void)argv; return oc_make_variant("Not_found", 0, NULL); }

// ---------------------------------------------------------------------------
// Install builtins.
// ---------------------------------------------------------------------------

static void
install_builtins(CTX *c)
{
    // Print.
    oc_global_define(c, "print_int",      oc_make_prim("print_int",      prim_print_int,      1, 1));
    oc_global_define(c, "print_string",   oc_make_prim("print_string",   prim_print_string,   1, 1));
    oc_global_define(c, "print_endline",  oc_make_prim("print_endline",  prim_print_endline,  1, 1));
    oc_global_define(c, "print_newline",  oc_make_prim("print_newline",  prim_print_newline,  1, 1));
    oc_global_define(c, "print_char",     oc_make_prim("print_char",     prim_print_char,     1, 1));
    oc_global_define(c, "print_float",    oc_make_prim("print_float",    prim_print_float,    1, 1));
    // Conversions.
    oc_global_define(c, "string_of_int",  oc_make_prim("string_of_int",  prim_string_of_int,  1, 1));
    oc_global_define(c, "int_of_string",  oc_make_prim("int_of_string",  prim_int_of_string,  1, 1));
    oc_global_define(c, "string_of_float",oc_make_prim("string_of_float",prim_string_of_float,1, 1));
    oc_global_define(c, "float_of_int",   oc_make_prim("float_of_int",   prim_float_of_int,   1, 1));
    oc_global_define(c, "int_of_float",   oc_make_prim("int_of_float",   prim_int_of_float,   1, 1));
    oc_global_define(c, "float_of_string",oc_make_prim("float_of_string",prim_float_of_string,1, 1));
    // Misc.
    oc_global_define(c, "ignore",         oc_make_prim("ignore",         prim_ignore,         1, 1));
    oc_global_define(c, "failwith",       oc_make_prim("failwith",       prim_failwith,       1, 1));
    oc_global_define(c, "invalid_arg",    oc_make_prim("invalid_arg",    prim_invalid_arg,    1, 1));
    oc_global_define(c, "raise",          oc_make_prim("raise",          prim_raise,          1, 1));
    oc_global_define(c, "assert",         oc_make_prim("assert",         prim_assert,         1, 1));
    oc_global_define(c, "compare",        oc_make_prim("compare",        prim_compare,        2, 2));
    oc_global_define(c, "abs",            oc_make_prim("abs",            prim_abs,            1, 1));
    oc_global_define(c, "min",            oc_make_prim("min",            prim_min,            2, 2));
    oc_global_define(c, "max",            oc_make_prim("max",            prim_max,            2, 2));
    oc_global_define(c, "ref",            oc_make_prim("ref",            prim_ref,            1, 1));
    // Predefined exception constructors (singletons).
    oc_global_define(c, "Not_found",      oc_make_variant("Not_found", 0, NULL));
    oc_global_define(c, "Exit",           oc_make_variant("Exit", 0, NULL));
    // List as plain functions.
    oc_global_define(c, "length",         oc_make_prim("length",         prim_list_length,    1, 1));
    oc_global_define(c, "hd",             oc_make_prim("hd",             prim_list_hd,        1, 1));
    oc_global_define(c, "tl",             oc_make_prim("tl",             prim_list_tl,        1, 1));
    // List.* as qualified names.
    oc_global_define(c, "List.length",    oc_make_prim("List.length",    prim_list_length,    1, 1));
    oc_global_define(c, "List.hd",        oc_make_prim("List.hd",        prim_list_hd,        1, 1));
    oc_global_define(c, "List.tl",        oc_make_prim("List.tl",        prim_list_tl,        1, 1));
    oc_global_define(c, "List.rev",       oc_make_prim("List.rev",       prim_list_rev,       1, 1));
    oc_global_define(c, "List.append",    oc_make_prim("List.append",    prim_list_append,    2, 2));
    oc_global_define(c, "List.map",       oc_make_prim("List.map",       prim_list_map,       2, 2));
    oc_global_define(c, "List.filter",    oc_make_prim("List.filter",    prim_list_filter,    2, 2));
    oc_global_define(c, "List.fold_left", oc_make_prim("List.fold_left", prim_list_fold_left, 3, 3));
    oc_global_define(c, "List.fold_right",oc_make_prim("List.fold_right",prim_list_fold_right,3, 3));
    oc_global_define(c, "List.iter",      oc_make_prim("List.iter",      prim_list_iter,      2, 2));
    oc_global_define(c, "List.nth",       oc_make_prim("List.nth",       prim_list_nth,       2, 2));
    oc_global_define(c, "List.mem",       oc_make_prim("List.mem",       prim_list_mem,       2, 2));
    oc_global_define(c, "List.assoc",     oc_make_prim("List.assoc",     prim_list_assoc,     2, 2));
    oc_global_define(c, "List.assoc_opt", oc_make_prim("List.assoc_opt", prim_list_assoc_opt, 2, 2));
    oc_global_define(c, "List.mem_assoc", oc_make_prim("List.mem_assoc", prim_list_mem_assoc, 2, 2));
    oc_global_define(c, "List.exists",    oc_make_prim("List.exists",    prim_list_exists,    2, 2));
    oc_global_define(c, "List.for_all",   oc_make_prim("List.for_all",   prim_list_for_all,   2, 2));
    oc_global_define(c, "List.find",      oc_make_prim("List.find",      prim_list_find,      2, 2));
    oc_global_define(c, "List.find_opt",  oc_make_prim("List.find_opt",  prim_list_find_opt,  2, 2));
    oc_global_define(c, "List.init",      oc_make_prim("List.init",      prim_list_init,      2, 2));
    oc_global_define(c, "List.flatten",   oc_make_prim("List.flatten",   prim_list_flatten,   1, 1));
    oc_global_define(c, "List.concat",    oc_make_prim("List.concat",    prim_list_flatten,   1, 1));
    oc_global_define(c, "List.combine",   oc_make_prim("List.combine",   prim_list_combine,   2, 2));
    oc_global_define(c, "List.split",     oc_make_prim("List.split",     prim_list_split,     1, 1));
    oc_global_define(c, "List.partition", oc_make_prim("List.partition", prim_list_partition, 2, 2));
    oc_global_define(c, "List.sort",      oc_make_prim("List.sort",      prim_list_sort,      2, 2));
    oc_global_define(c, "List.iter2",     oc_make_prim("List.iter2",     prim_list_iter2,     3, 3));
    oc_global_define(c, "List.map2",      oc_make_prim("List.map2",      prim_list_map2,      3, 3));
    // Predefined Some/None as constructors via global aliases.
    // (When used as `Some x` the parser emits node_variant_n; here we just
    // ensure that a bare `None` reference resolves to a value.)
    oc_global_define(c, "None",           oc_make_variant("None", 0, NULL));
    // String.* as qualified names.
    oc_global_define(c, "String.length",  oc_make_prim("String.length",  prim_string_length,  1, 1));
    oc_global_define(c, "String.get",     oc_make_prim("String.get",     prim_string_get,     2, 2));
    oc_global_define(c, "String.sub",     oc_make_prim("String.sub",     prim_string_sub,     3, 3));
    oc_global_define(c, "String.concat",  oc_make_prim("String.concat",  prim_string_concat,  2, 2));
    oc_global_define(c, "String.make",    oc_make_prim("String.make",    prim_string_make,    2, 2));
    oc_global_define(c, "String.contains",oc_make_prim("String.contains",prim_string_contains,2, 2));
    oc_global_define(c, "String.index",   oc_make_prim("String.index",   prim_string_index,   2, 2));
    oc_global_define(c, "String.uppercase_ascii", oc_make_prim("String.uppercase_ascii", prim_string_uppercase, 1, 1));
    oc_global_define(c, "String.lowercase_ascii", oc_make_prim("String.lowercase_ascii", prim_string_lowercase, 1, 1));
    oc_global_define(c, "Bytes.blit",     oc_make_prim("Bytes.blit",     prim_bytes_blit,     5, 5));
    // Char.* (chars are ints).
    oc_global_define(c, "Char.code",      oc_make_prim("Char.code",      prim_char_code,      1, 1));
    oc_global_define(c, "Char.chr",       oc_make_prim("Char.chr",       prim_char_chr,       1, 1));
    // Array.*
    oc_global_define(c, "Array.make",     oc_make_prim("Array.make",     prim_array_make,     2, 2));
    oc_global_define(c, "Array.length",   oc_make_prim("Array.length",   prim_array_length,   1, 1));
    oc_global_define(c, "Array.get",      oc_make_prim("Array.get",      prim_array_get,      2, 2));
    oc_global_define(c, "Array.set",      oc_make_prim("Array.set",      prim_array_set,      3, 3));
    oc_global_define(c, "__array_make",   oc_make_prim("__array_make",   prim_array_make_n,   0, -1));
    oc_global_define(c, "__array_get",    oc_make_prim("__array_get",    prim_array_get,      2, 2));
    // Lazy.
    oc_global_define(c, "Lazy.force",     oc_make_prim("Lazy.force",     prim_lazy_force,     1, 1));
    oc_global_define(c, "Lazy.is_val",    oc_make_prim("Lazy.is_val",    prim_lazy_is_val,    1, 1));
    // Bytes.
    oc_global_define(c, "Bytes.create",   oc_make_prim("Bytes.create",   prim_bytes_create,   1, 1));
    oc_global_define(c, "Bytes.make",     oc_make_prim("Bytes.make",     prim_bytes_make,     2, 2));
    oc_global_define(c, "Bytes.length",   oc_make_prim("Bytes.length",   prim_bytes_length,   1, 1));
    oc_global_define(c, "Bytes.get",      oc_make_prim("Bytes.get",      prim_bytes_get,      2, 2));
    oc_global_define(c, "Bytes.set",      oc_make_prim("Bytes.set",      prim_bytes_set,      3, 3));
    oc_global_define(c, "Bytes.to_string",oc_make_prim("Bytes.to_string",prim_bytes_to_string,1, 1));
    oc_global_define(c, "Bytes.of_string",oc_make_prim("Bytes.of_string",prim_bytes_of_string,1, 1));
    // Printf.
    oc_global_define(c, "Printf.printf",  oc_make_prim("Printf.printf",  prim_printf,         1, -1));
    oc_global_define(c, "Printf.eprintf", oc_make_prim("Printf.eprintf", prim_eprintf,        1, -1));
    oc_global_define(c, "Printf.sprintf", oc_make_prim("Printf.sprintf", prim_sprintf,        1, -1));
    oc_global_define(c, "printf",         oc_make_prim("printf",         prim_printf,         1, -1));
    // Class build helper.
    oc_global_define(c, "__class_build",  oc_make_prim("__class_build",  prim_class_build,    2, 2));
    oc_global_define(c, "__class_build_with_parent", oc_make_prim("__class_build_with_parent", prim_class_build_with_parent, 3, 3));
    oc_global_define(c, "__open_module",  oc_make_prim("__open_module",  prim_open_module,    1, 1));
    // Random.
    oc_global_define(c, "Random.int",     oc_make_prim("Random.int",     prim_random_int,     1, 1));
    oc_global_define(c, "Random.float",   oc_make_prim("Random.float",   prim_random_float,   1, 1));
    oc_global_define(c, "Random.self_init", oc_make_prim("Random.self_init", prim_random_self_init, 0, -1));
    oc_global_define(c, "Random.init",    oc_make_prim("Random.init",    prim_random_init,    1, 1));
    // Sys.
    oc_global_define(c, "Sys.argv",       oc_make_prim("Sys.argv",       prim_sys_argv,       0, -1));
    oc_global_define(c, "Sys.getenv",     oc_make_prim("Sys.getenv",     prim_sys_getenv,     1, 1));
    oc_global_define(c, "Sys.time",       oc_make_prim("Sys.time",       prim_sys_time,       0, -1));
    oc_global_define(c, "Sys.command",    oc_make_prim("Sys.command",    prim_sys_command,    1, 1));
    oc_global_define(c, "exit",           oc_make_prim("exit",           prim_sys_exit,       1, 1));
    // Format ≈ Printf for our purposes.
    oc_global_define(c, "Format.sprintf", oc_make_prim("Format.sprintf", prim_sprintf,        1, -1));
    oc_global_define(c, "Format.printf",  oc_make_prim("Format.printf",  prim_printf,         1, -1));
    oc_global_define(c, "Format.eprintf", oc_make_prim("Format.eprintf", prim_eprintf,        1, -1));
    // succ / pred / etc.
    oc_global_define(c, "succ",           oc_make_prim("succ",           prim_succ,           1, 1));
    oc_global_define(c, "pred",           oc_make_prim("pred",           prim_pred,           1, 1));
    oc_global_define(c, "bool_of_string", oc_make_prim("bool_of_string", prim_bool_of_string, 1, 1));
    oc_global_define(c, "string_of_bool", oc_make_prim("string_of_bool", prim_string_of_bool, 1, 1));
    // Hashtbl
    oc_global_define(c, "Hashtbl.create",  oc_make_prim("Hashtbl.create",  prim_hash_create,  1, 1));
    oc_global_define(c, "Hashtbl.add",     oc_make_prim("Hashtbl.add",     prim_hash_add,     3, 3));
    oc_global_define(c, "Hashtbl.find",    oc_make_prim("Hashtbl.find",    prim_hash_find,    2, 2));
    oc_global_define(c, "Hashtbl.mem",     oc_make_prim("Hashtbl.mem",     prim_hash_mem,     2, 2));
    oc_global_define(c, "Hashtbl.remove",  oc_make_prim("Hashtbl.remove",  prim_hash_remove,  2, 2));
    oc_global_define(c, "Hashtbl.length",  oc_make_prim("Hashtbl.length",  prim_hash_length,  1, 1));
    oc_global_define(c, "Hashtbl.iter",    oc_make_prim("Hashtbl.iter",    prim_hash_iter,    2, 2));
    oc_global_define(c, "Hashtbl.replace", oc_make_prim("Hashtbl.replace", prim_hash_replace, 3, 3));
    // Stack
    oc_global_define(c, "Stack.create",    oc_make_prim("Stack.create",    prim_stack_create, 0, -1));
    oc_global_define(c, "Stack.push",      oc_make_prim("Stack.push",      prim_stack_push,   2, 2));
    oc_global_define(c, "Stack.pop",       oc_make_prim("Stack.pop",       prim_stack_pop,    1, 1));
    oc_global_define(c, "Stack.top",       oc_make_prim("Stack.top",       prim_stack_top,    1, 1));
    oc_global_define(c, "Stack.is_empty",  oc_make_prim("Stack.is_empty",  prim_stack_is_empty, 1, 1));
    oc_global_define(c, "Stack.length",    oc_make_prim("Stack.length",    prim_stack_length, 1, 1));
    // Queue
    oc_global_define(c, "Queue.create",    oc_make_prim("Queue.create",    prim_queue_create, 0, -1));
    oc_global_define(c, "Queue.add",       oc_make_prim("Queue.add",       prim_queue_add,    2, 2));
    oc_global_define(c, "Queue.push",      oc_make_prim("Queue.push",      prim_queue_add,    2, 2));
    oc_global_define(c, "Queue.pop",       oc_make_prim("Queue.pop",       prim_queue_pop,    1, 1));
    oc_global_define(c, "Queue.is_empty",  oc_make_prim("Queue.is_empty",  prim_queue_is_empty, 1, 1));
    oc_global_define(c, "Queue.length",    oc_make_prim("Queue.length",    prim_queue_length, 1, 1));
    // Buffer
    oc_global_define(c, "Buffer.create",   oc_make_prim("Buffer.create",   prim_buf_create,   1, 1));
    oc_global_define(c, "Buffer.add_string", oc_make_prim("Buffer.add_string", prim_buf_add_string, 2, 2));
    oc_global_define(c, "Buffer.add_char",   oc_make_prim("Buffer.add_char",   prim_buf_add_char,   2, 2));
    oc_global_define(c, "Buffer.contents",   oc_make_prim("Buffer.contents",   prim_buf_contents,   1, 1));
    oc_global_define(c, "Buffer.length",     oc_make_prim("Buffer.length",     prim_buf_length,     1, 1));
    oc_global_define(c, "Buffer.clear",      oc_make_prim("Buffer.clear",      prim_buf_clear,      1, 1));
    // Infix operators as standalone function values.
    oc_global_define(c, "+",   oc_make_prim("+",   prim_op_add, 2, 2));
    oc_global_define(c, "-",   oc_make_prim("-",   prim_op_sub, 2, 2));
    oc_global_define(c, "*",   oc_make_prim("*",   prim_op_mul, 2, 2));
    oc_global_define(c, "/",   oc_make_prim("/",   prim_op_div, 2, 2));
    oc_global_define(c, "mod", oc_make_prim("mod", prim_op_mod, 2, 2));
    oc_global_define(c, "+.",  oc_make_prim("+.",  prim_op_fadd, 2, 2));
    oc_global_define(c, "-.",  oc_make_prim("-.",  prim_op_fsub, 2, 2));
    oc_global_define(c, "*.",  oc_make_prim("*.",  prim_op_fmul, 2, 2));
    oc_global_define(c, "/.",  oc_make_prim("/.",  prim_op_fdiv, 2, 2));
    oc_global_define(c, "<",   oc_make_prim("<",   prim_op_lt, 2, 2));
    oc_global_define(c, ">",   oc_make_prim(">",   prim_op_gt, 2, 2));
    oc_global_define(c, "<=",  oc_make_prim("<=",  prim_op_le, 2, 2));
    oc_global_define(c, ">=",  oc_make_prim(">=",  prim_op_ge, 2, 2));
    oc_global_define(c, "=",   oc_make_prim("=",   prim_op_eq, 2, 2));
    oc_global_define(c, "<>",  oc_make_prim("<>",  prim_op_ne, 2, 2));
    oc_global_define(c, "^",   oc_make_prim("^",   prim_op_concat, 2, 2));
    oc_global_define(c, "::",  oc_make_prim("::",  prim_op_cons,  2, 2));
    // Float math.
    oc_global_define(c, "sqrt",           oc_make_prim("sqrt",           prim_sqrt,           1, 1));
    oc_global_define(c, "sin",            oc_make_prim("sin",            prim_sin,            1, 1));
    oc_global_define(c, "cos",            oc_make_prim("cos",            prim_cos,            1, 1));
    oc_global_define(c, "log",            oc_make_prim("log",            prim_log,            1, 1));
    oc_global_define(c, "exp",            oc_make_prim("exp",            prim_exp,            1, 1));
    oc_global_define(c, "floor",          oc_make_prim("floor",          prim_floor,          1, 1));
    oc_global_define(c, "ceil",           oc_make_prim("ceil",           prim_ceil,           1, 1));
}

// ---------------------------------------------------------------------------
// File / REPL drivers.
// ---------------------------------------------------------------------------

static char *
read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(1); }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if ((long)fread(s, 1, (size_t)n, fp) != n) { /* short read, treat as ok */ }
    s[n] = '\0';
    fclose(fp);
    return s;
}

static void
init_lexer(const char *text)
{
    src = text; src_pos = 0; src_line = 1;
    next_token();
}

int
main(int argc, char **argv)
{
    GC_INIT();
    g_sys_argc = argc;
    g_sys_argv = argv;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) OPTION.quiet = true;
        else if (!strcmp(argv[i], "--no-compile")) OPTION.no_compiled_code = true;
        else if (!strcmp(argv[i], "--no-codegen")) OPTION.no_generate_specialized_code = true;
        else if (!strcmp(argv[i], "--compile") || !strcmp(argv[i], "-c")) OPTION.compile = true;
        else if (!strcmp(argv[i], "--check") || !strcmp(argv[i], "-T")) OPTION.type_check = true;
        else if (!strcmp(argv[i], "--no-check"))                        OPTION.type_check = false;
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n", argv[i]); exit(1); }
        else path = argv[i];
    }

    INIT();
    CTX *c = (CTX *)calloc(1, sizeof(CTX));
    c->handlers_top = -1;
    install_builtins(c);
    // Type check is on by default — disable with `--no-check`.
    if (!OPTION.type_check) OPTION.type_check = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-check")) { OPTION.type_check = false; break; }
    }

    if (path) {
        char *text = read_file(path);
        init_lexer(text);
        if (setjmp(c->err_jmp) == 0) {
            c->err_jmp_active = 1;
            parse_program(c);
        }
        else {
            fprintf(stderr, "astocaml: %s\n", c->err_msg);
            exit(2);
        }
        return 0;
    }

    char *line;
    while (1) {
#ifdef USE_READLINE
        line = readline("# ");
        if (!line) break;
        if (*line) add_history(line);
#else
        static char buf[4096];
        fputs("# ", stdout); fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) break;
        line = buf;
#endif
        char *full = strdup(line);
#ifdef USE_READLINE
        free(line);
#endif
        while (!strstr(full, ";;")) {
#ifdef USE_READLINE
            char *more = readline("  ");
            if (!more) break;
            char *combined = (char *)malloc(strlen(full) + strlen(more) + 2);
            sprintf(combined, "%s\n%s", full, more);
            free(full); free(more);
            full = combined;
#else
            static char more[4096];
            fputs("  ", stdout); fflush(stdout);
            if (!fgets(more, sizeof more, stdin)) break;
            char *combined = (char *)malloc(strlen(full) + strlen(more) + 2);
            sprintf(combined, "%s%s", full, more);
            free(full);
            full = combined;
#endif
        }
        init_lexer(full);
        if (setjmp(c->err_jmp) == 0) {
            c->err_jmp_active = 1;
            parse_program(c);
        }
        else {
            fprintf(stderr, "astocaml: %s\n", c->err_msg);
        }
        free(full);
    }
    return 0;
}
