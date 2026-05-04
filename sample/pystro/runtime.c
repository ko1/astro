// runtime.c — pystro runtime (heap, builders, globals, apply, display,
// numeric tower, containers, attribute / method, iteration, exceptions,
// builtins).  #included from main.c.

#include <math.h>

// ---------------------------------------------------------------------------
// Singletons.
// ---------------------------------------------------------------------------

struct pyobj PY_NONE_OBJ  = { .type = PY_T_NONE };
struct pyobj PY_TRUE_OBJ  = { .type = PY_T_BOOL, .b = true };
struct pyobj PY_FALSE_OBJ = { .type = PY_T_BOOL, .b = false };

// Set by main() at startup so the parameterless py_display can call
// instance __str__ / __repr__ without changing its signature (called
// from many places, including recursively from list/dict display).
CTX *py_current_ctx = NULL;

// ---------------------------------------------------------------------------
// GC + GMP.
// ---------------------------------------------------------------------------

static void *gmp_alloc  (size_t sz)                  { return GC_malloc(sz); }
static void *gmp_realloc(void *p, size_t old, size_t nw) { (void)old; return GC_realloc(p, nw); }
static void  gmp_free   (void *p, size_t sz)         { (void)p; (void)sz; /* GC sweeps */ }

static void
py_gc_init(void)
{
    GC_init();
    GC_set_free_space_divisor(1);
    GC_expand_hp((size_t)16 * 1024 * 1024);
    mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
}

// ---------------------------------------------------------------------------
// Heap allocation helpers.
// ---------------------------------------------------------------------------

struct pyobj *
py_alloc(int type)
{
    struct pyobj *o = (struct pyobj *)GC_malloc(sizeof(struct pyobj));
    o->type = type;
    return o;
}

