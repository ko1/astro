// runtime.c — pystro runtime (heap, builders, globals, apply, display,
// numeric tower, containers, attribute / method, iteration, exceptions,
// builtins).  #included from main.c.

#include <math.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

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

// Apply a user-supplied metaclass to a class that's already been
// constructed via py_make_class.  Builds attrs dict from the class's
// methods, calls metaclass(name, (base,), attrs), and returns the
// metaclass's result (or the original class if metaclass returns
// non-class — that's incompatible with Python but pragmatic).
extern VALUE py_class_lookup_method(VALUE cls, const char *name);
VALUE
py_class_meta_apply(CTX *c, VALUE cls, VALUE meta, const char *name)
{
    if (!py_is_class(cls)) return cls;
    if (meta == PY_NONE) return cls;
    // Build attrs dict from class methods.
    VALUE attrs = py_make_dict();
    struct pyclass *cd = &PY_PTR(cls)->cls;
    for (int i = 0; i < cd->nmethods; i++) {
        VALUE k = py_make_str(cd->methods[i].name, strlen(cd->methods[i].name));
        py_dict_set(c, attrs, k, cd->methods[i].value);
    }
    // Bases tuple.
    VALUE *bv = (VALUE *)alloca(sizeof(VALUE) * (cd->nbases ? cd->nbases : 1));
    for (int i = 0; i < cd->nbases; i++) bv[i] = cd->bases[i];
    VALUE bases_tuple = py_make_tuple(bv, cd->nbases);
    VALUE name_v = py_make_str(name, strlen(name));

    extern const char *intern_name(const char *s, size_t len);
    // If metaclass is a user class with __new__, call __new__(meta, ...).
    // The __new__ implementor is expected to return a class (typically by
    // calling type(name, bases, attrs)).
    if (py_is_class(meta)) {
        VALUE new_m = py_class_lookup_method(meta, "__new__");
        if (new_m != PY_NONE) {
            VALUE av[4] = { meta, name_v, bases_tuple, attrs };
            VALUE r = py_apply(c, new_m, 4, av);
            if (c->state == PY_STATE_RAISE) return PY_NONE;
            // If __init__ is also defined, call it on the new class.
            if (py_is_class(r)) {
                VALUE init_m = py_class_lookup_method(meta, "__init__");
                if (init_m != PY_NONE) {
                    VALUE iav[4] = { r, name_v, bases_tuple, attrs };
                    py_apply(c, init_m, 4, iav);
                    if (c->state == PY_STATE_RAISE) return PY_NONE;
                }
                // Stamp metaclass for inheritance.
                py_class_add_method(c, r, intern_name("__metaclass__", 13), meta);
                return r;
            }
        }
    }
    // Otherwise just call metaclass(name, bases, attrs).  For builtin
    // `type` this hits the 3-arg form (creates a class).
    VALUE av[3] = { name_v, bases_tuple, attrs };
    VALUE result = py_apply(c, meta, 3, av);
    if (c->state == PY_STATE_RAISE) return PY_NONE;
    if (py_is_class(result)) {
        py_class_add_method(c, result, intern_name("__metaclass__", 13), meta);
        return result;
    }
    return cls;
}

// Walk class methods for `__slots__` and stash the parsed name list on
// the class.  Called after the class body has finished evaluating so
// the slots tuple is already on the class.
void
py_class_extract_slots(CTX *c, VALUE cls)
{
    // Look up __slots__ ONLY on the class itself, not inherited.
    struct pyclass *cd0 = &PY_PTR(cls)->cls;
    VALUE sv = PY_NONE;
    for (int j = 0; j < cd0->nmethods; j++) {
        if (strcmp(cd0->methods[j].name, "__slots__") == 0) {
            sv = cd0->methods[j].value;
            break;
        }
    }
    if (sv == PY_NONE) return;
    // sv may be tuple/list/str/iterable.  Treat str specially: single name.
    struct pyclass *cd = &PY_PTR(cls)->cls;
    if (py_is_str(sv)) {
        cd->slots = (const char **)GC_malloc(sizeof(char *) * 1);
        cd->slots[0] = PY_PTR(sv)->str.chars;
        cd->nslots = 1;
        return;
    }
    if (py_is_list(sv) || py_is_tuple(sv)) {
        size_t n = PY_PTR(sv)->list.len;
        cd->slots = (const char **)GC_malloc(sizeof(char *) * (n ? n : 1));
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            VALUE v = PY_PTR(sv)->list.items[i];
            if (py_is_str(v)) cd->slots[k++] = PY_PTR(v)->str.chars;
        }
        cd->nslots = (int)k;
        return;
    }
    (void)c;
}

// True if `cls` (or any ancestor) has __slots__ declared, and `name`
// isn't in any __slots__ list.  Used by py_setattr to enforce.
static bool
py_class_has_slots_anywhere(VALUE cls)
{
    if (!py_is_class(cls)) return false;
    struct pyclass *cd = &PY_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) {
        struct pyclass *kd = &PY_PTR(cd->mro[i])->cls;
        if (kd->slots) return true;
    }
    return false;
}

static bool
py_class_slot_allowed(VALUE cls, const char *name)
{
    if (!py_is_class(cls)) return true;
    struct pyclass *cd = &PY_PTR(cls)->cls;
    bool any_slots = false;
    for (int i = 0; i < cd->nmro; i++) {
        struct pyclass *kd = &PY_PTR(cd->mro[i])->cls;
        if (!kd->slots) continue;
        any_slots = true;
        for (int j = 0; j < kd->nslots; j++) {
            if (strcmp(kd->slots[j], name) == 0) return true;
            // "__dict__" in slots → instance gets a __dict__ (any attr OK).
            if (strcmp(kd->slots[j], "__dict__") == 0) return true;
        }
        // If any base in MRO has no __slots__, instance gets __dict__
        // so any attr is allowed (CPython rule).
    }
    if (!any_slots) return true;
    // Walk MRO again: if any class in MRO has no slots AND isn't object,
    // arbitrary attrs are allowed.
    for (int i = 0; i < cd->nmro; i++) {
        struct pyclass *kd = &PY_PTR(cd->mro[i])->cls;
        if (kd->slots) continue;
        // object/built-in marker classes don't grant a __dict__ for
        // slot purposes.  Approximation: any user class with no slots
        // grants __dict__.
        // For simplicity in pystro: only `object` itself is exempt.
        if (strcmp(kd->name, "object") == 0) continue;
        return true;
    }
    return false;
}

static bool  pydict_entry_live(const struct pydict *d, size_t i);

// If any of `bases` (or their ancestors) has a `__metaclass__` attribute,
// apply it to `cls` (the freshly-built class).  Returns either `cls` or
// the metaclass-produced replacement.
VALUE
py_class_inherit_metaclass(CTX *c, VALUE cls, VALUE *bases, int nbases, const char *name)
{
    VALUE final_cls = cls;
    for (int i = 0; i < nbases; i++) {
        VALUE b = bases[i];
        if (!py_is_class(b)) continue;
        VALUE meta = py_class_lookup_method(b, "__metaclass__");
        if (meta != PY_NONE && py_is_class(meta)) {
            final_cls = py_class_meta_apply(c, cls, meta, name);
            break;
        }
    }
    // __init_subclass__ — invoked on a base class when it gets subclassed.
    // Walk MRO above `cls` itself; first defining class wins.
    if (py_is_class(final_cls)) {
        struct pyclass *cd = &PY_PTR(final_cls)->cls;
        for (int j = 1; j < cd->nmro; j++) {
            VALUE base = cd->mro[j];
            if (!py_is_class(base)) continue;
            struct pyclass *bd = &PY_PTR(base)->cls;
            VALUE found = PY_NONE;
            for (int k = 0; k < bd->nmethods; k++) {
                if (strcmp(bd->methods[k].name, "__init_subclass__") == 0) {
                    found = bd->methods[k].value;
                    break;
                }
            }
            if (found != PY_NONE) {
                // Forward class-level kwargs (`class C(B, foo=1)`) as
                // keyword args to __init_subclass__.
                struct pyclass *fcd = &PY_PTR(final_cls)->cls;
                VALUE kw_dict = PY_NONE;
                for (int kk = 0; kk < fcd->nmethods; kk++) {
                    if (strcmp(fcd->methods[kk].name, "__class_kwargs__") == 0) {
                        kw_dict = fcd->methods[kk].value;
                        break;
                    }
                }
                if (kw_dict != PY_NONE && py_is_dict(kw_dict)) {
                    struct pydict *dd = PY_PTR(kw_dict)->dict;
                    int nkw = 0;
                    const char **kn = NULL; VALUE *kv = NULL;
                    if (dd->used > 0) {
                        kn = (const char **)alloca(sizeof(char *) * dd->used);
                        kv = (VALUE *)alloca(sizeof(VALUE) * dd->used);
                        for (size_t ii = 0; ii < dd->elen; ii++) {
                            if (!pydict_entry_live(dd, ii)) continue;
                            VALUE k = dd->entries[ii].key;
                            if (!py_is_str(k)) continue;
                            kn[nkw] = PY_PTR(k)->str.chars;
                            kv[nkw] = dd->entries[ii].value;
                            nkw++;
                        }
                    }
                    extern VALUE py_apply_kw(CTX *c, VALUE fn, int argc, VALUE *argv,
                                             int kwc, const char **kwnames, VALUE *kwvalues);
                    VALUE av[1] = { final_cls };
                    py_apply_kw(c, found, 1, av, nkw, kn, kv);
                } else {
                    VALUE av[1] = { final_cls };
                    py_apply(c, found, 1, av);
                }
                if (c->state == PY_STATE_RAISE) return PY_NONE;
                break;
            }
        }
    }
    return final_cls;
}

VALUE
py_make_super(VALUE start_cls, VALUE self)
{
    struct pyobj *o = py_alloc(PY_T_SUPER);
    o->super_.start_cls = start_cls;
    o->super_.self = self;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_complex(double re, double im)
{
    struct pyobj *o = py_alloc(PY_T_COMPLEX);
    o->cpx.re = re;
    o->cpx.im = im;
    return PY_OBJ_VAL(o);
}

// Get (re, im) of v if it's a number; for non-complex, im=0.  Returns
// false if v isn't numeric.
static bool
py_to_cpx(CTX *c, VALUE v, double *re, double *im)
{
    (void)c;
    if (py_is_complex(v)) { *re = PY_PTR(v)->cpx.re; *im = PY_PTR(v)->cpx.im; return true; }
    if (PY_IS_FIXNUM(v)) { *re = (double)PY_FIXVAL(v); *im = 0; return true; }
    if (v == PY_TRUE)    { *re = 1; *im = 0; return true; }
    if (v == PY_FALSE)   { *re = 0; *im = 0; return true; }
    if (PY_IS_FLONUM(v)) { *re = py_flonum_to_double(v); *im = 0; return true; }
    if (py_is_heap_float(v)) { *re = PY_PTR(v)->dbl; *im = 0; return true; }
    if (py_is_bignum(v)) { *re = mpz_get_d(PY_PTR(v)->mpz); *im = 0; return true; }
    return false;
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

VALUE
py_make_bytes(const char *s, size_t len)
{
    struct pyobj *o = py_alloc(PY_T_BYTES);
    char *buf = (char *)GC_malloc_atomic(len + 1);
    if (s && len) memcpy(buf, s, len);
    buf[len] = '\0';
    o->str.chars = buf;
    o->str.len = len;
    return PY_OBJ_VAL(o);
}

VALUE
py_make_bytearray(const char *s, size_t len)
{
    struct pyobj *o = py_alloc(PY_T_BYTEARRAY);
    char *buf = (char *)GC_malloc_atomic(len + 1);
    if (s && len) memcpy(buf, s, len);
    buf[len] = '\0';
    o->str.chars = buf;
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
             bool has_varargs, bool has_kwargs,
             bool is_generator)
{
    struct pyobj *o = py_alloc(PY_T_FUNC);
    o->func.body = body;
    o->func.env = env;
    o->func.name = name;
    o->func.nparams = nparams;
    o->func.n_pos_named = n_pos_named;
    o->func.n_pos_only = 0;  // overwritten by node_def from flags bits 8-15
    o->func.nlocals = nlocals;
    o->func.leaf = leaf;
    o->func.has_varargs = has_varargs;
    o->func.has_kwargs = has_kwargs;
    o->func.is_generator = is_generator;
    // Copy param_names into a private GC-managed array.  The caller
    // may pass a pointer into PYSTRO_NAME_TABLE which can be moved by
    // a later GC_realloc; per-func storage is stable.
    if (param_names && nparams > 0) {
        const char **pn = (const char **)GC_malloc(sizeof(char *) * nparams);
        for (int i = 0; i < nparams; i++) pn[i] = param_names[i];
        o->func.param_names = pn;
    } else {
        o->func.param_names = NULL;
    }
    o->func.defining_class = PY_NONE;
    extern CTX *py_current_ctx;
    o->func.fglobals = py_current_ctx ? py_current_ctx->globals : NULL;
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

// C3 linearization (Python's MRO algorithm).  Builds:
//   L[C(B1,...,Bn)] = C + merge(L[B1], ..., L[Bn], [B1, ..., Bn])
// where merge picks at each step the head of some list that does not
// appear in the tail of any other list.  If no such head exists, the
// hierarchy is inconsistent and we fall back to a simple BFS order.
void
py_compute_mro(VALUE cls)
{
    struct pyclass *cd = &PY_PTR(cls)->cls;
    if (cd->nbases == 0) {
        // Implicit object base, except for object itself.
        extern CTX *py_current_ctx;
        VALUE obj_cls = py_current_ctx ? py_current_ctx->TYPE_object : (VALUE)0;
        if (obj_cls && obj_cls != PY_NONE && py_is_class(obj_cls) && cls != obj_cls) {
            cd->mro = (VALUE *)GC_malloc(sizeof(VALUE) * 2);
            cd->mro[0] = cls;
            cd->mro[1] = obj_cls;
            cd->nmro = 2;
            return;
        }
        cd->mro = (VALUE *)GC_malloc(sizeof(VALUE) * 1);
        cd->mro[0] = cls;
        cd->nmro = 1;
        return;
    }
    // Source lists: each base's MRO + the bases-list itself.
    int nsrc = cd->nbases + 1;
    int *idx = (int *)GC_malloc(sizeof(int) * nsrc);
    int *len = (int *)GC_malloc(sizeof(int) * nsrc);
    VALUE **list = (VALUE **)GC_malloc(sizeof(VALUE *) * nsrc);
    for (int i = 0; i < cd->nbases; i++) {
        struct pyclass *b = &PY_PTR(cd->bases[i])->cls;
        list[i] = b->mro;
        len[i]  = b->nmro;
        idx[i]  = 0;
    }
    list[cd->nbases] = cd->bases;
    len[cd->nbases]  = cd->nbases;
    idx[cd->nbases]  = 0;
    int total_max = 1;
    for (int i = 0; i < nsrc; i++) total_max += len[i];
    VALUE *out = (VALUE *)GC_malloc(sizeof(VALUE) * total_max);
    int nout = 0;
    out[nout++] = cls;
    for (;;) {
        // pick head: first non-empty list whose head doesn't appear in
        // any other list's tail.
        int picked = -1;
        for (int i = 0; i < nsrc; i++) {
            if (idx[i] >= len[i]) continue;
            VALUE head = list[i][idx[i]];
            bool in_tail = false;
            for (int j = 0; j < nsrc && !in_tail; j++) {
                if (j == i) continue;
                for (int k = idx[j] + 1; k < len[j]; k++)
                    if (list[j][k] == head) { in_tail = true; break; }
            }
            if (!in_tail) { picked = i; break; }
        }
        if (picked < 0) {
            // Inconsistent — collect remaining in BFS order so things
            // still work (matches CPython only for simple hierarchies).
            for (int i = 0; i < nsrc; i++) {
                while (idx[i] < len[i]) {
                    VALUE v = list[i][idx[i]++];
                    bool dup = false;
                    for (int k = 0; k < nout; k++) if (out[k] == v) { dup = true; break; }
                    if (!dup) out[nout++] = v;
                }
            }
            break;
        }
        VALUE head = list[picked][idx[picked]];
        bool dup = false;
        for (int k = 0; k < nout; k++) if (out[k] == head) { dup = true; break; }
        if (!dup) out[nout++] = head;
        // remove head from any list whose head matches
        for (int i = 0; i < nsrc; i++)
            if (idx[i] < len[i] && list[i][idx[i]] == head) idx[i]++;
        // done?
        bool empty = true;
        for (int i = 0; i < nsrc; i++) if (idx[i] < len[i]) { empty = false; break; }
        if (empty) break;
    }
    cd->mro = out;
    cd->nmro = nout;
}

VALUE
py_make_builtin_class(const char *name, py_builtin_fn ctor, int tag)
{
    struct pyobj *o = py_alloc(PY_T_CLASS);
    o->cls.name = name;
    o->cls.methods = NULL;
    o->cls.nmethods = 0;
    o->cls.methods_capa = 0;
    o->cls.is_exception = false;
    o->cls.base = PY_NONE;
    o->cls.bases = NULL;
    o->cls.nbases = 0;
    o->cls.builtin_ctor = ctor;
    o->cls.builtin_tag = tag;
    extern void py_compute_mro(VALUE cls);
    py_compute_mro(PY_OBJ_VAL(o));
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
    o->cls.builtin_ctor = NULL;
    o->cls.builtin_tag = -1;
    // If base is a built-in type constructor (PY_T_BUILTIN named "list",
    // "int", etc.), pystro can't really do built-in subclassing — drop
    // the base silently so the class definition at least parses and
    // the new class can hold its own methods/state.  Real method
    // delegation requires composition.
    if (base != PY_NONE && !py_is_class(base)) base = PY_NONE;
    o->cls.base = base;
    if (base == PY_NONE) {
        o->cls.bases = NULL;
        o->cls.nbases = 0;
    } else {
        o->cls.bases = (VALUE *)GC_malloc(sizeof(VALUE));
        o->cls.bases[0] = base;
        o->cls.nbases = 1;
    }
    py_compute_mro(PY_OBJ_VAL(o));
    return PY_OBJ_VAL(o);
}

// Used when class C(A, B, ...): multi-inheritance.  Replaces the
// single-base bases[] array with the full list.  Caller passes the
// already-built class and its full base list.  Updates `is_exception`
// if any base is an exception class.
void
py_class_set_bases(VALUE cls, VALUE *bases, int n)
{
    struct pyclass *cd = &PY_PTR(cls)->cls;
    cd->bases = n > 0 ? (VALUE *)GC_malloc(sizeof(VALUE) * n) : NULL;
    for (int i = 0; i < n; i++) cd->bases[i] = bases[i];
    cd->nbases = n;
    cd->base = n > 0 ? bases[0] : PY_NONE;
    for (int i = 0; i < n; i++)
        if (py_is_class(bases[i]) && PY_PTR(bases[i])->cls.is_exception)
            cd->is_exception = true;
    py_compute_mro(cls);
}

void
py_class_add_method(CTX *c, VALUE cls, const char *name, VALUE fn)
{
    // Stamp the method's defining class so cooperative super() can
    // walk MRO from this point.
    if (PY_IS_PTR(fn) && PY_PTR(fn)->type == PY_T_FUNC)
        PY_PTR(fn)->func.defining_class = cls;
    else if (PY_IS_PTR(fn) && (PY_PTR(fn)->type == PY_T_STATICMETHOD
                            || PY_PTR(fn)->type == PY_T_CLASSMETHOD
                            || PY_PTR(fn)->type == PY_T_PROPERTY)) {
        VALUE inner = PY_PTR(fn)->wrap.wrapped;
        if (PY_IS_PTR(inner) && PY_PTR(inner)->type == PY_T_FUNC)
            PY_PTR(inner)->func.defining_class = cls;
    }
    // __set_name__ — if `fn` is a user instance with __set_name__,
    // call it now so descriptors can capture their attribute name.
    if (PY_IS_PTR(fn) && PY_PTR(fn)->type == PY_T_INSTANCE) {
        VALUE sn = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(fn)->inst.cls), "__set_name__");
        if (sn != PY_NONE) {
            VALUE av[3] = { fn, cls, py_make_str(name, strlen(name)) };
            py_apply(c, sn, 3, av);
            // Ignore raise — but propagate the state.
            if (c->state == PY_STATE_RAISE) return;
        }
    }
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

// Walk the C3-linearised MRO and return the first matching method.
// `cls.mro[]` is computed at class creation time and includes self at
// index 0 + all bases in MRO order.
VALUE
py_class_lookup_method(VALUE cls, const char *name)
{
    if (cls == PY_NONE || !py_is_class(cls)) return PY_NONE;
    struct pyclass *cd = &PY_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) {
        struct pyclass *kd = &PY_PTR(cd->mro[i])->cls;
        for (int j = 0; j < kd->nmethods; j++)
            if (strcmp(kd->methods[j].name, name) == 0) return kd->methods[j].value;
    }
    return PY_NONE;
}

bool
py_class_has_method(VALUE cls, const char *name)
{
    if (cls == PY_NONE || !py_is_class(cls)) return false;
    struct pyclass *cd = &PY_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) {
        struct pyclass *kd = &PY_PTR(cd->mro[i])->cls;
        for (int j = 0; j < kd->nmethods; j++)
            if (strcmp(kd->methods[j].name, name) == 0) return true;
    }
    return false;
}

VALUE
py_class_lookup_method_pub(VALUE cls, const char *name)
{
    return py_class_lookup_method(cls, name);
}

bool
py_func_is_generator(VALUE fn)
{
    return PY_IS_PTR(fn) && PY_PTR(fn)->type == PY_T_FUNC
        && PY_PTR(fn)->func.is_generator;
}

// Cooperative super() lookup: walk self.__class__'s MRO; find
// `start_after_cls`; return the first method found AFTER it.
// True if `v` is a bound method whose inner is a built-in.  Used by
// node_super_method to decide whether to prepend `self` (built-in
// already carries its own receiver via the bound wrapper).
bool
py_is_bound_builtin(VALUE v)
{
    return py_is_bound(v) && py_is_builtin(PY_PTR(v)->bound.func);
}

VALUE
py_super_lookup(CTX *c, VALUE self, VALUE start_after_cls, const char *name)
{
    (void)c;
    // self may be an instance OR a class (classmethod context).  Use
    // the relevant MRO either way.
    struct pyclass *cd;
    if (py_is_instance(self)) {
        cd = &PY_PTR(self)->inst.cls->cls;
    } else if (py_is_class(self)) {
        cd = &PY_PTR(self)->cls;
    } else {
        return PY_NONE;
    }
    int i = 0;
    while (i < cd->nmro && cd->mro[i] != start_after_cls) i++;
    for (int j = i + 1; j < cd->nmro; j++) {
        struct pyclass *kd = &PY_PTR(cd->mro[j])->cls;
        for (int k = 0; k < kd->nmethods; k++)
            if (strcmp(kd->methods[k].name, name) == 0) return kd->methods[k].value;
    }
    // Walk past start_after_cls's MRO; if any of the remaining classes
    // is a built-in subclass-base AND the instance has a primary,
    // dispatch to the built-in method on the primary.
    if (py_is_instance(self) && PY_PTR(self)->inst.primary) {
        VALUE prim = PY_PTR(self)->inst.primary;
        extern VALUE py_builtin_method(CTX *c, VALUE recv, const char *name);
        VALUE bm = py_builtin_method(c, prim, name);
        if (bm != PY_NONE) return bm;
    }
    return PY_NONE;
}

VALUE
py_make_instance(VALUE cls)
{
    struct pyobj *o = py_alloc(PY_T_INSTANCE);
    o->inst.cls = PY_PTR(cls);
    o->inst.attrs = NULL;       // lazily allocated when first attr is set
    o->inst.primary = 0;
    return PY_OBJ_VAL(o);
}

// object.__getattribute__(self, name) — default attribute lookup that
// does NOT recursively call __getattribute__.  We implement this by
// temporarily marking the call so py_getattr skips the user's hook.
static __thread int py_skip_getattribute_hook = 0;

static VALUE
bi_object_getattribute(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[1]))
        py_raise_exc(c, c->EXC_TypeError, "attribute name must be string");
    py_skip_getattribute_hook++;
    VALUE r = py_getattr(c, argv[0], PY_PTR(argv[1])->str.chars);
    py_skip_getattribute_hook--;
    return r;
}

// object.__setattr__(self, name, value) — default attribute set.
static VALUE
bi_object_setattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[1]))
        py_raise_exc(c, c->EXC_TypeError, "attribute name must be string");
    py_setattr(c, argv[0], PY_PTR(argv[1])->str.chars, argv[2]);
    return PY_NONE;
}

// object.__delattr__(self, name) — default attribute delete.
static VALUE
bi_object_delattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[1]))
        py_raise_exc(c, c->EXC_TypeError, "attribute name must be string");
    if (!py_is_instance(argv[0]))
        py_raise_exc(c, c->EXC_TypeError, "delattr: not an instance");
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->inst.attrs) {
        // Wrap inst.attrs as a dict VALUE for py_dict_remove.
        struct pyobj *d = py_alloc(PY_T_DICT);
        d->dict = o->inst.attrs;
        py_dict_remove(c, PY_OBJ_VAL(d), argv[1]);
    }
    return PY_NONE;
}

// object.__init__(self, *args, **kwargs) — accepts anything, returns None.
// Used by 'super().__init__()' from a built-in subclass so the chain
// terminates cleanly.
static VALUE
bi_object_init(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    return PY_NONE;
}

VALUE
bi_object_new(CTX *c, int argc, VALUE *argv)
{
    if (argc < 1 || !py_is_class(argv[0]))
        py_raise_exc(c, c->EXC_TypeError, "object.__new__() needs class");
    VALUE cls = argv[0];
    VALUE inst = py_make_instance(cls);
    // For built-in subclasses, set up an EMPTY primary value.  If the
    // caller forwarded constructor args, only forward them when the
    // class has no user-defined __init__ (so plain `class M(list): pass;
    // M([1,2,3])` still populates).  Otherwise the user's __init__ is
    // responsible for calling super().__init__(args).
    extern VALUE py_class_find_builtin_base(VALUE cls);
    VALUE bin_base = py_class_find_builtin_base(cls);
    if (bin_base != PY_NONE) {
        bool has_user_init = false;
        struct pyclass *cd = &PY_PTR(cls)->cls;
        for (int i = 0; i < cd->nmro; i++) {
            VALUE mc = cd->mro[i];
            // Stop walking once we reach the built-in base — methods
            // beyond that are object/builtin defaults.
            if (mc == bin_base) break;
            struct pyclass *kd = &PY_PTR(mc)->cls;
            for (int j = 0; j < kd->nmethods; j++) {
                if (strcmp(kd->methods[j].name, "__init__") == 0) {
                    has_user_init = true; break;
                }
            }
            if (has_user_init) break;
        }
        int fwd_argc = (has_user_init || argc <= 1) ? 0 : argc - 1;
        VALUE *fwd_argv = (has_user_init || argc <= 1) ? NULL : argv + 1;
        VALUE primary = PY_PTR(bin_base)->cls.builtin_ctor(c, fwd_argc, fwd_argv);
        if (c->state == PY_STATE_RAISE) return PY_NONE;
        PY_PTR(inst)->inst.primary = primary;
    }
    return inst;
}

// Walk a class's MRO and find the first built-in type class (one with
// builtin_ctor set).  Returns PY_NONE if none found.
VALUE
py_class_find_builtin_base(VALUE cls)
{
    if (!py_is_class(cls)) return PY_NONE;
    struct pyclass *cd = &PY_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) {
        if (py_is_class(cd->mro[i]) && PY_PTR(cd->mro[i])->cls.builtin_ctor)
            return cd->mro[i];
    }
    return PY_NONE;
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

// Process-wide monotone counter so every `pyglobals.serial` is unique
// across modules.  This is what the inline gref cache compares against:
// if a gref node is only ever evaluated under one specific module's
// globals (always true since each AST belongs to its module), the
// cache stays consistent only when the cached serial equals the
// current globals' serial — which is exactly what we want.
uint64_t SHARED_GLOBALS_SERIAL = 1;

// Keyword args being passed to a built-in.  py_apply_kw saves/restores
// these around the call; built-ins (sorted / min / max / enumerate)
// consult them directly.  Single-threaded, so a single global is fine.
int    PYSTRO_BI_KWC = 0;
const char **PYSTRO_BI_KWNAMES = NULL;
VALUE *PYSTRO_BI_KWVALUES = NULL;

static VALUE
pystro_bi_kwarg(const char *name)
{
    for (int i = 0; i < PYSTRO_BI_KWC; i++)
        if (strcmp(PYSTRO_BI_KWNAMES[i], name) == 0) return PYSTRO_BI_KWVALUES[i];
    return (VALUE)0;
}

struct pyglobals *
py_globals_new(void)
{
    struct pyglobals *g = (struct pyglobals *)GC_malloc(sizeof(struct pyglobals));
    g->entries = NULL;
    g->size = g->capa = 0;
    g->serial = ++SHARED_GLOBALS_SERIAL;
    return g;
}

static int
py_global_index(CTX *c, const char *name)
{
    struct pyglobals *g = c->globals;
    for (size_t i = 0; i < g->size; i++)
        if (strcmp(g->entries[i].name, name) == 0) return (int)i;
    return -1;
}

static int
py_global_alloc(CTX *c, const char *name)
{
    struct pyglobals *g = c->globals;
    if (g->size == g->capa) {
        size_t cap = g->capa ? g->capa * 2 : 32;
        g->entries = (struct gentry *)GC_realloc(g->entries, cap * sizeof(struct gentry));
        g->capa = cap;
    }
    int i = (int)g->size++;
    g->entries[i].name = name;
    g->entries[i].value = PY_NONE;
    g->entries[i].defined = false;
    return i;
}

void
py_global_define(CTX *c, const char *name, VALUE v)
{
    struct pyglobals *g = c->globals;
    int i = py_global_index(c, name);
    bool is_new = (i < 0);
    if (is_new) i = py_global_alloc(c, name);
    bool was_defined = g->entries[i].defined;
    g->entries[i].value = v;
    g->entries[i].defined = true;
    if (is_new || !was_defined) g->serial = ++SHARED_GLOBALS_SERIAL;
}

bool
py_global_has(CTX *c, const char *name)
{
    int i = py_global_index(c, name);
    return i >= 0 && c->globals->entries[i].defined;
}

VALUE
py_global_ref(CTX *c, const char *name)
{
    int i = py_global_index(c, name);
    if (i < 0 || !c->globals->entries[i].defined)
        py_raise_exc(c, c->EXC_NameError, "name '%s' is not defined", name);
    return c->globals->entries[i].value;
}

int
py_global_resolve(CTX *c, const char *name)
{
    int i = py_global_index(c, name);
    if (i < 0)
        py_raise_exc(c, c->EXC_NameError, "name '%s' is not defined", name);
    if (!c->globals->entries[i].defined)
        py_raise_exc(c, c->EXC_NameError, "name '%s' is not defined", name);
    return i;
}

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
    if (c->current_handling_exc && c->current_handling_exc != PY_NONE)
        py_setattr(c, inst, "__context__", c->current_handling_exc);
    // Capture a snapshot of the active call stack as a list-of-strings
    // attribute on the exception, so an uncaught exception can show
    // a traceback even though we longjmp away from this point.
    if (c->call_top > 0) {
        VALUE *frames = (VALUE *)alloca(sizeof(VALUE) * c->call_top);
        for (int i = 0; i < c->call_top; i++) {
            const char *fn = c->call_stack[i] ? c->call_stack[i] : "<anon>";
            frames[i] = py_make_str(fn, strlen(fn));
        }
        py_setattr(c, inst, "__traceback__", py_make_list(frames, c->call_top));
    }
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
    if (py_is_complex(a)) return py_make_complex(-PY_PTR(a)->cpx.re, -PY_PTR(a)->cpx.im);
    if (py_int_or_bool(a)) {
        mpz_t z; py_to_mpz(c, a, z);
        mpz_neg(z, z);
        VALUE r = py_normalise_int(z);
        mpz_clear(z);
        return r;
    }
    if (py_is_instance(a)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(a)->inst.cls), "__neg__");
        if (m != PY_NONE) {
            VALUE av[1] = { a };
            return py_apply(c, m, 1, av);
        }
    }
    py_raise_exc(c, c->EXC_TypeError, "bad operand type for unary -");
}

// Try a binary dunder hook on an instance operand: returns the result
// if the dunder exists, else PY_NONE (caller falls through to the
// regular numeric / type logic).  We use 0 as "method not defined"
// sentinel since PY_NONE is a valid return.
//
// CPython always dispatches arithmetic through type slots (tp_as_number).
// We mirror that: any user instance with a defined dunder takes priority.
static inline VALUE
py_try_binop_dunder(CTX *c, const char *name, VALUE a, VALUE b)
{
    // Only user-defined classes can have a dunder we don't already
    // know about; for built-in types we know they don't override these.
    if (LIKELY(!(PY_IS_PTR(a) && PY_PTR(a)->type == PY_T_INSTANCE)))
        return (VALUE)0;
    VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(a)->inst.cls), name);
    if (m != PY_NONE) {
        VALUE av[2] = { a, b };
        return py_apply(c, m, 2, av);
    }
    return (VALUE)0;
}

// For built-in subclasses (`class M(list):`), unwrap to the primary
// value so binary ops fall through to the built-in path.
static inline VALUE
py_unwrap_primary(VALUE v)
{
    if (PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_INSTANCE && PY_PTR(v)->inst.primary)
        return PY_PTR(v)->inst.primary;
    return v;
}

VALUE
py_add(CTX *c, VALUE a, VALUE b)
{
    VALUE r = py_try_binop_dunder(c, "__add__", a, b);
    if (r) return r;
    r = py_try_binop_dunder(c, "__radd__", b, a);
    if (r) return r;
    r = py_try_binop_dunder(c, "__iadd__", a, b);
    if (r) return r;
    a = py_unwrap_primary(a);
    b = py_unwrap_primary(b);
    if (py_is_str(a) && py_is_str(b)) {
        size_t la = PY_PTR(a)->str.len, lb = PY_PTR(b)->str.len;
        char *buf = (char *)GC_malloc_atomic(la + lb + 1);
        memcpy(buf,      PY_PTR(a)->str.chars, la);
        memcpy(buf + la, PY_PTR(b)->str.chars, lb);
        buf[la + lb] = '\0';
        return py_make_str_take(buf, la + lb);
    }
    if (py_is_byteseq(a) && py_is_byteseq(b)) {
        size_t la = PY_PTR(a)->str.len, lb = PY_PTR(b)->str.len;
        char *buf = (char *)GC_malloc_atomic(la + lb + 1);
        memcpy(buf,      PY_PTR(a)->str.chars, la);
        memcpy(buf + la, PY_PTR(b)->str.chars, lb);
        buf[la + lb] = '\0';
        // Result type: bytearray if either operand is bytearray, else bytes.
        if (py_is_bytearray(a) || py_is_bytearray(b))
            return py_make_bytearray(buf, la + lb);
        return py_make_bytes(buf, la + lb);
    }
    if ((py_is_list(a) && py_is_list(b)) || (py_is_tuple(a) && py_is_tuple(b))) {
        size_t la = PY_PTR(a)->list.len, lb = PY_PTR(b)->list.len;
        VALUE *items = (VALUE *)alloca(sizeof(VALUE) * (la + lb + 1));
        memcpy(items,      PY_PTR(a)->list.items, sizeof(VALUE) * la);
        memcpy(items + la, PY_PTR(b)->list.items, sizeof(VALUE) * lb);
        return py_is_list(a) ? py_make_list(items, la + lb) : py_make_tuple(items, la + lb);
    }
    // list + iterable (only via __iadd__-style) — Python's list __iadd__
    // accepts any iterable.  We support `list + iter` here too.
    if (py_is_list(a) && PY_IS_PTR(b)
        && (PY_PTR(b)->type == PY_T_ITER || PY_PTR(b)->type == PY_T_GEN)) {
        VALUE r = py_make_list(PY_PTR(a)->list.items, PY_PTR(a)->list.len);
        struct py_iter it; py_iter_init(c, &it, b);
        if (c->state != PY_STATE_NORMAL) return r;
        VALUE x;
        while (py_iter_next(c, &it, &x)) py_list_append(c, r, x);
        return r;
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
    {
        double ra, ia, rb, ib;
        if (py_to_cpx(c, a, &ra, &ia) && py_to_cpx(c, b, &rb, &ib)) {
            return py_make_complex(ra + rb, ia + ib);
        }
    }
    py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for +");
}

// `py_func_set_doc` definition lives further down (after pydict_new).

// Forward decls used by binop fallthroughs into set methods.
static VALUE sm_union(CTX *c, int argc, VALUE *argv);
static VALUE sm_intersection(CTX *c, int argc, VALUE *argv);
static VALUE sm_difference(CTX *c, int argc, VALUE *argv);
// (declaration moved above py_class_inherit_metaclass)

VALUE
py_sub(CTX *c, VALUE a, VALUE b)
{
    if (py_is_any_set(a) && py_is_any_set(b)) {
        VALUE av[2] = { a, b };
        return sm_difference(c, 2, av);
    }
    // list - list as set difference (dict_keys-style courtesy).
    if ((py_is_list(a) || py_is_any_set(a)) && (py_is_list(b) || py_is_any_set(b))) {
        VALUE r = py_make_set();
        size_t na = PY_PTR(a)->list.len;
        for (size_t i = 0; i < na; i++) {
            VALUE x = PY_PTR(a)->list.items[i];
            if (!py_contains(c, b, x)) py_dict_set(c, r, x, PY_NONE);
        }
        return r;
    }
    VALUE r = py_try_binop_dunder(c, "__sub__", a, b);
    if (r) return r;
    r = py_try_binop_dunder(c, "__rsub__", b, a);
    if (r) return r;
    r = py_try_binop_dunder(c, "__isub__", a, b);
    if (r) return r;
    a = py_unwrap_primary(a);
    b = py_unwrap_primary(b);
    if (py_int_or_bool(a) && py_int_or_bool(b)) {
        mpz_t za, zb; py_to_mpz(c, a, za); py_to_mpz(c, b, zb);
        mpz_sub(za, za, zb);
        VALUE r = py_normalise_int(za);
        mpz_clear(za); mpz_clear(zb);
        return r;
    }
    if ((py_int_or_bool(a) || py_is_float(a)) && (py_int_or_bool(b) || py_is_float(b)))
        return py_make_float(py_to_double(c, a) - py_to_double(c, b));
    {
        double ra, ia, rb, ib;
        if (py_to_cpx(c, a, &ra, &ia) && py_to_cpx(c, b, &rb, &ib))
            return py_make_complex(ra - rb, ia - ib);
    }
    py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for -");
}

VALUE
py_mul(CTX *c, VALUE a, VALUE b)
{
    VALUE r = py_try_binop_dunder(c, "__mul__", a, b);
    if (r) return r;
    r = py_try_binop_dunder(c, "__rmul__", b, a);
    if (r) return r;
    r = py_try_binop_dunder(c, "__imul__", a, b);
    if (r) return r;
    a = py_unwrap_primary(a);
    b = py_unwrap_primary(b);
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
    if (py_is_byteseq(a) && py_int_or_bool(b)) {
        int64_t k = PY_IS_FIXNUM(b) ? PY_FIXVAL(b) : (b == PY_TRUE ? 1 : 0);
        if (k <= 0) return py_make_bytes("", 0);
        size_t la = PY_PTR(a)->str.len;
        char *buf = (char *)GC_malloc_atomic(la * (size_t)k + 1);
        for (int64_t i = 0; i < k; i++) memcpy(buf + i * la, PY_PTR(a)->str.chars, la);
        VALUE r = py_make_bytes(buf, la * (size_t)k);
        if (PY_PTR(a)->type == PY_T_BYTEARRAY) PY_PTR(r)->type = PY_T_BYTEARRAY;
        return r;
    }
    if (py_int_or_bool(a) && py_is_byteseq(b)) return py_mul(c, b, a);
    if (py_int_or_bool(a) && (py_is_list(b) || py_is_tuple(b))) return py_mul(c, b, a);
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
    {
        double ra, ia, rb, ib;
        if (py_to_cpx(c, a, &ra, &ia) && py_to_cpx(c, b, &rb, &ib))
            return py_make_complex(ra*rb - ia*ib, ra*ib + ia*rb);
    }
    py_raise_exc(c, c->EXC_TypeError, "unsupported operand type(s) for *");
}

VALUE
py_truediv(CTX *c, VALUE a, VALUE b)
{
    VALUE r = py_try_binop_dunder(c, "__truediv__", a, b);
    if (r) return r;
    if (py_is_complex(a) || py_is_complex(b)) {
        double ra, ia, rb, ib;
        if (py_to_cpx(c, a, &ra, &ia) && py_to_cpx(c, b, &rb, &ib)) {
            double denom = rb*rb + ib*ib;
            if (denom == 0) py_raise_exc(c, c->EXC_ZeroDivisionError, "complex division by zero");
            return py_make_complex((ra*rb + ia*ib)/denom, (ia*rb - ra*ib)/denom);
        }
    }
    double bd = py_to_double(c, b);
    if (bd == 0.0) py_raise_exc(c, c->EXC_ZeroDivisionError, "division by zero");
    return py_make_float(py_to_double(c, a) / bd);
}

VALUE
py_fdiv(CTX *c, VALUE a, VALUE b)
{
    VALUE rd = py_try_binop_dunder(c, "__floordiv__", a, b);
    if (rd) return rd;
    rd = py_try_binop_dunder(c, "__rfloordiv__", b, a);
    if (rd) return rd;
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

extern VALUE py_str_pct_format(CTX *c, VALUE fmt, VALUE args);  // forward
VALUE
py_mod(CTX *c, VALUE a, VALUE b)
{
    // String % formatting: `"fmt" % args`.
    if (py_is_str(a)) return py_str_pct_format(c, a, b);
    {
        VALUE rd = py_try_binop_dunder(c, "__mod__", a, b);
        if (rd) return rd;
        rd = py_try_binop_dunder(c, "__rmod__", b, a);
        if (rd) return rd;
    }
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
    VALUE r = py_try_binop_dunder(c, "__pow__", a, b);
    if (r) return r;
    r = py_try_binop_dunder(c, "__rpow__", b, a);
    if (r) return r;
    if (py_is_complex(a) || py_is_complex(b)) {
        double ra, ia, rb, ib;
        if (py_to_cpx(c, a, &ra, &ia) && py_to_cpx(c, b, &rb, &ib)) {
            // (ra+ia*i)^(rb+ib*i) via exp(b * log(a))
            double mod = sqrt(ra * ra + ia * ia);
            if (mod == 0.0) return py_make_complex(0, 0);
            double th = atan2(ia, ra);
            double lr = log(mod);
            // b*log(a) = (rb*lr - ib*th) + i*(rb*th + ib*lr)
            double er = rb * lr - ib * th;
            double ei = rb * th + ib * lr;
            double scale = exp(er);
            return py_make_complex(scale * cos(ei), scale * sin(ei));
        }
    }
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
    {
        // Negative base with non-integer exponent → complex.
        double da = py_to_double(c, a);
        double db = py_to_double(c, b);
        if (da < 0 && db != (double)(int64_t)db) {
            // (a)^b = exp(b * ln(a)) — a < 0 so use complex formula:
            // ln(-r) = ln(r) + i*pi, so b*ln(-r) = b*ln(r) + i*b*pi
            // exp(...) = e^{b*ln(r)} * (cos(b*pi) + i*sin(b*pi))
            double mag = pow(-da, db);
            return py_make_complex(mag * cos(db * 3.14159265358979323846),
                                   mag * sin(db * 3.14159265358979323846));
        }
        return py_make_float(pow(da, db));
    }
}

VALUE
py_bit_and(CTX *c, VALUE a, VALUE b)
{
    {
        VALUE rd = py_try_binop_dunder(c, "__and__", a, b);
        if (rd) return rd;
        rd = py_try_binop_dunder(c, "__rand__", b, a);
        if (rd) return rd;
    }
    if (py_is_any_set(a) && py_is_any_set(b)) {
        VALUE av[2] = { a, b };
        return sm_intersection(c, 2, av);
    }
    // dict_keys / dict_items support set ops in CPython.  Pystro
    // returns lists from .keys()/.items()/.values(); allow set
    // intersection between two lists as a courtesy so view-style
    // code (`d1.keys() & d2.keys()`) works.
    if ((py_is_list(a) || py_is_any_set(a)) && (py_is_list(b) || py_is_any_set(b))) {
        VALUE r = py_make_set();
        size_t na = PY_PTR(a)->list.len;
        for (size_t i = 0; i < na; i++) {
            VALUE x = PY_PTR(a)->list.items[i];
            if (py_contains(c, b, x)) py_dict_set(c, r, x, PY_NONE);
        }
        return r;
    }
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
    VALUE rd = py_try_binop_dunder(c, "__or__", a, b);
    if (rd) return rd;
    rd = py_try_binop_dunder(c, "__ror__", b, a);
    if (rd) return rd;
    if (py_is_any_set(a) && py_is_any_set(b)) {
        VALUE av[2] = { a, b };
        return sm_union(c, 2, av);
    }
    if (py_is_dict(a) && py_is_dict(b)) {
        // dict | dict: merge (RHS wins)
        VALUE r = py_make_dict();
        struct pydict *src = PY_PTR(a)->dict;
        for (size_t i = 0; i < src->elen; i++)
            if (pydict_entry_live(src, i))
                py_dict_set(c, r, src->entries[i].key, src->entries[i].value);
        struct pydict *src2 = PY_PTR(b)->dict;
        for (size_t i = 0; i < src2->elen; i++)
            if (pydict_entry_live(src2, i))
                py_dict_set(c, r, src2->entries[i].key, src2->entries[i].value);
        return r;
    }
    // list | list as set union (for dict_keys-style usage).
    if ((py_is_list(a) || py_is_any_set(a)) && (py_is_list(b) || py_is_any_set(b))) {
        VALUE r = py_make_set();
        size_t na = PY_PTR(a)->list.len;
        for (size_t i = 0; i < na; i++)
            py_dict_set(c, r, PY_PTR(a)->list.items[i], PY_NONE);
        if (py_is_list(b)) {
            size_t nb = PY_PTR(b)->list.len;
            for (size_t i = 0; i < nb; i++)
                py_dict_set(c, r, PY_PTR(b)->list.items[i], PY_NONE);
        } else {
            struct pydict *bd = PY_PTR(b)->dict;
            for (size_t i = 0; i < bd->elen; i++)
                if (pydict_entry_live(bd, i))
                    py_dict_set(c, r, bd->entries[i].key, PY_NONE);
        }
        return r;
    }
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
    VALUE rd = py_try_binop_dunder(c, "__xor__", a, b);
    if (rd) return rd;
    rd = py_try_binop_dunder(c, "__rxor__", b, a);
    if (rd) return rd;
    if (py_is_any_set(a) && py_is_any_set(b)) {
        // Symmetric difference: (a - b) | (b - a)
        VALUE r = py_make_set();
        struct pydict *aa = PY_PTR(a)->dict;
        struct pydict *bb = PY_PTR(b)->dict;
        for (size_t i = 0; i < aa->elen; i++)
            if (pydict_entry_live(aa, i) && !py_contains(c, b, aa->entries[i].key))
                py_dict_set(c, r, aa->entries[i].key, PY_NONE);
        for (size_t i = 0; i < bb->elen; i++)
            if (pydict_entry_live(bb, i) && !py_contains(c, a, bb->entries[i].key))
                py_dict_set(c, r, bb->entries[i].key, PY_NONE);
        return r;
    }
    // list ^ list as set symmetric_difference.
    if ((py_is_list(a) || py_is_any_set(a)) && (py_is_list(b) || py_is_any_set(b))) {
        VALUE r = py_make_set();
        size_t na = PY_PTR(a)->list.len;
        size_t nb = PY_PTR(b)->list.len;
        // Use list iteration (works for both list & set internal storage).
        struct py_iter ita; py_iter_init(c, &ita, a);
        VALUE x;
        while (py_iter_next(c, &ita, &x))
            if (!py_contains(c, b, x)) py_dict_set(c, r, x, PY_NONE);
        struct py_iter itb; py_iter_init(c, &itb, b);
        while (py_iter_next(c, &itb, &x))
            if (!py_contains(c, a, x)) py_dict_set(c, r, x, PY_NONE);
        (void)na; (void)nb;
        return r;
    }
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
    if (py_is_instance(a)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(a)->inst.cls), "__invert__");
        if (m != PY_NONE) {
            VALUE av[1] = { a };
            return py_apply(c, m, 1, av);
        }
    }
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
    {
        VALUE rd = py_try_binop_dunder(c, "__lshift__", a, b);
        if (rd) return rd;
        rd = py_try_binop_dunder(c, "__rlshift__", b, a);
        if (rd) return rd;
    }
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
    {
        VALUE rd = py_try_binop_dunder(c, "__rshift__", a, b);
        if (rd) return rd;
        rd = py_try_binop_dunder(c, "__rrshift__", b, a);
        if (rd) return rd;
    }
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

// Set comparison helper.  Returns -1 (a strict subset of b), 0 (equal),
// 1 (a strict superset), or -2 (incomparable).  The set node_lt/le/gt/ge
// special-case sets to use this so set-vs-set respects partial ordering.
int
py_set_cmp_partial(CTX *c, VALUE a, VALUE b)
{
    struct pydict *aa = PY_PTR(a)->dict;
    struct pydict *bb = PY_PTR(b)->dict;
    bool a_in_b = true, b_in_a = true;
    for (size_t i = 0; i < aa->elen; i++) {
        if (!pydict_entry_live(aa, i)) continue;
        if (!py_contains(c, b, aa->entries[i].key)) { a_in_b = false; break; }
    }
    for (size_t i = 0; i < bb->elen; i++) {
        if (!pydict_entry_live(bb, i)) continue;
        if (!py_contains(c, a, bb->entries[i].key)) { b_in_a = false; break; }
    }
    if (a_in_b && b_in_a) return 0;
    if (a_in_b) return -1;
    if (b_in_a) return 1;
    return -2;
}

// Convenience predicate: true iff entries[i] is a live (non-deleted) slot.
static inline bool
pydict_entry_live(const struct pydict *d, size_t i)
{
    VALUE k = d->entries[i].key;
    return k != 0 && k != DICT_DELETED_KEY;
}

VALUE
py_eq(CTX *c, VALUE a, VALUE b)
{
    VALUE r = py_try_binop_dunder(c, "__eq__", a, b);
    if (r && !(PY_IS_PTR(r) && PY_PTR(r)->type == PY_T_NOTIMPL)) {
        return py_is_truthy(r) ? PY_TRUE : PY_FALSE;
    }
    // a's __eq__ returned NotImplemented (or wasn't defined): try b's.
    r = py_try_binop_dunder(c, "__eq__", b, a);
    if (r && !(PY_IS_PTR(r) && PY_PTR(r)->type == PY_T_NOTIMPL)) {
        return py_is_truthy(r) ? PY_TRUE : PY_FALSE;
    }
    // NaN is special: NaN != NaN, even when stored in the same VALUE.
    if (py_is_float(a)) {
        double d = PY_IS_FLONUM(a) ? py_flonum_to_double(a) : PY_PTR(a)->dbl;
        if (d != d) {  // NaN check
            if (py_is_float(b)) {
                double d2 = PY_IS_FLONUM(b) ? py_flonum_to_double(b) : PY_PTR(b)->dbl;
                if (d2 != d2) return PY_FALSE;   // NaN == NaN → False
            }
            // NaN != non-float: still False (NaN equals nothing).
            return PY_FALSE;
        }
    }
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
    if (py_is_byteseq(a) && py_is_byteseq(b)) {
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
    if (py_is_dict(a) && py_is_dict(b)) {
        struct pydict *da = PY_PTR(a)->dict, *db = PY_PTR(b)->dict;
        if (da->used != db->used) return PY_FALSE;
        for (size_t i = 0; i < da->elen; i++) {
            if (!pydict_entry_live(da, i)) continue;
            VALUE k = da->entries[i].key;
            if (!py_dict_has(c, b, k)) return PY_FALSE;
            VALUE vb = py_dict_get(c, b, k);
            if (py_eq(c, da->entries[i].value, vb) != PY_TRUE) return PY_FALSE;
        }
        return PY_TRUE;
    }
    if (py_is_complex(a) || py_is_complex(b)) {
        double ra, ia, rb, ib;
        if (py_to_cpx(c, a, &ra, &ia) && py_to_cpx(c, b, &rb, &ib))
            return (ra == rb && ia == ib) ? PY_TRUE : PY_FALSE;
    }
    if (py_is_any_set(a) && py_is_any_set(b)) {
        struct pydict *da = PY_PTR(a)->dict, *db = PY_PTR(b)->dict;
        if (da->used != db->used) return PY_FALSE;
        for (size_t i = 0; i < da->elen; i++) {
            if (!pydict_entry_live(da, i)) continue;
            if (!py_dict_has(c, b, da->entries[i].key)) return PY_FALSE;
        }
        return PY_TRUE;
    }
    if (py_is_bound(a) && py_is_bound(b)) {
        // Bound methods compare equal when bound to the same object and
        // the underlying function is the same.  CPython matches by
        // self-identity and func-identity.
        return (PY_PTR(a)->bound.self == PY_PTR(b)->bound.self
                && PY_PTR(a)->bound.func == PY_PTR(b)->bound.func)
               ? PY_TRUE : PY_FALSE;
    }
    if (py_is_range(a) && py_is_range(b)) {
        // CPython: equal iff same sequence of values.  Two empty ranges
        // are equal regardless of start/step; otherwise len, start, and
        // (if len > 1) step must match.
        struct pyobj *ra = PY_PTR(a), *rb = PY_PTR(b);
        size_t la = py_seq_len(c, a), lb = py_seq_len(c, b);
        if (la != lb) return PY_FALSE;
        if (la == 0) return PY_TRUE;
        if (ra->range.start != rb->range.start) return PY_FALSE;
        if (la == 1) return PY_TRUE;
        return ra->range.step == rb->range.step ? PY_TRUE : PY_FALSE;
    }
    return PY_FALSE;
}

bool
py_eq_bool(CTX *c, VALUE a, VALUE b)
{
    return py_eq(c, a, b) == PY_TRUE;
}

// (pydict_entry_live moved earlier — see below)

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
    if (v == PY_TRUE)  return py_hash(c, PY_FIX(1));
    if (v == PY_FALSE) return py_hash(c, PY_FIX(0));
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
      case PY_T_COMPLEX: {
        union { uint64_t u; double d; } pr = { .d = o->cpx.re };
        union { uint64_t u; double d; } pi = { .d = o->cpx.im };
        return pr.u ^ pi.u;
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
      case PY_T_FROZENSET: {
        // XOR-of-hashes — order-independent.
        uint64_t h = 0;
        struct pydict *d = o->dict;
        for (size_t i = 0; i < d->elen; i++)
            if (pydict_entry_live(d, i)) h ^= py_hash(c, d->entries[i].key);
        return h ^ 0xC2B2AE3D27D4EB4FULL;
      }
      case PY_T_RANGE: {
        // Hash the (start, stop, step) triple so equal ranges hash same.
        // Use start, len, last (CPython approximation).
        size_t L = py_seq_len(c, v);
        if (L == 0) return 0xCBF29CE484222325ULL;  // empty: fixed
        int64_t last = o->range.start + (int64_t)(L - 1) * o->range.step;
        uint64_t h = (uint64_t)L;
        h = h * 0x9E3779B97F4A7C15ULL ^ (uint64_t)o->range.start;
        if (L > 1) h = h * 0x100000001B3ULL ^ (uint64_t)last;
        return h;
      }
      case PY_T_INSTANCE: {
        // User-defined __hash__: call it and convert to int.
        VALUE cls = PY_OBJ_VAL(o->inst.cls);
        // __hash__ explicitly set to None makes the type unhashable.
        if (py_class_has_method(cls, "__hash__")) {
            VALUE hm0 = py_class_lookup_method(cls, "__hash__");
            if (hm0 == PY_NONE) {
                py_raise_exc(c, c->EXC_TypeError,
                             "unhashable type: '%s'", o->inst.cls->cls.name);
                return 0;
            }
        }
        VALUE hm = py_class_lookup_method(cls, "__hash__");
        if (hm != PY_NONE) {
            VALUE av[1] = { v };
            VALUE r = py_apply(c, hm, 1, av);
            if (c->state != PY_STATE_NORMAL) return 0;
            // Coerce result to a 64-bit hash (signed-truncated, like CPython's PyObject_Hash).
            if (PY_IS_FIXNUM(r)) {
                int64_t hv = PY_FIXVAL(r);
                if (hv == -1) hv = -2;  // CPython convention.
                return (uint64_t)hv;
            }
            if (py_is_int(r)) {
                int64_t hv = mpz_get_si(PY_PTR(r)->mpz);
                if (hv == -1) hv = -2;
                return (uint64_t)hv;
            }
            // Non-int return → fall through to identity hash.
        }
        return (uint64_t)(uintptr_t)o * 0x9E3779B97F4A7C15ULL;
      }
      case PY_T_LIST:
        py_raise_exc(c, c->EXC_TypeError, "unhashable type: 'list'");
        return 0;
      case PY_T_DICT:
        py_raise_exc(c, c->EXC_TypeError, "unhashable type: 'dict'");
        return 0;
      case PY_T_SET:
        py_raise_exc(c, c->EXC_TypeError, "unhashable type: 'set'");
        return 0;
      case PY_T_BYTEARRAY:
        py_raise_exc(c, c->EXC_TypeError, "unhashable type: 'bytearray'");
        return 0;
      case PY_T_BOUND_METHOD: {
        // Hash by (self, func) so equal bound methods hash to the same
        // bucket — matches the eq rule.
        uint64_t hs = py_hash(c, o->bound.self);
        uint64_t hf = py_hash(c, o->bound.func);
        return hs * 0x100000001B3ULL ^ hf;
      }
      default:
        // Identity hash for class/method/etc.
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
    d->icapa = DICT_INIT_CAPA;
    d->ecapa = DICT_INIT_CAPA;
    d->elen = 0;
    d->used = 0;
    d->fill = 0;
    d->indices = (int32_t *)GC_malloc_atomic(sizeof(int32_t) * d->icapa);
    for (size_t i = 0; i < d->icapa; i++) d->indices[i] = DICT_EMPTY_IDX;
    d->entries = (struct pydict_entry *)GC_malloc(sizeof(struct pydict_entry) * d->ecapa);
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

VALUE
py_make_frozenset(void)
{
    struct pyobj *o = py_alloc(PY_T_FROZENSET);
    o->dict = pydict_new();
    return PY_OBJ_VAL(o);
}

// Look up `key` in `d->indices`.  Returns:
//   - the bucket index in *out_bucket
//   - the entry index (>=0) in *out_eidx if found, else -1
// `*out_first_tomb` points at the first tombstone bucket we passed
// (so an insert can reuse it without further search).
static inline void
pydict_indices_lookup(CTX * restrict c, struct pydict * restrict d,
                      VALUE key, uint64_t h,
                      size_t *out_bucket, int32_t *out_eidx,
                      ssize_t *out_first_tomb)
{
    size_t mask = d->icapa - 1;
    size_t i = (size_t)h & mask;
    size_t step = 0;
    ssize_t first_tomb = -1;
    bool key_is_none = (key == PY_NONE);
    for (;;) {
        int32_t idx = d->indices[i];
        if (idx == DICT_EMPTY_IDX) {
            *out_bucket = i; *out_eidx = -1; *out_first_tomb = first_tomb; return;
        }
        if (idx == DICT_TOMB_IDX) {
            if (first_tomb < 0) first_tomb = (ssize_t)i;
        } else {
            struct pydict_entry *e = &d->entries[idx];
            if (LIKELY(e->hash == h)) {
                if (LIKELY(e->key == key)) {
                    *out_bucket = i; *out_eidx = idx; *out_first_tomb = first_tomb; return;
                }
                if (key_is_none || e->key == PY_NONE) {
                    /* None equals only itself. */
                }
                else if (py_is_str(key) && py_is_str(e->key)) {
                    size_t l1 = PY_PTR(key)->str.len, l2 = PY_PTR(e->key)->str.len;
                    if (l1 == l2 && memcmp(PY_PTR(key)->str.chars,
                                           PY_PTR(e->key)->str.chars, l1) == 0) {
                        *out_bucket = i; *out_eidx = idx; *out_first_tomb = first_tomb; return;
                    }
                }
                else if (py_eq_bool(c, e->key, key)) {
                    *out_bucket = i; *out_eidx = idx; *out_first_tomb = first_tomb; return;
                }
            }
        }
        step++;
        i = (i + step) & mask;
    }
}

// Find the entry index for `key`, or -1 if absent.  Fast path: doesn't
// track the bucket or first-tombstone (read-only path).
static inline int32_t
pydict_find(CTX * restrict c, struct pydict * restrict d, VALUE key, uint64_t h)
{
    size_t mask = d->icapa - 1;
    size_t i = (size_t)h & mask;
    size_t step = 0;
    bool key_is_none = (key == PY_NONE);
    for (;;) {
        int32_t idx = d->indices[i];
        if (idx == DICT_EMPTY_IDX) return -1;
        if (idx != DICT_TOMB_IDX) {
            struct pydict_entry *e = &d->entries[idx];
            if (LIKELY(e->hash == h)) {
                if (LIKELY(e->key == key)) return idx;
                // None equals only itself.
                if (key_is_none || e->key == PY_NONE) {
                    /* skip py_eq */
                }
                else if (py_is_str(key) && py_is_str(e->key)) {
                    size_t l1 = PY_PTR(key)->str.len, l2 = PY_PTR(e->key)->str.len;
                    if (l1 == l2 && memcmp(PY_PTR(key)->str.chars,
                                           PY_PTR(e->key)->str.chars, l1) == 0) return idx;
                }
                else if (py_eq_bool(c, e->key, key)) return idx;
            }
        }
        step++;
        i = (i + step) & mask;
    }
}

// Resize indices[] (without touching entries[] in the common case) — or
// also compact entries[] if we have a lot of deleted entries.
static void
pydict_resize(struct pydict *d, size_t new_icapa, bool compact_entries)
{
    int32_t *new_indices = (int32_t *)GC_malloc_atomic(sizeof(int32_t) * new_icapa);
    for (size_t i = 0; i < new_icapa; i++) new_indices[i] = DICT_EMPTY_IDX;
    size_t mask = new_icapa - 1;

    if (compact_entries) {
        // Walk old entries[] in insertion order, copy live ones to a
        // new dense entries[], rebuild indices[].
        size_t new_ecapa = d->used > DICT_INIT_CAPA ? d->used * 2 : DICT_INIT_CAPA;
        struct pydict_entry *new_entries = (struct pydict_entry *)GC_malloc(
            sizeof(struct pydict_entry) * new_ecapa);
        size_t new_elen = 0;
        for (size_t i = 0; i < d->elen; i++) {
            VALUE k = d->entries[i].key;
            if (k == 0 || k == DICT_DELETED_KEY) continue;
            new_entries[new_elen] = d->entries[i];
            // Insert into new_indices[].
            uint64_t h = d->entries[i].hash;
            size_t bi = (size_t)h & mask;
            size_t step = 0;
            while (new_indices[bi] != DICT_EMPTY_IDX) { step++; bi = (bi + step) & mask; }
            new_indices[bi] = (int32_t)new_elen;
            new_elen++;
        }
        d->entries = new_entries;
        d->ecapa = new_ecapa;
        d->elen = new_elen;
        d->fill = new_elen;     // no tombstones in indices[] after compact
    } else {
        // Just rebuild indices[]; entries[] unchanged.
        for (size_t i = 0; i < d->elen; i++) {
            VALUE k = d->entries[i].key;
            if (k == 0 || k == DICT_DELETED_KEY) continue;
            uint64_t h = d->entries[i].hash;
            size_t bi = (size_t)h & mask;
            size_t step = 0;
            while (new_indices[bi] != DICT_EMPTY_IDX) { step++; bi = (bi + step) & mask; }
            new_indices[bi] = (int32_t)i;
        }
        d->fill = d->used;
    }
    d->indices = new_indices;
    d->icapa = new_icapa;
}

static void
pydict_grow_entries(struct pydict *d)
{
    size_t nc = d->ecapa * 2;
    struct pydict_entry *ne = (struct pydict_entry *)GC_malloc(sizeof(struct pydict_entry) * nc);
    memcpy(ne, d->entries, sizeof(struct pydict_entry) * d->elen);
    d->entries = ne;
    d->ecapa = nc;
}

// Lower-level set: takes a struct pydict* and a precomputed hash.
// Used both by py_dict_set and by callers (instance attrs) that hold
// a struct pydict* directly.
static void
pydict_set_h(CTX *c, struct pydict *d, VALUE key, uint64_t h, VALUE val)
{
    size_t bucket;
    int32_t eidx;
    ssize_t first_tomb;
    pydict_indices_lookup(c, d, key, h, &bucket, &eidx, &first_tomb);
    if (eidx >= 0) {
        d->entries[eidx].value = val;
        return;
    }
    if (d->elen == d->ecapa) pydict_grow_entries(d);
    int32_t new_idx = (int32_t)d->elen;
    d->entries[new_idx].key = key;
    d->entries[new_idx].value = val;
    d->entries[new_idx].hash = h;
    d->elen++;
    d->used++;
    if (first_tomb >= 0) {
        d->indices[first_tomb] = new_idx;
    } else {
        d->indices[bucket] = new_idx;
        d->fill++;
    }
    if (d->fill * DICT_LOAD_DEN >= d->icapa * DICT_LOAD_NUM) {
        bool compact = (d->elen - d->used) * 2 >= d->elen;
        pydict_resize(d, d->icapa * 2, compact);
    }
}

void
py_dict_set(CTX *c, VALUE dv, VALUE key, VALUE val)
{
    struct pydict *d = PY_PTR(dv)->dict;
    uint64_t h = py_hash(c, key);
    pydict_set_h(c, d, key, h, val);
    return;
}

// Truthiness dispatcher for user instances: tries `__bool__` then
// `__len__`; defaults to true.  Uses the active context (no CTX
// argument needed since `py_is_truthy` is called from many places
// without a CTX in scope).
bool
py_is_truthy_instance(VALUE v)
{
    extern CTX *py_current_ctx;
    CTX *c = py_current_ctx;
    VALUE cls = PY_OBJ_VAL(PY_PTR(v)->inst.cls);
    VALUE m = py_class_lookup_method(cls, "__bool__");
    if (m != PY_NONE) {
        VALUE av[1] = { v };
        VALUE r = py_apply(c, m, 1, av);
        if (c->state == PY_STATE_RAISE) return false;
        return r == PY_TRUE || (PY_IS_FIXNUM(r) && PY_FIXVAL(r) != 0);
    }
    VALUE lm = py_class_lookup_method(cls, "__len__");
    if (lm != PY_NONE) {
        VALUE av[1] = { v };
        VALUE r = py_apply(c, lm, 1, av);
        if (c->state == PY_STATE_RAISE) return false;
        return PY_IS_FIXNUM(r) ? PY_FIXVAL(r) != 0 : true;
    }
    return true;
}

void
py_func_set_doc(CTX *c, VALUE fn, const char *s)
{
    if (!py_is_func(fn) || !s) return;
    struct pyobj *o = PY_PTR(fn);
    if (!o->func.attrs) o->func.attrs = pydict_new();
    VALUE k = py_make_str("__doc__", 7);
    pydict_set_h(c, o->func.attrs, k, py_hash(c, k), py_make_str(s, strlen(s)));
}

VALUE
py_dict_get(CTX *c, VALUE dv, VALUE key)
{
    struct pydict *d = PY_PTR(dv)->dict;
    uint64_t h = py_hash(c, key);
    size_t bucket; int32_t eidx; ssize_t ft;
    pydict_indices_lookup(c, d, key, h, &bucket, &eidx, &ft);
    if (eidx < 0) {
        VALUE r = py_to_repr(c, key);
        py_raise_exc(c, c->EXC_KeyError, "%s",
                     py_is_str(r) ? PY_PTR(r)->str.chars : "?");
    }
    return d->entries[eidx].value;
}

bool
py_dict_has(CTX *c, VALUE dv, VALUE key)
{
    struct pydict *d = PY_PTR(dv)->dict;
    uint64_t h = py_hash(c, key);
    size_t bucket; int32_t eidx; ssize_t ft;
    pydict_indices_lookup(c, d, key, h, &bucket, &eidx, &ft);
    return eidx >= 0;
}

bool
py_dict_remove(CTX *c, VALUE dv, VALUE key)
{
    struct pydict *d = PY_PTR(dv)->dict;
    uint64_t h = py_hash(c, key);
    size_t bucket; int32_t eidx; ssize_t ft;
    pydict_indices_lookup(c, d, key, h, &bucket, &eidx, &ft);
    if (eidx < 0) return false;
    d->indices[bucket] = DICT_TOMB_IDX;
    d->entries[eidx].key = DICT_DELETED_KEY;
    d->entries[eidx].value = PY_NONE;
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
        // Bignum out of long range — clamp to long extreme so slice
        // callers naturally get empty/everything.  CPython does the
        // same for slice indices.
        return mpz_sgn(PY_PTR(v)->mpz) < 0 ? INT64_MIN : INT64_MAX;
    }
    // __index__ protocol.
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__index__");
        if (m != PY_NONE) {
            VALUE r = py_apply(c, m, 1, &v);
            if (c->state == PY_STATE_RAISE) return 0;
            return py_int_to_long(c, r);
        }
    }
    py_raise_exc(c, c->EXC_TypeError, "expected an integer index");
}

// Strict variant: for non-slice indexing, oversized bignum should raise.
int64_t
py_int_to_long_strict(CTX *c, VALUE v)
{
    if (PY_IS_FIXNUM(v)) return PY_FIXVAL(v);
    if (v == PY_TRUE) return 1;
    if (v == PY_FALSE) return 0;
    if (py_is_bignum(v)) {
        if (mpz_fits_slong_p(PY_PTR(v)->mpz)) return mpz_get_si(PY_PTR(v)->mpz);
        py_raise_exc(c, c->EXC_OverflowError, "Python int too large to convert to C long");
    }
    py_raise_exc(c, c->EXC_TypeError, "expected an integer");
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
        // Built-in subclass: forward to primary.
        if (PY_PTR(seq)->inst.primary) return py_list_get(c, PY_PTR(seq)->inst.primary, idx);
    }
    // Slice index: convert to py_list_slice call.
    if (PY_IS_PTR(idx) && PY_PTR(idx)->type == PY_T_SLICE) {
        struct pyobj *sl = PY_PTR(idx);
        return py_list_slice(c, seq, sl->slice_.start, sl->slice_.stop, sl->slice_.step);
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
    if (py_is_byteseq(seq)) {
        int64_t i = py_int_to_long(c, idx);
        int64_t len = (int64_t)PY_PTR(seq)->str.len;
        i = clamp_idx(i, len, false);
        if (i < 0 || i >= len) py_raise_exc(c, c->EXC_IndexError, "bytes index out of range");
        return PY_FIX((unsigned char)PY_PTR(seq)->str.chars[i]);
    }
    if (py_is_range(seq)) {
        struct pyobj *o = PY_PTR(seq);
        size_t total = py_seq_len(c, seq);
        int64_t i = py_int_to_long(c, idx);
        if (i < 0) i += (int64_t)total;
        if (i < 0 || i >= (int64_t)total) py_raise_exc(c, c->EXC_IndexError, "range index out of range");
        return py_make_int(o->range.start + i * o->range.step);
    }
    if (py_is_dict(seq)) {
        return py_dict_get(c, seq, idx);
    }
    // `cls[arg]` — class subscript via __class_getitem__.
    if (py_is_class(seq)) {
        VALUE m = py_class_lookup_method(seq, "__class_getitem__");
        if (m != PY_NONE) {
            VALUE av[2] = { seq, idx };
            return py_apply(c, m, 2, av);
        }
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
        if (PY_PTR(seq)->inst.primary) return py_list_set(c, PY_PTR(seq)->inst.primary, idx, val);
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
    if (PY_IS_PTR(seq) && PY_PTR(seq)->type == PY_T_BYTEARRAY) {
        int64_t i = py_int_to_long(c, idx);
        int64_t len = (int64_t)PY_PTR(seq)->str.len;
        if (i < 0) i += len;
        if (i < 0 || i >= len)
            py_raise_exc(c, c->EXC_IndexError, "bytearray index out of range");
        int64_t b = py_int_to_long(c, val);
        if (b < 0 || b > 255)
            py_raise_exc(c, c->EXC_ValueError, "byte must be in range(0, 256)");
        PY_PTR(seq)->str.chars[i] = (char)b;
        return PY_NONE;
    }
    py_raise_exc(c, c->EXC_TypeError, "object does not support item assignment");
}

VALUE
py_list_slice(CTX *c, VALUE seq, VALUE start, VALUE stop, VALUE step)
{
    // User-class with __getitem__ — call with slice() object.
    if (py_is_instance(seq)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(seq)->inst.cls), "__getitem__");
        if (m != PY_NONE) {
            struct pyobj *sl = py_alloc(PY_T_SLICE);
            sl->slice_.start = start;
            sl->slice_.stop = stop;
            sl->slice_.step = step;
            VALUE av[2] = { seq, PY_OBJ_VAL(sl) };
            return py_apply(c, m, 2, av);
        }
        if (PY_PTR(seq)->inst.primary)
            return py_list_slice(c, PY_PTR(seq)->inst.primary, start, stop, step);
    }
    int64_t len;
    bool is_str = py_is_str(seq);
    bool is_byteseq = py_is_byteseq(seq);
    bool is_range_seq = py_is_range(seq);
    if (is_str) len = (int64_t)PY_PTR(seq)->str.len;
    else if (is_byteseq) len = (int64_t)PY_PTR(seq)->str.len;
    else if (py_is_list(seq) || py_is_tuple(seq)) len = (int64_t)PY_PTR(seq)->list.len;
    else if (is_range_seq) {
        struct pyobj *o = PY_PTR(seq);
        len = (o->range.step > 0)
            ? ((o->range.start >= o->range.stop) ? 0 : (o->range.stop - o->range.start + o->range.step - 1) / o->range.step)
            : ((o->range.start <= o->range.stop) ? 0 : (o->range.start - o->range.stop + (-o->range.step) - 1) / (-o->range.step));
    }
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
    if (is_byteseq) {
        char *buf = (char *)GC_malloc_atomic(n + 1);
        for (size_t i = 0; i < n; i++) buf[i] = PY_PTR(seq)->str.chars[a + (int64_t)i * st];
        buf[n] = '\0';
        return py_is_bytes(seq) ? py_make_bytes(buf, n) : py_make_bytearray(buf, n);
    }
    if (is_range_seq) {
        // Slicing a range returns a range when step is 1, else a list of ints.
        struct pyobj *o = PY_PTR(seq);
        VALUE *items = n ? (VALUE *)alloca(sizeof(VALUE) * n) : NULL;
        for (size_t i = 0; i < n; i++) {
            int64_t idx = a + (int64_t)i * st;
            items[i] = py_make_int(o->range.start + idx * o->range.step);
        }
        return py_make_list(items, n);
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
    if (py_is_instance(seq)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(seq)->inst.cls), "__setitem__");
        if (m != PY_NONE) {
            struct pyobj *sl = py_alloc(PY_T_SLICE);
            sl->slice_.start = start;
            sl->slice_.stop = stop;
            sl->slice_.step = step;
            VALUE av[3] = { seq, PY_OBJ_VAL(sl), val };
            py_apply(c, m, 3, av);
            return;
        }
        if (PY_PTR(seq)->inst.primary) {
            py_list_slice_set(c, PY_PTR(seq)->inst.primary, start, stop, step, val);
            return;
        }
    }
    if (PY_IS_PTR(seq) && PY_PTR(seq)->type == PY_T_BYTEARRAY) {
        // bytearray slice assignment.  Source must be iterable of ints
        // 0..255 (or another bytes/bytearray).
        int64_t st = (step == PY_NONE) ? 1 : py_int_to_long(c, step);
        if (st == 0) py_raise_exc(c, c->EXC_ValueError, "slice step cannot be zero");
        int64_t len = (int64_t)PY_PTR(seq)->str.len;
        int64_t a = (start == PY_NONE) ? (st > 0 ? 0 : len - 1) : py_int_to_long(c, start);
        int64_t b = (stop  == PY_NONE) ? (st > 0 ? len : -1)    : py_int_to_long(c, stop);
        if (a < 0) a += len;
        if (b < 0 && stop != PY_NONE) b += len;
        if (st > 0) { if (a < 0) a = 0; if (b > len) b = len; }
        else        { if (a >= len) a = len - 1; }
        // Materialise val as a byte buffer.
        unsigned char *vbuf;
        size_t nval;
        if (py_is_byteseq(val) || py_is_str(val)) {
            nval = PY_PTR(val)->str.len;
            vbuf = (unsigned char *)PY_PTR(val)->str.chars;
        } else {
            struct py_iter it; py_iter_init(c, &it, val);
            size_t cap = 16; nval = 0;
            unsigned char *buf = (unsigned char *)GC_malloc_atomic(cap);
            VALUE x;
            while (py_iter_next(c, &it, &x)) {
                int64_t bv = py_int_to_long(c, x);
                if (bv < 0 || bv > 255)
                    py_raise_exc(c, c->EXC_ValueError, "byte must be in range(0, 256)");
                if (nval == cap) { cap *= 2; buf = (unsigned char *)GC_realloc(buf, cap); }
                buf[nval++] = (unsigned char)bv;
            }
            vbuf = buf;
        }
        if (st == 1) {
            if (a > len) a = len;
            if (b > len) b = len;
            if (b < a)  b = a;
            size_t prefix = (size_t)a;
            size_t suffix_off = (size_t)b;
            size_t suffix_len = (size_t)(len - b);
            size_t new_len = prefix + nval + suffix_len;
            char *out = (char *)GC_malloc_atomic(new_len + 1);
            if (prefix) memcpy(out, PY_PTR(seq)->str.chars, prefix);
            if (nval)   memcpy(out + prefix, vbuf, nval);
            if (suffix_len) memcpy(out + prefix + nval,
                                   PY_PTR(seq)->str.chars + suffix_off, suffix_len);
            out[new_len] = '\0';
            PY_PTR(seq)->str.chars = out;
            PY_PTR(seq)->str.len = new_len;
            return;
        }
        // Stepped: require matching length.
        size_t target_n = 0;
        if (st > 0 && a < b) target_n = (size_t)((b - a + st - 1) / st);
        else if (st < 0 && a > b) target_n = (size_t)((a - b - st - 1) / -st);
        if (target_n != nval)
            py_raise_exc(c, c->EXC_ValueError,
                         "attempt to assign bytes of size %zu to extended slice of size %zu",
                         nval, target_n);
        for (size_t i = 0; i < nval; i++)
            PY_PTR(seq)->str.chars[a + (int64_t)i * st] = (char)vbuf[i];
        return;
    }
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

    // step != 1: requires matching length, OR nval == 0 (deletion via `del L[::s]`).
    size_t target_n = 0;
    if (st > 0 && a < b) target_n = (size_t)((b - a + st - 1) / st);
    else if (st < 0 && a > b) target_n = (size_t)((a - b - st - 1) / -st);
    if (nval == 0 && target_n > 0) {
        // Delete the addressed indices.
        bool *del = (bool *)GC_malloc_atomic(len);
        for (size_t i = 0; i < (size_t)len; i++) del[i] = false;
        for (size_t i = 0; i < target_n; i++) del[a + (int64_t)i * st] = true;
        size_t w = 0;
        VALUE *itp = PY_PTR(seq)->list.items;
        for (size_t r = 0; r < (size_t)len; r++)
            if (!del[r]) itp[w++] = itp[r];
        PY_PTR(seq)->list.len = w;
        return;
    }
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
    if (py_is_byteseq(v)) return PY_PTR(v)->str.len;
    if (py_is_list(v) || py_is_tuple(v)) return PY_PTR(v)->list.len;
    if (py_is_dict(v) || py_is_any_set(v))  return PY_PTR(v)->dict->used;
    if (py_is_range(v)) {
        struct pyobj *o = PY_PTR(v);
        if (o->range.step > 0) {
            if (o->range.start >= o->range.stop) return 0;
            return (size_t)((o->range.stop - o->range.start + o->range.step - 1) / o->range.step);
        } else {
            if (o->range.start <= o->range.stop) return 0;
            return (size_t)((o->range.start - o->range.stop + (-o->range.step) - 1) / (-o->range.step));
        }
    }
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__len__");
        if (m != PY_NONE) {
            VALUE av[1] = { v };
            VALUE r = py_apply(c, m, 1, av);
            if (PY_IS_FIXNUM(r)) return (size_t)PY_FIXVAL(r);
        }
        // Fall back to primary value (built-in subclass instance).
        if (PY_PTR(v)->inst.primary) return py_seq_len(c, PY_PTR(v)->inst.primary);
    }
    py_raise_exc(c, c->EXC_TypeError, "object has no len()");
}

bool
py_contains(CTX *c, VALUE container, VALUE v)
{
    if (py_is_list(container) || py_is_tuple(container)) {
        size_t n = PY_PTR(container)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE x = PY_PTR(container)->list.items[i];
            // Identity-equality short-circuit: handles `nan in [nan]`
            // (CPython semantics — same object equals itself even if
            // py_eq returns False due to NaN).
            if (x == v) return true;
            if (py_eq_bool(c, x, v)) return true;
        }
        return false;
    }
    if (py_is_dict(container) || py_is_any_set(container)) return py_dict_has(c, container, v);
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
    if (py_is_instance(container)) {
        VALUE cls = PY_OBJ_VAL(PY_PTR(container)->inst.cls);
        VALUE m = py_class_lookup_method(cls, "__contains__");
        if (m != PY_NONE) {
            VALUE av[2] = { container, v };
            VALUE r = py_apply(c, m, 2, av);
            return py_is_truthy(r);
        }
        // Built-in subclass: forward to primary.
        if (PY_PTR(container)->inst.primary)
            return py_contains(c, PY_PTR(container)->inst.primary, v);
        // Fall back: route through py_iter_init (handles generators,
        // built-in iters, __getitem__ protocol, etc.).
        struct py_iter it;
        py_iter_init(c, &it, container);
        if (c->state != PY_STATE_NORMAL) return false;
        VALUE x;
        while (py_iter_next(c, &it, &x)) {
            if (c->state == PY_STATE_RAISE) return false;
            if (x == v) return true;
            if (py_eq_bool(c, x, v)) return true;
        }
        return false;
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
    if (py_is_byteseq(iterable)) {
        it->kind = 6;       // bytes: yield int 0..255
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
    if (py_is_dict(iterable) || py_is_any_set(iterable)) {
        it->kind = 3;
        it->end = (int64_t)PY_PTR(iterable)->dict->elen;
        return;
    }
    // Already-an-iterator (PY_T_ITER created by `iter(seq)` builtin):
    // copy its inner state.  This makes `for x in iter(seq)` work.
    if (PY_IS_PTR(iterable) && PY_PTR(iterable)->type == PY_T_ITER) {
        *it = *PY_PTR(iterable)->iter_state;
        return;
    }
    if (py_is_instance(iterable)) {
        VALUE cls = PY_OBJ_VAL(PY_PTR(iterable)->inst.cls);
        VALUE im = py_class_lookup_method(cls, "__iter__");
        if (im != PY_NONE) {
            VALUE av[1] = { iterable };
            VALUE iter_obj = py_apply(c, im, 1, av);
            if (c->state != PY_STATE_NORMAL) return;
            // If __iter__ returned a generator object, dispatch via the
            // generator path (kind 7) rather than the user-iterator
            // path (which expects __next__ on a class).
            if (PY_IS_PTR(iter_obj) && PY_PTR(iter_obj)->type == PY_T_GEN) {
                it->kind = 7;
                it->container = iter_obj;
                return;
            }
            // If __iter__ returned a built-in iterator (PY_T_ITER),
            // unwrap and use its state directly.
            if (PY_IS_PTR(iter_obj) && PY_PTR(iter_obj)->type == PY_T_ITER) {
                *it = *PY_PTR(iter_obj)->iter_state;
                return;
            }
            it->kind = 5;       // user iterator
            it->container = iter_obj;
            it->i = 0; it->end = 0; it->step = 0;
            return;
        }
        // Built-in subclass: iterate over primary.
        if (PY_PTR(iterable)->inst.primary) {
            py_iter_init(c, it, PY_PTR(iterable)->inst.primary);
            return;
        }
        // Sequence protocol fallback: __getitem__ with integer indices.
        VALUE gm = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(iterable)->inst.cls), "__getitem__");
        if (gm != PY_NONE) {
            it->kind = 13;          // __getitem__-based iterator
            it->container = iterable;
            it->i = 0; it->end = 0; it->step = 0;
            return;
        }
    }
    if (PY_IS_PTR(iterable) && PY_PTR(iterable)->type == PY_T_GEN) {
        it->kind = 7;
        it->container = iterable;
        return;
    }
    if (PY_IS_PTR(iterable) && PY_PTR(iterable)->type == PY_T_FILE) {
        it->kind = 12;
        it->container = iterable;
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
      case 6:
        if (it->i >= it->end) return false;
        *out = PY_FIX((unsigned char)PY_PTR(it->container)->str.chars[it->i]);
        it->i++;
        return true;
      case 7: {
        extern VALUE py_gen_next(CTX *c, VALUE g);
        VALUE r = py_gen_next(c, it->container);
        if (c->state == PY_STATE_RAISE) {
            VALUE exc = c->state_value;
            if (py_exc_matches(c, exc, c->EXC_StopIteration)) {
                c->state = PY_STATE_NORMAL;
                c->state_value = PY_NONE;
                return false;
            }
            return false;
        }
        *out = r;
        return true;
      }
      case 2:
        if (it->step > 0 ? it->i >= it->end : it->i <= it->end) return false;
        *out = py_make_int(it->i);
        it->i += it->step;
        return true;
      case 3: {
        struct pydict *d = PY_PTR(it->container)->dict;
        while (it->i < it->end) {
            size_t i = (size_t)it->i++;
            if (pydict_entry_live(d, i)) {
                *out = d->entries[i].key;
                return true;
            }
        }
        return false;
      }
      case 5: {
        // User iterator: call __next__; on StopIteration, clear state
        // and return false.
        VALUE iter_obj = it->container;
        if (!py_is_instance(iter_obj)) return false;
        VALUE cls = PY_OBJ_VAL(PY_PTR(iter_obj)->inst.cls);
        VALUE nm = py_class_lookup_method(cls, "__next__");
        if (nm == PY_NONE) py_raise_exc(c, c->EXC_TypeError, "iter object has no __next__");
        VALUE av[1] = { iter_obj };
        VALUE r = py_apply(c, nm, 1, av);
        if (c->state == PY_STATE_RAISE) {
            VALUE exc = c->state_value;
            if (py_exc_matches(c, exc, c->EXC_StopIteration)) {
                c->state = PY_STATE_NORMAL;
                c->state_value = PY_NONE;
                return false;
            }
            return false;
        }
        *out = r;
        return true;
      }
      case 4: {
        // iter(callable, sentinel): call container() until result == sentinel.
        VALUE r = py_apply(c, it->container, 0, NULL);
        if (c->state == PY_STATE_RAISE) return false;
        if (py_eq_bool(c, r, it->sentinel)) return false;
        *out = r;
        return true;
      }
      case 8: {
        // enumerate: yield (i, v).  inner[0] is the source.
        VALUE v;
        if (!py_iter_next(c, &it->inner[0], &v)) return false;
        VALUE pair[2] = { PY_FIX(it->i++), v };
        *out = py_make_tuple(pair, 2);
        return true;
      }
      case 9: {
        // zip: yield tuple of one element from each inner.  Stops when
        // any inner is exhausted.  If strict (it->i != 0), raise if
        // others still produce.
        if (it->n_inner == 0) return false;  // zip() with no args is empty
        VALUE *vs = (VALUE *)alloca(sizeof(VALUE) * it->n_inner);
        for (int k = 0; k < it->n_inner; k++) {
            if (!py_iter_next(c, &it->inner[k], &vs[k])) {
                if (c->state == PY_STATE_RAISE) return false;
                if (it->i != 0) {
                    // Strict.  If k > 0, earlier iters already produced
                    // — they're "longer". If k == 0, check subsequent
                    // iters can produce one more — they're "longer".
                    if (k > 0) {
                        py_raise_exc(c, c->EXC_ValueError,
                                     "zip() argument %d is shorter than argument %d",
                                     k + 1, k);
                        return false;
                    }
                    VALUE dummy;
                    for (int j = k + 1; j < it->n_inner; j++) {
                        if (py_iter_next(c, &it->inner[j], &dummy)) {
                            py_raise_exc(c, c->EXC_ValueError,
                                         "zip() argument %d is longer than argument %d",
                                         j + 1, k + 1);
                            return false;
                        }
                        if (c->state == PY_STATE_RAISE) return false;
                    }
                }
                return false;
            }
            if (c->state == PY_STATE_RAISE) return false;
        }
        *out = py_make_tuple(vs, it->n_inner);
        return true;
      }
      case 10: {
        // map(fn, *iters): apply fn to one element from each inner.
        VALUE *vs = (VALUE *)alloca(sizeof(VALUE) * it->n_inner);
        for (int k = 0; k < it->n_inner; k++) {
            if (!py_iter_next(c, &it->inner[k], &vs[k])) return false;
            if (c->state == PY_STATE_RAISE) return false;
        }
        *out = py_apply(c, it->container, it->n_inner, vs);
        return c->state == PY_STATE_NORMAL;
      }
      case 11: {
        // filter(fn, it): yield v from inner where fn(v) is truthy.
        // fn==None means yield truthy v.
        for (;;) {
            VALUE v;
            if (!py_iter_next(c, &it->inner[0], &v)) return false;
            if (c->state == PY_STATE_RAISE) return false;
            VALUE keep;
            if (it->container == PY_NONE) keep = v;
            else {
                VALUE av[1] = { v };
                keep = py_apply(c, it->container, 1, av);
                if (c->state == PY_STATE_RAISE) return false;
            }
            if (py_is_truthy(keep)) {
                *out = v;
                return true;
            }
        }
      }
      case 12: {
        // file: yield each line.
        VALUE av[1] = { it->container };
        extern VALUE fm_readline(CTX *c, int argc, VALUE *argv);
        VALUE line = fm_readline(c, 1, av);
        if (c->state == PY_STATE_RAISE) return false;
        if (py_is_str(line) && PY_PTR(line)->str.len == 0) return false;
        *out = line;
        return true;
      }
      case 13: {
        // __getitem__ sequence protocol.
        VALUE gm = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(it->container)->inst.cls), "__getitem__");
        VALUE av[2] = { it->container, PY_FIX(it->i) };
        VALUE r = py_apply(c, gm, 2, av);
        if (c->state == PY_STATE_RAISE) {
            if (py_exc_matches(c, c->state_value, c->EXC_IndexError)
                || py_exc_matches(c, c->state_value, c->EXC_StopIteration)) {
                c->state = PY_STATE_NORMAL; c->state_value = PY_NONE;
                return false;
            }
            return false;
        }
        it->i++;
        *out = r;
        return true;
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
static struct type_method frozenset_methods[];
static struct type_method gen_methods[];
static struct type_method bytes_methods[];
static struct type_method int_methods[];
static struct type_method float_methods[];
static struct type_method complex_methods[];
static struct type_method tuple_methods[];
static struct type_method range_methods[];

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
    else if (py_is_tuple(recv)) tbl = tuple_methods;
    else if (py_is_range(recv)) tbl = range_methods;
    else if (py_is_dict(recv)) tbl = dict_methods;
    else if (py_is_set(recv))  tbl = set_methods;
    else if (py_is_frozenset(recv)) tbl = frozenset_methods;
    else if (PY_IS_PTR(recv) && PY_PTR(recv)->type == PY_T_GEN) tbl = gen_methods;
    else if (py_is_byteseq(recv)) tbl = bytes_methods;
    else if (py_is_file(recv))    { extern struct type_method file_methods[]; tbl = file_methods; }
    else if (py_int_or_bool(recv)) tbl = int_methods;
    else if (py_is_float(recv))    tbl = float_methods;
    else if (py_is_complex(recv))  tbl = complex_methods;
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
        else if (tag == PY_T_FROZENSET) tbl = frozenset_methods;
        else if (tag == PY_T_GEN)  tbl = gen_methods;
        else if (tag == PY_T_BYTES || tag == PY_T_BYTEARRAY) tbl = bytes_methods;
        else if (tag == PY_T_FILE) { extern struct type_method file_methods[]; tbl = file_methods; }
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

// Like py_getattr but returns 0 (not raised) when the attr is missing.
VALUE
py_getattr_optional(CTX *c, VALUE v, const char *name)
{
    if (py_is_instance(v)) {
        struct pyobj *o = PY_PTR(v);
        if (o->inst.attrs) {
            VALUE key = py_make_str(name, strlen(name));
            uint64_t h = py_hash(c, key);
            int32_t eidx = pydict_find(c, o->inst.attrs, key, h);
            if (eidx >= 0) return o->inst.attrs->entries[eidx].value;
        }
    }
    return 0;
}

static VALUE bi_property_setter_call(CTX *c, int argc, VALUE *argv);
static VALUE bi_property_deleter_call(CTX *c, int argc, VALUE *argv);
static VALUE bi_property_getter_call(CTX *c, int argc, VALUE *argv);

VALUE
py_getattr(CTX *c, VALUE v, const char *name)
{
    if (py_is_super(v)) {
        // Walk MRO from start_cls (exclusive) for a method named `name`.
        extern VALUE py_super_lookup(CTX *c, VALUE self, VALUE start_after_cls, const char *name);
        VALUE self = PY_PTR(v)->super_.self;
        VALUE start = PY_PTR(v)->super_.start_cls;
        VALUE m = py_super_lookup(c, self, start, name);
        if (m == PY_NONE)
            py_raise_exc(c, c->EXC_AttributeError,
                         "'super' object has no attribute '%s'", name);
        // If `m` is already a bound method (built-in dispatched via
        // primary in super_lookup), don't re-bind.
        if (py_is_bound(m)) return m;
        return py_make_bound(self, m);
    }
    if (PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_SLICE) {
        if (strcmp(name, "start") == 0) return PY_PTR(v)->slice_.start;
        if (strcmp(name, "stop") == 0)  return PY_PTR(v)->slice_.stop;
        if (strcmp(name, "step") == 0)  return PY_PTR(v)->slice_.step;
        py_raise_exc(c, c->EXC_AttributeError,
                     "'slice' object has no attribute '%s'", name);
    }
    if (py_is_range(v)) {
        struct pyobj *o = PY_PTR(v);
        if (strcmp(name, "start") == 0) return py_make_int(o->range.start);
        if (strcmp(name, "stop") == 0)  return py_make_int(o->range.stop);
        if (strcmp(name, "step") == 0)  return py_make_int(o->range.step);
        // Fall through to method lookup
    }
    if (PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_PROPERTY) {
        if (strcmp(name, "fget") == 0) return PY_PTR(v)->wrap.wrapped;
        if (strcmp(name, "fset") == 0) return PY_PTR(v)->wrap.setter;
        if (strcmp(name, "fdel") == 0) return PY_PTR(v)->wrap.deleter;
        if (strcmp(name, "setter") == 0) {
            VALUE fn = py_make_builtin("setter", bi_property_setter_call, 2, 2);
            return py_make_bound(v, fn);
        }
        if (strcmp(name, "deleter") == 0) {
            VALUE fn = py_make_builtin("deleter", bi_property_deleter_call, 2, 2);
            return py_make_bound(v, fn);
        }
        if (strcmp(name, "getter") == 0) {
            VALUE fn = py_make_builtin("getter", bi_property_getter_call, 2, 2);
            return py_make_bound(v, fn);
        }
        py_raise_exc(c, c->EXC_AttributeError,
                     "'property' object has no attribute '%s'", name);
    }
    if (py_is_complex(v)) {
        if (strcmp(name, "real") == 0) return py_make_float(PY_PTR(v)->cpx.re);
        if (strcmp(name, "imag") == 0) return py_make_float(PY_PTR(v)->cpx.im);
        if (strcmp(name, "conjugate") == 0) {
            // Return a bound method-like — but pystro has no easy way
            // to bind a builtin to a complex.  Inline as a closure:
            // for v0 just raise (caller can use `complex(c.real, -c.imag)`).
        }
    }
    if (py_is_module(v)) {
        const char *mname = PY_PTR(v)->module.name;
        if (strcmp(name, "__name__") == 0)
            return py_make_str(mname, strlen(mname));
        if (strcmp(name, "__doc__") == 0) return PY_NONE;
        struct pyglobals *g = PY_PTR(v)->module.globals;
        for (size_t i = 0; i < g->size; i++)
            if (strcmp(g->entries[i].name, name) == 0 && g->entries[i].defined)
                return g->entries[i].value;
        py_raise_exc(c, c->EXC_AttributeError, "module '%s' has no attribute '%s'",
                     PY_PTR(v)->module.name, name);
    }
    if (py_is_instance(v)) {
        struct pyobj *o = PY_PTR(v);
        if (strcmp(name, "__class__") == 0) return PY_OBJ_VAL(o->inst.cls);
        if (strcmp(name, "__dict__") == 0) {
            // Return a live alias dict over inst.attrs so mutations
            // (`obj.__dict__[k] = v`) actually persist on the instance.
            if (!o->inst.attrs) o->inst.attrs = pydict_new();
            struct pyobj *d = py_alloc(PY_T_DICT);
            d->dict = o->inst.attrs;
            return PY_OBJ_VAL(d);
        }
        // __getattribute__: user-overridable hook called before any
        // lookup.  Only fires when the user class defines it (we don't
        // install a default on object) — this prevents infinite
        // recursion when user code calls object.__getattribute__.
        if (!py_skip_getattribute_hook
            && (strncmp(name, "__", 2) != 0 || strcmp(name, "__class__") == 0
                || strcmp(name, "__dict__") == 0)) {
            VALUE ga = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), "__getattribute__");
            // Skip the default object.__getattribute__ — only user
            // overrides should fire the hook.
            if (ga != PY_NONE
                && !(PY_IS_PTR(ga) && PY_PTR(ga)->type == PY_T_BUILTIN
                     && PY_PTR(ga)->builtin.fn == bi_object_getattribute)) {
                VALUE av[2] = { v, py_make_str(name, strlen(name)) };
                return py_apply(c, ga, 2, av);
            }
        }
        if (o->inst.attrs) {
            VALUE key = py_make_str(name, strlen(name));
            uint64_t h = py_hash(c, key);
            int32_t eidx = pydict_find(c, o->inst.attrs, key, h);
            if (eidx >= 0) return o->inst.attrs->entries[eidx].value;
        }
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), name);
        if (m != PY_NONE) {
            if (PY_IS_PTR(m)) {
                int t = PY_PTR(m)->type;
                if (t == PY_T_STATICMETHOD) return PY_PTR(m)->wrap.wrapped;
                if (t == PY_T_CLASSMETHOD) return py_make_bound(PY_OBJ_VAL(o->inst.cls), PY_PTR(m)->wrap.wrapped);
                if (t == PY_T_PROPERTY) {
                    VALUE av[1] = { v };
                    return py_apply(c, PY_PTR(m)->wrap.wrapped, 1, av);
                }
                if (t == PY_T_FUNC || t == PY_T_BUILTIN)
                    return py_make_bound(v, m);
            }
            // Otherwise fall through.
        }
        // No user-class method found.  If the instance's class has a
        // built-in base AND the instance has a primary value, look up
        // the method on the primary's type and bind it.
        if (m == PY_NONE && o->inst.primary) {
            extern VALUE py_builtin_method(CTX *c, VALUE recv, const char *name);
            VALUE bm = py_builtin_method(c, o->inst.primary, name);
            if (bm != PY_NONE) {
                // py_builtin_method returns a bound built-in method
                // bound to the primary; that's exactly what we want.
                return bm;
            }
        }
        if (m != PY_NONE) {
            if (PY_IS_PTR(m)) {
                int t = PY_PTR(m)->type;
                // User-defined descriptor: if `m` is itself an instance
                // with a `__get__` method, call it as a descriptor.
                if (t == PY_T_INSTANCE) {
                    VALUE get_m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(m)->inst.cls), "__get__");
                    if (get_m != PY_NONE) {
                        VALUE av[3] = { m, v, PY_OBJ_VAL(o->inst.cls) };
                        return py_apply(c, get_m, 3, av);
                    }
                }
                return m;     // class data attribute (str / list / etc.)
            }
            return m;         // immediate (fixnum, None, True, False, flonum)
        }
        // __getattr__ fallback (only when the regular lookup misses).
        VALUE getattr_m = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), "__getattr__");
        if (getattr_m != PY_NONE) {
            VALUE av[2] = { v, py_make_str(name, strlen(name)) };
            return py_apply(c, getattr_m, 2, av);
        }
        py_raise_exc(c, c->EXC_AttributeError, "'%s' object has no attribute '%s'",
                     o->inst.cls->cls.name, name);
    }
    if (py_is_class(v)) {
        struct pyclass *cd = &PY_PTR(v)->cls;
        if (strcmp(name, "__name__") == 0)
            return py_make_str(cd->name, strlen(cd->name));
        if (strcmp(name, "__doc__") == 0) {
            VALUE d = py_class_lookup_method(v, "__doc__");
            return d;        // PY_NONE if absent
        }
        if (strcmp(name, "__module__") == 0) return py_make_str("__main__", 8);
        if (strcmp(name, "__bases__") == 0) {
            return py_make_tuple(cd->bases, cd->nbases);
        }
        if (strcmp(name, "__mro__") == 0) {
            return py_make_tuple(cd->mro, cd->nmro);
        }
        if (strcmp(name, "__dict__") == 0) {
            VALUE d = py_make_dict();
            for (int i = 0; i < cd->nmethods; i++) {
                VALUE k = py_make_str(cd->methods[i].name, strlen(cd->methods[i].name));
                py_dict_set(c, d, k, cd->methods[i].value);
            }
            return d;
        }
        if (strcmp(name, "__qualname__") == 0)
            return py_make_str(cd->name, strlen(cd->name));
        if (py_class_has_method(v, name)) {
            VALUE m = py_class_lookup_method(v, name);
            if (PY_IS_PTR(m)) {
                int t = PY_PTR(m)->type;
                if (t == PY_T_STATICMETHOD) return PY_PTR(m)->wrap.wrapped;
                if (t == PY_T_CLASSMETHOD)  return py_make_bound(v, PY_PTR(m)->wrap.wrapped);
            }
            return m;
        }
        // Built-in type class: look up method as unbound function via
        // type_method tables (e.g. `str.lower`, `list.append`).
        {
            int btag = cd->builtin_tag;
            struct type_method *tbl = NULL;
            if (btag == PY_T_STR)        tbl = str_methods;
            else if (btag == PY_T_LIST)  tbl = list_methods;
            else if (btag == PY_T_DICT)  tbl = dict_methods;
            else if (btag == PY_T_SET)   tbl = set_methods;
            else if (btag == PY_T_FROZENSET) tbl = frozenset_methods;
            else if (btag == PY_T_TUPLE) tbl = tuple_methods;
            else if (btag == PY_T_BYTES || btag == PY_T_BYTEARRAY) tbl = bytes_methods;
            else if (btag == PY_T_BIGNUM) tbl = int_methods;
            else if (btag == PY_T_FLOAT) tbl = float_methods;
            else if (btag == PY_T_COMPLEX) tbl = complex_methods;
            else if (btag == PY_T_RANGE) tbl = range_methods;
            if (tbl) {
                for (int i = 0; tbl[i].name; i++) {
                    if (strcmp(tbl[i].name, name) == 0) {
                        return py_make_builtin(tbl[i].name, tbl[i].fn,
                                               tbl[i].min_argc, tbl[i].max_argc);
                    }
                }
            }
        }
        py_raise_exc(c, c->EXC_AttributeError, "type object '%s' has no attribute '%s'",
                     cd->name, name);
    }
    if (py_is_func(v)) {
        struct pyobj *o = PY_PTR(v);
        if (strcmp(name, "__name__") == 0 || strcmp(name, "__qualname__") == 0) {
            // If user set __name__ via setattr (e.g. functools.wraps),
            // honour the override; otherwise fall back to the original
            // function name.
            if (o->func.attrs) {
                VALUE k = py_make_str(name, strlen(name));
                int32_t e = pydict_find(c, o->func.attrs, k, py_hash(c, k));
                if (e >= 0) return o->func.attrs->entries[e].value;
            }
            const char *n = o->func.name ? o->func.name : "<func>";
            return py_make_str(n, strlen(n));
        }
        if (strcmp(name, "__doc__") == 0) {
            if (o->func.attrs) {
                VALUE k = py_make_str("__doc__", 7);
                int32_t e = pydict_find(c, o->func.attrs, k, py_hash(c, k));
                if (e >= 0) return o->func.attrs->entries[e].value;
            }
            return PY_NONE;
        }
        if (strcmp(name, "__module__") == 0) return py_make_str("__main__", 8);
        if (strcmp(name, "__annotations__") == 0) return py_make_dict();
        if (strcmp(name, "__defaults__") == 0) {
            // Tuple of trailing defaults for pos-or-kw params, or None.
            if (!o->func.defaults) return PY_NONE;
            VALUE buf[32];
            int n = 0;
            for (int i = 0; i < o->func.n_pos_named && n < 32; i++) {
                VALUE d = o->func.defaults[i];
                if (d) buf[n++] = d;
            }
            if (n == 0) return PY_NONE;
            return py_make_tuple(buf, n);
        }
        if (strcmp(name, "__kwdefaults__") == 0) {
            if (!o->func.defaults) return py_make_dict();
            VALUE r = py_make_dict();
            for (int i = o->func.n_pos_named; i < o->func.nparams; i++) {
                VALUE d = o->func.defaults[i];
                if (d && o->func.param_names) {
                    py_dict_set(c, r,
                                py_make_str(o->func.param_names[i],
                                            strlen(o->func.param_names[i])), d);
                }
            }
            return r;
        }
        if (strcmp(name, "__code__") == 0 || strcmp(name, "__globals__") == 0
            || strcmp(name, "__closure__") == 0) {
            return PY_NONE;  // stubs — not modeling code/globals/closure objects
        }
        if (o->func.attrs) {
            VALUE key = py_make_str(name, strlen(name));
            uint64_t h = py_hash(c, key);
            int32_t eidx = pydict_find(c, o->func.attrs, key, h);
            if (eidx >= 0) return o->func.attrs->entries[eidx].value;
        }
        py_raise_exc(c, c->EXC_AttributeError, "function has no attribute '%s'", name);
    }
    if (py_is_builtin(v)) {
        if (strcmp(name, "__name__") == 0
            || strcmp(name, "__qualname__") == 0) {
            const char *n = PY_PTR(v)->builtin.name ? PY_PTR(v)->builtin.name : "<builtin>";
            return py_make_str(n, strlen(n));
        }
        if (strcmp(name, "__class__") == 0) return c->TYPE_builtin_function_or_method;
        if (strcmp(name, "__doc__") == 0) return PY_NONE;
        if (strcmp(name, "__module__") == 0) return py_make_str("builtins", 8);
    }
    if (py_is_bound(v)) {
        // Forward attribute lookup to the underlying func — covers
        // __doc__, __name__, etc. on bound methods.
        VALUE inner = PY_PTR(v)->bound.func;
        if (strcmp(name, "__self__") == 0) return PY_PTR(v)->bound.self;
        if (strcmp(name, "__func__") == 0) return inner;
        return py_getattr(c, inner, name);
    }
    VALUE m = py_builtin_method(c, v, name);
    if (m != PY_NONE) return m;
    if (strcmp(name, "__class__") == 0) {
        extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
        VALUE av[1] = { v };
        return bi_type(c, 1, av);
    }
    py_raise_exc(c, c->EXC_AttributeError, "object has no attribute '%s'", name);
}

static __thread int py_skip_setattr_hook = 0;

void
py_setattr(CTX *c, VALUE v, const char *name, VALUE val)
{
    if (py_is_instance(v)) {
        struct pyobj *o = PY_PTR(v);
        // __setattr__ user override
        if (!py_skip_setattr_hook) {
            VALUE sm = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), "__setattr__");
            if (sm != PY_NONE
                && !(PY_IS_PTR(sm) && PY_PTR(sm)->type == PY_T_BUILTIN
                     && PY_PTR(sm)->builtin.fn == bi_object_setattr)) {
                py_skip_setattr_hook++;
                VALUE av[3] = { v, py_make_str(name, strlen(name)), val };
                py_apply(c, sm, 3, av);
                py_skip_setattr_hook--;
                return;
            }
        }
        // Data descriptor on the class (with __set__) intercepts.
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), name);
        if (m != PY_NONE && PY_IS_PTR(m)) {
            int t = PY_PTR(m)->type;
            if (t == PY_T_PROPERTY) {
                VALUE setter = PY_PTR(m)->wrap.setter;
                if (setter == PY_NONE)
                    py_raise_exc(c, c->EXC_AttributeError,
                                 "property '%s' has no setter", name);
                VALUE av[2] = { v, val };
                py_apply(c, setter, 2, av);
                return;
            }
            if (t == PY_T_INSTANCE) {
                VALUE set_m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(m)->inst.cls), "__set__");
                if (set_m != PY_NONE) {
                    VALUE av[3] = { m, v, val };
                    py_apply(c, set_m, 3, av);
                    return;
                }
            }
        }
        // __slots__ enforcement.
        if (py_class_has_slots_anywhere(PY_OBJ_VAL(o->inst.cls))
            && !py_class_slot_allowed(PY_OBJ_VAL(o->inst.cls), name)) {
            py_raise_exc(c, c->EXC_AttributeError,
                         "'%s' object has no attribute '%s'",
                         o->inst.cls->cls.name, name);
        }
        if (!o->inst.attrs) o->inst.attrs = pydict_new();
        VALUE key = py_make_str(name, strlen(name));
        uint64_t h = py_hash(c, key);
        pydict_set_h(c, o->inst.attrs, key, h, val);
        return;
    }
    if (py_is_class(v)) {
        extern const char *intern_name(const char *s, size_t len);
        // Special-case __name__: update the C-level cls.name so
        // type() / repr / etc. see the new name.
        if (strcmp(name, "__name__") == 0 && py_is_str(val)) {
            const char *nn = intern_name(PY_PTR(val)->str.chars, PY_PTR(val)->str.len);
            PY_PTR(v)->cls.name = nn;
            return;
        }
        // Otherwise, store via method table.
        py_class_add_method(c, v, intern_name(name, strlen(name)), val);
        SHARED_GLOBALS_SERIAL++;
        return;
    }
    if (py_is_func(v)) {
        struct pyobj *o = PY_PTR(v);
        if (!o->func.attrs) o->func.attrs = pydict_new();
        VALUE key = py_make_str(name, strlen(name));
        uint64_t h = py_hash(c, key);
        pydict_set_h(c, o->func.attrs, key, h, val);
        return;
    }
    if (py_is_module(v)) {
        struct pyglobals *g = PY_PTR(v)->module.globals;
        // Find or insert in module globals.
        for (size_t i = 0; i < g->size; i++) {
            if (strcmp(g->entries[i].name, name) == 0) {
                g->entries[i].value = val;
                g->entries[i].defined = true;
                return;
            }
        }
        // Append (uses the same growth rules as py_global_define).
        struct pyglobals *saved = c->globals;
        c->globals = g;
        py_global_define(c, name, val);
        c->globals = saved;
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

    // Place each kwarg.  Pos-only slots (j < n_pos_only) are matchable
    // ONLY via **kwargs (their name belongs to a positional-only param).
    int n_pos_only = f->func.n_pos_only;
    for (int i = 0; i < kwc; i++) {
        int slot = -1;
        if (f->func.param_names) {
            for (int j = n_pos_only; j < n_pos_named; j++) {
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
            // Check if name matches a pos-only param: helpful diagnostic.
            if (f->func.param_names) {
                for (int j = 0; j < n_pos_only; j++) {
                    if (f->func.param_names[j] && strcmp(f->func.param_names[j], kwnames[i]) == 0) {
                        py_raise_exc(c, c->EXC_TypeError,
                            "%s() got some positional-only arguments passed as keyword arguments: '%s'",
                            f->func.name ? f->func.name : "<anonymous>", kwnames[i]);
                    }
                }
            }
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
    VALUE saved_mc = c->method_class;
    struct pyglobals *saved_g = c->globals;
    c->env = new_env;
    c->method_class = f->func.defining_class;
    if (f->func.fglobals) c->globals = f->func.fglobals;
    EVAL(c, f->func.body);
    c->env = saved;
    c->method_class = saved_mc;
    c->globals = saved_g;
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
    if (py_is_instance(fn)) {
        VALUE call = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(fn)->inst.cls), "__call__");
        if (call != PY_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = fn;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            return py_apply_kw(c, call, argc + 1, av, kwc, kwnames, kwvalues);
        }
    }
    if (py_is_class(fn)) {
        if (PY_PTR(fn)->cls.builtin_ctor) {
            // Forward kwargs through the thread-local pointers used by
            // pystro_bi_kwarg().
            int saved_kwc = PYSTRO_BI_KWC;
            const char **saved_kn = PYSTRO_BI_KWNAMES;
            VALUE *saved_kv = PYSTRO_BI_KWVALUES;
            PYSTRO_BI_KWC = kwc;
            PYSTRO_BI_KWNAMES = (const char **)kwnames;
            PYSTRO_BI_KWVALUES = kwvalues;
            VALUE r = PY_PTR(fn)->cls.builtin_ctor(c, argc, argv);
            PYSTRO_BI_KWC = saved_kwc;
            PYSTRO_BI_KWNAMES = saved_kn;
            PYSTRO_BI_KWVALUES = saved_kv;
            return r;
        }
        // Custom __new__: lets users intercept instance creation (singleton
        // pattern, immutable types, etc.).  Return value of __new__ becomes
        // the instance; if it's an instance of cls, __init__ runs on it.
        VALUE inst;
        VALUE new_m = py_class_lookup_method(fn, "__new__");
        if (new_m != PY_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = fn;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            inst = py_apply_kw(c, new_m, argc + 1, av, kwc, kwnames, kwvalues);
            if (UNLIKELY(c->state == PY_STATE_RAISE)) return PY_NONE;
        } else {
            inst = py_make_instance(fn);
        }
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
    if (py_is_func(fn)) {
        extern bool py_func_is_generator(VALUE fn);
        extern VALUE py_make_gen(CTX *c, VALUE fn, int argc, VALUE *argv, int kwc, const char **kwn, VALUE *kwv);
        if (py_func_is_generator(fn))
            return py_make_gen(c, fn, argc, argv, kwc, kwnames, kwvalues);
        return py_apply_kw_func(c, fn, argc, argv, kwc, kwnames, kwvalues);
    }
    if (py_is_builtin(fn)) {
        if (kwc > 0) {
            // A few builtins accept specific keyword arguments
            // (sorted: key, reverse / min/max: key, default / enumerate: start).
            // The kwargs are forwarded to the builtin via thread-local
            // pointers; the builtin itself reads them from there.
            extern int    PYSTRO_BI_KWC;
            extern const char **PYSTRO_BI_KWNAMES;
            extern VALUE *PYSTRO_BI_KWVALUES;
            int saved_kwc = PYSTRO_BI_KWC;
            const char **saved_kn = PYSTRO_BI_KWNAMES;
            VALUE *saved_kv = PYSTRO_BI_KWVALUES;
            PYSTRO_BI_KWC = kwc;
            PYSTRO_BI_KWNAMES = (const char **)kwnames;
            PYSTRO_BI_KWVALUES = kwvalues;
            VALUE r = py_apply_slow(c, fn, argc, argv);
            PYSTRO_BI_KWC = saved_kwc;
            PYSTRO_BI_KWNAMES = saved_kn;
            PYSTRO_BI_KWVALUES = saved_kv;
            return r;
        }
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
        // Built-in type class (int / list / str / ...): call its
        // C constructor directly.  Result is a primitive value, not a
        // PY_T_INSTANCE, since the constructor returns int/list/etc.
        if (PY_PTR(fn)->cls.builtin_ctor) {
            return PY_PTR(fn)->cls.builtin_ctor(c, argc, argv);
        }
        // __new__ — always defined (object.__new__ is the default).
        // It returns the new instance and handles built-in subclass
        // primary value setup.
        VALUE inst;
        VALUE new_m = py_class_lookup_method(fn, "__new__");
        if (new_m != PY_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = fn;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            inst = py_apply(c, new_m, argc + 1, av);
            if (UNLIKELY(c->state == PY_STATE_RAISE)) return PY_NONE;
        } else {
            inst = py_make_instance(fn);
            VALUE bin_base = py_class_find_builtin_base(fn);
            if (bin_base != PY_NONE) {
                VALUE primary = PY_PTR(bin_base)->cls.builtin_ctor(c, argc, argv);
                if (UNLIKELY(c->state == PY_STATE_RAISE)) return PY_NONE;
                PY_PTR(inst)->inst.primary = primary;
            }
        }
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
        extern bool py_func_is_generator(VALUE fn);
        extern VALUE py_make_gen(CTX *c, VALUE fn, int argc, VALUE *argv, int kwc, const char **kwn, VALUE *kwv);
        if (py_func_is_generator(fn))
            return py_make_gen(c, fn, argc, argv, 0, NULL, NULL);
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
    // Instance with __call__ — dispatch to it with self prepended.
    if (py_is_instance(fn)) {
        VALUE call = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(fn)->inst.cls), "__call__");
        if (call != PY_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = fn;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            return py_apply(c, call, argc + 1, av);
        }
    }
    py_raise_exc(c, c->EXC_TypeError, "object is not callable");
}

// ---------------------------------------------------------------------------
// Display + repr.
// ---------------------------------------------------------------------------

// Visiting set for cycle-safe py_display.  Static because py_display
// is single-threaded (no concurrent prints) and reentrant — a list
// item that's the parent list itself would otherwise stack-overflow.
#define PY_DISPLAY_MAX_DEPTH 64
static struct pyobj *py_display_visit[PY_DISPLAY_MAX_DEPTH];
static int           py_display_visit_top = 0;

static inline bool
py_display_seen(struct pyobj *o)
{
    for (int i = 0; i < py_display_visit_top; i++)
        if (py_display_visit[i] == o) return true;
    return false;
}

// Format a double using the shortest decimal that round-trips back to
// the same value (matches CPython's repr).  Tries %.NNg for NN=1..17
// and picks the smallest NN whose result parses back to the exact d.
// If the chosen format used scientific notation but |d| is within the
// "no-exponent" range, redo with %.NNe-derived %f-style.
static void
py_fmt_double(char *buf, size_t bufsz, double d)
{
    if (d != d) { snprintf(buf, bufsz, "nan"); return; }
    if (d > 1e308 || d < -1e308) { snprintf(buf, bufsz, d > 0 ? "inf" : "-inf"); return; }
    int chosen_prec = 17;
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, bufsz, "%.*g", prec, d);
        double back = strtod(buf, NULL);
        if (back == d) { chosen_prec = prec; break; }
    }
    // Detect 'e' in result.  CPython prefers fixed when -4 < exp < 16.
    // We approximate: if d is finite and |d| in [1e-4, 1e16), force
    // non-scientific.
    bool has_exp = false;
    for (const char *p = buf; *p; p++) if (*p == 'e' || *p == 'E') { has_exp = true; break; }
    double absd = d < 0 ? -d : d;
    if (has_exp && absd >= 1e-4 && absd < 1e16 && d != 0.0) {
        // Compute exponent (integer log10 of |d|).
        int exp = (int)floor(log10(absd));
        // The non-scientific form needs (chosen_prec - 1 - exp) decimal places.
        int after_dot = chosen_prec - 1 - exp;
        if (after_dot < 0) after_dot = 0;
        snprintf(buf, bufsz, "%.*f", after_dot, d);
        // Verify roundtrip; if not, fall back to scientific (already in buf).
        double back = strtod(buf, NULL);
        if (back != d) {
            snprintf(buf, bufsz, "%.*g", chosen_prec, d);
        }
    }
}

void
py_display(FILE *fp, VALUE v, bool repr)
{
    if (PY_IS_FIXNUM(v)) { fprintf(fp, "%ld", (long)PY_FIXVAL(v)); return; }
    if (PY_IS_FLONUM(v)) {
        double d = py_flonum_to_double(v);
        char buf[64]; py_fmt_double(buf, sizeof(buf), d);
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
        py_fmt_double(buf, sizeof(buf), o->dbl);
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
      case PY_T_COMPLEX: {
        char re[64], im[64];
        py_fmt_double(re, sizeof(re), o->cpx.re);
        py_fmt_double(im, sizeof(im), o->cpx.im);
        if (o->cpx.re == 0.0) {
            fprintf(fp, "%sj", im);
        } else {
            fprintf(fp, "(%s%s%sj)", re, (o->cpx.im >= 0) ? "+" : "", im);
        }
        return;
      }
      case PY_T_STR:
        if (repr) {
            // Pick quote like Python: single, unless the string has
            // single but no double — then double.
            bool has_sq = false, has_dq = false;
            for (size_t i = 0; i < o->str.len; i++) {
                if (o->str.chars[i] == '\'') has_sq = true;
                else if (o->str.chars[i] == '"') has_dq = true;
            }
            char q = (has_sq && !has_dq) ? '"' : '\'';
            fputc(q, fp);
            for (size_t i = 0; i < o->str.len; i++) {
                char ch = o->str.chars[i];
                if      (ch == '\\') fputs("\\\\", fp);
                else if (ch == q)    { fputc('\\', fp); fputc(q, fp); }
                else if (ch == '\n') fputs("\\n", fp);
                else if (ch == '\t') fputs("\\t", fp);
                else                 fputc(ch, fp);
            }
            fputc(q, fp);
        } else {
            fwrite(o->str.chars, 1, o->str.len, fp);
        }
        return;
      case PY_T_BYTES:
      case PY_T_BYTEARRAY: {
        if (o->type == PY_T_BYTEARRAY) fputs("bytearray(", fp);
        fputs("b'", fp);
        for (size_t i = 0; i < o->str.len; i++) {
            unsigned char ch = (unsigned char)o->str.chars[i];
            if      (ch == '\\') fputs("\\\\", fp);
            else if (ch == '\'') fputs("\\'", fp);
            else if (ch == '\n') fputs("\\n", fp);
            else if (ch == '\t') fputs("\\t", fp);
            else if (ch >= 32 && ch < 127) fputc((char)ch, fp);
            else fprintf(fp, "\\x%02x", ch);
        }
        fputc('\'', fp);
        if (o->type == PY_T_BYTEARRAY) fputc(')', fp);
        return;
      }
      case PY_T_LIST:
        if (py_display_seen(o)) { fputs("[...]", fp); return; }
        if (py_display_visit_top < PY_DISPLAY_MAX_DEPTH)
            py_display_visit[py_display_visit_top++] = o;
        fputc('[', fp);
        for (size_t i = 0; i < o->list.len; i++) {
            if (i) fputs(", ", fp);
            py_display(fp, o->list.items[i], true);
        }
        fputc(']', fp);
        py_display_visit_top--;
        return;
      case PY_T_TUPLE:
        if (py_display_seen(o)) { fputs("(...)", fp); return; }
        if (py_display_visit_top < PY_DISPLAY_MAX_DEPTH)
            py_display_visit[py_display_visit_top++] = o;
        fputc('(', fp);
        for (size_t i = 0; i < o->list.len; i++) {
            if (i) fputs(", ", fp);
            py_display(fp, o->list.items[i], true);
        }
        if (o->list.len == 1) fputc(',', fp);
        fputc(')', fp);
        py_display_visit_top--;
        return;
      case PY_T_DICT: {
        if (py_display_seen(o)) { fputs("{...}", fp); return; }
        if (py_display_visit_top < PY_DISPLAY_MAX_DEPTH)
            py_display_visit[py_display_visit_top++] = o;
        fputc('{', fp);
        struct pydict *d = o->dict;
        size_t printed = 0;
        for (size_t i = 0; i < d->elen; i++) {
            if (!pydict_entry_live(d, i)) continue;
            if (printed++) fputs(", ", fp);
            py_display(fp, d->entries[i].key, true);
            fputs(": ", fp);
            py_display(fp, d->entries[i].value, true);
        }
        fputc('}', fp);
        py_display_visit_top--;
        return;
      }
      case PY_T_SET: {
        struct pydict *d = o->dict;
        if (d->used == 0) { fputs("set()", fp); return; }
        fputc('{', fp);
        size_t printed = 0;
        for (size_t i = 0; i < d->elen; i++) {
            if (!pydict_entry_live(d, i)) continue;
            if (printed++) fputs(", ", fp);
            py_display(fp, d->entries[i].key, true);
        }
        fputc('}', fp);
        return;
      }
      case PY_T_FROZENSET: {
        struct pydict *d = o->dict;
        fputs("frozenset(", fp);
        if (d->used > 0) {
            fputc('{', fp);
            size_t printed = 0;
            for (size_t i = 0; i < d->elen; i++) {
                if (!pydict_entry_live(d, i)) continue;
                if (printed++) fputs(", ", fp);
                py_display(fp, d->entries[i].key, true);
            }
            fputc('}', fp);
        }
        fputc(')', fp);
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
      case PY_T_BUILTIN: {
        // Built-in type constructors (int, str, list, ...) display as
        // `<class 'name'>` to match CPython's repr — they are the
        // canonical type objects for their respective values.
        const char *nm = o->builtin.name;
        bool is_type =
            strcmp(nm, "int") == 0 || strcmp(nm, "float") == 0 ||
            strcmp(nm, "str") == 0 || strcmp(nm, "bool") == 0 ||
            strcmp(nm, "list") == 0 || strcmp(nm, "tuple") == 0 ||
            strcmp(nm, "dict") == 0 || strcmp(nm, "set") == 0 ||
            strcmp(nm, "frozenset") == 0 || strcmp(nm, "bytes") == 0 ||
            strcmp(nm, "bytearray") == 0 || strcmp(nm, "range") == 0 ||
            strcmp(nm, "complex") == 0;
        if (is_type) fprintf(fp, "<class '%s'>", nm);
        else         fprintf(fp, "<built-in function %s>", nm);
        return;
      }
      case PY_T_BOUND_METHOD:
        fprintf(fp, "<bound method>");
        return;
      case PY_T_SLICE: {
        fputs("slice(", fp);
        py_display(fp, o->slice_.start, true);
        fputs(", ", fp);
        py_display(fp, o->slice_.stop, true);
        fputs(", ", fp);
        py_display(fp, o->slice_.step, true);
        fputc(')', fp);
        return;
      }
      case PY_T_ELLIPSIS:
        fputs("Ellipsis", fp);
        return;
      case PY_T_NOTIMPL:
        fputs("NotImplemented", fp);
        return;
      case PY_T_MEMVIEW:
        fprintf(fp, "<memory>");
        return;
      case PY_T_MODULE:
        fprintf(fp, "<module '%s'>", o->module.name);
        return;
      case PY_T_CLASS:
        fprintf(fp, "<class '%s'>", o->cls.name);
        return;
      case PY_T_INSTANCE: {
        extern bool class_is_ancestor(VALUE cls, VALUE target);
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
            // Default str() / repr() for exception instances.  CPython
            // uses the .args tuple:
            //   0 args → ""           1 arg → str(args[0])
            //   N args → repr(args)   (i.e. "(a, b, ...)")
            // repr always produces ClassName(args...).
            if (py_is_class(PY_OBJ_VAL(o->inst.cls))
                && py_current_ctx->EXC_Exception
                && class_is_ancestor(PY_OBJ_VAL(o->inst.cls), py_current_ctx->EXC_Exception)) {
                VALUE args = py_getattr(py_current_ctx, v, "args");
                if (py_current_ctx->state != PY_STATE_NORMAL) {
                    py_current_ctx->state = PY_STATE_NORMAL;
                    args = PY_NONE;
                }
                if (py_is_tuple(args)) {
                    size_t n = PY_PTR(args)->list.len;
                    if (!repr) {
                        if (n == 0) return;
                        if (n == 1) {
                            py_display(fp, PY_PTR(args)->list.items[0], false);
                            return;
                        }
                        // Fall through: print tuple repr.
                        py_display(fp, args, true);
                        return;
                    }
                    fprintf(fp, "%s(", o->inst.cls->cls.name);
                    for (size_t i = 0; i < n; i++) {
                        if (i) fputs(", ", fp);
                        py_display(fp, PY_PTR(args)->list.items[i], true);
                    }
                    fputc(')', fp);
                    return;
                }
                // Fallback: fall through to "<ClassName object>"
            }
        }
        // Built-in subclass with a primary value: mirror the primary's
        // display so subclasses of int/list/tuple/etc. don't show as
        // generic "<MyInt object>".
        if (o->inst.primary) {
            py_display(fp, o->inst.primary, repr);
            return;
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

// True if `target` appears in `cls`'s C3 MRO.
bool
class_is_ancestor(VALUE cls, VALUE target)
{
    if (cls == PY_NONE || !py_is_class(cls)) return false;
    struct pyclass *cd = &PY_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) if (cd->mro[i] == target) return true;
    return false;
}

bool
py_exc_matches(CTX *c, VALUE exc, VALUE cls)
{
    if (!py_is_instance(exc)) return false;
    // Tuple of classes: match any.
    if (py_is_tuple(cls)) {
        size_t n = PY_PTR(cls)->list.len;
        for (size_t i = 0; i < n; i++)
            if (py_exc_matches(c, exc, PY_PTR(cls)->list.items[i])) return true;
        return false;
    }
    if (!py_is_class(cls)) return false;
    return class_is_ancestor(PY_OBJ_VAL(PY_PTR(exc)->inst.cls), cls);
}

// ---------------------------------------------------------------------------
// Generators (ucontext-based lazy yield).
//
// A generator function (one whose body contains `yield`) is detected by
// the parser; calling it does NOT run the body — instead it builds a
// PY_T_GEN object holding the captured args + a fresh stack + an
// uninitialised `body_ctx`.  next() / iter().__next__ swap into the
// body, which runs until it hits `yield expr`; yield stashes the value
// and swaps back.  The C stack used by the generator is a separate
// mmap-style allocation (we use GC_malloc atomic; Boehm scans the
// caller's stack but not pointers ON the gen's stack — to keep VALUEs
// alive we add the stack region to GC_add_roots before running).
// ---------------------------------------------------------------------------

#include <sys/mman.h>

#define PYGEN_STACK_SZ (256 * 1024)

struct pygen {
    ucontext_t caller_ctx;
    ucontext_t body_ctx;
    void      *stack;
    bool       started;
    bool       done;
    VALUE      yield_value;
    VALUE      return_value;     // value from `return X` in gen body
    // Captured at instantiation:
    VALUE      func;
    int        argc;
    VALUE     *argv;
    int        kwc;
    const char **kwnames;
    VALUE     *kwvalues;
    // Saved gen-side CTX at every yield/exit:
    struct pyframe *gen_env;
    struct pyglobals *gen_globals;
    VALUE       gen_method_class;
    int         gen_state;
    VALUE       gen_state_value;
    int         gen_try_top;
    jmp_buf    *gen_try_stack[64];
    // Send value carried into the body — yield expression evaluates to
    // it.  Default PY_NONE for plain next().
    VALUE       send_value;
    // throw() / close() — when set, yield's swap-back raises this
    // exception inside the body.
    VALUE       throw_exc;
    bool        throw_pending;
    // Back-link to outer enclosing gen (NULL if outermost) so nested
    // gens compose.
    struct pygen *prev_gen;
};

// makecontext takes only int args; pass the gen pointer through a
// process-global slot.
static struct pygen *G_gen_to_start = NULL;

static VALUE py_apply_gen_call(CTX *c, VALUE fn, int argc, VALUE *argv, int kwc, const char **kwn, VALUE *kwv);

static void
gen_entry(void)
{
    extern CTX *py_current_ctx;
    CTX *c = py_current_ctx;
    struct pygen *g = G_gen_to_start;
    G_gen_to_start = NULL;

    // Same setup as py_apply_kw_func: build a frame, fill from argv /
    // kwargs / defaults, EVAL the body.
    extern VALUE py_apply_kw_func(CTX *c, VALUE fn, int argc, VALUE *argv,
                                  int kwc, const char **kwnames, VALUE *kwvalues);
    // We can't simply call py_apply_kw_func because that would set up
    // CTX state including saving/restoring c->env around EVAL, but we
    // want the call-stack to STAY in the gen's context until done.
    // Instead emulate: build the frame, set CTX, run, set done.
    struct pyobj *f = PY_PTR(g->func);
    int needed = f->func.nparams;
    int n_pos_named = f->func.n_pos_named;
    bool has_va = f->func.has_varargs;
    bool has_kw = f->func.has_kwargs;
    int va_slot = has_va ? n_pos_named : -1;
    int kwonly_start = n_pos_named + (has_va ? 1 : 0);
    int n_kwonly = needed - kwonly_start - (has_kw ? 1 : 0);
    int kw_slot = has_kw ? (kwonly_start + n_kwonly) : -1;

    struct pyframe *new_env = py_new_frame(f->func.env, f->func.nlocals);
    bool *filled = (bool *)alloca(sizeof(bool) * (needed > 0 ? needed : 1));
    for (int i = 0; i < needed; i++) filled[i] = false;
    int pos_into = g->argc < n_pos_named ? g->argc : n_pos_named;
    for (int i = 0; i < pos_into; i++) { new_env->slots[i] = g->argv[i]; filled[i] = true; }
    if (has_va) {
        int n_extra = g->argc - pos_into;
        VALUE *items = n_extra > 0 ? (VALUE *)alloca(sizeof(VALUE) * n_extra) : NULL;
        for (int i = 0; i < n_extra; i++) items[i] = g->argv[pos_into + i];
        new_env->slots[va_slot] = py_make_tuple(items, n_extra);
        filled[va_slot] = true;
    }
    if (has_kw) {
        new_env->slots[kw_slot] = py_make_dict();
        filled[kw_slot] = true;
    }
    for (int i = 0; i < g->kwc; i++) {
        int slot = -1;
        if (f->func.param_names) {
            for (int j = 0; j < n_pos_named; j++)
                if (f->func.param_names[j] && strcmp(f->func.param_names[j], g->kwnames[i]) == 0) { slot = j; break; }
            if (slot < 0)
                for (int j = kwonly_start; j < kwonly_start + n_kwonly; j++)
                    if (f->func.param_names[j] && strcmp(f->func.param_names[j], g->kwnames[i]) == 0) { slot = j; break; }
        }
        if (slot >= 0) { new_env->slots[slot] = g->kwvalues[i]; filled[slot] = true; }
        else if (has_kw) py_dict_set(c, new_env->slots[kw_slot],
                                     py_make_str(g->kwnames[i], strlen(g->kwnames[i])),
                                     g->kwvalues[i]);
    }
    for (int i = 0; i < needed; i++) {
        if (filled[i] || i == va_slot || i == kw_slot) continue;
        VALUE d = f->func.defaults[i];
        if (d == (VALUE)0) {
            // missing required — set None and let the body misbehave;
            // gen creation should have caught this.
            new_env->slots[i] = PY_NONE;
        } else new_env->slots[i] = d;
    }

    c->env = new_env;
    c->globals = f->func.fglobals ? f->func.fglobals : c->globals;
    c->method_class = f->func.defining_class;
    g->prev_gen = c->current_gen;
    c->current_gen = g;
    // Reset try-stack inside the gen body: caller's jbufs are pointers
    // into the caller's C stack, but we're now running on a separate
    // ucontext stack.  Longjmping into them would land in dead frames.
    // Instead, gen body has its own (initially empty) try-stack — when
    // an exception escapes the gen, it sets state=RAISE and returns
    // here; the swapcontext-back path in py_gen_next propagates it.
    c->try_top = 0;

    EVAL(c, f->func.body);

    // If gen body executed `return X`, capture X for StopIteration.value.
    if (c->state == PY_STATE_RETURN) {
        g->return_value = c->state_value;
        c->state = PY_STATE_NORMAL;
        c->state_value = PY_NONE;
    } else {
        g->return_value = PY_NONE;
    }
    g->done = true;
    c->current_gen = g->prev_gen;
    swapcontext(&g->body_ctx, &g->caller_ctx);
    // unreachable
}

VALUE
py_make_gen(CTX *c, VALUE fn, int argc, VALUE *argv, int kwc, const char **kwn, VALUE *kwv)
{
    (void)c;
    struct pygen *g = (struct pygen *)GC_malloc(sizeof(struct pygen));
    g->stack = GC_malloc(PYGEN_STACK_SZ);
    g->started = false;
    g->done = false;
    g->send_value = PY_NONE;
    g->throw_exc = PY_NONE;
    g->throw_pending = false;
    g->func = fn;
    g->argc = argc;
    g->argv = (VALUE *)GC_malloc(sizeof(VALUE) * (argc ? argc : 1));
    for (int i = 0; i < argc; i++) g->argv[i] = argv[i];
    g->kwc = kwc;
    if (kwc > 0) {
        g->kwnames = (const char **)GC_malloc(sizeof(char *) * kwc);
        g->kwvalues = (VALUE *)GC_malloc(sizeof(VALUE) * kwc);
        for (int i = 0; i < kwc; i++) { g->kwnames[i] = kwn[i]; g->kwvalues[i] = kwv[i]; }
    }
    g->prev_gen = NULL;
    struct pyobj *o = py_alloc(PY_T_GEN);
    o->gen = g;
    return PY_OBJ_VAL(o);
}

VALUE
py_gen_next(CTX *c, VALUE gen_v)
{
    struct pygen *g = PY_PTR(gen_v)->gen;
    if (g->done) {
        // Set RAISE without longjmp so the caller's iter loop can
        // catch StopIteration without setjmp gymnastics.
        c->state = PY_STATE_RAISE;
        VALUE si = py_make_instance(c->EXC_StopIteration);
        py_setattr(c, si, "value",
                   g->return_value ? g->return_value : PY_NONE);
        c->state_value = si;
        return PY_NONE;
    }

    // Save caller state on this stack frame.
    struct pyframe *saved_env = c->env;
    int saved_state = c->state;
    VALUE saved_sval = c->state_value;
    VALUE saved_mc = c->method_class;
    struct pyglobals *saved_g = c->globals;
    struct pygen *saved_cg = c->current_gen;
    int saved_try_top = c->try_top;
    jmp_buf *saved_try_stack[64];
    if (saved_try_top > 0) memcpy(saved_try_stack, c->try_stack, saved_try_top * sizeof(jmp_buf *));

    if (!g->started) {
        g->started = true;
        getcontext(&g->body_ctx);
        g->body_ctx.uc_stack.ss_sp = g->stack;
        g->body_ctx.uc_stack.ss_size = PYGEN_STACK_SZ;
        g->body_ctx.uc_link = &g->caller_ctx;
        G_gen_to_start = g;
        makecontext(&g->body_ctx, gen_entry, 0);
    } else {
        // Restore gen state for resume.
        c->env = g->gen_env;
        c->state = g->gen_state;
        c->state_value = g->gen_state_value;
        c->method_class = g->gen_method_class;
        c->globals = g->gen_globals;
        c->current_gen = g;
        c->try_top = g->gen_try_top;
        if (g->gen_try_top > 0)
            memcpy(c->try_stack, g->gen_try_stack, g->gen_try_top * sizeof(jmp_buf *));
    }
    swapcontext(&g->caller_ctx, &g->body_ctx);
    // Body yielded or finished.  Save gen-side CTX so resume restores it.
    g->gen_env = c->env;
    g->gen_state = c->state;
    g->gen_state_value = c->state_value;
    g->gen_method_class = c->method_class;
    g->gen_globals = c->globals;
    g->gen_try_top = c->try_top;
    if (c->try_top > 0)
        memcpy(g->gen_try_stack, c->try_stack, c->try_top * sizeof(jmp_buf *));

    bool was_done = g->done;
    bool raised = (c->state == PY_STATE_RAISE);
    VALUE exc = c->state_value;
    VALUE r = was_done ? PY_NONE : g->yield_value;

    // Restore caller state.
    c->env = saved_env;
    c->state = saved_state;
    c->state_value = saved_sval;
    c->method_class = saved_mc;
    c->globals = saved_g;
    c->current_gen = saved_cg;
    c->try_top = saved_try_top;
    if (saved_try_top > 0)
        memcpy(c->try_stack, saved_try_stack, saved_try_top * sizeof(jmp_buf *));

    if (raised) { c->state = PY_STATE_RAISE; c->state_value = exc; return PY_NONE; }
    if (was_done) {
        VALUE si = py_make_instance(c->EXC_StopIteration);
        py_setattr(c, si, "value", g->return_value);
        // args = (value,) when value is non-None, else ()
        if (g->return_value != PY_NONE)
            py_setattr(c, si, "args", py_make_tuple(&g->return_value, 1));
        c->state = PY_STATE_RAISE;
        c->state_value = si;
        return PY_NONE;
    }
    return r;
}

// Returns the value passed to .send() / .next() — yield expression
// evaluates to this.  When throw() was used, sets the pending exc
// state instead so the EVAL chain unwinds out of the yield site.
VALUE
py_gen_yield(CTX *c, VALUE v)
{
    struct pygen *g = c->current_gen;
    if (!g) py_raise_exc(c, c->EXC_RuntimeError, "yield outside of generator");
    g->yield_value = v;
    // Reset send_value / throw to defaults; py_gen_send / py_gen_throw
    // overwrite before swap.
    g->send_value = PY_NONE;
    g->throw_pending = false;
    // py_gen_next() does the save/restore around the swap.
    swapcontext(&g->body_ctx, &g->caller_ctx);
    // We're back in the body.  If the caller threw, raise inside body.
    if (g->throw_pending) {
        g->throw_pending = false;
        c->state = PY_STATE_RAISE;
        c->state_value = g->throw_exc;
        if (c->try_top > 0) longjmp(*c->try_stack[c->try_top - 1], 1);
        // No try in body — propagate via state; gen_entry sees state
        // RAISE and exits, marking done.
        return PY_NONE;
    }
    return g->send_value;
}

// `yield from iter` — yield each value from the iterable, return the
// final StopIteration.value when exhausted.  Used as an expression so
// the value can be bound: `result = yield from gen()`.
VALUE
py_gen_yield_from(CTX *c, VALUE iter)
{
    struct py_iter it;
    py_iter_init(c, &it, iter);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    VALUE result = PY_NONE;
    while (py_iter_next(c, &it, &x)) {
        py_gen_yield(c, x);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
    }
    // Inner exhausted normally.  If the source is a generator with a
    // captured return-value, surface it as our expression value.
    if (PY_IS_PTR(iter) && PY_PTR(iter)->type == PY_T_GEN) {
        struct pygen *gg = PY_PTR(iter)->gen;
        if (gg->return_value) result = gg->return_value;
    }
    return result;
}

VALUE
py_gen_send(CTX *c, VALUE gen_v, VALUE v)
{
    struct pygen *g = PY_PTR(gen_v)->gen;
    if (!g->started && v != PY_NONE) {
        py_raise_exc(c, c->EXC_TypeError,
                     "can't send non-None value to a just-started generator");
        return PY_NONE;
    }
    g->send_value = v;
    return py_gen_next(c, gen_v);
}

VALUE
py_gen_throw(CTX *c, VALUE gen_v, VALUE exc)
{
    // Materialise a class into an instance, invoking the class as a
    // constructor so .args/.message etc. get properly initialised.
    if (py_is_class(exc)) {
        VALUE inst = py_apply(c, exc, 0, NULL);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
        exc = inst;
    }
    struct pygen *g = PY_PTR(gen_v)->gen;
    if (g->done) {
        c->state = PY_STATE_RAISE;
        c->state_value = exc;
        return PY_NONE;
    }
    g->throw_pending = true;
    g->throw_exc = exc;
    if (!g->started) {
        // Throw before first yield — body never gets to run; just raise.
        g->done = true;
        c->state = PY_STATE_RAISE;
        c->state_value = g->throw_exc;
        return PY_NONE;
    }
    return py_gen_next(c, gen_v);
}

VALUE
py_gen_close(CTX *c, VALUE gen_v)
{
    struct pygen *g = PY_PTR(gen_v)->gen;
    if (g->done) return PY_NONE;
    if (!g->started) { g->done = true; return PY_NONE; }
    g->throw_pending = true;
    g->throw_exc = py_make_instance(c->EXC_GeneratorExit);
    py_setattr(c, g->throw_exc, "message", py_make_str("GeneratorExit", 13));
    py_gen_next(c, gen_v);
    // After close, swallow StopIteration / RuntimeError (caller doesn't
    // want close() to raise).
    if (c->state == PY_STATE_RAISE) {
        c->state = PY_STATE_NORMAL;
        c->state_value = PY_NONE;
    }
    g->done = true;
    return PY_NONE;
}

// ---------------------------------------------------------------------------
// match / case pattern matching.
//
// Patterns:
//   PYPAT_LITERAL   value-equality vs the literal expression
//   PYPAT_CAPTURE   bind the value to a name (always succeeds)
//   PYPAT_WILDCARD  matches anything, no binding
//   PYPAT_OR        first matching child wins (bindings from that child)
//   PYPAT_SEQUENCE  list/tuple of matching length, element-wise
//   PYPAT_CLASS     isinstance check (no nested attribute matching here)
//   PYPAT_VALUE     dotted-name lookup at match time (e.g. enum)
// ---------------------------------------------------------------------------

bool
py_pat_match(CTX *c, int pat_idx, VALUE v)
{
    struct pypat *p = &PYSTRO_PATTERNS[pat_idx];
    switch (p->kind) {
      case PYPAT_WILDCARD:
        return true;
      case PYPAT_LITERAL: {
        VALUE lit = EVAL(c, p->literal);
        if (c->state != PY_STATE_NORMAL) return false;
        return py_eq(c, v, lit) == PY_TRUE;
      }
      case PYPAT_VALUE: {
        VALUE val = EVAL(c, p->literal);
        if (c->state != PY_STATE_NORMAL) return false;
        return v == val;     // identity (Python uses == here, close enough)
      }
      case PYPAT_CAPTURE:
        if (p->slot >= 0) c->env->slots[p->slot] = v;
        else              py_global_set(c, p->name, v);
        return true;
      case PYPAT_OR:
        for (int i = 0; i < p->nchildren; i++)
            if (py_pat_match(c, p->first_child + i, v)) return true;
        return false;
      case PYPAT_SEQUENCE: {
        if (!(py_is_list(v) || py_is_tuple(v))) return false;
        // Locate any PYPAT_STAR within the children.
        int star_idx = -1;
        for (int i = 0; i < p->nchildren; i++)
            if (PYSTRO_PATTERNS[p->first_child + i].kind == PYPAT_STAR) {
                if (star_idx >= 0) return false;  // only one star allowed
                star_idx = i;
            }
        size_t len = PY_PTR(v)->list.len;
        if (star_idx < 0) {
            if ((int)len != p->nchildren) return false;
            for (int i = 0; i < p->nchildren; i++)
                if (!py_pat_match(c, p->first_child + i, PY_PTR(v)->list.items[i]))
                    return false;
            return true;
        }
        // With star: prefix is star_idx items; suffix is (nchildren-1-star_idx) items.
        int prefix = star_idx;
        int suffix = p->nchildren - star_idx - 1;
        if ((int)len < prefix + suffix) return false;
        for (int i = 0; i < prefix; i++)
            if (!py_pat_match(c, p->first_child + i, PY_PTR(v)->list.items[i]))
                return false;
        for (int i = 0; i < suffix; i++)
            if (!py_pat_match(c, p->first_child + star_idx + 1 + i,
                              PY_PTR(v)->list.items[len - suffix + i]))
                return false;
        // Bind the star to the rest as a list.
        struct pypat *sp = &PYSTRO_PATTERNS[p->first_child + star_idx];
        size_t mid_len = len - prefix - suffix;
        VALUE rest = py_make_list(PY_PTR(v)->list.items + prefix, mid_len);
        if (sp->slot >= 0) c->env->slots[sp->slot] = rest;
        else if (sp->name) py_global_set(c, sp->name, rest);
        return true;
      }
      case PYPAT_STAR:
        // Standalone (not inside SEQUENCE) — treat as wildcard.
        return true;
      case PYPAT_CLASS: {
        VALUE cls = EVAL(c, p->literal);
        if (c->state != PY_STATE_NORMAL) return false;
        if (!py_is_class(cls)) return false;
        // Built-in type pattern (int, str, float, list, ...): match by type tag.
        extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
        VALUE av[1] = { v };
        VALUE actual_cls = bi_type(c, 1, av);
        if (c->state != PY_STATE_NORMAL) return false;
        if (actual_cls == cls) return true;
        if (py_is_class(actual_cls))
            return class_is_ancestor(actual_cls, cls);
        return false;
      }
      case PYPAT_CLASS_ARGS: {
        VALUE cls = EVAL(c, p->literal);
        if (c->state != PY_STATE_NORMAL) return false;
        if (!py_is_class(cls)) return false;
        if (!py_is_instance(v)) return false;
        if (!class_is_ancestor(PY_OBJ_VAL(PY_PTR(v)->inst.cls), cls)) return false;
        for (int i = 0; i < p->nchildren; i++) {
            // Read attribute via the dict (avoids dunder fall-throughs
            // which could fail).  If the attr is missing, no match.
            struct pyobj *o = PY_PTR(v);
            VALUE attr_val = (VALUE)0;
            if (o->inst.attrs) {
                VALUE k = py_make_str(p->attrs[i], strlen(p->attrs[i]));
                uint64_t h = py_hash(c, k);
                int32_t eidx = pydict_find(c, o->inst.attrs, k, h);
                if (eidx >= 0) attr_val = o->inst.attrs->entries[eidx].value;
            }
            if (!attr_val) return false;
            if (!py_pat_match(c, p->first_child + i, attr_val)) return false;
        }
        return true;
      }
      case PYPAT_MAPPING: {
        if (!py_is_dict(v)) return false;
        for (int i = 0; i < p->nchildren; i++) {
            VALUE key = EVAL(c, p->keys[i]);
            if (c->state != PY_STATE_NORMAL) return false;
            if (!py_dict_has(c, v, key)) return false;
            VALUE val = py_dict_get(c, v, key);
            if (c->state != PY_STATE_NORMAL) return false;
            if (!py_pat_match(c, p->first_child + i, val)) return false;
        }
        return true;
      }
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
    // Save CTX state that py_apply / others mutate inside the body.
    // longjmp from py_raise_exc skips their normal restore path, so
    // we must restore ourselves before running the handler.
    struct pyframe *saved_env = c->env;
    VALUE saved_mc = c->method_class;
    struct pyglobals *saved_g = c->globals;
    int saved_call_top = c->call_top;
    if (c->try_top < 64) c->try_stack[c->try_top++] = &jb;

    bool caught_raise = false;
    if (setjmp(jb) == 0) {
        EVAL(c, body);
        if (c->state == PY_STATE_RAISE) caught_raise = true;
    } else {
        // longjmp'd here from py_raise_exc; CTX may be in an
        // intermediate state — restore.
        c->env = saved_env;
        c->method_class = saved_mc;
        c->globals = saved_g;
        c->call_top = saved_call_top;
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
                // Keep `exc` in state_value so a bare `raise` inside
                // the handler can re-raise the active exception.
                c->state_value = exc;
                VALUE saved_handling = c->current_handling_exc;
                c->current_handling_exc = exc;
                if (h->name) {
                    if (h->name_is_global) py_global_set(c, h->name, exc);
                    else                   c->env->slots[h->name_slot] = exc;
                }
                EVAL(c, h->body);
                c->current_handling_exc = saved_handling;
                // If the body did not itself raise, clear the active
                // exception so it doesn't leak past the handler.
                if (c->state == PY_STATE_NORMAL) c->state_value = PY_NONE;
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

// `with EXPR as NAME: body` runner.  `cm` is the already-evaluated
// context manager (its __enter__ has already been called by the desugar).
// Runs `body` with proper exception protocol: on raise, calls
// `cm.__exit__(type, value, None)`; if it returns truthy the exception
// is suppressed.  On normal exit, calls `cm.__exit__(None, None, None)`.
void
py_run_with(CTX *c, VALUE cm, NODE *body)
{
    jmp_buf jb;
    int saved_top = c->try_top;
    struct pyframe *saved_env = c->env;
    VALUE saved_mc = c->method_class;
    struct pyglobals *saved_g = c->globals;
    int saved_call_top = c->call_top;
    if (c->try_top < 64) c->try_stack[c->try_top++] = &jb;

    bool caught = false;
    if (setjmp(jb) == 0) {
        EVAL(c, body);
        if (c->state == PY_STATE_RAISE) caught = true;
    } else {
        c->env = saved_env;
        c->method_class = saved_mc;
        c->globals = saved_g;
        c->call_top = saved_call_top;
        caught = true;
    }
    c->try_top = saved_top;

    if (caught) {
        VALUE exc = c->state_value;
        VALUE etype = py_is_instance(exc) ? PY_OBJ_VAL(PY_PTR(exc)->inst.cls) : PY_NONE;
        VALUE av[3] = { etype, exc, PY_NONE };
        c->state = PY_STATE_NORMAL;
        c->state_value = PY_NONE;
        VALUE exit_m = py_getattr(c, cm, "__exit__");
        if (c->state != PY_STATE_NORMAL) return;
        VALUE r = py_apply(c, exit_m, 3, av);
        if (c->state != PY_STATE_NORMAL) return;
        if (!py_is_truthy(r)) {
            // Re-raise the original exception.
            c->state = PY_STATE_RAISE;
            c->state_value = exc;
        }
    } else {
        VALUE av[3] = { PY_NONE, PY_NONE, PY_NONE };
        VALUE exit_m = py_getattr(c, cm, "__exit__");
        if (c->state != PY_STATE_NORMAL) return;
        py_apply(c, exit_m, 3, av);
    }
}

// ---------------------------------------------------------------------------
// Tuple-unpacking assignment (struct pyunpack_target in context.h).
// ---------------------------------------------------------------------------

void
py_unpack_assign(CTX *c, struct pyunpack_target *targets, uint32_t n, VALUE rhs)
{
    // Materialise rhs into an array.
    VALUE *items = NULL;
    size_t nitems = 0;
    if (py_is_list(rhs) || py_is_tuple(rhs)) {
        items = PY_PTR(rhs)->list.items;
        nitems = PY_PTR(rhs)->list.len;
    } else {
        // any iterable
        struct py_iter it; py_iter_init(c, &it, rhs);
        if (c->state != PY_STATE_NORMAL) return;
        size_t cap = 8; nitems = 0;
        items = (VALUE *)GC_malloc(sizeof(VALUE) * cap);
        VALUE x;
        while (py_iter_next(c, &it, &x)) {
            if (nitems == cap) { cap *= 2; items = (VALUE *)GC_realloc(items, sizeof(VALUE) * cap); }
            items[nitems++] = x;
        }
    }
    // Find a starred target if any (at most one allowed).
    int star_idx = -1;
    for (uint32_t i = 0; i < n; i++) if (targets[i].is_starred) { star_idx = (int)i; break; }
    if (star_idx < 0) {
        if (nitems != n)
            py_raise_exc(c, c->EXC_ValueError,
                         "expected %u values, got %zu", n, nitems);
        for (uint32_t i = 0; i < n; i++) {
            VALUE v = items[i];
            if (targets[i].is_local) c->env->slots[targets[i].slot] = v;
            else                     py_global_set(c, targets[i].global_name, v);
        }
        return;
    }
    // Starred form: prefix [0..star_idx), starred [star_idx..len-suffix],
    // suffix [len-suffix..len) where suffix length = n - star_idx - 1.
    int n_pre = star_idx;
    int n_suf = (int)n - star_idx - 1;
    if ((int)nitems < n_pre + n_suf)
        py_raise_exc(c, c->EXC_ValueError,
                     "not enough values to unpack (expected at least %d)", n_pre + n_suf);
    // Pre.
    for (int i = 0; i < n_pre; i++) {
        VALUE v = items[i];
        if (targets[i].is_local) c->env->slots[targets[i].slot] = v;
        else                     py_global_set(c, targets[i].global_name, v);
    }
    // Starred = list of middle items.
    int rest_len = (int)nitems - n_pre - n_suf;
    VALUE *rest_items = rest_len > 0 ? &items[n_pre] : NULL;
    VALUE rest = py_make_list(rest_items, (size_t)rest_len);
    if (targets[star_idx].is_local) c->env->slots[targets[star_idx].slot] = rest;
    else                            py_global_set(c, targets[star_idx].global_name, rest);
    // Suffix.
    for (int j = 0; j < n_suf; j++) {
        VALUE v = items[nitems - n_suf + j];
        struct pyunpack_target *t = &targets[star_idx + 1 + j];
        if (t->is_local) c->env->slots[t->slot] = v;
        else             py_global_set(c, t->global_name, v);
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
    int64_t maxsplit = (argc >= 3) ? py_int_to_long(c, argv[2]) : -1;
    size_t i = 0;
    int64_t splits = 0;
    while (i <= len) {
        if (maxsplit >= 0 && splits >= maxsplit) {
            py_list_append(c, result, py_make_str_borrow(s + i, len - i));
            break;
        }
        const char *p = i + slen <= len ? memmem(s + i, len - i, sep, slen) : NULL;
        if (!p) { py_list_append(c, result, py_make_str_borrow(s + i, len - i)); break; }
        py_list_append(c, result, py_make_str_borrow(s + i, (size_t)(p - (s + i))));
        i = (size_t)(p - s) + slen;
        splits++;
    }
    return result;
}

static VALUE
sm_join(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE self = argv[0];
    VALUE seq  = argv[1];
    const char *sep = PY_PTR(self)->str.chars;
    size_t slen = PY_PTR(self)->str.len;
    // Materialise iterable into a (possibly heap-allocated) list of strings.
    VALUE  fixed[64];
    VALUE *items = fixed;
    size_t cap = 64;
    int n = 0;
    if (py_is_list(seq) || py_is_tuple(seq)) {
        size_t sn = PY_PTR(seq)->list.len;
        if (sn > cap) {
            cap = sn;
            items = (VALUE *)GC_malloc(sizeof(VALUE) * cap);
        }
        for (size_t i = 0; i < sn; i++) items[n++] = PY_PTR(seq)->list.items[i];
    } else {
        struct py_iter it; py_iter_init(c, &it, seq);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
        VALUE x;
        while (py_iter_next(c, &it, &x)) {
            if ((size_t)n >= cap) {
                cap *= 2;
                if (items == fixed) {
                    items = (VALUE *)GC_malloc(sizeof(VALUE) * cap);
                    memcpy(items, fixed, sizeof(fixed));
                } else {
                    items = (VALUE *)GC_realloc(items, sizeof(VALUE) * cap);
                }
            }
            items[n++] = x;
        }
    }
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        VALUE e = items[i];
        if (!py_is_str(e)) py_raise_exc(c, c->EXC_TypeError, "join element must be str");
        total += PY_PTR(e)->str.len;
        if (i) total += slen;
    }
    char *buf = (char *)GC_malloc_atomic(total + 1);
    char *p = buf;
    for (int i = 0; i < n; i++) {
        if (i) { memcpy(p, sep, slen); p += slen; }
        VALUE e = items[i];
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
    (void)c;
    struct pyobj *o = PY_PTR(argv[0]);
    size_t i = 0, j = o->str.len;
    if (argc >= 2 && py_is_str(argv[1])) {
        // Strip any character in the argument.
        const char *cs = PY_PTR(argv[1])->str.chars;
        size_t cn = PY_PTR(argv[1])->str.len;
        bool in_set[256] = { false };
        for (size_t k = 0; k < cn; k++) in_set[(unsigned char)cs[k]] = true;
        while (i < j && in_set[(unsigned char)o->str.chars[i]]) i++;
        while (j > i && in_set[(unsigned char)o->str.chars[j-1]]) j--;
    } else {
        while (i < j && (o->str.chars[i] == ' ' || o->str.chars[i] == '\t' || o->str.chars[i] == '\n' || o->str.chars[i] == '\r')) i++;
        while (j > i && (o->str.chars[j-1] == ' ' || o->str.chars[j-1] == '\t' || o->str.chars[j-1] == '\n' || o->str.chars[j-1] == '\r')) j--;
    }
    return py_make_str(o->str.chars + i, j - i);
}

static VALUE
sm_startswith(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pyobj *s = PY_PTR(argv[0]);
    VALUE arg = argv[1];
    int64_t slen = (int64_t)s->str.len;
    int64_t start = 0, end = slen;
    if (argc >= 3 && argv[2] != PY_NONE) start = py_int_to_long(c, argv[2]);
    if (argc >= 4 && argv[3] != PY_NONE) end = py_int_to_long(c, argv[3]);
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    int64_t span = end - start;
    if (span < 0) span = 0;
    const char *base = s->str.chars + start;
    if (py_is_tuple(arg)) {
        size_t n = PY_PTR(arg)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE p = PY_PTR(arg)->list.items[i];
            if (!py_is_str(p)) continue;
            struct pyobj *pp = PY_PTR(p);
            if ((int64_t)pp->str.len > span) continue;
            if (memcmp(base, pp->str.chars, pp->str.len) == 0) return PY_TRUE;
        }
        return PY_FALSE;
    }
    if (!py_is_str(arg)) py_raise_exc(c, c->EXC_TypeError, "startswith: not str/tuple");
    struct pyobj *p = PY_PTR(arg);
    if ((int64_t)p->str.len > span) return PY_FALSE;
    return memcmp(base, p->str.chars, p->str.len) == 0 ? PY_TRUE : PY_FALSE;
}

static VALUE
sm_endswith(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pyobj *s = PY_PTR(argv[0]);
    VALUE arg = argv[1];
    int64_t slen = (int64_t)s->str.len;
    int64_t start = 0, end = slen;
    if (argc >= 3 && argv[2] != PY_NONE) start = py_int_to_long(c, argv[2]);
    if (argc >= 4 && argv[3] != PY_NONE) end = py_int_to_long(c, argv[3]);
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    int64_t span = end - start;
    if (span < 0) span = 0;
    const char *tail_end = s->str.chars + end;
    if (py_is_tuple(arg)) {
        size_t n = PY_PTR(arg)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE p = PY_PTR(arg)->list.items[i];
            if (!py_is_str(p)) continue;
            struct pyobj *pp = PY_PTR(p);
            if ((int64_t)pp->str.len > span) continue;
            if (memcmp(tail_end - pp->str.len, pp->str.chars, pp->str.len) == 0) return PY_TRUE;
        }
        return PY_FALSE;
    }
    if (!py_is_str(arg)) py_raise_exc(c, c->EXC_TypeError, "endswith: not str/tuple");
    struct pyobj *p = PY_PTR(arg);
    if ((int64_t)p->str.len > span) return PY_FALSE;
    return memcmp(tail_end - p->str.len, p->str.chars, p->str.len) == 0 ? PY_TRUE : PY_FALSE;
}

VALUE
sm_find(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "find: not str");
    struct pyobj *p = PY_PTR(argv[1]);
    int64_t start = (argc >= 3) ? py_int_to_long(c, argv[2]) : 0;
    int64_t end   = (argc >= 4) ? py_int_to_long(c, argv[3]) : (int64_t)s->str.len;
    if (start < 0) start += (int64_t)s->str.len;
    if (start < 0) start = 0;
    if (end < 0) end += (int64_t)s->str.len;
    if (end > (int64_t)s->str.len) end = (int64_t)s->str.len;
    if (start > end) return PY_FIX(-1);
    if (p->str.len == 0) return PY_FIX(start);
    if ((size_t)(end - start) < p->str.len) return PY_FIX(-1);
    void *r = memmem(s->str.chars + start, (size_t)(end - start),
                     p->str.chars, p->str.len);
    return r ? PY_FIX((int64_t)((char *)r - s->str.chars)) : PY_FIX(-1);
}

static VALUE
sm_replace(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_str(argv[1]) || !py_is_str(argv[2]))
        py_raise_exc(c, c->EXC_TypeError, "replace args must be str");
    struct pyobj *o = PY_PTR(argv[1]);
    struct pyobj *n = PY_PTR(argv[2]);
    int64_t max_count = (argc >= 4) ? py_int_to_long(c, argv[3]) : -1;
    if (o->str.len == 0) return argv[0];
    size_t cap = s->str.len + 16, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap + 1);
    size_t i = 0;
    int64_t cnt = 0;
    while (i < s->str.len) {
        if ((max_count < 0 || cnt < max_count) &&
            i + o->str.len <= s->str.len &&
            memcmp(s->str.chars + i, o->str.chars, o->str.len) == 0) {
            if (len + n->str.len + 1 > cap) {
                while (len + n->str.len + 1 > cap) cap *= 2;
                buf = (char *)GC_realloc(buf, cap + 1);
            }
            memcpy(buf + len, n->str.chars, n->str.len);
            len += n->str.len;
            i += o->str.len;
            cnt++;
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
    (void)c;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "count: not str");
    struct pyobj *p = PY_PTR(argv[1]);
    int64_t slen = (int64_t)s->str.len;
    int64_t start = 0, end = slen;
    if (argc >= 3 && argv[2] != PY_NONE) start = py_int_to_long(c, argv[2]);
    if (argc >= 4 && argv[3] != PY_NONE) end = py_int_to_long(c, argv[3]);
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    if (p->str.len == 0) return PY_FIX(end > start ? end - start + 1 : 0);
    int64_t n = 0;
    int64_t i = start;
    while (i + (int64_t)p->str.len <= end) {
        if (memcmp(s->str.chars + i, p->str.chars, p->str.len) == 0) { n++; i += (int64_t)p->str.len; }
        else i++;
    }
    return PY_FIX(n);
}

static VALUE
sm_encode(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    return py_make_bytes(o->str.chars, o->str.len);
}

static VALUE bi_format(CTX *c, int argc, VALUE *argv);  // forward
// `"{} {}".format(a, b)` — auto / positional indices + format spec.
// Kwargs (`{name}`) are not supported in this v0; use f-strings instead.
static VALUE
sm_format(CTX *c, int argc, VALUE *argv)
{
    VALUE self = argv[0];
    const char *src = PY_PTR(self)->str.chars;
    size_t srclen = PY_PTR(self)->str.len;
    int auto_idx = 0;
    size_t out_capa = srclen + 16;
    char *out = (char *)GC_malloc_atomic(out_capa);
    size_t out_len = 0;
#define OUT_CH(ch) do { \
    if (out_len + 1 > out_capa) { \
        out_capa *= 2; \
        char *no = (char *)GC_malloc_atomic(out_capa); \
        memcpy(no, out, out_len); out = no; \
    } \
    out[out_len++] = (ch); \
} while (0)
#define OUT_PUT(buf, len) do { \
    while (out_len + (len) > out_capa) { \
        out_capa *= 2; \
        char *no = (char *)GC_malloc_atomic(out_capa); \
        memcpy(no, out, out_len); out = no; \
    } \
    memcpy(out + out_len, (buf), (len)); out_len += (len); \
} while (0)
    for (size_t i = 0; i < srclen; i++) {
        char ch = src[i];
        if (ch == '{') {
            if (i + 1 < srclen && src[i+1] == '{') { OUT_CH('{'); i++; continue; }
            // Find matching `}`, allowing nested in spec.
            size_t j = i + 1;
            int depth = 1;
            while (j < srclen && depth > 0) {
                if (src[j] == '{') depth++;
                else if (src[j] == '}') depth--;
                if (depth > 0) j++;
            }
            if (j >= srclen) py_raise_exc(c, c->EXC_ValueError, "unterminated '{' in format");
            // Inside [i+1..j) is `[N][!conv][:spec]`.
            const char *body = src + i + 1;
            size_t bn = j - i - 1;
            // Field name: positional index, auto (empty), or named kw.
            int idx = -1;
            size_t k = 0;
            VALUE val;
            if (bn == 0 || body[0] == ':' || body[0] == '!') {
                // Empty field name → auto-numbered.
                idx = auto_idx++;
            } else if (body[0] >= '0' && body[0] <= '9') {
                idx = 0;
                while (k < bn && body[k] >= '0' && body[k] <= '9') {
                    idx = idx * 10 + (body[k] - '0'); k++;
                }
            } else {
                // Named field → look up in kwargs.  Name ends at ':', '!', '.', '['.
                size_t nm_start = k;
                while (k < bn && body[k] != ':' && body[k] != '!'
                       && body[k] != '.' && body[k] != '[') k++;
                size_t nm_len = k - nm_start;
                extern int    PYSTRO_BI_KWC;
                extern const char **PYSTRO_BI_KWNAMES;
                extern VALUE *PYSTRO_BI_KWVALUES;
                int found = -1;
                for (int ki = 0; ki < PYSTRO_BI_KWC; ki++) {
                    const char *nm = PYSTRO_BI_KWNAMES[ki];
                    if (strlen(nm) == nm_len && memcmp(nm, body + nm_start, nm_len) == 0) {
                        found = ki; break;
                    }
                }
                if (found < 0) py_raise_exc(c, c->EXC_KeyError, "format: missing kwarg");
                val = PYSTRO_BI_KWVALUES[found];
                goto have_val;
            }
            if (idx < 0 || idx + 1 > argc - 1)
                py_raise_exc(c, c->EXC_IndexError, "format: index out of range");
            val = argv[1 + idx];
          have_val:;
            // Trailers: .attr or [idx] chains.
            while (k < bn && (body[k] == '.' || body[k] == '[')) {
                if (body[k] == '.') {
                    k++;
                    size_t s = k;
                    while (k < bn && body[k] != '.' && body[k] != '['
                           && body[k] != ':' && body[k] != '!') k++;
                    char nm[128];
                    size_t nl = k - s;
                    if (nl >= sizeof(nm)) nl = sizeof(nm) - 1;
                    memcpy(nm, body + s, nl); nm[nl] = '\0';
                    val = py_getattr(c, val, nm);
                    if (c->state == PY_STATE_RAISE) return PY_NONE;
                } else {
                    k++;  // [
                    size_t s = k;
                    while (k < bn && body[k] != ']') k++;
                    if (k >= bn) py_raise_exc(c, c->EXC_ValueError, "unmatched '[' in format");
                    // Try integer first, else string key.
                    bool is_int = true;
                    int64_t iv = 0;
                    for (size_t p = s; p < k; p++) {
                        if (body[p] < '0' || body[p] > '9') { is_int = false; break; }
                        iv = iv * 10 + (body[p] - '0');
                    }
                    if (is_int && k > s) {
                        val = py_list_get(c, val, PY_FIX(iv));
                    } else {
                        VALUE key = py_make_str(body + s, k - s);
                        val = py_list_get(c, val, key);
                    }
                    if (c->state == PY_STATE_RAISE) return PY_NONE;
                    k++;  // ]
                }
            }
            // Optional `!conv`.
            if (k < bn && body[k] == '!') {
                k++;
                if (k >= bn) py_raise_exc(c, c->EXC_ValueError, "expected conversion");
                char conv = body[k++];
                if (conv == 'r' || conv == 'a') val = py_to_repr(c, val);
                else if (conv == 's')           val = py_to_str(c, val);
                else py_raise_exc(c, c->EXC_ValueError, "bad conversion");
            }
            // Optional `:spec`.
            VALUE spec_val = py_make_str("", 0);
            if (k < bn && body[k] == ':') {
                k++;
                spec_val = py_make_str(body + k, bn - k);
            }
            // Apply.
            VALUE av[2] = { val, spec_val };
            VALUE rendered = bi_format(c, 2, av);
            if (!py_is_str(rendered)) rendered = py_to_str(c, rendered);
            OUT_PUT(PY_PTR(rendered)->str.chars, PY_PTR(rendered)->str.len);
            i = j;
        } else if (ch == '}') {
            if (i + 1 < srclen && src[i+1] == '}') { OUT_CH('}'); i++; continue; }
            py_raise_exc(c, c->EXC_ValueError, "single '}' in format");
        } else {
            OUT_CH(ch);
        }
    }
    return py_make_str(out, out_len);
#undef OUT_CH
#undef OUT_PUT
}

static VALUE
sm_lstrip(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pyobj *o = PY_PTR(argv[0]);
    size_t i = 0, j = o->str.len;
    if (argc >= 2 && py_is_str(argv[1])) {
        const char *cs = PY_PTR(argv[1])->str.chars;
        size_t cn = PY_PTR(argv[1])->str.len;
        bool in_set[256] = { false };
        for (size_t k = 0; k < cn; k++) in_set[(unsigned char)cs[k]] = true;
        while (i < j && in_set[(unsigned char)o->str.chars[i]]) i++;
    } else {
        while (i < j && (o->str.chars[i] == ' ' || o->str.chars[i] == '\t' ||
                         o->str.chars[i] == '\n' || o->str.chars[i] == '\r')) i++;
    }
    return py_make_str(o->str.chars + i, j - i);
}

static VALUE
sm_rstrip(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pyobj *o = PY_PTR(argv[0]);
    size_t i = 0, j = o->str.len;
    if (argc >= 2 && py_is_str(argv[1])) {
        const char *cs = PY_PTR(argv[1])->str.chars;
        size_t cn = PY_PTR(argv[1])->str.len;
        bool in_set[256] = { false };
        for (size_t k = 0; k < cn; k++) in_set[(unsigned char)cs[k]] = true;
        while (j > i && in_set[(unsigned char)o->str.chars[j-1]]) j--;
    } else {
        while (j > i && (o->str.chars[j-1] == ' ' || o->str.chars[j-1] == '\t' ||
                         o->str.chars[j-1] == '\n' || o->str.chars[j-1] == '\r')) j--;
    }
    return py_make_str(o->str.chars + i, j - i);
}

static VALUE
sm_zfill(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    int w = (int)py_int_to_long(c, argv[1]);
    if ((int)o->str.len >= w) return py_make_str(o->str.chars, o->str.len);
    int pad = w - (int)o->str.len;
    char *buf = (char *)GC_malloc_atomic(w + 1);
    int off = 0;
    if (o->str.len > 0 && (o->str.chars[0] == '+' || o->str.chars[0] == '-')) {
        buf[0] = o->str.chars[0]; off = 1;
        for (int i = 0; i < pad; i++) buf[off + i] = '0';
        memcpy(buf + off + pad, o->str.chars + 1, o->str.len - 1);
    } else {
        for (int i = 0; i < pad; i++) buf[i] = '0';
        memcpy(buf + pad, o->str.chars, o->str.len);
    }
    buf[w] = '\0';
    return py_make_str_take(buf, w);
}

static VALUE
sm_center(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    int w = (int)py_int_to_long(c, argv[1]);
    char fill = (argc >= 3 && py_is_str(argv[2]) && PY_PTR(argv[2])->str.len >= 1) ? PY_PTR(argv[2])->str.chars[0] : ' ';
    if ((int)o->str.len >= w) return py_make_str(o->str.chars, o->str.len);
    int pad = w - (int)o->str.len;
    int left = pad / 2, right = pad - left;
    char *buf = (char *)GC_malloc_atomic(w + 1);
    for (int i = 0; i < left; i++) buf[i] = fill;
    memcpy(buf + left, o->str.chars, o->str.len);
    for (int i = 0; i < right; i++) buf[left + (int)o->str.len + i] = fill;
    buf[w] = '\0';
    return py_make_str_take(buf, w);
}

static VALUE
sm_ljust(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    int w = (int)py_int_to_long(c, argv[1]);
    char fill = (argc >= 3 && py_is_str(argv[2]) && PY_PTR(argv[2])->str.len >= 1) ? PY_PTR(argv[2])->str.chars[0] : ' ';
    if ((int)o->str.len >= w) return py_make_str(o->str.chars, o->str.len);
    char *buf = (char *)GC_malloc_atomic(w + 1);
    memcpy(buf, o->str.chars, o->str.len);
    for (size_t i = o->str.len; i < (size_t)w; i++) buf[i] = fill;
    buf[w] = '\0';
    return py_make_str_take(buf, w);
}

static VALUE
sm_rjust(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    int w = (int)py_int_to_long(c, argv[1]);
    char fill = (argc >= 3 && py_is_str(argv[2]) && PY_PTR(argv[2])->str.len >= 1) ? PY_PTR(argv[2])->str.chars[0] : ' ';
    if ((int)o->str.len >= w) return py_make_str(o->str.chars, o->str.len);
    int pad = w - (int)o->str.len;
    char *buf = (char *)GC_malloc_atomic(w + 1);
    for (int i = 0; i < pad; i++) buf[i] = fill;
    memcpy(buf + pad, o->str.chars, o->str.len);
    buf[w] = '\0';
    return py_make_str_take(buf, w);
}

static VALUE
sm_title(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    bool prev_alpha = false;
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        bool is_alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (is_alpha && !prev_alpha) {
            if (ch >= 'a' && ch <= 'z') ch -= 32;
        } else if (is_alpha) {
            if (ch >= 'A' && ch <= 'Z') ch += 32;
        }
        buf[i] = ch;
        prev_alpha = is_alpha;
    }
    buf[o->str.len] = '\0';
    return py_make_str_take(buf, o->str.len);
}

static VALUE
sm_capitalize(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->str.len == 0) return py_make_str("", 0);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        if (i == 0) {
            if (ch >= 'a' && ch <= 'z') ch -= 32;
        } else {
            if (ch >= 'A' && ch <= 'Z') ch += 32;
        }
        buf[i] = ch;
    }
    buf[o->str.len] = '\0';
    return py_make_str_take(buf, o->str.len);
}

static VALUE
sm_rfind(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pyobj *o = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) return PY_FIX(-1);
    struct pyobj *needle = PY_PTR(argv[1]);
    int64_t slen = (int64_t)o->str.len;
    int64_t start = 0, end = slen;
    if (argc >= 3 && argv[2] != PY_NONE) start = py_int_to_long(c, argv[2]);
    if (argc >= 4 && argv[3] != PY_NONE) end = py_int_to_long(c, argv[3]);
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    if (needle->str.len == 0) return PY_FIX(end);
    if ((int64_t)needle->str.len > end - start) return PY_FIX(-1);
    for (int64_t i = end - (int64_t)needle->str.len; i >= start; i--) {
        if (memcmp(o->str.chars + i, needle->str.chars, needle->str.len) == 0)
            return PY_FIX(i);
    }
    return PY_FIX(-1);
}

static VALUE
sm_rindex(CTX *c, int argc, VALUE *argv)
{
    VALUE r = sm_rfind(c, argc, argv);
    if (PY_IS_FIXNUM(r) && PY_FIXVAL(r) == -1)
        py_raise_exc(c, c->EXC_ValueError, "substring not found");
    return r;
}

static VALUE
sm_index(CTX *c, int argc, VALUE *argv)
{
    extern VALUE sm_find(CTX *c, int argc, VALUE *argv);
    VALUE r = sm_find(c, argc, argv);
    if (PY_IS_FIXNUM(r) && PY_FIXVAL(r) == -1)
        py_raise_exc(c, c->EXC_ValueError, "substring not found");
    return r;
}

static VALUE
sm_isnumeric(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->str.len == 0) return PY_FALSE;
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        if (!(ch >= '0' && ch <= '9')) return PY_FALSE;
    }
    return PY_TRUE;
}

static VALUE
sm_isascii(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    for (size_t i = 0; i < o->str.len; i++)
        if ((unsigned char)o->str.chars[i] > 127) return PY_FALSE;
    return PY_TRUE;
}

static VALUE
sm_isidentifier(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->str.len == 0) return PY_FALSE;
    char ch = o->str.chars[0];
    if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
        return PY_FALSE;
    for (size_t i = 1; i < o->str.len; i++) {
        ch = o->str.chars[i];
        if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9')))
            return PY_FALSE;
    }
    return PY_TRUE;
}

static VALUE
sm_isprintable(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    for (size_t i = 0; i < o->str.len; i++) {
        unsigned char ch = (unsigned char)o->str.chars[i];
        if (ch < 32 || ch == 127) return PY_FALSE;
    }
    return PY_TRUE;
}

static VALUE
sm_istitle(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    bool seen_alpha = false;
    bool prev_alpha = false;
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        bool is_upper = (ch >= 'A' && ch <= 'Z');
        bool is_lower = (ch >= 'a' && ch <= 'z');
        bool is_alpha = is_upper || is_lower;
        if (is_alpha) seen_alpha = true;
        if (is_upper && prev_alpha) return PY_FALSE;
        if (is_lower && !prev_alpha) return PY_FALSE;
        prev_alpha = is_alpha;
    }
    return seen_alpha ? PY_TRUE : PY_FALSE;
}

static VALUE
sm_swapcase(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        else if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[o->str.len] = '\0';
    return py_make_str_take(buf, o->str.len);
}

static VALUE
sm_casefold(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[o->str.len] = '\0';
    return py_make_str_take(buf, o->str.len);
}

static VALUE
sm_splitlines(CTX *c, int argc, VALUE *argv)
{
    bool keepends = (argc >= 2) && py_is_truthy(argv[1]);
    struct pyobj *o = PY_PTR(argv[0]);
    VALUE r = py_make_list(NULL, 0);
    size_t i = 0;
    while (i < o->str.len) {
        size_t j = i;
        while (j < o->str.len && o->str.chars[j] != '\n' && o->str.chars[j] != '\r') j++;
        size_t end = j;
        if (j < o->str.len && o->str.chars[j] == '\r' && j + 1 < o->str.len && o->str.chars[j+1] == '\n') j += 2;
        else if (j < o->str.len) j++;
        size_t out_end = keepends ? j : end;
        py_list_append(c, r, py_make_str(o->str.chars + i, out_end - i));
        i = j;
    }
    return r;
}

static VALUE
sm_removeprefix(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) return py_make_str(o->str.chars, o->str.len);
    struct pyobj *p = PY_PTR(argv[1]);
    if (o->str.len >= p->str.len && memcmp(o->str.chars, p->str.chars, p->str.len) == 0)
        return py_make_str(o->str.chars + p->str.len, o->str.len - p->str.len);
    return py_make_str(o->str.chars, o->str.len);
}

static VALUE
sm_removesuffix(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) return py_make_str(o->str.chars, o->str.len);
    struct pyobj *p = PY_PTR(argv[1]);
    if (o->str.len >= p->str.len &&
        memcmp(o->str.chars + o->str.len - p->str.len, p->str.chars, p->str.len) == 0)
        return py_make_str(o->str.chars, o->str.len - p->str.len);
    return py_make_str(o->str.chars, o->str.len);
}

#define _STR_PRED(name, expr) \
    static VALUE sm_##name(CTX *c, int argc, VALUE *argv) { \
        (void)c; (void)argc; \
        struct pyobj *o = PY_PTR(argv[0]); \
        if (o->str.len == 0) return PY_FALSE; \
        for (size_t i = 0; i < o->str.len; i++) { \
            unsigned char ch = (unsigned char)o->str.chars[i]; \
            if (!(expr)) return PY_FALSE; \
        } \
        return PY_TRUE; \
    }
_STR_PRED(isdigit, ch >= '0' && ch <= '9')
_STR_PRED(isalpha, (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
_STR_PRED(isspace, ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f')
_STR_PRED(isupper, ch >= 'A' && ch <= 'Z')
_STR_PRED(islower, ch >= 'a' && ch <= 'z')
_STR_PRED(isalnum, (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
#undef _STR_PRED

static VALUE
sm_partition(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "partition needs str");
    struct pyobj *sep = PY_PTR(argv[1]);
    if (sep->str.len == 0) py_raise_exc(c, c->EXC_ValueError, "empty separator");
    const char *p = (const char *)memmem(o->str.chars, o->str.len, sep->str.chars, sep->str.len);
    if (!p) {
        VALUE items[3] = {
            py_make_str(o->str.chars, o->str.len),
            py_make_str("", 0),
            py_make_str("", 0),
        };
        return py_make_tuple(items, 3);
    }
    size_t hl = (size_t)(p - o->str.chars);
    VALUE items[3] = {
        py_make_str(o->str.chars, hl),
        py_make_str(sep->str.chars, sep->str.len),
        py_make_str(p + sep->str.len, o->str.len - hl - sep->str.len),
    };
    return py_make_tuple(items, 3);
}

static VALUE
sm_rpartition(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "rpartition needs str");
    struct pyobj *sep = PY_PTR(argv[1]);
    if (sep->str.len == 0) py_raise_exc(c, c->EXC_ValueError, "empty separator");
    if (sep->str.len > o->str.len) {
        VALUE items[3] = {
            py_make_str("", 0), py_make_str("", 0),
            py_make_str(o->str.chars, o->str.len),
        };
        return py_make_tuple(items, 3);
    }
    ssize_t hit = -1;
    for (ssize_t i = (ssize_t)(o->str.len - sep->str.len); i >= 0; i--) {
        if (memcmp(o->str.chars + i, sep->str.chars, sep->str.len) == 0) {
            hit = i; break;
        }
    }
    if (hit < 0) {
        VALUE items[3] = {
            py_make_str("", 0), py_make_str("", 0),
            py_make_str(o->str.chars, o->str.len),
        };
        return py_make_tuple(items, 3);
    }
    VALUE items[3] = {
        py_make_str(o->str.chars, (size_t)hit),
        py_make_str(sep->str.chars, sep->str.len),
        py_make_str(o->str.chars + hit + sep->str.len,
                    o->str.len - hit - sep->str.len),
    };
    return py_make_tuple(items, 3);
}

static VALUE
sm_rsplit(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    int maxsplit = (argc >= 3) ? (int)py_int_to_long(c, argv[2]) : -1;
    VALUE r = py_make_list(NULL, 0);
    if (argc < 2 || argv[1] == PY_NONE) {
        // Whitespace split, from right.
        ssize_t i = (ssize_t)o->str.len;
        int splits = 0;
        VALUE pieces[256];
        int np = 0;
        while (i > 0) {
            if (maxsplit >= 0 && splits >= maxsplit) break;
            // skip trailing whitespace
            while (i > 0 && (o->str.chars[i-1] == ' ' || o->str.chars[i-1] == '\t' ||
                             o->str.chars[i-1] == '\n' || o->str.chars[i-1] == '\r'))
                i--;
            if (i == 0) break;
            ssize_t end = i;
            while (i > 0 && !(o->str.chars[i-1] == ' ' || o->str.chars[i-1] == '\t' ||
                              o->str.chars[i-1] == '\n' || o->str.chars[i-1] == '\r'))
                i--;
            if (np >= 256) py_raise_exc(c, c->EXC_RuntimeError, "rsplit too many parts");
            pieces[np++] = py_make_str(o->str.chars + i, (size_t)(end - i));
            splits++;
        }
        if (i > 0 && maxsplit >= 0 && splits >= maxsplit) {
            // remainder
            // strip trailing ws from index i? Actually keep it.
            pieces[np++] = py_make_str(o->str.chars, (size_t)i);
        }
        // pieces are right-to-left; reverse.
        for (int k = np - 1; k >= 0; k--) py_list_append(c, r, pieces[k]);
        return r;
    }
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "rsplit sep must be str");
    struct pyobj *sep = PY_PTR(argv[1]);
    if (sep->str.len == 0) py_raise_exc(c, c->EXC_ValueError, "empty separator");
    VALUE pieces[256]; int np = 0;
    ssize_t i = (ssize_t)o->str.len;
    int splits = 0;
    while (i >= (ssize_t)sep->str.len) {
        if (maxsplit >= 0 && splits >= maxsplit) break;
        ssize_t hit = -1;
        for (ssize_t j = i - (ssize_t)sep->str.len; j >= 0; j--) {
            if (memcmp(o->str.chars + j, sep->str.chars, sep->str.len) == 0) {
                hit = j; break;
            }
        }
        if (hit < 0) break;
        if (np >= 256) py_raise_exc(c, c->EXC_RuntimeError, "rsplit too many");
        pieces[np++] = py_make_str(o->str.chars + hit + sep->str.len,
                                    (size_t)(i - hit - sep->str.len));
        i = hit;
        splits++;
    }
    if (np >= 256) py_raise_exc(c, c->EXC_RuntimeError, "rsplit too many");
    pieces[np++] = py_make_str(o->str.chars, (size_t)i);
    for (int k = np - 1; k >= 0; k--) py_list_append(c, r, pieces[k]);
    return r;
}

static VALUE
sm_format_map(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    // Reuse sm_format by injecting a dict's items as kwargs is hard;
    // simpler: implement directly using subscript on the mapping.
    extern VALUE bi_format(CTX *c, int argc, VALUE *argv);
    struct pyobj *o = PY_PTR(argv[0]);
    VALUE map_v = argv[1];
    const char *src = o->str.chars;
    size_t srclen = o->str.len;
    int auto_idx = 0;
    size_t out_capa = srclen + 16;
    char *out = (char *)GC_malloc_atomic(out_capa);
    size_t out_len = 0;
    for (size_t i = 0; i < srclen; i++) {
        char ch = src[i];
        if (ch == '{') {
            if (i + 1 < srclen && src[i+1] == '{') {
                if (out_len + 1 > out_capa) { out_capa *= 2; char *no = GC_malloc_atomic(out_capa); memcpy(no, out, out_len); out = no; }
                out[out_len++] = '{'; i++; continue;
            }
            size_t j = i + 1;
            int depth = 1;
            while (j < srclen && depth > 0) {
                if (src[j] == '{') depth++;
                else if (src[j] == '}') depth--;
                if (depth > 0) j++;
            }
            if (j >= srclen) py_raise_exc(c, c->EXC_ValueError, "unterminated '{'");
            const char *body = src + i + 1;
            size_t bn = j - i - 1;
            size_t k = 0;
            VALUE val;
            if (bn == 0 || body[0] == ':' || body[0] == '!') {
                // auto: use auto_idx
                py_raise_exc(c, c->EXC_KeyError, "format_map: positional fields not supported");
                (void)auto_idx;
            }
            if (body[0] >= '0' && body[0] <= '9') {
                py_raise_exc(c, c->EXC_KeyError, "format_map: positional fields not supported");
            }
            // Named field: lookup in map_v.
            size_t nm_start = 0;
            while (k < bn && body[k] != ':' && body[k] != '!') k++;
            VALUE key = py_make_str(body + nm_start, k);
            val = py_dict_get(c, map_v, key);
            // Optional !conv.
            if (k < bn && body[k] == '!') {
                k++;
                if (k >= bn) py_raise_exc(c, c->EXC_ValueError, "expected conversion");
                char conv = body[k++];
                if (conv == 'r' || conv == 'a') val = py_to_repr(c, val);
                else if (conv == 's')           val = py_to_str(c, val);
            }
            VALUE spec_val = py_make_str("", 0);
            if (k < bn && body[k] == ':') {
                k++; spec_val = py_make_str(body + k, bn - k);
            }
            VALUE av[2] = { val, spec_val };
            VALUE rendered = bi_format(c, 2, av);
            if (!py_is_str(rendered)) rendered = py_to_str(c, rendered);
            size_t rl = PY_PTR(rendered)->str.len;
            while (out_len + rl > out_capa) { out_capa *= 2; char *no = GC_malloc_atomic(out_capa); memcpy(no, out, out_len); out = no; }
            memcpy(out + out_len, PY_PTR(rendered)->str.chars, rl);
            out_len += rl;
            i = j;
        } else if (ch == '}') {
            if (i + 1 < srclen && src[i+1] == '}') {
                if (out_len + 1 > out_capa) { out_capa *= 2; char *no = GC_malloc_atomic(out_capa); memcpy(no, out, out_len); out = no; }
                out[out_len++] = '}'; i++; continue;
            }
            py_raise_exc(c, c->EXC_ValueError, "single '}' in format");
        } else {
            if (out_len + 1 > out_capa) { out_capa *= 2; char *no = GC_malloc_atomic(out_capa); memcpy(no, out, out_len); out = no; }
            out[out_len++] = ch;
        }
    }
    return py_make_str(out, out_len);
}

// str.maketrans — classmethod-like (called via str.maketrans(...)).
// Forms: maketrans(dict) or maketrans(from, to[, drop]).
static VALUE
bi_str_maketrans(CTX *c, int argc, VALUE *argv)
{
    VALUE r = py_make_dict();
    if (argc == 1 && py_is_dict(argv[0])) {
        struct pydict *d = PY_PTR(argv[0])->dict;
        for (size_t i = 0; i < d->elen; i++) {
            if (!pydict_entry_live(d, i)) continue;
            VALUE k = d->entries[i].key;
            VALUE v = d->entries[i].value;
            if (py_is_str(k) && PY_PTR(k)->str.len == 1) {
                VALUE intk = PY_FIX((unsigned char)PY_PTR(k)->str.chars[0]);
                py_dict_set(c, r, intk, v);
            } else if (PY_IS_FIXNUM(k)) {
                py_dict_set(c, r, k, v);
            }
        }
        return r;
    }
    if (argc >= 2 && py_is_str(argv[0]) && py_is_str(argv[1])) {
        struct pyobj *a = PY_PTR(argv[0]);
        struct pyobj *b = PY_PTR(argv[1]);
        if (a->str.len != b->str.len)
            py_raise_exc(c, c->EXC_ValueError, "maketrans: from and to differ in length");
        for (size_t i = 0; i < a->str.len; i++) {
            VALUE k = PY_FIX((unsigned char)a->str.chars[i]);
            VALUE v = PY_FIX((unsigned char)b->str.chars[i]);
            py_dict_set(c, r, k, v);
        }
        if (argc >= 3 && py_is_str(argv[2])) {
            struct pyobj *d = PY_PTR(argv[2]);
            for (size_t i = 0; i < d->str.len; i++) {
                VALUE k = PY_FIX((unsigned char)d->str.chars[i]);
                py_dict_set(c, r, k, PY_NONE);
            }
        }
        return r;
    }
    py_raise_exc(c, c->EXC_TypeError, "maketrans: bad args");
}

static VALUE
sm_translate(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (!py_is_dict(argv[1])) py_raise_exc(c, c->EXC_TypeError, "translate: dict required");
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    size_t len = 0;
    for (size_t i = 0; i < o->str.len; i++) {
        unsigned char ch = (unsigned char)o->str.chars[i];
        VALUE k = PY_FIX(ch);
        VALUE v = py_dict_has(c, argv[1], k) ? py_dict_get(c, argv[1], k) : k;
        if (v == PY_NONE) continue;
        if (PY_IS_FIXNUM(v)) {
            int64_t cv = PY_FIXVAL(v);
            if (cv < 0 || cv > 255) py_raise_exc(c, c->EXC_ValueError, "translate: out of range");
            buf[len++] = (char)cv;
        } else if (py_is_str(v)) {
            struct pyobj *sv = PY_PTR(v);
            if (len + sv->str.len + 1 > o->str.len + 1) {
                buf = (char *)GC_realloc(buf, len + sv->str.len + 16);
            }
            memcpy(buf + len, sv->str.chars, sv->str.len);
            len += sv->str.len;
        }
    }
    return py_make_str(buf, len);
}

static VALUE
sm_expandtabs(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    int w = (argc >= 2) ? (int)py_int_to_long(c, argv[1]) : 8;
    size_t cap = o->str.len * 2 + 16, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    int col = 0;
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        if (ch == '\t') {
            // expandtabs(0) → drop tabs (CPython behaviour: every tab
            // expands to zero spaces, advancing nothing).
            int n = (w == 0) ? 0 : w - (col % w);
            if (len + n + 1 > cap) { cap = (len + n + 1) * 2; char *nb = (char *)GC_malloc_atomic(cap); memcpy(nb, buf, len); buf = nb; }
            for (int k = 0; k < n; k++) buf[len++] = ' ';
            col += n;
        } else {
            if (len + 1 > cap) { cap *= 2; char *nb = (char *)GC_malloc_atomic(cap); memcpy(nb, buf, len); buf = nb; }
            buf[len++] = ch;
            if (ch == '\n') col = 0; else col++;
        }
    }
    return py_make_str(buf, len);
}

static struct type_method str_methods[] = {
    { "split",         sm_split,         1, 3 },
    { "join",          sm_join,          2, 2 },
    { "upper",         sm_upper,         1, 1 },
    { "lower",         sm_lower,         1, 1 },
    { "strip",         sm_strip,         1, 2 },
    { "lstrip",        sm_lstrip,        1, 2 },
    { "rstrip",        sm_rstrip,        1, 2 },
    { "startswith",    sm_startswith,    2, 4 },
    { "endswith",      sm_endswith,      2, 4 },
    { "find",          sm_find,          2, 4 },
    { "replace",       sm_replace,       3, 4 },
    { "count",         sm_count,         2, 4 },
    { "encode",        sm_encode,        1, 2 },
    { "format",        sm_format,        1, -1 },
    { "zfill",         sm_zfill,         2, 2 },
    { "center",        sm_center,        2, 3 },
    { "ljust",         sm_ljust,         2, 3 },
    { "rjust",         sm_rjust,         2, 3 },
    { "title",         sm_title,         1, 1 },
    { "capitalize",    sm_capitalize,    1, 1 },
    { "swapcase",      sm_swapcase,      1, 1 },
    { "casefold",      sm_casefold,      1, 1 },
    { "rfind",         sm_rfind,         2, 4 },
    { "rindex",        sm_rindex,        2, 4 },
    { "index",         sm_index,         2, 4 },
    { "isnumeric",     sm_isnumeric,     1, 1 },
    { "isdecimal",     sm_isnumeric,     1, 1 },
    { "isascii",       sm_isascii,       1, 1 },
    { "isidentifier",  sm_isidentifier,  1, 1 },
    { "isprintable",   sm_isprintable,   1, 1 },
    { "istitle",       sm_istitle,       1, 1 },
    { "splitlines",    sm_splitlines,    1, 2 },
    { "removeprefix",  sm_removeprefix,  2, 2 },
    { "removesuffix",  sm_removesuffix,  2, 2 },
    { "isdigit",       sm_isdigit,       1, 1 },
    { "isalpha",       sm_isalpha,       1, 1 },
    { "isspace",       sm_isspace,       1, 1 },
    { "isupper",       sm_isupper,       1, 1 },
    { "islower",       sm_islower,       1, 1 },
    { "isalnum",       sm_isalnum,       1, 1 },
    { "partition",     sm_partition,     2, 2 },
    { "rpartition",    sm_rpartition,    2, 2 },
    { "rsplit",        sm_rsplit,        1, 3 },
    { "format_map",    sm_format_map,    2, 2 },
    { "translate",     sm_translate,     2, 2 },
    { "expandtabs",    sm_expandtabs,    1, 2 },
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
    struct pyobj *o = PY_PTR(argv[0]);
    int64_t start = (argc >= 3) ? py_int_to_long(c, argv[2]) : 0;
    int64_t stop  = (argc >= 4) ? py_int_to_long(c, argv[3]) : (int64_t)o->list.len;
    if (start < 0) start += (int64_t)o->list.len;
    if (start < 0) start = 0;
    if (stop < 0) stop += (int64_t)o->list.len;
    if (stop > (int64_t)o->list.len) stop = (int64_t)o->list.len;
    for (int64_t i = start; i < stop; i++)
        if (py_eq_bool(c, o->list.items[i], argv[1])) return PY_FIX(i);
    py_raise_exc(c, c->EXC_ValueError, "value not in list");
}

static VALUE
lm_count(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    int64_t n = 0;
    for (size_t i = 0; i < o->list.len; i++)
        if (py_eq_bool(c, o->list.items[i], argv[1])) n++;
    return PY_FIX(n);
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
    VALUE key_fn = pystro_bi_kwarg("key");
    VALUE rev_v  = pystro_bi_kwarg("reverse");
    bool reverse = (rev_v == PY_TRUE);
    // Pre-compute sort keys when key_fn is set (Schwartzian transform).
    VALUE *keys = NULL;
    if (key_fn) {
        keys = (VALUE *)GC_malloc(sizeof(VALUE) * (o->list.len ? o->list.len : 1));
        for (size_t i = 0; i < o->list.len; i++) {
            keys[i] = py_apply(c, key_fn, 1, &o->list.items[i]);
            if (c->state != PY_STATE_NORMAL) return PY_NONE;
        }
    }
    // Insertion sort over items[] using keys[] for comparison.
    for (size_t i = 1; i < o->list.len; i++) {
        VALUE xv = o->list.items[i];
        VALUE xk = keys ? keys[i] : xv;
        size_t j = i;
        while (j > 0) {
            VALUE prev_k = keys ? keys[j - 1] : o->list.items[j - 1];
            int cmp = py_cmp(c, prev_k, xk);
            if (reverse ? (cmp >= 0) : (cmp <= 0)) break;
            o->list.items[j] = o->list.items[j - 1];
            if (keys) keys[j] = keys[j - 1];
            j--;
        }
        o->list.items[j] = xv;
        if (keys) keys[j] = xk;
    }
    return PY_NONE;
}

// list.remove(x) — remove first occurrence of x.
static VALUE
lm_remove(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    for (size_t i = 0; i < o->list.len; i++) {
        if (py_eq_bool(c, o->list.items[i], argv[1])) {
            for (size_t j = i; j + 1 < o->list.len; j++)
                o->list.items[j] = o->list.items[j+1];
            o->list.len--;
            return PY_NONE;
        }
    }
    py_raise_exc(c, c->EXC_ValueError, "list.remove(x): not in list");
}

// list.copy() / list.clear().
static VALUE
lm_copy(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    return py_make_list(o->list.items, o->list.len);
}
static VALUE
lm_clear(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    PY_PTR(argv[0])->list.len = 0;
    return PY_NONE;
}

static struct type_method list_methods[] = {
    { "append",  lm_append,  2, 2 },
    { "pop",     lm_pop,     1, 2 },
    { "extend",  lm_extend,  2, 2 },
    { "insert",  lm_insert,  3, 3 },
    { "index",   lm_index,   2, 4 },
    { "count",   lm_count,   2, 2 },
    { "reverse", lm_reverse, 1, 1 },
    { "sort",    lm_sort,    1, 1 },
    { "remove",  lm_remove,  2, 2 },
    { "copy",    lm_copy,    1, 1 },
    { "clear",   lm_clear,   1, 1 },
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
    for (size_t i = 0; i < d->elen; i++) {
        if (!pydict_entry_live(d, i)) continue;
        py_list_append(c, r, d->entries[i].key);
    }
    return r;
}

static VALUE
dm_values(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pydict *d = PY_PTR(argv[0])->dict;
    VALUE r = py_make_list(NULL, 0);
    for (size_t i = 0; i < d->elen; i++) {
        if (!pydict_entry_live(d, i)) continue;
        py_list_append(c, r, d->entries[i].value);
    }
    return r;
}

static VALUE
dm_items(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pydict *d = PY_PTR(argv[0])->dict;
    VALUE r = py_make_list(NULL, 0);
    for (size_t i = 0; i < d->elen; i++) {
        if (!pydict_entry_live(d, i)) continue;
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

static VALUE
dm_update(CTX *c, int argc, VALUE *argv)
{
    VALUE dst = argv[0];
    if (argc >= 2) {
        VALUE src = argv[1];
        if (py_is_dict(src)) {
            struct pydict *sd = PY_PTR(src)->dict;
            for (size_t i = 0; i < sd->elen; i++)
                if (pydict_entry_live(sd, i))
                    py_dict_set(c, dst, sd->entries[i].key, sd->entries[i].value);
        } else {
            struct py_iter it; py_iter_init(c, &it, src);
            if (c->state != PY_STATE_NORMAL) return PY_NONE;
            VALUE pair;
            while (py_iter_next(c, &it, &pair)) {
                if (!py_is_tuple(pair) && !py_is_list(pair)) py_raise_exc(c, c->EXC_TypeError, "update: pair");
                if (PY_PTR(pair)->list.len != 2) py_raise_exc(c, c->EXC_ValueError, "update: pair size");
                py_dict_set(c, dst, PY_PTR(pair)->list.items[0], PY_PTR(pair)->list.items[1]);
            }
        }
    }
    // Also pull in kwargs.
    extern int    PYSTRO_BI_KWC;
    extern const char **PYSTRO_BI_KWNAMES;
    extern VALUE *PYSTRO_BI_KWVALUES;
    for (int i = 0; i < PYSTRO_BI_KWC; i++) {
        VALUE k = py_make_str(PYSTRO_BI_KWNAMES[i], strlen(PYSTRO_BI_KWNAMES[i]));
        py_dict_set(c, dst, k, PYSTRO_BI_KWVALUES[i]);
    }
    return PY_NONE;
}

static VALUE
dm_setdefault(CTX *c, int argc, VALUE *argv)
{
    VALUE d = argv[0], k = argv[1];
    VALUE dflt = (argc >= 3) ? argv[2] : PY_NONE;
    if (py_dict_has(c, d, k)) return py_dict_get(c, d, k);
    py_dict_set(c, d, k, dflt);
    return dflt;
}

static VALUE
dm_popitem(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pydict *d = PY_PTR(argv[0])->dict;
    if (d->used == 0) py_raise_exc(c, c->EXC_KeyError, "popitem: empty dict");
    // Find the LAST live entry (Python 3.7+ semantic: LIFO).
    for (size_t i = d->elen; i > 0; ) {
        i--;
        if (pydict_entry_live(d, i)) {
            VALUE pair[2] = { d->entries[i].key, d->entries[i].value };
            py_dict_remove(c, argv[0], pair[0]);
            return py_make_tuple(pair, 2);
        }
    }
    py_raise_exc(c, c->EXC_KeyError, "popitem: empty");
}

static VALUE
dm_clear(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pydict *d = PY_PTR(argv[0])->dict;
    // Re-init the dict.
    d->elen = 0;
    d->used = 0;
    d->fill = 0;
    for (size_t i = 0; i < d->icapa; i++) d->indices[i] = DICT_EMPTY_IDX;
    return PY_NONE;
}

static VALUE
dm_copy(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pydict *d = PY_PTR(argv[0])->dict;
    VALUE r = py_make_dict();
    for (size_t i = 0; i < d->elen; i++)
        if (pydict_entry_live(d, i))
            py_dict_set(c, r, d->entries[i].key, d->entries[i].value);
    return r;
}

// Dunder dispatchers used so that built-in subclasses can call
// super().__setitem__ etc. via py_super_lookup → py_builtin_method.
static VALUE
dm_setitem(CTX *c, int argc, VALUE *argv)
{ (void)argc; py_dict_set(c, argv[0], argv[1], argv[2]); return PY_NONE; }
static VALUE
dm_getitem(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_dict_has(c, argv[0], argv[1])) {
        VALUE r = py_to_repr(c, argv[1]);
        py_raise_exc(c, c->EXC_KeyError, "%s",
                     py_is_str(r) ? PY_PTR(r)->str.chars : "?");
    }
    return py_dict_get(c, argv[0], argv[1]);
}
static VALUE
dm_delitem(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_dict_remove(c, argv[0], argv[1])) {
        VALUE r = py_to_repr(c, argv[1]);
        py_raise_exc(c, c->EXC_KeyError, "%s",
                     py_is_str(r) ? PY_PTR(r)->str.chars : "?");
    }
    return PY_NONE;
}
static VALUE
dm_contains(CTX *c, int argc, VALUE *argv)
{ (void)argc; return py_dict_has(c, argv[0], argv[1]) ? PY_TRUE : PY_FALSE; }
static VALUE
dm_len(CTX *c, int argc, VALUE *argv)
{ (void)c; (void)argc; return PY_FIX((int64_t)PY_PTR(argv[0])->dict->used); }

static struct type_method dict_methods[] = {
    { "get",        dm_get,        2, 3 },
    { "keys",       dm_keys,       1, 1 },
    { "values",     dm_values,     1, 1 },
    { "items",      dm_items,      1, 1 },
    { "pop",        dm_pop,        2, 3 },
    { "update",     dm_update,     1, 2 },
    { "setdefault", dm_setdefault, 2, 3 },
    { "popitem",    dm_popitem,    1, 1 },
    { "clear",      dm_clear,      1, 1 },
    { "copy",       dm_copy,       1, 1 },
    { "__setitem__", dm_setitem,   3, 3 },
    { "__getitem__", dm_getitem,   2, 2 },
    { "__delitem__", dm_delitem,   2, 2 },
    { "__contains__", dm_contains, 2, 2 },
    { "__len__",     dm_len,       1, 1 },
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
    VALUE sv = argv[0];
    struct pydict *d = PY_PTR(sv)->dict;
    for (size_t i = 0; i < d->elen; i++) {
        if (pydict_entry_live(d, i)) {
            VALUE k = d->entries[i].key;
            py_dict_remove(c, sv, k);
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
    for (size_t i = 0; i < a->elen; i++)
        if (pydict_entry_live(a, i)) py_dict_set(c, r, a->entries[i].key, PY_NONE);
    if (py_is_set(argv[1])) {
        struct pydict *b = PY_PTR(argv[1])->dict;
        for (size_t i = 0; i < b->elen; i++)
            if (pydict_entry_live(b, i)) py_dict_set(c, r, b->entries[i].key, PY_NONE);
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
    for (size_t i = 0; i < a->elen; i++) {
        if (pydict_entry_live(a, i) && py_contains(c, argv[1], a->entries[i].key))
            py_dict_set(c, r, a->entries[i].key, PY_NONE);
    }
    return r;
}
static VALUE
sm_difference(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE r = py_make_set();
    struct pydict *a = PY_PTR(argv[0])->dict;
    for (size_t i = 0; i < a->elen; i++) {
        if (pydict_entry_live(a, i) && !py_contains(c, argv[1], a->entries[i].key))
            py_dict_set(c, r, a->entries[i].key, PY_NONE);
    }
    return r;
}

static VALUE
sm_symmetric_difference(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE a = argv[0], b = argv[1];
    VALUE r = py_make_set();
    struct pydict *aa = PY_PTR(a)->dict;
    for (size_t i = 0; i < aa->elen; i++)
        if (pydict_entry_live(aa, i) && !py_contains(c, b, aa->entries[i].key))
            py_dict_set(c, r, aa->entries[i].key, PY_NONE);
    if (py_is_set(b)) {
        struct pydict *bb = PY_PTR(b)->dict;
        for (size_t i = 0; i < bb->elen; i++)
            if (pydict_entry_live(bb, i) && !py_contains(c, a, bb->entries[i].key))
                py_dict_set(c, r, bb->entries[i].key, PY_NONE);
    } else {
        struct py_iter it; py_iter_init(c, &it, b);
        VALUE x;
        while (py_iter_next(c, &it, &x))
            if (!py_contains(c, a, x))
                py_dict_set(c, r, x, PY_NONE);
    }
    return r;
}
static VALUE
sm_issubset(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    struct pydict *a = PY_PTR(argv[0])->dict;
    for (size_t i = 0; i < a->elen; i++)
        if (pydict_entry_live(a, i) && !py_contains(c, argv[1], a->entries[i].key))
            return PY_FALSE;
    return PY_TRUE;
}
static VALUE
sm_issuperset(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    struct py_iter it; py_iter_init(c, &it, argv[1]);
    VALUE x;
    while (py_iter_next(c, &it, &x))
        if (!py_contains(c, argv[0], x)) return PY_FALSE;
    return PY_TRUE;
}
static VALUE
sm_isdisjoint(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    struct py_iter it; py_iter_init(c, &it, argv[1]);
    VALUE x;
    while (py_iter_next(c, &it, &x))
        if (py_contains(c, argv[0], x)) return PY_FALSE;
    return PY_TRUE;
}
static VALUE
sm_set_copy(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE r = py_make_set();
    struct pydict *src = PY_PTR(argv[0])->dict;
    for (size_t i = 0; i < src->elen; i++)
        if (pydict_entry_live(src, i))
            py_dict_set(c, r, src->entries[i].key, PY_NONE);
    return r;
}
static VALUE
sm_set_clear(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    PY_PTR(argv[0])->dict = pydict_new();
    return PY_NONE;
}
static VALUE
sm_set_update(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE a = argv[0], b = argv[1];
    struct py_iter it; py_iter_init(c, &it, b);
    VALUE x;
    while (py_iter_next(c, &it, &x))
        py_dict_set(c, a, x, PY_NONE);
    return PY_NONE;
}

static VALUE
sm_difference_update(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE a = argv[0], b = argv[1];
    struct py_iter it; py_iter_init(c, &it, b);
    VALUE x;
    while (py_iter_next(c, &it, &x)) py_dict_remove(c, a, x);
    return PY_NONE;
}
static VALUE
sm_intersection_update(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE a = argv[0], b = argv[1];
    // Build a set of keys in `a` that are NOT in b, then remove.
    struct pydict *aa = PY_PTR(a)->dict;
    VALUE *to_remove = (VALUE *)alloca(sizeof(VALUE) * aa->elen);
    int n = 0;
    for (size_t i = 0; i < aa->elen; i++) {
        if (!pydict_entry_live(aa, i)) continue;
        if (!py_contains(c, b, aa->entries[i].key))
            to_remove[n++] = aa->entries[i].key;
    }
    for (int i = 0; i < n; i++) py_dict_remove(c, a, to_remove[i]);
    return PY_NONE;
}
static VALUE
sm_symmetric_difference_update(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE a = argv[0], b = argv[1];
    struct py_iter it; py_iter_init(c, &it, b);
    VALUE x;
    while (py_iter_next(c, &it, &x)) {
        if (py_contains(c, a, x)) py_dict_remove(c, a, x);
        else py_dict_set(c, a, x, PY_NONE);
    }
    return PY_NONE;
}

static struct type_method set_methods[] = {
    { "add",                  sm_add,                  2, 2 },
    { "discard",              sm_discard,              2, 2 },
    { "remove",               sm_remove,               2, 2 },
    { "pop",                  sm_set_pop,              1, 1 },
    { "clear",                sm_set_clear,            1, 1 },
    { "copy",                 sm_set_copy,             1, 1 },
    { "update",               sm_set_update,           2, 2 },
    { "union",                sm_union,                2, 2 },
    { "intersection",         sm_intersection,         2, 2 },
    { "difference",           sm_difference,           2, 2 },
    { "symmetric_difference", sm_symmetric_difference, 2, 2 },
    { "intersection_update",  sm_intersection_update,  2, 2 },
    { "difference_update",    sm_difference_update,    2, 2 },
    { "symmetric_difference_update", sm_symmetric_difference_update, 2, 2 },
    { "issubset",             sm_issubset,             2, 2 },
    { "issuperset",           sm_issuperset,           2, 2 },
    { "isdisjoint",           sm_isdisjoint,           2, 2 },
    { NULL, NULL, 0, 0 }
};

// frozenset: read-only ops only.
static struct type_method frozenset_methods[] = {
    { "copy",                 sm_set_copy,             1, 1 },
    { "union",                sm_union,                2, 2 },
    { "intersection",         sm_intersection,         2, 2 },
    { "difference",           sm_difference,           2, 2 },
    { "symmetric_difference", sm_symmetric_difference, 2, 2 },
    { "issubset",             sm_issubset,             2, 2 },
    { "issuperset",           sm_issuperset,           2, 2 },
    { "isdisjoint",           sm_isdisjoint,           2, 2 },
    { NULL, NULL, 0, 0 }
};

// bytes / bytearray methods.  Most mirror str methods but operate on
// raw byte sequences and yield int (0-255) on per-element access.
static VALUE
bm_decode(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    // ASCII-equivalent passthrough — pystro doesn't distinguish UTF-8
    // from raw bytes at the str level.  Encoding arg (if any) ignored.
    struct pyobj *o = PY_PTR(argv[0]);
    return py_make_str(o->str.chars, o->str.len);
}

static VALUE
bm_encode(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    return py_make_bytes(o->str.chars, o->str.len);
}

// (find / endswith with start/end defined later in file)
static VALUE
bm_startswith(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pyobj *s = PY_PTR(argv[0]);
    int64_t slen = (int64_t)s->str.len;
    int64_t start = (argc >= 3 && argv[2] != PY_NONE) ? py_int_to_long(c, argv[2]) : 0;
    int64_t end   = (argc >= 4 && argv[3] != PY_NONE) ? py_int_to_long(c, argv[3]) : slen;
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    int64_t span = end - start;
    if (span < 0) span = 0;
    const char *base = s->str.chars + start;
    VALUE arg = argv[1];
    if (py_is_tuple(arg)) {
        size_t n = PY_PTR(arg)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE p = PY_PTR(arg)->list.items[i];
            if (!py_is_byteseq(p)) continue;
            struct pyobj *pp = PY_PTR(p);
            if ((int64_t)pp->str.len > span) continue;
            if (memcmp(base, pp->str.chars, pp->str.len) == 0) return PY_TRUE;
        }
        return PY_FALSE;
    }
    if (!py_is_byteseq(arg)) py_raise_exc(c, c->EXC_TypeError, "startswith: not bytes/tuple");
    struct pyobj *p = PY_PTR(arg);
    if ((int64_t)p->str.len > span) return PY_FALSE;
    return memcmp(base, p->str.chars, p->str.len) == 0 ? PY_TRUE : PY_FALSE;
}

static VALUE
bm_split(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *s = PY_PTR(argv[0]);
    VALUE result = py_make_list(NULL, 0);
    if (argc == 1) {
        // split on whitespace runs
        size_t i = 0;
        while (i < s->str.len) {
            unsigned char ch;
            while (i < s->str.len && ((ch = (unsigned char)s->str.chars[i]),
                   ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')) i++;
            if (i >= s->str.len) break;
            size_t j = i;
            while (j < s->str.len && ((ch = (unsigned char)s->str.chars[j]),
                   !(ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'))) j++;
            py_list_append(c, result, py_make_bytes(s->str.chars + i, j - i));
            i = j;
        }
        return result;
    }
    if (!py_is_byteseq(argv[1])) py_raise_exc(c, c->EXC_TypeError, "bytes.split sep must be bytes");
    struct pyobj *sep = PY_PTR(argv[1]);
    if (sep->str.len == 0) py_raise_exc(c, c->EXC_ValueError, "empty separator");
    size_t i = 0;
    while (i <= s->str.len) {
        const char *p = i + sep->str.len <= s->str.len
            ? memmem(s->str.chars + i, s->str.len - i, sep->str.chars, sep->str.len) : NULL;
        if (!p) { py_list_append(c, result, py_make_bytes(s->str.chars + i, s->str.len - i)); break; }
        py_list_append(c, result, py_make_bytes(s->str.chars + i, (size_t)(p - (s->str.chars + i))));
        i = (size_t)(p - s->str.chars) + sep->str.len;
    }
    return result;
}

static VALUE
bm_replace(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_byteseq(argv[1]) || !py_is_byteseq(argv[2]))
        py_raise_exc(c, c->EXC_TypeError, "bytes.replace args must be bytes");
    struct pyobj *o = PY_PTR(argv[1]);
    struct pyobj *r = PY_PTR(argv[2]);
    if (o->str.len == 0) return argv[0];
    size_t cap = s->str.len + 16, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap + 1);
    size_t i = 0;
    while (i < s->str.len) {
        if (i + o->str.len <= s->str.len &&
            memcmp(s->str.chars + i, o->str.chars, o->str.len) == 0) {
            if (len + r->str.len + 1 > cap) {
                while (len + r->str.len + 1 > cap) cap *= 2;
                buf = (char *)GC_realloc(buf, cap + 1);
            }
            memcpy(buf + len, r->str.chars, r->str.len);
            len += r->str.len;
            i += o->str.len;
        } else {
            if (len + 2 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap + 1); }
            buf[len++] = s->str.chars[i++];
        }
    }
    buf[len] = '\0';
    struct pyobj *out = py_alloc(PY_T_BYTES);
    out->str.chars = buf; out->str.len = len;
    return PY_OBJ_VAL(out);
}

static VALUE
bm_hex(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(s->str.len * 2 + 1);
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < s->str.len; i++) {
        unsigned char ch = (unsigned char)s->str.chars[i];
        buf[i*2] = hexd[ch >> 4];
        buf[i*2+1] = hexd[ch & 0xf];
    }
    buf[s->str.len * 2] = '\0';
    return py_make_str_take(buf, s->str.len * 2);
}

static VALUE
bm_append(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (PY_PTR(argv[0])->type != PY_T_BYTEARRAY)
        py_raise_exc(c, c->EXC_TypeError, "append on bytes (not bytearray)");
    int64_t b = py_int_to_long(c, argv[1]);
    if (b < 0 || b > 255) py_raise_exc(c, c->EXC_ValueError, "byte must be 0..255");
    struct pyobj *o = PY_PTR(argv[0]);
    size_t L = o->str.len;
    char *nb = (char *)GC_malloc_atomic(L + 2);
    if (L > 0) memcpy(nb, o->str.chars, L);
    nb[L] = (char)b;
    nb[L + 1] = '\0';
    o->str.chars = nb;
    o->str.len = L + 1;
    return PY_NONE;
}

static VALUE
bm_extend(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (PY_PTR(argv[0])->type != PY_T_BYTEARRAY)
        py_raise_exc(c, c->EXC_TypeError, "extend on bytes (not bytearray)");
    struct pyobj *o = PY_PTR(argv[0]);
    if (py_is_byteseq(argv[1])) {
        size_t add = PY_PTR(argv[1])->str.len;
        size_t L = o->str.len;
        char *nb = (char *)GC_malloc_atomic(L + add + 1);
        if (L > 0) memcpy(nb, o->str.chars, L);
        memcpy(nb + L, PY_PTR(argv[1])->str.chars, add);
        nb[L + add] = '\0';
        o->str.chars = nb;
        o->str.len = L + add;
        return PY_NONE;
    }
    // Iterable of ints.
    struct py_iter it; py_iter_init(c, &it, argv[1]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    while (py_iter_next(c, &it, &x)) {
        VALUE av[2] = { argv[0], x };
        bm_append(c, 2, av);
    }
    return PY_NONE;
}

// int methods.  Operate on fixnums and bignums.
static VALUE
im_bit_length(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (PY_IS_FIXNUM(v)) {
        int64_t x = PY_FIXVAL(v);
        if (x < 0) x = -x;
        int n = 0;
        while (x) { n++; x >>= 1; }
        return PY_FIX(n);
    }
    if (v == PY_TRUE)  return PY_FIX(1);
    if (v == PY_FALSE) return PY_FIX(0);
    if (py_is_bignum(v)) {
        mpz_t z; py_to_mpz(c, v, z);
        size_t n = mpz_sizeinbase(z, 2);
        // mpz_sizeinbase returns 1 for 0; CPython returns 0.
        if (mpz_sgn(z) == 0) n = 0;
        mpz_clear(z);
        return PY_FIX((int64_t)n);
    }
    py_raise_exc(c, c->EXC_TypeError, "bit_length: int required");
}

static VALUE
im_bit_count(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (PY_IS_FIXNUM(v)) {
        uint64_t x = (uint64_t)(PY_FIXVAL(v) < 0 ? -PY_FIXVAL(v) : PY_FIXVAL(v));
        return PY_FIX(__builtin_popcountll(x));
    }
    if (v == PY_TRUE)  return PY_FIX(1);
    if (v == PY_FALSE) return PY_FIX(0);
    if (py_is_bignum(v)) {
        mpz_t z; py_to_mpz(c, v, z);
        if (mpz_sgn(z) < 0) mpz_neg(z, z);
        mp_bitcnt_t n = mpz_popcount(z);
        mpz_clear(z);
        return PY_FIX((int64_t)n);
    }
    py_raise_exc(c, c->EXC_TypeError, "bit_count: int required");
}

static VALUE
im_to_bytes(CTX *c, int argc, VALUE *argv)
{
    // self.to_bytes(length, byteorder='big', signed=False)
    VALUE self = argv[0];
    int64_t length = py_int_to_long(c, argv[1]);
    const char *order = "big";
    if (argc >= 3) {
        if (!py_is_str(argv[2])) py_raise_exc(c, c->EXC_TypeError, "byteorder must be str");
        order = PY_PTR(argv[2])->str.chars;
    }
    bool is_signed = false;
    VALUE sk = pystro_bi_kwarg("signed");
    if (sk) is_signed = py_is_truthy(sk);
    VALUE bk = pystro_bi_kwarg("byteorder");
    if (bk && py_is_str(bk)) order = PY_PTR(bk)->str.chars;
    bool big = strcmp(order, "big") == 0;
    if (length < 0) py_raise_exc(c, c->EXC_ValueError, "length must be non-negative");
    char *buf = (char *)GC_malloc_atomic(length + 1);
    memset(buf, 0, length);
    mpz_t z; py_to_mpz(c, self, z);
    bool neg = mpz_sgn(z) < 0;
    if (neg) {
        if (!is_signed) {
            mpz_clear(z);
            py_raise_exc(c, c->EXC_OverflowError, "can't convert negative int to unsigned");
        }
        // Two's complement: add 2^(8*length).
        mpz_t mod; mpz_init(mod);
        mpz_ui_pow_ui(mod, 2, (unsigned long)(8 * length));
        mpz_add(z, z, mod);
        mpz_clear(mod);
    }
    // Check overflow.
    {
        mpz_t cap; mpz_init(cap);
        mpz_ui_pow_ui(cap, 2, (unsigned long)(8 * length));
        if (mpz_cmp(z, cap) >= 0) {
            mpz_clear(cap); mpz_clear(z);
            py_raise_exc(c, c->EXC_OverflowError, "int too big to convert");
        }
        mpz_clear(cap);
    }
    for (int64_t i = 0; i < length; i++) {
        unsigned int b = (unsigned int)mpz_fdiv_ui(z, 256);
        mpz_fdiv_q_ui(z, z, 256);
        if (big) buf[length - 1 - i] = (char)b;
        else     buf[i] = (char)b;
    }
    mpz_clear(z);
    VALUE r = py_make_bytes(buf, (size_t)length);
    return r;
}

static VALUE
im_to_bytes_method(CTX *c, int argc, VALUE *argv) { return im_to_bytes(c, argc, argv); }

static VALUE
im_int_index(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

static VALUE
im_real(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

static VALUE
im_imag(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return PY_FIX(0); }

static VALUE
im_numerator(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

static VALUE
im_denominator(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return PY_FIX(1); }

static VALUE
im_conjugate(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

static struct type_method int_methods[] = {
    { "bit_length",  im_bit_length, 1, 1 },
    { "bit_count",   im_bit_count,  1, 1 },
    { "to_bytes",    im_to_bytes_method, 2, 3 },
    { "__index__",   im_int_index,  1, 1 },
    { "__int__",     im_int_index,  1, 1 },
    { "real",        im_real,       1, 1 },
    { "imag",        im_imag,       1, 1 },
    { "numerator",   im_numerator,  1, 1 },
    { "denominator", im_denominator,1, 1 },
    { "conjugate",   im_conjugate,  1, 1 },
    { NULL, NULL, 0, 0 }
};

// float methods
static VALUE
fm_is_integer(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    double d = PY_IS_FLONUM(v) ? py_flonum_to_double(v) : PY_PTR(v)->dbl;
    if (d != d) return PY_FALSE;          // NaN
    if (d == (double)(int64_t)d) return PY_TRUE;
    return PY_FALSE;
}

static VALUE
fm_hex(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    double d = PY_IS_FLONUM(v) ? py_flonum_to_double(v) : PY_PTR(v)->dbl;
    char buf[64];
    snprintf(buf, sizeof(buf), "%a", d);
    // CPython uses "0x1.8p+0" form, glibc %a matches.
    return py_make_str(buf, strlen(buf));
}

static VALUE
fm_real_f(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }
static VALUE
fm_imag_f(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return py_make_float(0.0); }
static VALUE
fm_conj_f(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

// (a, b) such that float == a/b exactly, with b > 0 and gcd(a, b) == 1.
// Mirrors CPython's float.as_integer_ratio.
static VALUE
fm_as_integer_ratio(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    double d = py_to_double(c, argv[0]);
    if (d != d || d == d * 0.5)  // NaN or +/-inf (inf == inf*0.5)
        py_raise_exc(c, c->EXC_OverflowError, "cannot convert non-finite to ratio");
    int exp;
    double m = frexp(d, &exp);  // d = m * 2^exp, m in [0.5, 1)
    // Make m an integer: shift mantissa by 53 bits.
    for (int i = 0; i < 53 && m != floor(m); i++) {
        m *= 2.0;
        exp--;
    }
    // Build numerator from m, denominator = 1 << -exp (or scale by 2^exp).
    mpz_t num, den;
    mpz_init(num); mpz_init(den);
    if (m < 0) {
        mpz_set_d(num, -m);
        mpz_neg(num, num);
    } else {
        mpz_set_d(num, m);
    }
    if (exp >= 0) {
        mpz_mul_2exp(num, num, (unsigned)exp);
        mpz_set_ui(den, 1);
    } else {
        mpz_set_ui(den, 1);
        mpz_mul_2exp(den, den, (unsigned)(-exp));
    }
    // Reduce.
    mpz_t g; mpz_init(g);
    mpz_gcd(g, num, den);
    mpz_divexact(num, num, g);
    mpz_divexact(den, den, g);
    mpz_clear(g);
    VALUE pair[2];
    pair[0] = py_normalise_int(num);
    pair[1] = py_normalise_int(den);
    mpz_clear(num); mpz_clear(den);
    return py_make_tuple(pair, 2);
}

static struct type_method float_methods[] = {
    { "is_integer", fm_is_integer, 1, 1 },
    { "hex",        fm_hex,        1, 1 },
    { "as_integer_ratio", fm_as_integer_ratio, 1, 1 },
    { "real",       fm_real_f,     1, 1 },
    { "imag",       fm_imag_f,     1, 1 },
    { "conjugate",  fm_conj_f,     1, 1 },
    { NULL, NULL, 0, 0 }
};

// complex methods
static VALUE
cm_real(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return py_make_float(PY_PTR(argv[0])->cpx.re); }
static VALUE
cm_imag(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return py_make_float(PY_PTR(argv[0])->cpx.im); }
static VALUE
cm_conj(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    return py_make_complex(PY_PTR(argv[0])->cpx.re, -PY_PTR(argv[0])->cpx.im);
}
static struct type_method complex_methods[] = {
    { "real",      cm_real, 1, 1 },
    { "imag",      cm_imag, 1, 1 },
    { "conjugate", cm_conj, 1, 1 },
    { NULL, NULL, 0, 0 }
};

// tuple methods (read-only)
static VALUE
tm_index(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    int64_t start = (argc >= 3) ? py_int_to_long(c, argv[2]) : 0;
    int64_t stop  = (argc >= 4) ? py_int_to_long(c, argv[3]) : (int64_t)o->list.len;
    if (start < 0) start += (int64_t)o->list.len;
    if (start < 0) start = 0;
    if (stop < 0) stop += (int64_t)o->list.len;
    if (stop > (int64_t)o->list.len) stop = (int64_t)o->list.len;
    for (int64_t i = start; i < stop; i++)
        if (py_eq_bool(c, o->list.items[i], argv[1])) return PY_FIX(i);
    py_raise_exc(c, c->EXC_ValueError, "value not in tuple");
}
static VALUE
tm_count(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    int64_t n = 0;
    for (size_t i = 0; i < o->list.len; i++)
        if (py_eq_bool(c, o->list.items[i], argv[1])) n++;
    return PY_FIX(n);
}
static struct type_method tuple_methods[] = {
    { "index", tm_index, 2, 4 },
    { "count", tm_count, 2, 2 },
    { NULL, NULL, 0, 0 }
};

// range methods + start/stop/step (also exposed via py_getattr below)
static VALUE
rm_index(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    int64_t target = py_int_to_long(c, argv[1]);
    struct pyobj *o = PY_PTR(argv[0]);
    int64_t start = o->range.start, stop = o->range.stop, step = o->range.step;
    if (step > 0) {
        if (target < start || target >= stop) py_raise_exc(c, c->EXC_ValueError, "not in range");
        if ((target - start) % step != 0) py_raise_exc(c, c->EXC_ValueError, "not in range");
        return PY_FIX((target - start) / step);
    } else {
        if (target > start || target <= stop) py_raise_exc(c, c->EXC_ValueError, "not in range");
        if ((start - target) % (-step) != 0) py_raise_exc(c, c->EXC_ValueError, "not in range");
        return PY_FIX((start - target) / step);
    }
}
static VALUE
rm_count(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_int_or_bool(argv[1])) return PY_FIX(0);
    int64_t target = py_int_to_long(c, argv[1]);
    struct pyobj *o = PY_PTR(argv[0]);
    int64_t start = o->range.start, stop = o->range.stop, step = o->range.step;
    if (step > 0) {
        if (target < start || target >= stop) return PY_FIX(0);
        return ((target - start) % step == 0) ? PY_FIX(1) : PY_FIX(0);
    } else {
        if (target > start || target <= stop) return PY_FIX(0);
        return ((start - target) % (-step) == 0) ? PY_FIX(1) : PY_FIX(0);
    }
}
static struct type_method range_methods[] = {
    { "index", rm_index, 2, 2 },
    { "count", rm_count, 2, 2 },
    { NULL, NULL, 0, 0 }
};

// Class-level: int.from_bytes(bytes, byteorder='big', signed=False)
static VALUE
bi_int_from_bytes(CTX *c, int argc, VALUE *argv)
{
    if (argc < 1 || !(py_is_bytes(argv[0]) || py_is_bytearray(argv[0])))
        py_raise_exc(c, c->EXC_TypeError, "from_bytes: bytes-like required");
    const char *order = "big";
    if (argc >= 2 && py_is_str(argv[1])) order = PY_PTR(argv[1])->str.chars;
    bool is_signed = false;
    VALUE sk = pystro_bi_kwarg("signed");
    if (sk) is_signed = py_is_truthy(sk);
    VALUE bk = pystro_bi_kwarg("byteorder");
    if (bk && py_is_str(bk)) order = PY_PTR(bk)->str.chars;
    bool big = strcmp(order, "big") == 0;
    struct pyobj *o = PY_PTR(argv[0]);
    size_t n = o->str.len;
    mpz_t z; mpz_init(z);
    for (size_t i = 0; i < n; i++) {
        size_t k = big ? i : (n - 1 - i);
        unsigned char b = (unsigned char)o->str.chars[k];
        mpz_mul_ui(z, z, 256);
        mpz_add_ui(z, z, b);
    }
    if (is_signed && n > 0) {
        unsigned char first = (unsigned char)o->str.chars[big ? 0 : n - 1];
        if (first & 0x80) {
            mpz_t cap; mpz_init(cap);
            mpz_ui_pow_ui(cap, 2, (unsigned long)(8 * n));
            mpz_sub(z, z, cap);
            mpz_clear(cap);
        }
    }
    VALUE r = py_normalise_int(z);
    mpz_clear(z);
    return r;
}

// bytes.fromhex(s)
static VALUE
bi_bytes_fromhex(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "fromhex: str required");
    const char *s = PY_PTR(argv[0])->str.chars;
    size_t n = PY_PTR(argv[0])->str.len;
    char *buf = (char *)GC_malloc_atomic(n / 2 + 1);
    size_t out = 0;
    int hi = -1;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\n') continue;
        int d = (ch >= '0' && ch <= '9') ? ch - '0'
              : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
              : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (d < 0) py_raise_exc(c, c->EXC_ValueError, "non-hex digit in fromhex");
        if (hi < 0) hi = d;
        else { buf[out++] = (char)((hi << 4) | d); hi = -1; }
    }
    if (hi >= 0) py_raise_exc(c, c->EXC_ValueError, "odd-length hex string");
    return py_make_bytes(buf, out);
}

// float.fromhex(s)
static VALUE
bi_float_fromhex(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "fromhex: str required");
    char *end;
    double d = strtod(PY_PTR(argv[0])->str.chars, &end);
    return py_make_float(d);
}

// dict.fromkeys(iter, default=None)
static VALUE
bi_dict_fromkeys(CTX *c, int argc, VALUE *argv)
{
    VALUE def = argc >= 2 ? argv[1] : PY_NONE;
    VALUE r = py_make_dict();
    struct py_iter it; py_iter_init(c, &it, argv[0]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE k;
    while (py_iter_next(c, &it, &k)) {
        py_dict_set(c, r, k, def);
    }
    return r;
}

// bytes.join(iter) — concatenate bytes-like with self as separator.
static VALUE
bm_join(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE self = argv[0];
    VALUE iter = argv[1];
    const char *sep = PY_PTR(self)->str.chars;
    size_t slen = PY_PTR(self)->str.len;
    VALUE items[256]; int n = 0;
    if (py_is_list(iter) || py_is_tuple(iter)) {
        size_t sn = PY_PTR(iter)->list.len;
        if (sn > 256) py_raise_exc(c, c->EXC_RuntimeError, "bytes.join too many");
        for (size_t i = 0; i < sn; i++) items[n++] = PY_PTR(iter)->list.items[i];
    } else {
        struct py_iter it; py_iter_init(c, &it, iter);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
        VALUE x;
        while (py_iter_next(c, &it, &x)) {
            if (n >= 256) py_raise_exc(c, c->EXC_RuntimeError, "bytes.join too many");
            items[n++] = x;
        }
    }
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        if (!py_is_byteseq(items[i]))
            py_raise_exc(c, c->EXC_TypeError, "bytes.join element must be bytes-like");
        total += PY_PTR(items[i])->str.len;
        if (i) total += slen;
    }
    char *buf = (char *)GC_malloc_atomic(total + 1);
    char *p = buf;
    for (int i = 0; i < n; i++) {
        if (i) { memcpy(p, sep, slen); p += slen; }
        memcpy(p, PY_PTR(items[i])->str.chars, PY_PTR(items[i])->str.len);
        p += PY_PTR(items[i])->str.len;
    }
    *p = '\0';
    return py_is_bytearray(self) ? py_make_bytearray(buf, total) : py_make_bytes(buf, total);
}

// bytes.count(sub)
static VALUE
bm_count(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_byteseq(argv[1])) py_raise_exc(c, c->EXC_TypeError, "bytes.count: bytes-like required");
    struct pyobj *sub = PY_PTR(argv[1]);
    if (sub->str.len == 0) return PY_FIX((int64_t)s->str.len + 1);
    int64_t count = 0;
    size_t i = 0;
    while (i + sub->str.len <= s->str.len) {
        if (memcmp(s->str.chars + i, sub->str.chars, sub->str.len) == 0) {
            count++;
            i += sub->str.len;
        } else {
            i++;
        }
    }
    return PY_FIX(count);
}

// bytes.upper / lower
static VALUE
bm_upper(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    return py_is_bytearray(argv[0]) ? py_make_bytearray(buf, s->str.len)
                                    : py_make_bytes(buf, s->str.len);
}

static VALUE
bm_lower(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    return py_is_bytearray(argv[0]) ? py_make_bytearray(buf, s->str.len)
                                    : py_make_bytes(buf, s->str.len);
}

// bytes additional methods.
static VALUE
bm_title(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(s->str.len + 1);
    bool prev_alpha = false;
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        bool is_alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (is_alpha && !prev_alpha) {
            if (ch >= 'a' && ch <= 'z') ch -= 32;
        } else if (is_alpha) {
            if (ch >= 'A' && ch <= 'Z') ch += 32;
        }
        buf[i] = ch;
        prev_alpha = is_alpha;
    }
    buf[s->str.len] = '\0';
    return py_is_bytearray(argv[0]) ? py_make_bytearray(buf, s->str.len)
                                    : py_make_bytes(buf, s->str.len);
}

static VALUE
bm_capitalize(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (s->str.len == 0)
        return py_is_bytearray(argv[0]) ? py_make_bytearray("", 0)
                                        : py_make_bytes("", 0);
    char *buf = (char *)GC_malloc_atomic(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (i == 0) {
            if (ch >= 'a' && ch <= 'z') ch -= 32;
        } else if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    return py_is_bytearray(argv[0]) ? py_make_bytearray(buf, s->str.len)
                                    : py_make_bytes(buf, s->str.len);
}

static VALUE
bm_swapcase(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        else if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    return py_is_bytearray(argv[0]) ? py_make_bytearray(buf, s->str.len)
                                    : py_make_bytes(buf, s->str.len);
}

static VALUE
bm_zfill(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    int64_t w = py_int_to_long(c, argv[1]);
    if ((int64_t)s->str.len >= w)
        return py_is_bytearray(argv[0]) ? py_make_bytearray(s->str.chars, s->str.len)
                                        : py_make_bytes(s->str.chars, s->str.len);
    char *buf = (char *)GC_malloc_atomic(w + 1);
    int sign = 0;
    if (s->str.len > 0 && (s->str.chars[0] == '-' || s->str.chars[0] == '+')) {
        buf[0] = s->str.chars[0]; sign = 1;
    }
    int64_t pad = w - (int64_t)s->str.len;
    for (int64_t i = 0; i < pad; i++) buf[sign + i] = '0';
    memcpy(buf + sign + pad, s->str.chars + sign, s->str.len - sign);
    buf[w] = '\0';
    return py_is_bytearray(argv[0]) ? py_make_bytearray(buf, w)
                                    : py_make_bytes(buf, w);
}

static VALUE
bm_pad_helper(CTX *c, int argc, VALUE *argv, int align)
{
    (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    int64_t w = py_int_to_long(c, argv[1]);
    char fill = ' ';
    if (argc >= 3 && py_is_byteseq(argv[2])
        && PY_PTR(argv[2])->str.len == 1) fill = PY_PTR(argv[2])->str.chars[0];
    if ((int64_t)s->str.len >= w)
        return py_is_bytearray(argv[0]) ? py_make_bytearray(s->str.chars, s->str.len)
                                        : py_make_bytes(s->str.chars, s->str.len);
    char *buf = (char *)GC_malloc_atomic(w + 1);
    int64_t pad = w - (int64_t)s->str.len;
    if (align == 0) { // ljust
        memcpy(buf, s->str.chars, s->str.len);
        memset(buf + s->str.len, fill, pad);
    } else if (align == 1) { // rjust
        memset(buf, fill, pad);
        memcpy(buf + pad, s->str.chars, s->str.len);
    } else { // center
        int64_t left = pad / 2, right = pad - left;
        memset(buf, fill, left);
        memcpy(buf + left, s->str.chars, s->str.len);
        memset(buf + left + s->str.len, fill, right);
    }
    buf[w] = '\0';
    return py_is_bytearray(argv[0]) ? py_make_bytearray(buf, w)
                                    : py_make_bytes(buf, w);
}

static VALUE bm_ljust(CTX *c, int argc, VALUE *argv)  { return bm_pad_helper(c, argc, argv, 0); }
static VALUE bm_rjust(CTX *c, int argc, VALUE *argv)  { return bm_pad_helper(c, argc, argv, 1); }
static VALUE bm_center(CTX *c, int argc, VALUE *argv) { return bm_pad_helper(c, argc, argv, 2); }

static VALUE
bm_isalpha(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (s->str.len == 0) return PY_FALSE;
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) return PY_FALSE;
    }
    return PY_TRUE;
}

static VALUE
bm_isdigit(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (s->str.len == 0) return PY_FALSE;
    for (size_t i = 0; i < s->str.len; i++)
        if (s->str.chars[i] < '0' || s->str.chars[i] > '9') return PY_FALSE;
    return PY_TRUE;
}

static VALUE
bm_isspace(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (s->str.len == 0) return PY_FALSE;
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\v' && ch != '\f')
            return PY_FALSE;
    }
    return PY_TRUE;
}

static VALUE
bm_find(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_byteseq(argv[1])) py_raise_exc(c, c->EXC_TypeError, "find: not bytes");
    struct pyobj *p = PY_PTR(argv[1]);
    int64_t slen = (int64_t)s->str.len;
    int64_t start = (argc >= 3 && argv[2] != PY_NONE) ? py_int_to_long(c, argv[2]) : 0;
    int64_t end   = (argc >= 4 && argv[3] != PY_NONE) ? py_int_to_long(c, argv[3]) : slen;
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    if (start > end) return PY_FIX(-1);
    if (p->str.len == 0) return PY_FIX(start);
    if ((size_t)(end - start) < p->str.len) return PY_FIX(-1);
    void *r = memmem(s->str.chars + start, (size_t)(end - start), p->str.chars, p->str.len);
    return r ? PY_FIX((int64_t)((char *)r - s->str.chars)) : PY_FIX(-1);
}

static VALUE
bm_rfind(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    if (!py_is_byteseq(argv[1])) py_raise_exc(c, c->EXC_TypeError, "rfind: not bytes");
    struct pyobj *p = PY_PTR(argv[1]);
    int64_t slen = (int64_t)s->str.len;
    int64_t start = (argc >= 3 && argv[2] != PY_NONE) ? py_int_to_long(c, argv[2]) : 0;
    int64_t end   = (argc >= 4 && argv[3] != PY_NONE) ? py_int_to_long(c, argv[3]) : slen;
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    if (p->str.len == 0) return PY_FIX(end);
    if ((int64_t)p->str.len > end - start) return PY_FIX(-1);
    for (int64_t i = end - (int64_t)p->str.len; i >= start; i--)
        if (memcmp(s->str.chars + i, p->str.chars, p->str.len) == 0) return PY_FIX(i);
    return PY_FIX(-1);
}

static VALUE
bm_index(CTX *c, int argc, VALUE *argv)
{
    VALUE r = bm_find(c, argc, argv);
    if (PY_IS_FIXNUM(r) && PY_FIXVAL(r) == -1)
        py_raise_exc(c, c->EXC_ValueError, "subsection not found");
    return r;
}

static VALUE
bm_rindex(CTX *c, int argc, VALUE *argv)
{
    VALUE r = bm_rfind(c, argc, argv);
    if (PY_IS_FIXNUM(r) && PY_FIXVAL(r) == -1)
        py_raise_exc(c, c->EXC_ValueError, "subsection not found");
    return r;
}

static VALUE
bm_endswith(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pyobj *s = PY_PTR(argv[0]);
    int64_t slen = (int64_t)s->str.len;
    int64_t start = (argc >= 3 && argv[2] != PY_NONE) ? py_int_to_long(c, argv[2]) : 0;
    int64_t end   = (argc >= 4 && argv[3] != PY_NONE) ? py_int_to_long(c, argv[3]) : slen;
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    int64_t span = end - start;
    if (span < 0) span = 0;
    const char *tail_end = s->str.chars + end;
    VALUE arg = argv[1];
    if (py_is_tuple(arg)) {
        size_t n = PY_PTR(arg)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE p = PY_PTR(arg)->list.items[i];
            if (!py_is_byteseq(p)) continue;
            struct pyobj *pp = PY_PTR(p);
            if ((int64_t)pp->str.len > span) continue;
            if (memcmp(tail_end - pp->str.len, pp->str.chars, pp->str.len) == 0) return PY_TRUE;
        }
        return PY_FALSE;
    }
    if (!py_is_byteseq(arg)) py_raise_exc(c, c->EXC_TypeError, "endswith: not bytes/tuple");
    struct pyobj *p = PY_PTR(arg);
    if ((int64_t)p->str.len > span) return PY_FALSE;
    return memcmp(tail_end - p->str.len, p->str.chars, p->str.len) == 0 ? PY_TRUE : PY_FALSE;
}

// bytes.strip / lstrip / rstrip — whitespace only by default.
static VALUE
bm_strip_impl(CTX *c, int argc, VALUE *argv, bool left, bool right)
{
    (void)c; (void)argc;
    struct pyobj *s = PY_PTR(argv[0]);
    size_t start = 0, end = s->str.len;
    if (left) {
        while (start < end) {
            unsigned char ch = s->str.chars[start];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') start++;
            else break;
        }
    }
    if (right) {
        while (end > start) {
            unsigned char ch = s->str.chars[end - 1];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') end--;
            else break;
        }
    }
    char *buf = (char *)GC_malloc_atomic(end - start + 1);
    memcpy(buf, s->str.chars + start, end - start);
    buf[end - start] = '\0';
    return py_is_bytearray(argv[0]) ? py_make_bytearray(buf, end - start)
                                    : py_make_bytes(buf, end - start);
}
static VALUE bm_strip(CTX *c, int argc, VALUE *argv)  { return bm_strip_impl(c, argc, argv, true, true); }
static VALUE bm_lstrip(CTX *c, int argc, VALUE *argv) { return bm_strip_impl(c, argc, argv, true, false); }
static VALUE bm_rstrip(CTX *c, int argc, VALUE *argv) { return bm_strip_impl(c, argc, argv, false, true); }

static struct type_method bytes_methods[] = {
    { "decode",     bm_decode,     1, 2 },
    { "encode",     bm_encode,     1, 2 },
    { "startswith", bm_startswith, 2, 4 },
    { "split",      bm_split,      1, 3 },
    { "replace",    bm_replace,    3, 3 },
    { "hex",        bm_hex,        1, 1 },
    { "append",     bm_append,     2, 2 },
    { "extend",     bm_extend,     2, 2 },
    { "join",       bm_join,       2, 2 },
    { "count",      bm_count,      2, 2 },
    { "upper",      bm_upper,      1, 1 },
    { "lower",      bm_lower,      1, 1 },
    { "strip",      bm_strip,      1, 2 },
    { "lstrip",     bm_lstrip,     1, 2 },
    { "rstrip",     bm_rstrip,     1, 2 },
    { "title",      bm_title,      1, 1 },
    { "capitalize", bm_capitalize, 1, 1 },
    { "swapcase",   bm_swapcase,   1, 1 },
    { "zfill",      bm_zfill,      2, 2 },
    { "center",     bm_center,     2, 3 },
    { "ljust",      bm_ljust,      2, 3 },
    { "rjust",      bm_rjust,      2, 3 },
    { "isalpha",    bm_isalpha,    1, 1 },
    { "isdigit",    bm_isdigit,    1, 1 },
    { "isspace",    bm_isspace,    1, 1 },
    { "find",       bm_find,       2, 4 },
    { "rfind",      bm_rfind,      2, 4 },
    { "index",      bm_index,      2, 4 },
    { "rindex",     bm_rindex,     2, 4 },
    { "endswith",   bm_endswith,   2, 4 },
    { NULL, NULL, 0, 0 }
};

// generator methods: send / throw / close + __next__ / __iter__.
extern VALUE py_gen_next(CTX *c, VALUE g);
extern VALUE py_gen_send(CTX *c, VALUE g, VALUE v);
extern VALUE py_gen_throw(CTX *c, VALUE g, VALUE exc);
extern VALUE py_gen_close(CTX *c, VALUE g);

static VALUE gm_send(CTX *c, int argc, VALUE *argv)  { (void)argc; return py_gen_send(c, argv[0], argv[1]); }
static VALUE
gm_throw(CTX *c, int argc, VALUE *argv)
{
    // Two-arg form: throw(exc) where exc is a class or instance.
    // Three-arg form: throw(type, value [, traceback]) — instantiate.
    VALUE exc = argv[1];
    if (argc >= 3 && py_is_class(exc)) {
        VALUE av[1] = { argv[2] };
        exc = py_apply(c, exc, 1, av);
    }
    return py_gen_throw(c, argv[0], exc);
}
static VALUE gm_close(CTX *c, int argc, VALUE *argv) { (void)argc; return py_gen_close(c, argv[0]); }
static VALUE gm_next(CTX *c, int argc, VALUE *argv)  { (void)argc; return py_gen_next(c, argv[0]); }
static VALUE gm_iter(CTX *c, int argc, VALUE *argv)  { (void)c; (void)argc; return argv[0]; }

static struct type_method gen_methods[] = {
    { "send",     gm_send,  2, 2 },
    { "throw",    gm_throw, 2, 4 },
    { "close",    gm_close, 1, 1 },
    { "__next__", gm_next,  1, 1 },
    { "__iter__", gm_iter,  1, 1 },
    { NULL, NULL, 0, 0 }
};

// ---------------------------------------------------------------------------
// Builtins.
// ---------------------------------------------------------------------------

static VALUE
bi_print(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    VALUE sep_v = pystro_bi_kwarg("sep");
    VALUE end_v = pystro_bi_kwarg("end");
    VALUE file_v = pystro_bi_kwarg("file");
    const char *sep = " ";  size_t sep_len = 1;
    const char *end = "\n"; size_t end_len = 1;
    if (sep_v && py_is_str(sep_v)) {
        sep = PY_PTR(sep_v)->str.chars;
        sep_len = PY_PTR(sep_v)->str.len;
    }
    if (end_v && py_is_str(end_v)) {
        end = PY_PTR(end_v)->str.chars;
        end_len = PY_PTR(end_v)->str.len;
    }
    FILE *fp = stdout;
    if (file_v && py_is_file(file_v)) fp = (FILE *)PY_PTR(file_v)->file.fp;
    for (int i = 0; i < argc; i++) {
        if (i) fwrite(sep, 1, sep_len, fp);
        py_display(fp, argv[i], false);
    }
    fwrite(end, 1, end_len, fp);
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
        if (d != d) py_raise_exc(c, c->EXC_ValueError, "cannot convert NaN to int");
        if (d == 1.0/0.0 || d == -1.0/0.0)
            py_raise_exc(c, c->EXC_OverflowError, "cannot convert inf to int");
        if (d >= (double)PY_FIXNUM_MIN && d <= (double)PY_FIXNUM_MAX)
            return PY_FIX((int64_t)d);
        mpz_t z; mpz_init(z); mpz_set_d(z, d);
        VALUE r = py_normalise_int(z); mpz_clear(z); return r;
    }
    if (py_is_str(v)) {
        // String may be a slice-borrow (no NUL-terminator within bounds);
        // copy into a stack buffer.
        size_t L = PY_PTR(v)->str.len;
        char small[64];
        char *buf = (L < sizeof(small)) ? small : (char *)GC_malloc_atomic(L + 1);
        memcpy(buf, PY_PTR(v)->str.chars, L);
        buf[L] = '\0';
        // strip leading/trailing whitespace.
        char *p = buf;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        char *end_p = buf + strlen(buf);
        while (end_p > p && (end_p[-1] == ' ' || end_p[-1] == '\t' || end_p[-1] == '\n' || end_p[-1] == '\r')) end_p--;
        *end_p = '\0';
        int base = 10;
        VALUE bk = pystro_bi_kwarg("base");
        if (argc >= 2) {
            base = (int)py_int_to_long(c, argv[1]);
            if (base != 0 && (base < 2 || base > 36))
                py_raise_exc(c, c->EXC_ValueError, "int() base must be 2..36 or 0");
        } else if (bk) {
            base = (int)py_int_to_long(c, bk);
            if (base != 0 && (base < 2 || base > 36))
                py_raise_exc(c, c->EXC_ValueError, "int() base must be 2..36 or 0");
        }
        // Handle 0x / 0b / 0o prefix when base==0 or matches.
        // Accept a leading +/- before any prefix (CPython allows this).
        char sign = 0;
        if (p[0] == '+' || p[0] == '-') {
            sign = p[0];
            p++;
        }
        if (base == 0 || base == 16) {
            if ((p[0] == '0') && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
        }
        if (base == 0 || base == 2) {
            if ((p[0] == '0') && (p[1] == 'b' || p[1] == 'B')) { p += 2; base = 2; }
        }
        if (base == 0 || base == 8) {
            if ((p[0] == '0') && (p[1] == 'o' || p[1] == 'O')) { p += 2; base = 8; }
        }
        if (base == 0) base = 10;
        // Re-attach the sign so mpz_set_str sees a sensible string.
        if (sign == '-') {
            p--;
            *p = '-';
        }
        // Strip embedded underscores (Python allows them as digit
        // grouping but only between digits; we accept any position).
        char *q = p;
        char *w = p;
        while (*q) { if (*q != '_') *w++ = *q; q++; }
        *w = '\0';
        mpz_t z; mpz_init(z);
        if (mpz_set_str(z, p, base) != 0) {
            mpz_clear(z); py_raise_exc(c, c->EXC_ValueError, "invalid literal for int()");
        }
        VALUE r = py_normalise_int(z); mpz_clear(z); return r;
    }
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__int__");
        if (m != PY_NONE) { VALUE av[1] = { v }; return py_apply(c, m, 1, av); }
        m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__index__");
        if (m != PY_NONE) { VALUE av[1] = { v }; return py_apply(c, m, 1, av); }
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
        size_t L = PY_PTR(v)->str.len;
        char small[64];
        char *buf = (L < sizeof(small)) ? small : (char *)GC_malloc_atomic(L + 1);
        memcpy(buf, PY_PTR(v)->str.chars, L);
        buf[L] = '\0';
        // strip underscores
        char *q = buf, *w = buf;
        while (*q) { if (*q != '_') *w++ = *q; q++; }
        *w = '\0';
        // Accept "inf", "Infinity", "nan" (case-insensitive).
        char *end;
        double d = strtod(buf, &end);
        if (end == buf) py_raise_exc(c, c->EXC_ValueError, "could not convert string to float");
        return py_make_float(d);
    }
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__float__");
        if (m != PY_NONE) { VALUE av[1] = { v }; return py_apply(c, m, 1, av); }
    }
    py_raise_exc(c, c->EXC_TypeError, "float() argument type not supported");
}

static VALUE
bi_complex(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return py_make_complex(0, 0);
    double re = 0, im = 0;
    if (argc >= 1) {
        if (py_is_complex(argv[0])) {
            re = PY_PTR(argv[0])->cpx.re;
            im = PY_PTR(argv[0])->cpx.im;
        } else {
            re = py_to_double(c, argv[0]);
        }
    }
    if (argc >= 2) {
        if (py_is_complex(argv[1])) {
            im += PY_PTR(argv[1])->cpx.re;
            re -= PY_PTR(argv[1])->cpx.im;
        } else {
            im += py_to_double(c, argv[1]);
        }
    }
    return py_make_complex(re, im);
}

static VALUE
bi_bool(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return PY_FALSE;
    VALUE v = argv[0];
    if (py_is_instance(v)) {
        VALUE cls = PY_OBJ_VAL(PY_PTR(v)->inst.cls);
        VALUE m = py_class_lookup_method(cls, "__bool__");
        if (m != PY_NONE) {
            VALUE av[1] = { v };
            VALUE r = py_apply(c, m, 1, av);
            return py_is_truthy(r) ? PY_TRUE : PY_FALSE;
        }
        VALUE lm = py_class_lookup_method(cls, "__len__");
        if (lm != PY_NONE) {
            VALUE av[1] = { v };
            VALUE r = py_apply(c, lm, 1, av);
            return py_int_to_long(c, r) != 0 ? PY_TRUE : PY_FALSE;
        }
    }
    return py_is_truthy(v) ? PY_TRUE : PY_FALSE;
}

static VALUE
bi_len(CTX *c, int argc, VALUE *argv) { (void)argc; return PY_FIX((int64_t)py_seq_len(c, argv[0])); }

static VALUE
bi_abs(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (v == PY_TRUE) return PY_FIX(1);
    if (v == PY_FALSE) return PY_FIX(0);
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
    if (py_is_complex(v)) {
        double re = PY_PTR(v)->cpx.re;
        double im = PY_PTR(v)->cpx.im;
        return py_make_float(sqrt(re*re + im*im));
    }
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__abs__");
        if (m != PY_NONE) {
            VALUE av[1] = { v };
            return py_apply(c, m, 1, av);
        }
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
    VALUE r = py_make_dict();
    // dict(another_dict) — copy.
    if (argc >= 1 && py_is_dict(argv[0])) {
        struct pydict *src = PY_PTR(argv[0])->dict;
        for (size_t i = 0; i < src->elen; i++)
            if (pydict_entry_live(src, i))
                py_dict_set(c, r, src->entries[i].key, src->entries[i].value);
    }
    // dict(mapping_like) — instance with `keys()` method.
    else if (argc >= 1 && py_is_instance(argv[0])) {
        VALUE keys_m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "keys");
        if (keys_m != PY_NONE) {
            VALUE av0[1] = { argv[0] };
            VALUE keys = py_apply(c, keys_m, 1, av0);
            if (c->state != PY_STATE_NORMAL) return PY_NONE;
            struct py_iter it; py_iter_init(c, &it, keys);
            if (c->state != PY_STATE_NORMAL) return PY_NONE;
            VALUE k;
            while (py_iter_next(c, &it, &k)) {
                VALUE v = py_list_get(c, argv[0], k);
                if (c->state != PY_STATE_NORMAL) return PY_NONE;
                py_dict_set(c, r, k, v);
            }
        } else {
            // Treat as iterable of (k, v) pairs.
            struct py_iter it; py_iter_init(c, &it, argv[0]);
            if (c->state != PY_STATE_NORMAL) return PY_NONE;
            VALUE x;
            while (py_iter_next(c, &it, &x)) {
                if (py_is_tuple(x) || py_is_list(x)) {
                    if (PY_PTR(x)->list.len != 2)
                        py_raise_exc(c, c->EXC_ValueError, "dict update: pair must be length 2");
                    py_dict_set(c, r, PY_PTR(x)->list.items[0], PY_PTR(x)->list.items[1]);
                } else {
                    py_raise_exc(c, c->EXC_TypeError, "dict update: not a pair");
                }
            }
        }
    }
    // dict([(k, v), ...]) — from iterable of pairs.
    else if (argc >= 1) {
        struct py_iter it; py_iter_init(c, &it, argv[0]);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
        VALUE x;
        while (py_iter_next(c, &it, &x)) {
            if (py_is_tuple(x) || py_is_list(x)) {
                if (PY_PTR(x)->list.len != 2)
                    py_raise_exc(c, c->EXC_ValueError, "dict update: pair must be length 2");
                py_dict_set(c, r, PY_PTR(x)->list.items[0], PY_PTR(x)->list.items[1]);
            } else {
                py_raise_exc(c, c->EXC_TypeError, "dict update: not a pair");
            }
        }
    }
    // **kwargs from caller.
    extern int    PYSTRO_BI_KWC;
    extern const char **PYSTRO_BI_KWNAMES;
    extern VALUE *PYSTRO_BI_KWVALUES;
    for (int i = 0; i < PYSTRO_BI_KWC; i++) {
        VALUE k = py_make_str(PYSTRO_BI_KWNAMES[i], strlen(PYSTRO_BI_KWNAMES[i]));
        py_dict_set(c, r, k, PYSTRO_BI_KWVALUES[i]);
    }
    return r;
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
bi_bytes(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return py_make_bytes("", 0);
    VALUE v = argv[0];
    if (PY_IS_FIXNUM(v)) {
        int64_t n = PY_FIXVAL(v);
        if (n < 0) py_raise_exc(c, c->EXC_ValueError, "negative count");
        char *buf = (char *)GC_malloc_atomic(n + 1);
        memset(buf, 0, n + 1);
        struct pyobj *o = py_alloc(PY_T_BYTES);
        o->str.chars = buf; o->str.len = (size_t)n;
        return PY_OBJ_VAL(o);
    }
    if (py_is_byteseq(v)) return py_make_bytes(PY_PTR(v)->str.chars, PY_PTR(v)->str.len);
    if (py_is_str(v))     return py_make_bytes(PY_PTR(v)->str.chars, PY_PTR(v)->str.len);
    // iterable of ints
    struct py_iter it; py_iter_init(c, &it, v);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    size_t cap = 16, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    VALUE x;
    while (py_iter_next(c, &it, &x)) {
        int64_t b = py_int_to_long(c, x);
        if (b < 0 || b > 255) py_raise_exc(c, c->EXC_ValueError, "byte must be 0..255");
        if (len == cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
        buf[len++] = (char)b;
    }
    return py_make_bytes(buf, len);
}

static VALUE
bi_bytearray(CTX *c, int argc, VALUE *argv)
{
    VALUE r = bi_bytes(c, argc, argv);
    if (py_is_bytes(r)) PY_PTR(r)->type = PY_T_BYTEARRAY;
    return r;
}

static VALUE
bi_frozenset(CTX *c, int argc, VALUE *argv)
{
    VALUE r = py_make_frozenset();
    if (argc == 0) return r;
    struct py_iter it; py_iter_init(c, &it, argv[0]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    VALUE x;
    while (py_iter_next(c, &it, &x)) py_dict_set(c, r, x, PY_NONE);
    return r;
}

// Look up a builtin by name and return its VALUE (the same object the
// user gets via the global name).  Used by `type()` to make
// `type(5) is int` true.
static VALUE
type_lookup_builtin(CTX *c, const char *name)
{
    int i = py_global_index(c, name);
    if (i >= 0 && c->globals->entries[i].defined)
        return c->globals->entries[i].value;
    return py_make_str(name, strlen(name));
}

VALUE
bi_type(CTX *c, int argc, VALUE *argv)
{
    // 3-arg form: type(name, bases, attrs) creates a new class.
    if (argc == 3) {
        if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "type(): name must be str");
        const char *name = PY_PTR(argv[0])->str.chars;
        // Bases tuple/list.
        VALUE bases = argv[1];
        VALUE first_base = PY_NONE;
        int nbases = 0;
        if (py_is_tuple(bases) || py_is_list(bases)) nbases = (int)PY_PTR(bases)->list.len;
        if (nbases > 0) first_base = PY_PTR(bases)->list.items[0];
        VALUE cls = py_make_class(name, first_base, false);
        if (nbases > 1) {
            VALUE *bv = (VALUE *)alloca(sizeof(VALUE) * nbases);
            for (int i = 0; i < nbases; i++) bv[i] = PY_PTR(bases)->list.items[i];
            extern void py_class_set_bases(VALUE cls, VALUE *bases, int n);
            py_class_set_bases(cls, bv, nbases);
        }
        // Pour attrs into the class.
        if (py_is_dict(argv[2])) {
            struct pydict *d = PY_PTR(argv[2])->dict;
            for (size_t i = 0; i < d->elen; i++) {
                if (!pydict_entry_live(d, i)) continue;
                VALUE k = d->entries[i].key;
                if (!py_is_str(k)) continue;
                extern const char *intern_name(const char *s, size_t len);
                py_class_add_method(c, cls, intern_name(PY_PTR(k)->str.chars, PY_PTR(k)->str.len),
                                    d->entries[i].value);
            }
        }
        return cls;
    }
    VALUE v = argv[0];
    if (PY_IS_FIXNUM(v)) return c->TYPE_int;
    if (PY_IS_FLONUM(v)) return c->TYPE_float;
    if (v == PY_NONE)  return c->TYPE_NoneType;
    if (v == PY_TRUE || v == PY_FALSE) return c->TYPE_bool;
    if (py_is_bignum(v)) return c->TYPE_int;
    struct pyobj *o = PY_PTR(v);
    switch (o->type) {
      case PY_T_FLOAT: return c->TYPE_float;
      case PY_T_STR:   return c->TYPE_str;
      case PY_T_BYTES: return c->TYPE_bytes;
      case PY_T_BYTEARRAY: return c->TYPE_bytearray;
      case PY_T_LIST:  return c->TYPE_list;
      case PY_T_TUPLE: return c->TYPE_tuple;
      case PY_T_DICT:  return c->TYPE_dict;
      case PY_T_SET:   return c->TYPE_set;
      case PY_T_FROZENSET: return c->TYPE_frozenset;
      case PY_T_RANGE: return c->TYPE_range;
      case PY_T_COMPLEX: return c->TYPE_complex;
      case PY_T_FUNC: return c->TYPE_function;
      case PY_T_BUILTIN: return c->TYPE_builtin_function_or_method;
      case PY_T_BOUND_METHOD: return c->TYPE_method;
      case PY_T_MODULE: return c->TYPE_module;
      case PY_T_SLICE: return c->TYPE_slice;
      case PY_T_ELLIPSIS: return c->TYPE_ellipsis;
      case PY_T_NOTIMPL: return c->TYPE_NotImplementedType;
      case PY_T_MEMVIEW: return c->TYPE_memoryview;
      case PY_T_GEN:   return c->TYPE_generator;
      case PY_T_PROPERTY: return c->TYPE_property;
      case PY_T_STATICMETHOD: return c->TYPE_staticmethod;
      case PY_T_CLASSMETHOD: return c->TYPE_classmethod;
      case PY_T_SUPER: return c->TYPE_super;
      case PY_T_CLASS: return c->TYPE_type;
      case PY_T_INSTANCE: return PY_OBJ_VAL(o->inst.cls);
      case PY_T_FILE:  return c->TYPE_object;
      default: return c->TYPE_object;
    }
}

static VALUE
bi_id(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    int64_t id = PY_IS_PTR(v) ? (int64_t)(uintptr_t)PY_PTR(v) : (int64_t)v;
    // Return as a (potentially big) int — the high bit is fine for fixnum.
    return py_make_int(id);
}

static VALUE
bi_dir(CTX *c, int argc, VALUE *argv)
{
    // User-defined __dir__ override.
    if (argc == 1 && py_is_instance(argv[0])) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "__dir__");
        if (m != PY_NONE) {
            VALUE result = py_apply(c, m, 1, argv);
            if (c->state == PY_STATE_RAISE) return PY_NONE;
            if (py_is_list(result)) {
                VALUE av[1] = { result };
                lm_sort(c, 1, av);
            }
            return result;
        }
    }
    VALUE r = py_make_list(NULL, 0);
    if (argc == 0) {
        // dir() with no args: list current frame's local names.  We
        // don't track param-names per pyframe well, so return globals.
        struct pyglobals *g = c->globals;
        for (size_t i = 0; i < g->size; i++)
            if (g->entries[i].defined)
                py_list_append(c, r, py_make_str(g->entries[i].name, strlen(g->entries[i].name)));
    } else {
        VALUE v = argv[0];
        if (py_is_module(v)) {
            struct pyglobals *g = PY_PTR(v)->module.globals;
            for (size_t i = 0; i < g->size; i++)
                if (g->entries[i].defined)
                    py_list_append(c, r, py_make_str(g->entries[i].name, strlen(g->entries[i].name)));
        } else if (py_is_class(v)) {
            // Walk MRO; collect unique method names.
            struct pyclass *cd = &PY_PTR(v)->cls;
            for (int j = 0; j < cd->nmro; j++) {
                struct pyclass *kd = &PY_PTR(cd->mro[j])->cls;
                for (int i = 0; i < kd->nmethods; i++) {
                    const char *nm = kd->methods[i].name;
                    bool dup = false;
                    size_t rl = PY_PTR(r)->list.len;
                    for (size_t k = 0; k < rl; k++) {
                        VALUE existing = PY_PTR(r)->list.items[k];
                        if (py_is_str(existing) &&
                            strcmp(PY_PTR(existing)->str.chars, nm) == 0) {
                            dup = true; break;
                        }
                    }
                    if (!dup) py_list_append(c, r, py_make_str(nm, strlen(nm)));
                }
            }
        } else if (py_is_instance(v)) {
            struct pyobj *o = PY_PTR(v);
            if (o->inst.attrs) {
                struct pydict *d = o->inst.attrs;
                for (size_t i = 0; i < d->elen; i++)
                    if (pydict_entry_live(d, i) && py_is_str(d->entries[i].key))
                        py_list_append(c, r, d->entries[i].key);
            }
            // Plus class methods (walk MRO).
            struct pyclass *cd = &PY_PTR(o->inst.cls)->cls;
            for (int j = 0; j < cd->nmro; j++) {
                struct pyclass *kd = &PY_PTR(cd->mro[j])->cls;
                for (int i = 0; i < kd->nmethods; i++) {
                    const char *nm = kd->methods[i].name;
                    bool dup = false;
                    size_t rl = PY_PTR(r)->list.len;
                    for (size_t k = 0; k < rl; k++) {
                        VALUE existing = PY_PTR(r)->list.items[k];
                        if (py_is_str(existing) &&
                            strcmp(PY_PTR(existing)->str.chars, nm) == 0) {
                            dup = true; break;
                        }
                    }
                    if (!dup) py_list_append(c, r, py_make_str(nm, strlen(nm)));
                }
            }
        }
    }
    // Sort the result.
    VALUE av[1] = { r };
    lm_sort(c, 1, av);
    return r;
}

// Materialise CTX globals as a dict (read-only-ish copy).
static VALUE
bi_globals(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    VALUE d = py_make_dict();
    struct pyglobals *g = c->globals;
    for (size_t i = 0; i < g->size; i++) {
        if (g->entries[i].defined) {
            VALUE k = py_make_str(g->entries[i].name, strlen(g->entries[i].name));
            py_dict_set(c, d, k, g->entries[i].value);
        }
    }
    return d;
}

static VALUE
bi_locals(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    // pystro doesn't track names-per-slot, so locals() returns an empty
    // dict at function scope.  At module scope, fall back to globals.
    if (!c->env) return bi_globals(c, 0, NULL);
    return py_make_dict();
}

static VALUE
bi_vars(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return bi_locals(c, 0, NULL);
    VALUE v = argv[0];
    if (py_is_module(v)) {
        struct pyglobals *saved = c->globals;
        c->globals = PY_PTR(v)->module.globals;
        VALUE r = bi_globals(c, 0, NULL);
        c->globals = saved;
        return r;
    }
    if (py_is_instance(v)) {
        VALUE d = py_make_dict();
        struct pyobj *o = PY_PTR(v);
        if (o->inst.attrs) {
            struct pydict *src = o->inst.attrs;
            for (size_t i = 0; i < src->elen; i++)
                if (pydict_entry_live(src, i))
                    py_dict_set(c, d, src->entries[i].key, src->entries[i].value);
        }
        return d;
    }
    if (py_is_class(v)) {
        // Return a dict of {name: value} for all methods on the class.
        VALUE d = py_make_dict();
        struct pyclass *cd = &PY_PTR(v)->cls;
        for (int i = 0; i < cd->nmethods; i++) {
            py_dict_set(c, d,
                py_make_str(cd->methods[i].name, strlen(cd->methods[i].name)),
                cd->methods[i].value);
        }
        return d;
    }
    py_raise_exc(c, c->EXC_TypeError, "vars() argument must be a module or instance");
}

static VALUE
bi_hasattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "hasattr name must be str");
    // Copy out of slice-borrow into NUL-terminated buf.
    size_t L = PY_PTR(argv[1])->str.len;
    char *namebuf = (char *)GC_malloc_atomic(L + 1);
    memcpy(namebuf, PY_PTR(argv[1])->str.chars, L);
    namebuf[L] = '\0';
    const char *name = namebuf;
    // Save state to detect AttributeError.
    int saved_state = c->state;
    VALUE saved_value = c->state_value;
    int saved_top = c->try_top;
    jmp_buf jb;
    if (c->try_top < 64) c->try_stack[c->try_top++] = &jb;
    bool ok = true;
    if (setjmp(jb) == 0) {
        py_getattr(c, argv[0], name);
        if (c->state == PY_STATE_RAISE) ok = false;
    } else {
        ok = false;
    }
    c->try_top = saved_top;
    c->state = saved_state;
    c->state_value = saved_value;
    return ok ? PY_TRUE : PY_FALSE;
}

static VALUE
bi_getattr(CTX *c, int argc, VALUE *argv)
{
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "getattr name must be str");
    size_t L = PY_PTR(argv[1])->str.len;
    char *namebuf = (char *)GC_malloc_atomic(L + 1);
    memcpy(namebuf, PY_PTR(argv[1])->str.chars, L);
    namebuf[L] = '\0';
    const char *name = namebuf;
    if (argc < 3) return py_getattr(c, argv[0], name);
    // With default: try, fall back on AttributeError.
    int saved_state = c->state;
    VALUE saved_value = c->state_value;
    int saved_top = c->try_top;
    jmp_buf jb;
    if (c->try_top < 64) c->try_stack[c->try_top++] = &jb;
    VALUE r = PY_NONE;
    bool ok = false;
    if (setjmp(jb) == 0) {
        r = py_getattr(c, argv[0], name);
        if (c->state != PY_STATE_RAISE) ok = true;
    }
    c->try_top = saved_top;
    if (!ok) {
        c->state = saved_state; c->state_value = saved_value;
        return argv[2];
    }
    return r;
}

static VALUE
bi_setattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "setattr name must be str");
    // String may be slice-borrowed (no NUL terminator within bounds);
    // copy to a small heap buffer so strlen sees the right length.
    size_t L = PY_PTR(argv[1])->str.len;
    char *buf = (char *)GC_malloc_atomic(L + 1);
    memcpy(buf, PY_PTR(argv[1])->str.chars, L);
    buf[L] = '\0';
    py_setattr(c, argv[0], buf, argv[2]);
    return PY_NONE;
}

// File I/O — `open(path, mode)` returns a PY_T_FILE.  Supports the
// usual context-manager protocol (`with open(...) as f:` works because
// __enter__/__exit__ are class methods).
static VALUE
bi_open(CTX *c, int argc, VALUE *argv)
{
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "open: path must be str");
    const char *modestr = "r";
    if (argc >= 2) {
        if (!py_is_str(argv[1])) py_raise_exc(c, c->EXC_TypeError, "open: mode must be str");
        modestr = PY_PTR(argv[1])->str.chars;
    }
    bool binary = strchr(modestr, 'b') != NULL;
    char libcmode[8] = {0};
    int mi = 0;
    for (const char *p = modestr; *p && mi < 7; p++)
        if (*p != 't') libcmode[mi++] = *p;
    libcmode[mi] = '\0';
    size_t L = PY_PTR(argv[0])->str.len;
    char *pbuf = (char *)alloca(L + 1);
    memcpy(pbuf, PY_PTR(argv[0])->str.chars, L); pbuf[L] = '\0';
    FILE *fp = fopen(pbuf, libcmode);
    if (!fp) py_raise_exc(c, c->EXC_RuntimeError, "open: cannot open '%s'", pbuf);
    struct pyobj *o = py_alloc(PY_T_FILE);
    o->file.fp = fp;
    o->file.path = (char *)GC_malloc_atomic(L + 1);
    memcpy(o->file.path, pbuf, L + 1);
    o->file.binary = binary;
    o->file.closed = false;
    return PY_OBJ_VAL(o);
}

static VALUE
fm_read(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->type != PY_T_FILE || o->file.closed) py_raise_exc(c, c->EXC_RuntimeError, "read on closed file");
    FILE *fp = (FILE *)o->file.fp;
    long limit = -1;
    if (argc >= 2) limit = (long)py_int_to_long(c, argv[1]);
    size_t cap = 4096, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    while (limit < 0 || (long)len < limit) {
        size_t want = (limit < 0) ? (cap - len) : ((size_t)limit - len);
        if (want == 0) break;
        if (len + want > cap) {
            cap = (len + want) * 2;
            char *nb = (char *)GC_malloc_atomic(cap);
            memcpy(nb, buf, len); buf = nb;
        }
        size_t n = fread(buf + len, 1, want, fp);
        if (n == 0) break;
        len += n;
    }
    if (o->file.binary) {
        struct pyobj *bo = py_alloc(PY_T_BYTES);
        bo->str.chars = buf;
        bo->str.len = len;
        return PY_OBJ_VAL(bo);
    }
    return py_make_str(buf, len);
}

VALUE
fm_readline(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->type != PY_T_FILE || o->file.closed) py_raise_exc(c, c->EXC_RuntimeError, "readline on closed file");
    FILE *fp = (FILE *)o->file.fp;
    size_t cap = 256, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)GC_malloc_atomic(cap);
            memcpy(nb, buf, len); buf = nb;
        }
        buf[len++] = (char)ch;
        if (ch == '\n') break;
    }
    return py_make_str(buf, len);
}

static VALUE
fm_readlines(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE r = py_make_list(NULL, 0);
    for (;;) {
        VALUE line = fm_readline(c, 1, argv);
        if (!py_is_str(line) || PY_PTR(line)->str.len == 0) break;
        py_list_append(c, r, line);
    }
    return r;
}

static VALUE
fm_write(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->type != PY_T_FILE || o->file.closed) py_raise_exc(c, c->EXC_RuntimeError, "write on closed file");
    FILE *fp = (FILE *)o->file.fp;
    VALUE v = argv[1];
    const char *src; size_t L;
    if (py_is_str(v) || py_is_byteseq(v)) {
        src = PY_PTR(v)->str.chars;
        L = PY_PTR(v)->str.len;
    } else {
        VALUE s = py_to_str(c, v);
        src = PY_PTR(s)->str.chars;
        L = PY_PTR(s)->str.len;
    }
    fwrite(src, 1, L, fp);
    return py_make_int((int64_t)L);
}

static VALUE
fm_close(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->type == PY_T_FILE && !o->file.closed) {
        fclose((FILE *)o->file.fp);
        o->file.closed = true;
    }
    return PY_NONE;
}

static VALUE
fm_enter(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    return argv[0];
}
static VALUE
fm_exit(CTX *c, int argc, VALUE *argv)
{
    return fm_close(c, argc, argv);
}

static VALUE
fm_flush(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->type == PY_T_FILE && !o->file.closed) fflush((FILE *)o->file.fp);
    return PY_NONE;
}

static VALUE
fm_tell(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->type != PY_T_FILE || o->file.closed)
        py_raise_exc(c, c->EXC_RuntimeError, "tell on closed file");
    return py_make_int((int64_t)ftell((FILE *)o->file.fp));
}

static VALUE
fm_seek(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->type != PY_T_FILE || o->file.closed)
        py_raise_exc(c, c->EXC_RuntimeError, "seek on closed file");
    int64_t off = py_int_to_long(c, argv[1]);
    int whence = (argc >= 3) ? (int)py_int_to_long(c, argv[2]) : 0;
    int sw = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
    if (fseek((FILE *)o->file.fp, off, sw) != 0)
        py_raise_exc(c, c->EXC_OSError, "seek failed");
    return py_make_int((int64_t)ftell((FILE *)o->file.fp));
}

static VALUE
fm_readable(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    return (o->type == PY_T_FILE && !o->file.closed) ? PY_TRUE : PY_FALSE;
}

static VALUE
fm_writable(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pyobj *o = PY_PTR(argv[0]);
    return (o->type == PY_T_FILE && !o->file.closed) ? PY_TRUE : PY_FALSE;
}

static VALUE
fm_seekable(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    return PY_TRUE;
}

static VALUE
fm_truncate(CTX *c, int argc, VALUE *argv)
{
    struct pyobj *o = PY_PTR(argv[0]);
    if (o->type != PY_T_FILE || o->file.closed)
        py_raise_exc(c, c->EXC_RuntimeError, "truncate on closed file");
    int64_t size = (argc >= 2) ? py_int_to_long(c, argv[1])
                               : (int64_t)ftell((FILE *)o->file.fp);
    int fd = fileno((FILE *)o->file.fp);
    extern int ftruncate(int fd, off_t length);
    if (ftruncate(fd, (off_t)size) != 0)
        py_raise_exc(c, c->EXC_OSError, "truncate failed");
    return py_make_int(size);
}

struct type_method file_methods[] = {
    { "read",       fm_read,      1, 2 },
    { "readline",   fm_readline,  1, 1 },
    { "readlines",  fm_readlines, 1, 1 },
    { "write",      fm_write,     2, 2 },
    { "close",      fm_close,     1, 1 },
    { "flush",      fm_flush,     1, 1 },
    { "tell",       fm_tell,      1, 1 },
    { "seek",       fm_seek,      2, 3 },
    { "readable",   fm_readable,  1, 1 },
    { "writable",   fm_writable,  1, 1 },
    { "seekable",   fm_seekable,  1, 1 },
    { "truncate",   fm_truncate,  1, 2 },
    { "__enter__",  fm_enter,     1, 1 },
    { "__exit__",   fm_exit,      4, 4 },
    { NULL, NULL, 0, 0 }
};

// Re-tokenize + parse expression for eval(); parse program for exec().
extern void   tokenize(const char *src, const char *filename);
extern struct Node *parse_program(void);
extern struct Node *parse_eval_expr(void);  // see parser.c

static VALUE
bi_eval(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "eval: code must be str");
    size_t L = PY_PTR(argv[0])->str.len;
    char *src = (char *)GC_malloc_atomic(L + 2);
    memcpy(src, PY_PTR(argv[0])->str.chars, L);
    src[L] = '\n'; src[L+1] = '\0';
    tokenize(src, "<eval>");
    NODE *expr = parse_eval_expr();
    return EVAL(c, expr);
}

static VALUE
bi_exec(CTX *c, int argc, VALUE *argv)
{
    // Accept exec(code [, globals [, locals]]); ignore the dict args (the
    // current implementation has no real namespace separation, so we just
    // execute in the current global scope).
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "exec: code must be str");
    size_t L = PY_PTR(argv[0])->str.len;
    char *src = (char *)GC_malloc_atomic(L + 2);
    memcpy(src, PY_PTR(argv[0])->str.chars, L);
    src[L] = '\n'; src[L+1] = '\0';
    tokenize(src, "<exec>");
    NODE *body = parse_program();
    EVAL(c, body);
    return PY_NONE;
}

static VALUE bi_pystro_delattr(CTX *c, int argc, VALUE *argv);  // fwd
static VALUE
bi_delattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    return bi_pystro_delattr(c, 2, argv);
}

static VALUE
bi_callable(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    if (py_is_func(v) || py_is_builtin(v) || py_is_bound(v) || py_is_class(v)) return PY_TRUE;
    if (py_is_instance(v)) {
        VALUE call = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__call__");
        return call != PY_NONE ? PY_TRUE : PY_FALSE;
    }
    return PY_FALSE;
}

// issubclass(cls, classinfo) — classinfo can be a class or tuple of classes.
static VALUE
bi_issubclass(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE cls = argv[0], info = argv[1];
    if (py_is_tuple(info)) {
        size_t n = PY_PTR(info)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE av[2] = { cls, PY_PTR(info)->list.items[i] };
            if (bi_issubclass(c, 2, av) == PY_TRUE) return PY_TRUE;
        }
        return PY_FALSE;
    }
    if (!py_is_class(cls)) py_raise_exc(c, c->EXC_TypeError, "issubclass() arg 1 must be a class");
    if (!py_is_class(info)) py_raise_exc(c, c->EXC_TypeError, "issubclass() arg 2 must be a class");
    return class_is_ancestor(cls, info) ? PY_TRUE : PY_FALSE;
}

// Match `v`'s Python type against a class.  Handles both built-in
// type classes (matched via cls.builtin_tag) and user classes
// (matched via instance.cls's MRO).
static bool
py_isinstance_check(CTX *c, VALUE v, VALUE cls)
{
    if (!py_is_class(cls)) {
        py_raise_exc(c, c->EXC_TypeError, "isinstance() second arg must be class");
    }
    struct pyclass *cd = &PY_PTR(cls)->cls;
    // Quick: object accepts everything.
    if (cls == c->TYPE_object) return true;
    // Built-in type class — check value's tag.
    // First find the value's "type class" and walk its MRO.
    {
        VALUE av[1] = { v };
        VALUE vtype = bi_type(c, 1, av);
        if (py_is_class(vtype) && vtype != cls) {
            // Walk MRO.
            struct pyclass *vd = &PY_PTR(vtype)->cls;
            for (int i = 0; i < vd->nmro; i++) if (vd->mro[i] == cls) return true;
        } else if (vtype == cls) {
            return true;
        }
    }
    if (cd->builtin_ctor) {
        const char *nm = cd->name;
        if (strcmp(nm, "int")        == 0) return py_is_int(v) || v == PY_TRUE || v == PY_FALSE;
        if (strcmp(nm, "float")      == 0) return py_is_float(v);
        if (strcmp(nm, "str")        == 0) return py_is_str(v);
        if (strcmp(nm, "bool")       == 0) return (v == PY_TRUE || v == PY_FALSE);
        if (strcmp(nm, "list")       == 0) return py_is_list(v);
        if (strcmp(nm, "tuple")      == 0) return py_is_tuple(v);
        if (strcmp(nm, "dict")       == 0) return py_is_dict(v);
        if (strcmp(nm, "set")        == 0) return py_is_set(v);
        if (strcmp(nm, "frozenset")  == 0) return py_is_frozenset(v);
        if (strcmp(nm, "bytes")      == 0) return py_is_bytes(v);
        if (strcmp(nm, "bytearray")  == 0) return py_is_bytearray(v);
        if (strcmp(nm, "range")      == 0) return py_is_range(v);
        if (strcmp(nm, "complex")    == 0) return py_is_complex(v);
        if (strcmp(nm, "type")       == 0) return py_is_class(v);
        return false;
    }
    // User class — `v` must be an instance whose class has cls in MRO.
    if (!py_is_instance(v)) return false;
    return class_is_ancestor(PY_OBJ_VAL(PY_PTR(v)->inst.cls), cls);
}

static VALUE
bi_isinstance(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0], cls = argv[1];
    if (py_is_tuple(cls)) {
        size_t n = PY_PTR(cls)->list.len;
        for (size_t i = 0; i < n; i++) {
            if (py_isinstance_check(c, v, PY_PTR(cls)->list.items[i])) return PY_TRUE;
        }
        return PY_FALSE;
    }
    return py_isinstance_check(c, v, cls) ? PY_TRUE : PY_FALSE;
}

static VALUE
bi_min(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) py_raise_exc(c, c->EXC_TypeError, "min() needs args");
    VALUE key_fn = pystro_bi_kwarg("key");
    VALUE deflt  = pystro_bi_kwarg("default");
    VALUE best = PY_NONE, best_key = PY_NONE;
    bool started = false;
    if (argc == 1) {
        struct py_iter it; py_iter_init(c, &it, argv[0]);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
        VALUE x;
        while (py_iter_next(c, &it, &x)) {
            VALUE xk = key_fn ? py_apply(c, key_fn, 1, &x) : x;
            if (!started || py_cmp(c, xk, best_key) < 0) {
                best = x; best_key = xk; started = true;
            }
        }
        if (!started) {
            if (deflt) return deflt;
            py_raise_exc(c, c->EXC_ValueError, "min() empty");
        }
        return best;
    }
    for (int i = 0; i < argc; i++) {
        VALUE xk = key_fn ? py_apply(c, key_fn, 1, &argv[i]) : argv[i];
        if (!started || py_cmp(c, xk, best_key) < 0) {
            best = argv[i]; best_key = xk; started = true;
        }
    }
    return best;
}

static VALUE
bi_max(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) py_raise_exc(c, c->EXC_TypeError, "max() needs args");
    VALUE key_fn = pystro_bi_kwarg("key");
    VALUE deflt  = pystro_bi_kwarg("default");
    VALUE best = PY_NONE, best_key = PY_NONE;
    bool started = false;
    if (argc == 1) {
        struct py_iter it; py_iter_init(c, &it, argv[0]);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
        VALUE x;
        while (py_iter_next(c, &it, &x)) {
            VALUE xk = key_fn ? py_apply(c, key_fn, 1, &x) : x;
            if (!started || py_cmp(c, xk, best_key) > 0) {
                best = x; best_key = xk; started = true;
            }
        }
        if (!started) {
            if (deflt) return deflt;
            py_raise_exc(c, c->EXC_ValueError, "max() empty");
        }
        return best;
    }
    for (int i = 0; i < argc; i++) {
        VALUE xk = key_fn ? py_apply(c, key_fn, 1, &argv[i]) : argv[i];
        if (!started || py_cmp(c, xk, best_key) > 0) {
            best = argv[i]; best_key = xk; started = true;
        }
    }
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
    VALUE start_v = pystro_bi_kwarg("start");
    int64_t i = start_v ? py_int_to_long(c, start_v)
                        : (argc >= 2 ? py_int_to_long(c, argv[1]) : 0);
    struct pyobj *o = py_alloc(PY_T_ITER);
    o->iter_state = (struct py_iter *)GC_malloc(sizeof(struct py_iter));
    o->iter_state->kind = 8;
    o->iter_state->i = i;
    o->iter_state->inner = (struct py_iter *)GC_malloc(sizeof(struct py_iter));
    py_iter_init(c, &o->iter_state->inner[0], argv[0]);
    o->iter_state->n_inner = 1;
    return PY_OBJ_VAL(o);
}

static VALUE
bi_zip(CTX *c, int argc, VALUE *argv)
{
    VALUE strict = pystro_bi_kwarg("strict");
    struct pyobj *o = py_alloc(PY_T_ITER);
    o->iter_state = (struct py_iter *)GC_malloc(sizeof(struct py_iter));
    o->iter_state->kind = 9;
    o->iter_state->n_inner = argc;
    o->iter_state->i = (strict == PY_TRUE) ? 1 : 0;  // strict flag
    if (argc == 0) {
        o->iter_state->inner = NULL;
        return PY_OBJ_VAL(o);
    }
    o->iter_state->inner = (struct py_iter *)GC_malloc(sizeof(struct py_iter) * argc);
    for (int i = 0; i < argc; i++) {
        py_iter_init(c, &o->iter_state->inner[i], argv[i]);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
    }
    return PY_OBJ_VAL(o);
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
    VALUE v = argv[0];
    // Coerce via __index__ if available.
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__index__");
        if (m != PY_NONE) {
            VALUE av[1] = { v };
            v = py_apply(c, m, 1, av);
            if (c->state == PY_STATE_RAISE) return PY_NONE;
        }
    }
    if (!py_int_or_bool(v)) py_raise_exc(c, c->EXC_TypeError, "hex() needs int");
    mpz_t z; py_to_mpz(c, v, z);
    char *s = mpz_get_str(NULL, 16, z);
    char *r;
    int an = (s[0] == '-') ? asprintf(&r, "-0x%s", s + 1) : asprintf(&r, "0x%s", s);
    (void)an;
    VALUE rv = py_make_str(r, strlen(r));
    free(r); mpz_clear(z); return rv;
}

static VALUE
bi_bin(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__index__");
        if (m != PY_NONE) { VALUE av[1] = { v }; v = py_apply(c, m, 1, av);
            if (c->state == PY_STATE_RAISE) return PY_NONE; }
    }
    if (!py_int_or_bool(v)) py_raise_exc(c, c->EXC_TypeError, "bin() needs int");
    mpz_t z; py_to_mpz(c, v, z);
    char *s = mpz_get_str(NULL, 2, z);
    char *r;
    int an = (s[0] == '-') ? asprintf(&r, "-0b%s", s + 1) : asprintf(&r, "0b%s", s);
    (void)an;
    VALUE rv = py_make_str(r, strlen(r));
    free(r); mpz_clear(z); return rv;
}

static VALUE
bi_oct(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__index__");
        if (m != PY_NONE) { VALUE av[1] = { v }; v = py_apply(c, m, 1, av);
            if (c->state == PY_STATE_RAISE) return PY_NONE; }
    }
    if (!py_int_or_bool(v)) py_raise_exc(c, c->EXC_TypeError, "oct() needs int");
    mpz_t z; py_to_mpz(c, v, z);
    char *s = mpz_get_str(NULL, 8, z);
    char *r;
    int an = (s[0] == '-') ? asprintf(&r, "-0o%s", s + 1) : asprintf(&r, "0o%s", s);
    (void)an;
    VALUE rv = py_make_str(r, strlen(r));
    free(r); mpz_clear(z); return rv;
}

// slice(stop) / slice(start, stop) / slice(start, stop, step)
static VALUE
bi_slice(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pyobj *o = py_alloc(PY_T_SLICE);
    if (argc == 1) {
        o->slice_.start = PY_NONE;
        o->slice_.stop  = argv[0];
        o->slice_.step  = PY_NONE;
    } else if (argc == 2) {
        o->slice_.start = argv[0];
        o->slice_.stop  = argv[1];
        o->slice_.step  = PY_NONE;
    } else {
        o->slice_.start = argv[0];
        o->slice_.stop  = argv[1];
        o->slice_.step  = argv[2];
    }
    return PY_OBJ_VAL(o);
}

static VALUE
bi_memoryview(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    if (!(py_is_bytes(v) || py_is_bytearray(v)))
        py_raise_exc(c, c->EXC_TypeError, "memoryview: bytes-like required");
    struct pyobj *o = py_alloc(PY_T_MEMVIEW);
    o->memview.source = v;
    o->memview.off = 0;
    o->memview.len = PY_PTR(v)->str.len;
    return PY_OBJ_VAL(o);
}

static VALUE
bi_breakpoint(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    // No-op stub; pystro has no debugger.  Honour PYTHONBREAKPOINT=0.
    return PY_NONE;
}

// `compile(source, filename, mode)` — pystro doesn't expose a syntax
// tree object; return the source string for `exec` / `eval` to re-tokenize.
static VALUE
bi_compile(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    return argv[0];     // pass-through; eval/exec accept str directly
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
    // User class with __format__ — dispatch to it.
    if (py_is_instance(v)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(v)->inst.cls), "__format__");
        if (m != PY_NONE) {
            VALUE spec = (argc >= 2 && py_is_str(argv[1])) ? argv[1] : py_make_str("", 0);
            VALUE av[2] = { v, spec };
            return py_apply(c, m, 2, av);
        }
    }
    if (argc < 2 || !py_is_str(argv[1]) || PY_PTR(argv[1])->str.len == 0)
        return py_to_str(c, v);
    const char *s = PY_PTR(argv[1])->str.chars;
    size_t n = PY_PTR(argv[1])->str.len;
    char fill = ' ';
    char align = 0;            // 0 = unset, '<', '>', '^'
    bool zero_pad = false;
    bool alt_form = false;
    bool comma_sep = false;
    char group_ch = 0;     // ',' or '_'
    char sign_ch = 0;
    int  width = 0;
    int  precision = -1;
    char type_ch = 0;
    size_t i = 0;
    // [fill][align]
    if (n >= 2 && (s[1] == '<' || s[1] == '>' || s[1] == '^' || s[1] == '=')) {
        fill = s[0]; align = s[1]; i = 2;
    } else if (n >= 1 && (s[0] == '<' || s[0] == '>' || s[0] == '^' || s[0] == '=')) {
        align = s[0]; i = 1;
    }
    // [sign]
    if (i < n && (s[i] == '+' || s[i] == '-' || s[i] == ' ')) {
        sign_ch = s[i++];
    }
    // [#]
    if (i < n && s[i] == '#') { alt_form = true; i++; }
    // [0]
    if (i < n && s[i] == '0') { zero_pad = true; i++; }
    // [width]
    while (i < n && s[i] >= '0' && s[i] <= '9') { width = width * 10 + (s[i] - '0'); i++; }
    // [, or _]
    if (i < n && (s[i] == ',' || s[i] == '_')) {
        comma_sep = true; group_ch = s[i]; i++;
    }
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
        // For negative values, %llo / %llx interpret as unsigned, which
        // is wrong for Python (Python uses '-' sign).  Format the
        // absolute value and prepend '-' if needed.
        bool was_neg = iv < 0;
        unsigned long long uv = was_neg ? (unsigned long long)(-iv) : (unsigned long long)iv;
        const char *abs_fmt = type_ch == 'd' ? "%llu" :
                              type_ch == 'b' ? "%llu" :     // handled below
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
        } else {
            char tmp[200];
            snprintf(tmp, sizeof(tmp), abs_fmt, uv);
            if (was_neg) snprintf(body, sizeof(body), "-%s", tmp);
            else         snprintf(body, sizeof(body), "%s", tmp);
        }
    } else if (type_ch == 'f' || type_ch == 'g' || type_ch == 'e' || type_ch == 'E') {
        double d = py_to_double(c, v);
        if (precision >= 0) snprintf(fmt, sizeof(fmt), "%%.%d%c", precision, type_ch);
        else                snprintf(fmt, sizeof(fmt), "%%%c", type_ch);
        snprintf(body, sizeof(body), fmt, d);
    } else if (type_ch == '%') {
        double d = py_to_double(c, v) * 100.0;
        if (precision >= 0) snprintf(fmt, sizeof(fmt), "%%.%df%%%%", precision);
        else                snprintf(fmt, sizeof(fmt), "%%f%%%%");
        snprintf(body, sizeof(body), fmt, d);
    } else if (type_ch == 's' || type_ch == 0) {
        // No type AND numeric value with sign/width/precision/comma → use 'd'/'g' path.
        if (type_ch == 0 && (PY_IS_FIXNUM(v) || py_is_bignum(v) || v == PY_TRUE || v == PY_FALSE)
            && (sign_ch != 0 || comma_sep)) {
            // Render as decimal int.
            long long iv = PY_IS_FIXNUM(v) ? PY_FIXVAL(v)
                          : v == PY_TRUE ? 1 : v == PY_FALSE ? 0 : 0;
            if (py_is_bignum(v)) {
                char *bs = mpz_get_str(NULL, 10, PY_PTR(v)->mpz);
                snprintf(body, sizeof(body), "%s", bs);
            } else {
                snprintf(body, sizeof(body), "%lld", iv);
            }
            // sign handled in pad path.
        } else if (type_ch == 0 && py_is_float(v) && (sign_ch != 0 || comma_sep)) {
            double d = py_to_double(c, v);
            int prec = precision < 0 ? 6 : precision;
            snprintf(body, sizeof(body), "%.*g", prec, d);
        } else {
            VALUE sv = py_to_str(c, v);
            if (py_is_str(sv)) {
                size_t L = PY_PTR(sv)->str.len;
                if (L >= sizeof(body)) L = sizeof(body) - 1;
                memcpy(body, PY_PTR(sv)->str.chars, L);
                body[L] = '\0';
                if (precision >= 0 && (size_t)precision < L) body[precision] = '\0';
            }
        }
    } else {
        return py_to_str(c, v);    // unknown type — fall back
    }
  pad: {
        size_t bl = strlen(body);
        // [#] alternate form: prefix with 0x/0o/0b for x/o/b types.
        if (alt_form && (type_ch == 'x' || type_ch == 'X' || type_ch == 'o' || type_ch == 'b')) {
            char prefix[4] = "0\0\0\0";
            prefix[1] = (type_ch == 'X') ? 'X' : type_ch;
            char tmp[260];
            int neg = (body[0] == '-');
            if (neg) {
                snprintf(tmp, sizeof(tmp), "-%s%s", prefix, body + 1);
            } else {
                snprintf(tmp, sizeof(tmp), "%s%s", prefix, body);
            }
            strncpy(body, tmp, sizeof(body) - 1);
            body[sizeof(body) - 1] = '\0';
            bl = strlen(body);
        }
        // [,] thousands separator for d type, float types, or for int values with no type.
        bool int_like = (type_ch == 'd') ||
                        (type_ch == 0 && (PY_IS_FIXNUM(v) || py_is_bignum(v) ||
                                          v == PY_TRUE || v == PY_FALSE));
        bool float_like = (type_ch == 'f' || type_ch == 'g' || type_ch == 'e'
                           || type_ch == 'E' || type_ch == 'F'
                           || (type_ch == 0 && py_is_float(v)));
        if (comma_sep && (int_like || float_like)) {
            int neg = (body[0] == '-');
            int start = neg ? 1 : 0;
            // For float, find decimal point or exponent — only group digits before it.
            int int_end = (int)bl;
            for (int k = start; k < (int)bl; k++) {
                if (body[k] == '.' || body[k] == 'e' || body[k] == 'E') {
                    int_end = k; break;
                }
            }
            int dl = int_end - start;
            int commas = (dl - 1) / 3;
            if (commas > 0) {
                char tmp[260];
                int ti = 0;
                if (neg) tmp[ti++] = '-';
                int j = start;
                int first = dl % 3;
                if (first == 0) first = 3;
                for (int k = 0; k < first && j < int_end; k++) tmp[ti++] = body[j++];
                char gc = group_ch ? group_ch : ',';
                while (j < int_end) {
                    tmp[ti++] = gc;
                    for (int k = 0; k < 3 && j < int_end; k++) tmp[ti++] = body[j++];
                }
                // Append the rest unchanged.
                while (j < (int)bl) tmp[ti++] = body[j++];
                tmp[ti] = '\0';
                strncpy(body, tmp, sizeof(body) - 1);
                body[sizeof(body) - 1] = '\0';
                bl = strlen(body);
            }
        }
        // [+] sign on positive numbers.
        bool is_numeric = (type_ch == 'd' || type_ch == 'f' || type_ch == 'g' ||
                          type_ch == 'e' || type_ch == 'E' ||
                          type_ch == 'x' || type_ch == 'X' || type_ch == 'o' || type_ch == 'b' ||
                          (type_ch == 0 && (PY_IS_FIXNUM(v) || py_is_bignum(v)
                                            || py_is_float(v) || v == PY_TRUE || v == PY_FALSE)));
        if (sign_ch == '+' && is_numeric && body[0] != '-') {
            char tmp[260];
            snprintf(tmp, sizeof(tmp), "+%s", body);
            strncpy(body, tmp, sizeof(body) - 1);
            body[sizeof(body) - 1] = '\0';
            bl = strlen(body);
        } else if (sign_ch == ' ' && is_numeric && body[0] != '-') {
            char tmp[260];
            snprintf(tmp, sizeof(tmp), " %s", body);
            strncpy(body, tmp, sizeof(body) - 1);
            body[sizeof(body) - 1] = '\0';
            bl = strlen(body);
        }
        if ((int)bl >= width) return py_make_str(body, bl);
        size_t pad = (size_t)width - bl;
        char *out = (char *)GC_malloc_atomic(width + 1);
        char eff_fill = zero_pad ? '0' : fill;
        if (align == 0) {
            // Default: numbers right-align, strings left-align.  For type_ch==0,
            // numeric values still right-align (matching CPython).
            bool numeric_v = PY_IS_FIXNUM(v) || py_is_bignum(v) || py_is_float(v)
                             || v == PY_TRUE || v == PY_FALSE;
            if (type_ch == 's') align = '<';
            else if (type_ch == 0) align = numeric_v ? '>' : '<';
            else align = '>';
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
            // For zero-pad: keep sign and any 0x/0o/0b prefix at the start,
            // pad zeros between them and the digits.
            int sign_skip = 0;
            if (zero_pad && bl > 0 && (body[0] == '-' || body[0] == '+' || body[0] == ' '))
                sign_skip = 1;
            int prefix_len = 0;
            if (zero_pad && (size_t)(sign_skip + 1) < bl && body[sign_skip] == '0'
                && (body[sign_skip + 1] == 'x' || body[sign_skip + 1] == 'X'
                    || body[sign_skip + 1] == 'o' || body[sign_skip + 1] == 'b')) {
                prefix_len = 2;
            }
            int head = sign_skip + prefix_len;
            for (int i = 0; i < head; i++) out[i] = body[i];
            for (size_t j = 0; j < pad; j++) out[head + j] = eff_fill;
            memcpy(out + head + pad, body + head, bl - head);
        }
        out[width] = '\0';
        return py_make_str_take(out, (size_t)width);
    }
}

// `"fmt" % args` — Python-style %-formatting.  Supports:
//   %d %i — int (signed decimal)
//   %u    — uint (decimal)
//   %x %X — hex (lower / upper)
//   %o    — octal
//   %b    — binary (Python doesn't have this, but bench/utility friendly)
//   %f %F %e %E %g %G — float
//   %s    — str()
//   %r    — repr()
//   %c    — single char from int or 1-char str
//   %%    — literal %
// Each spec accepts optional flags (-+0 #), width (digits or *), precision
// (.digits or .*).  `args` may be a tuple (multi-arg) or a single value.
VALUE
py_str_pct_format(CTX *c, VALUE fmt, VALUE args)
{
    const char *src = PY_PTR(fmt)->str.chars;
    size_t srclen = PY_PTR(fmt)->str.len;
    bool args_is_tuple = py_is_tuple(args);
    size_t nargs = args_is_tuple ? PY_PTR(args)->list.len : 1;
    size_t argi = 0;
    size_t out_capa = srclen + 16;
    char *out = (char *)GC_malloc_atomic(out_capa);
    size_t out_len = 0;
#define OUT_RESERVE(extra) do { \
    if (out_len + (extra) > out_capa) { \
        out_capa = (out_len + (extra)) * 2; \
        char *no = (char *)GC_malloc_atomic(out_capa); \
        memcpy(no, out, out_len); out = no; \
    } \
} while (0)
#define OUT_PUT(buf, len) do { OUT_RESERVE(len); memcpy(out + out_len, (buf), (len)); out_len += (len); } while (0)
#define OUT_CH(ch) do { OUT_RESERVE(1); out[out_len++] = (ch); } while (0)
    bool args_is_dict = py_is_dict(args);
    for (size_t i = 0; i < srclen; i++) {
        char ch = src[i];
        if (ch != '%') { OUT_CH(ch); continue; }
        i++;
        if (i >= srclen) py_raise_exc(c, c->EXC_ValueError, "incomplete format");
        // %(name)X — mapping key.
        VALUE key_arg = PY_NONE;
        if (src[i] == '(') {
            if (!args_is_dict)
                py_raise_exc(c, c->EXC_TypeError, "format requires a mapping");
            i++;
            size_t ks = i;
            int depth = 1;
            while (i < srclen && depth > 0) {
                if (src[i] == '(') depth++;
                else if (src[i] == ')') { depth--; if (depth == 0) break; }
                i++;
            }
            if (i >= srclen) py_raise_exc(c, c->EXC_ValueError, "unmatched '('");
            VALUE keystr = py_make_str(src + ks, i - ks);
            i++;  // skip ')'
            uint64_t kh = py_hash(c, keystr);
            int32_t kidx = pydict_find(c, PY_PTR(args)->dict, keystr, kh);
            if (kidx < 0) py_raise_exc(c, c->EXC_KeyError, "%s",
                                        PY_PTR(keystr)->str.chars);
            key_arg = PY_PTR(args)->dict->entries[kidx].value;
        }
        // Flags.
        bool flag_minus = false, flag_plus = false, flag_zero = false, flag_space = false, flag_hash = false;
        for (;; i++) {
            if (i >= srclen) py_raise_exc(c, c->EXC_ValueError, "incomplete format");
            char f = src[i];
            if      (f == '-') flag_minus = true;
            else if (f == '+') flag_plus  = true;
            else if (f == '0') flag_zero  = true;
            else if (f == ' ') flag_space = true;
            else if (f == '#') flag_hash  = true;
            else break;
        }
        // Width.
        int width = 0;
        if (src[i] == '*') {
            if (argi >= nargs) py_raise_exc(c, c->EXC_TypeError, "not enough args");
            VALUE wv = args_is_tuple ? PY_PTR(args)->list.items[argi++] : args;
            if (!args_is_tuple) argi++;
            width = (int)py_int_to_long(c, wv);
            i++;
        } else {
            while (i < srclen && src[i] >= '0' && src[i] <= '9') {
                width = width * 10 + (src[i] - '0'); i++;
            }
        }
        // Precision.
        int precision = -1;
        if (i < srclen && src[i] == '.') {
            i++;
            if (i < srclen && src[i] == '*') {
                if (argi >= nargs) py_raise_exc(c, c->EXC_TypeError, "not enough args");
                VALUE pv = args_is_tuple ? PY_PTR(args)->list.items[argi++] : args;
                if (!args_is_tuple) argi++;
                precision = (int)py_int_to_long(c, pv);
                i++;
            } else {
                precision = 0;
                while (i < srclen && src[i] >= '0' && src[i] <= '9') {
                    precision = precision * 10 + (src[i] - '0'); i++;
                }
            }
        }
        if (i >= srclen) py_raise_exc(c, c->EXC_ValueError, "incomplete format");
        char conv = src[i];
        if (conv == '%') { OUT_CH('%'); continue; }
        // Get next arg.
        VALUE arg;
        if (key_arg != PY_NONE || args_is_dict) {
            arg = key_arg;
        } else if (args_is_tuple) {
            if (argi >= nargs) py_raise_exc(c, c->EXC_TypeError, "not enough args");
            arg = PY_PTR(args)->list.items[argi++];
        } else {
            if (argi > 0) py_raise_exc(c, c->EXC_TypeError, "not all args converted");
            arg = args; argi++;
        }
        // Render `arg` into `body`.
        char body[512];
        body[0] = '\0';
        size_t bl = 0;
        if (conv == 'd' || conv == 'i' || conv == 'u') {
            if (py_is_bignum(arg)) {
                char *bs = mpz_get_str(NULL, 10, PY_PTR(arg)->mpz);
                bl = strlen(bs);
                if (bl >= sizeof(body)) bl = sizeof(body) - 1;
                memcpy(body, bs, bl); body[bl] = '\0';
            } else {
                long long iv = py_int_to_long(c, arg);
                if (flag_plus && iv >= 0)       bl = snprintf(body, sizeof(body), "+%lld", iv);
                else if (flag_space && iv >= 0) bl = snprintf(body, sizeof(body), " %lld", iv);
                else                            bl = snprintf(body, sizeof(body), "%lld", iv);
            }
        } else if (conv == 'x' || conv == 'X' || conv == 'o') {
            long long iv = py_int_to_long(c, arg);
            unsigned long long u = (unsigned long long)(iv < 0 ? -iv : iv);
            char numbuf[64];
            int nl;
            if (conv == 'x') nl = snprintf(numbuf, sizeof(numbuf), "%llx", u);
            else if (conv == 'X') nl = snprintf(numbuf, sizeof(numbuf), "%llX", u);
            else nl = snprintf(numbuf, sizeof(numbuf), "%llo", u);
            int j = 0;
            if (iv < 0) body[j++] = '-';
            if (flag_hash) {
                body[j++] = '0';
                body[j++] = (conv == 'X') ? 'X' : (conv == 'o' ? 'o' : 'x');
            }
            for (int k = 0; k < nl && j < (int)sizeof(body) - 1; k++) body[j++] = numbuf[k];
            body[j] = '\0'; bl = (size_t)j;
        } else if (conv == 'b') {
            long long iv = py_int_to_long(c, arg);
            unsigned long long u = (unsigned long long)(iv < 0 ? -iv : iv);
            char buf[80]; int p = 0;
            if (u == 0) buf[p++] = '0';
            while (u) { buf[p++] = (u & 1) + '0'; u >>= 1; }
            int j = 0;
            if (iv < 0) body[j++] = '-';
            for (int k = p - 1; k >= 0; k--) body[j++] = buf[k];
            body[j] = '\0'; bl = (size_t)j;
        } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
            double d = py_to_double(c, arg);
            char fmtb[16];
            int p = precision < 0 ? 6 : precision;
            snprintf(fmtb, sizeof(fmtb), "%%%s%s.%d%c",
                     flag_plus ? "+" : (flag_space ? " " : ""),
                     flag_hash ? "#" : "", p, conv);
            bl = snprintf(body, sizeof(body), fmtb, d);
        } else if (conv == 's') {
            VALUE sv = py_to_str(c, arg);
            if (!py_is_str(sv)) sv = py_make_str("?", 1);
            bl = PY_PTR(sv)->str.len;
            if (precision >= 0 && (size_t)precision < bl) bl = (size_t)precision;
            if (bl >= sizeof(body)) bl = sizeof(body) - 1;
            memcpy(body, PY_PTR(sv)->str.chars, bl);
            body[bl] = '\0';
        } else if (conv == 'r' || conv == 'a') {
            VALUE sv = py_to_repr(c, arg);
            if (!py_is_str(sv)) sv = py_make_str("?", 1);
            bl = PY_PTR(sv)->str.len;
            if (precision >= 0 && (size_t)precision < bl) bl = (size_t)precision;
            if (bl >= sizeof(body)) bl = sizeof(body) - 1;
            memcpy(body, PY_PTR(sv)->str.chars, bl);
            body[bl] = '\0';
        } else if (conv == 'c') {
            if (PY_IS_FIXNUM(arg)) {
                int cv = (int)PY_FIXVAL(arg);
                body[0] = (char)cv; body[1] = '\0'; bl = 1;
            } else if (py_is_str(arg) && PY_PTR(arg)->str.len == 1) {
                body[0] = PY_PTR(arg)->str.chars[0]; body[1] = '\0'; bl = 1;
            } else py_raise_exc(c, c->EXC_TypeError, "%%c needs int or 1-char str");
        } else {
            py_raise_exc(c, c->EXC_ValueError, "unsupported format character '%c'", conv);
        }
        // Pad.
        if ((int)bl < width) {
            size_t pad = (size_t)width - bl;
            char eff_fill = (flag_zero && !flag_minus
                             && (conv == 'd' || conv == 'i' || conv == 'u'
                                 || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'b'
                                 || conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E'
                                 || conv == 'g' || conv == 'G')) ? '0' : ' ';
            if (flag_minus) {
                OUT_PUT(body, bl);
                for (size_t j = 0; j < pad; j++) OUT_CH(eff_fill);
            } else {
                // For zero-pad of signed numbers, keep sign in front.
                if (eff_fill == '0' && bl > 0 && (body[0] == '-' || body[0] == '+' || body[0] == ' ')) {
                    OUT_CH(body[0]);
                    for (size_t j = 0; j < pad; j++) OUT_CH('0');
                    OUT_PUT(body + 1, bl - 1);
                } else {
                    for (size_t j = 0; j < pad; j++) OUT_CH(eff_fill);
                    OUT_PUT(body, bl);
                }
            }
        } else {
            OUT_PUT(body, bl);
        }
    }
    if (args_is_tuple && argi < nargs)
        py_raise_exc(c, c->EXC_TypeError, "not all arguments converted");
    return py_make_str(out, out_len);
#undef OUT_RESERVE
#undef OUT_PUT
#undef OUT_CH
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
    o->wrap.setter  = PY_NONE;
    o->wrap.deleter = PY_NONE;
    return PY_OBJ_VAL(o);
}

static VALUE
bi_property_setter_call(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)c;
    struct pyobj *src = PY_PTR(argv[0]);
    struct pyobj *o = py_alloc(PY_T_PROPERTY);
    o->wrap.wrapped = src->wrap.wrapped;
    o->wrap.setter  = argv[1];
    o->wrap.deleter = src->wrap.deleter;
    return PY_OBJ_VAL(o);
}

static VALUE
bi_property_deleter_call(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)c;
    struct pyobj *src = PY_PTR(argv[0]);
    struct pyobj *o = py_alloc(PY_T_PROPERTY);
    o->wrap.wrapped = src->wrap.wrapped;
    o->wrap.setter  = src->wrap.setter;
    o->wrap.deleter = argv[1];
    return PY_OBJ_VAL(o);
}

static VALUE
bi_property_getter_call(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)c;
    struct pyobj *src = PY_PTR(argv[0]);
    struct pyobj *o = py_alloc(PY_T_PROPERTY);
    o->wrap.wrapped = argv[1];
    o->wrap.setter  = src->wrap.setter;
    o->wrap.deleter = src->wrap.deleter;
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

static __thread int py_skip_delattr_hook = 0;

static VALUE
bi_pystro_delattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE obj = argv[0];
    VALUE name = argv[1];
    if (!py_is_str(name)) py_raise_exc(c, c->EXC_TypeError, "delattr name must be str");
    if (py_is_instance(obj)) {
        struct pyobj *o = PY_PTR(obj);
        // Property deleter: if a property with fdel matches, call it.
        VALUE pm = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), PY_PTR(name)->str.chars);
        if (pm != PY_NONE && PY_IS_PTR(pm) && PY_PTR(pm)->type == PY_T_PROPERTY) {
            VALUE deleter = PY_PTR(pm)->wrap.deleter;
            if (deleter != PY_NONE) {
                VALUE av[1] = { obj };
                py_apply(c, deleter, 1, av);
                return PY_NONE;
            }
            py_raise_exc(c, c->EXC_AttributeError,
                         "property '%s' has no deleter", PY_PTR(name)->str.chars);
        }
        // __delattr__ user override.
        if (!py_skip_delattr_hook) {
            VALUE dm = py_class_lookup_method(PY_OBJ_VAL(o->inst.cls), "__delattr__");
            if (dm != PY_NONE
                && !(PY_IS_PTR(dm) && PY_PTR(dm)->type == PY_T_BUILTIN
                     && PY_PTR(dm)->builtin.fn == bi_object_delattr)) {
                py_skip_delattr_hook++;
                VALUE av[2] = { obj, name };
                py_apply(c, dm, 2, av);
                py_skip_delattr_hook--;
                return PY_NONE;
            }
        }
        if (o->inst.attrs) {
            uint64_t h = py_hash(c, name);
            size_t bucket; int32_t eidx; ssize_t ft;
            pydict_indices_lookup(c, o->inst.attrs, name, h, &bucket, &eidx, &ft);
            if (eidx >= 0) {
                o->inst.attrs->indices[bucket] = DICT_TOMB_IDX;
                o->inst.attrs->entries[eidx].key = DICT_DELETED_KEY;
                o->inst.attrs->entries[eidx].value = PY_NONE;
                o->inst.attrs->used--;
                return PY_NONE;
            }
        }
        py_raise_exc(c, c->EXC_AttributeError, "no such attribute '%s'", PY_PTR(name)->str.chars);
    }
    if (py_is_class(obj)) {
        struct pyclass *cd = &PY_PTR(obj)->cls;
        const char *cname = PY_PTR(name)->str.chars;
        for (int i = 0; i < cd->nmethods; i++) {
            if (strcmp(cd->methods[i].name, cname) == 0) {
                for (int j = i; j + 1 < cd->nmethods; j++)
                    cd->methods[j] = cd->methods[j + 1];
                cd->nmethods--;
                SHARED_GLOBALS_SERIAL++;
                return PY_NONE;
            }
        }
        py_raise_exc(c, c->EXC_AttributeError, "no such attribute '%s'", cname);
    }
    if (py_is_module(obj)) {
        struct pyglobals *g = PY_PTR(obj)->module.globals;
        const char *cname = PY_PTR(name)->str.chars;
        for (size_t i = 0; i < g->size; i++) {
            if (strcmp(g->entries[i].name, cname) == 0 && g->entries[i].defined) {
                g->entries[i].defined = false;
                g->entries[i].value = PY_NONE;
                return PY_NONE;
            }
        }
        py_raise_exc(c, c->EXC_AttributeError, "no such attribute '%s'", cname);
    }
    py_raise_exc(c, c->EXC_TypeError, "delattr: object does not support attribute deletion");
}

static VALUE
bi_pystro_delglobal(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "name must be str");
    const char *name = PY_PTR(argv[0])->str.chars;
    int i = py_global_index(c, name);
    if (i < 0 || !c->globals->entries[i].defined)
        py_raise_exc(c, c->EXC_NameError, "name '%s' is not defined", name);
    c->globals->entries[i].defined = false;
    c->globals->entries[i].value = PY_NONE;
    c->globals->serial = ++SHARED_GLOBALS_SERIAL;     // structural change
    return PY_NONE;
}

extern void install_builtins(CTX *c);

// Cache of already-imported modules (name → module pyobj).
// Lives in `c->globals` of the main module... actually use a static
// global so re-import shares the same instance regardless of which
// module triggers the import.
static VALUE PYSTRO_MODULES = 0;     // PY_T_DICT — initialised lazily

static VALUE
modules_dict(CTX *c)
{
    if (!PYSTRO_MODULES) PYSTRO_MODULES = py_make_dict();
    (void)c;
    return PYSTRO_MODULES;
}

// Read entire file into a malloc'd buffer.  Caller free's.
static char *
read_file_into_buf(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { free(buf); fclose(fp); return NULL; }
    buf[sz] = '\0';
    fclose(fp);
    return buf;
}

// Forward decls — main.c provides PARSE_program (we expose tokenize +
// parse_program through extern).
extern void tokenize(const char *src, const char *filename);
extern struct Node *parse_program(void);

static VALUE
bi_import(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "import name must be str");
    VALUE mod_dict = modules_dict(c);
    if (py_dict_has(c, mod_dict, argv[0])) return py_dict_get(c, mod_dict, argv[0]);

    const char *name = PY_PTR(argv[0])->str.chars;
    size_t name_len = PY_PTR(argv[0])->str.len;
    // Translate `a.b.c` into `a/b/c.py` for dotted imports.
    char relpath[1024];
    if (name_len + 4 >= sizeof(relpath)) py_raise_exc(c, c->EXC_RuntimeError, "import: name too long");
    char *p = relpath;
    for (size_t i = 0; i < name_len; i++) *p++ = (name[i] == '.') ? '/' : name[i];
    *p++ = '.'; *p++ = 'p'; *p++ = 'y'; *p = '\0';
    // Search path: CWD first, then sys.path entries (if any), then
    // PYTHONPATH (env), then the directory of pystro itself (so that
    // built-in stdlib modules are found regardless of cwd).
    char path[1024];
    char *src = NULL;
    snprintf(path, sizeof(path), "%s", relpath);
    src = read_file_into_buf(path);
    if (!src) {
        const char *pp = getenv("PYTHONPATH");
        while (pp && *pp && !src) {
            const char *colon = strchr(pp, ':');
            size_t plen = colon ? (size_t)(colon - pp) : strlen(pp);
            if (plen + strlen(relpath) + 2 < sizeof(path)) {
                memcpy(path, pp, plen);
                path[plen] = '/';
                strcpy(path + plen + 1, relpath);
                src = read_file_into_buf(path);
            }
            pp = colon ? colon + 1 : NULL;
        }
    }
    if (!src) {
        // sys.path equivalent: a static array set at startup.  For now
        // also try the directory of the running pystro binary (so user
        // modules adjacent to the binary are findable).
        extern const char *PYSTRO_BINDIR;
        if (PYSTRO_BINDIR) {
            snprintf(path, sizeof(path), "%s/%s", PYSTRO_BINDIR, relpath);
            src = read_file_into_buf(path);
        }
    }
    if (!src) {
        py_raise_exc(c, c->EXC_ModuleNotFoundError, "No module named '%s'", name);
    }

    // Build a fresh globals namespace and install the same builtins so
    // user code can call print() etc.  Save the caller's exception
    // classes — install_builtins overwrites c->EXC_* with fresh classes,
    // and we want the imported module to reuse the same exception
    // classes as the caller (so `except` matches across modules).
    struct pyglobals *new_g = py_globals_new();
    struct pyglobals *saved_g = c->globals;
#define SAVE_EXC(name) VALUE saved_EXC_##name = c->EXC_##name;
    PYSTRO_EXC_LIST(SAVE_EXC)
#undef SAVE_EXC
    // Save TYPE_* so that `int`, `str`, etc. across modules share identity.
    VALUE saved_TYPE_int       = c->TYPE_int;
    VALUE saved_TYPE_float     = c->TYPE_float;
    VALUE saved_TYPE_complex   = c->TYPE_complex;
    VALUE saved_TYPE_bool      = c->TYPE_bool;
    VALUE saved_TYPE_str       = c->TYPE_str;
    VALUE saved_TYPE_bytes     = c->TYPE_bytes;
    VALUE saved_TYPE_bytearray = c->TYPE_bytearray;
    VALUE saved_TYPE_list      = c->TYPE_list;
    VALUE saved_TYPE_tuple     = c->TYPE_tuple;
    VALUE saved_TYPE_dict      = c->TYPE_dict;
    VALUE saved_TYPE_set       = c->TYPE_set;
    VALUE saved_TYPE_frozenset = c->TYPE_frozenset;
    VALUE saved_TYPE_range     = c->TYPE_range;
    VALUE saved_TYPE_type      = c->TYPE_type;
    VALUE saved_TYPE_object    = c->TYPE_object;
    c->globals = new_g;
    install_builtins(c);
    // Re-bind the imported module's globals to the caller's exception
    // classes so exceptions raised inside cross module boundary match
    // by identity.
#define RESTORE_EXC(name) c->EXC_##name = saved_EXC_##name;
    PYSTRO_EXC_LIST(RESTORE_EXC)
#undef RESTORE_EXC
    c->TYPE_int       = saved_TYPE_int;
    c->TYPE_float     = saved_TYPE_float;
    c->TYPE_complex   = saved_TYPE_complex;
    c->TYPE_bool      = saved_TYPE_bool;
    c->TYPE_str       = saved_TYPE_str;
    c->TYPE_bytes     = saved_TYPE_bytes;
    c->TYPE_bytearray = saved_TYPE_bytearray;
    c->TYPE_list      = saved_TYPE_list;
    c->TYPE_tuple     = saved_TYPE_tuple;
    c->TYPE_dict      = saved_TYPE_dict;
    c->TYPE_set       = saved_TYPE_set;
    c->TYPE_frozenset = saved_TYPE_frozenset;
    c->TYPE_range     = saved_TYPE_range;
    c->TYPE_type      = saved_TYPE_type;
    c->TYPE_object    = saved_TYPE_object;
    py_global_set(c, "int",        c->TYPE_int);
    py_global_set(c, "float",      c->TYPE_float);
    py_global_set(c, "complex",    c->TYPE_complex);
    py_global_set(c, "bool",       c->TYPE_bool);
    py_global_set(c, "str",        c->TYPE_str);
    py_global_set(c, "bytes",      c->TYPE_bytes);
    py_global_set(c, "bytearray",  c->TYPE_bytearray);
    py_global_set(c, "list",       c->TYPE_list);
    py_global_set(c, "tuple",      c->TYPE_tuple);
    py_global_set(c, "dict",       c->TYPE_dict);
    py_global_set(c, "set",        c->TYPE_set);
    py_global_set(c, "frozenset",  c->TYPE_frozenset);
    py_global_set(c, "range",      c->TYPE_range);
    py_global_set(c, "type",       c->TYPE_type);
    py_global_set(c, "object",     c->TYPE_object);
#define SET_EXC_GLOBAL(name) py_global_set(c, #name, c->EXC_##name);
    PYSTRO_EXC_LIST(SET_EXC_GLOBAL)
#undef SET_EXC_GLOBAL
    py_global_set(c, "IOError",              c->EXC_OSError);

    // tokenize + parse the module file.  The lexer/parser state is
    // reset by tokenize(), and we're being called at runtime so the
    // original program's tokens aren't needed any more (its AST is
    // already built).
    tokenize(src, path);
    NODE *body = parse_program();

    // Wrap module body in a local try-frame so that any exception
    // inside module init is caught here.  We then restore globals
    // and re-raise via py_raise_exc so the caller's try/except
    // (which runs on the *original* globals) sees it.
    jmp_buf jb;
    if (c->try_top < 64) c->try_stack[c->try_top++] = &jb;
    if (setjmp(jb) == 0) {
        EVAL(c, body);
    }
    c->try_top--;
    if (c->state == PY_STATE_RAISE) {
        // Module init raised — restore caller's globals first, then
        // re-raise so the caller's try/except (running on the original
        // globals) sees it.  Do NOT cache the half-initialised module.
        VALUE exc = c->state_value;
        c->globals = saved_g;
        free(src);
        c->state = PY_STATE_RAISE;
        c->state_value = exc;
        if (c->try_top > 0) longjmp(*c->try_stack[c->try_top - 1], 1);
        if (c->err_jmp_active) longjmp(c->err_jmp, 1);
        return PY_NONE;
    }
    c->state = PY_STATE_NORMAL; c->state_value = PY_NONE;

    // Wrap globals into a module pyobj.
    struct pyobj *mo = py_alloc(PY_T_MODULE);
    mo->module.name = py_make_str(name, strlen(name)) ? name : name;
    mo->module.globals = new_g;
    VALUE mod = PY_OBJ_VAL(mo);

    c->globals = saved_g;
    py_dict_set(c, mod_dict, argv[0], mod);
    free(src);
    return mod;
}

// Synthetic Exception.__init__(self, *args) — sets self.args and
// self.message (= args[0] if there's exactly one arg).
VALUE
bi_exception_init(CTX *c, int argc, VALUE *argv)
{
    VALUE self = argv[0];
    int nargs = argc - 1;
    VALUE args_tuple = py_make_tuple(argc > 1 ? &argv[1] : NULL, nargs);
    py_setattr(c, self, "args", args_tuple);
    if (nargs == 1) {
        py_setattr(c, self, "message", argv[1]);
    } else if (nargs == 0) {
        py_setattr(c, self, "message", py_make_str("", 0));
    }
    // StopIteration(value) — expose .value (used by `yield from` /
    // generator return).  Harmless for other Exception subclasses.
    py_setattr(c, self, "value", nargs >= 1 ? argv[1] : PY_NONE);
    // SystemExit(code) — CPython exposes .code; for simplicity we set
    // it on every Exception (matches the .value pattern above and is a
    // no-op for non-SystemExit subclasses).
    py_setattr(c, self, "code", nargs >= 1 ? argv[1] : PY_NONE);
    return PY_NONE;
}

// Math primitives surfaced as `__pystro_*__` and wrapped by `math.py`.
static VALUE
bi_pystro_sqrt(CTX *c, int argc, VALUE *argv)
{ (void)argc; double d = py_to_double(c, argv[0]);
  if (d < 0) py_raise_exc(c, c->EXC_ValueError, "math domain error");
  return py_make_float(sqrt(d));
}
static VALUE
bi_pystro_sin(CTX *c, int argc, VALUE *argv)
{ (void)argc; return py_make_float(sin(py_to_double(c, argv[0]))); }
static VALUE
bi_pystro_cos(CTX *c, int argc, VALUE *argv)
{ (void)argc; return py_make_float(cos(py_to_double(c, argv[0]))); }
static VALUE
bi_pystro_tan(CTX *c, int argc, VALUE *argv)
{ (void)argc; return py_make_float(tan(py_to_double(c, argv[0]))); }
static VALUE
bi_pystro_log(CTX *c, int argc, VALUE *argv)
{
    double x = py_to_double(c, argv[0]);
    if (x <= 0) py_raise_exc(c, c->EXC_ValueError, "math domain error");
    if (argc >= 2) {
        double base = py_to_double(c, argv[1]);
        if (base <= 0 || base == 1) py_raise_exc(c, c->EXC_ValueError, "math domain error");
        return py_make_float(log(x) / log(base));
    }
    return py_make_float(log(x));
}
static VALUE
bi_pystro_exp(CTX *c, int argc, VALUE *argv)
{ (void)argc; return py_make_float(exp(py_to_double(c, argv[0]))); }
static VALUE
bi_pystro_floor(CTX *c, int argc, VALUE *argv)
{
    if (py_is_instance(argv[0])) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "__floor__");
        if (m != PY_NONE) return py_apply(c, m, 1, argv);
    }
    (void)argc; return py_make_int((int64_t)floor(py_to_double(c, argv[0])));
}
static VALUE
bi_pystro_ceil(CTX *c, int argc, VALUE *argv)
{
    if (py_is_instance(argv[0])) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "__ceil__");
        if (m != PY_NONE) return py_apply(c, m, 1, argv);
    }
    (void)argc; return py_make_int((int64_t)ceil(py_to_double(c, argv[0])));
}
static VALUE
bi_pystro_trunc_dispatch(CTX *c, int argc, VALUE *argv)
{
    if (py_is_instance(argv[0])) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "__trunc__");
        if (m != PY_NONE) return py_apply(c, m, 1, argv);
    }
    (void)argc;
    double d = py_to_double(c, argv[0]);
    return py_make_int((int64_t)d);
}
static VALUE
bi_pystro_atan2(CTX *c, int argc, VALUE *argv)
{ (void)argc;
  return py_make_float(atan2(py_to_double(c, argv[0]), py_to_double(c, argv[1]))); }
static VALUE
bi_pystro_pow(CTX *c, int argc, VALUE *argv)
{ (void)argc;
  return py_make_float(pow(py_to_double(c, argv[0]), py_to_double(c, argv[1]))); }

// Time / OS primitives surfaced through `time.py` and `os.py`.
static VALUE
bi_pystro_time(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return py_make_float(ts.tv_sec + ts.tv_nsec / 1e9);
}
static VALUE
bi_pystro_sleep(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    double d = py_to_double(c, argv[0]);
    if (d <= 0) return PY_NONE;
    struct timespec req = { (time_t)d, (long)((d - (time_t)d) * 1e9) };
    nanosleep(&req, NULL);
    return PY_NONE;
}
static VALUE
bi_pystro_perf_counter(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return py_make_float(ts.tv_sec + ts.tv_nsec / 1e9);
}
static VALUE
bi_pystro_getenv(CTX *c, int argc, VALUE *argv)
{
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "getenv: name must be str");
    size_t L = PY_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PY_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    const char *v = getenv(buf);
    if (v) return py_make_str(v, strlen(v));
    return argc >= 2 ? argv[1] : PY_NONE;
}
static VALUE
bi_pystro_getcwd(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return py_make_str("", 0);
    return py_make_str(buf, strlen(buf));
}
// MD5 — RFC 1321 reference implementation.
static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const int MD5_S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21,
};
static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static VALUE
bi_pystro_md5(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0]) && !py_is_byteseq(argv[0]))
        py_raise_exc(c, c->EXC_TypeError, "md5 needs str or bytes");
    const unsigned char *src = (const unsigned char *)PY_PTR(argv[0])->str.chars;
    size_t L = PY_PTR(argv[0])->str.len;
    // Pad: append 0x80, zeros until length % 64 == 56, then 8-byte length.
    size_t total = ((L + 9 + 63) / 64) * 64;
    unsigned char *m = (unsigned char *)GC_malloc_atomic(total);
    memcpy(m, src, L);
    m[L] = 0x80;
    for (size_t i = L + 1; i < total - 8; i++) m[i] = 0;
    uint64_t bits = (uint64_t)L * 8;
    for (int i = 0; i < 8; i++) m[total - 8 + i] = (unsigned char)(bits >> (8 * i));

    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (size_t off = 0; off < total; off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++)
            M[i] = (uint32_t)m[off+i*4]
                 | ((uint32_t)m[off+i*4+1] << 8)
                 | ((uint32_t)m[off+i*4+2] << 16)
                 | ((uint32_t)m[off+i*4+3] << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D; g = (3*i + 5) & 15; }
            else { F = C ^ (B | ~D); g = (7*i) & 15; }
            uint32_t tmp = D;
            D = C; C = B;
            B = B + rotl32(A + F + MD5_K[i] + M[g], MD5_S[i]);
            A = tmp;
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    char hex[33];
    uint32_t parts[4] = { a0, b0, c0, d0 };
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++)
            sprintf(&hex[i*8 + j*2], "%02x", (parts[i] >> (8*j)) & 0xff);
    }
    hex[32] = '\0';
    return py_make_str(hex, 32);
}

// SHA-256 — FIPS 180-4 reference impl.
static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static VALUE
bi_pystro_sha256(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0]) && !py_is_byteseq(argv[0]))
        py_raise_exc(c, c->EXC_TypeError, "sha256 needs str or bytes");
    const unsigned char *src = (const unsigned char *)PY_PTR(argv[0])->str.chars;
    size_t L = PY_PTR(argv[0])->str.len;
    size_t total = ((L + 9 + 63) / 64) * 64;
    unsigned char *m = (unsigned char *)GC_malloc_atomic(total);
    memcpy(m, src, L);
    m[L] = 0x80;
    for (size_t i = L + 1; i < total - 8; i++) m[i] = 0;
    uint64_t bits = (uint64_t)L * 8;
    for (int i = 0; i < 8; i++) m[total - 8 + i] = (unsigned char)(bits >> (8 * (7 - i)));

    uint32_t H[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    for (size_t off = 0; off < total; off += 64) {
        uint32_t W[64];
        for (int i = 0; i < 16; i++)
            W[i] = ((uint32_t)m[off+i*4] << 24)
                 | ((uint32_t)m[off+i*4+1] << 16)
                 | ((uint32_t)m[off+i*4+2] << 8)
                 | (uint32_t)m[off+i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = (W[i-15] >> 7 | W[i-15] << 25) ^ (W[i-15] >> 18 | W[i-15] << 14) ^ (W[i-15] >> 3);
            uint32_t s1 = (W[i-2] >> 17 | W[i-2] << 15) ^ (W[i-2] >> 19 | W[i-2] << 13) ^ (W[i-2] >> 10);
            W[i] = W[i-16] + s0 + W[i-7] + s1;
        }
        uint32_t a=H[0],b=H[1],cc=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = (e>>6 | e<<26) ^ (e>>11 | e<<21) ^ (e>>25 | e<<7);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + SHA256_K[i] + W[i];
            uint32_t S0 = (a>>2 | a<<30) ^ (a>>13 | a<<19) ^ (a>>22 | a<<10);
            uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
            uint32_t t2 = S0 + mj;
            h = g; g = f; f = e; e = d + t1;
            d = cc; cc = b; b = a; a = t1 + t2;
        }
        H[0] += a; H[1] += b; H[2] += cc; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }
    char hex[65];
    for (int i = 0; i < 8; i++) sprintf(&hex[i*8], "%08x", H[i]);
    hex[64] = '\0';
    return py_make_str(hex, 64);
}

static VALUE
bi_pystro_path_exists(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "exists: path must be str");
    size_t L = PY_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PY_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    return access(buf, F_OK) == 0 ? PY_TRUE : PY_FALSE;
}

static VALUE
bi_pystro_listdir(CTX *c, int argc, VALUE *argv)
{
    extern int closedir(DIR *);
    extern DIR *opendir(const char *);
    extern struct dirent *readdir(DIR *);
    const char *p = ".";
    char buf[1024];
    if (argc >= 1 && py_is_str(argv[0])) {
        size_t L = PY_PTR(argv[0])->str.len;
        if (L >= sizeof(buf)) py_raise_exc(c, c->EXC_RuntimeError, "listdir: path too long");
        memcpy(buf, PY_PTR(argv[0])->str.chars, L); buf[L] = '\0';
        p = buf;
    }
    DIR *d = opendir(p);
    if (!d) py_raise_exc(c, c->EXC_RuntimeError, "listdir: %s", p);
    VALUE r = py_make_list(NULL, 0);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        py_list_append(c, r, py_make_str(de->d_name, strlen(de->d_name)));
    }
    closedir(d);
    return r;
}

static VALUE
bi_pystro_remove(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "remove: path must be str");
    size_t L = PY_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PY_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    if (unlink(buf) != 0) py_raise_exc(c, c->EXC_RuntimeError, "remove failed: %s", buf);
    return PY_NONE;
}

static VALUE
bi_pystro_makedirs(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "makedirs: path must be str");
    size_t L = PY_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PY_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    bool exist_ok = (argc >= 2 && argv[1] == PY_TRUE);
    // Create intermediate dirs.
    char path[1024];
    size_t plen = 0;
    for (size_t i = 0; i <= L; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            if (plen == 0 && buf[i] == '/') {
                path[plen++] = '/'; path[plen] = '\0';
                continue;
            }
            if (plen > 0) {
                path[plen] = '\0';
                if (mkdir(path, 0755) != 0) {
                    if (errno == EEXIST && exist_ok) { /* ok */ }
                    else if (errno != EEXIST) py_raise_exc(c, c->EXC_RuntimeError, "makedirs failed: %s", path);
                }
            }
            if (buf[i] == '/' && plen + 1 < sizeof(path)) {
                path[plen++] = '/'; path[plen] = '\0';
            }
        } else {
            if (plen + 1 < sizeof(path)) { path[plen++] = buf[i]; path[plen] = '\0'; }
        }
    }
    return PY_NONE;
}

static VALUE
bi_pystro_isdir(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "isdir: path must be str");
    size_t L = PY_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PY_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    struct stat st;
    if (stat(buf, &st) != 0) return PY_FALSE;
    return S_ISDIR(st.st_mode) ? PY_TRUE : PY_FALSE;
}

static VALUE
bi_pystro_isfile(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "isfile: path must be str");
    size_t L = PY_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PY_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    struct stat st;
    if (stat(buf, &st) != 0) return PY_FALSE;
    return S_ISREG(st.st_mode) ? PY_TRUE : PY_FALSE;
}

static VALUE
bi_pystro_abspath(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!py_is_str(argv[0])) py_raise_exc(c, c->EXC_TypeError, "abspath: path must be str");
    size_t L = PY_PTR(argv[0])->str.len;
    char in[1024];
    if (L >= sizeof(in)) py_raise_exc(c, c->EXC_RuntimeError, "abspath: path too long");
    memcpy(in, PY_PTR(argv[0])->str.chars, L); in[L] = '\0';
    char out[4096];
    if (in[0] == '/') {
        snprintf(out, sizeof(out), "%s", in);
    } else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof(cwd))) py_raise_exc(c, c->EXC_RuntimeError, "abspath: getcwd");
        snprintf(out, sizeof(out), "%s/%s", cwd, in);
    }
    return py_make_str(out, strlen(out));
}

// Process info / control surfaced through the `sys` module (sys.py).
extern int    PYSTRO_ARGC;
extern char **PYSTRO_ARGV;
static VALUE
bi_pystro_argv(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    VALUE r = py_make_list(NULL, 0);
    for (int i = 0; i < PYSTRO_ARGC; i++)
        py_list_append(c, r, py_make_str(PYSTRO_ARGV[i], strlen(PYSTRO_ARGV[i])));
    return r;
}
static VALUE
bi_pystro_exit(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    int code = 0;
    if (argc >= 1 && PY_IS_FIXNUM(argv[0])) code = (int)PY_FIXVAL(argv[0]);
    fflush(stdout); fflush(stderr);
    exit(code);
}

static VALUE
bi_pystro_current_exc(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    if (c->current_handling_exc && c->current_handling_exc != PY_NONE)
        return c->current_handling_exc;
    return PY_NONE;
}

// Reinterpret a float as its IEEE-754 bits.  Returns int (may be a
// bignum for double's 64-bit pattern).
static VALUE
bi_pystro_float_to_bits(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    double d = py_to_double(c, argv[0]);
    bool is_double = (argc < 2) || py_is_truthy(argv[1]);
    if (is_double) {
        uint64_t b;
        memcpy(&b, &d, 8);
        // Build via mpz to avoid sign issues with int64_t.
        struct pyobj *o = py_alloc(PY_T_BIGNUM);
        mpz_init(o->mpz);
        // Use two limbs: hi 32 bits, lo 32 bits.
        mpz_set_ui(o->mpz, (unsigned long)(b >> 32));
        mpz_mul_2exp(o->mpz, o->mpz, 32);
        mpz_add_ui(o->mpz, o->mpz, (unsigned long)(b & 0xFFFFFFFFu));
        return PY_OBJ_VAL(o);
    }
    float f = (float)d;
    uint32_t b;
    memcpy(&b, &f, 4);
    return py_make_int((int64_t)b);
}

// Inverse of float_to_bits.
static VALUE
bi_pystro_bits_to_float(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    bool is_double = (argc < 2) || py_is_truthy(argv[1]);
    if (is_double) {
        // Get the int as a 64-bit pattern.
        uint64_t b = 0;
        if (py_is_bignum(argv[0])) {
            mpz_t tmp; mpz_init(tmp);
            mpz_set(tmp, PY_PTR(argv[0])->mpz);
            mpz_t mod; mpz_init(mod);
            mpz_set_ui(mod, 1); mpz_mul_2exp(mod, mod, 64);
            mpz_mod(tmp, tmp, mod);
            // Extract via successive division.
            mpz_t lo32; mpz_init(lo32);
            mpz_t hi32; mpz_init(hi32);
            mpz_set(hi32, tmp);
            mpz_fdiv_q_2exp(hi32, hi32, 32);
            mpz_set(lo32, tmp);
            mpz_set_ui(mod, 0xFFFFFFFFul);
            mpz_and(lo32, lo32, mod);
            unsigned long lo = mpz_get_ui(lo32);
            unsigned long hi = mpz_get_ui(hi32);
            b = ((uint64_t)hi << 32) | (uint64_t)lo;
            mpz_clear(tmp); mpz_clear(mod); mpz_clear(lo32); mpz_clear(hi32);
        } else {
            int64_t s = py_int_to_long(c, argv[0]);
            b = (uint64_t)s;
        }
        double d;
        memcpy(&d, &b, 8);
        return py_make_float(d);
    }
    uint32_t b = (uint32_t)py_int_to_long(c, argv[0]);
    float f;
    memcpy(&f, &b, 4);
    return py_make_float((double)f);
}

// `from m import *` — copy all non-underscore names from the module's
// globals into the current frame's globals.  If the module defines
// `__all__` (a list of names), only those are exported.
static VALUE
bi_import_star(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE mod = argv[0];
    if (!py_is_module(mod)) py_raise_exc(c, c->EXC_TypeError, "import * needs a module");
    struct pyglobals *g = PY_PTR(mod)->module.globals;
    // Honour __all__ if present and is a list/tuple of strings.
    VALUE all = 0;
    for (size_t i = 0; i < g->size; i++) {
        if (g->entries[i].defined && strcmp(g->entries[i].name, "__all__") == 0) {
            all = g->entries[i].value; break;
        }
    }
    if (all != 0 && (py_is_list(all) || py_is_tuple(all))) {
        size_t n = PY_PTR(all)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE nm = PY_PTR(all)->list.items[i];
            if (!py_is_str(nm)) continue;
            const char *cname = PY_PTR(nm)->str.chars;
            for (size_t j = 0; j < g->size; j++) {
                if (g->entries[j].defined && strcmp(g->entries[j].name, cname) == 0) {
                    py_global_set(c, cname, g->entries[j].value);
                    break;
                }
            }
        }
    } else {
        for (size_t i = 0; i < g->size; i++) {
            if (!g->entries[i].defined) continue;
            const char *nm = g->entries[i].name;
            if (nm[0] == '_') continue;
            // Skip names that look like our own builtins so we don't
            // overwrite them (print, str, ...) — built-ins are already
            // present in the destination via install_builtins.
            // (The destination's own copies stay; the imported ones
            // would be identical anyway.)
            py_global_set(c, nm, g->entries[i].value);
        }
    }
    return PY_NONE;
}

static VALUE
bi_pystro_yield_from(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    extern VALUE py_gen_yield_from(CTX *c, VALUE iter);
    return py_gen_yield_from(c, argv[0]);
}

// Unary + dispatch: call __pos__ on instances; identity otherwise.
static VALUE
bi_pystro_pos(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (py_is_instance(argv[0])) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "__pos__");
        if (m != PY_NONE) return py_apply(c, m, 1, argv);
    }
    return argv[0];
}

static VALUE
bi_pystro_del(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE container = argv[0], key = argv[1];
    if (py_is_dict(container)) {
        if (!py_dict_remove(c, container, key)) {
            VALUE r = py_to_repr(c, key);
            py_raise_exc(c, c->EXC_KeyError, "%s",
                         py_is_str(r) ? PY_PTR(r)->str.chars : "?");
        }
        return PY_NONE;
    }
    if (py_is_list(container)) {
        int64_t i = py_int_to_long(c, key);
        struct pyobj *o = PY_PTR(container);
        if (i < 0) i += (int64_t)o->list.len;
        if (i < 0 || i >= (int64_t)o->list.len)
            py_raise_exc(c, c->EXC_IndexError, "del: index out of range");
        for (size_t j = (size_t)i; j + 1 < o->list.len; j++)
            o->list.items[j] = o->list.items[j + 1];
        o->list.len--;
        return PY_NONE;
    }
    if (py_is_set(container)) {
        py_dict_remove(c, container, key);
        return PY_NONE;
    }
    if (py_is_instance(container)) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(container)->inst.cls), "__delitem__");
        if (m != PY_NONE) {
            VALUE av[2] = { container, key };
            return py_apply(c, m, 2, av);
        }
        // Built-in subclass (e.g., class OD(dict)): forward to primary.
        if (PY_PTR(container)->inst.primary)
            return bi_pystro_del(c, argc, (VALUE[]){PY_PTR(container)->inst.primary, key});
    }
    py_raise_exc(c, c->EXC_TypeError, "del: unsupported container type");
}

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
    // Dispatch to __divmod__ on instances.
    if (py_is_instance(argv[0])) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "__divmod__");
        if (m != PY_NONE) {
            return py_apply(c, m, 2, argv);
        }
    }
    VALUE q = py_fdiv(c, argv[0], argv[1]);
    VALUE r = py_mod (c, argv[0], argv[1]);
    VALUE pair[2] = { q, r };
    return py_make_tuple(pair, 2);
}

static VALUE
bi_round(CTX *c, int argc, VALUE *argv)
{
    // Dispatch to user-class __round__.
    if (py_is_instance(argv[0])) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "__round__");
        if (m != PY_NONE) {
            return py_apply(c, m, argc, argv);
        }
    }
    int ndig = (argc >= 2) ? (int)py_int_to_long(c, argv[1]) : 0;
    double d = py_to_double(c, argv[0]);
    double mul = 1.0;
    for (int i = 0; i < ndig; i++) mul *= 10.0;
    for (int i = 0; i > ndig; i--) mul /= 10.0;
    // Banker's rounding (round-half-to-even) — matches Python.
    double scaled = d * mul;
    double rounded;
    double fr = floor(scaled);
    double diff = scaled - fr;
    if (diff < 0.5) {
        rounded = fr;
    } else if (diff > 0.5) {
        rounded = fr + 1.0;
    } else {
        // Exactly half — round to even.
        if (fmod(fr, 2.0) == 0.0) rounded = fr;
        else                       rounded = fr + 1.0;
    }
    double r = rounded / mul;
    // Python: round(x) → int; round(x, n) → same type as x.
    if (argc < 2) return PY_FIX((int64_t)rounded);
    if (PY_IS_FIXNUM(argv[0]) || py_is_bignum(argv[0])) return PY_FIX((int64_t)r);
    return py_make_float(r);
}

static VALUE
bi_pow(CTX *c, int argc, VALUE *argv)
{
    // User class with __pow__: forward all args.
    if (py_is_instance(argv[0])) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "__pow__");
        if (m != PY_NONE) return py_apply(c, m, argc, argv);
    }
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
    if (py_is_dict(argv[0])) {
        struct pydict *d = PY_PTR(argv[0])->dict;
        // Walk entries in reverse insertion order.
        for (size_t i = d->elen; i > 0; i--)
            if (pydict_entry_live(d, i - 1)) py_list_append(c, r, d->entries[i - 1].key);
        return r;
    }
    if (py_is_instance(argv[0])) {
        VALUE m = py_class_lookup_method(PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls), "__reversed__");
        if (m != PY_NONE) {
            VALUE av[1] = { argv[0] };
            return py_apply(c, m, 1, av);
        }
    }
    py_raise_exc(c, c->EXC_TypeError, "argument to reversed() must be a sequence");
}

static VALUE
bi_map(CTX *c, int argc, VALUE *argv)
{
    if (argc < 2) py_raise_exc(c, c->EXC_TypeError, "map() needs >=2 args");
    int n_iters = argc - 1;
    struct pyobj *o = py_alloc(PY_T_ITER);
    o->iter_state = (struct py_iter *)GC_malloc(sizeof(struct py_iter));
    o->iter_state->kind = 10;
    o->iter_state->container = argv[0];
    o->iter_state->inner = (struct py_iter *)GC_malloc(sizeof(struct py_iter) * n_iters);
    o->iter_state->n_inner = n_iters;
    for (int i = 0; i < n_iters; i++) {
        py_iter_init(c, &o->iter_state->inner[i], argv[i + 1]);
        if (c->state != PY_STATE_NORMAL) return PY_NONE;
    }
    return PY_OBJ_VAL(o);
}

static VALUE
bi_filter(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pyobj *o = py_alloc(PY_T_ITER);
    o->iter_state = (struct py_iter *)GC_malloc(sizeof(struct py_iter));
    o->iter_state->kind = 11;
    o->iter_state->container = argv[0];   // None or callable
    o->iter_state->inner = (struct py_iter *)GC_malloc(sizeof(struct py_iter));
    o->iter_state->n_inner = 1;
    py_iter_init(c, &o->iter_state->inner[0], argv[1]);
    if (c->state != PY_STATE_NORMAL) return PY_NONE;
    return PY_OBJ_VAL(o);
}

static VALUE
bi_iter(CTX *c, int argc, VALUE *argv)
{
    if (argc == 2) {
        // iter(callable, sentinel)
        struct pyobj *o = py_alloc(PY_T_ITER);
        o->iter_state = (struct py_iter *)GC_malloc(sizeof(struct py_iter));
        o->iter_state->kind = 4;
        o->iter_state->container = argv[0];
        o->iter_state->sentinel  = argv[1];
        return PY_OBJ_VAL(o);
    }
    if (PY_IS_PTR(argv[0]) && PY_PTR(argv[0])->type == PY_T_GEN) return argv[0];
    if (PY_IS_PTR(argv[0]) && PY_PTR(argv[0])->type == PY_T_ITER) return argv[0];
    if (py_is_instance(argv[0])) {
        VALUE cls = PY_OBJ_VAL(PY_PTR(argv[0])->inst.cls);
        VALUE im = py_class_lookup_method(cls, "__iter__");
        if (im != PY_NONE) {
            VALUE av[1] = { argv[0] };
            return py_apply(c, im, 1, av);
        }
    }
    struct pyobj *o = py_alloc(PY_T_ITER);
    o->iter_state = (struct py_iter *)GC_malloc(sizeof(struct py_iter));
    py_iter_init(c, o->iter_state, argv[0]);
    return PY_OBJ_VAL(o);
}

static VALUE
bi_next(CTX *c, int argc, VALUE *argv)
{
    VALUE it = argv[0];
    if (PY_IS_PTR(it) && PY_PTR(it)->type == PY_T_GEN) {
        if (argc >= 2 && PY_PTR(it)->gen->done) return argv[1];
        VALUE r = py_gen_next(c, it);
        if (c->state == PY_STATE_RAISE && argc >= 2) {
            VALUE exc = c->state_value;
            if (py_exc_matches(c, exc, c->EXC_StopIteration)) {
                c->state = PY_STATE_NORMAL; c->state_value = PY_NONE;
                return argv[1];
            }
        }
        return r;
    }
    if (PY_IS_PTR(it) && PY_PTR(it)->type == PY_T_ITER) {
        VALUE r;
        if (py_iter_next(c, PY_PTR(it)->iter_state, &r)) return r;
        if (argc >= 2) return argv[1];
        py_raise_exc(c, c->EXC_StopIteration, "iterator exhausted");
    }
    if (py_is_instance(it)) {
        VALUE cls = PY_OBJ_VAL(PY_PTR(it)->inst.cls);
        VALUE nm = py_class_lookup_method(cls, "__next__");
        if (nm != PY_NONE) {
            VALUE av[1] = { it };
            return py_apply(c, nm, 1, av);
        }
    }
    py_raise_exc(c, c->EXC_TypeError, "next() argument is not an iterator");
}

void
install_builtins(CTX *c)
{
    py_global_define(c, "print",      py_make_builtin("print",      bi_print,      0, -1));
    py_global_define(c, "repr",       py_make_builtin("repr",       bi_repr,       1,  1));
    py_global_define(c, "ascii",      py_make_builtin("ascii",      bi_repr,       1,  1));
    // Built-in type classes — proper class objects whose constructors
    // are the C bi_* functions.  isinstance(5, int), type(5) is int,
    // and class M(int): pass all work via these.
    c->TYPE_int       = py_make_builtin_class("int",       bi_int,       PY_T_BIGNUM);
    py_class_add_method(c, c->TYPE_int, "from_bytes",
        py_make_builtin("from_bytes", bi_int_from_bytes, 1, 3));
    c->TYPE_float     = py_make_builtin_class("float",     bi_float,     PY_T_FLOAT);
    py_class_add_method(c, c->TYPE_float, "fromhex",
        py_make_builtin("fromhex", bi_float_fromhex, 1, 1));
    c->TYPE_complex   = py_make_builtin_class("complex",   bi_complex,   PY_T_COMPLEX);
    c->TYPE_bool      = py_make_builtin_class("bool",      bi_bool,      -1);
    c->TYPE_str       = py_make_builtin_class("str",       bi_str,       PY_T_STR);
    py_class_add_method(c, c->TYPE_str, "maketrans",
        py_make_builtin("maketrans", bi_str_maketrans, 1, 3));
    c->TYPE_bytes     = py_make_builtin_class("bytes",     bi_bytes,     PY_T_BYTES);
    py_class_add_method(c, c->TYPE_bytes, "fromhex",
        py_make_builtin("fromhex", bi_bytes_fromhex, 1, 1));
    c->TYPE_bytearray = py_make_builtin_class("bytearray", bi_bytearray, PY_T_BYTEARRAY);
    py_class_add_method(c, c->TYPE_bytearray, "fromhex",
        py_make_builtin("fromhex", bi_bytes_fromhex, 1, 1));
    c->TYPE_list      = py_make_builtin_class("list",      bi_list,      PY_T_LIST);
    c->TYPE_tuple     = py_make_builtin_class("tuple",     bi_tuple,     PY_T_TUPLE);
    c->TYPE_dict      = py_make_builtin_class("dict",      bi_dict,      PY_T_DICT);
    py_class_add_method(c, c->TYPE_dict, "fromkeys",
        py_make_builtin("fromkeys", bi_dict_fromkeys, 1, 2));
    c->TYPE_set       = py_make_builtin_class("set",       bi_set,       PY_T_SET);
    c->TYPE_frozenset = py_make_builtin_class("frozenset", bi_frozenset, PY_T_FROZENSET);
    c->TYPE_range     = py_make_builtin_class("range",     bi_range,     PY_T_RANGE);
    c->TYPE_type      = py_make_builtin_class("type",      bi_type,      PY_T_CLASS);
    c->TYPE_object    = py_make_class("object", PY_NONE, false);
    {
        // object.__new__(cls, *args, **kwargs) — default implementation
        // that just allocates a new instance of `cls`.
        extern VALUE bi_object_new(CTX *c, int argc, VALUE *argv);
        py_class_add_method(c, c->TYPE_object, "__new__",
            py_make_builtin("__new__", bi_object_new, 1, -1));
        py_class_add_method(c, c->TYPE_object, "__getattribute__",
            py_make_builtin("__getattribute__", bi_object_getattribute, 2, 2));
        py_class_add_method(c, c->TYPE_object, "__setattr__",
            py_make_builtin("__setattr__", bi_object_setattr, 3, 3));
        py_class_add_method(c, c->TYPE_object, "__delattr__",
            py_make_builtin("__delattr__", bi_object_delattr, 2, 2));
        py_class_add_method(c, c->TYPE_object, "__init__",
            py_make_builtin("__init__", bi_object_init, 1, -1));
    }
    // Synthetic type classes for built-in non-constructable types.
    c->TYPE_NoneType                    = py_make_class("NoneType",                     PY_NONE, false);
    c->TYPE_function                    = py_make_class("function",                     PY_NONE, false);
    c->TYPE_builtin_function_or_method  = py_make_class("builtin_function_or_method",   PY_NONE, false);
    c->TYPE_method                      = py_make_class("method",                       PY_NONE, false);
    c->TYPE_module                      = py_make_class("module",                       PY_NONE, false);
    c->TYPE_slice                       = py_make_class("slice",                        PY_NONE, false);
    c->TYPE_ellipsis                    = py_make_class("ellipsis",                     PY_NONE, false);
    c->TYPE_NotImplementedType          = py_make_class("NotImplementedType",           PY_NONE, false);
    c->TYPE_memoryview                  = py_make_class("memoryview",                   PY_NONE, false);
    c->TYPE_generator                   = py_make_class("generator",                    PY_NONE, false);
    c->TYPE_property                    = py_make_class("property",                     PY_NONE, false);
    c->TYPE_staticmethod                = py_make_class("staticmethod",                 PY_NONE, false);
    c->TYPE_classmethod                 = py_make_class("classmethod",                  PY_NONE, false);
    c->TYPE_super                       = py_make_class("super",                        PY_NONE, false);
    // Wire up base classes so issubclass/isinstance walk MRO properly.
    // bool < int < object; everything else < object.  bytes/bytearray
    // share an ancestor (object) — pystro doesn't model the C-level
    // bytes-like protocol.
    {
        VALUE base_obj[1] = { c->TYPE_object };
        VALUE base_int[1] = { c->TYPE_int };
        py_class_set_bases(c->TYPE_int, base_obj, 1);
        py_class_set_bases(c->TYPE_float, base_obj, 1);
        py_class_set_bases(c->TYPE_complex, base_obj, 1);
        py_class_set_bases(c->TYPE_bool, base_int, 1);     // bool subclasses int
        py_class_set_bases(c->TYPE_str, base_obj, 1);
        py_class_set_bases(c->TYPE_bytes, base_obj, 1);
        py_class_set_bases(c->TYPE_bytearray, base_obj, 1);
        py_class_set_bases(c->TYPE_list, base_obj, 1);
        py_class_set_bases(c->TYPE_tuple, base_obj, 1);
        py_class_set_bases(c->TYPE_dict, base_obj, 1);
        py_class_set_bases(c->TYPE_set, base_obj, 1);
        py_class_set_bases(c->TYPE_frozenset, base_obj, 1);
        py_class_set_bases(c->TYPE_range, base_obj, 1);
        py_class_set_bases(c->TYPE_type, base_obj, 1);
        py_class_set_bases(c->TYPE_NoneType, base_obj, 1);
        py_class_set_bases(c->TYPE_function, base_obj, 1);
        py_class_set_bases(c->TYPE_builtin_function_or_method, base_obj, 1);
        py_class_set_bases(c->TYPE_method, base_obj, 1);
        py_class_set_bases(c->TYPE_module, base_obj, 1);
        py_class_set_bases(c->TYPE_slice, base_obj, 1);
        py_class_set_bases(c->TYPE_ellipsis, base_obj, 1);
        py_class_set_bases(c->TYPE_NotImplementedType, base_obj, 1);
        py_class_set_bases(c->TYPE_memoryview, base_obj, 1);
        py_class_set_bases(c->TYPE_generator, base_obj, 1);
        py_class_set_bases(c->TYPE_property, base_obj, 1);
        py_class_set_bases(c->TYPE_staticmethod, base_obj, 1);
        py_class_set_bases(c->TYPE_classmethod, base_obj, 1);
        py_class_set_bases(c->TYPE_super, base_obj, 1);
    }
    py_global_define(c, "int",        c->TYPE_int);
    py_global_define(c, "float",      c->TYPE_float);
    py_global_define(c, "complex",    c->TYPE_complex);
    py_global_define(c, "bool",       c->TYPE_bool);
    py_global_define(c, "str",        c->TYPE_str);
    py_global_define(c, "bytes",      c->TYPE_bytes);
    py_global_define(c, "bytearray",  c->TYPE_bytearray);
    py_global_define(c, "list",       c->TYPE_list);
    py_global_define(c, "tuple",      c->TYPE_tuple);
    py_global_define(c, "dict",       c->TYPE_dict);
    py_global_define(c, "set",        c->TYPE_set);
    py_global_define(c, "frozenset",  c->TYPE_frozenset);
    py_global_define(c, "range",      c->TYPE_range);
    py_global_define(c, "type",       c->TYPE_type);
    py_global_define(c, "object",     c->TYPE_object);
    // int/float/complex/bool/str/bytes/bytearray/list/tuple/dict/set/
    // frozenset/range/type are registered as TYPE classes above.
    py_global_define(c, "len",        py_make_builtin("len",        bi_len,        1,  1));
    py_global_define(c, "abs",        py_make_builtin("abs",        bi_abs,        1,  1));
    py_global_define(c, "isinstance", py_make_builtin("isinstance", bi_isinstance, 2,  2));
    py_global_define(c, "issubclass", py_make_builtin("issubclass", bi_issubclass, 2,  2));
    py_global_define(c, "id",         py_make_builtin("id",         bi_id,         1,  1));
    py_global_define(c, "dir",        py_make_builtin("dir",        bi_dir,        0,  1));
    py_global_define(c, "globals",    py_make_builtin("globals",    bi_globals,    0,  0));
    py_global_define(c, "locals",     py_make_builtin("locals",     bi_locals,     0,  0));
    py_global_define(c, "vars",       py_make_builtin("vars",       bi_vars,       0,  1));
    py_global_define(c, "hasattr",    py_make_builtin("hasattr",    bi_hasattr,    2,  2));
    py_global_define(c, "getattr",    py_make_builtin("getattr",    bi_getattr,    2,  3));
    py_global_define(c, "setattr",    py_make_builtin("setattr",    bi_setattr,    3,  3));
    py_global_define(c, "delattr",    py_make_builtin("delattr",    bi_delattr,    2,  2));
    py_global_define(c, "callable",   py_make_builtin("callable",   bi_callable,   1,  1));
    py_global_define(c, "open",       py_make_builtin("open",       bi_open,       1,  2));
    py_global_define(c, "eval",       py_make_builtin("eval",       bi_eval,       1,  1));
    py_global_define(c, "exec",       py_make_builtin("exec",       bi_exec,       1,  3));
    py_global_define(c, "min",        py_make_builtin("min",        bi_min,        1, -1));
    py_global_define(c, "max",        py_make_builtin("max",        bi_max,        1, -1));
    py_global_define(c, "sum",        py_make_builtin("sum",        bi_sum,        1,  2));
    py_global_define(c, "sorted",     py_make_builtin("sorted",     bi_sorted,     1,  1));
    py_global_define(c, "enumerate",  py_make_builtin("enumerate",  bi_enumerate,  1,  2));
    py_global_define(c, "zip",        py_make_builtin("zip",        bi_zip,        0, -1));
    py_global_define(c, "chr",        py_make_builtin("chr",        bi_chr,        1,  1));
    py_global_define(c, "ord",        py_make_builtin("ord",        bi_ord,        1,  1));
    py_global_define(c, "hex",        py_make_builtin("hex",        bi_hex,        1,  1));
    py_global_define(c, "bin",        py_make_builtin("bin",        bi_bin,        1,  1));
    py_global_define(c, "oct",        py_make_builtin("oct",        bi_oct,        1,  1));
    py_global_define(c, "slice",      py_make_builtin("slice",      bi_slice,      1,  3));
    py_global_define(c, "memoryview", py_make_builtin("memoryview", bi_memoryview, 1,  1));
    py_global_define(c, "breakpoint", py_make_builtin("breakpoint", bi_breakpoint, 0, -1));
    py_global_define(c, "compile",    py_make_builtin("compile",    bi_compile,    3,  6));
    {
        struct pyobj *e = py_alloc(PY_T_ELLIPSIS);
        py_global_define(c, "Ellipsis",       PY_OBJ_VAL(e));
        struct pyobj *n = py_alloc(PY_T_NOTIMPL);
        py_global_define(c, "NotImplemented", PY_OBJ_VAL(n));
    }
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
    py_global_define(c, "iter",        py_make_builtin("iter",        bi_iter,       1, 2));
    py_global_define(c, "next",        py_make_builtin("next",        bi_next,       1, 2));

    // Synthetic Exception.__init__: sets self.args and self.message.
    // Lives as a builtin function on the class so user-defined
    // exception subclasses can `super().__init__(msg)`.
    extern VALUE bi_exception_init(CTX *c, int argc, VALUE *argv);
    // Built-in exception classes.  Hierarchy is BaseException root, but
    // for now everything inherits Exception → no base.
    c->EXC_BaseException    = py_make_class("BaseException",    PY_NONE, true);
    {
        VALUE init = py_make_builtin("__init__", bi_exception_init, 1, -1);
        py_class_add_method(c, c->EXC_BaseException, "__init__", init);
    }
    c->EXC_Exception         = py_make_class("Exception",        c->EXC_BaseException, true);
    c->EXC_SystemExit        = py_make_class("SystemExit",        c->EXC_BaseException, true);
    c->EXC_KeyboardInterrupt = py_make_class("KeyboardInterrupt", c->EXC_BaseException, true);
    c->EXC_GeneratorExit     = py_make_class("GeneratorExit",     c->EXC_BaseException, true);
    c->EXC_TypeError        = py_make_class("TypeError",        c->EXC_Exception, true);
    c->EXC_ValueError       = py_make_class("ValueError",       c->EXC_Exception, true);
    c->EXC_NameError        = py_make_class("NameError",        c->EXC_Exception, true);
    c->EXC_UnboundLocalError = py_make_class("UnboundLocalError", c->EXC_NameError, true);
    c->EXC_SystemError       = py_make_class("SystemError",      c->EXC_Exception, true);
    c->EXC_PendingDeprecationWarning = py_make_class("PendingDeprecationWarning", c->EXC_Exception, true);
    c->EXC_LookupError       = py_make_class("LookupError",       c->EXC_Exception, true);
    c->EXC_IndexError       = py_make_class("IndexError",       c->EXC_LookupError, true);
    c->EXC_KeyError         = py_make_class("KeyError",         c->EXC_LookupError, true);
    c->EXC_ArithmeticError   = py_make_class("ArithmeticError",  c->EXC_Exception, true);
    c->EXC_ZeroDivisionError= py_make_class("ZeroDivisionError",c->EXC_ArithmeticError, true);
    c->EXC_OverflowError     = py_make_class("OverflowError",    c->EXC_ArithmeticError, true);
    c->EXC_FloatingPointError= py_make_class("FloatingPointError",c->EXC_ArithmeticError, true);
    c->EXC_AttributeError   = py_make_class("AttributeError",   c->EXC_Exception, true);
    c->EXC_RuntimeError     = py_make_class("RuntimeError",     c->EXC_Exception, true);
    c->EXC_NotImplementedError= py_make_class("NotImplementedError", c->EXC_RuntimeError, true);
    c->EXC_RecursionError    = py_make_class("RecursionError",   c->EXC_RuntimeError, true);
    c->EXC_StopIteration    = py_make_class("StopIteration",    c->EXC_Exception, true);
    c->EXC_StopAsyncIteration= py_make_class("StopAsyncIteration",c->EXC_Exception, true);
    c->EXC_AssertionError   = py_make_class("AssertionError",   c->EXC_Exception, true);
    c->EXC_ImportError       = py_make_class("ImportError",       c->EXC_Exception, true);
    c->EXC_ModuleNotFoundError= py_make_class("ModuleNotFoundError", c->EXC_ImportError, true);
    c->EXC_OSError           = py_make_class("OSError",          c->EXC_Exception, true);
    c->EXC_FileNotFoundError = py_make_class("FileNotFoundError",c->EXC_OSError, true);
    c->EXC_PermissionError   = py_make_class("PermissionError",  c->EXC_OSError, true);
    c->EXC_NotADirectoryError= py_make_class("NotADirectoryError",c->EXC_OSError, true);
    c->EXC_IsADirectoryError = py_make_class("IsADirectoryError",c->EXC_OSError, true);
    c->EXC_TimeoutError      = py_make_class("TimeoutError",     c->EXC_OSError, true);
    c->EXC_BrokenPipeError   = py_make_class("BrokenPipeError",  c->EXC_OSError, true);
    c->EXC_InterruptedError  = py_make_class("InterruptedError", c->EXC_OSError, true);
    c->EXC_ConnectionError   = py_make_class("ConnectionError",  c->EXC_OSError, true);
    c->EXC_BlockingIOError   = py_make_class("BlockingIOError",  c->EXC_OSError, true);
    c->EXC_ChildProcessError = py_make_class("ChildProcessError",c->EXC_OSError, true);
    c->EXC_UnicodeError      = py_make_class("UnicodeError",     c->EXC_ValueError, true);
    c->EXC_UnicodeDecodeError= py_make_class("UnicodeDecodeError",c->EXC_UnicodeError, true);
    c->EXC_UnicodeEncodeError= py_make_class("UnicodeEncodeError",c->EXC_UnicodeError, true);
    c->EXC_MemoryError       = py_make_class("MemoryError",      c->EXC_Exception, true);
    c->EXC_BufferError       = py_make_class("BufferError",      c->EXC_Exception, true);
    c->EXC_ReferenceError    = py_make_class("ReferenceError",   c->EXC_Exception, true);
    c->EXC_SyntaxError       = py_make_class("SyntaxError",      c->EXC_Exception, true);
    c->EXC_IndentationError  = py_make_class("IndentationError", c->EXC_SyntaxError, true);
    c->EXC_TabError          = py_make_class("TabError",         c->EXC_IndentationError, true);
    c->EXC_EOFError          = py_make_class("EOFError",         c->EXC_Exception, true);

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
    py_global_define(c, "AssertionError",   c->EXC_AssertionError);
    py_global_define(c, "ImportError",          c->EXC_ImportError);
    py_global_define(c, "ModuleNotFoundError",  c->EXC_ModuleNotFoundError);
    py_global_define(c, "NotImplementedError",  c->EXC_NotImplementedError);
    py_global_define(c, "ArithmeticError",      c->EXC_ArithmeticError);
    py_global_define(c, "OverflowError",        c->EXC_OverflowError);
    py_global_define(c, "OSError",              c->EXC_OSError);
    py_global_define(c, "FileNotFoundError",    c->EXC_FileNotFoundError);
    py_global_define(c, "IOError",              c->EXC_OSError);    // alias
    py_global_define(c, "EnvironmentError",     c->EXC_OSError);    // alias
    py_global_define(c, "BaseException",        c->EXC_BaseException);
    py_global_define(c, "SystemExit",           c->EXC_SystemExit);
    py_global_define(c, "KeyboardInterrupt",    c->EXC_KeyboardInterrupt);
    py_global_define(c, "GeneratorExit",        c->EXC_GeneratorExit);
    py_global_define(c, "LookupError",          c->EXC_LookupError);
    py_global_define(c, "FloatingPointError",   c->EXC_FloatingPointError);
    py_global_define(c, "RecursionError",       c->EXC_RecursionError);
    py_global_define(c, "StopAsyncIteration",   c->EXC_StopAsyncIteration);
    py_global_define(c, "PermissionError",      c->EXC_PermissionError);
    py_global_define(c, "NotADirectoryError",   c->EXC_NotADirectoryError);
    py_global_define(c, "IsADirectoryError",    c->EXC_IsADirectoryError);
    py_global_define(c, "TimeoutError",         c->EXC_TimeoutError);
    py_global_define(c, "BrokenPipeError",      c->EXC_BrokenPipeError);
    py_global_define(c, "InterruptedError",     c->EXC_InterruptedError);
    py_global_define(c, "ConnectionError",      c->EXC_ConnectionError);
    py_global_define(c, "BlockingIOError",      c->EXC_BlockingIOError);
    py_global_define(c, "ChildProcessError",    c->EXC_ChildProcessError);
    py_global_define(c, "UnicodeError",         c->EXC_UnicodeError);
    py_global_define(c, "UnicodeDecodeError",   c->EXC_UnicodeDecodeError);
    py_global_define(c, "UnicodeEncodeError",   c->EXC_UnicodeEncodeError);
    py_global_define(c, "MemoryError",          c->EXC_MemoryError);
    py_global_define(c, "BufferError",          c->EXC_BufferError);
    py_global_define(c, "ReferenceError",       c->EXC_ReferenceError);
    py_global_define(c, "SyntaxError",          c->EXC_SyntaxError);
    py_global_define(c, "IndentationError",     c->EXC_IndentationError);
    py_global_define(c, "TabError",             c->EXC_TabError);
    py_global_define(c, "EOFError",             c->EXC_EOFError);
    py_global_define(c, "UnboundLocalError",    c->EXC_UnboundLocalError);
    py_global_define(c, "SystemError",          c->EXC_SystemError);
    py_global_define(c, "__pystro_del__",   py_make_builtin("__pystro_del__", bi_pystro_del, 2, 2));
    py_global_define(c, "__pystro_yield_from__", py_make_builtin("__pystro_yield_from__", bi_pystro_yield_from, 1, 1));
    py_global_define(c, "__pystro_pos__", py_make_builtin("__pystro_pos__", bi_pystro_pos, 1, 1));
    py_global_define(c, "__pystro_delattr__",   py_make_builtin("__pystro_delattr__", bi_pystro_delattr, 2, 2));
    py_global_define(c, "__pystro_delglobal__", py_make_builtin("__pystro_delglobal__", bi_pystro_delglobal, 1, 1));
    py_global_define(c, "__pystro_import__",    py_make_builtin("__pystro_import__", bi_import, 1, 1));
    py_global_define(c, "__import__",           py_make_builtin("__import__", bi_import, 1, 1));
    py_global_define(c, "__name__",             py_make_str("__main__", 8));
    py_global_define(c, "__pystro_import_star__", py_make_builtin("__pystro_import_star__", bi_import_star, 1, 1));
    // C-level math primitives, surfaced through the `math` module (math.py).
    py_global_define(c, "__pystro_sqrt__",  py_make_builtin("__pystro_sqrt__",  bi_pystro_sqrt,  1, 1));
    py_global_define(c, "__pystro_sin__",   py_make_builtin("__pystro_sin__",   bi_pystro_sin,   1, 1));
    py_global_define(c, "__pystro_cos__",   py_make_builtin("__pystro_cos__",   bi_pystro_cos,   1, 1));
    py_global_define(c, "__pystro_tan__",   py_make_builtin("__pystro_tan__",   bi_pystro_tan,   1, 1));
    py_global_define(c, "__pystro_log__",   py_make_builtin("__pystro_log__",   bi_pystro_log,   1, 2));
    py_global_define(c, "__pystro_exp__",   py_make_builtin("__pystro_exp__",   bi_pystro_exp,   1, 1));
    py_global_define(c, "__pystro_floor__", py_make_builtin("__pystro_floor__", bi_pystro_floor, 1, 1));
    py_global_define(c, "__pystro_ceil__",  py_make_builtin("__pystro_ceil__",  bi_pystro_ceil,  1, 1));
    py_global_define(c, "__pystro_atan2__", py_make_builtin("__pystro_atan2__", bi_pystro_atan2, 2, 2));
    py_global_define(c, "__pystro_pow__",   py_make_builtin("__pystro_pow__",   bi_pystro_pow,   2, 2));
    py_global_define(c, "__pystro_argv__",  py_make_builtin("__pystro_argv__",  bi_pystro_argv,  0, 0));
    py_global_define(c, "__pystro_exit__",  py_make_builtin("__pystro_exit__",  bi_pystro_exit,  0, 1));
    py_global_define(c, "__pystro_current_exc__",
                     py_make_builtin("__pystro_current_exc__", bi_pystro_current_exc, 0, 0));
    py_global_define(c, "__pystro_float_to_bits__",
                     py_make_builtin("__pystro_float_to_bits__", bi_pystro_float_to_bits, 1, 2));
    py_global_define(c, "__pystro_bits_to_float__",
                     py_make_builtin("__pystro_bits_to_float__", bi_pystro_bits_to_float, 1, 2));
    py_global_define(c, "__pystro_time__",      py_make_builtin("__pystro_time__",      bi_pystro_time,      0, 0));
    py_global_define(c, "__pystro_sleep__",     py_make_builtin("__pystro_sleep__",     bi_pystro_sleep,     1, 1));
    py_global_define(c, "__pystro_perf_counter__", py_make_builtin("__pystro_perf_counter__", bi_pystro_perf_counter, 0, 0));
    py_global_define(c, "__pystro_getenv__",    py_make_builtin("__pystro_getenv__",    bi_pystro_getenv,    1, 2));
    py_global_define(c, "__pystro_getcwd__",    py_make_builtin("__pystro_getcwd__",    bi_pystro_getcwd,    0, 0));
    py_global_define(c, "__pystro_path_exists__", py_make_builtin("__pystro_path_exists__", bi_pystro_path_exists, 1, 1));
    py_global_define(c, "__pystro_md5__",     py_make_builtin("__pystro_md5__",     bi_pystro_md5,     1, 1));
    py_global_define(c, "__pystro_sha256__",  py_make_builtin("__pystro_sha256__",  bi_pystro_sha256,  1, 1));
    py_global_define(c, "__pystro_listdir__",     py_make_builtin("__pystro_listdir__",     bi_pystro_listdir,     0, 1));
    py_global_define(c, "__pystro_remove__",      py_make_builtin("__pystro_remove__",      bi_pystro_remove,      1, 1));
    py_global_define(c, "__pystro_makedirs__",    py_make_builtin("__pystro_makedirs__",    bi_pystro_makedirs,    1, 2));
    py_global_define(c, "__pystro_isdir__",       py_make_builtin("__pystro_isdir__",       bi_pystro_isdir,       1, 1));
    py_global_define(c, "__pystro_isfile__",      py_make_builtin("__pystro_isfile__",      bi_pystro_isfile,      1, 1));
    py_global_define(c, "__pystro_abspath__",     py_make_builtin("__pystro_abspath__",     bi_pystro_abspath,     1, 1));

    c->current_class = PY_NONE;
}