VALUE
py_make_float(double d)
{
    VALUE inline_v = py_try_flonum(d);
    if (LIKELY(inline_v)) return inline_v;
    struct pyobj *o = py_alloc(PY_T_FLOAT);
    o->dbl = d;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_bignum(mpz_srcptr z)
{
    struct pyobj *o = py_alloc(PY_T_BIGNUM);
    mpz_init_set(o->mpz, z);
    return PY_OBJ_VAL(o);
}

// Normalise: bignum that fits → fixnum.
static VALUE
py_normalise_int(mpz_srcptr z)
{
    if (mpz_fits_slong_p(z)) {
        long v = mpz_get_si(z);
        if (v >= PY_FIXNUM_MIN && v <= PY_FIXNUM_MAX) return PY_FIX(v);
    }
    return py_make_bignum(z);
}

VALUE
py_make_int(int64_t v)
{
    if (v >= PY_FIXNUM_MIN && v <= PY_FIXNUM_MAX) return PY_FIX(v);
    mpz_t z; mpz_init(z);
    mpz_set_si(z, (long)v);     // covers up to long range
    if ((int64_t)(long)v != v) {
        // Wider than long — set via string.
        char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
        mpz_set_str(z, buf, 10);
    }
    VALUE r = py_make_bignum(z);
    mpz_clear(z);
    return r;
}

VALUE
py_make_str(const char *s, size_t len)
{
    struct pyobj *o = py_alloc(PY_T_STR);
    char *buf = (char *)GC_malloc_atomic(len + 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    o->str.chars = buf;
    o->str.len = len;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_str_take(char *s, size_t len)
{
    struct pyobj *o = py_alloc(PY_T_STR);
    o->str.chars = s;
    o->str.len = len;
    return PY_OBJ_VAL(o);
}

// Share the buffer of an existing string (e.g. a substring / slice).
// Boehm's interior-pointer support keeps the parent buffer alive as
// long as any sub-string holds a pointer into it.  No NUL terminator
// is required (str.len is authoritative); the only places that touch
// trailing chars are py_display (uses fwrite + len) and the numeric
// converters (which copy out first).
//
// Allocates only the bytes a string-typed pyobj actually uses (type +
// chars + len) — Boehm buckets requests by size and a 24-byte block is
// dramatically smaller than a full sizeof(struct pyobj) (which is
// dominated by the union's biggest member, the func / class struct).
static const size_t py_str_size = offsetof(struct pyobj, str) + sizeof(((struct pyobj *)0)->str);

static VALUE
py_make_str_borrow(const char *src, size_t len)
{
    struct pyobj *o = (struct pyobj *)GC_malloc(py_str_size);
    o->type = PY_T_STR;
    o->str.chars = (char *)src;
    o->str.len = len;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_list(VALUE *items, size_t n)
{
    struct pyobj *o = py_alloc(PY_T_LIST);
    size_t capa = n < 4 ? 4 : n;
    o->list.items = (VALUE *)GC_malloc(sizeof(VALUE) * capa);
    if (n) memcpy(o->list.items, items, sizeof(VALUE) * n);
    o->list.len = n;
    o->list.capa = capa;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_tuple(VALUE *items, size_t n)
{
    struct pyobj *o = py_alloc(PY_T_TUPLE);
    o->list.items = n ? (VALUE *)GC_malloc(sizeof(VALUE) * n) : NULL;
    if (n) memcpy(o->list.items, items, sizeof(VALUE) * n);
    o->list.len = n;
    o->list.capa = n;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_range(int64_t start, int64_t stop, int64_t step)
{
    struct pyobj *o = py_alloc(PY_T_RANGE);
    o->range.start = start;
    o->range.stop  = stop;
    o->range.step  = step;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_func(struct Node *body, struct pyframe *env,
             const char *name, int nparams, int n_pos_named,
             int nlocals, VALUE *defaults_per_slot, bool leaf,
             const char **param_names,
             bool has_varargs, bool has_kwargs)
{
    struct pyobj *o = py_alloc(PY_T_FUNC);
    o->func.body = body;
    o->func.env = env;
    o->func.name = name;
    o->func.nparams = nparams;
    o->func.n_pos_named = n_pos_named;
    o->func.nlocals = nlocals;
    o->func.leaf = leaf;
    o->func.has_varargs = has_varargs;
    o->func.has_kwargs = has_kwargs;
    o->func.param_names = param_names;
    if (nparams > 0) {
        VALUE *d = (VALUE *)GC_malloc(sizeof(VALUE) * nparams);
        if (defaults_per_slot) memcpy(d, defaults_per_slot, sizeof(VALUE) * nparams);
        else for (int i = 0; i < nparams; i++) d[i] = (VALUE)0;
        o->func.defaults = d;
    } else {
        o->func.defaults = NULL;
    }
    return PY_OBJ_VAL(o);
}

VALUE
py_make_builtin(const char *name, py_builtin_fn fn, int min_argc, int max_argc)
{
    struct pyobj *o = py_alloc(PY_T_BUILTIN);
    o->builtin.fn = fn;
    o->builtin.name = name;
    o->builtin.min_argc = min_argc;
    o->builtin.max_argc = max_argc;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_bound(VALUE self, VALUE func)
{
    struct pyobj *o = py_alloc(PY_T_BOUND_METHOD);
    o->bound.self = self;
    o->bound.func = func;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_class(const char *name, VALUE base, bool is_exception)
{
    struct pyobj *o = py_alloc(PY_T_CLASS);
    o->cls.name = name;
    o->cls.methods = NULL;
    o->cls.nmethods = 0;
    o->cls.methods_capa = 0;
    o->cls.is_exception = is_exception;
    o->cls.base = base;
    return PY_OBJ_VAL(o);
}

void
py_class_add_method(CTX *c, VALUE cls, const char *name, VALUE fn)
{
    (void)c;
    struct pyclass *cd = &PY_PTR(cls)->cls;
    // Replace if a method with the same name already exists — required
    // for decorator-wrapped methods (`@classmethod\ndef m`) which first
    // register the plain func and then re-register the wrapped version.
    for (int i = 0; i < cd->nmethods; i++) {
        if (strcmp(cd->methods[i].name, name) == 0) {
            cd->methods[i].value = fn;
            return;
        }
    }
    if (cd->nmethods == cd->methods_capa) {
        int cap = cd->methods_capa ? cd->methods_capa * 2 : 4;
        cd->methods = (struct pyclass_method *)GC_realloc(
            cd->methods, sizeof(struct pyclass_method) * cap);
        cd->methods_capa = cap;
    }
    cd->methods[cd->nmethods].name = name;
    cd->methods[cd->nmethods].value = fn;
    cd->nmethods++;
}

static VALUE
py_class_lookup_method(VALUE cls, const char *name)
{
    while (cls != PY_NONE && py_is_class(cls)) {
        struct pyclass *cd = &PY_PTR(cls)->cls;
        for (int i = 0; i < cd->nmethods; i++) {
            if (strcmp(cd->methods[i].name, name) == 0) return cd->methods[i].value;
        }
        cls = cd->base;
    }
    return PY_NONE;
}

VALUE
py_class_lookup_method_pub(VALUE cls, const char *name)
{
    return py_class_lookup_method(cls, name);
}

VALUE
py_make_instance(VALUE cls)
{
    struct pyobj *o = py_alloc(PY_T_INSTANCE);
    o->inst.cls = PY_PTR(cls);
    o->inst.attrs = NULL;       // lazily allocated when first attr is set
    return PY_OBJ_VAL(o);
}

struct pyframe *
py_new_frame(struct pyframe *parent, int nslots)
{
    struct pyframe *f = (struct pyframe *)GC_malloc(
        sizeof(struct pyframe) + sizeof(VALUE) * (nslots ? nslots : 1));
    f->parent = parent;
    f->nslots = nslots;
    for (int i = 0; i < nslots; i++) f->slots[i] = PY_NONE;
    return f;
}

// ---------------------------------------------------------------------------
// Globals.
// ---------------------------------------------------------------------------

static int
py_global_index(CTX *c, const char *name)
{
    for (size_t i = 0; i < c->globals_size; i++)
        if (strcmp(c->globals[i].name, name) == 0) return (int)i;
    return -1;
}

static int
py_global_alloc(CTX *c, const char *name)
{
    if (c->globals_size == c->globals_capa) {
        size_t cap = c->globals_capa ? c->globals_capa * 2 : 32;
        c->globals = (struct gentry *)GC_realloc(c->globals, cap * sizeof(struct gentry));
        c->globals_capa = cap;
    }
    int i = (int)c->globals_size++;
    c->globals[i].name = name;
    c->globals[i].value = PY_NONE;
    c->globals[i].defined = false;
    return i;
}

void
py_global_define(CTX *c, const char *name, VALUE v)
{
    int i = py_global_index(c, name);
    bool is_new = (i < 0);
    if (is_new) i = py_global_alloc(c, name);
    bool was_defined = c->globals[i].defined;
    c->globals[i].value = v;
    c->globals[i].defined = true;
    // Only bump on _structural_ change (new slot or first define of an
    // existing slot).  Plain value updates would invalidate every other
    // gref's cache on every assignment — fatal for tight `i += 1` loops.
    if (is_new || !was_defined) c->globals_serial++;
}

bool
py_global_has(CTX *c, const char *name)
{
    int i = py_global_index(c, name);
    return i >= 0 && c->globals[i].defined;
}

VALUE
py_global_ref(CTX *c, const char *name)
{
    int i = py_global_index(c, name);
    if (i < 0 || !c->globals[i].defined)
        py_raise_exc(c, c->EXC_NameError, "name '%s' is not defined", name);
    return c->globals[i].value;
}

// Used by `node_gref`'s @ref-cache path: resolve a name to a globals
// index (allocating an undefined slot if missing) so subsequent ref
// reads can index directly without strcmp.
int
py_global_resolve(CTX *c, const char *name)
{
    int i = py_global_index(c, name);
    if (i < 0) {
        // Pre-allocate a slot so `idx` stays stable even if the name
        // hasn't been defined yet (a NameError will be raised via the
        // `defined` flag check on the read).  But we want the read fast
        // path NOT to check `defined` on every call — so raise here on
        // first miss instead.
        py_raise_exc(c, c->EXC_NameError, "name '%s' is not defined", name);
    }
    if (!c->globals[i].defined)
        py_raise_exc(c, c->EXC_NameError, "name '%s' is not defined", name);
    return i;
}

// Variant for write-side resolution: allocates the slot if missing
// (like `py_global_define` would on first set) so for/gset hot loops
// can skip the linear strcmp after the first iteration.
int
py_global_resolve_or_alloc(CTX *c, const char *name)
{
    int i = py_global_index(c, name);
    if (i < 0) i = py_global_alloc(c, name);
    return i;
}

void
py_global_set(CTX *c, const char *name, VALUE v)
{
    py_global_define(c, name, v);
}

// ---------------------------------------------------------------------------
// Errors / raise.
// ---------------------------------------------------------------------------

void
py_error(CTX *c, const char *fmt, ...)
{
    fprintf(stderr, "pystro: ");
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (c && c->err_jmp_active) longjmp(c->err_jmp, 1);
    exit(1);
}

void
py_raise_exc(CTX *c, VALUE cls, const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (cls == 0 || !py_is_class(cls)) cls = c->EXC_RuntimeError;
    VALUE inst = py_make_instance(cls);
    VALUE msg = py_make_str(buf, strlen(buf));
    py_setattr(c, inst, "args", py_make_tuple(&msg, 1));
    py_setattr(c, inst, "message", msg);
    c->state = PY_STATE_RAISE;
    c->state_value = inst;
    if (c->try_top > 0) longjmp(*c->try_stack[c->try_top - 1], 1);
    if (c->err_jmp_active) longjmp(c->err_jmp, 1);
    exit(1);
}

// ---------------------------------------------------------------------------
// Numeric tower.
// ---------------------------------------------------------------------------

static double
py_to_double(CTX *c, VALUE v)
{
    if (PY_IS_FIXNUM(v))    return (double)PY_FIXVAL(v);
    if (PY_IS_FLONUM(v))    return py_flonum_to_double(v);
    if (py_is_heap_float(v)) return PY_PTR(v)->dbl;
    if (py_is_bignum(v))    return mpz_get_d(PY_PTR(v)->mpz);
    if (v == PY_TRUE)       return 1.0;
    if (v == PY_FALSE)      return 0.0;
    py_raise_exc(c, c->EXC_TypeError, "expected a number");
}

// `v` must already be int-ish (int / bool / bignum).
static void
py_to_mpz(CTX *c, VALUE v, mpz_t out)
{
    if (PY_IS_FIXNUM(v)) { mpz_init_set_si(out, (long)PY_FIXVAL(v)); return; }
    if (py_is_bignum(v)) { mpz_init_set(out, PY_PTR(v)->mpz); return; }
    if (v == PY_TRUE)    { mpz_init_set_si(out, 1); return; }
    if (v == PY_FALSE)   { mpz_init_set_si(out, 0); return; }
    py_raise_exc(c, c->EXC_TypeError, "expected an integer");
}

static bool
py_int_or_bool(VALUE v)
{
    return PY_IS_FIXNUM(v) || py_is_bignum(v) || v == PY_TRUE || v == PY_FALSE;
}

VALUE
py_neg(CTX *c, VALUE a)
{
    if (PY_IS_FIXNUM(a)) {
        int64_t r;
        if (!__builtin_sub_overflow((int64_t)0, PY_FIXVAL(a), &r) &&
            r >= PY_FIXNUM_MIN && r <= PY_FIXNUM_MAX)
            return PY_FIX(r);
    }
    if (PY_IS_FLONUM(a)) return py_make_float(-py_flonum_to_double(a));
    if (py_is_heap_float(a)) return py_make_float(-PY_PTR(a)->dbl);
    if (py_int_or_bool(a)) {
        mpz_t z; py_to_mpz(c, a, z);
        mpz_neg(z, z);
        VALUE r = py_normalise_int(z);
        mpz_clear(z);
        return r;
    }
    py_raise_exc(c, c->EXC_TypeError, "bad operand type for unary -");
}

// Try a binary dunder hook on an instance operand: returns the result
// if the dunder exists, else PY_NONE (caller falls through to the
// regular numeric / type logic).  We use 0 as "method not defined"
// sentinel since PY_NONE is a valid return.
static VALUE
py_try_binop_dunder(CTX *c, const char *name, VALUE a, VALUE b)
{
    if (py_is_instance(a)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(a)->inst.cls), name);
        if (m != PY_NONE) {
            VALUE av[2] = { a, b };
            return py_apply(c, m, 2, av);
        }
    }
    return (VALUE)0;
}

VALUE
py_add(CTX *c, VALUE a, VALUE b)
{
    VALUE r = py_try_binop_dunder(c, "__add__", a, b);
    if (r) return r;
    r = py_try_binop_dunder(c, "__radd__", b, a);
    if (r) return r;
    if (py_is_str(a) && py_is_str(b)) {
        size_t la = PY_PTR(a)->str.len, lb = PY_PTR(b)->str.len;
        char *buf = (char *)GC_malloc_atomic(la + lb + 1);
        memcpy(buf,      PY_PTR(a)->str.chars, la);
        memcpy(buf + la, PY_PTR(b)->str.chars, lb);
        buf[la + lb] = '\0';
        return py_make_str_take(buf, la + lb);
    }
    if ((py_is_list(a) && py_is_list(b)) || (py_is_tuple(a) && py_is_tuple(b))) {
        size_t la = PY_PTR(a)->list.len, lb = PY_PTR(b)->list.len;
        VALUE *items = (VALUE *)alloca(sizeof(VALUE) * (la + lb + 1));
        memcpy(items,      PY_PTR(a)->list.items, sizeof(VALUE) * la);
        memcpy(items + la, PY_PTR(b)->list.items, sizeof(VALUE) * lb);
        return py_is_list(a) ? py_make_list(items, la + lb) : py_make_tuple(items, la + lb);
    }
    if (py_int_or_bool(a) && py_int_or_bool(b)) {
        mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
        mpz_add(za, za, zb);
        VALUE r = py_normalise_int(za);
        mpz_clear(za); mpz_clear(zb);
        return r;
    }
    if ((py_int_or_bool(a) || py_is_float(a)) && (py_int_or_bool(b) || py_is_float(b)))
        return py_make_float(py_to_double(c, a) + py_to_double(c, b));
    py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for +");
}

VALUE
py_sub(CTX *c, VALUE a, VALUE b)
{
    VALUE r = py_try_binop_dunder(c, "__sub__", a, b);
    if (r) return r;
    if (py_int_or_bool(a) && py_int_or_bool(b)) {
        mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
        mpz_sub(za, za, zb);
        VALUE r = py_normalise_int(za);
        mpz_clear(za); mpz_clear(zb);
        return r;
    }
    if ((py_int_or_bool(a) || py_is_float(a)) && (py_int_or_bool(b) || py_is_float(b)))
        return py_make_float(py_to_double(c, a) - py_to_double(c, b));
    py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for -");
}

VALUE
py_mul(CTX *c, VALUE a, VALUE b)
{
    VALUE r = py_try_binop_dunder(c, "__mul__", a, b);
    if (r) return r;
    if (py_is_str(a) && py_int_or_bool(b)) {
        int64_t k = PY_IS_FIXNUM(b) ? PY_FIXVAL(b) : (b == PY_TRUE ? 1 : 0);
        if (k <= 0) return py_make_str("", 0);
        size_t la = PY_PTR(a)->str.len;
        char *buf = (char *)GC_malloc_atomic(la * (size_t)k + 1);
        for (int64_t i = 0; i < k; i++) memcpy(buf + i * la, PY_PTR(a)->str.chars, la);
        buf[la * k] = '\0';
        return py_make_str_take(buf, la * (size_t)k);
    }
    if (py_int_or_bool(a) && py_is_str(b)) return py_mul(c, b, a);
    if ((py_is_list(a) || py_is_tuple(a)) && py_int_or_bool(b)) {
        int64_t k = PY_IS_FIXNUM(b) ? PY_FIXVAL(b) : (b == PY_TRUE ? 1 : 0);
        if (k <= 0) return py_is_list(a) ? py_make_list(NULL, 0) : py_make_tuple(NULL, 0);
        size_t la = PY_PTR(a)->list.len;
        size_t total = la * (size_t)k;
        VALUE *items = (VALUE *)alloca(sizeof(VALUE) * (total + 1));
        for (int64_t i = 0; i < k; i++)
            memcpy(items + i * la, PY_PTR(a)->list.items, sizeof(VALUE) * la);
        return py_is_list(a) ? py_make_list(items, total) : py_make_tuple(items, total);
    }
    if (py_int_or_bool(a) && py_int_or_bool(b)) {
        mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
        mpz_mul(za, za, zb);
        VALUE r = py_normalise_int(za);
        mpz_clear(za); mpz_clear(zb);
        return r;
    }
    if ((py_int_or_bool(a) || py_is_float(a)) && (py_int_or_bool(b) || py_is_float(b)))
        return py_make_float(py_to_double(c, a) * py_to_double(c, b));
    py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for *");
}

VALUE
py_truediv(CTX *c, VALUE a, VALUE b)
{
    double bd = py_to_double(c, b);
    if (bd == 0.0) py_raise_exc(c, c->EXC_ZeroDivisionError, "division by zero");
    return py_make_float(py_to_double(c, a) / bd);
}

VALUE
py_fdiv(CTX *c, VALUE a, VALUE b)
{
    if (py_int_or_bool(a) && py_int_or_bool(b)) {
        mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
        if (mpz_sgn(zb) == 0) {
            mpz_clear(za); mpz_clear(zb);
            py_raise_exc(c, c->EXC_ZeroDivisionError, "integer division or modulo by zero");
        }
        mpz_t q; mpz_init(q);
        mpz_fdiv_q(q, za, zb);
        VALUE r = py_normalise_int(q);
        mpz_clear(za); mpz_clear(zb); mpz_clear(q);
        return r;
    }
    double bd = py_to_double(c, b);
    if (bd == 0.0) py_raise_exc(c, c->EXC_ZeroDivisionError, "float floor division by zero");
    return py_make_float(floor(py_to_double(c, a) / bd));
}

VALUE
py_mod(CTX *c, VALUE a, VALUE b)
{
    if (py_int_or_bool(a) && py_int_or_bool(b)) {
        mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
        if (mpz_sgn(zb) == 0) {
            mpz_clear(za); mpz_clear(zb);
            py_raise_exc(c, c->EXC_ZeroDivisionError, "integer division or modulo by zero");
        }
        mpz_t r; mpz_init(r);
        mpz_fdiv_r(r, za, zb);
        VALUE rv = py_normalise_int(r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r);
        return rv;
    }
    double bd = py_to_double(c, b);
    if (bd == 0.0) py_raise_exc(c, c->EXC_ZeroDivisionError, "float modulo");
    double ad = py_to_double(c, a);
    double r = fmod(ad, bd);
    if ((r != 0.0) && ((r < 0) != (bd < 0))) r += bd;
    return py_make_float(r);
}

VALUE
py_pow(CTX *c, VALUE a, VALUE b)
{
    // int ** non-negative-int → bignum (exact)
    if (py_int_or_bool(a) && py_int_or_bool(b)) {
        mpz_t zb; py_to_mpz(c, b, zb);
        if (mpz_sgn(zb) < 0) {
            mpz_clear(zb);
            return py_make_float(pow(py_to_double(c, a), py_to_double(c, b)));
        }
        if (!mpz_fits_ulong_p(zb)) {
            mpz_clear(zb);
            py_raise_exc(c, c->EXC_ValueError, "exponent too large");
        }
        unsigned long e = mpz_get_ui(zb);
        mpz_t za; py_to_mpz(c, a, za);
        mpz_t r; mpz_init(r);
        mpz_pow_ui(r, za, e);
        VALUE rv = py_normalise_int(r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r);
        return rv;
    }
    return py_make_float(pow(py_to_double(c, a), py_to_double(c, b)));
}

VALUE
py_bit_and(CTX *c, VALUE a, VALUE b)
{
    if (!py_int_or_bool(a) || !py_int_or_bool(b))
        py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for &");
    mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
    mpz_and(za, za, zb);
    VALUE r = py_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

VALUE
py_bit_or(CTX *c, VALUE a, VALUE b)
{
    if (!py_int_or_bool(a) || !py_int_or_bool(b))
        py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for |");
    mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
    mpz_ior(za, za, zb);
    VALUE r = py_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

VALUE
py_bit_xor(CTX *c, VALUE a, VALUE b)
{
    if (!py_int_or_bool(a) || !py_int_or_bool(b))
        py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for ^");
    mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
    mpz_xor(za, za, zb);
    VALUE r = py_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

VALUE
py_bit_inv(CTX *c, VALUE a)
{
    if (!py_int_or_bool(a))
        py_raise_exc(c, c->EXC_TypeError, "bad operand type for unary ~");
    mpz_t z; py_to_mpz(c, a, z);
    mpz_com(z, z);
    VALUE r = py_normalise_int(z);
    mpz_clear(z);
    return r;
}

VALUE
py_lshift(CTX *c, VALUE a, VALUE b)
{
    if (!py_int_or_bool(a) || !py_int_or_bool(b))
        py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for <<");
    mpz_t zb; py_to_mpz(c, b, zb);
    if (mpz_sgn(zb) < 0) {
        mpz_clear(zb);
        py_raise_exc(c, c->EXC_ValueError, "negative shift count");
    }
    if (!mpz_fits_ulong_p(zb)) { mpz_clear(zb); py_raise_exc(c, c->EXC_ValueError, "shift too large"); }
    unsigned long s = mpz_get_ui(zb);
    mpz_t za; py_to_mpz(c, a, za);
    mpz_mul_2exp(za, za, s);
    VALUE r = py_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

VALUE
py_rshift(CTX *c, VALUE a, VALUE b)
{
    if (!py_int_or_bool(a) || !py_int_or_bool(b))
        py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for >>");
    mpz_t zb; py_to_mpz(c, b, zb);
    if (mpz_sgn(zb) < 0) {
        mpz_clear(zb);
        py_raise_exc(c, c->EXC_ValueError, "negative shift count");
    }
    if (!mpz_fits_ulong_p(zb)) { mpz_clear(zb); return PY_FIX(0); }
    unsigned long s = mpz_get_ui(zb);
    mpz_t za; py_to_mpz(c, a, za);
    mpz_fdiv_q_2exp(za, za, s);
    VALUE r = py_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

int
py_cmp(CTX *c, VALUE a, VALUE b)
{
    if (py_is_instance(a)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(a)->inst.cls), "__lt__");
        if (m != PY_NONE) {
            VALUE av[2] = { a, b };
            VALUE r = py_apply(c, m, 2, av);
            if (py_is_truthy(r)) return -1;
            // try __eq__ for == 0
            m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(a)->inst.cls), "__eq__");
            if (m != PY_NONE) {
                VALUE r2 = py_apply(c, m, 2, av);
                if (py_is_truthy(r2)) return 0;
            }
            return 1;
        }
    }
    if (py_is_str(a) && py_is_str(b)) {
        size_t la = PY_PTR(a)->str.len, lb = PY_PTR(b)->str.len;
        size_t n = la < lb ? la : lb;
        int r = memcmp(PY_PTR(a)->str.chars, PY_PTR(b)->str.chars, n);
        if (r != 0) return r < 0 ? -1 : 1;
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    if (PY_IS_FIXNUM(a) && PY_IS_FIXNUM(b)) {
        int64_t ai = PY_FIXVAL(a), bi = PY_FIXVAL(b);
        return ai < bi ? -1 : ai > bi ? 1 : 0;
    }
    if (py_int_or_bool(a) && py_int_or_bool(b)) {
        mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
        int r = mpz_cmp(za, zb);
        mpz_clear(za); mpz_clear(zb);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    if ((py_int_or_bool(a) || py_is_float(a)) && (py_int_or_bool(b) || py_is_float(b))) {
        double ad = py_to_double(c, a), bd = py_to_double(c, b);
        return ad < bd ? -1 : ad > bd ? 1 : 0;
    }
    if ((py_is_list(a) && py_is_list(b)) || (py_is_tuple(a) && py_is_tuple(b))) {
        size_t la = PY_PTR(a)->list.len, lb = PY_PTR(b)->list.len;
        size_t n = la < lb ? la : lb;
        for (size_t i = 0; i < n; i++) {
            int r = py_cmp(c, PY_PTR(a)->list.items[i], PY_PTR(b)->list.items[i]);
            if (r != 0) return r;
        }
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    py_raise_exc(c, c->EXC_TypeError, "incomparable operand types");
}

VALUE
py_eq(CTX *c, VALUE a, VALUE b)
{
    VALUE r = py_try_binop_dunder(c, "__eq__", a, b);
    if (r) return r;
    if (a == b) return PY_TRUE;
    if (PY_IS_FIXNUM(a) && PY_IS_FIXNUM(b)) return PY_FALSE;
    if (py_int_or_bool(a) && py_int_or_bool(b)) {
        mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
        bool eq = (mpz_cmp(za, zb) == 0);
        mpz_clear(za); mpz_clear(zb);
        return eq ? PY_TRUE : PY_FALSE;
    }
    if ((py_int_or_bool(a) || py_is_float(a)) && (py_int_or_bool(b) || py_is_float(b)))
        return py_to_double(c, a) == py_to_double(c, b) ? PY_TRUE : PY_FALSE;
    if (py_is_str(a) && py_is_str(b)) {
        if (PY_PTR(a)->str.len != PY_PTR(b)->str.len) return PY_FALSE;
        return memcmp(PY_PTR(a)->str.chars, PY_PTR(b)->str.chars,
                      PY_PTR(a)->str.len) == 0 ? PY_TRUE : PY_FALSE;
    }
    if ((py_is_list(a) && py_is_list(b)) || (py_is_tuple(a) && py_is_tuple(b))) {
        size_t la = PY_PTR(a)->list.len, lb = PY_PTR(b)->list.len;
        if (la != lb) return PY_FALSE;
        for (size_t i = 0; i < la; i++)
            if (py_eq(c, PY_PTR(a)->list.items[i], PY_PTR(b)->list.items[i]) != PY_TRUE)
                return PY_FALSE;
        return PY_TRUE;
    }
    return PY_FALSE;
}

bool
py_eq_bool(CTX *c, VALUE a, VALUE b)
{
    return py_eq(c, a, b) == PY_TRUE;
}

// ---------------------------------------------------------------------------
// Hash.
// ---------------------------------------------------------------------------

uint64_t
py_hash(CTX *c, VALUE v)
{
    if (PY_IS_FIXNUM(v)) {
        uint64_t k = (uint64_t)PY_FIXVAL(v);
        k *= 0x9E3779B97F4A7C15ULL;
        return k ^ (k >> 32);
    }
    if (PY_IS_FLONUM(v)) {
        double d = py_flonum_to_double(v);
        if (d == (double)(int64_t)d) return py_hash(c, PY_FIX((int64_t)d));
        union { uint64_t u; double d; } pun = { .d = d == 0 ? 0 : d };
        return pun.u;
    }
    if (v == PY_NONE)  return 0xDEADBEEFCAFEBABEULL;
    if (v == PY_TRUE)  return 1;
    if (v == PY_FALSE) return 0;
    struct pyobj *o = PY_PTR(v);
    switch (o->type) {
      case PY_T_FLOAT: {
        if (o->dbl == (double)(int64_t)o->dbl) return py_hash(c, PY_FIX((int64_t)o->dbl));
        union { uint64_t u; double d; } pun = { .d = o->dbl == 0 ? 0 : o->dbl };
        return pun.u;
      }
      case PY_T_BIGNUM: {
        // FNV over the limbs.
        uint64_t h = 0xCBF29CE484222325ULL;
        size_t n = mpz_size(o->mpz);
        for (size_t i = 0; i < n; i++) {
            mp_limb_t l = mpz_getlimbn(o->mpz, i);
            h ^= l;
            h *= 0x100000001B3ULL;
        }
        if (mpz_sgn(o->mpz) < 0) h = ~h;
        return h;
      }
      case PY_T_STR: {
        uint64_t h = 0xCBF29CE484222325ULL;
        for (size_t i = 0; i < o->str.len; i++) {
            h ^= (unsigned char)o->str.chars[i];
            h *= 0x100000001B3ULL;
        }
        return h;
      }
      case PY_T_TUPLE: {
        uint64_t h = 0x9E3779B97F4A7C15ULL;
        for (size_t i = 0; i < o->list.len; i++) {
            h = (h ^ py_hash(c, o->list.items[i])) * 0x100000001B3ULL;
        }
        return h;
      }
      default:
        // Use object identity for other types (lists/dicts are unhashable
        // but we fall through here for class/instance/etc.).
        return (uint64_t)(uintptr_t)o * 0x9E3779B97F4A7C15ULL;
    }
}

// ---------------------------------------------------------------------------
// Dict.
// ---------------------------------------------------------------------------

#define DICT_INIT_CAPA 8
#define DICT_LOAD_NUM  2
#define DICT_LOAD_DEN  3

static struct pydict *
pydict_new(void)
{
    struct pydict *d = (struct pydict *)GC_malloc(sizeof(struct pydict));
    d->capa = DICT_INIT_CAPA;
    d->used = 0;
    d->fill = 0;
    d->entries = (struct pydict_entry *)GC_malloc(sizeof(struct pydict_entry) * d->capa);
    return d;
}

VALUE
py_make_dict(void)
{
    struct pyobj *o = py_alloc(PY_T_DICT);
    o->dict = pydict_new();
    return PY_OBJ_VAL(o);
}

VALUE
py_make_set(void)
{
    // A set is implemented as a dict-shaped table where only keys are
    // tracked.  We reuse `pydict` for the storage and ignore values
    // (always PY_NONE) on reads.  The PY_T_SET tag selects the right
    // display / dunder behaviour.
    struct pyobj *o = py_alloc(PY_T_SET);
    o->dict = pydict_new();
    return PY_OBJ_VAL(o);
}

static struct pydict_entry *
pydict_lookup(CTX *c, struct pydict *d, VALUE key, uint64_t h)
{
    size_t mask = d->capa - 1;
    size_t i = (size_t)h & mask;
    size_t step = 0;
    struct pydict_entry *first_tomb = NULL;
    bool key_is_immediate = PY_IS_FIXNUM(key) || key == PY_NONE
                          || key == PY_TRUE  || key == PY_FALSE;
    for (;;) {
        struct pydict_entry *e = &d->entries[i];
        if (e->state == 0) return first_tomb ? first_tomb : e;
        if (e->state == 2) { if (!first_tomb) first_tomb = e; }
        else if (LIKELY(e->hash == h)) {
            // Fast path: identity-equal (same fixnum / None / True /
            // False, or interned-pointer-equal — very common).
            if (LIKELY(e->key == key)) return e;
            // If either key is an immediate, identity is equality.
            if (UNLIKELY(!key_is_immediate &&
                         !(PY_IS_FIXNUM(e->key) || e->key == PY_NONE
                           || e->key == PY_TRUE || e->key == PY_FALSE)
                         && py_eq_bool(c, e->key, key)))
                return e;
        }
        step++;
        i = (i + step) & mask;
    }
}

static void
pydict_resize(CTX *c, struct pydict *d, size_t new_capa)
{
    struct pydict_entry *old = d->entries;
    size_t old_capa = d->capa;
    d->capa = new_capa;
    d->entries = (struct pydict_entry *)GC_malloc(sizeof(struct pydict_entry) * new_capa);
    d->used = 0; d->fill = 0;
    for (size_t i = 0; i < old_capa; i++) {
        if (old[i].state == 1) {
            struct pydict_entry *e = pydict_lookup(c, d, old[i].key, old[i].hash);
            e->key = old[i].key;
            e->value = old[i].value;
            e->hash = old[i].hash;
            e->state = 1;
            d->used++; d->fill++;
        }
    }
}

void
py_dict_set(CTX *c, VALUE dv, VALUE key, VALUE val)
{
    struct pydict *d = PY_PTR(dv)->dict;
    uint64_t h = py_hash(c, key);
    struct pydict_entry *e = pydict_lookup(c, d, key, h);
    if (e->state != 1) {
        if (e->state == 0) d->fill++;
        d->used++;
        e->key = key;
        e->hash = h;
        e->state = 1;
    }
    e->value = val;
    if (d->fill * DICT_LOAD_DEN >= d->capa * DICT_LOAD_NUM)
        pydict_resize(c, d, d->capa * 2);
}

VALUE
py_dict_get(CTX *c, VALUE dv, VALUE key)
{
    struct pydict *d = PY_PTR(dv)->dict;
    uint64_t h = py_hash(c, key);
    struct pydict_entry *e = pydict_lookup(c, d, key, h);
    if (e->state != 1) py_raise_exc(c, c->EXC_KeyError, "key not found");
    return e->value;
}

bool
py_dict_has(CTX *c, VALUE dv, VALUE key)
{
    struct pydict *d = PY_PTR(dv)->dict;
    uint64_t h = py_hash(c, key);
    struct pydict_entry *e = pydict_lookup(c, d, key, h);
    return e->state == 1;
}

bool
py_dict_remove(CTX *c, VALUE dv, VALUE key)
{
    struct pydict *d = PY_PTR(dv)->dict;
    uint64_t h = py_hash(c, key);
    struct pydict_entry *e = pydict_lookup(c, d, key, h);
    if (e->state != 1) return false;
    e->state = 2;
    d->used--;
    return true;
}

// ---------------------------------------------------------------------------
// List ops.
// ---------------------------------------------------------------------------

void
py_list_append(CTX *c, VALUE lv, VALUE v)
{
    (void)c;
    struct pyobj *o = PY_PTR(lv);
    if (o->list.len == o->list.capa) {
        size_t cap = o->list.capa ? o->list.capa * 2 : 4;
        VALUE *items = (VALUE *)GC_malloc(sizeof(VALUE) * cap);
        if (o->list.len) memcpy(items, o->list.items, sizeof(VALUE) * o->list.len);
        o->list.items = items;
        o->list.capa = cap;
    }
    o->list.items[o->list.len++] = v;
}

static int64_t
clamp_idx(int64_t i, int64_t len, bool clamp_for_slice)
{
    if (i < 0) i += len;
    if (clamp_for_slice) {
        if (i < 0) i = 0;
        if (i > len) i = len;
    }
    return i;
}

static int64_t
py_int_to_long(CTX *c, VALUE v)
{
    if (PY_IS_FIXNUM(v)) return PY_FIXVAL(v);
    if (v == PY_TRUE) return 1;
    if (v == PY_FALSE) return 0;
    if (py_is_bignum(v)) {
        if (mpz_fits_slong_p(PY_PTR(v)->mpz)) return mpz_get_si(PY_PTR(v)->mpz);
        py_raise_exc(c, c->EXC_IndexError, "index too large");
    }
    py_raise_exc(c, c->EXC_TypeError, "expected an integer index");
}

VALUE
py_list_get(CTX *c, VALUE seq, VALUE idx)
{
    if (py_is_instance(seq)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(seq)->inst.cls), "__getitem__");
        if (m != PY_NONE) {
            VALUE av[2] = { seq, idx };
            return py_apply(c, m, 2, av);
        }
    }
    if (py_is_list(seq) || py_is_tuple(seq)) {
        int64_t i = py_int_to_long(c, idx);
        int64_t len = (int64_t)PY_PTR(seq)->list.len;
        i = clamp_idx(i, len, false);
        if (i < 0 || i >= len) py_raise_exc(c, c->EXC_IndexError, "index out of range");
        return PY_PTR(seq)->list.items[i];
    }
    if (py_is_str(seq)) {
        int64_t i = py_int_to_long(c, idx);
        int64_t len = (int64_t)PY_PTR(seq)->str.len;
        i = clamp_idx(i, len, false);
        if (i < 0 || i >= len) py_raise_exc(c, c->EXC_IndexError, "string index out of range");
        return py_make_str(PY_PTR(seq)->str.chars + i, 1);
    }
    if (py_is_dict(seq)) {
        return py_dict_get(c, seq, idx);
    }
    py_raise_exc(c, c->EXC_TypeError, "object is not subscriptable");
}

VALUE
py_list_set(CTX *c, VALUE seq, VALUE idx, VALUE val)
{
    if (py_is_instance(seq)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(seq)->inst.cls), "__setitem__");
        if (m != PY_NONE) {
            VALUE av[3] = { seq, idx, val };
            return py_apply(c, m, 3, av);
        }
    }
    if (py_is_list(seq)) {
        int64_t i = py_int_to_long(c, idx);
        int64_t len = (int64_t)PY_PTR(seq)->list.len;
        i = clamp_idx(i, len, false);
        if (i < 0 || i >= len) py_raise_exc(c, c->EXC_IndexError, "list index out of range");
        PY_PTR(seq)->list.items[i] = val;
        return PY_NONE;
    }
    if (py_is_dict(seq)) { py_dict_set(c, seq, idx, val); return PY_NONE; }
    py_raise_exc(c, c->EXC_TypeError, "object does not support item assignment");
}

VALUE
py_list_slice(CTX *c, VALUE seq, VALUE start, VALUE stop, VALUE step)
{
    int64_t len;
    bool is_str = py_is_str(seq);
    if (is_str) len = (int64_t)PY_PTR(seq)->str.len;
    else if (py_is_list(seq) || py_is_tuple(seq)) len = (int64_t)PY_PTR(seq)->list.len;
    else py_raise_exc(c, c->EXC_TypeError, "object is not sliceable");

    int64_t st = (step == PY_NONE) ? 1 : py_int_to_long(c, step);
    if (st == 0) py_raise_exc(c, c->EXC_ValueError, "slice step cannot be zero");

    int64_t a, b;
    if (start == PY_NONE) a = (st > 0) ? 0 : len - 1;
    else                  a = clamp_idx(py_int_to_long(c, start), len, st > 0);
    if (stop == PY_NONE)  b = (st > 0) ? len : -1;
    else                  b = clamp_idx(py_int_to_long(c, stop), len, st > 0);

    // Pythonic slice clamping for negative step.
    if (st < 0) {
        if (a >= len) a = len - 1;
        if (b < -1)   b = -1;
    } else {
        if (a < 0) a = 0;
        if (b > len) b = len;
    }

    size_t n = 0;
    if (st > 0 && a < b) n = (size_t)((b - a + st - 1) / st);
    else if (st < 0 && a > b) n = (size_t)((a - b - st - 1) / -st);

    if (is_str) {
        // Step 1 → borrow a contiguous range of the parent buffer.
        if (st == 1)
            return py_make_str_borrow(PY_PTR(seq)->str.chars + a, n);
        char *buf = (char *)GC_malloc_atomic(n + 1);
        for (size_t i = 0; i < n; i++) buf[i] = PY_PTR(seq)->str.chars[a + (int64_t)i * st];
        buf[n] = '\0';
        return py_make_str_take(buf, n);
    }
    VALUE *items = n ? (VALUE *)alloca(sizeof(VALUE) * n) : NULL;
    for (size_t i = 0; i < n; i++)
        items[i] = PY_PTR(seq)->list.items[a + (int64_t)i * st];
    return py_is_list(seq) ? py_make_list(items, n) : py_make_tuple(items, n);
}

// Slice-assign for lists.  For step == 1, supports general resize:
// `a[i:j] = list` deletes a[i:j] and inserts the items from `val` at i.
// Other steps require len(val) == len(slice).
void
py_list_slice_set(CTX *c, VALUE seq, VALUE start, VALUE stop, VALUE step, VALUE val)
{
    if (!py_is_list(seq))
        py_raise_exc(c, c->EXC_TypeError, "slice assignment requires a list");
    int64_t st = (step == PY_NONE) ? 1 : py_int_to_long(c, step);
    if (st == 0) py_raise_exc(c, c->EXC_ValueError, "slice step cannot be zero");
    int64_t len = (int64_t)PY_PTR(seq)->list.len;
    int64_t a = (start == PY_NONE) ? (st > 0 ? 0 : len - 1) : py_int_to_long(c, start);
    int64_t b = (stop  == PY_NONE) ? (st > 0 ? len : -1)    : py_int_to_long(c, stop);
    if (a < 0) a += len;
    if (b < 0 && stop != PY_NONE) b += len;
    if (st > 0) { if (a < 0) a = 0; if (b > len) b = len; }
    else        { if (a >= len) a = len - 1; }

    // Collect val's elements.
    VALUE *items = NULL;
    size_t nval = 0;
    if (py_is_list(val) || py_is_tuple(val)) {
        nval = PY_PTR(val)->list.len;
        items = PY_PTR(val)->list.items;
    } else {
        // iterable → buffer
        struct py_iter it; py_iter_init(c, &it, val);
        size_t cap = 16; nval = 0;
        items = (VALUE *)GC_malloc(sizeof(VALUE) * cap);
        VALUE x;
        while (py_iter_next(c, &it, &x)) {
            if (nval == cap) { cap *= 2; items = (VALUE *)GC_realloc(items, sizeof(VALUE) * cap); }
            items[nval++] = x;
        }
    }

    if (st == 1) {
        // General-purpose case: build a new items array.
        if (a > len) a = len;
        if (b > len) b = len;
        if (b < a)  b = a;
        size_t prefix = (size_t)a;
        size_t suffix_off = (size_t)b;
        size_t suffix_len = (size_t)(len - b);
        size_t new_len = prefix + nval + suffix_len;
        size_t cap = new_len < 4 ? 4 : new_len;
        VALUE *out = (VALUE *)GC_malloc(sizeof(VALUE) * cap);
        if (prefix) memcpy(out, PY_PTR(seq)->list.items, sizeof(VALUE) * prefix);
        if (nval)   memcpy(out + prefix, items, sizeof(VALUE) * nval);
        if (suffix_len) memcpy(out + prefix + nval,
                               PY_PTR(seq)->list.items + suffix_off,
                               sizeof(VALUE) * suffix_len);
        PY_PTR(seq)->list.items = out;
        PY_PTR(seq)->list.len = new_len;
        PY_PTR(seq)->list.capa = cap;
        return;
    }

    // step != 1: requires matching length.
    size_t target_n = 0;
    if (st > 0 && a < b) target_n = (size_t)((b - a + st - 1) / st);
    else if (st < 0 && a > b) target_n = (size_t)((a - b - st - 1) / -st);
    if (target_n != nval)
        py_raise_exc(c, c->EXC_ValueError,
                     "slice assignment length mismatch (%zu vs %zu)", target_n, nval);
    for (size_t i = 0; i < nval; i++)
        PY_PTR(seq)->list.items[a + (int64_t)i * st] = items[i];
}

size_t
py_seq_len(CTX *c, VALUE v)
{
    if (py_is_str(v))   return PY_PTR(v)->str.len;
    if (py_is_list(v) || py_is_tuple(v)) return PY_PTR(v)->list.len;
    if (py_is_dict(v) || py_is_set(v))  return PY_PTR(v)->dict->used;
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__len__");
        if (m != PY_NONE) {
            VALUE av[1] = { v };
            VALUE r = py_apply(c, m, 1, av);
            if (PY_IS_FIXNUM(r)) return (size_t)PY_FIXVAL(r);
        }
    }
    py_raise_exc(c, c->EXC_TypeError, "object has no len()");
}

bool
py_contains(CTX *c, VALUE container, VALUE v)
{
    if (py_is_list(container) || py_is_tuple(container)) {
        size_t n = PY_PTR(container)->list.len;
        for (size_t i = 0; i < n; i++)
            if (py_eq_bool(c, PY_PTR(container)->list.items[i], v)) return true;
        return false;
    }
    if (py_is_dict(container) || py_is_set(container)) return py_dict_has(c, container, v);
    if (py_is_str(container) && py_is_str(v)) {
        return memmem(PY_PTR(container)->str.chars, PY_PTR(container)->str.len,
                      PY_PTR(v)->str.chars, PY_PTR(v)->str.len) != NULL;
    }
    if (py_is_range(container) && py_int_or_bool(v)) {
        int64_t x = py_int_to_long(c, v);
        struct pyobj *r = PY_PTR(container);
        if (r->range.step > 0)
            return x >= r->range.start && x < r->range.stop &&
                   ((x - r->range.start) % r->range.step == 0);
        else
            return x <= r->range.start && x > r->range.stop &&
                   ((r->range.start - x) % (-r->range.step) == 0);
    }
    py_raise_exc(c, c->EXC_TypeError, "argument is not iterable for `in`");
}

// ---------------------------------------------------------------------------
// Iter protocol.
// ---------------------------------------------------------------------------

void
py_iter_init(CTX *c, struct py_iter *it, VALUE iterable)
{
    it->container = iterable;
    it->i = 0;
    it->step = 1;
    if (py_is_list(iterable) || py_is_tuple(iterable)) {
        it->kind = 0;
        it->end = (int64_t)PY_PTR(iterable)->list.len;
        return;
    }
    if (py_is_str(iterable)) {
        it->kind = 1;
        it->end = (int64_t)PY_PTR(iterable)->str.len;
        return;
    }
    if (py_is_range(iterable)) {
        it->kind = 2;
        it->i    = PY_PTR(iterable)->range.start;
        it->end  = PY_PTR(iterable)->range.stop;
        it->step = PY_PTR(iterable)->range.step;
        return;
    }
    if (py_is_dict(iterable) || py_is_set(iterable)) {
        it->kind = 3;
        it->end = (int64_t)PY_PTR(iterable)->dict->capa;
        return;
    }
    py_raise_exc(c, c->EXC_TypeError, "object is not iterable");
}

bool
py_iter_next(CTX *c, struct py_iter *it, VALUE *out)
{
    (void)c;
    switch (it->kind) {
      case 0:
        if (it->i >= it->end) return false;
        *out = PY_PTR(it->container)->list.items[it->i++];
        return true;
      case 1:
        if (it->i >= it->end) return false;
        *out = py_make_str(PY_PTR(it->container)->str.chars + it->i, 1);
        it->i++;
        return true;
      case 2:
        if (it->step > 0 ? it->i >= it->end : it->i <= it->end) return false;
        *out = py_make_int(it->i);
        it->i += it->step;
        return true;
      case 3: {
        struct pydict *d = PY_PTR(it->container)->dict;
        while (it->i < it->end) {
            if (d->entries[it->i].state == 1) {
                *out = d->entries[it->i].key;
                it->i++;
                return true;
            }
            it->i++;
        }
        return false;
      }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Attribute access.  For instances, look up self.attrs first, then the
// class's method table (binding `self`).  For built-in types, look up
// in a per-type method registry.
// ---------------------------------------------------------------------------

struct type_method { const char *name; py_builtin_fn fn; int min_argc, max_argc; };

// Forward decls.
static struct type_method str_methods[];
static struct type_method list_methods[];
static struct type_method dict_methods[];
static struct type_method set_methods[];

static VALUE
make_builtin_bound(VALUE self, struct type_method *tm)
{
    VALUE fn = py_make_builtin(tm->name, tm->fn, tm->min_argc, tm->max_argc);
    return py_make_bound(self, fn);
}

VALUE
py_builtin_method(CTX *c, VALUE recv, const char *name)
{
    struct type_method *tbl;
    if (py_is_str(recv))      tbl = str_methods;
    else if (py_is_list(recv)) tbl = list_methods;
    else if (py_is_dict(recv)) tbl = dict_methods;
    else if (py_is_set(recv))  tbl = set_methods;
    else { (void)c; return PY_NONE; }
    for (int i = 0; tbl[i].name; i++)
        if (strcmp(tbl[i].name, name) == 0) return make_builtin_bound(recv, &tbl[i]);
    return PY_NONE;
}

// Cold-path method resolution used by inline-cached `node_method_*`.
// On a builtin-type method hit, stamps the cache with (type_tag, fn) so
// subsequent calls take the inline fast path (no bound-method alloc, no
// table strcmp).  On instance methods or class-level lookups, returns
// the resolved callable as-is and clears the cache so the type_tag
// check on the next call falls through correctly.
VALUE
py_method_resolve(CTX *c, VALUE recv, const char *name, struct method_cache *cache)
{
    // Builtin type method?
    if (PY_IS_PTR(recv)) {
        int tag = PY_PTR(recv)->type;
        struct type_method *tbl = NULL;
        if (tag == PY_T_STR)       tbl = str_methods;
        else if (tag == PY_T_LIST) tbl = list_methods;
        else if (tag == PY_T_DICT) tbl = dict_methods;
        else if (tag == PY_T_SET)  tbl = set_methods;
        if (tbl) {
            for (int i = 0; tbl[i].name; i++) {
                if (strcmp(tbl[i].name, name) == 0) {
                    cache->type_tag = tag;
                    cache->fn = (void *)tbl[i].fn;
                    // Return a bound builtin for the slow path that
                    // invoked us; subsequent calls hit the inline cache
                    // and skip this entirely.
                    return make_builtin_bound(recv, &tbl[i]);
                }
            }
        }
    }
    // Instance / class method (no inline-cache-able fast path).
    cache->type_tag = -1;
    cache->fn = NULL;
    return py_getattr(c, recv, name);
}

VALUE
py_getattr(CTX *c, VALUE v, const char *name)
{
    if (py_is_instance(v)) {
        struct pyobj *o = PY_PTR(v);
        if (o->inst.attrs) {
            VALUE key = py_make_str(name, strlen(name));
            uint64_t h = py_hash(c, key);
            struct pydict_entry *e = pydict_lookup(c, o->inst.attrs, key, h);
            if (e->state == 1) return e->value;
        }
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), name);
        if (m != PY_NONE) {
            // Honor descriptor wrappers.
            if (PY_IS_PTR(m)) {
                int t = PY_PTR(m)->type;
                if (t == PY_T_STATICMETHOD) return PY_PTR(m)->wrap.wrapped;
                if (t == PY_T_CLASSMETHOD) return py_make_bound(PY_OBJ_VAL(o->inst.cls), PY_PTR(m)->wrap.wrapped);
                if (t == PY_T_PROPERTY) {
                    VALUE av[1] = { v };
                    return py_apply(c, PY_PTR(m)->wrap.wrapped, 1, av);
                }
            }
            return py_make_bound(v, m);
        }
        py_raise_exc(c, c->EXC_AttributeError, "'%s' object has no attribute '%s'",
                     o->inst.cls->cls.name, name);
    }
    if (py_is_class(v)) {
        VALUE m = py_class_lookup_method(v, name);
        if (m != PY_NONE) {
            if (PY_IS_PTR(m)) {
                int t = PY_PTR(m)->type;
                if (t == PY_T_STATICMETHOD) return PY_PTR(m)->wrap.wrapped;
                if (t == PY_T_CLASSMETHOD)  return py_make_bound(v, PY_PTR(m)->wrap.wrapped);
            }
            return m;
        }
        py_raise_exc(c, c->EXC_AttributeError, "type object '%s' has no attribute '%s'",
                     PY_PTR(v)->cls.name, name);
    }
    VALUE m = py_builtin_method(c, v, name);
    if (m != PY_NONE) return m;
    py_raise_exc(c, c->EXC_AttributeError, "object has no attribute '%s'", name);
}

void
py_setattr(CTX *c, VALUE v, const char *name, VALUE val)
{
    if (py_is_instance(v)) {
        struct pyobj *o = PY_PTR(v);
        if (!o->inst.attrs) o->inst.attrs = pydict_new();
        VALUE key = py_make_str(name, strlen(name));
        uint64_t h = py_hash(c, key);
        struct pydict_entry *e = pydict_lookup(c, o->inst.attrs, key, h);
        if (e->state != 1) { o->inst.attrs->used++; if (e->state == 0) o->inst.attrs->fill++; }
        e->key = key; e->value = val; e->hash = h; e->state = 1;
        if (o->inst.attrs->fill * DICT_LOAD_DEN >= o->inst.attrs->capa * DICT_LOAD_NUM)
            pydict_resize(c, o->inst.attrs, o->inst.attrs->capa * 2);
        return;
    }
    py_raise_exc(c, c->EXC_AttributeError, "object does not support attribute assignment");
}

// ---------------------------------------------------------------------------
// Apply.
// ---------------------------------------------------------------------------

// Generic positional + kwarg apply for user-defined functions.
//
// Slot layout:
//   [0 .. n_pos_named)            : positional-or-keyword params
//   [n_pos_named]                 : *args (if has_varargs)
//   [n_pos_named + has_va .. *)   : keyword-only params
//   [..., last slot if has_kw]    : **kwargs
//
// `defaults[i]` is the default for slot i; (VALUE)0 means "required".
// (Real values can never be 0 — fixnums set bit 0; pointers are
// non-NULL after py_alloc.)
static VALUE
py_apply_kw_func(CTX *c, VALUE fn, int argc, VALUE *argv,
                 int kwc, const char **kwnames, VALUE *kwvalues)
{
    struct pyobj *f = PY_PTR(fn);
    int nparams = f->func.nparams;
    int n_pos_named = f->func.n_pos_named;
    bool has_va = f->func.has_varargs;
    bool has_kw = f->func.has_kwargs;
    int va_slot = has_va ? n_pos_named : -1;
    int kwonly_start = n_pos_named + (has_va ? 1 : 0);
    int n_kwonly = nparams - kwonly_start - (has_kw ? 1 : 0);
    int kw_slot = has_kw ? (kwonly_start + n_kwonly) : -1;

    struct pyframe *new_env = py_new_frame(f->func.env, f->func.nlocals);
    bool *filled = (bool *)alloca(sizeof(bool) * (nparams > 0 ? nparams : 1));
    for (int i = 0; i < nparams; i++) filled[i] = false;

    // Place positional.
    int pos_into = argc < n_pos_named ? argc : n_pos_named;
    for (int i = 0; i < pos_into; i++) { new_env->slots[i] = argv[i]; filled[i] = true; }

    // *args.
    if (has_va) {
        int n_extra = argc - pos_into;
        VALUE *items = n_extra > 0 ? (VALUE *)alloca(sizeof(VALUE) * n_extra) : NULL;
        for (int i = 0; i < n_extra; i++) items[i] = argv[pos_into + i];
        new_env->slots[va_slot] = py_make_tuple(items, n_extra);
        filled[va_slot] = true;
    } else if (argc > n_pos_named) {
        py_raise_exc(c, c->EXC_TypeError,
                     "%s() got %d positional arg(s), expected at most %d",
                     f->func.name ? f->func.name : "<anonymous>", argc, n_pos_named);
    }

    // **kwargs.
    if (has_kw) {
        new_env->slots[kw_slot] = py_make_dict();
        filled[kw_slot] = true;
    }

    // Place each kwarg.
    for (int i = 0; i < kwc; i++) {
        int slot = -1;
        if (f->func.param_names) {
            for (int j = 0; j < n_pos_named; j++) {
                if (f->func.param_names[j] && strcmp(f->func.param_names[j], kwnames[i]) == 0) {
                    slot = j; break;
                }
            }
            if (slot < 0) {
                for (int j = kwonly_start; j < kwonly_start + n_kwonly; j++) {
                    if (f->func.param_names[j] && strcmp(f->func.param_names[j], kwnames[i]) == 0) {
                        slot = j; break;
                    }
                }
            }
        }
        if (slot >= 0) {
            if (filled[slot]) {
                py_raise_exc(c, c->EXC_TypeError,
                             "%s() got multiple values for argument '%s'",
                             f->func.name ? f->func.name : "<anonymous>", kwnames[i]);
            }
            new_env->slots[slot] = kwvalues[i];
            filled[slot] = true;
        } else if (has_kw) {
            py_dict_set(c, new_env->slots[kw_slot],
                        py_make_str(kwnames[i], strlen(kwnames[i])), kwvalues[i]);
        } else {
            py_raise_exc(c, c->EXC_TypeError,
                         "%s() got an unexpected keyword argument '%s'",
                         f->func.name ? f->func.name : "<anonymous>", kwnames[i]);
        }
    }

    // Fill defaults / raise on required missing.
    for (int i = 0; i < nparams; i++) {
        if (filled[i]) continue;
        if (i == va_slot || i == kw_slot) continue;
        VALUE d = f->func.defaults[i];
        if (d == (VALUE)0) {
            py_raise_exc(c, c->EXC_TypeError,
                         "%s() missing required argument '%s'",
                         f->func.name ? f->func.name : "<anonymous>",
                         (f->func.param_names && f->func.param_names[i])
                             ? f->func.param_names[i] : "?");
        }
        new_env->slots[i] = d;
    }

    struct pyframe *saved = c->env;
    c->env = new_env;
    EVAL(c, f->func.body);
    c->env = saved;
    if (c->state == PY_STATE_RETURN) {
        VALUE r = c->state_value;
        c->state = PY_STATE_NORMAL;
        c->state_value = PY_NONE;
        return r;
    }
    if (c->state == PY_STATE_RAISE) return PY_NONE;
    return PY_NONE;
}

// Public entry for kwarg / *args expansion.  Handles bound methods
// (prepend self) and class calls (instantiate then call __init__);
// otherwise dispatches to py_apply_kw_func or, when there are no
// kwargs / varargs, to the regular py_apply slow path.
VALUE
py_apply_kw(CTX *c, VALUE fn, int argc, VALUE *argv,
            int kwc, const char **kwnames, VALUE *kwvalues)
{
    if (py_is_bound(fn)) {
        struct pyobj *bm = PY_PTR(fn);
        VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
        av[0] = bm->bound.self;
        for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
        return py_apply_kw(c, bm->bound.func, argc + 1, av, kwc, kwnames, kwvalues);
    }
    if (py_is_class(fn)) {
        VALUE inst = py_make_instance(fn);
        if (PY_PTR(fn)->cls.is_exception) {
            py_setattr(c, inst, "args", py_make_tuple(argv, argc));
            if (argc >= 1 && py_is_str(argv[0])) py_setattr(c, inst, "message", argv[0]);
        }
        VALUE init = py_class_lookup_method(fn, "__init__");
        if (init != PY_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = inst;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            py_apply_kw(c, init, argc + 1, av, kwc, kwnames, kwvalues);
            if (UNLIKELY(c->state == PY_STATE_RAISE)) return PY_NONE;
        }
        return inst;
    }
    if (py_is_func(fn)) return py_apply_kw_func(c, fn, argc, argv, kwc, kwnames, kwvalues);
    if (py_is_builtin(fn)) {
        if (kwc > 0) py_raise_exc(c, c->EXC_TypeError,
                                  "%s() does not accept keyword arguments",
                                  PY_PTR(fn)->builtin.name);
        // delegate via py_apply (slow path is fine)
        return py_apply_slow(c, fn, argc, argv);
    }
    py_raise_exc(c, c->EXC_TypeError, "object is not callable");
}

// Slow-path apply: bound / class / builtin / func-with-defaults / wrong type.
// The closure-with-matching-arity fast path lives inline in `py_apply` in
// node.h so SD code folds the call setup directly.
VALUE
py_apply_slow(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    if (py_is_bound(fn)) {
        struct pyobj *bm = PY_PTR(fn);
        VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
        av[0] = bm->bound.self;
        for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
        return py_apply(c, bm->bound.func, argc + 1, av);
    }
    if (py_is_class(fn)) {
        VALUE inst = py_make_instance(fn);
        if (PY_PTR(fn)->cls.is_exception) {
            py_setattr(c, inst, "args", py_make_tuple(argv, argc));
            if (argc >= 1 && py_is_str(argv[0])) py_setattr(c, inst, "message", argv[0]);
        }
        VALUE init = py_class_lookup_method(fn, "__init__");
        if (init != PY_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = inst;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            py_apply(c, init, argc + 1, av);
            if (UNLIKELY(c->state == PY_STATE_RAISE)) return PY_NONE;
        }
        return inst;
    }
    if (py_is_func(fn)) {
        // All other cases (default args, *args, **kwargs, arity
        // mismatch) route through the keyword-aware dispatcher.
        return py_apply_kw_func(c, fn, argc, argv, 0, NULL, NULL);
    }
    if (py_is_builtin(fn)) {
        struct pyobj *f = PY_PTR(fn);
        if (argc < f->builtin.min_argc ||
            (f->builtin.max_argc >= 0 && argc > f->builtin.max_argc))
            py_raise_exc(c, c->EXC_TypeError,
                         "%s() takes %d-%d arguments but %d were given",
                         f->builtin.name, f->builtin.min_argc,
                         f->builtin.max_argc, argc);
        return f->builtin.fn(c, argc, argv);
    }
    py_raise_exc(c, c->EXC_TypeError, "object is not callable");
}

// ---------------------------------------------------------------------------
// Display + repr.
// ---------------------------------------------------------------------------

void
py_display(FILE *fp, VALUE v, bool repr)
{
    if (PY_IS_FIXNUM(v)) { fprintf(fp, "%ld", (long)PY_FIXVAL(v)); return; }
    if (PY_IS_FLONUM(v)) {
        double d = py_flonum_to_double(v);
        char buf[64]; snprintf(buf, sizeof(buf), "%g", d);
        bool has_marker = false;
        for (const char *p = buf; *p; p++)
            if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') { has_marker = true; break; }
        fputs(buf, fp);
        if (!has_marker) fputs(".0", fp);
        return;
    }
    if (v == PY_NONE)  { fputs("None", fp);  return; }
    if (v == PY_TRUE)  { fputs("True", fp);  return; }
    if (v == PY_FALSE) { fputs("False", fp); return; }
    struct pyobj *o = PY_PTR(v);
    switch (o->type) {
      case PY_T_FLOAT: {
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", o->dbl);
        bool has_marker = false;
        for (const char *p = buf; *p; p++)
            if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') {
                has_marker = true; break;
            }
        fputs(buf, fp);
        if (!has_marker) fputs(".0", fp);
        return;
      }
      case PY_T_BIGNUM: {
        char *s = mpz_get_str(NULL, 10, o->mpz);
        fputs(s, fp);
        return;
      }
      case PY_T_STR:
        if (repr) {
            fputc('\'', fp);
            for (size_t i = 0; i < o->str.len; i++) {
                char ch = o->str.chars[i];
                if      (ch == '\\') fputs("\\\\", fp);
                else if (ch == '\'') fputs("\\'", fp);
                else if (ch == '\n') fputs("\\n", fp);
                else if (ch == '\t') fputs("\\t", fp);
                else                 fputc(ch, fp);
            }
            fputc('\'', fp);
        } else {
            fwrite(o->str.chars, 1, o->str.len, fp);
        }
        return;
      case PY_T_LIST:
        fputc('[', fp);
        for (size_t i = 0; i < o->list.len; i++) {
            if (i) fputs(", ", fp);
            py_display(fp, o->list.items[i], true);
        }
        fputc(']', fp);
        return;
      case PY_T_TUPLE:
        fputc('(', fp);
        for (size_t i = 0; i < o->list.len; i++) {
            if (i) fputs(", ", fp);
            py_display(fp, o->list.items[i], true);
        }
        if (o->list.len == 1) fputc(',', fp);
        fputc(')', fp);
        return;
      case PY_T_DICT: {
        fputc('{', fp);
        struct pydict *d = o->dict;
        size_t printed = 0;
        for (size_t i = 0; i < d->capa; i++) {
            if (d->entries[i].state == 1) {
                if (printed++) fputs(", ", fp);
                py_display(fp, d->entries[i].key, true);
                fputs(": ", fp);
                py_display(fp, d->entries[i].value, true);
            }
        }
        fputc('}', fp);
        return;
      }
      case PY_T_SET: {
        struct pydict *d = o->dict;
        if (d->used == 0) { fputs("set()", fp); return; }
        fputc('{', fp);
        size_t printed = 0;
        for (size_t i = 0; i < d->capa; i++) {
            if (d->entries[i].state == 1) {
                if (printed++) fputs(", ", fp);
                py_display(fp, d->entries[i].key, true);
            }
        }
        fputc('}', fp);
        return;
      }
      case PY_T_RANGE:
        fprintf(fp, "range(%lld, %lld",
                (long long)o->range.start, (long long)o->range.stop);
        if (o->range.step != 1) fprintf(fp, ", %lld", (long long)o->range.step);
        fputc(')', fp);
        return;
      case PY_T_FUNC:
        fprintf(fp, "<function %s>", o->func.name ? o->func.name : "?");
        return;
      case PY_T_BUILTIN:
        fprintf(fp, "<built-in function %s>", o->builtin.name);
        return;
      case PY_T_BOUND_METHOD:
        fprintf(fp, "<bound method>");
        return;
      case PY_T_CLASS:
        fprintf(fp, "<class '%s'>", o->cls.name);
        return;
      case PY_T_INSTANCE: {
        // Defer to __str__ / __repr__ if defined.  We need a CTX to
        // call methods, but py_display doesn't take one — work around
        // by stashing it in a TLS-ish "current ctx" pointer set by
        // bi_print / py_to_str.  For v0, use a simpler approach: just
        // walk class methods directly via cached ctx.
        extern CTX *py_current_ctx;
        if (py_current_ctx) {
            const char *m_name = repr ? "__repr__" : "__str__";
            VALUE m = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), m_name);
            if (m == PY_NONE && !repr)
                m = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), "__repr__");
            if (m != PY_NONE) {
                VALUE av[1] = { v };
                VALUE r = py_apply(py_current_ctx, m, 1, av);
                if (py_is_str(r)) { fwrite(PY_PTR(r)->str.chars, 1, PY_PTR(r)->str.len, fp); return; }
            }
        }
        fprintf(fp, "<%s object>", o->inst.cls->cls.name);
        return;
      }
      default:
        fputs("<object>", fp);
        return;
    }
}

VALUE
py_to_str(CTX *c, VALUE v)
{
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__str__");
        if (m == PY_NONE)
            m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__repr__");
        if (m != PY_NONE) {
            VALUE av[1] = { v };
            VALUE r = py_apply(c, m, 1, av);
            if (py_is_str(r)) return r;
        }
    }
    if (py_is_str(v)) return v;
    char buf[256];
    FILE *mfp = fmemopen(buf, sizeof(buf) - 1, "w");
    if (mfp) {
        py_display(mfp, v, false);
        fflush(mfp);
        long len = ftell(mfp);
        fclose(mfp);
        if (len >= 0 && (size_t)len < sizeof(buf) - 1) return py_make_str(buf, (size_t)len);
    }
    // fallback: large repr — alloc dynamically.
    size_t cap = 1024;
    char *big = (char *)GC_malloc_atomic(cap);
    FILE *bfp = open_memstream(&big, &cap);
    py_display(bfp, v, false);
    fclose(bfp);
    VALUE r = py_make_str(big, strlen(big));
    return r;
}

VALUE
py_to_repr(CTX *c, VALUE v)
{
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__repr__");
        if (m != PY_NONE) {
            VALUE av[1] = { v };
            VALUE r = py_apply(c, m, 1, av);
            if (py_is_str(r)) return r;
        }
    }
    char *big = NULL;
    size_t cap = 0;
    FILE *bfp = open_memstream(&big, &cap);
    py_display(bfp, v, true);
    fclose(bfp);
    VALUE r = py_make_str(big, strlen(big));
    free(big);
    return r;
}

// ---------------------------------------------------------------------------
// Exception matching.
// ---------------------------------------------------------------------------

bool
py_exc_matches(CTX *c, VALUE exc, VALUE cls)
{
    (void)c;
    if (!py_is_instance(exc)) return false;
    if (!py_is_class(cls)) return false;
    struct pyobj *eo = PY_PTR(exc);
    VALUE k = PY_OBJ_VAL(eo->inst.cls);
    while (k != PY_NONE && py_is_class(k)) {
        if (k == cls) return true;
        k = PY_PTR(k)->cls.base;
    }
    return false;
}

// ---------------------------------------------------------------------------
// try/except/finally driver.  Sits behind `node_try` so the setjmp lives
// in a function the C compiler doesn't try to inline (EVAL_node_try is
// generated with always_inline by ASTroGen).
// ---------------------------------------------------------------------------

void
py_run_try(CTX *c, NODE *body, uint32_t handlers_idx, uint32_t nhandlers, NODE *else_body, NODE *finally_body)
{
    jmp_buf jb;
    int saved_top = c->try_top;
    if (c->try_top < 64) c->try_stack[c->try_top++] = &jb;

    bool caught_raise = false;
    if (setjmp(jb) == 0) {
        EVAL(c, body);
        if (c->state == PY_STATE_RAISE) caught_raise = true;
    } else {
        caught_raise = true;
    }
    c->try_top = saved_top;

    if (caught_raise) {
        VALUE exc = c->state_value;
        for (uint32_t i = 0; i < nhandlers; i++) {
            struct pyhandler *h = &PYSTRO_HANDLERS[handlers_idx + i];
            VALUE cls_val = PY_NONE;
            if (h->exc_class) {
                int sst = c->state; VALUE sval = c->state_value;
                c->state = PY_STATE_NORMAL; c->state_value = PY_NONE;
                cls_val = EVAL(c, h->exc_class);
                if (c->state != PY_STATE_NORMAL) goto run_finally;
                c->state = sst; c->state_value = sval;
            }
            if (!h->exc_class || py_exc_matches(c, exc, cls_val)) {
                c->state = PY_STATE_NORMAL;
                c->state_value = PY_NONE;
                if (h->name) {
                    if (h->name_is_global) py_global_set(c, h->name, exc);
                    else                   c->env->slots[h->name_slot] = exc;
                }
                EVAL(c, h->body);
                goto run_finally;
            }
        }
    } else if (else_body) {
        // No exception → run else clause (after body, before finally).
        EVAL(c, else_body);
    }
  run_finally:
    if (finally_body) {
        int sst = c->state; VALUE sval = c->state_value;
        c->state = PY_STATE_NORMAL; c->state_value = PY_NONE;
        EVAL(c, finally_body);
        if (c->state == PY_STATE_NORMAL) { c->state = sst; c->state_value = sval; }
    }
}

// ---------------------------------------------------------------------------
// Tuple-unpacking assignment (struct pyunpack_target in context.h).
// ---------------------------------------------------------------------------

void
py_unpack_assign(CTX *c, struct pyunpack_target *targets, uint32_t n, VALUE rhs)
{
    if (!(py_is_list(rhs) || py_is_tuple(rhs)))
        py_raise_exc(c, c->EXC_TypeError, "cannot unpack non-iterable");
    if (PY_PTR(rhs)->list.len != n)
        py_raise_exc(c, c->EXC_ValueError,
                     "expected %u values, got %zu", n, PY_PTR(rhs)->list.len);
    for (uint32_t i = 0; i < n; i++) {
        VALUE v = PY_PTR(rhs)->list.items[i];
        if (targets[i].is_local) c->env->slots[targets[i].slot] = v;
        else                     py_global_set(c, targets[i].global_name, v);
    }
}

// ---------------------------------------------------------------------------
// Built-in type methods.
// ---------------------------------------------------------------------------

static VALUE
sm_split(CTX *c, int argc, VALUE *argv)
{
    VALUE self = argv[0];
    if (!py_is_str(self)) py_raise_exc(c, c->EXC_TypeError, "split: not str");
    const char *s = PY_PTR(self)->str.chars;
    size_t len = PY_PTR(self)->str.len;
    VALUE result = py_make_list(NULL, 0);
    if (argc == 1) {
        // Split on runs of whitespace — produce sub-strings that
        // borrow into the parent buffer.  No char-buffer allocation
        // per piece; only the pyobj header.
        size_t i = 0;
        while (i < len) {
            while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            if (i >= len) break;
            size_t j = i;
            while (j < len && !(s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
            py_list_append(c, result, py_make_str_borrow(s + i, j - i));
            i = j;
        }
        return result;
    }
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "split sep must be str");
    const char *sep = PY_PTR(argv[1])->str.chars;
    size_t slen = PY_PTR(argv[1])->str.len;
    if (slen == 0) py_raise_exc(c, c->EXC_ValueError, "empty separator");
    size_t i = 0;
    while (i <= len) {
        const char *p = i + slen <= len ? memmem(s + i, len - i, sep, slen) : NULL;
        if (!p) { py_list_append(c, result, py_make_str_borrow(s + i, len - i)); break; }
        py_list_append(c, result, py_make_str_borrow(s + i, (size_t)(p - (s + i))));
        i = (size_t)(p - s) + slen;
    }
    return result;
}

static VALUE
sm_join(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE self = argv[0];
    VALUE seq  = argv[1];
    if (!(py_is_list(seq) || py_is_tuple(seq)))
        py_raise_exc(c, c->EXC_TypeError, "join argument must be iterable");
    const char *sep = PY_PTR(self)->str.chars;
    size_t slen = PY_PTR(self)->str.len;
    size_t n = PY_PTR(seq)->list.len;
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        VALUE e = PY_PTR(seq)->list.items[i];
        if (!py_is_str(e)) py_raise_exc(c, c->EXC_TypeError, "join element must be str");
        total += PY_PTR(e)->str.len;
        if (i) total += slen;
    }
    char *buf = (char *)GC_malloc_atomic(total + 1);
    char *p = buf;
    for (size_t i = 0; i < n; i++) {
        if (i) { memcpy(p, sep, slen); p += slen; }
        VALUE e = PY_PTR(seq)->list.items[i];
        memcpy(p, PY_PTR(e)->str.chars, PY_PTR(e)->str.len);
        p += PY_PTR(e)->str.len;
    }
    *p = '\0';
    return py_make_str_take(buf, total);
}

static VALUE
sm_upper(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) buf[i] = (char)toupper((unsigned char)o->str.chars[i]);
    buf[o->str.len] = '\0';
    return py_make_str_take(buf, o->str.len);
}

static VALUE
sm_lower(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) buf[i] = (char)tolower((unsigned char)o->str.chars[i]);
    buf[o->str.len] = '\0';
    return py_make_str_take(buf, o->str.len);
}

static VALUE
sm_strip(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    size_t i = 0, j = o->str.len;
    while (i < j && (o->str.chars[i] == ' ' || o->str.chars[i] == '\t' || o->str.chars[i] == '\n' || o->str.chars[i] == '\r')) i++;
    while (j > i && (o->str.chars[j-1] == ' ' || o->str.chars[j-1] == '\t' || o->str.chars[j-1] == '\n' || o->str.chars[j-1] == '\r')) j--;
    return py_make_str(o->str.chars + i, j - i);
}

static VALUE
sm_startswith(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "startswith: not str");
    struct pyobj *p = PY_PTR(argv[1]);
    if (p->str.len > s->str.len) return PY_FALSE;
    return memcmp(s->str.chars, p->str.chars, p->str.len) == 0 ? PY_TRUE : PY_FALSE;
}

static VALUE
sm_endswith(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "endswith: not str");
    struct pyobj *p = PY_PTR(argv[1]);
    if (p->str.len > s->str.len) return PY_FALSE;
    return memcmp(s->str.chars + s->str.len - p->str.len, p->str.chars, p->str.len) == 0 ? PY_TRUE : PY_FALSE;
}

static VALUE
sm_find(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "find: not str");
    struct pyobj *p = PY_PTR(argv[1]);
    if (p->str.len == 0) return PY_FIX(0);
    void *r = memmem(s->str.chars, s->str.len, p->str.chars, p->str.len);
    return r ? PY_FIX((int64_t)((char *)r - s->str.chars)) : PY_FIX(-1);
}

static VALUE
sm_replace(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_str(argv[1]) || !py_is_str(argv[2]))
        py_raise_exc(c, c->EXC_TypeError, "replace args must be str");
    struct pyobj *o = PY_PTR(argv[1]);
    struct pyobj *n = PY_PTR(argv[2]);
    if (o->str.len == 0) return argv[0];
    size_t cap = s->str.len + 16, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap + 1);
    size_t i = 0;
    while (i < s->str.len) {
        if (i + o->str.len <= s->str.len &&
            memcmp(s->str.chars + i, o->str.chars, o->str.len) == 0) {
            if (len + n->str.len + 1 > cap) {
                while (len + n->str.len + 1 > cap) cap *= 2;
                buf = (char *)GC_realloc(buf, cap + 1);
            }
            memcpy(buf + len, n->str.chars, n->str.len);
            len += n->str.len;
            i += o->str.len;
        } else {
            if (len + 2 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap + 1); }
            buf[len++] = s->str.chars[i++];
        }
    }
    buf[len] = '\0';
    return py_make_str_take(buf, len);
}

static VALUE
sm_count(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "count: not str");
    struct pyobj *p = PY_PTR(argv[1]);
    if (p->str.len == 0) return PY_FIX((int64_t)s->str.len + 1);
    int64_t n = 0;
    size_t i = 0;
    while (i + p->str.len <= s->str.len) {
        if (memcmp(s->str.chars + i, p->str.chars, p->str.len) == 0) { n++; i += p->str.len; }
        else i++;
    }
    return PY_FIX(n);
}

static struct type_method str_methods[] = {
    { "split",      sm_split,      1, 2 },
    { "join",       sm_join,       2, 2 },
    { "upper",      sm_upper,      1, 1 },
    { "lower",      sm_lower,      1, 1 },
    { "strip",      sm_strip,      1, 1 },
    { "startswith", sm_startswith, 2, 2 },
    { "endswith",   sm_endswith,   2, 2 },
    { "find",       sm_find,       2, 2 },
    { "replace",    sm_replace,    3, 3 },
    { "count",      sm_count,      2, 2 },
    { NULL, NULL, 0, 0 }
};

static VALUE
lm_append(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    py_list_append(c, argv[0], argv[1]);
    return PY_NONE;
}

static VALUE
lm_pop(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    int64_t i = (argc >= 2) ? py_int_to_long(c, argv[1]) : (int64_t)o->list.len - 1;
    if (i < 0) i += (int64_t)o->list.len;
    if (i < 0 || i >= (int64_t)o->list.len)
        py_raise_exc(c, c->EXC_IndexError, "pop from empty / out-of-range list");
    VALUE v = o->list.items[i];
    for (size_t j = (size_t)i; j + 1 < o->list.len; j++)
        o->list.items[j] = o->list.items[j + 1];
    o->list.len--;
    return v;
}

static VALUE
lm_extend(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE other = argv[1];
    struct py_iter it; py_iter_init(c, &it, other);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    while (py_iter_next(c, &it, &x)) py_list_append(c, argv[0], x);
    return PY_NONE;
}

static VALUE
lm_insert(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    int64_t i = py_int_to_long(c, argv[1]);
    if (i < 0) i += (int64_t)o->list.len;
    if (i < 0) i = 0;
    if (i > (int64_t)o->list.len) i = (int64_t)o->list.len;
    py_list_append(c, argv[0], PY_NONE);  // grow
    for (size_t j = o->list.len - 1; j > (size_t)i; j--) o->list.items[j] = o->list.items[j - 1];
    o->list.items[i] = argv[2];
    return PY_NONE;
}

static VALUE
lm_index(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    for (size_t i = 0; i < o->list.len; i++)
        if (py_eq_bool(c, o->list.items[i], argv[1])) return PY_FIX((int64_t)i);
    py_raise_exc(c, c->EXC_ValueError, "value not in list");
}

static VALUE
lm_reverse(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    for (size_t i = 0, j = o->list.len; i + 1 < j; i++, j--) {
        VALUE t = o->list.items[i];
        o->list.items[i] = o->list.items[j - 1];
        o->list.items[j - 1] = t;
    }
    return PY_NONE;
}

static VALUE
lm_sort(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    // Insertion sort (good enough for non-bench code).
    for (size_t i = 1; i < o->list.len; i++) {
        VALUE x = o->list.items[i];
        size_t j = i;
        while (j > 0 && py_cmp(c, o->list.items[j - 1], x) > 0) {
            o->list.items[j] = o->list.items[j - 1]; j--;
        }
        o->list.items[j] = x;
    }
    return PY_NONE;
}

static struct type_method list_methods[] = {
    { "append",  lm_append,  2, 2 },
    { "pop",     lm_pop,     1, 2 },
    { "extend",  lm_extend,  2, 2 },
    { "insert",  lm_insert,  3, 3 },
    { "index",   lm_index,   2, 2 },
    { "reverse", lm_reverse, 1, 1 },
    { "sort",    lm_sort,    1, 1 },
    { NULL, NULL, 0, 0 }
};

static VALUE
dm_get(CTX *c, int argc, VALUE *argv)
{
    VALUE d = argv[0], k = argv[1];
    VALUE dflt = (argc >= 3) ? argv[2] : PY_NONE;
    if (py_dict_has(c, d, k)) return py_dict_get(c, d, k);
    return dflt;
}

static VALUE
dm_keys(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pydict *d = PY_PTR(argv[0])->dict;
    VALUE r = py_make_list(NULL, 0);
    for (size_t i = 0; i < d->capa; i++)
        if (d->entries[i].state == 1) py_list_append(c, r, d->entries[i].key);
    return r;
}

static VALUE
dm_values(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pydict *d = PY_PTR(argv[0])->dict;
    VALUE r = py_make_list(NULL, 0);
    for (size_t i = 0; i < d->capa; i++)
        if (d->entries[i].state == 1) py_list_append(c, r, d->entries[i].value);
    return r;
}

static VALUE
dm_items(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pydict *d = PY_PTR(argv[0])->dict;
    VALUE r = py_make_list(NULL, 0);
    for (size_t i = 0; i < d->capa; i++)
        if (d->entries[i].state == 1) {
            VALUE pair[2] = { d->entries[i].key, d->entries[i].value };
            py_list_append(c, r, py_make_tuple(pair, 2));
        }
    return r;
}

static VALUE
dm_pop(CTX *c, int argc, VALUE *argv)
{
    VALUE d = argv[0], k = argv[1];
    if (py_dict_has(c, d, k)) {
        VALUE v = py_dict_get(c, d, k);
        py_dict_remove(c, d, k);
        return v;
    }
    if (argc >= 3) return argv[2];
    py_raise_exc(c, c->EXC_KeyError, "pop: key not found");
}

static struct type_method dict_methods[] = {
    { "get",    dm_get,    2, 3 },
    { "keys",   dm_keys,   1, 1 },
    { "values", dm_values, 1, 1 },
    { "items",  dm_items,  1, 1 },
    { "pop",    dm_pop,    2, 3 },
    { NULL, NULL, 0, 0 }
};

// Set methods.
static VALUE
sm_add(CTX *c, int argc, VALUE *argv) { (void)argc; py_dict_set(c, argv[0], argv[1], PY_NONE); return PY_NONE; }
static VALUE
sm_discard(CTX *c, int argc, VALUE *argv) { (void)argc; py_dict_remove(c, argv[0], argv[1]); return PY_NONE; }
static VALUE
sm_remove(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    if (!py_dict_remove(c, argv[0], argv[1]))
        py_raise_exc(c, c->EXC_KeyError, "remove: key not in set");
    return PY_NONE;
}
static VALUE
sm_set_pop(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    struct pydict *d = PY_PTR(argv[0])->dict;
    for (size_t i = 0; i < d->capa; i++) {
        if (d->entries[i].state == 1) {
            VALUE k = d->entries[i].key;
            d->entries[i].state = 2;
            d->used--;
            return k;
        }
    }
    py_raise_exc(c, c->EXC_KeyError, "pop from an empty set");
}
static VALUE
sm_union(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE r = py_make_set();
    struct pydict *a = PY_PTR(argv[0])->dict;
    for (size_t i = 0; i < a->capa; i++)
        if (a->entries[i].state == 1) py_dict_set(c, r, a->entries[i].key, PY_NONE);
    if (py_is_set(argv[1])) {
        struct pydict *b = PY_PTR(argv[1])->dict;
        for (size_t i = 0; i < b->capa; i++)
            if (b->entries[i].state == 1) py_dict_set(c, r, b->entries[i].key, PY_NONE);
    } else {
        struct py_iter it; py_iter_init(c, &it, argv[1]);
        VALUE x;
        while (py_iter_next(c, &it, &x)) py_dict_set(c, r, x, PY_NONE);
    }
    return r;
}
static VALUE
sm_intersection(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE r = py_make_set();
    struct pydict *a = PY_PTR(argv[0])->dict;
    for (size_t i = 0; i < a->capa; i++) {
        if (a->entries[i].state == 1 && py_contains(c, argv[1], a->entries[i].key))
            py_dict_set(c, r, a->entries[i].key, PY_NONE);
    }
    return r;
}
static VALUE
sm_difference(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE r = py_make_set();
    struct pydict *a = PY_PTR(argv[0])->dict;
    for (size_t i = 0; i < a->capa; i++) {
        if (a->entries[i].state == 1 && !py_contains(c, argv[1], a->entries[i].key))
            py_dict_set(c, r, a->entries[i].key, PY_NONE);
    }
    return r;
}

static struct type_method set_methods[] = {
    { "add",          sm_add,          2, 2 },
    { "discard",      sm_discard,      2, 2 },
    { "remove",       sm_remove,       2, 2 },
    { "pop",          sm_set_pop,      1, 1 },
    { "union",        sm_union,        2, 2 },
    { "intersection", sm_intersection, 2, 2 },
    { "difference",   sm_difference,   2, 2 },
    { NULL, NULL, 0, 0 }
};

// ---------------------------------------------------------------------------
// Builtins.
// ---------------------------------------------------------------------------

static VALUE
bi_print(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    for (int i = 0; i < argc; i++) {
        if (i) fputc(' ', stdout);
        py_display(stdout, argv[i], false);
    }
    fputc('\n', stdout);
    return PY_NONE;
}

static VALUE
bi_str(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return py_make_str("", 0);
    return py_to_str(c, argv[0]);
}

static VALUE
bi_repr(CTX *c, int argc, VALUE *argv) { (void)argc; return py_to_repr(c, argv[0]); }

static VALUE
bi_int(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return PY_FIX(0);
    VALUE v = argv[0];
    if (PY_IS_FIXNUM(v) || py_is_bignum(v)) return v;
    if (v == PY_TRUE)    return PY_FIX(1);
    if (v == PY_FALSE)   return PY_FIX(0);
    if (py_is_float(v)) {
        double d = PY_IS_FLONUM(v) ? py_flonum_to_double(v) : PY_PTR(v)->dbl;
        if (d >= (double)PY_FIXNUM_MIN && d <= (double)PY_FIXNUM_MAX)
            return PY_FIX((int64_t)d);
        mpz_t z; mpz_init(z); mpz_set_d(z, d);
        VALUE r = py_normalise_int(z); mpz_clear(z); return r;
    }
    if (py_is_str(v)) {
        mpz_t z; mpz_init(z);
        if (mpz_set_str(z, PY_PTR(v)->str.chars, 10) != 0) {
            mpz_clear(z); py_raise_exc(c, c->EXC_ValueError, "invalid literal for int()");
        }
        VALUE r = py_normalise_int(z); mpz_clear(z); return r;
    }
    py_raise_exc(c, c->EXC_TypeError, "int() argument type not supported");
}

static VALUE
bi_float(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return py_make_float(0.0);
    VALUE v = argv[0];
    if (py_is_float(v)) return v;
    if (PY_IS_FIXNUM(v)) return py_make_float((double)PY_FIXVAL(v));
    if (py_is_bignum(v)) return py_make_float(mpz_get_d(PY_PTR(v)->mpz));
    if (v == PY_TRUE)    return py_make_float(1.0);
    if (v == PY_FALSE)   return py_make_float(0.0);
    if (py_is_str(v)) {
        char *end;
        double d = strtod(PY_PTR(v)->str.chars, &end);
        if (end == PY_PTR(v)->str.chars) py_raise_exc(c, c->EXC_ValueError, "could not convert string to float");
        return py_make_float(d);
    }
    py_raise_exc(c, c->EXC_TypeError, "float() argument type not supported");
}

static VALUE
bi_bool(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    if (argc == 0) return PY_FALSE;
    return py_is_truthy(argv[0]) ? PY_TRUE : PY_FALSE;
}

static VALUE
bi_len(CTX *c, int argc, VALUE *argv) { (void)argc; return PY_FIX((int64_t)py_seq_len(c, argv[0])); }

static VALUE
bi_abs(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (PY_IS_FIXNUM(v)) {
        int64_t x = PY_FIXVAL(v);
        return PY_FIX(x < 0 ? -x : x);
    }
    if (py_is_bignum(v)) {
        mpz_t z; mpz_init_set(z, PY_PTR(v)->mpz);
        mpz_abs(z, z);
        VALUE r = py_normalise_int(z); mpz_clear(z); return r;
    }
    if (py_is_float(v)) {
        double d = PY_IS_FLONUM(v) ? py_flonum_to_double(v) : PY_PTR(v)->dbl;
        return py_make_float(fabs(d));
    }
    py_raise_exc(c, c->EXC_TypeError, "bad operand type for abs()");
}

static VALUE
bi_range(CTX *c, int argc, VALUE *argv)
{
    int64_t start = 0, stop = 0, step = 1;
    if (argc == 1) stop = py_int_to_long(c, argv[0]);
    else if (argc == 2) { start = py_int_to_long(c, argv[0]); stop = py_int_to_long(c, argv[1]); }
    else { start = py_int_to_long(c, argv[0]); stop = py_int_to_long(c, argv[1]); step = py_int_to_long(c, argv[2]); }
    if (step == 0) py_raise_exc(c, c->EXC_ValueError, "range() arg 3 must not be zero");
    return py_make_range(start, stop, step);
}

static VALUE
bi_list(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return py_make_list(NULL, 0);
    VALUE r = py_make_list(NULL, 0);
    struct py_iter it; py_iter_init(c, &it, argv[0]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    while (py_iter_next(c, &it, &x)) py_list_append(c, r, x);
    return r;
}

static VALUE
bi_tuple(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return py_make_tuple(NULL, 0);
    VALUE l = bi_list(c, argc, argv);
    return py_make_tuple(PY_PTR(l)->list.items, PY_PTR(l)->list.len);
}

static VALUE
bi_dict(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    return py_make_dict();
}

static VALUE
bi_set(CTX *c, int argc, VALUE *argv)
{
    VALUE r = py_make_set();
    if (argc == 0) return r;
    struct py_iter it; py_iter_init(c, &it, argv[0]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    while (py_iter_next(c, &it, &x)) py_dict_set(c, r, x, PY_NONE);
    return r;
}

static VALUE
bi_type(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    if (PY_IS_FIXNUM(v) || py_is_bignum(v)) return py_make_str("int", 3);
    if (v == PY_NONE)  return py_make_str("NoneType", 8);
    if (v == PY_TRUE || v == PY_FALSE) return py_make_str("bool", 4);
    struct pyobj *o = PY_PTR(v);
    switch (o->type) {
      case PY_T_FLOAT: return py_make_str("float", 5);
      case PY_T_STR:   return py_make_str("str", 3);
      case PY_T_LIST:  return py_make_str("list", 4);
      case PY_T_TUPLE: return py_make_str("tuple", 5);
      case PY_T_DICT:  return py_make_str("dict", 4);
      case PY_T_SET:   return py_make_str("set", 3);
      case PY_T_RANGE: return py_make_str("range", 5);
      case PY_T_FUNC: case PY_T_BUILTIN: case PY_T_BOUND_METHOD: return py_make_str("function", 8);
      case PY_T_CLASS: return py_make_str("type", 4);
      case PY_T_INSTANCE: return py_make_str(o->inst.cls->cls.name, strlen(o->inst.cls->cls.name));
      default: return py_make_str("object", 6);
    }
}

static VALUE
bi_isinstance(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0], cls = argv[1];
    if (!py_is_class(cls)) py_raise_exc(c, c->EXC_TypeError, "isinstance() second arg must be class");
    if (!py_is_instance(v)) return PY_FALSE;
    VALUE k = PY_OBJ_VAL(PY_PTR(v)->inst.cls);
    while (k != PY_NONE && py_is_class(k)) {
        if (k == cls) return PY_TRUE;
        k = PY_PTR(k)->cls.base;
    }
    return PY_FALSE;
}

static VALUE
bi_min(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) py_raise_exc(c, c->EXC_TypeError, "min() needs args");
    VALUE best;
    if (argc == 1) {
        struct py_iter it; py_iter_init(c, &it, argv[0]);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
        if (!py_iter_next(c, &it, &best)) py_raise_exc(c, c->EXC_ValueError, "min() empty");
        VALUE x;
        while (py_iter_next(c, &it, &x)) if (py_cmp(c, x, best) < 0) best = x;
        return best;
    }
    best = argv[0];
    for (int i = 1; i < argc; i++) if (py_cmp(c, argv[i], best) < 0) best = argv[i];
    return best;
}

static VALUE
bi_max(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) py_raise_exc(c, c->EXC_TypeError, "max() needs args");
    VALUE best;
    if (argc == 1) {
        struct py_iter it; py_iter_init(c, &it, argv[0]);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
        if (!py_iter_next(c, &it, &best)) py_raise_exc(c, c->EXC_ValueError, "max() empty");
        VALUE x;
        while (py_iter_next(c, &it, &x)) if (py_cmp(c, x, best) > 0) best = x;
        return best;
    }
    best = argv[0];
    for (int i = 1; i < argc; i++) if (py_cmp(c, argv[i], best) > 0) best = argv[i];
    return best;
}

static VALUE
bi_sum(CTX *c, int argc, VALUE *argv)
{
    VALUE acc = (argc >= 2) ? argv[1] : PY_FIX(0);
    struct py_iter it; py_iter_init(c, &it, argv[0]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    while (py_iter_next(c, &it, &x)) acc = py_add(c, acc, x);
    return acc;
}

static VALUE
bi_sorted(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE r = bi_list(c, 1, argv);
    lm_sort(c, 1, &r);
    return r;
}

static VALUE
bi_enumerate(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE r = py_make_list(NULL, 0);
    struct py_iter it; py_iter_init(c, &it, argv[0]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    int64_t i = 0;
    VALUE x;
    while (py_iter_next(c, &it, &x)) {
        VALUE pair[2] = { PY_FIX(i++), x };
        py_list_append(c, r, py_make_tuple(pair, 2));
    }
    return r;
}

static VALUE
bi_zip(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return py_make_list(NULL, 0);
    struct py_iter *its = (struct py_iter *)alloca(sizeof(struct py_iter) * argc);
    for (int i = 0; i < argc; i++) {
        py_iter_init(c, &its[i], argv[i]);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
    }
    VALUE r = py_make_list(NULL, 0);
    for (;;) {
        VALUE *tup = (VALUE *)alloca(sizeof(VALUE) * argc);
        for (int i = 0; i < argc; i++) {
            if (!py_iter_next(c, &its[i], &tup[i])) return r;
        }
        py_list_append(c, r, py_make_tuple(tup, argc));
    }
}

static VALUE
bi_chr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    int64_t k = py_int_to_long(c, argv[0]);
    if (k < 0 || k > 255) py_raise_exc(c, c->EXC_ValueError, "chr() out of range (ascii only)");
    char b[2] = { (char)k, 0 };
    return py_make_str(b, 1);
}

static VALUE
bi_ord(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0]) || PY_PTR(argv[0])->str.len != 1)
        py_raise_exc(c, c->EXC_TypeError, "ord() expected length-1 string");
    return PY_FIX((unsigned char)PY_PTR(argv[0])->str.chars[0]);
}

static VALUE
bi_hex(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_int_or_bool(argv[0])) py_raise_exc(c, c->EXC_TypeError, "hex() needs int");
    mpz_t z; py_to_mpz(c, argv[0], z);
    char *s = mpz_get_str(NULL, 16, z);
    char *r;
    int an = (s[0] == '-') ? asprintf(&r, "-0x%s", s + 1) : asprintf(&r, "0x%s", s);
    (void)an;
    VALUE v = py_make_str(r, strlen(r));
    free(r); mpz_clear(z); return v;
}

static VALUE
bi_bin(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_int_or_bool(argv[0])) py_raise_exc(c, c->EXC_TypeError, "bin() needs int");
    mpz_t z; py_to_mpz(c, argv[0], z);
    char *s = mpz_get_str(NULL, 2, z);
    char *r;
    int an = (s[0] == '-') ? asprintf(&r, "-0b%s", s + 1) : asprintf(&r, "0b%s", s);
    (void)an;
    VALUE v = py_make_str(r, strlen(r));
    free(r); mpz_clear(z); return v;
}

// Minimal `format(value, spec)` — handles the common f-string format
// specifications: `[fill][align][0][width][.precision][type]` where
// type ∈ { d / s / f / g / e / x / X / b / o / "" }.
//
// Recognised: width, alignment (`<` / `>` / `^`), zero-pad (`0`),
// precision for floats, type letter.  Anything else falls back to
// str(value) without formatting (a documented v0 limitation).
static VALUE
bi_format(CTX *c, int argc, VALUE *argv)
{
    VALUE v = argv[0];
    if (argc < 2 || !py_is_str(argv[1]) || PY_PTR(argv[1])->str.len == 0)
        return py_to_str(c, v);
    const char *s = PY_PTR(argv[1])->str.chars;
    size_t n = PY_PTR(argv[1])->str.len;
    char fill = ' ';
    char align = 0;            // 0 = unset, '<', '>', '^'
    bool zero_pad = false;
    int  width = 0;
    int  precision = -1;
    char type_ch = 0;
    size_t i = 0;
    // [fill][align]
    if (n >= 2 && (s[1] == '<' || s[1] == '>' || s[1] == '^')) {
        fill = s[0]; align = s[1]; i = 2;
    } else if (n >= 1 && (s[0] == '<' || s[0] == '>' || s[0] == '^')) {
        align = s[0]; i = 1;
    }
    // [0]
    if (i < n && s[i] == '0') { zero_pad = true; i++; }
    // [width]
    while (i < n && s[i] >= '0' && s[i] <= '9') { width = width * 10 + (s[i] - '0'); i++; }
    // [.precision]
    if (i < n && s[i] == '.') {
        i++;
        precision = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') { precision = precision * 10 + (s[i] - '0'); i++; }
    }
    // [type]
    if (i < n) type_ch = s[i++];
    // Format the value into `body`.
    char fmt[32];
    char body[256];
    body[0] = '\0';
    if (type_ch == 'd' || type_ch == 'b' || type_ch == 'o' || type_ch == 'x' || type_ch == 'X') {
        // Integer formatting.  Convert v to int.
        long long iv;
        if (PY_IS_FIXNUM(v)) iv = PY_FIXVAL(v);
        else if (v == PY_TRUE) iv = 1;
        else if (v == PY_FALSE) iv = 0;
        else if (py_is_bignum(v)) {
            char *bs = mpz_get_str(NULL, type_ch == 'b' ? 2 : type_ch == 'o' ? 8 :
                                          (type_ch == 'x' || type_ch == 'X') ? 16 : 10,
                                   PY_PTR(v)->mpz);
            snprintf(body, sizeof(body), "%s", bs);
            goto pad;
        }
        else iv = (long long)py_to_double(c, v);
        const char *base_fmt = type_ch == 'd' ? "%lld" :
                               type_ch == 'b' ? "%lld" :     // handled below
                               type_ch == 'o' ? "%llo" :
                               type_ch == 'x' ? "%llx" : "%llX";
        if (type_ch == 'b') {
            // C has no %b; do it manually.
            char buf[80]; int p = 0;
            unsigned long long x = (unsigned long long)(iv < 0 ? -iv : iv);
            if (x == 0) buf[p++] = '0';
            while (x) { buf[p++] = (x & 1) + '0'; x >>= 1; }
            int j = 0;
            if (iv < 0) body[j++] = '-';
            for (int k = p - 1; k >= 0; k--) body[j++] = buf[k];
            body[j] = '\0';
        } else snprintf(body, sizeof(body), base_fmt, iv);
    } else if (type_ch == 'f' || type_ch == 'g' || type_ch == 'e' || type_ch == 'E') {
        double d = py_to_double(c, v);
        if (precision >= 0) snprintf(fmt, sizeof(fmt), "%%.%d%c", precision, type_ch);
        else                snprintf(fmt, sizeof(fmt), "%%%c", type_ch);
        snprintf(body, sizeof(body), fmt, d);
    } else if (type_ch == 's' || type_ch == 0) {
        VALUE sv = py_to_str(c, v);
        if (py_is_str(sv)) {
            size_t L = PY_PTR(sv)->str.len;
            if (L >= sizeof(body)) L = sizeof(body) - 1;
            memcpy(body, PY_PTR(sv)->str.chars, L);
            body[L] = '\0';
            if (precision >= 0 && (size_t)precision < L) body[precision] = '\0';
        }
    } else {
        return py_to_str(c, v);    // unknown type — fall back
    }
  pad: {
        size_t bl = strlen(body);
        if ((int)bl >= width) return py_make_str(body, bl);
        size_t pad = (size_t)width - bl;
        char *out = (char *)GC_malloc_atomic(width + 1);
        char eff_fill = zero_pad ? '0' : fill;
        if (align == 0) {
            // Default: numbers right-align, strings left-align.
            align = (type_ch == 's' || type_ch == 0) ? '<' : '>';
        }
        if (align == '<') {
            memcpy(out, body, bl);
            for (size_t j = 0; j < pad; j++) out[bl + j] = eff_fill;
        } else if (align == '^') {
            size_t left = pad / 2, right = pad - left;
            for (size_t j = 0; j < left; j++) out[j] = eff_fill;
            memcpy(out + left, body, bl);
            for (size_t j = 0; j < right; j++) out[left + bl + j] = eff_fill;
        } else { // '>'
            for (size_t j = 0; j < pad; j++) out[j] = eff_fill;
            memcpy(out + pad, body, bl);
        }
        out[width] = '\0';
        return py_make_str_take(out, (size_t)width);
    }
}

static VALUE
bi_staticmethod(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = py_alloc(PY_T_STATICMETHOD);
    o->wrap.wrapped = argv[0];
    return PY_OBJ_VAL(o);
}

static VALUE
bi_classmethod(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = py_alloc(PY_T_CLASSMETHOD);
    o->wrap.wrapped = argv[0];
    return PY_OBJ_VAL(o);
}

static VALUE
bi_property(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = py_alloc(PY_T_PROPERTY);
    o->wrap.wrapped = argv[0];
    return PY_OBJ_VAL(o);
}

static VALUE
bi_input(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    if (argc >= 1 && py_is_str(argv[0])) {
        fwrite(PY_PTR(argv[0])->str.chars, 1, PY_PTR(argv[0])->str.len, stdout);
        fflush(stdout);
    }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return py_make_str("", 0);
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n') n--;
    return py_make_str(buf, n);
}

static VALUE
bi_hash(CTX *c, int argc, VALUE *argv) { (void)argc; return PY_FIX((int64_t)(py_hash(c, argv[0]) & 0x7FFFFFFFFFFFFFFFULL)); }

static VALUE
bi_all(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct py_iter it; py_iter_init(c, &it, argv[0]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    while (py_iter_next(c, &it, &x)) if (!py_is_truthy(x)) return PY_FALSE;
    return PY_TRUE;
}

static VALUE
bi_any(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct py_iter it; py_iter_init(c, &it, argv[0]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    while (py_iter_next(c, &it, &x)) if (py_is_truthy(x)) return PY_TRUE;
    return PY_FALSE;
}

static VALUE
bi_divmod(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE q = py_fdiv(c, argv[0], argv[1]);
    VALUE r = py_mod (c, argv[0], argv[1]);
    VALUE pair[2] = { q, r };
    return py_make_tuple(pair, 2);
}

static VALUE
bi_round(CTX *c, int argc, VALUE *argv)
{
    int ndig = (argc >= 2) ? (int)py_int_to_long(c, argv[1]) : 0;
    double d = py_to_double(c, argv[0]);
    double mul = 1.0;
    for (int i = 0; i < ndig; i++) mul *= 10.0;
    for (int i = 0; i > ndig; i--) mul /= 10.0;
    double r = (d >= 0 ? floor(d * mul + 0.5) : -floor(-d * mul + 0.5)) / mul;
    // Python: round(x) → int; round(x, n) → same type as x.
    if (argc < 2) return PY_FIX((int64_t)r);
    if (PY_IS_FIXNUM(argv[0]) || py_is_bignum(argv[0])) return PY_FIX((int64_t)r);
    return py_make_float(r);
}

static VALUE
bi_pow(CTX *c, int argc, VALUE *argv)
{
    if (argc == 3) {
        // a ** b mod m — only int int int for v0.
        if (!py_int_or_bool(argv[0]) || !py_int_or_bool(argv[1]) || !py_int_or_bool(argv[2]))
            py_raise_exc(c, c->EXC_TypeError, "pow() with 3 args needs ints");
        mpz_t a, b, m, r;
        py_to_mpz(c, argv[0], a); py_to_mpz(c, argv[1], b); py_to_mpz(c, argv[2], m);
        mpz_init(r); mpz_powm(r, a, b, m);
        VALUE rv = py_normalise_int(r);
        mpz_clear(a); mpz_clear(b); mpz_clear(m); mpz_clear(r);
        return rv;
    }
    return py_pow(c, argv[0], argv[1]);
}

static VALUE
bi_reversed(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE r = py_make_list(NULL, 0);
    if (py_is_list(argv[0]) || py_is_tuple(argv[0])) {
        struct pyobj *o = PY_PTR(argv[0]);
        for (size_t i = o->list.len; i > 0; i--) py_list_append(c, r, o->list.items[i - 1]);
        return r;
    }
    if (py_is_str(argv[0])) {
        struct pyobj *o = PY_PTR(argv[0]);
        for (size_t i = o->str.len; i > 0; i--) py_list_append(c, r, py_make_str(o->str.chars + i - 1, 1));
        return r;
    }
    if (py_is_range(argv[0])) {
        struct pyobj *o = PY_PTR(argv[0]);
        int64_t s = o->range.start, e = o->range.stop, st = o->range.step;
        // last element of a positive-step range: s + ((e-s-1)//st) * st
        int64_t last;
        if (st > 0 && s < e) last = s + ((e - s - 1) / st) * st;
        else if (st < 0 && s > e) last = s + ((s - e - 1) / -st) * st;
        else return r;
        for (int64_t v = last; (st > 0 ? v >= s : v <= s); v -= st)
            py_list_append(c, r, py_make_int(v));
        return r;
    }
    py_raise_exc(c, c->EXC_TypeError, "argument to reversed() must be a sequence");
}

static VALUE
bi_map(CTX *c, int argc, VALUE *argv)
{
    if (argc < 2) py_raise_exc(c, c->EXC_TypeError, "map() needs >=2 args");
    VALUE r = py_make_list(NULL, 0);
    int n_iters = argc - 1;
    struct py_iter *its = (struct py_iter *)alloca(sizeof(struct py_iter) * n_iters);
    for (int i = 0; i < n_iters; i++) {
        py_iter_init(c, &its[i], argv[i + 1]);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
    }
    for (;;) {
        VALUE *args = (VALUE *)alloca(sizeof(VALUE) * n_iters);
        for (int i = 0; i < n_iters; i++) {
            if (!py_iter_next(c, &its[i], &args[i])) return r;
        }
        VALUE v = py_apply(c, argv[0], n_iters, args);
        if (UNLIKELY(c->state == PY_STATE_RAISE)) return PY_NONE;
        py_list_append(c, r, v);
    }
}

static VALUE
bi_filter(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE r = py_make_list(NULL, 0);
    struct py_iter it; py_iter_init(c, &it, argv[1]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    while (py_iter_next(c, &it, &x)) {
        VALUE keep = (argv[0] == PY_NONE) ? (py_is_truthy(x) ? PY_TRUE : PY_FALSE)
                                          : py_apply(c, argv[0], 1, &x);
        if (UNLIKELY(c->state == PY_STATE_RAISE)) return PY_NONE;
        if (py_is_truthy(keep)) py_list_append(c, r, x);
    }
    return r;
}

static VALUE
bi_iter(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; /* iterables in pystro are self-iterators */ }

static VALUE
bi_next(CTX *c, int argc, VALUE *argv)
{
    // pystro doesn't have a separate iterator object — `next()` works
    // only on lists/tuples/dicts/etc. by treating them as a stream.
    // We don't currently track iter state across calls, so this is a
    // no-op stub raising StopIteration on first call.  Use `for` for
    // real iteration.
    (void)argc; (void)argv;
    py_raise_exc(c, c->EXC_StopIteration, "iter object not supported (use for-loop)");
}

static void
install_builtins(CTX *c)
{
    py_global_define(c, "print",      py_make_builtin("print",      bi_print,      0, -1));
    py_global_define(c, "str",        py_make_builtin("str",        bi_str,        0,  1));
    py_global_define(c, "repr",       py_make_builtin("repr",       bi_repr,       1,  1));
    py_global_define(c, "int",        py_make_builtin("int",        bi_int,        0,  1));
    py_global_define(c, "float",      py_make_builtin("float",      bi_float,      0,  1));
    py_global_define(c, "bool",       py_make_builtin("bool",       bi_bool,       0,  1));
    py_global_define(c, "len",        py_make_builtin("len",        bi_len,        1,  1));
    py_global_define(c, "abs",        py_make_builtin("abs",        bi_abs,        1,  1));
    py_global_define(c, "range",      py_make_builtin("range",      bi_range,      1,  3));
    py_global_define(c, "list",       py_make_builtin("list",       bi_list,       0,  1));
    py_global_define(c, "tuple",      py_make_builtin("tuple",      bi_tuple,      0,  1));
    py_global_define(c, "dict",       py_make_builtin("dict",       bi_dict,       0,  0));
    py_global_define(c, "set",        py_make_builtin("set",        bi_set,        0,  1));
    py_global_define(c, "type",       py_make_builtin("type",       bi_type,       1,  1));
    py_global_define(c, "isinstance", py_make_builtin("isinstance", bi_isinstance, 2,  2));
    py_global_define(c, "min",        py_make_builtin("min",        bi_min,        1, -1));
    py_global_define(c, "max",        py_make_builtin("max",        bi_max,        1, -1));
    py_global_define(c, "sum",        py_make_builtin("sum",        bi_sum,        1,  2));
    py_global_define(c, "sorted",     py_make_builtin("sorted",     bi_sorted,     1,  1));
    py_global_define(c, "enumerate",  py_make_builtin("enumerate",  bi_enumerate,  1,  1));
    py_global_define(c, "zip",        py_make_builtin("zip",        bi_zip,        0, -1));
    py_global_define(c, "chr",        py_make_builtin("chr",        bi_chr,        1,  1));
    py_global_define(c, "ord",        py_make_builtin("ord",        bi_ord,        1,  1));
    py_global_define(c, "hex",        py_make_builtin("hex",        bi_hex,        1,  1));
    py_global_define(c, "bin",        py_make_builtin("bin",        bi_bin,        1,  1));
    py_global_define(c, "input",      py_make_builtin("input",      bi_input,      0,  1));
    py_global_define(c, "hash",       py_make_builtin("hash",       bi_hash,       1,  1));
    py_global_define(c, "format",     py_make_builtin("format",     bi_format,     1,  2));
    py_global_define(c, "staticmethod",py_make_builtin("staticmethod",bi_staticmethod, 1, 1));
    py_global_define(c, "classmethod", py_make_builtin("classmethod", bi_classmethod, 1, 1));
    py_global_define(c, "property",    py_make_builtin("property",    bi_property,    1, 1));
    py_global_define(c, "all",         py_make_builtin("all",         bi_all,        1, 1));
    py_global_define(c, "any",         py_make_builtin("any",         bi_any,        1, 1));
    py_global_define(c, "divmod",      py_make_builtin("divmod",      bi_divmod,     2, 2));
    py_global_define(c, "round",       py_make_builtin("round",       bi_round,      1, 2));
    py_global_define(c, "pow",         py_make_builtin("pow",         bi_pow,        2, 3));
    py_global_define(c, "reversed",    py_make_builtin("reversed",    bi_reversed,   1, 1));
    py_global_define(c, "map",         py_make_builtin("map",         bi_map,        2,-1));
    py_global_define(c, "filter",      py_make_builtin("filter",      bi_filter,     2, 2));
    py_global_define(c, "iter",        py_make_builtin("iter",        bi_iter,       1, 1));
    py_global_define(c, "next",        py_make_builtin("next",        bi_next,       1, 2));

    // Built-in exception classes.  Hierarchy is BaseException root, but
    // for now everything inherits Exception → no base.
    c->EXC_Exception        = py_make_class("Exception",        PY_NONE, true);
    c->EXC_TypeError        = py_make_class("TypeError",        c->EXC_Exception, true);
    c->EXC_ValueError       = py_make_class("ValueError",       c->EXC_Exception, true);
    c->EXC_NameError        = py_make_class("NameError",        c->EXC_Exception, true);
    c->EXC_IndexError       = py_make_class("IndexError",       c->EXC_Exception, true);
    c->EXC_KeyError         = py_make_class("KeyError",         c->EXC_Exception, true);
    c->EXC_ZeroDivisionError= py_make_class("ZeroDivisionError",c->EXC_Exception, true);
    c->EXC_AttributeError   = py_make_class("AttributeError",   c->EXC_Exception, true);
    c->EXC_RuntimeError     = py_make_class("RuntimeError",     c->EXC_Exception, true);
    c->EXC_StopIteration    = py_make_class("StopIteration",    c->EXC_Exception, true);

    py_global_define(c, "Exception",        c->EXC_Exception);
    py_global_define(c, "TypeError",        c->EXC_TypeError);
    py_global_define(c, "ValueError",       c->EXC_ValueError);
    py_global_define(c, "NameError",        c->EXC_NameError);
    py_global_define(c, "IndexError",       c->EXC_IndexError);
    py_global_define(c, "KeyError",         c->EXC_KeyError);
    py_global_define(c, "ZeroDivisionError",c->EXC_ZeroDivisionError);
    py_global_define(c, "AttributeError",   c->EXC_AttributeError);
    py_global_define(c, "RuntimeError",     c->EXC_RuntimeError);
    py_global_define(c, "StopIteration",    c->EXC_StopIteration);

    c->current_class = PY_NONE;
}
