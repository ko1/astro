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

struct pysobj PYS_NONE_OBJ  = { .type = PYS_T_NONE };
struct pysobj PYS_TRUE_OBJ  = { .type = PYS_T_BOOL, .b = true };
struct pysobj PYS_FALSE_OBJ = { .type = PYS_T_BOOL, .b = false };

// Set by main() at startup so the parameterless pys_display can call
// instance __str__ / __repr__ without changing its signature (called
// from many places, including recursively from list/dict display).
CTX *pys_current_ctx = NULL;

// ---------------------------------------------------------------------------
// GC + GMP.
// ---------------------------------------------------------------------------

static void *gmp_alloc  (size_t sz)                  { return GC_malloc(sz); }
static void *gmp_realloc(void *p, size_t old, size_t nw) { (void)old; return GC_realloc(p, nw); }
static void  gmp_free   (void *p, size_t sz)         { (void)p; (void)sz; /* GC sweeps */ }

static void
pys_gc_init(void)
{
    // GC_INITIAL_HEAP_SIZE — request a large starting heap before
    // GC_init().  Setting via env (rather than GC_expand_hp post-init)
    // avoids a heap-corruption window during the first collection
    // after expansion that flakily segfaulted at module-init shutdown
    // (test_errno + test_typechecks ~70% reproducer).
    //
    // 128 MiB default (was 64 MiB): test_dict / test_userdict / test_set
    // bench-scale tests grow past 64 MiB and hit a libgc.so internal
    // SEGV (mark phase NULL deref, ~5/6 reproduction) when the heap
    // expands.  Setting the initial heap large enough that no expansion
    // happens during a typical test_*.py run sidesteps the Boehm bug.
    // Users can override with GC_INITIAL_HEAP_SIZE for memory-tight
    // setups.
    if (!getenv("GC_INITIAL_HEAP_SIZE"))
        setenv("GC_INITIAL_HEAP_SIZE", "134217728", 0);   // 128 MiB
    GC_init();
    GC_set_free_space_divisor(1);
    mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
}

// ---------------------------------------------------------------------------
// Heap allocation helpers.
// ---------------------------------------------------------------------------

// Per-type allocation size: struct pysobj's union is 312 bytes (PYS_T_CLASS
// dominates due to dunder slots).  For an instance / float / list /
// dict / etc. we only need ~16-32 bytes.  Per-type sizing saves up to
// 280 bytes / instance — for raytrace's ~1M Vector allocations that's
// ~280 MB of avoided GC pressure.
static inline size_t pyobj_size_for(int type) {
    #define VARIANT_END(member) (offsetof(struct pysobj, member) + sizeof(((struct pysobj *)0)->member))
    switch (type) {
      case PYS_T_NONE:
      case PYS_T_BOOL:
      case PYS_T_ELLIPSIS:
      case PYS_T_NOTIMPL:
        return offsetof(struct pysobj, b) + sizeof(bool);
      case PYS_T_FLOAT:        return VARIANT_END(dbl);
      case PYS_T_BIGNUM:       return VARIANT_END(mpz);
      case PYS_T_COMPLEX:      return VARIANT_END(cpx);
      case PYS_T_STR:
      case PYS_T_BYTES:
      case PYS_T_BYTEARRAY:    return VARIANT_END(str);
      case PYS_T_MODULE:       return VARIANT_END(module);
      case PYS_T_LIST:
      case PYS_T_TUPLE:        return VARIANT_END(list);
      case PYS_T_DICT:
      case PYS_T_SET:
      case PYS_T_FROZENSET:    return VARIANT_END(dict);
      case PYS_T_RANGE:        return VARIANT_END(range);
      case PYS_T_FUNC:         return VARIANT_END(func);
      case PYS_T_BUILTIN:      return VARIANT_END(builtin);
      case PYS_T_BOUND_METHOD: return VARIANT_END(bound);
      case PYS_T_STATICMETHOD:
      case PYS_T_CLASSMETHOD:
      case PYS_T_PROPERTY:     return VARIANT_END(wrap);
      case PYS_T_ITER:         return VARIANT_END(iter_state);
      case PYS_T_GEN:          return VARIANT_END(gen);
      case PYS_T_FILE:         return VARIANT_END(file);
      case PYS_T_SUPER:        return VARIANT_END(super_);
      case PYS_T_SLICE:        return VARIANT_END(slice_);
      case PYS_T_MEMVIEW:      return VARIANT_END(memview);
      case PYS_T_INSTANCE:     return VARIANT_END(inst);
      case PYS_T_CLASS:        return VARIANT_END(cls);
      default:                return sizeof(struct pysobj);
    }
    #undef VARIANT_END
}

struct pysobj *
pys_alloc(int type)
{
    struct pysobj *o = (struct pysobj *)GC_malloc(pyobj_size_for(type));
    o->type = type;
    return o;
}

VALUE
pys_make_float(double d)
{
    VALUE inline_v = pys_try_flonum(d);
    if (LIKELY(inline_v)) return inline_v;
    struct pysobj *o = pys_alloc(PYS_T_FLOAT);
    o->dbl = d;
    return PYS_OBJ_VAL(o);
}

// Apply a user-supplied metaclass to a class that's already been
// constructed via pys_make_class.  Builds attrs dict from the class's
// methods, calls metaclass(name, (base,), attrs), and returns the
// metaclass's result (or the original class if metaclass returns
// non-class — that's incompatible with Python but pragmatic).
extern VALUE pys_class_lookup_method(VALUE cls, const char *name);

// Pre-interned dunder names (filled by install_builtins).
extern const char *PYS_INTERN_init, *PYS_INTERN_new, *PYS_INTERN_eq,
    *PYS_INTERN_lt, *PYS_INTERN_hash, *PYS_INTERN_setattr,
    *PYS_INTERN_getattr, *PYS_INTERN_getattribute, *PYS_INTERN_bool,
    *PYS_INTERN_len, *PYS_INTERN_getitem, *PYS_INTERN_setitem,
    *PYS_INTERN_index, *PYS_INTERN_invert, *PYS_INTERN_neg,
    *PYS_INTERN_metaclass, *PYS_INTERN_set_name, *PYS_INTERN_iter,
    *PYS_INTERN_next, *PYS_INTERN_call, *PYS_INTERN_get,
    *PYS_INTERN_repr, *PYS_INTERN_str, *PYS_INTERN_contains,
    // Math/comparison dunders for pys_add/pys_sub/etc and pys_eq/pys_cmp.
    *PYS_INTERN_add, *PYS_INTERN_sub, *PYS_INTERN_mul,
    *PYS_INTERN_truediv, *PYS_INTERN_floordiv, *PYS_INTERN_mod,
    *PYS_INTERN_pow, *PYS_INTERN_or, *PYS_INTERN_and,
    *PYS_INTERN_xor, *PYS_INTERN_lshift, *PYS_INTERN_rshift,
    *PYS_INTERN_radd, *PYS_INTERN_rsub, *PYS_INTERN_rmul,
    *PYS_INTERN_iadd, *PYS_INTERN_isub, *PYS_INTERN_imul,
    *PYS_INTERN_le, *PYS_INTERN_gt, *PYS_INTERN_ge,
    *PYS_INTERN_ne;
VALUE
pys_class_meta_apply(CTX *c, VALUE cls, VALUE meta, const char *name)
{
    if (!pys_is_class(cls)) return cls;
    if (meta == PYS_NONE) return cls;
    // Build attrs dict from class methods.
    VALUE attrs = pys_make_dict();
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    for (int i = 0; i < cd->nmethods; i++) {
        VALUE k = pys_make_str(cd->methods[i].name, strlen(cd->methods[i].name));
        pys_dict_set(c, attrs, k, cd->methods[i].value);
    }
    // Bases tuple.
    VALUE *bv = (VALUE *)alloca(sizeof(VALUE) * (cd->nbases ? cd->nbases : 1));
    for (int i = 0; i < cd->nbases; i++) bv[i] = cd->bases[i];
    VALUE bases_tuple = pys_make_tuple(bv, cd->nbases);
    VALUE name_v = pys_make_str(name, strlen(name));

    extern const char *intern_name(const char *s, size_t len);
    // If metaclass is a user class with __new__, call __new__(meta, ...).
    // The __new__ implementor is expected to return a class (typically by
    // calling type(name, bases, attrs)).
    if (pys_is_class(meta)) {
        VALUE new_m = pys_class_lookup_method(meta, PYS_INTERN_new);
        if (new_m != PYS_NONE) {
            VALUE av[4] = { meta, name_v, bases_tuple, attrs };
            VALUE r = pys_apply(c, new_m, 4, av);
            if (c->state == PYS_STATE_RAISE) return 0;
            // If __init__ is also defined, call it on the new class.
            if (pys_is_class(r)) {
                VALUE init_m = pys_class_lookup_method(meta, PYS_INTERN_init);
                if (init_m != PYS_NONE) {
                    VALUE iav[4] = { r, name_v, bases_tuple, attrs };
                    pys_apply(c, init_m, 4, iav);
                    if (c->state == PYS_STATE_RAISE) return 0;
                }
                // Stamp metaclass for inheritance.
                pys_class_add_method(c, r, intern_name("__metaclass__", 13), meta);
                return r;
            }
        }
    }
    // No __new__ override on the metaclass.  Just stamp the metaclass
    // on the original class and return it — this lets subsequent `cls(...)`
    // calls dispatch to meta.__call__ if defined.
    if (pys_is_class(meta)) {
        pys_class_add_method(c, cls, intern_name("__metaclass__", 13), meta);
        // Run __init__(cls, name, bases, attrs) if defined.
        VALUE init_m = pys_class_lookup_method(meta, PYS_INTERN_init);
        if (init_m != PYS_NONE) {
            VALUE iav[4] = { cls, name_v, bases_tuple, attrs };
            pys_apply(c, init_m, 4, iav);
            if (c->state == PYS_STATE_RAISE) return 0;
        }
        return cls;
    }
    // Otherwise just call metaclass(name, bases, attrs).  For builtin
    // `type` this hits the 3-arg form (creates a class).
    VALUE av[3] = { name_v, bases_tuple, attrs };
    VALUE result = pys_apply(c, meta, 3, av);
    if (c->state == PYS_STATE_RAISE) return 0;
    if (pys_is_class(result)) {
        pys_class_add_method(c, result, intern_name("__metaclass__", 13), meta);
        return result;
    }
    return cls;
}

// Walk class methods for `__slots__` and stash the parsed name list on
// the class.  Called after the class body has finished evaluating so
// the slots tuple is already on the class.
void
pys_class_extract_slots(CTX *c, VALUE cls)
{
    // Look up __slots__ ONLY on the class itself, not inherited.
    struct pysclass *cd0 = &PYS_PTR(cls)->cls;
    VALUE sv = PYS_NONE;
    for (int j = 0; j < cd0->nmethods; j++) {
        if (strcmp(cd0->methods[j].name, "__slots__") == 0) {
            sv = cd0->methods[j].value;
            break;
        }
    }
    if (sv == PYS_NONE) return;
    // sv may be tuple/list/str/iterable.  Treat str specially: single name.
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    if (pys_is_str(sv)) {
        cd->slots = (const char **)GC_malloc(sizeof(char *) * 1);
        cd->slots[0] = PYS_PTR(sv)->str.chars;
        cd->nslots = 1;
        return;
    }
    if (pys_is_list(sv) || pys_is_tuple(sv)) {
        size_t n = PYS_PTR(sv)->list.len;
        cd->slots = (const char **)GC_malloc(sizeof(char *) * (n ? n : 1));
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            VALUE v = PYS_PTR(sv)->list.items[i];
            if (pys_is_str(v)) cd->slots[k++] = PYS_PTR(v)->str.chars;
        }
        cd->nslots = (int)k;
        return;
    }
    (void)c;
}

// True if `cls` (or any ancestor) has __slots__ declared, and `name`
// isn't in any __slots__ list.  Used by pys_setattr to enforce.
bool
pys_class_has_slots_anywhere(VALUE cls)
{
    if (!pys_is_class(cls)) return false;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) {
        struct pysclass *kd = &PYS_PTR(cd->mro[i])->cls;
        if (kd->slots) return true;
    }
    return false;
}

static bool
pys_class_slot_allowed(VALUE cls, const char *name)
{
    if (!pys_is_class(cls)) return true;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    bool any_slots = false;
    for (int i = 0; i < cd->nmro; i++) {
        struct pysclass *kd = &PYS_PTR(cd->mro[i])->cls;
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
        struct pysclass *kd = &PYS_PTR(cd->mro[i])->cls;
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

static bool  pydict_entry_live(const struct pysdict *d, size_t i);

// If any of `bases` (or their ancestors) has a `__metaclass__` attribute,
// apply it to `cls` (the freshly-built class).  Returns either `cls` or
// the metaclass-produced replacement.
VALUE
pys_class_inherit_metaclass(CTX *c, VALUE cls, VALUE *bases, int nbases, const char *name)
{
    VALUE final_cls = cls;
    for (int i = 0; i < nbases; i++) {
        VALUE b = bases[i];
        if (!pys_is_class(b)) continue;
        VALUE meta = pys_class_lookup_method(b, PYS_INTERN_metaclass);
        if (meta != PYS_NONE && pys_is_class(meta)) {
            final_cls = pys_class_meta_apply(c, cls, meta, name);
            break;
        }
    }
    // __init_subclass__ — invoked on a base class when it gets subclassed.
    // Walk MRO above `cls` itself; first defining class wins.
    if (pys_is_class(final_cls)) {
        struct pysclass *cd = &PYS_PTR(final_cls)->cls;
        for (int j = 1; j < cd->nmro; j++) {
            VALUE base = cd->mro[j];
            if (!pys_is_class(base)) continue;
            struct pysclass *bd = &PYS_PTR(base)->cls;
            VALUE found = PYS_NONE;
            for (int k = 0; k < bd->nmethods; k++) {
                if (strcmp(bd->methods[k].name, "__init_subclass__") == 0) {
                    found = bd->methods[k].value;
                    break;
                }
            }
            if (found != PYS_NONE) {
                // Unwrap classmethod / staticmethod descriptors —
                // __init_subclass__ is implicit classmethod, and users
                // sometimes write @classmethod explicitly.
                if (PYS_IS_PTR(found) && PYS_PTR(found)->type == PYS_T_CLASSMETHOD)
                    found = PYS_PTR(found)->wrap.wrapped;
                else if (PYS_IS_PTR(found) && PYS_PTR(found)->type == PYS_T_STATICMETHOD)
                    found = PYS_PTR(found)->wrap.wrapped;
                // Forward class-level kwargs (`class C(B, foo=1)`) as
                // keyword args to __init_subclass__.
                struct pysclass *fcd = &PYS_PTR(final_cls)->cls;
                VALUE kw_dict = PYS_NONE;
                for (int kk = 0; kk < fcd->nmethods; kk++) {
                    if (strcmp(fcd->methods[kk].name, "__class_kwargs__") == 0) {
                        kw_dict = fcd->methods[kk].value;
                        break;
                    }
                }
                if (kw_dict != PYS_NONE && pys_is_dict(kw_dict)) {
                    struct pysdict *dd = PYS_PTR(kw_dict)->dict;
                    int nkw = 0;
                    const char **kn = NULL; VALUE *kv = NULL;
                    if (dd->used > 0) {
                        kn = (const char **)alloca(sizeof(char *) * dd->used);
                        kv = (VALUE *)alloca(sizeof(VALUE) * dd->used);
                        for (size_t ii = 0; ii < dd->elen; ii++) {
                            if (!pydict_entry_live(dd, ii)) continue;
                            VALUE k = dd->entries[ii].key;
                            if (!pys_is_str(k)) continue;
                            kn[nkw] = PYS_PTR(k)->str.chars;
                            kv[nkw] = dd->entries[ii].value;
                            nkw++;
                        }
                    }
                    extern VALUE pys_apply_kw(CTX *c, VALUE fn, int argc, VALUE *argv,
                                             int kwc, const char **kwnames, VALUE *kwvalues);
                    VALUE av[1] = { final_cls };
                    pys_apply_kw(c, found, 1, av, nkw, kn, kv);
                } else {
                    VALUE av[1] = { final_cls };
                    pys_apply(c, found, 1, av);
                }
                if (c->state == PYS_STATE_RAISE) return 0;
                break;
            }
        }
    }
    return final_cls;
}

VALUE
pys_make_super(VALUE start_cls, VALUE self)
{
    struct pysobj *o = pys_alloc(PYS_T_SUPER);
    o->super_.start_cls = start_cls;
    o->super_.self = self;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_complex(double re, double im)
{
    struct pysobj *o = pys_alloc(PYS_T_COMPLEX);
    o->cpx.re = re;
    o->cpx.im = im;
    return PYS_OBJ_VAL(o);
}

// Get (re, im) of v if it's a number; for non-complex, im=0.  Returns
// false if v isn't numeric.
static bool
pys_to_cpx(CTX *c, VALUE v, double *re, double *im)
{
    (void)c;
    if (pys_is_complex(v)) { *re = PYS_PTR(v)->cpx.re; *im = PYS_PTR(v)->cpx.im; return true; }
    if (PYS_IS_FIXNUM(v)) { *re = (double)PYS_FIXVAL(v); *im = 0; return true; }
    if (v == PYS_TRUE)    { *re = 1; *im = 0; return true; }
    if (v == PYS_FALSE)   { *re = 0; *im = 0; return true; }
    if (PYS_IS_FLONUM(v)) { *re = pys_flonum_to_double(v); *im = 0; return true; }
    if (pys_is_heap_float(v)) { *re = PYS_PTR(v)->dbl; *im = 0; return true; }
    if (pys_is_bignum(v)) { *re = mpz_get_d(PYS_PTR(v)->mpz); *im = 0; return true; }
    return false;
}

VALUE
pys_make_bignum(mpz_srcptr z)
{
    struct pysobj *o = pys_alloc(PYS_T_BIGNUM);
    mpz_init_set(o->mpz, z);
    return PYS_OBJ_VAL(o);
}

// Normalise: bignum that fits → fixnum.
static VALUE
pys_normalise_int(mpz_srcptr z)
{
    if (mpz_fits_slong_p(z)) {
        long v = mpz_get_si(z);
        if (v >= PYS_FIXNUM_MIN && v <= PYS_FIXNUM_MAX) return PYS_FIX(v);
    }
    return pys_make_bignum(z);
}

// Out-of-line bignum boxing.  Caller has already verified v is outside
// fixnum range; we just construct the heap mpz.
VALUE
pys_make_int_bignum(int64_t v)
{
    mpz_t z; mpz_init(z);
    mpz_set_si(z, (long)v);     // covers up to long range
    if ((int64_t)(long)v != v) {
        // Wider than long — set via string.
        char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
        mpz_set_str(z, buf, 10);
    }
    VALUE r = pys_make_bignum(z);
    mpz_clear(z);
    return r;
}

VALUE
pys_make_str(const char *s, size_t len)
{
    struct pysobj *o = pys_alloc(PYS_T_STR);
    char *buf = (char *)GC_malloc_atomic(len + 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    o->str.chars = buf;
    o->str.len = len;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_str_take(char *s, size_t len)
{
    struct pysobj *o = pys_alloc(PYS_T_STR);
    o->str.chars = s;
    o->str.len = len;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_bytes(const char *s, size_t len)
{
    struct pysobj *o = pys_alloc(PYS_T_BYTES);
    char *buf = (char *)GC_malloc_atomic(len + 1);
    if (s && len) memcpy(buf, s, len);
    buf[len] = '\0';
    o->str.chars = buf;
    o->str.len = len;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_bytearray(const char *s, size_t len)
{
    struct pysobj *o = pys_alloc(PYS_T_BYTEARRAY);
    char *buf = (char *)GC_malloc_atomic(len + 1);
    if (s && len) memcpy(buf, s, len);
    buf[len] = '\0';
    o->str.chars = buf;
    o->str.len = len;
    return PYS_OBJ_VAL(o);
}

// Share the buffer of an existing string (e.g. a substring / slice).
// Boehm's interior-pointer support keeps the parent buffer alive as
// long as any sub-string holds a pointer into it.  No NUL terminator
// is required (str.len is authoritative); the only places that touch
// trailing chars are pys_display (uses fwrite + len) and the numeric
// converters (which copy out first).
//
// Allocates only the bytes a string-typed pysobj actually uses (type +
// chars + len) — Boehm buckets requests by size and a 24-byte block is
// dramatically smaller than a full sizeof(struct pysobj) (which is
// dominated by the union's biggest member, the func / class struct).
static const size_t pys_str_size = offsetof(struct pysobj, str) + sizeof(((struct pysobj *)0)->str);

// UTF-8 codepoint helpers (defined later in file).
extern size_t pys_str_cp_count(const char *s, size_t bytelen);
extern size_t pys_str_cp_to_byte(const char *s, size_t bytelen, int64_t cp_idx);
extern size_t pys_str_byte_to_cp(const char *s, size_t byte_off);

static VALUE
pys_make_str_borrow(const char *src, size_t len)
{
    struct pysobj *o = (struct pysobj *)GC_malloc(pys_str_size);
    o->type = PYS_T_STR;
    o->str.chars = (char *)src;
    o->str.len = len;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_list(VALUE *items, size_t n)
{
    struct pysobj *o = pys_alloc(PYS_T_LIST);
    size_t capa = n < 4 ? 4 : n;
    o->list.items = (VALUE *)GC_malloc(sizeof(VALUE) * capa);
    if (n) memcpy(o->list.items, items, sizeof(VALUE) * n);
    o->list.len = n;
    o->list.capa = capa;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_tuple(VALUE *items, size_t n)
{
    struct pysobj *o = pys_alloc(PYS_T_TUPLE);
    o->list.items = n ? (VALUE *)GC_malloc(sizeof(VALUE) * n) : NULL;
    if (n) memcpy(o->list.items, items, sizeof(VALUE) * n);
    o->list.len = n;
    o->list.capa = n;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_range(int64_t start, int64_t stop, int64_t step)
{
    struct pysobj *o = pys_alloc(PYS_T_RANGE);
    o->range.start = start;
    o->range.stop  = stop;
    o->range.step  = step;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_func(struct Node *body, struct pysframe *env,
             const char *name, int nparams, int n_pos_named,
             int nlocals, VALUE *defaults_per_slot, bool leaf,
             const char **param_names,
             bool has_varargs, bool has_kwargs,
             bool is_generator)
{
    // Register the body so AOT bake (`pystro -c`) can iterate each
    // function's entry point and emit a per-body SD_<hash>.c.  The
    // dispatcher swap itself happens in OPTIMIZE() during NODE
    // allocation (parser-time), so no astro_cs_load needed here.
    extern void code_repo_add(const char *name, struct Node *body, bool force);
    if (body) code_repo_add(name, body, false);
    struct pysobj *o = pys_alloc(PYS_T_FUNC);
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
    // may pass a pointer into PYS_NAME_TABLE which can be moved by
    // a later GC_realloc; per-func storage is stable.
    if (param_names && nparams > 0) {
        const char **pn = (const char **)GC_malloc(sizeof(char *) * nparams);
        for (int i = 0; i < nparams; i++) pn[i] = param_names[i];
        o->func.param_names = pn;
    } else {
        o->func.param_names = NULL;
    }
    // node_def / node_lambda set this immediately after via a setter that
    // COPIES into per-func storage (the table pointer can move under
    // GC_realloc).  Default to NULL so locals() is a safe no-op for funcs
    // created via other paths (built-in shims, etc.).
    o->func.local_names = NULL;
    o->func.defining_class = PYS_NONE;
    extern CTX *pys_current_ctx;
    o->func.fglobals = pys_current_ctx ? pys_current_ctx->globals : NULL;
    if (nparams > 0) {
        VALUE *d = (VALUE *)GC_malloc(sizeof(VALUE) * nparams);
        if (defaults_per_slot) memcpy(d, defaults_per_slot, sizeof(VALUE) * nparams);
        else for (int i = 0; i < nparams; i++) d[i] = (VALUE)0;
        o->func.defaults = d;
    } else {
        o->func.defaults = NULL;
    }
    return PYS_OBJ_VAL(o);
}

// Copy `n` local-name pointers into per-func GC storage and attach.
// node_def / node_lambda call this immediately after pys_make_func so
// the function value owns a stable name array regardless of later
// PYS_LOCAL_NAMES_TABLE realloc.
void
pys_func_set_local_names(VALUE fn, const char **names, int n)
{
    if (!fn || !PYS_IS_PTR(fn) || PYS_PTR(fn)->type != PYS_T_FUNC) return;
    if (!names || n <= 0) { PYS_PTR(fn)->func.local_names = NULL; return; }
    const char **ln = (const char **)GC_malloc(sizeof(char *) * n);
    for (int i = 0; i < n; i++) ln[i] = names[i];
    PYS_PTR(fn)->func.local_names = ln;
}

VALUE
pys_make_builtin(const char *name, pys_builtin_fn fn, int min_argc, int max_argc)
{
    struct pysobj *o = pys_alloc(PYS_T_BUILTIN);
    o->builtin.fn = fn;
    o->builtin.name = name;
    o->builtin.min_argc = min_argc;
    o->builtin.max_argc = max_argc;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_bound(VALUE self, VALUE func)
{
    struct pysobj *o = pys_alloc(PYS_T_BOUND_METHOD);
    o->bound.self = self;
    o->bound.func = func;
    return PYS_OBJ_VAL(o);
}

// C3 linearization (Python's MRO algorithm).  Builds:
//   L[C(B1,...,Bn)] = C + merge(L[B1], ..., L[Bn], [B1, ..., Bn])
// where merge picks at each step the head of some list that does not
// appear in the tail of any other list.  If no such head exists, the
// hierarchy is inconsistent and we fall back to a simple BFS order.
void
pys_compute_mro(VALUE cls)
{
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    if (cd->nbases == 0) {
        // Implicit object base, except for object itself.
        extern CTX *pys_current_ctx;
        VALUE obj_cls = pys_current_ctx ? pys_current_ctx->TYPE_object : (VALUE)0;
        if (obj_cls && obj_cls != PYS_NONE && pys_is_class(obj_cls) && cls != obj_cls) {
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
        struct pysclass *b = &PYS_PTR(cd->bases[i])->cls;
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
    cd->slots_initialized = false;       // lazy fill on first lookup
}

VALUE
pys_make_builtin_class(const char *name, pys_builtin_fn ctor, int tag)
{
    struct pysobj *o = pys_alloc(PYS_T_CLASS);
    o->cls.name = name;
    o->cls.methods = NULL;
    o->cls.nmethods = 0;
    o->cls.methods_capa = 0;
    o->cls.is_exception = false;
    o->cls.base = PYS_NONE;
    o->cls.bases = NULL;
    o->cls.nbases = 0;
    o->cls.builtin_ctor = ctor;
    o->cls.builtin_tag = tag;
    extern void pys_compute_mro(VALUE cls);
    pys_compute_mro(PYS_OBJ_VAL(o));
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_class(const char *name, VALUE base, bool is_exception)
{
    struct pysobj *o = pys_alloc(PYS_T_CLASS);
    o->cls.name = name;
    o->cls.methods = NULL;
    o->cls.nmethods = 0;
    o->cls.methods_capa = 0;
    o->cls.is_exception = is_exception;
    o->cls.builtin_ctor = NULL;
    o->cls.builtin_tag = -1;
    // If base is a built-in type constructor (PYS_T_BUILTIN named "list",
    // "int", etc.), pystro can't really do built-in subclassing — drop
    // the base silently so the class definition at least parses and
    // the new class can hold its own methods/state.  Real method
    // delegation requires composition.
    if (base != PYS_NONE && !pys_is_class(base)) base = PYS_NONE;
    o->cls.base = base;
    if (base == PYS_NONE) {
        o->cls.bases = NULL;
        o->cls.nbases = 0;
    } else {
        o->cls.bases = (VALUE *)GC_malloc(sizeof(VALUE));
        o->cls.bases[0] = base;
        o->cls.nbases = 1;
    }
    pys_compute_mro(PYS_OBJ_VAL(o));
    return PYS_OBJ_VAL(o);
}

// Used when class C(A, B, ...): multi-inheritance.  Replaces the
// single-base bases[] array with the full list.  Caller passes the
// already-built class and its full base list.  Updates `is_exception`
// if any base is an exception class.
void
pys_class_set_bases(VALUE cls, VALUE *bases, int n)
{
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    cd->bases = n > 0 ? (VALUE *)GC_malloc(sizeof(VALUE) * n) : NULL;
    for (int i = 0; i < n; i++) cd->bases[i] = bases[i];
    cd->nbases = n;
    cd->base = n > 0 ? bases[0] : PYS_NONE;
    for (int i = 0; i < n; i++)
        if (pys_is_class(bases[i]) && PYS_PTR(bases[i])->cls.is_exception)
            cd->is_exception = true;
    pys_compute_mro(cls);
}

void
pys_class_add_method(CTX *c, VALUE cls, const char *name, VALUE fn)
{
    // Stamp the method's defining class so cooperative super() can
    // walk MRO from this point.
    if (PYS_IS_PTR(fn) && PYS_PTR(fn)->type == PYS_T_FUNC)
        PYS_PTR(fn)->func.defining_class = cls;
    else if (PYS_IS_PTR(fn) && (PYS_PTR(fn)->type == PYS_T_STATICMETHOD
                            || PYS_PTR(fn)->type == PYS_T_CLASSMETHOD
                            || PYS_PTR(fn)->type == PYS_T_PROPERTY)) {
        VALUE inner = PYS_PTR(fn)->wrap.wrapped;
        if (PYS_IS_PTR(inner) && PYS_PTR(inner)->type == PYS_T_FUNC)
            PYS_PTR(inner)->func.defining_class = cls;
    }
    // __set_name__ — if `fn` is a user instance with __set_name__,
    // call it now so descriptors can capture their attribute name.
    if (PYS_IS_PTR(fn) && PYS_PTR(fn)->type == PYS_T_INSTANCE) {
        VALUE sn = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(fn)->inst.cls), PYS_INTERN_set_name);
        if (sn != PYS_NONE) {
            VALUE av[3] = { fn, cls, pys_make_str(name, strlen(name)) };
            pys_apply(c, sn, 3, av);
            // Ignore raise — but propagate the state.
            if (c->state == PYS_STATE_RAISE) return;
        }
    }
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    // Replace if a method with the same name already exists — required
    // for decorator-wrapped methods (`@classmethod\ndef m`) which first
    // register the plain func and then re-register the wrapped version.
    for (int i = 0; i < cd->nmethods; i++) {
        if (strcmp(cd->methods[i].name, name) == 0) {
            cd->methods[i].value = fn;
            cd->slots_initialized = false;     // lazy refresh on next lookup
            cd->shape_version++;               // invalidate class-attr caches
            return;
        }
    }
    if (cd->nmethods == cd->methods_capa) {
        int cap = cd->methods_capa ? cd->methods_capa * 2 : 4;
        cd->methods = (struct pysclass_method *)GC_realloc(
            cd->methods, sizeof(struct pysclass_method) * cap);
        cd->methods_capa = cap;
    }
    cd->methods[cd->nmethods].name = name;
    cd->methods[cd->nmethods].value = fn;
    cd->nmethods++;
    cd->slots_initialized = false;       // lazy refresh on next lookup
    cd->shape_version++;                 // invalidate attr_cache stamps
}

// Walk the C3-linearised MRO and return the first matching method.
// `cls.mro[]` is computed at class creation time and includes self at
// index 0 + all bases in MRO order.
// Pointer-compare-first lookup.  All method names added via
// `pys_class_add_method` are interned (caller passes intern_name(...)),
// so when the lookup name is also interned we avoid strcmp entirely on
// the hit path.  Fall back to strcmp for safety when names happen not
// to be interned (e.g., a few literal-string call sites in runtime.c).
// Slow path: actual MRO walk + (pointer-or-strcmp) compare.  Used by
// the public lookup as a fallback for non-dunder names, and by
// `pyclass_refresh_slots` to populate the dunder slots.
static VALUE
pys_class_lookup_method_slow(VALUE cls, const char *name)
{
    if (cls == PYS_NONE || !pys_is_class(cls)) return PYS_NONE;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) {
        struct pysclass *kd = &PYS_PTR(cd->mro[i])->cls;
        for (int j = 0; j < kd->nmethods; j++) {
            const char *mn = kd->methods[j].name;
            if (mn == name || strcmp(mn, name) == 0)
                return kd->methods[j].value;
        }
    }
    return PYS_NONE;
}

// Recompute the dunder-slot fields from the current MRO + method tables.
// Called on class creation (after pys_compute_mro), every pys_class_add_method,
// and when MRO is recomputed.  All names compared by interned-pointer
// equality (PYS_INTERN_*).
void
pyclass_refresh_slots(VALUE cls)
{
    if (cls == PYS_NONE || !pys_is_class(cls)) return;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    cd->slot_init          = pys_class_lookup_method_slow(cls, PYS_INTERN_init);
    cd->slot_new           = pys_class_lookup_method_slow(cls, PYS_INTERN_new);
    cd->slot_eq            = pys_class_lookup_method_slow(cls, PYS_INTERN_eq);
    cd->slot_lt            = pys_class_lookup_method_slow(cls, PYS_INTERN_lt);
    cd->slot_hash          = pys_class_lookup_method_slow(cls, PYS_INTERN_hash);
    cd->slot_bool          = pys_class_lookup_method_slow(cls, PYS_INTERN_bool);
    cd->slot_len           = pys_class_lookup_method_slow(cls, PYS_INTERN_len);
    cd->slot_getitem       = pys_class_lookup_method_slow(cls, PYS_INTERN_getitem);
    cd->slot_setitem       = pys_class_lookup_method_slow(cls, PYS_INTERN_setitem);
    cd->slot_contains      = pys_class_lookup_method_slow(cls, PYS_INTERN_contains);
    cd->slot_iter          = pys_class_lookup_method_slow(cls, PYS_INTERN_iter);
    cd->slot_next          = pys_class_lookup_method_slow(cls, PYS_INTERN_next);
    cd->slot_call          = pys_class_lookup_method_slow(cls, PYS_INTERN_call);
    cd->slot_get           = pys_class_lookup_method_slow(cls, PYS_INTERN_get);
    cd->slot_getattr       = pys_class_lookup_method_slow(cls, PYS_INTERN_getattr);
    cd->slot_getattribute  = pys_class_lookup_method_slow(cls, PYS_INTERN_getattribute);
    cd->slot_setattr       = pys_class_lookup_method_slow(cls, PYS_INTERN_setattr);
    cd->slot_index         = pys_class_lookup_method_slow(cls, PYS_INTERN_index);
    cd->slot_invert        = pys_class_lookup_method_slow(cls, PYS_INTERN_invert);
    cd->slot_neg           = pys_class_lookup_method_slow(cls, PYS_INTERN_neg);
    cd->slot_repr          = pys_class_lookup_method_slow(cls, PYS_INTERN_repr);
    cd->slot_str           = pys_class_lookup_method_slow(cls, PYS_INTERN_str);
    cd->slot_metaclass     = pys_class_lookup_method_slow(cls, PYS_INTERN_metaclass);
    cd->slot_set_name      = pys_class_lookup_method_slow(cls, PYS_INTERN_set_name);
    // Compute fast_new: true iff slot_new resolves to the built-in
    // object.__new__ AND the class neither needs exception setup nor
    // has a built-in base (so bi_object_new's primary-value branch is
    // a no-op anyway).  Not gated on metaclass — caller still does
    // the metaclass __call__ check before consulting fast_new.
    {
        extern VALUE bi_object_new(CTX *c, int argc, VALUE *argv);
        bool ok = !cd->is_exception
                  && cd->slot_new != PYS_NONE
                  && PYS_IS_PTR(cd->slot_new)
                  && PYS_PTR(cd->slot_new)->type == PYS_T_BUILTIN
                  && PYS_PTR(cd->slot_new)->builtin.fn == bi_object_new;
        if (ok) {
            // Walk MRO for any class with a builtin_ctor (besides self if
            // it's already a builtin — but builtin types aren't routed
            // here in the fast path).
            for (int i = 0; i < cd->nmro; i++) {
                if (pys_is_class(cd->mro[i]) && PYS_PTR(cd->mro[i])->cls.builtin_ctor) {
                    ok = false; break;
                }
            }
        }
        cd->fast_new = ok;
    }
    cd->slots_initialized  = true;
}

// Public lookup: short-circuit through pre-resolved slots when `name` is
// one of the known dunders, falling back to the slow MRO walk otherwise.
// Pointer-compare on `name` against PYS_INTERN_* succeeds when the
// caller passed a global PYS_INTERN_* directly (the common path from
// runtime.c) or when the SD-baked code's `n->u.X.name` has been interned
// to the same pool entry.
VALUE
pys_class_lookup_method(VALUE cls, const char *name)
{
    if (cls == PYS_NONE || !pys_is_class(cls)) return PYS_NONE;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    // Lazy initialise (paranoia — install_builtins paths construct
    // classes before slots may have been populated).
    if (UNLIKELY(!cd->slots_initialized)) pyclass_refresh_slots(cls);
    // Skip the 24-way slot scan unless `name` looks like a dunder
    // (`__xxx__`).  All non-dunder names — the bulk of richards's
    // / deltablue's lookups (e.g. "weakest_of") — go straight to the
    // slow path and avoid 24 pointer-equality checks.
    if (UNLIKELY(name[0] != '_' || name[1] != '_'))
        return pys_class_lookup_method_slow(cls, name);
    // Dunder slot fast path.  Linear pointer-compare scan over the
    // ~24 known names; on hit, return the pre-resolved MRO value.
    if (name == PYS_INTERN_init)         return cd->slot_init;
    if (name == PYS_INTERN_eq)           return cd->slot_eq;
    if (name == PYS_INTERN_lt)           return cd->slot_lt;
    if (name == PYS_INTERN_hash)         return cd->slot_hash;
    if (name == PYS_INTERN_new)          return cd->slot_new;
    if (name == PYS_INTERN_bool)         return cd->slot_bool;
    if (name == PYS_INTERN_len)          return cd->slot_len;
    if (name == PYS_INTERN_getitem)      return cd->slot_getitem;
    if (name == PYS_INTERN_setitem)      return cd->slot_setitem;
    if (name == PYS_INTERN_contains)     return cd->slot_contains;
    if (name == PYS_INTERN_iter)         return cd->slot_iter;
    if (name == PYS_INTERN_next)         return cd->slot_next;
    if (name == PYS_INTERN_call)         return cd->slot_call;
    if (name == PYS_INTERN_get)          return cd->slot_get;
    if (name == PYS_INTERN_getattr)      return cd->slot_getattr;
    if (name == PYS_INTERN_getattribute) return cd->slot_getattribute;
    if (name == PYS_INTERN_setattr)      return cd->slot_setattr;
    if (name == PYS_INTERN_index)        return cd->slot_index;
    if (name == PYS_INTERN_invert)       return cd->slot_invert;
    if (name == PYS_INTERN_neg)          return cd->slot_neg;
    if (name == PYS_INTERN_repr)         return cd->slot_repr;
    if (name == PYS_INTERN_str)          return cd->slot_str;
    if (name == PYS_INTERN_metaclass)    return cd->slot_metaclass;
    if (name == PYS_INTERN_set_name)     return cd->slot_set_name;
    // Non-dunder: slow path.
    return pys_class_lookup_method_slow(cls, name);
}

bool
pys_class_has_method(VALUE cls, const char *name)
{
    if (cls == PYS_NONE || !pys_is_class(cls)) return false;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) {
        struct pysclass *kd = &PYS_PTR(cd->mro[i])->cls;
        for (int j = 0; j < kd->nmethods; j++) {
            const char *mn = kd->methods[j].name;
            if (mn == name || strcmp(mn, name) == 0)
                return true;
        }
    }
    return false;
}

VALUE
pys_class_lookup_method_pub(VALUE cls, const char *name)
{
    return pys_class_lookup_method(cls, name);
}

bool
pys_func_is_generator(VALUE fn)
{
    return PYS_IS_PTR(fn) && PYS_PTR(fn)->type == PYS_T_FUNC
        && PYS_PTR(fn)->func.is_generator;
}

// Cooperative super() lookup: walk self.__class__'s MRO; find
// `start_after_cls`; return the first method found AFTER it.
// True if `v` is a bound method whose inner is a built-in.  Used by
// node_super_method to decide whether to prepend `self` (built-in
// already carries its own receiver via the bound wrapper).
bool
pys_is_bound_builtin(VALUE v)
{
    return pys_is_bound(v) && pys_is_builtin(PYS_PTR(v)->bound.func);
}

VALUE
pys_super_lookup(CTX *c, VALUE self, VALUE start_after_cls, const char *name)
{
    (void)c;
    // self may be an instance OR a class (classmethod context).  Use
    // the relevant MRO either way.
    struct pysclass *cd;
    if (pys_is_instance(self)) {
        cd = &PYS_PTR(self)->inst.cls->cls;
    } else if (pys_is_class(self)) {
        cd = &PYS_PTR(self)->cls;
    } else {
        return PYS_NONE;
    }
    int i = 0;
    while (i < cd->nmro && cd->mro[i] != start_after_cls) i++;
    for (int j = i + 1; j < cd->nmro; j++) {
        struct pysclass *kd = &PYS_PTR(cd->mro[j])->cls;
        for (int k = 0; k < kd->nmethods; k++)
            if (strcmp(kd->methods[k].name, name) == 0) return kd->methods[k].value;
    }
    // Walk past start_after_cls's MRO; if any of the remaining classes
    // is a built-in subclass-base AND the instance has a primary,
    // dispatch to the built-in method on the primary.
    if (pys_is_instance(self) && PYS_PTR(self)->inst.primary) {
        VALUE prim = PYS_PTR(self)->inst.primary;
        extern VALUE pys_builtin_method(CTX *c, VALUE recv, const char *name);
        VALUE bm = pys_builtin_method(c, prim, name);
        if (bm != PYS_NONE) return bm;
    }
    return PYS_NONE;
}

VALUE
pys_make_instance(VALUE cls)
{
    struct pysobj *o = pys_alloc(PYS_T_INSTANCE);
    o->inst.cls = PYS_PTR(cls);
    o->inst.attrs = NULL;       // lazily allocated when first attr is set
    o->inst.primary = 0;
    return PYS_OBJ_VAL(o);
}

// object.__getattribute__(self, name) — default attribute lookup that
// does NOT recursively call __getattribute__.  We implement this by
// temporarily marking the call so pys_getattr skips the user's hook.
static __thread int pys_skip_getattribute_hook = 0;

static VALUE
bi_object_getattribute(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[1]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "attribute name must be string");
    pys_skip_getattribute_hook++;
    VALUE r = pys_getattr(c, argv[0], PYS_PTR(argv[1])->str.chars);
    pys_skip_getattribute_hook--;
    return r;
}

// object.__setattr__(self, name, value) — default attribute set.
static VALUE
bi_object_setattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[1]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "attribute name must be string");
    pys_setattr(c, argv[0], PYS_PTR(argv[1])->str.chars, argv[2]);
    return PYS_NONE;
}

// object.__delattr__(self, name) — default attribute delete.
static VALUE
bi_object_delattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[1]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "attribute name must be string");
    if (!pys_is_instance(argv[0]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "delattr: not an instance");
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->inst.attrs) {
        // Wrap inst.attrs as a dict VALUE for pys_dict_remove.
        struct pysobj *d = pys_alloc(PYS_T_DICT);
        d->dict = o->inst.attrs;
        pys_dict_remove(c, PYS_OBJ_VAL(d), argv[1]);
    }
    return PYS_NONE;
}

// object.__init__(self, *args, **kwargs) — accepts anything, returns None.
// Used by 'super().__init__()' from a built-in subclass so the chain
// terminates cleanly.
static VALUE
bi_object_init(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    return PYS_NONE;
}

// Synthetic slot descriptor for type-level introspection attrs.
// `descriptor(cls)` returns the matching attribute on cls — used
// when CPython introspection does
//   _static_getmro = type.__dict__['__mro__'].__get__
//   mro = _static_getmro(cls)
// and similar for __bases__.
static VALUE
bi_pyslot_mro(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE cls = argv[0];
    if (!pys_is_class(cls)) return PYS_NONE;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    return pys_make_tuple(cd->mro, cd->nmro);
}

static VALUE
bi_pyslot_bases(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE cls = argv[0];
    if (!pys_is_class(cls)) return PYS_NONE;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    if (cd->nbases == 0) return pys_make_tuple(&c->TYPE_object, 1);
    return pys_make_tuple(cd->bases, cd->nbases);
}

static VALUE
bi_pyslot_dict(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    extern VALUE pys_getattr(CTX *c, VALUE recv, const char *name);
    return pys_getattr(c, argv[0], "__dict__");
}

// `cls.mro()` — returns the MRO as a list (mutable).  CPython exposes
// this and `__mro__` (the tuple form).  Used by typing / tests.
VALUE
bi_class_mro(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE cls = argv[0];
    if (!pys_is_class(cls)) return pys_make_list(NULL, 0);
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    return pys_make_list(cd->mro, cd->nmro);
}

VALUE
pys_make_pyslot_descriptor(const char *attr)
{
    if (strcmp(attr, "__mro__") == 0)
        return pys_make_builtin("__mro__", bi_pyslot_mro, 1, 2);
    if (strcmp(attr, "__bases__") == 0)
        return pys_make_builtin("__bases__", bi_pyslot_bases, 1, 2);
    if (strcmp(attr, "__dict__") == 0)
        return pys_make_builtin("__dict__", bi_pyslot_dict, 1, 2);
    return PYS_NONE;
}

VALUE
bi_object_new(CTX *c, int argc, VALUE *argv)
{
    if (argc < 1 || !pys_is_class(argv[0]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "object.__new__() needs class");
    VALUE cls = argv[0];
    VALUE inst = pys_make_instance(cls);
    // For built-in subclasses, set up an EMPTY primary value.  If the
    // caller forwarded constructor args, only forward them when the
    // class has no user-defined __init__ (so plain `class M(list): pass;
    // M([1,2,3])` still populates).  Otherwise the user's __init__ is
    // responsible for calling super().__init__(args).
    extern VALUE pys_class_find_builtin_base(VALUE cls);
    VALUE bin_base = pys_class_find_builtin_base(cls);
    if (bin_base != PYS_NONE) {
        bool has_user_init = false;
        struct pysclass *cd = &PYS_PTR(cls)->cls;
        for (int i = 0; i < cd->nmro; i++) {
            VALUE mc = cd->mro[i];
            // Stop walking once we reach the built-in base — methods
            // beyond that are object/builtin defaults.
            if (mc == bin_base) break;
            struct pysclass *kd = &PYS_PTR(mc)->cls;
            for (int j = 0; j < kd->nmethods; j++) {
                if (strcmp(kd->methods[j].name, "__init__") == 0) {
                    has_user_init = true; break;
                }
            }
            if (has_user_init) break;
        }
        int fwd_argc = (has_user_init || argc <= 1) ? 0 : argc - 1;
        VALUE *fwd_argv = (has_user_init || argc <= 1) ? NULL : argv + 1;
        // The caller may have set PYS_BI_KWC for the user-level __init__
        // call (e.g. `Sub(seq, newarg=3)`); built-in ctors like bi_list
        // forbid kwargs, so suppress while we forward nothing on the
        // has_user_init path — the user's __init__ will (re)dispatch
        // super().__init__ with its own kwargs explicitly.
        extern int PYS_BI_KWC;
        int saved_kwc = PYS_BI_KWC;
        if (has_user_init) PYS_BI_KWC = 0;
        VALUE primary = PYS_PTR(bin_base)->cls.builtin_ctor(c, fwd_argc, fwd_argv);
        PYS_BI_KWC = saved_kwc;
        if (c->state == PYS_STATE_RAISE) return 0;
        PYS_PTR(inst)->inst.primary = primary;
    }
    return inst;
}

// Walk a class's MRO and find the first built-in type class (one with
// builtin_ctor set).  Returns PYS_NONE if none found.
VALUE
pys_class_find_builtin_base(VALUE cls)
{
    if (!pys_is_class(cls)) return PYS_NONE;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) {
        if (pys_is_class(cd->mro[i]) && PYS_PTR(cd->mro[i])->cls.builtin_ctor)
            return cd->mro[i];
    }
    return PYS_NONE;
}

struct pysframe *
pys_new_frame(struct pysframe *parent, int nslots)
{
    struct pysframe *f = (struct pysframe *)GC_malloc(
        sizeof(struct pysframe) + sizeof(VALUE) * (nslots ? nslots : 1));
    f->parent = parent;
    f->slot_names = NULL;
    f->nslots = nslots;
    for (int i = 0; i < nslots; i++) f->slots[i] = PYS_NONE;
    return f;
}

// ---------------------------------------------------------------------------
// Globals.
// ---------------------------------------------------------------------------

// Process-wide monotone counter so every `pysglobals.serial` is unique
// across modules.  This is what the inline gref cache compares against:
// if a gref node is only ever evaluated under one specific module's
// globals (always true since each AST belongs to its module), the
// cache stays consistent only when the cached serial equals the
// current globals' serial — which is exactly what we want.
uint64_t SHARED_GLOBALS_SERIAL = 1;

// Keyword args being passed to a built-in.  pys_apply_kw saves/restores
// these around the call; built-ins (sorted / min / max / enumerate)
// consult them directly.  Single-threaded, so a single global is fine.
int    PYS_BI_KWC = 0;
const char **PYS_BI_KWNAMES = NULL;
VALUE *PYS_BI_KWVALUES = NULL;

static VALUE
pys_bi_kwarg(const char *name)
{
    for (int i = 0; i < PYS_BI_KWC; i++)
        if (strcmp(PYS_BI_KWNAMES[i], name) == 0) return PYS_BI_KWVALUES[i];
    return (VALUE)0;
}

struct pysglobals *
pys_globals_new(void)
{
    struct pysglobals *g = (struct pysglobals *)GC_malloc(sizeof(struct pysglobals));
    g->entries = NULL;
    g->size = g->capa = 0;
    g->serial = ++SHARED_GLOBALS_SERIAL;
    return g;
}

static int
pys_global_index(CTX *c, const char *name)
{
    struct pysglobals *g = c->globals;
    for (size_t i = 0; i < g->size; i++)
        if (strcmp(g->entries[i].name, name) == 0) return (int)i;
    return -1;
}

static int
pys_global_alloc(CTX *c, const char *name)
{
    struct pysglobals *g = c->globals;
    if (g->size == g->capa) {
        size_t cap = g->capa ? g->capa * 2 : 32;
        g->entries = (struct gentry *)GC_realloc(g->entries, cap * sizeof(struct gentry));
        g->capa = cap;
    }
    int i = (int)g->size++;
    g->entries[i].name = name;
    g->entries[i].value = PYS_NONE;
    g->entries[i].defined = false;
    return i;
}

void
pys_global_define(CTX *c, const char *name, VALUE v)
{
    struct pysglobals *g = c->globals;
    int i = pys_global_index(c, name);
    bool is_new = (i < 0);
    if (is_new) i = pys_global_alloc(c, name);
    bool was_defined = g->entries[i].defined;
    g->entries[i].value = v;
    g->entries[i].defined = true;
    if (is_new || !was_defined) g->serial = ++SHARED_GLOBALS_SERIAL;
}

bool
pys_global_has(CTX *c, const char *name)
{
    int i = pys_global_index(c, name);
    return i >= 0 && c->globals->entries[i].defined;
}

VALUE
pys_global_ref(CTX *c, const char *name)
{
    int i = pys_global_index(c, name);
    if (i < 0 || !c->globals->entries[i].defined)
        PYS_RAISE_EXC(c, c->EXC_NameError, "name '%s' is not defined", name);
    return c->globals->entries[i].value;
}

int
pys_global_resolve(CTX *c, const char *name)
{
    int i = pys_global_index(c, name);
    if (i < 0)
        PYS_RAISE_EXC(c, c->EXC_NameError, "name '%s' is not defined", name);
    if (!c->globals->entries[i].defined)
        PYS_RAISE_EXC(c, c->EXC_NameError, "name '%s' is not defined", name);
    return i;
}

int
pys_global_resolve_or_alloc(CTX *c, const char *name)
{
    int i = pys_global_index(c, name);
    if (i < 0) i = pys_global_alloc(c, name);
    return i;
}

void
pys_global_set(CTX *c, const char *name, VALUE v)
{
    pys_global_define(c, name, v);
}

bool
pys_global_lookup(CTX *c, const char *name, VALUE *out)
{
    int i = pys_global_index(c, name);
    if (i < 0 || !c->globals->entries[i].defined) return false;
    *out = c->globals->entries[i].value;
    return true;
}

void
pys_global_undef(CTX *c, const char *name)
{
    int i = pys_global_index(c, name);
    if (i < 0) return;
    c->globals->entries[i].defined = false;
    c->globals->serial = ++SHARED_GLOBALS_SERIAL;
}

// ---------------------------------------------------------------------------
// Errors / raise.
// ---------------------------------------------------------------------------

void
pys_error(CTX *c, const char *fmt, ...)
{
    (void)c;
    fprintf(stderr, "pystro: ");
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void
pys_raise_exc(CTX *c, VALUE cls, const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (cls == 0 || !pys_is_class(cls)) cls = c->EXC_RuntimeError;
    VALUE inst = pys_make_instance(cls);
    VALUE msg = pys_make_str(buf, strlen(buf));
    pys_setattr(c, inst, "args", pys_make_tuple(&msg, 1));
    pys_setattr(c, inst, "message", msg);
    if (c->current_handling_exc && c->current_handling_exc != PYS_NONE)
        pys_setattr(c, inst, "__context__", c->current_handling_exc);
    else
        pys_setattr(c, inst, "__context__", PYS_NONE);
    // CPython always exposes __cause__ / __suppress_context__ /
    // __context__ — initialise to None for implicit raises so attribute
    // access is consistent.
    pys_setattr(c, inst, "__cause__", PYS_NONE);
    pys_setattr(c, inst, "__suppress_context__", PYS_FALSE);
    // SyntaxError / OSError / ImportError attribute defaults — same
    // surface as bi_exception_init, applied here for runtime-raised
    // exceptions that bypass __init__.
    pys_setattr(c, inst, "filename",   PYS_NONE);
    pys_setattr(c, inst, "lineno",     PYS_NONE);
    pys_setattr(c, inst, "offset",     PYS_NONE);
    pys_setattr(c, inst, "text",       PYS_NONE);
    pys_setattr(c, inst, "end_lineno", PYS_NONE);
    pys_setattr(c, inst, "end_offset", PYS_NONE);
    pys_setattr(c, inst, "msg",        msg);
    pys_setattr(c, inst, "errno",      PYS_NONE);
    pys_setattr(c, inst, "strerror",   PYS_NONE);
    pys_setattr(c, inst, "filename2",  PYS_NONE);
    pys_setattr(c, inst, "winerror",   PYS_NONE);
    pys_setattr(c, inst, "name",       PYS_NONE);
    pys_setattr(c, inst, "path",       PYS_NONE);
    pys_setattr(c, inst, "encoding",   PYS_NONE);
    pys_setattr(c, inst, "object",     PYS_NONE);
    pys_setattr(c, inst, "start",      PYS_NONE);
    pys_setattr(c, inst, "end",        PYS_NONE);
    pys_setattr(c, inst, "reason",     PYS_NONE);
    pys_setattr(c, inst, "obj",        PYS_NONE);
    // Capture a snapshot of the active call stack as a chain of
    // traceback objects (TracebackType-like) so CPython introspection
    // — `tb.tb_frame.f_code.co_name`, `traceback.extract_tb`, etc. —
    // can walk the stack.  Each tb has tb_frame (a Frame instance with
    // f_code.co_name + f_globals/f_locals), tb_lineno (best-effort 0),
    // tb_lasti (-1), and tb_next (the deeper frame).
    if (c->call_top > 0 && c->TYPE_traceback != 0) {
        VALUE next = PYS_NONE;
        for (int i = 0; i < c->call_top; i++) {
            const char *fn = c->call_stack[i] ? c->call_stack[i] : "<anon>";
            VALUE frame = pys_make_instance(c->TYPE_frame);
            VALUE code  = pys_make_instance(c->TYPE_object);
            pys_setattr(c, code, "co_name", pys_make_str(fn, strlen(fn)));
            pys_setattr(c, code, "co_filename", pys_make_str("<pystro>", 8));
            pys_setattr(c, code, "co_firstlineno", PYS_FIX(0));
            pys_setattr(c, frame, "f_code", code);
            pys_setattr(c, frame, "f_lineno", PYS_FIX(0));
            pys_setattr(c, frame, "f_lasti", PYS_FIX(-1));
            pys_setattr(c, frame, "f_globals", pys_make_dict());
            pys_setattr(c, frame, "f_locals", pys_make_dict());
            pys_setattr(c, frame, "f_back", PYS_NONE);
            pys_setattr(c, frame, "f_trace", PYS_NONE);
            VALUE tb = pys_make_instance(c->TYPE_traceback);
            pys_setattr(c, tb, "tb_frame", frame);
            pys_setattr(c, tb, "tb_lineno", PYS_FIX(0));
            pys_setattr(c, tb, "tb_lasti", PYS_FIX(-1));
            pys_setattr(c, tb, "tb_next", next);
            next = tb;
        }
        pys_setattr(c, inst, "__traceback__", next);
    } else if (c->call_top > 0) {
        // Fallback (TYPE_traceback not initialised yet — happens during
        // very early bootstrap before install_builtins).
        VALUE *frames = (VALUE *)alloca(sizeof(VALUE) * c->call_top);
        for (int i = 0; i < c->call_top; i++) {
            const char *fn = c->call_stack[i] ? c->call_stack[i] : "<anon>";
            frames[i] = pys_make_str(fn, strlen(fn));
        }
        pys_setattr(c, inst, "__traceback__", pys_make_list(frames, c->call_top));
    } else {
        // CPython always exposes __traceback__ on the instance (None
        // for raises that didn't originate from a call frame).
        pys_setattr(c, inst, "__traceback__", PYS_NONE);
    }
    c->state = PYS_STATE_RAISE;
    c->state_value = inst;
    // State-based propagation only.  Caller (PYS_RAISE_EXC macro)
    // returns 0 immediately; node_try / pys_apply observe state and
    // unwind frames manually.  No longjmp.
}

// ---------------------------------------------------------------------------
// Numeric tower.
// ---------------------------------------------------------------------------

static double
pys_to_double(CTX *c, VALUE v)
{
    if (PYS_IS_FIXNUM(v))    return (double)PYS_FIXVAL(v);
    if (PYS_IS_FLONUM(v))    return pys_flonum_to_double(v);
    if (pys_is_heap_float(v)) return PYS_PTR(v)->dbl;
    if (pys_is_bignum(v))    return mpz_get_d(PYS_PTR(v)->mpz);
    if (v == PYS_TRUE)       return 1.0;
    if (v == PYS_FALSE)      return 0.0;
    PYS_RAISE_EXC(c, c->EXC_TypeError, "expected a number");
}

// `v` must already be int-ish (int / bool / bignum).
static void
pys_to_mpz(CTX *c, VALUE v, mpz_t out)
{
    if (PYS_IS_FIXNUM(v)) { mpz_init_set_si(out, (long)PYS_FIXVAL(v)); return; }
    if (pys_is_bignum(v)) { mpz_init_set(out, PYS_PTR(v)->mpz); return; }
    if (v == PYS_TRUE)    { mpz_init_set_si(out, 1); return; }
    if (v == PYS_FALSE)   { mpz_init_set_si(out, 0); return; }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "expected an integer");
}

static bool
pys_int_or_bool(VALUE v)
{
    return PYS_IS_FIXNUM(v) || pys_is_bignum(v) || v == PYS_TRUE || v == PYS_FALSE;
}

VALUE
pys_neg(CTX *c, VALUE a)
{
    if (PYS_IS_FIXNUM(a)) {
        int64_t r;
        if (!__builtin_sub_overflow((int64_t)0, PYS_FIXVAL(a), &r) &&
            r >= PYS_FIXNUM_MIN && r <= PYS_FIXNUM_MAX)
            return PYS_FIX(r);
    }
    if (PYS_IS_FLONUM(a)) return pys_make_float(-pys_flonum_to_double(a));
    if (pys_is_heap_float(a)) return pys_make_float(-PYS_PTR(a)->dbl);
    if (pys_is_complex(a)) return pys_make_complex(-PYS_PTR(a)->cpx.re, -PYS_PTR(a)->cpx.im);
    if (pys_int_or_bool(a)) {
        mpz_t z; pys_to_mpz(c, a, z);
        mpz_neg(z, z);
        VALUE r = pys_normalise_int(z);
        mpz_clear(z);
        return r;
    }
    if (pys_is_instance(a)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(a)->inst.cls), PYS_INTERN_neg);
        if (m != PYS_NONE) {
            VALUE av[1] = { a };
            return pys_apply(c, m, 1, av);
        }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "bad operand type for unary -");
}

// Try a binary dunder hook on an instance operand: returns the result
// if the dunder exists, else PYS_NONE (caller falls through to the
// regular numeric / type logic).  We use 0 as "method not defined"
// sentinel since PYS_NONE is a valid return.
//
// CPython always dispatches arithmetic through type slots (tp_as_number).
// We mirror that: any user instance with a defined dunder takes priority.
// Returns 0 if the dunder isn't defined OR if it returned NotImplemented
// (so the caller can fall through to reflected ops or built-in paths).
static inline __attribute__((always_inline)) VALUE
pys_try_binop_dunder(CTX *c, const char *name, VALUE a, VALUE b)
{
    // Only user-defined classes can have a dunder we don't already
    // know about; for built-in types we know they don't override these.
    if (LIKELY(!(PYS_IS_PTR(a) && PYS_PTR(a)->type == PYS_T_INSTANCE)))
        return (VALUE)0;
    VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(a)->inst.cls), name);
    if (m != PYS_NONE) {
        VALUE av[2] = { a, b };
        VALUE r = pys_apply(c, m, 2, av);
        if (UNLIKELY(!r)) return 0;     // raised
        if (PYS_IS_PTR(r) && PYS_PTR(r)->type == PYS_T_NOTIMPL) return (VALUE)0;
        return r;
    }
    return (VALUE)0;
}

// For built-in subclasses (`class M(list):`), unwrap to the primary
// value so binary ops fall through to the built-in path.
static inline VALUE
pys_unwrap_primary(VALUE v)
{
    if (PYS_IS_PTR(v) && PYS_PTR(v)->type == PYS_T_INSTANCE && PYS_PTR(v)->inst.primary)
        return PYS_PTR(v)->inst.primary;
    return v;
}

VALUE
pys_add(CTX *c, VALUE a, VALUE b)
{
    VALUE r = pys_try_binop_dunder(c, PYS_INTERN_add, a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    r = pys_try_binop_dunder(c, PYS_INTERN_radd, b, a);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    r = pys_try_binop_dunder(c, PYS_INTERN_iadd, a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    a = pys_unwrap_primary(a);
    b = pys_unwrap_primary(b);
    if (pys_is_str(a) && pys_is_str(b)) {
        size_t la = PYS_PTR(a)->str.len, lb = PYS_PTR(b)->str.len;
        char *buf = (char *)GC_malloc_atomic(la + lb + 1);
        memcpy(buf,      PYS_PTR(a)->str.chars, la);
        memcpy(buf + la, PYS_PTR(b)->str.chars, lb);
        buf[la + lb] = '\0';
        return pys_make_str_take(buf, la + lb);
    }
    if (pys_is_byteseq(a) && pys_is_byteseq(b)) {
        size_t la = PYS_PTR(a)->str.len, lb = PYS_PTR(b)->str.len;
        char *buf = (char *)GC_malloc_atomic(la + lb + 1);
        memcpy(buf,      PYS_PTR(a)->str.chars, la);
        memcpy(buf + la, PYS_PTR(b)->str.chars, lb);
        buf[la + lb] = '\0';
        // Result type tracks the LEFT operand (CPython behaviour):
        //   bytes + bytearray → bytes; bytearray + bytes → bytearray.
        if (pys_is_bytearray(a))
            return pys_make_bytearray(buf, la + lb);
        return pys_make_bytes(buf, la + lb);
    }
    if ((pys_is_list(a) && pys_is_list(b)) || (pys_is_tuple(a) && pys_is_tuple(b))) {
        size_t la = PYS_PTR(a)->list.len, lb = PYS_PTR(b)->list.len;
        VALUE *items = (VALUE *)alloca(sizeof(VALUE) * (la + lb + 1));
        memcpy(items,      PYS_PTR(a)->list.items, sizeof(VALUE) * la);
        memcpy(items + la, PYS_PTR(b)->list.items, sizeof(VALUE) * lb);
        return pys_is_list(a) ? pys_make_list(items, la + lb) : pys_make_tuple(items, la + lb);
    }
    // list + iterable (only via __iadd__-style) — Python's list __iadd__
    // accepts any iterable.  Supports str / range / iter / gen / dict /
    // set / etc. on the right.
    if (pys_is_list(a) && PYS_IS_PTR(b)) {
        int t = PYS_PTR(b)->type;
        if (t == PYS_T_ITER || t == PYS_T_GEN || t == PYS_T_STR
            || t == PYS_T_BYTES || t == PYS_T_BYTEARRAY || t == PYS_T_RANGE
            || t == PYS_T_DICT || t == PYS_T_SET || t == PYS_T_FROZENSET) {
            VALUE r = pys_make_list(PYS_PTR(a)->list.items, PYS_PTR(a)->list.len);
            struct pys_iter it; pys_iter_init(c, &it, b);
            if (c->state != PYS_STATE_NORMAL) return r;
            VALUE x;
            while (pys_iter_next(c, &it, &x)) pys_list_append(c, r, x);
            return r;
        }
    }
    if (pys_int_or_bool(a) && pys_int_or_bool(b)) {
        mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
        mpz_add(za, za, zb);
        VALUE r = pys_normalise_int(za);
        mpz_clear(za); mpz_clear(zb);
        return r;
    }
    if ((pys_int_or_bool(a) || pys_is_float(a)) && (pys_int_or_bool(b) || pys_is_float(b)))
        return pys_make_float(pys_to_double(c, a) + pys_to_double(c, b));
    {
        double ra, ia, rb, ib;
        if (pys_to_cpx(c, a, &ra, &ia) && pys_to_cpx(c, b, &rb, &ib)) {
            return pys_make_complex(ra + rb, ia + ib);
        }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "unsupported operand type(s) for +");
}

// `pys_func_set_doc` definition lives further down (after pydict_new).

// Forward decls used by binop fallthroughs into set methods.
static VALUE sm_union(CTX *c, int argc, VALUE *argv);
static VALUE sm_intersection(CTX *c, int argc, VALUE *argv);
static VALUE sm_difference(CTX *c, int argc, VALUE *argv);

// CPython parity: result type matches the LEFT operand for set binops.
// `frozenset(...) | set(...)` → frozenset; `set(...) | frozenset(...)` → set.
// SetSubclass instance left → result is set/frozenset of the underlying
// primary type (CPython parity — subclass __or__ inherits from set/frozenset
// returning the base type unless overridden).
static inline VALUE pys_make_set_like(VALUE left) {
    VALUE u = pys_unwrap_primary(left);
    return pys_is_frozenset(u) ? pys_make_frozenset() : pys_make_set();
}
// (declaration moved above pys_class_inherit_metaclass)

VALUE
pys_sub(CTX *c, VALUE a, VALUE b)
{
    {
        VALUE au = pys_unwrap_primary(a);
        VALUE bu = pys_unwrap_primary(b);
        if (pys_is_any_set(au) && pys_is_any_set(bu)) {
            VALUE av[2] = { a, b };
            return sm_difference(c, 2, av);
        }
    }
    // list - list as set difference (dict_keys-style courtesy).
    if ((pys_is_list(a) || pys_is_any_set(a)) && (pys_is_list(b) || pys_is_any_set(b))) {
        VALUE r = pys_make_set();
        size_t na = PYS_PTR(a)->list.len;
        for (size_t i = 0; i < na; i++) {
            VALUE x = PYS_PTR(a)->list.items[i];
            if (!pys_contains(c, b, x)) pys_dict_set(c, r, x, PYS_NONE);
        }
        return r;
    }
    VALUE r = pys_try_binop_dunder(c, PYS_INTERN_sub, a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    r = pys_try_binop_dunder(c, PYS_INTERN_rsub, b, a);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    r = pys_try_binop_dunder(c, PYS_INTERN_isub, a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    a = pys_unwrap_primary(a);
    b = pys_unwrap_primary(b);
    if (pys_int_or_bool(a) && pys_int_or_bool(b)) {
        mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
        mpz_sub(za, za, zb);
        VALUE r = pys_normalise_int(za);
        mpz_clear(za); mpz_clear(zb);
        return r;
    }
    if ((pys_int_or_bool(a) || pys_is_float(a)) && (pys_int_or_bool(b) || pys_is_float(b)))
        return pys_make_float(pys_to_double(c, a) - pys_to_double(c, b));
    {
        double ra, ia, rb, ib;
        if (pys_to_cpx(c, a, &ra, &ia) && pys_to_cpx(c, b, &rb, &ib))
            return pys_make_complex(ra - rb, ia - ib);
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "unsupported operand type(s) for -");
}

VALUE
pys_mul(CTX *c, VALUE a, VALUE b)
{
    VALUE r = pys_try_binop_dunder(c, PYS_INTERN_mul, a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    r = pys_try_binop_dunder(c, PYS_INTERN_rmul, b, a);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    r = pys_try_binop_dunder(c, PYS_INTERN_imul, a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    a = pys_unwrap_primary(a);
    b = pys_unwrap_primary(b);
    if (pys_is_str(a) && pys_int_or_bool(b)) {
        int64_t k = PYS_IS_FIXNUM(b) ? PYS_FIXVAL(b) : (b == PYS_TRUE ? 1 : 0);
        if (k <= 0) return pys_make_str("", 0);
        size_t la = PYS_PTR(a)->str.len;
        char *buf = (char *)GC_malloc_atomic(la * (size_t)k + 1);
        for (int64_t i = 0; i < k; i++) memcpy(buf + i * la, PYS_PTR(a)->str.chars, la);
        buf[la * k] = '\0';
        return pys_make_str_take(buf, la * (size_t)k);
    }
    if (pys_int_or_bool(a) && pys_is_str(b)) return pys_mul(c, b, a);
    if (pys_is_byteseq(a) && pys_int_or_bool(b)) {
        int64_t k = PYS_IS_FIXNUM(b) ? PYS_FIXVAL(b) : (b == PYS_TRUE ? 1 : 0);
        if (k <= 0) return pys_make_bytes("", 0);
        size_t la = PYS_PTR(a)->str.len;
        char *buf = (char *)GC_malloc_atomic(la * (size_t)k + 1);
        for (int64_t i = 0; i < k; i++) memcpy(buf + i * la, PYS_PTR(a)->str.chars, la);
        VALUE r = pys_make_bytes(buf, la * (size_t)k);
        if (PYS_PTR(a)->type == PYS_T_BYTEARRAY) PYS_PTR(r)->type = PYS_T_BYTEARRAY;
        return r;
    }
    if (pys_int_or_bool(a) && pys_is_byteseq(b)) return pys_mul(c, b, a);
    if (pys_int_or_bool(a) && (pys_is_list(b) || pys_is_tuple(b))) return pys_mul(c, b, a);
    if ((pys_is_list(a) || pys_is_tuple(a)) && pys_int_or_bool(b)) {
        // Bignum factor → also try to fit in int64.
        int64_t k;
        if (PYS_IS_FIXNUM(b)) k = PYS_FIXVAL(b);
        else if (b == PYS_TRUE) k = 1;
        else if (b == PYS_FALSE) k = 0;
        else if (pys_is_bignum(b)) {
            if (mpz_fits_slong_p(PYS_PTR(b)->mpz)) k = mpz_get_si(PYS_PTR(b)->mpz);
            else PYS_RAISE_EXC(c, c->EXC_OverflowError,
                              "cannot fit '%s' into an index-sized integer",
                              pys_is_list(a) ? "list" : "tuple");
        } else k = 0;
        if (k <= 0) return pys_is_list(a) ? pys_make_list(NULL, 0) : pys_make_tuple(NULL, 0);
        // CPython optimization: tuple * 1 returns the same tuple (immutable).
        if (k == 1 && pys_is_tuple(a)) return a;
        size_t la = PYS_PTR(a)->list.len;
        // Overflow guard: la * k must fit in size_t and not exceed a
        // reasonable threshold (~2 GiB worth of VALUE entries = 256M
        // entries on 64-bit).  CPython raises MemoryError / OverflowError
        // here; test_overflow / test_list_resize_overflow check this.
        size_t total;
        if (la != 0 && (size_t)k > (size_t)-1 / la)
            PYS_RAISE_EXC(c, c->EXC_OverflowError, "list size exceeds maximum");
        total = la * (size_t)k;
        if (total > ((size_t)1 << 28))
            PYS_RAISE_EXC(c, c->EXC_MemoryError, "list size too large");
        VALUE *items = (VALUE *)GC_malloc(sizeof(VALUE) * (total + 1));
        for (int64_t i = 0; i < k; i++)
            memcpy(items + i * la, PYS_PTR(a)->list.items, sizeof(VALUE) * la);
        return pys_is_list(a) ? pys_make_list(items, total) : pys_make_tuple(items, total);
    }
    if (pys_int_or_bool(a) && pys_int_or_bool(b)) {
        mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
        mpz_mul(za, za, zb);
        VALUE r = pys_normalise_int(za);
        mpz_clear(za); mpz_clear(zb);
        return r;
    }
    if ((pys_int_or_bool(a) || pys_is_float(a)) && (pys_int_or_bool(b) || pys_is_float(b)))
        return pys_make_float(pys_to_double(c, a) * pys_to_double(c, b));
    {
        double ra, ia, rb, ib;
        if (pys_to_cpx(c, a, &ra, &ia) && pys_to_cpx(c, b, &rb, &ib))
            return pys_make_complex(ra*rb - ia*ib, ra*ib + ia*rb);
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "unsupported operand type(s) for *");
}

VALUE
pys_matmul(CTX *c, VALUE a, VALUE b)
{
    VALUE r = pys_try_binop_dunder(c, "__matmul__", a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    r = pys_try_binop_dunder(c, "__rmatmul__", b, a);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    r = pys_try_binop_dunder(c, "__imatmul__", a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    PYS_RAISE_EXC(c, c->EXC_TypeError, "unsupported operand type(s) for @");
}

VALUE
pys_truediv(CTX *c, VALUE a, VALUE b)
{
    VALUE r = pys_try_binop_dunder(c, PYS_INTERN_truediv, a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (pys_is_complex(a) || pys_is_complex(b)) {
        double ra, ia, rb, ib;
        if (pys_to_cpx(c, a, &ra, &ia) && pys_to_cpx(c, b, &rb, &ib)) {
            double denom = rb*rb + ib*ib;
            if (denom == 0) PYS_RAISE_EXC(c, c->EXC_ZeroDivisionError, "complex division by zero");
            return pys_make_complex((ra*rb + ia*ib)/denom, (ia*rb - ra*ib)/denom);
        }
    }
    double bd = pys_to_double(c, b);
    if (bd == 0.0) PYS_RAISE_EXC(c, c->EXC_ZeroDivisionError, "division by zero");
    return pys_make_float(pys_to_double(c, a) / bd);
}

VALUE
pys_fdiv(CTX *c, VALUE a, VALUE b)
{
    VALUE rd = pys_try_binop_dunder(c, PYS_INTERN_floordiv, a, b);
    if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    rd = pys_try_binop_dunder(c, "__rfloordiv__", b, a);
    if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (pys_int_or_bool(a) && pys_int_or_bool(b)) {
        mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
        if (mpz_sgn(zb) == 0) {
            mpz_clear(za); mpz_clear(zb);
            PYS_RAISE_EXC(c, c->EXC_ZeroDivisionError, "integer division or modulo by zero");
        }
        mpz_t q; mpz_init(q);
        mpz_fdiv_q(q, za, zb);
        VALUE r = pys_normalise_int(q);
        mpz_clear(za); mpz_clear(zb); mpz_clear(q);
        return r;
    }
    if (pys_is_complex(a) || pys_is_complex(b))
        PYS_RAISE_EXC(c, c->EXC_TypeError,
                     "unsupported operand type(s) for //");
    double bd = pys_to_double(c, b);
    if (bd == 0.0) PYS_RAISE_EXC(c, c->EXC_ZeroDivisionError, "float floor division by zero");
    return pys_make_float(floor(pys_to_double(c, a) / bd));
}

extern VALUE pys_str_pct_format(CTX *c, VALUE fmt, VALUE args);  // forward
VALUE
pys_mod(CTX *c, VALUE a, VALUE b)
{
    // String % formatting: `"fmt" % args`.
    if (pys_is_str(a)) return pys_str_pct_format(c, a, b);
    {
        VALUE rd = pys_try_binop_dunder(c, PYS_INTERN_mod, a, b);
        if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        rd = pys_try_binop_dunder(c, "__rmod__", b, a);
        if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    if (pys_int_or_bool(a) && pys_int_or_bool(b)) {
        mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
        if (mpz_sgn(zb) == 0) {
            mpz_clear(za); mpz_clear(zb);
            PYS_RAISE_EXC(c, c->EXC_ZeroDivisionError, "integer division or modulo by zero");
        }
        mpz_t r; mpz_init(r);
        mpz_fdiv_r(r, za, zb);
        VALUE rv = pys_normalise_int(r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r);
        return rv;
    }
    // CPython rule: complex % anything → TypeError ("unsupported operand
    // type(s) for %"). Same for `divmod` / `//` / etc. on complex.
    if (pys_is_complex(a) || pys_is_complex(b))
        PYS_RAISE_EXC(c, c->EXC_TypeError,
                     "unsupported operand type(s) for %% on complex");
    double bd = pys_to_double(c, b);
    if (bd == 0.0) PYS_RAISE_EXC(c, c->EXC_ZeroDivisionError, "float modulo");
    double ad = pys_to_double(c, a);
    double r = fmod(ad, bd);
    if ((r != 0.0) && ((r < 0) != (bd < 0))) r += bd;
    return pys_make_float(r);
}

VALUE
pys_pow(CTX *c, VALUE a, VALUE b)
{
    VALUE r = pys_try_binop_dunder(c, PYS_INTERN_pow, a, b);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    r = pys_try_binop_dunder(c, "__rpow__", b, a);
    if (r) return r;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (pys_is_complex(a) || pys_is_complex(b)) {
        double ra, ia, rb, ib;
        if (pys_to_cpx(c, a, &ra, &ia) && pys_to_cpx(c, b, &rb, &ib)) {
            // (ra+ia*i)^(rb+ib*i) via exp(b * log(a))
            double mod = sqrt(ra * ra + ia * ia);
            if (mod == 0.0) return pys_make_complex(0, 0);
            double th = atan2(ia, ra);
            double lr = log(mod);
            // b*log(a) = (rb*lr - ib*th) + i*(rb*th + ib*lr)
            double er = rb * lr - ib * th;
            double ei = rb * th + ib * lr;
            double scale = exp(er);
            return pys_make_complex(scale * cos(ei), scale * sin(ei));
        }
    }
    // int ** non-negative-int → bignum (exact)
    if (pys_int_or_bool(a) && pys_int_or_bool(b)) {
        mpz_t zb; pys_to_mpz(c, b, zb);
        if (mpz_sgn(zb) < 0) {
            // 0 ** negative_int → ZeroDivisionError (CPython behaviour).
            mpz_t za; pys_to_mpz(c, a, za);
            if (mpz_sgn(za) == 0) {
                mpz_clear(za); mpz_clear(zb);
                PYS_RAISE_EXC(c, c->EXC_ZeroDivisionError,
                             "0.0 cannot be raised to a negative power");
            }
            mpz_clear(za);
            mpz_clear(zb);
            return pys_make_float(pow(pys_to_double(c, a), pys_to_double(c, b)));
        }
        if (!mpz_fits_ulong_p(zb)) {
            mpz_clear(zb);
            PYS_RAISE_EXC(c, c->EXC_ValueError, "exponent too large");
        }
        unsigned long e = mpz_get_ui(zb);
        mpz_t za; pys_to_mpz(c, a, za);
        mpz_t r; mpz_init(r);
        mpz_pow_ui(r, za, e);
        VALUE rv = pys_normalise_int(r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r);
        return rv;
    }
    {
        // Negative base with non-integer exponent → complex.
        double da = pys_to_double(c, a);
        double db = pys_to_double(c, b);
        if (da < 0 && db != (double)(int64_t)db) {
            // (a)^b = exp(b * ln(a)) — a < 0 so use complex formula:
            // ln(-r) = ln(r) + i*pi, so b*ln(-r) = b*ln(r) + i*b*pi
            // exp(...) = e^{b*ln(r)} * (cos(b*pi) + i*sin(b*pi))
            double mag = pow(-da, db);
            return pys_make_complex(mag * cos(db * 3.14159265358979323846),
                                   mag * sin(db * 3.14159265358979323846));
        }
        return pys_make_float(pow(da, db));
    }
}

VALUE
pys_bit_and(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(PYS_IS_FIXNUM(a) & PYS_IS_FIXNUM(b)))
        return PYS_FIX(PYS_FIXVAL(a) & PYS_FIXVAL(b));
    {
        VALUE rd = pys_try_binop_dunder(c, PYS_INTERN_and, a, b);
        if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        rd = pys_try_binop_dunder(c, "__rand__", b, a);
        if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    {
        VALUE au = pys_unwrap_primary(a);
        VALUE bu = pys_unwrap_primary(b);
        if (pys_is_any_set(au) && pys_is_any_set(bu)) {
            VALUE av[2] = { a, b };
            return sm_intersection(c, 2, av);
        }
    }
    // dict_keys / dict_items support set ops in CPython.  Pystro
    // returns lists from .keys()/.items()/.values(); allow set
    // intersection between two lists as a courtesy so view-style
    // code (`d1.keys() & d2.keys()`) works.
    if ((pys_is_list(a) || pys_is_any_set(a)) && (pys_is_list(b) || pys_is_any_set(b))) {
        VALUE r = pys_make_set();
        size_t na = PYS_PTR(a)->list.len;
        for (size_t i = 0; i < na; i++) {
            VALUE x = PYS_PTR(a)->list.items[i];
            if (pys_contains(c, b, x)) pys_dict_set(c, r, x, PYS_NONE);
        }
        return r;
    }
    if (!pys_int_or_bool(a) || !pys_int_or_bool(b))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "unsupported operand type(s) for &");
    mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
    mpz_and(za, za, zb);
    VALUE r = pys_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

VALUE
pys_bit_or(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(PYS_IS_FIXNUM(a) & PYS_IS_FIXNUM(b)))
        return PYS_FIX(PYS_FIXVAL(a) | PYS_FIXVAL(b));
    VALUE rd = pys_try_binop_dunder(c, PYS_INTERN_or, a, b);
    if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    rd = pys_try_binop_dunder(c, "__ror__", b, a);
    if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    {
        VALUE au = pys_unwrap_primary(a);
        VALUE bu = pys_unwrap_primary(b);
        if (pys_is_any_set(au) && pys_is_any_set(bu)) {
            VALUE av[2] = { a, b };
            return sm_union(c, 2, av);
        }
    }
    if (pys_is_dict(a) && pys_is_dict(b)) {
        // dict | dict: merge (RHS wins)
        VALUE r = pys_make_dict();
        struct pysdict *src = PYS_PTR(a)->dict;
        for (size_t i = 0; i < src->elen; i++)
            if (pydict_entry_live(src, i))
                pys_dict_set(c, r, src->entries[i].key, src->entries[i].value);
        struct pysdict *src2 = PYS_PTR(b)->dict;
        for (size_t i = 0; i < src2->elen; i++)
            if (pydict_entry_live(src2, i))
                pys_dict_set(c, r, src2->entries[i].key, src2->entries[i].value);
        return r;
    }
    // list | list as set union (for dict_keys-style usage).
    if ((pys_is_list(a) || pys_is_any_set(a)) && (pys_is_list(b) || pys_is_any_set(b))) {
        VALUE r = pys_make_set();
        size_t na = PYS_PTR(a)->list.len;
        for (size_t i = 0; i < na; i++)
            pys_dict_set(c, r, PYS_PTR(a)->list.items[i], PYS_NONE);
        if (pys_is_list(b)) {
            size_t nb = PYS_PTR(b)->list.len;
            for (size_t i = 0; i < nb; i++)
                pys_dict_set(c, r, PYS_PTR(b)->list.items[i], PYS_NONE);
        } else {
            struct pysdict *bd = PYS_PTR(b)->dict;
            for (size_t i = 0; i < bd->elen; i++)
                if (pydict_entry_live(bd, i))
                    pys_dict_set(c, r, bd->entries[i].key, PYS_NONE);
        }
        return r;
    }
    // PEP 604: `int | str` constructs a UnionType.  Pystro doesn't have
    // a real UnionType class — represent it as a tuple of classes (and
    // None, for `T | None` Optional form), which is also accepted by
    // isinstance() / issubclass() / except.
    bool a_is_class_or_none = pys_is_class(a) || a == PYS_NONE;
    bool b_is_class_or_none = pys_is_class(b) || b == PYS_NONE;
    if (a_is_class_or_none && b_is_class_or_none && (a != PYS_NONE || b != PYS_NONE)) {
        VALUE items[2] = { a, b };
        return pys_make_tuple(items, 2);
    }
    // tuple | class — extend a Union: if `a` is a tuple of classes (a
    // prior union), append `b`.
    if (pys_is_tuple(a) && b_is_class_or_none) {
        size_t n = PYS_PTR(a)->list.len;
        bool all_cls = true;
        for (size_t i = 0; i < n; i++) {
            VALUE el = PYS_PTR(a)->list.items[i];
            if (!(pys_is_class(el) || el == PYS_NONE)) { all_cls = false; break; }
        }
        if (all_cls) {
            VALUE *items = (VALUE *)alloca(sizeof(VALUE) * (n + 1));
            for (size_t i = 0; i < n; i++) items[i] = PYS_PTR(a)->list.items[i];
            items[n] = b;
            return pys_make_tuple(items, n + 1);
        }
    }
    // class | tuple-of-classes (right-side accumulation).
    if (a_is_class_or_none && pys_is_tuple(b)) {
        size_t n = PYS_PTR(b)->list.len;
        bool all_cls = true;
        for (size_t i = 0; i < n; i++) {
            VALUE el = PYS_PTR(b)->list.items[i];
            if (!(pys_is_class(el) || el == PYS_NONE)) { all_cls = false; break; }
        }
        if (all_cls) {
            VALUE *items = (VALUE *)alloca(sizeof(VALUE) * (n + 1));
            items[0] = a;
            for (size_t i = 0; i < n; i++) items[i + 1] = PYS_PTR(b)->list.items[i];
            return pys_make_tuple(items, n + 1);
        }
    }
    if (!pys_int_or_bool(a) || !pys_int_or_bool(b))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "unsupported operand type(s) for |");
    mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
    mpz_ior(za, za, zb);
    VALUE r = pys_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

VALUE
pys_bit_xor(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(PYS_IS_FIXNUM(a) & PYS_IS_FIXNUM(b)))
        return PYS_FIX(PYS_FIXVAL(a) ^ PYS_FIXVAL(b));
    VALUE rd = pys_try_binop_dunder(c, PYS_INTERN_xor, a, b);
    if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    rd = pys_try_binop_dunder(c, "__rxor__", b, a);
    if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    {
        VALUE au = pys_unwrap_primary(a);
        VALUE bu = pys_unwrap_primary(b);
        if (pys_is_any_set(au) && pys_is_any_set(bu)) {
            // Symmetric difference: (a - b) | (b - a) — left-type result.
            VALUE r = pys_make_set_like(a);
            struct pysdict *aa = PYS_PTR(au)->dict;
            struct pysdict *bb = PYS_PTR(bu)->dict;
            for (size_t i = 0; i < aa->elen; i++)
                if (pydict_entry_live(aa, i) && !pys_contains(c, bu, aa->entries[i].key))
                    pys_dict_set(c, r, aa->entries[i].key, PYS_NONE);
            for (size_t i = 0; i < bb->elen; i++)
                if (pydict_entry_live(bb, i) && !pys_contains(c, au, bb->entries[i].key))
                    pys_dict_set(c, r, bb->entries[i].key, PYS_NONE);
            return r;
        }
    }
    // list ^ list as set symmetric_difference.
    if ((pys_is_list(a) || pys_is_any_set(a)) && (pys_is_list(b) || pys_is_any_set(b))) {
        VALUE r = pys_make_set();
        size_t na = PYS_PTR(a)->list.len;
        size_t nb = PYS_PTR(b)->list.len;
        // Use list iteration (works for both list & set internal storage).
        struct pys_iter ita; pys_iter_init(c, &ita, a);
        VALUE x;
        while (pys_iter_next(c, &ita, &x))
            if (!pys_contains(c, b, x)) pys_dict_set(c, r, x, PYS_NONE);
        struct pys_iter itb; pys_iter_init(c, &itb, b);
        while (pys_iter_next(c, &itb, &x))
            if (!pys_contains(c, a, x)) pys_dict_set(c, r, x, PYS_NONE);
        (void)na; (void)nb;
        return r;
    }
    if (!pys_int_or_bool(a) || !pys_int_or_bool(b))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "unsupported operand type(s) for ^");
    mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
    mpz_xor(za, za, zb);
    VALUE r = pys_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

VALUE
pys_bit_inv(CTX *c, VALUE a)
{
    if (pys_is_instance(a)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(a)->inst.cls), PYS_INTERN_invert);
        if (m != PYS_NONE) {
            VALUE av[1] = { a };
            return pys_apply(c, m, 1, av);
        }
    }
    if (!pys_int_or_bool(a))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "bad operand type for unary ~");
    mpz_t z; pys_to_mpz(c, a, z);
    mpz_com(z, z);
    VALUE r = pys_normalise_int(z);
    mpz_clear(z);
    return r;
}

VALUE
pys_lshift(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(PYS_IS_FIXNUM(a) & PYS_IS_FIXNUM(b))) {
        int64_t x = PYS_FIXVAL(a), y = PYS_FIXVAL(b);
        if (LIKELY(y >= 0 && y < 62)) {
            int64_t r = x << y;
            if (LIKELY((r >> y) == x &&
                       r >= PYS_FIXNUM_MIN && r <= PYS_FIXNUM_MAX))
                return PYS_FIX(r);
        }
    }
    {
        VALUE rd = pys_try_binop_dunder(c, PYS_INTERN_lshift, a, b);
        if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        rd = pys_try_binop_dunder(c, "__rlshift__", b, a);
        if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    if (!pys_int_or_bool(a) || !pys_int_or_bool(b))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "unsupported operand type(s) for <<");
    mpz_t zb; pys_to_mpz(c, b, zb);
    if (mpz_sgn(zb) < 0) {
        mpz_clear(zb);
        PYS_RAISE_EXC(c, c->EXC_ValueError, "negative shift count");
    }
    if (!mpz_fits_ulong_p(zb)) { mpz_clear(zb); PYS_RAISE_EXC(c, c->EXC_ValueError, "shift too large"); }
    unsigned long s = mpz_get_ui(zb);
    mpz_t za; pys_to_mpz(c, a, za);
    mpz_mul_2exp(za, za, s);
    VALUE r = pys_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

VALUE
pys_rshift(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(PYS_IS_FIXNUM(a) & PYS_IS_FIXNUM(b))) {
        int64_t x = PYS_FIXVAL(a), y = PYS_FIXVAL(b);
        if (LIKELY(y >= 0 && y < 63))
            return PYS_FIX(x >> y);
    }
    {
        VALUE rd = pys_try_binop_dunder(c, PYS_INTERN_rshift, a, b);
        if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        rd = pys_try_binop_dunder(c, "__rrshift__", b, a);
        if (rd) return rd;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    if (!pys_int_or_bool(a) || !pys_int_or_bool(b))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "unsupported operand type(s) for >>");
    mpz_t zb; pys_to_mpz(c, b, zb);
    if (mpz_sgn(zb) < 0) {
        mpz_clear(zb);
        PYS_RAISE_EXC(c, c->EXC_ValueError, "negative shift count");
    }
    if (!mpz_fits_ulong_p(zb)) { mpz_clear(zb); return PYS_FIX(0); }
    unsigned long s = mpz_get_ui(zb);
    mpz_t za; pys_to_mpz(c, a, za);
    mpz_fdiv_q_2exp(za, za, s);
    VALUE r = pys_normalise_int(za);
    mpz_clear(za); mpz_clear(zb);
    return r;
}

int
pys_cmp(CTX *c, VALUE a, VALUE b)
{
    if (pys_is_instance(a)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(a)->inst.cls), PYS_INTERN_lt);
        if (m != PYS_NONE) {
            VALUE av[2] = { a, b };
            VALUE r = pys_apply(c, m, 2, av);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            if (pys_is_truthy(r)) return -1;
            // try __eq__ for == 0
            m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(a)->inst.cls), PYS_INTERN_eq);
            if (m != PYS_NONE) {
                VALUE r2 = pys_apply(c, m, 2, av);
                if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
                if (pys_is_truthy(r2)) return 0;
            }
            return 1;
        }
        // Built-in subclass with no __lt__: compare via primary value.
        if (PYS_PTR(a)->inst.primary)
            a = PYS_PTR(a)->inst.primary;
    }
    if (pys_is_instance(b) && PYS_PTR(b)->inst.primary)
        b = PYS_PTR(b)->inst.primary;
    if (pys_is_str(a) && pys_is_str(b)) {
        size_t la = PYS_PTR(a)->str.len, lb = PYS_PTR(b)->str.len;
        size_t n = la < lb ? la : lb;
        int r = memcmp(PYS_PTR(a)->str.chars, PYS_PTR(b)->str.chars, n);
        if (r != 0) return r < 0 ? -1 : 1;
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    if (pys_is_byteseq(a) && pys_is_byteseq(b)) {
        size_t la = PYS_PTR(a)->str.len, lb = PYS_PTR(b)->str.len;
        size_t n = la < lb ? la : lb;
        int r = memcmp(PYS_PTR(a)->str.chars, PYS_PTR(b)->str.chars, n);
        if (r != 0) return r < 0 ? -1 : 1;
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    if (PYS_IS_FIXNUM(a) && PYS_IS_FIXNUM(b)) {
        int64_t ai = PYS_FIXVAL(a), bi = PYS_FIXVAL(b);
        return ai < bi ? -1 : ai > bi ? 1 : 0;
    }
    if (pys_int_or_bool(a) && pys_int_or_bool(b)) {
        mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
        int r = mpz_cmp(za, zb);
        mpz_clear(za); mpz_clear(zb);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    if ((pys_int_or_bool(a) || pys_is_float(a)) && (pys_int_or_bool(b) || pys_is_float(b))) {
        double ad = pys_to_double(c, a), bd = pys_to_double(c, b);
        return ad < bd ? -1 : ad > bd ? 1 : 0;
    }
    if ((pys_is_list(a) && pys_is_list(b)) || (pys_is_tuple(a) && pys_is_tuple(b))) {
        size_t la = PYS_PTR(a)->list.len, lb = PYS_PTR(b)->list.len;
        size_t n = la < lb ? la : lb;
        for (size_t i = 0; i < n; i++) {
            int r = pys_cmp(c, PYS_PTR(a)->list.items[i], PYS_PTR(b)->list.items[i]);
            if (r != 0) return r;
        }
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    {
        extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
        VALUE av_a[1] = { a };
        VALUE av_b[1] = { b };
        VALUE ta = bi_type(c, 1, av_a);
        VALUE tb = bi_type(c, 1, av_b);
        const char *na = (pys_is_class(ta)) ? PYS_PTR(ta)->cls.name : "?";
        const char *nb = (pys_is_class(tb)) ? PYS_PTR(tb)->cls.name : "?";
        PYS_RAISE_EXC(c, c->EXC_TypeError,
                     "'<' not supported between instances of '%s' and '%s'",
                     na, nb);
    }
}

// Set comparison helper.  Returns -1 (a strict subset of b), 0 (equal),
// 1 (a strict superset), or -2 (incomparable).  The set node_lt/le/gt/ge
// special-case sets to use this so set-vs-set respects partial ordering.
int
pys_set_cmp_partial(CTX *c, VALUE a, VALUE b)
{
    struct pysdict *aa = PYS_PTR(a)->dict;
    struct pysdict *bb = PYS_PTR(b)->dict;
    bool a_in_b = true, b_in_a = true;
    for (size_t i = 0; i < aa->elen; i++) {
        if (!pydict_entry_live(aa, i)) continue;
        if (!pys_contains(c, b, aa->entries[i].key)) { a_in_b = false; break; }
    }
    for (size_t i = 0; i < bb->elen; i++) {
        if (!pydict_entry_live(bb, i)) continue;
        if (!pys_contains(c, a, bb->entries[i].key)) { b_in_a = false; break; }
    }
    if (a_in_b && b_in_a) return 0;
    if (a_in_b) return -1;
    if (b_in_a) return 1;
    return -2;
}

// Convenience predicate: true iff entries[i] is a live (non-deleted) slot.
static inline bool
pydict_entry_live(const struct pysdict *d, size_t i)
{
    VALUE k = d->entries[i].key;
    return k != 0 && k != DICT_DELETED_KEY;
}

VALUE
pys_eq(CTX *c, VALUE a, VALUE b)
{
    // Pass PYS_INTERN_eq (interned) instead of "__eq__" literal so
    // the lookup hits the dunder slot fast path instead of MRO+strcmp.
    // deltablue's `==` was 200K+ slow lookups before this fix.
    VALUE r = pys_try_binop_dunder(c, PYS_INTERN_eq, a, b);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (r && !(PYS_IS_PTR(r) && PYS_PTR(r)->type == PYS_T_NOTIMPL)) {
        return pys_is_truthy(r) ? PYS_TRUE : PYS_FALSE;
    }
    // a's __eq__ returned NotImplemented (or wasn't defined): try b's.
    r = pys_try_binop_dunder(c, PYS_INTERN_eq, b, a);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (r && !(PYS_IS_PTR(r) && PYS_PTR(r)->type == PYS_T_NOTIMPL)) {
        return pys_is_truthy(r) ? PYS_TRUE : PYS_FALSE;
    }
    // Built-in subclass instances with no override — compare via primary.
    if (pys_is_instance(a) && PYS_PTR(a)->inst.primary)
        a = PYS_PTR(a)->inst.primary;
    if (pys_is_instance(b) && PYS_PTR(b)->inst.primary)
        b = PYS_PTR(b)->inst.primary;
    // NaN is special: NaN != NaN, even when stored in the same VALUE.
    if (pys_is_float(a)) {
        double d = PYS_IS_FLONUM(a) ? pys_flonum_to_double(a) : PYS_PTR(a)->dbl;
        if (d != d) {  // NaN check
            if (pys_is_float(b)) {
                double d2 = PYS_IS_FLONUM(b) ? pys_flonum_to_double(b) : PYS_PTR(b)->dbl;
                if (d2 != d2) return PYS_FALSE;   // NaN == NaN → False
            }
            // NaN != non-float: still False (NaN equals nothing).
            return PYS_FALSE;
        }
    }
    if (a == b) return PYS_TRUE;
    if (PYS_IS_FIXNUM(a) && PYS_IS_FIXNUM(b)) return PYS_FALSE;
    if (pys_int_or_bool(a) && pys_int_or_bool(b)) {
        mpz_t za, zb; pys_to_mpz(c, a, za); pys_to_mpz(c, b, zb);
        bool eq = (mpz_cmp(za, zb) == 0);
        mpz_clear(za); mpz_clear(zb);
        return eq ? PYS_TRUE : PYS_FALSE;
    }
    if ((pys_int_or_bool(a) || pys_is_float(a)) && (pys_int_or_bool(b) || pys_is_float(b)))
        return pys_to_double(c, a) == pys_to_double(c, b) ? PYS_TRUE : PYS_FALSE;
    if (pys_is_str(a) && pys_is_str(b)) {
        if (PYS_PTR(a)->str.len != PYS_PTR(b)->str.len) return PYS_FALSE;
        return memcmp(PYS_PTR(a)->str.chars, PYS_PTR(b)->str.chars,
                      PYS_PTR(a)->str.len) == 0 ? PYS_TRUE : PYS_FALSE;
    }
    if (pys_is_byteseq(a) && pys_is_byteseq(b)) {
        if (PYS_PTR(a)->str.len != PYS_PTR(b)->str.len) return PYS_FALSE;
        return memcmp(PYS_PTR(a)->str.chars, PYS_PTR(b)->str.chars,
                      PYS_PTR(a)->str.len) == 0 ? PYS_TRUE : PYS_FALSE;
    }
    if ((pys_is_list(a) && pys_is_list(b)) || (pys_is_tuple(a) && pys_is_tuple(b))) {
        size_t la = PYS_PTR(a)->list.len, lb = PYS_PTR(b)->list.len;
        if (la != lb) return PYS_FALSE;
        for (size_t i = 0; i < la; i++) {
            VALUE ai = PYS_PTR(a)->list.items[i];
            VALUE bi = PYS_PTR(b)->list.items[i];
            // CPython rule: identity-equality short-circuit. `[nan] ==
            // [nan]` is True when both elements are the SAME object,
            // even though `nan == nan` is False.
            if (ai == bi) continue;
            if (pys_eq(c, ai, bi) != PYS_TRUE)
                return PYS_FALSE;
        }
        return PYS_TRUE;
    }
    if (pys_is_dict(a) && pys_is_dict(b)) {
        struct pysdict *da = PYS_PTR(a)->dict, *db = PYS_PTR(b)->dict;
        if (da->used != db->used) return PYS_FALSE;
        for (size_t i = 0; i < da->elen; i++) {
            if (!pydict_entry_live(da, i)) continue;
            VALUE k = da->entries[i].key;
            bool has = pys_dict_has(c, b, k);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            if (!has) return PYS_FALSE;
            VALUE vb = pys_dict_get(c, b, k);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            VALUE eq = pys_eq(c, da->entries[i].value, vb);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            if (eq != PYS_TRUE) return PYS_FALSE;
        }
        return PYS_TRUE;
    }
    if (pys_is_complex(a) || pys_is_complex(b)) {
        double ra, ia, rb, ib;
        if (pys_to_cpx(c, a, &ra, &ia) && pys_to_cpx(c, b, &rb, &ib))
            return (ra == rb && ia == ib) ? PYS_TRUE : PYS_FALSE;
    }
    if (pys_is_any_set(a) && pys_is_any_set(b)) {
        struct pysdict *da = PYS_PTR(a)->dict, *db = PYS_PTR(b)->dict;
        if (da->used != db->used) return PYS_FALSE;
        for (size_t i = 0; i < da->elen; i++) {
            if (!pydict_entry_live(da, i)) continue;
            bool has = pys_dict_has(c, b, da->entries[i].key);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            if (!has) return PYS_FALSE;
        }
        return PYS_TRUE;
    }
    if (pys_is_bound(a) && pys_is_bound(b)) {
        // Bound methods compare equal when bound to the same object and
        // the underlying function is the same.  CPython matches by
        // self-identity and func-identity.
        return (PYS_PTR(a)->bound.self == PYS_PTR(b)->bound.self
                && PYS_PTR(a)->bound.func == PYS_PTR(b)->bound.func)
               ? PYS_TRUE : PYS_FALSE;
    }
    if (pys_is_range(a) && pys_is_range(b)) {
        // CPython: equal iff same sequence of values.  Two empty ranges
        // are equal regardless of start/step; otherwise len, start, and
        // (if len > 1) step must match.
        struct pysobj *ra = PYS_PTR(a), *rb = PYS_PTR(b);
        size_t la = pys_seq_len(c, a), lb = pys_seq_len(c, b);
        if (la != lb) return PYS_FALSE;
        if (la == 0) return PYS_TRUE;
        if (ra->range.start != rb->range.start) return PYS_FALSE;
        if (la == 1) return PYS_TRUE;
        return ra->range.step == rb->range.step ? PYS_TRUE : PYS_FALSE;
    }
    return PYS_FALSE;
}

bool
pys_eq_bool(CTX *c, VALUE a, VALUE b)
{
    return pys_eq(c, a, b) == PYS_TRUE;
}

// (pydict_entry_live moved earlier — see below)

// ---------------------------------------------------------------------------
// Hash.
// ---------------------------------------------------------------------------

// Out-of-line hash for non-fixnum values.  Fixnum is handled by the
// inline pys_hash in context.h to keep the dict-bench hot path off the
// PLT.  Recursive calls back into pys_hash() from this body re-enter
// the inline (and may recurse here for nested non-fixnum components).
//
// CONVENTION: every return path is masked to 62-bit non-negative
// (0x3FFF...) by the outer pys_hash_slow wrapper.  This is the value
// range that survives PYS_FIX round-trip — bi_hash wraps the result in
// a fixnum, user code can store it via __hash__, and our dict lookups
// read it back the same.  Without the mask, FNV-1a strings produce
// uint64 with the top 2 bits set, PYS_FIX truncates them lossy, and a
// sign-bit-set value at storage time wouldn't match the lookup-time
// recomputation.
#define PYS_HASH_MASK 0x3FFFFFFFFFFFFFFFULL
static uint64_t _pys_hash_compute(CTX *c, VALUE v);
uint64_t
pys_hash_slow(CTX *c, VALUE v)
{
    return _pys_hash_compute(c, v) & PYS_HASH_MASK;
}
static uint64_t
_pys_hash_compute(CTX *c, VALUE v)
{
    if (PYS_IS_FIXNUM(v)) {
        // Defensive: callers should have taken the inline fast path,
        // but generic dispatch sites (recursive list/tuple hashes) may
        // land here with fixnums.  CPython parity: hash(int)==int with
        // -1 → -2 special case.
        int64_t k = PYS_FIXVAL(v);
        if (k == -1) k = -2;
        return (uint64_t)k;
    }
    if (PYS_IS_FLONUM(v)) {
        double d = pys_flonum_to_double(v);
        if (d == (double)(int64_t)d) return pys_hash(c, PYS_FIX((int64_t)d));
        union { uint64_t u; double d; } pun = { .d = d == 0 ? 0 : d };
        return pun.u;
    }
    if (v == PYS_NONE)  return 0xDEADBEEFCAFEBABEULL;
    if (v == PYS_TRUE)  return pys_hash(c, PYS_FIX(1));
    if (v == PYS_FALSE) return pys_hash(c, PYS_FIX(0));
    struct pysobj *o = PYS_PTR(v);
    switch (o->type) {
      case PYS_T_FLOAT: {
        if (o->dbl == (double)(int64_t)o->dbl) return pys_hash(c, PYS_FIX((int64_t)o->dbl));
        union { uint64_t u; double d; } pun = { .d = o->dbl == 0 ? 0 : o->dbl };
        return pun.u;
      }
      case PYS_T_BIGNUM: {
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
      case PYS_T_COMPLEX: {
        union { uint64_t u; double d; } pr = { .d = o->cpx.re };
        union { uint64_t u; double d; } pi = { .d = o->cpx.im };
        return pr.u ^ pi.u;
      }
      case PYS_T_STR:
      case PYS_T_BYTES: {
        uint64_t h = 0xCBF29CE484222325ULL;
        for (size_t i = 0; i < o->str.len; i++) {
            h ^= (unsigned char)o->str.chars[i];
            h *= 0x100000001B3ULL;
        }
        return h;
      }
      case PYS_T_TUPLE: {
        uint64_t h = 0x9E3779B97F4A7C15ULL;
        for (size_t i = 0; i < o->list.len; i++) {
            h = (h ^ pys_hash(c, o->list.items[i])) * 0x100000001B3ULL;
        }
        return h;
      }
      case PYS_T_FROZENSET: {
        // XOR-of-hashes — order-independent.
        uint64_t h = 0;
        struct pysdict *d = o->dict;
        for (size_t i = 0; i < d->elen; i++)
            if (pydict_entry_live(d, i)) h ^= pys_hash(c, d->entries[i].key);
        return h ^ 0xC2B2AE3D27D4EB4FULL;
      }
      case PYS_T_RANGE: {
        // Hash the (start, stop, step) triple so equal ranges hash same.
        // Use start, len, last (CPython approximation).
        size_t L = pys_seq_len(c, v);
        if (L == 0) return 0xCBF29CE484222325ULL;  // empty: fixed
        int64_t last = o->range.start + (int64_t)(L - 1) * o->range.step;
        uint64_t h = (uint64_t)L;
        h = h * 0x9E3779B97F4A7C15ULL ^ (uint64_t)o->range.start;
        if (L > 1) h = h * 0x100000001B3ULL ^ (uint64_t)last;
        return h;
      }
      case PYS_T_INSTANCE: {
        // User-defined __hash__: call it and convert to int.
        VALUE cls = PYS_OBJ_VAL(o->inst.cls);
        // __hash__ explicitly set to None makes the type unhashable.
        if (pys_class_has_method(cls, "__hash__")) {
            VALUE hm0 = pys_class_lookup_method(cls, PYS_INTERN_hash);
            if (hm0 == PYS_NONE) {
                PYS_RAISE_EXC(c, c->EXC_TypeError,
                             "unhashable type: '%s'", o->inst.cls->cls.name);
                return 0;
            }
        }
        VALUE hm = pys_class_lookup_method(cls, PYS_INTERN_hash);
        if (hm != PYS_NONE) {
            VALUE av[1] = { v };
            VALUE r = pys_apply(c, hm, 1, av);
            if (UNLIKELY(!r)) return 0;
            // Coerce result to a 64-bit hash (signed-truncated, like CPython's PyObject_Hash).
            if (PYS_IS_FIXNUM(r)) {
                int64_t hv = PYS_FIXVAL(r);
                if (hv == -1) hv = -2;  // CPython convention.
                return (uint64_t)hv;
            }
            if (pys_is_int(r)) {
                int64_t hv = mpz_get_si(PYS_PTR(r)->mpz);
                if (hv == -1) hv = -2;
                return (uint64_t)hv;
            }
            // Non-int return → fall through to identity hash.
        }
        // Built-in subclass (e.g. StrSub(str)): no user __hash__ defined,
        // delegate to the primary value's hash so that
        // `dict[StrSub('key3')] == dict['key3']` works (CPython parity).
        if (o->inst.primary)
            return pys_hash(c, o->inst.primary);
        return (uint64_t)(uintptr_t)o * 0x9E3779B97F4A7C15ULL;
      }
      case PYS_T_LIST:
        PYS_RAISE_EXC(c, c->EXC_TypeError, "unhashable type: 'list'");
        return 0;
      case PYS_T_DICT:
        PYS_RAISE_EXC(c, c->EXC_TypeError, "unhashable type: 'dict'");
        return 0;
      case PYS_T_SET:
        PYS_RAISE_EXC(c, c->EXC_TypeError, "unhashable type: 'set'");
        return 0;
      case PYS_T_BYTEARRAY:
        PYS_RAISE_EXC(c, c->EXC_TypeError, "unhashable type: 'bytearray'");
        return 0;
      case PYS_T_BOUND_METHOD: {
        // Hash by (self, func) so equal bound methods hash to the same
        // bucket — matches the eq rule.
        uint64_t hs = pys_hash(c, o->bound.self);
        uint64_t hf = pys_hash(c, o->bound.func);
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

struct pysdict *
pydict_new(void)
{
    struct pysdict *d = (struct pysdict *)GC_malloc(sizeof(struct pysdict));
    d->icapa = DICT_INIT_CAPA;
    d->ecapa = DICT_INIT_CAPA;
    d->elen = 0;
    d->used = 0;
    d->fill = 0;
    d->indices = (int32_t *)GC_malloc_atomic(sizeof(int32_t) * d->icapa);
    for (size_t i = 0; i < d->icapa; i++) d->indices[i] = DICT_EMPTY_IDX;
    d->entries = (struct pysdict_entry *)GC_malloc(sizeof(struct pysdict_entry) * d->ecapa);
    return d;
}

VALUE
pys_make_dict(void)
{
    struct pysobj *o = pys_alloc(PYS_T_DICT);
    o->dict = pydict_new();
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_set(void)
{
    // A set is implemented as a dict-shaped table where only keys are
    // tracked.  We reuse `pysdict` for the storage and ignore values
    // (always PYS_NONE) on reads.  The PYS_T_SET tag selects the right
    // display / dunder behaviour.
    struct pysobj *o = pys_alloc(PYS_T_SET);
    o->dict = pydict_new();
    return PYS_OBJ_VAL(o);
}

VALUE
pys_make_frozenset(void)
{
    struct pysobj *o = pys_alloc(PYS_T_FROZENSET);
    o->dict = pydict_new();
    return PYS_OBJ_VAL(o);
}

// Look up `key` in `d->indices`.  Returns:
//   - the bucket index in *out_bucket
//   - the entry index (>=0) in *out_eidx if found, else -1
// `*out_first_tomb` points at the first tombstone bucket we passed
// (so an insert can reuse it without further search).
static inline void
pydict_indices_lookup(CTX * restrict c, struct pysdict * restrict d,
                      VALUE key, uint64_t h,
                      size_t *out_bucket, int32_t *out_eidx,
                      ssize_t *out_first_tomb)
{
    size_t mask = d->icapa - 1;
    size_t i = (size_t)h & mask;
    size_t step = 0;
    ssize_t first_tomb = -1;
    bool key_is_none = (key == PYS_NONE);
    for (;;) {
        int32_t idx = d->indices[i];
        if (idx == DICT_EMPTY_IDX) {
            *out_bucket = i; *out_eidx = -1; *out_first_tomb = first_tomb; return;
        }
        if (idx == DICT_TOMB_IDX) {
            if (first_tomb < 0) first_tomb = (ssize_t)i;
        } else {
            struct pysdict_entry *e = &d->entries[idx];
            if (LIKELY(e->hash == h)) {
                if (LIKELY(e->key == key)) {
                    *out_bucket = i; *out_eidx = idx; *out_first_tomb = first_tomb; return;
                }
                if (key_is_none || e->key == PYS_NONE) {
                    /* None equals only itself. */
                }
                else if (pys_is_str(key) && pys_is_str(e->key)) {
                    size_t l1 = PYS_PTR(key)->str.len, l2 = PYS_PTR(e->key)->str.len;
                    if (l1 == l2 && memcmp(PYS_PTR(key)->str.chars,
                                           PYS_PTR(e->key)->str.chars, l1) == 0) {
                        *out_bucket = i; *out_eidx = idx; *out_first_tomb = first_tomb; return;
                    }
                }
                else if (pys_eq_bool(c, e->key, key)) {
                    *out_bucket = i; *out_eidx = idx; *out_first_tomb = first_tomb; return;
                }
                // __eq__ raised: bail out so callers don't operate on
                // an inconsistent (state==RAISE, eidx=-1) result.
                if (UNLIKELY(c->state == PYS_STATE_RAISE)) {
                    *out_bucket = i; *out_eidx = -1; *out_first_tomb = first_tomb; return;
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
pydict_find(CTX * restrict c, struct pysdict * restrict d, VALUE key, uint64_t h)
{
    size_t mask = d->icapa - 1;
    size_t i = (size_t)h & mask;
    size_t step = 0;
    bool key_is_none = (key == PYS_NONE);
    for (;;) {
        int32_t idx = d->indices[i];
        if (idx == DICT_EMPTY_IDX) return -1;
        if (idx != DICT_TOMB_IDX) {
            struct pysdict_entry *e = &d->entries[idx];
            if (LIKELY(e->hash == h)) {
                if (LIKELY(e->key == key)) return idx;
                // None equals only itself.
                if (key_is_none || e->key == PYS_NONE) {
                    /* skip pys_eq */
                }
                else if (pys_is_str(key) && pys_is_str(e->key)) {
                    size_t l1 = PYS_PTR(key)->str.len, l2 = PYS_PTR(e->key)->str.len;
                    if (l1 == l2 && memcmp(PYS_PTR(key)->str.chars,
                                           PYS_PTR(e->key)->str.chars, l1) == 0) return idx;
                }
                else if (pys_eq_bool(c, e->key, key)) return idx;
                if (UNLIKELY(c->state == PYS_STATE_RAISE)) return -1;
            }
        }
        step++;
        i = (i + step) & mask;
    }
}

// Resize indices[] (without touching entries[] in the common case) — or
// also compact entries[] if we have a lot of deleted entries.
static void
pydict_resize(struct pysdict *d, size_t new_icapa, bool compact_entries)
{
    d->version++;       // bpo-46615: resize invalidates entries[] indexing
    int32_t *new_indices = (int32_t *)GC_malloc_atomic(sizeof(int32_t) * new_icapa);
    for (size_t i = 0; i < new_icapa; i++) new_indices[i] = DICT_EMPTY_IDX;
    size_t mask = new_icapa - 1;

    if (compact_entries) {
        // Walk old entries[] in insertion order, copy live ones to a
        // new dense entries[], rebuild indices[].
        size_t new_ecapa = d->used > DICT_INIT_CAPA ? d->used * 2 : DICT_INIT_CAPA;
        struct pysdict_entry *new_entries = (struct pysdict_entry *)GC_malloc(
            sizeof(struct pysdict_entry) * new_ecapa);
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
pydict_grow_entries(struct pysdict *d)
{
    size_t nc = d->ecapa * 2;
    struct pysdict_entry *ne = (struct pysdict_entry *)GC_malloc(sizeof(struct pysdict_entry) * nc);
    memcpy(ne, d->entries, sizeof(struct pysdict_entry) * d->elen);
    d->entries = ne;
    d->ecapa = nc;
}

// Lower-level set: takes a struct pysdict* and a precomputed hash.
// Used both by pys_dict_set and by callers (instance attrs) that hold
// a struct pysdict* directly.  Returns the entries[] index where the
// (possibly new) value lives — saves callers the linear scan they used
// to do to refresh their attr_cache after a fresh insert.  Pre-resize:
// the index is stable; post-resize (rare) the caller refetches.
int32_t
pydict_set_h(CTX *c, struct pysdict *d, VALUE key, uint64_t h, VALUE val)
{
    size_t bucket;
    int32_t eidx;
    ssize_t first_tomb;
    pydict_indices_lookup(c, d, key, h, &bucket, &eidx, &first_tomb);
    // __eq__ during lookup raised — don't insert with a possibly-stale
    // bucket/tomb; the caller's exception must propagate.
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return -1;
    if (eidx >= 0) {
        d->entries[eidx].value = val;
        return eidx;
    }
    if (d->elen == d->ecapa) pydict_grow_entries(d);
    int32_t new_idx = (int32_t)d->elen;
    d->entries[new_idx].key = key;
    d->entries[new_idx].value = val;
    d->entries[new_idx].hash = h;
    d->elen++;
    d->used++;
    d->version++;       // bpo-46615: mutation invalidates active iterators
    if (first_tomb >= 0) {
        d->indices[first_tomb] = new_idx;
    } else {
        d->indices[bucket] = new_idx;
        d->fill++;
    }
    if (UNLIKELY(d->fill * DICT_LOAD_DEN >= d->icapa * DICT_LOAD_NUM)) {
        bool compact = (d->elen - d->used) * 2 >= d->elen;
        pydict_resize(d, d->icapa * 2, compact);
        // entries[] is rebuilt by resize when compacting; index becomes
        // unreliable.  Return -1 so callers know to refresh externally.
        if (compact) return -1;
    }
    return new_idx;
}

void
pys_dict_set(CTX *c, VALUE dv, VALUE key, VALUE val)
{
    struct pysdict *d = PYS_PTR(dv)->dict;
    uint64_t h = pys_hash(c, key);
    pydict_set_h(c, d, key, h, val);
    return;
}

// Truthiness dispatcher for user instances: tries `__bool__` then
// `__len__`; defaults to true.  Uses the active context (no CTX
// argument needed since `pys_is_truthy` is called from many places
// without a CTX in scope).
bool
pys_is_truthy_instance(VALUE v)
{
    extern CTX *pys_current_ctx;
    CTX *c = pys_current_ctx;
    VALUE cls = PYS_OBJ_VAL(PYS_PTR(v)->inst.cls);
    VALUE m = pys_class_lookup_method(cls, PYS_INTERN_bool);
    if (m != PYS_NONE) {
        VALUE av[1] = { v };
        VALUE r = pys_apply(c, m, 1, av);
        if (c->state == PYS_STATE_RAISE) return false;
        return r == PYS_TRUE || (PYS_IS_FIXNUM(r) && PYS_FIXVAL(r) != 0);
    }
    VALUE lm = pys_class_lookup_method(cls, PYS_INTERN_len);
    if (lm != PYS_NONE) {
        VALUE av[1] = { v };
        VALUE r = pys_apply(c, lm, 1, av);
        if (c->state == PYS_STATE_RAISE) return false;
        return PYS_IS_FIXNUM(r) ? PYS_FIXVAL(r) != 0 : true;
    }
    // Built-in subclass without overrides: defer to primary's truthiness
    // so `bool(class D(dict): pass; D())` returns False for an empty D.
    if (PYS_PTR(v)->inst.primary)
        return pys_is_truthy(PYS_PTR(v)->inst.primary);
    return true;
}

void
pys_func_set_doc(CTX *c, VALUE fn, const char *s)
{
    if (!pys_is_func(fn) || !s) return;
    struct pysobj *o = PYS_PTR(fn);
    if (!o->func.attrs) o->func.attrs = pydict_new();
    VALUE k = pys_make_str("__doc__", 7);
    pydict_set_h(c, o->func.attrs, k, pys_hash(c, k), pys_make_str(s, strlen(s)));
}

VALUE
pys_dict_get(CTX *c, VALUE dv, VALUE key)
{
    struct pysdict *d = PYS_PTR(dv)->dict;
    uint64_t h = pys_hash(c, key);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    size_t bucket; int32_t eidx; ssize_t ft;
    pydict_indices_lookup(c, d, key, h, &bucket, &eidx, &ft);
    // __eq__ raised while comparing during lookup — don't mask it with
    // KeyError.
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (eidx < 0) {
        // KeyError(key) — CPython parity: e.args == (key,), preserving
        // the original value's type (int, tuple, ...).  Earlier we
        // stringified the key which broke tests asserting on args[0]
        // type.
        VALUE inst = pys_make_instance(c->EXC_KeyError);
        pys_setattr(c, inst, "args", pys_make_tuple(&key, 1));
        pys_setattr(c, inst, "__context__", c->current_handling_exc ? c->current_handling_exc : PYS_NONE);
        pys_setattr(c, inst, "__cause__", PYS_NONE);
        pys_setattr(c, inst, "__suppress_context__", PYS_FALSE);
        c->state = PYS_STATE_RAISE;
        c->state_value = inst;
        return 0;
    }
    return d->entries[eidx].value;
}

bool
pys_dict_has(CTX *c, VALUE dv, VALUE key)
{
    struct pysdict *d = PYS_PTR(dv)->dict;
    uint64_t h = pys_hash(c, key);
    size_t bucket; int32_t eidx; ssize_t ft;
    pydict_indices_lookup(c, d, key, h, &bucket, &eidx, &ft);
    return eidx >= 0;
}

bool
pys_dict_remove(CTX *c, VALUE dv, VALUE key)
{
    struct pysdict *d = PYS_PTR(dv)->dict;
    uint64_t h = pys_hash(c, key);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return false;
    size_t bucket; int32_t eidx; ssize_t ft;
    pydict_indices_lookup(c, d, key, h, &bucket, &eidx, &ft);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return false;
    if (eidx < 0) return false;
    d->indices[bucket] = DICT_TOMB_IDX;
    d->entries[eidx].key = DICT_DELETED_KEY;
    d->entries[eidx].value = PYS_NONE;
    d->used--;
    d->version++;       // bpo-46615: mutation invalidates active iterators
    return true;
}

// ---------------------------------------------------------------------------
// List ops.
// ---------------------------------------------------------------------------

void
pys_list_append(CTX *c, VALUE lv, VALUE v)
{
    (void)c;
    struct pysobj *o = PYS_PTR(lv);
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
pys_int_to_long(CTX *c, VALUE v)
{
    if (PYS_IS_FIXNUM(v)) return PYS_FIXVAL(v);
    if (v == PYS_TRUE) return 1;
    if (v == PYS_FALSE) return 0;
    if (pys_is_bignum(v)) {
        if (mpz_fits_slong_p(PYS_PTR(v)->mpz)) return mpz_get_si(PYS_PTR(v)->mpz);
        // Bignum out of long range — clamp to long extreme so slice
        // callers naturally get empty/everything.  CPython does the
        // same for slice indices.
        return mpz_sgn(PYS_PTR(v)->mpz) < 0 ? INT64_MIN : INT64_MAX;
    }
    // __index__ protocol.
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), PYS_INTERN_index);
        if (m != PYS_NONE) {
            VALUE r = pys_apply(c, m, 1, &v);
            if (c->state == PYS_STATE_RAISE) return 0;
            return pys_int_to_long(c, r);
        }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "expected an integer index");
}

// Strict variant: for non-slice indexing, oversized bignum should raise.
int64_t
pys_int_to_long_strict(CTX *c, VALUE v)
{
    if (PYS_IS_FIXNUM(v)) return PYS_FIXVAL(v);
    if (v == PYS_TRUE) return 1;
    if (v == PYS_FALSE) return 0;
    if (pys_is_bignum(v)) {
        if (mpz_fits_slong_p(PYS_PTR(v)->mpz)) return mpz_get_si(PYS_PTR(v)->mpz);
        PYS_RAISE_EXC(c, c->EXC_OverflowError, "Python int too large to convert to C long");
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "expected an integer");
}

VALUE
pys_list_get(CTX *c, VALUE seq, VALUE idx)
{
    if (pys_is_instance(seq)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(seq)->inst.cls), PYS_INTERN_getitem);
        if (m != PYS_NONE) {
            VALUE av[2] = { seq, idx };
            return pys_apply(c, m, 2, av);
        }
        // Built-in subclass: forward to primary, then dispatch to
        // __missing__ on KeyError if defined (dict-subclass protocol).
        if (PYS_PTR(seq)->inst.primary) {
            VALUE primary = PYS_PTR(seq)->inst.primary;
            if (pys_is_dict(primary) && !pys_dict_has(c, primary, idx)) {
                VALUE miss = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(seq)->inst.cls), "__missing__");
                if (miss != PYS_NONE) {
                    VALUE av[2] = { seq, idx };
                    return pys_apply(c, miss, 2, av);
                }
            }
            return pys_list_get(c, primary, idx);
        }
    }
    if (PYS_IS_PTR(seq) && PYS_PTR(seq)->type == PYS_T_MEMVIEW) {
        struct pysobj *mv = PYS_PTR(seq);
        struct pysobj *src = PYS_PTR(mv->memview.source);
        if (PYS_IS_PTR(idx) && PYS_PTR(idx)->type == PYS_T_SLICE) {
            struct pysobj *sl = PYS_PTR(idx);
            int64_t a, b, st;
            int64_t len = (int64_t)mv->memview.len;
            st = (sl->slice_.step == PYS_NONE) ? 1 : pys_int_to_long(c, sl->slice_.step);
            if (st != 1) PYS_RAISE_EXC(c, c->EXC_ValueError, "memoryview: only step=1 slicing");
            a = (sl->slice_.start == PYS_NONE) ? 0 : pys_int_to_long(c, sl->slice_.start);
            b = (sl->slice_.stop == PYS_NONE) ? len : pys_int_to_long(c, sl->slice_.stop);
            if (a < 0) { a += len; }
            if (a < 0) { a = 0; }
            if (a > len) { a = len; }
            if (b < 0) { b += len; }
            if (b < 0) { b = 0; }
            if (b > len) { b = len; }
            if (b < a) { b = a; }
            struct pysobj *r = pys_alloc(PYS_T_MEMVIEW);
            r->memview.source = mv->memview.source;
            r->memview.off = mv->memview.off + (size_t)a;
            r->memview.len = (size_t)(b - a);
            return PYS_OBJ_VAL(r);
        }
        int64_t i = pys_int_to_long(c, idx);
        int64_t len = (int64_t)mv->memview.len;
        if (i < 0) i += len;
        if (i < 0 || i >= len) PYS_RAISE_EXC(c, c->EXC_IndexError, "memoryview index out of range");
        return PYS_FIX((unsigned char)src->str.chars[mv->memview.off + (size_t)i]);
    }
    // Slice index: convert to pys_list_slice call.
    if (PYS_IS_PTR(idx) && PYS_PTR(idx)->type == PYS_T_SLICE) {
        struct pysobj *sl = PYS_PTR(idx);
        return pys_list_slice(c, seq, sl->slice_.start, sl->slice_.stop, sl->slice_.step);
    }
    if (pys_is_list(seq) || pys_is_tuple(seq)) {
        // CPython's TypeError: "list indices must be integers or slices, not <type>"
        // wins over IndexError when the index is the wrong type entirely.
        if (!PYS_IS_FIXNUM(idx) && !pys_is_bignum(idx) && idx != PYS_TRUE && idx != PYS_FALSE
            && !(pys_is_instance(idx)
                 && pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(idx)->inst.cls), PYS_INTERN_index) != PYS_NONE)) {
            extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
            VALUE av_t[1] = { idx };
            VALUE tt = bi_type(c, 1, av_t);
            const char *tn = (pys_is_class(tt)) ? PYS_PTR(tt)->cls.name : "?";
            const char *seqkind = pys_is_list(seq) ? "list" : "tuple";
            PYS_RAISE_EXC(c, c->EXC_TypeError,
                "%s indices must be integers or slices, not %s", seqkind, tn);
        }
        int64_t i = pys_int_to_long(c, idx);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        int64_t len = (int64_t)PYS_PTR(seq)->list.len;
        i = clamp_idx(i, len, false);
        if (i < 0 || i >= len) PYS_RAISE_EXC(c, c->EXC_IndexError, "list index out of range");
        return PYS_PTR(seq)->list.items[i];
    }
    if (pys_is_str(seq)) {
        // Codepoint-indexed.  Walk UTF-8 to find the i-th codepoint.
        const char *s = PYS_PTR(seq)->str.chars;
        size_t bytelen = PYS_PTR(seq)->str.len;
        size_t cp_count = pys_str_cp_count(s, bytelen);
        int64_t i = pys_int_to_long(c, idx);
        if (i < 0) i += (int64_t)cp_count;
        if (i < 0 || i >= (int64_t)cp_count)
            PYS_RAISE_EXC(c, c->EXC_IndexError, "string index out of range");
        size_t off = pys_str_cp_to_byte(s, bytelen, i);
        // Determine codepoint byte length.
        unsigned char b = (unsigned char)s[off];
        int step;
        if (b < 0x80)               step = 1;
        else if ((b & 0xE0) == 0xC0) step = 2;
        else if ((b & 0xF0) == 0xE0) step = 3;
        else if ((b & 0xF8) == 0xF0) step = 4;
        else                          step = 1;
        return pys_make_str(s + off, step);
    }
    if (pys_is_byteseq(seq)) {
        int64_t i = pys_int_to_long(c, idx);
        int64_t len = (int64_t)PYS_PTR(seq)->str.len;
        i = clamp_idx(i, len, false);
        if (i < 0 || i >= len) PYS_RAISE_EXC(c, c->EXC_IndexError, "bytes index out of range");
        return PYS_FIX((unsigned char)PYS_PTR(seq)->str.chars[i]);
    }
    if (pys_is_range(seq)) {
        struct pysobj *o = PYS_PTR(seq);
        size_t total = pys_seq_len(c, seq);
        int64_t i = pys_int_to_long(c, idx);
        if (i < 0) i += (int64_t)total;
        if (i < 0 || i >= (int64_t)total) PYS_RAISE_EXC(c, c->EXC_IndexError, "range index out of range");
        return pys_make_int(o->range.start + i * o->range.step);
    }
    if (pys_is_dict(seq)) {
        return pys_dict_get(c, seq, idx);
    }
    // `cls[arg]` — class subscript via __class_getitem__ or
    // metaclass `__getitem__`.
    if (pys_is_class(seq)) {
        VALUE m = pys_class_lookup_method(seq, "__class_getitem__");
        if (m != PYS_NONE) {
            // Unwrap @classmethod so the call binds `cls` from the
            // class on which __class_getitem__ was looked up.
            if (PYS_IS_PTR(m) && PYS_PTR(m)->type == PYS_T_CLASSMETHOD) {
                m = PYS_PTR(m)->wrap.wrapped;
            }
            VALUE av[2] = { seq, idx };
            return pys_apply(c, m, 2, av);
        }
        // Metaclass __getitem__: e.g. _AbcMeta in collections.abc.
        VALUE meta = pys_class_lookup_method(seq, PYS_INTERN_metaclass);
        if (meta != PYS_NONE && pys_is_class(meta)) {
            VALUE mg = pys_class_lookup_method(meta, PYS_INTERN_getitem);
            if (mg != PYS_NONE) {
                VALUE av[2] = { seq, idx };
                return pys_apply(c, mg, 2, av);
            }
        }
        // PEP 585: built-in container generic alias.  list[int],
        // dict[str, int], tuple[int, ...], set[int], frozenset[int],
        // type[T] — pystro doesn't model the alias type, just return the
        // class itself so annotations parse without error.
        const char *nm = PYS_PTR(seq)->cls.name;
        if (strcmp(nm, "list") == 0 || strcmp(nm, "dict") == 0 ||
            strcmp(nm, "tuple") == 0 || strcmp(nm, "set") == 0 ||
            strcmp(nm, "frozenset") == 0 || strcmp(nm, "type") == 0) {
            return seq;
        }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "object is not subscriptable");
}

VALUE
pys_list_set(CTX *c, VALUE seq, VALUE idx, VALUE val)
{
    if (pys_is_instance(seq)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(seq)->inst.cls), PYS_INTERN_setitem);
        if (m != PYS_NONE) {
            VALUE av[3] = { seq, idx, val };
            return pys_apply(c, m, 3, av);
        }
        if (PYS_PTR(seq)->inst.primary) return pys_list_set(c, PYS_PTR(seq)->inst.primary, idx, val);
    }
    if (pys_is_list(seq)) {
        if (!PYS_IS_FIXNUM(idx) && !pys_is_bignum(idx) && idx != PYS_TRUE && idx != PYS_FALSE
            && !(pys_is_instance(idx)
                 && pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(idx)->inst.cls), PYS_INTERN_index) != PYS_NONE)
            && !(PYS_IS_PTR(idx) && PYS_PTR(idx)->type == PYS_T_SLICE)) {
            extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
            VALUE av_t[1] = { idx };
            VALUE tt = bi_type(c, 1, av_t);
            const char *tn = (pys_is_class(tt)) ? PYS_PTR(tt)->cls.name : "?";
            PYS_RAISE_EXC(c, c->EXC_TypeError,
                "list indices must be integers or slices, not %s", tn);
        }
        // slice-form: forward to pys_list_slice_set so step==0 / length-
        // mismatch errors come out as ValueError rather than TypeError.
        if (PYS_IS_PTR(idx) && PYS_PTR(idx)->type == PYS_T_SLICE) {
            struct pysobj *sl = PYS_PTR(idx);
            pys_list_slice_set(c, seq, sl->slice_.start, sl->slice_.stop, sl->slice_.step, val);
            return PYS_NONE;
        }
        int64_t i = pys_int_to_long(c, idx);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        int64_t len = (int64_t)PYS_PTR(seq)->list.len;
        i = clamp_idx(i, len, false);
        if (i < 0 || i >= len) PYS_RAISE_EXC(c, c->EXC_IndexError, "list assignment index out of range");
        PYS_PTR(seq)->list.items[i] = val;
        return PYS_NONE;
    }
    if (pys_is_dict(seq)) { pys_dict_set(c, seq, idx, val); return PYS_NONE; }
    if (PYS_IS_PTR(seq) && PYS_PTR(seq)->type == PYS_T_BYTEARRAY) {
        int64_t i = pys_int_to_long(c, idx);
        int64_t len = (int64_t)PYS_PTR(seq)->str.len;
        if (i < 0) i += len;
        if (i < 0 || i >= len)
            PYS_RAISE_EXC(c, c->EXC_IndexError, "bytearray index out of range");
        int64_t b = pys_int_to_long(c, val);
        if (b < 0 || b > 255)
            PYS_RAISE_EXC(c, c->EXC_ValueError, "byte must be in range(0, 256)");
        PYS_PTR(seq)->str.chars[i] = (char)b;
        return PYS_NONE;
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "object does not support item assignment");
}

VALUE
pys_list_slice(CTX *c, VALUE seq, VALUE start, VALUE stop, VALUE step)
{
    // User-class with __getitem__ — call with slice() object.
    if (pys_is_instance(seq)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(seq)->inst.cls), PYS_INTERN_getitem);
        if (m != PYS_NONE) {
            struct pysobj *sl = pys_alloc(PYS_T_SLICE);
            sl->slice_.start = start;
            sl->slice_.stop = stop;
            sl->slice_.step = step;
            VALUE av[2] = { seq, PYS_OBJ_VAL(sl) };
            return pys_apply(c, m, 2, av);
        }
        if (PYS_PTR(seq)->inst.primary)
            return pys_list_slice(c, PYS_PTR(seq)->inst.primary, start, stop, step);
    }
    // memoryview slice → new memoryview onto same buffer.
    if (PYS_IS_PTR(seq) && PYS_PTR(seq)->type == PYS_T_MEMVIEW) {
        struct pysobj *mv = PYS_PTR(seq);
        int64_t mlen = (int64_t)mv->memview.len;
        int64_t st = (step == PYS_NONE) ? 1 : pys_int_to_long(c, step);
        if (st != 1) PYS_RAISE_EXC(c, c->EXC_ValueError, "memoryview: only step=1 slicing");
        int64_t a = (start == PYS_NONE) ? 0 : pys_int_to_long(c, start);
        int64_t b = (stop  == PYS_NONE) ? mlen : pys_int_to_long(c, stop);
        if (a < 0) a += mlen;
        if (a < 0) a = 0;
        if (a > mlen) a = mlen;
        if (b < 0) b += mlen;
        if (b < 0) b = 0;
        if (b > mlen) b = mlen;
        if (b < a) b = a;
        struct pysobj *r = pys_alloc(PYS_T_MEMVIEW);
        r->memview.source = mv->memview.source;
        r->memview.off = mv->memview.off + (size_t)a;
        r->memview.len = (size_t)(b - a);
        return PYS_OBJ_VAL(r);
    }
    int64_t len;
    bool is_str = pys_is_str(seq);
    bool is_byteseq = pys_is_byteseq(seq);
    bool is_range_seq = pys_is_range(seq);
    // For str: length is codepoint count, not bytes.
    if (is_str) len = (int64_t)pys_str_cp_count(PYS_PTR(seq)->str.chars,
                                                PYS_PTR(seq)->str.len);
    else if (is_byteseq) len = (int64_t)PYS_PTR(seq)->str.len;
    else if (pys_is_list(seq) || pys_is_tuple(seq)) len = (int64_t)PYS_PTR(seq)->list.len;
    else if (is_range_seq) {
        struct pysobj *o = PYS_PTR(seq);
        len = (o->range.step > 0)
            ? ((o->range.start >= o->range.stop) ? 0 : (o->range.stop - o->range.start + o->range.step - 1) / o->range.step)
            : ((o->range.start <= o->range.stop) ? 0 : (o->range.start - o->range.stop + (-o->range.step) - 1) / (-o->range.step));
    }
    else PYS_RAISE_EXC(c, c->EXC_TypeError, "object is not sliceable");

    int64_t st = (step == PYS_NONE) ? 1 : pys_int_to_long(c, step);
    if (st == 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "slice step cannot be zero");

    int64_t a, b;
    if (start == PYS_NONE) a = (st > 0) ? 0 : len - 1;
    else                  a = clamp_idx(pys_int_to_long(c, start), len, st > 0);
    if (stop == PYS_NONE)  b = (st > 0) ? len : -1;
    else                  b = clamp_idx(pys_int_to_long(c, stop), len, st > 0);

    // Pythonic slice clamping for negative step.
    if (st < 0) {
        if (a >= len) a = len - 1;
        if (b < -1)   b = -1;
    } else {
        if (a < 0) a = 0;
        if (b > len) b = len;
    }

    // n = ceil(|b-a| / |st|).  Compute via (diff-1)/|st| + 1 to avoid
    // overflow in `(b-a) + st-1` when st is huge (e.g. sys.maxsize).
    size_t n = 0;
    if (st > 0 && a < b) n = (size_t)(((b - a) - 1) / st + 1);
    else if (st < 0 && a > b) n = (size_t)(((a - b) - 1) / (-st) + 1);

    if (is_str) {
        // Codepoint indices a / b — convert to byte ranges.  For step 1
        // we can borrow a contiguous range of the parent UTF-8 buffer.
        const char *src = PYS_PTR(seq)->str.chars;
        size_t bytelen = PYS_PTR(seq)->str.len;
        if (st == 1) {
            size_t boff = pys_str_cp_to_byte(src, bytelen, a);
            size_t eoff = pys_str_cp_to_byte(src, bytelen, b);
            return pys_make_str_borrow(src + boff, eoff - boff);
        }
        // Stepped: walk codepoint by codepoint, copy each.
        // Pre-build an array of byte offsets per codepoint up to len.
        size_t *cp_off = (size_t *)alloca(sizeof(size_t) * ((size_t)len + 1));
        size_t bi = 0; size_t ci = 0;
        cp_off[0] = 0;
        while (bi < bytelen) {
            unsigned char bb = (unsigned char)src[bi];
            int step_b;
            if (bb < 0x80)               step_b = 1;
            else if ((bb & 0xE0) == 0xC0) step_b = 2;
            else if ((bb & 0xF0) == 0xE0) step_b = 3;
            else if ((bb & 0xF8) == 0xF0) step_b = 4;
            else                          step_b = 1;
            bi += step_b;
            ci++;
            cp_off[ci] = bi;
        }
        // Now collect codepoints a, a+st, a+2st, ... → n of them.
        // Worst-case each codepoint is 4 bytes.
        char *buf = (char *)GC_malloc_atomic(n * 4 + 1);
        size_t out = 0;
        for (size_t i = 0; i < n; i++) {
            size_t cp = (size_t)(a + (int64_t)i * st);
            size_t b0 = cp_off[cp];
            size_t b1 = cp_off[cp + 1];
            for (size_t k = b0; k < b1; k++) buf[out++] = src[k];
        }
        buf[out] = '\0';
        return pys_make_str_take(buf, out);
    }
    if (is_byteseq) {
        char *buf = (char *)GC_malloc_atomic(n + 1);
        for (size_t i = 0; i < n; i++) buf[i] = PYS_PTR(seq)->str.chars[a + (int64_t)i * st];
        buf[n] = '\0';
        return pys_is_bytes(seq) ? pys_make_bytes(buf, n) : pys_make_bytearray(buf, n);
    }
    if (is_range_seq) {
        // Slicing a range returns a list of ints (step != 1).  Build
        // directly into the result list's GC-allocated items[] to avoid
        // alloca stack overflow for large slices.
        struct pysobj *src_o = PYS_PTR(seq);
        struct pysobj *o = pys_alloc(PYS_T_LIST);
        size_t capa = n < 4 ? 4 : n;
        o->list.items = (VALUE *)GC_malloc(sizeof(VALUE) * capa);
        o->list.len = n;
        o->list.capa = capa;
        for (size_t i = 0; i < n; i++) {
            int64_t idx = a + (int64_t)i * st;
            o->list.items[i] = pys_make_int(src_o->range.start + idx * src_o->range.step);
        }
        return PYS_OBJ_VAL(o);
    }
    // List / tuple slice — write directly into a GC-allocated items[]
    // buffer (alloca for n elements blows the stack for large slices).
    bool out_is_list = pys_is_list(seq);
    struct pysobj *o = pys_alloc(out_is_list ? PYS_T_LIST : PYS_T_TUPLE);
    size_t capa = out_is_list ? (n < 4 ? 4 : n) : n;
    o->list.items = capa ? (VALUE *)GC_malloc(sizeof(VALUE) * capa) : NULL;
    o->list.len = n;
    o->list.capa = capa;
    for (size_t i = 0; i < n; i++)
        o->list.items[i] = PYS_PTR(seq)->list.items[a + (int64_t)i * st];
    return PYS_OBJ_VAL(o);
}

// Slice-assign for lists.  For step == 1, supports general resize:
// `a[i:j] = list` deletes a[i:j] and inserts the items from `val` at i.
// Other steps require len(val) == len(slice).
void
pys_list_slice_set(CTX *c, VALUE seq, VALUE start, VALUE stop, VALUE step, VALUE val)
{
    if (pys_is_instance(seq)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(seq)->inst.cls), PYS_INTERN_setitem);
        if (m != PYS_NONE) {
            struct pysobj *sl = pys_alloc(PYS_T_SLICE);
            sl->slice_.start = start;
            sl->slice_.stop = stop;
            sl->slice_.step = step;
            VALUE av[3] = { seq, PYS_OBJ_VAL(sl), val };
            pys_apply(c, m, 3, av);
            return;
        }
        if (PYS_PTR(seq)->inst.primary) {
            pys_list_slice_set(c, PYS_PTR(seq)->inst.primary, start, stop, step, val);
            return;
        }
    }
    if (PYS_IS_PTR(seq) && PYS_PTR(seq)->type == PYS_T_BYTEARRAY) {
        // bytearray slice assignment.  Source must be iterable of ints
        // 0..255 (or another bytes/bytearray).
        int64_t st = (step == PYS_NONE) ? 1 : pys_int_to_long(c, step);
        if (st == 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "slice step cannot be zero");
        int64_t len = (int64_t)PYS_PTR(seq)->str.len;
        int64_t a = (start == PYS_NONE) ? (st > 0 ? 0 : len - 1) : pys_int_to_long(c, start);
        int64_t b = (stop  == PYS_NONE) ? (st > 0 ? len : -1)    : pys_int_to_long(c, stop);
        if (a < 0) a += len;
        if (b < 0 && stop != PYS_NONE) b += len;
        if (st > 0) { if (a < 0) a = 0; if (b > len) b = len; }
        else        { if (a >= len) a = len - 1; }
        // Materialise val as a byte buffer.
        unsigned char *vbuf;
        size_t nval;
        if (pys_is_byteseq(val) || pys_is_str(val)) {
            nval = PYS_PTR(val)->str.len;
            vbuf = (unsigned char *)PYS_PTR(val)->str.chars;
        } else {
            struct pys_iter it; pys_iter_init(c, &it, val);
            size_t cap = 16; nval = 0;
            unsigned char *buf = (unsigned char *)GC_malloc_atomic(cap);
            VALUE x;
            while (pys_iter_next(c, &it, &x)) {
                int64_t bv = pys_int_to_long(c, x);
                if (bv < 0 || bv > 255)
                    PYS_RAISE_EXC(c, c->EXC_ValueError, "byte must be in range(0, 256)");
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
            if (prefix) memcpy(out, PYS_PTR(seq)->str.chars, prefix);
            if (nval)   memcpy(out + prefix, vbuf, nval);
            if (suffix_len) memcpy(out + prefix + nval,
                                   PYS_PTR(seq)->str.chars + suffix_off, suffix_len);
            out[new_len] = '\0';
            PYS_PTR(seq)->str.chars = out;
            PYS_PTR(seq)->str.len = new_len;
            return;
        }
        // Stepped: require matching length.
        size_t target_n = 0;
        if (st > 0 && a < b) target_n = (size_t)((b - a + st - 1) / st);
        else if (st < 0 && a > b) target_n = (size_t)((a - b - st - 1) / -st);
        if (target_n != nval)
            PYS_RAISE_EXC(c, c->EXC_ValueError,
                         "attempt to assign bytes of size %zu to extended slice of size %zu",
                         nval, target_n);
        for (size_t i = 0; i < nval; i++)
            PYS_PTR(seq)->str.chars[a + (int64_t)i * st] = (char)vbuf[i];
        return;
    }
    if (!pys_is_list(seq))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "slice assignment requires a list");
    int64_t st = (step == PYS_NONE) ? 1 : pys_int_to_long(c, step);
    if (st == 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "slice step cannot be zero");
    int64_t len = (int64_t)PYS_PTR(seq)->list.len;
    int64_t a = (start == PYS_NONE) ? (st > 0 ? 0 : len - 1) : pys_int_to_long(c, start);
    int64_t b = (stop  == PYS_NONE) ? (st > 0 ? len : -1)    : pys_int_to_long(c, stop);
    if (a < 0) a += len;
    if (b < 0 && stop != PYS_NONE) b += len;
    if (st > 0) { if (a < 0) a = 0; if (b > len) b = len; }
    else        { if (a >= len) a = len - 1; }

    // Collect val's elements.  Validate the value is iterable BEFORE
    // we touch the list — `a[i:j] = 1` should raise TypeError without
    // partial-modifying `a` (CPython parity; list_tests.test_set_subscript
    // depends on this).
    VALUE *items = NULL;
    size_t nval = 0;
    if (pys_is_list(val) || pys_is_tuple(val)) {
        nval = PYS_PTR(val)->list.len;
        // `a[::-1] = a` aliases the RHS with the target — writes into
        // seq during the loop would clobber unread reads.  Copy when
        // val and seq share storage.  Tuples can never alias seq's list
        // storage, but check both cases uniformly.
        if (val == seq) {
            items = (VALUE *)GC_malloc(sizeof(VALUE) * (nval ? nval : 1));
            memcpy(items, PYS_PTR(val)->list.items, sizeof(VALUE) * nval);
        } else {
            items = PYS_PTR(val)->list.items;
        }
    } else {
        struct pys_iter it; pys_iter_init(c, &it, val);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return;
        // Reject obviously-not-iterable RHS (int, etc.) — pys_iter_init
        // for non-iterable sets state==RAISE, but for some types the
        // defensive defaults yield an empty iter that would silently
        // run the slice and shrink the list.
        if (!(pys_is_list(val) || pys_is_tuple(val) || pys_is_str(val) ||
              pys_is_byteseq(val) || pys_is_range(val) || pys_is_dict(val) ||
              pys_is_any_set(val) ||
              (PYS_IS_PTR(val) && (PYS_PTR(val)->type == PYS_T_ITER ||
                                   PYS_PTR(val)->type == PYS_T_GEN ||
                                   PYS_PTR(val)->type == PYS_T_INSTANCE)))) {
            PYS_RAISE_EXC(c, c->EXC_TypeError,
                "can only assign an iterable");
        }
        size_t cap = 16; nval = 0;
        items = (VALUE *)GC_malloc(sizeof(VALUE) * cap);
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
            if (nval == cap) { cap *= 2; items = (VALUE *)GC_realloc(items, sizeof(VALUE) * cap); }
            items[nval++] = x;
        }
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return;
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
        if (prefix) memcpy(out, PYS_PTR(seq)->list.items, sizeof(VALUE) * prefix);
        if (nval)   memcpy(out + prefix, items, sizeof(VALUE) * nval);
        if (suffix_len) memcpy(out + prefix + nval,
                               PYS_PTR(seq)->list.items + suffix_off,
                               sizeof(VALUE) * suffix_len);
        PYS_PTR(seq)->list.items = out;
        PYS_PTR(seq)->list.len = new_len;
        PYS_PTR(seq)->list.capa = cap;
        return;
    }

    // step != 1: requires matching length, OR nval == 0 (deletion via `del L[::s]`).
    // Use overflow-safe formula: ((diff - 1) / |st|) + 1.
    size_t target_n = 0;
    if (st > 0 && a < b) target_n = (size_t)(((b - a) - 1) / st + 1);
    else if (st < 0 && a > b) target_n = (size_t)(((a - b) - 1) / (-st) + 1);
    if (nval == 0 && target_n > 0) {
        // Delete the addressed indices.
        bool *del = (bool *)GC_malloc_atomic(len);
        for (size_t i = 0; i < (size_t)len; i++) del[i] = false;
        for (size_t i = 0; i < target_n; i++) del[a + (int64_t)i * st] = true;
        size_t w = 0;
        VALUE *itp = PYS_PTR(seq)->list.items;
        for (size_t r = 0; r < (size_t)len; r++)
            if (!del[r]) itp[w++] = itp[r];
        PYS_PTR(seq)->list.len = w;
        return;
    }
    if (target_n != nval)
        PYS_RAISE_EXC(c, c->EXC_ValueError,
                     "slice assignment length mismatch (%zu vs %zu)", target_n, nval);
    for (size_t i = 0; i < nval; i++)
        PYS_PTR(seq)->list.items[a + (int64_t)i * st] = items[i];
}

// UTF-8 helpers.  Pystro strings are UTF-8 byte arrays internally, but
// CPython-style str semantics expose codepoint indices.
//
// Walk a UTF-8 string and count codepoints (number of leading bytes).
size_t
pys_str_cp_count(const char *s, size_t bytelen)
{
    size_t cps = 0;
    for (size_t i = 0; i < bytelen; ) {
        unsigned char b = (unsigned char)s[i];
        int step;
        if (b < 0x80)               step = 1;
        else if ((b & 0xE0) == 0xC0) step = 2;
        else if ((b & 0xF0) == 0xE0) step = 3;
        else if ((b & 0xF8) == 0xF0) step = 4;
        else                          step = 1;
        i += step;
        cps++;
    }
    return cps;
}

// Convert codepoint index to byte offset.  Negative indices count from
// the end.  cp_idx may equal cp_count (slice end).
size_t
pys_str_cp_to_byte(const char *s, size_t bytelen, int64_t cp_idx)
{
    if (cp_idx <= 0) return 0;
    size_t i = 0;
    int64_t cps = 0;
    while (i < bytelen && cps < cp_idx) {
        unsigned char b = (unsigned char)s[i];
        int step;
        if (b < 0x80)               step = 1;
        else if ((b & 0xE0) == 0xC0) step = 2;
        else if ((b & 0xF0) == 0xE0) step = 3;
        else if ((b & 0xF8) == 0xF0) step = 4;
        else                          step = 1;
        i += step;
        cps++;
    }
    return i;
}

// Inverse: byte offset → codepoint index.
size_t
pys_str_byte_to_cp(const char *s, size_t byte_off)
{
    return pys_str_cp_count(s, byte_off);
}

size_t
pys_seq_len(CTX *c, VALUE v)
{
    if (pys_is_str(v))
        return pys_str_cp_count(PYS_PTR(v)->str.chars, PYS_PTR(v)->str.len);
    if (pys_is_byteseq(v)) return PYS_PTR(v)->str.len;
    if (PYS_IS_PTR(v) && PYS_PTR(v)->type == PYS_T_MEMVIEW) return PYS_PTR(v)->memview.len;
    if (pys_is_list(v) || pys_is_tuple(v)) return PYS_PTR(v)->list.len;
    if (pys_is_dict(v) || pys_is_any_set(v))  return PYS_PTR(v)->dict->used;
    if (pys_is_range(v)) {
        struct pysobj *o = PYS_PTR(v);
        if (o->range.step > 0) {
            if (o->range.start >= o->range.stop) return 0;
            return (size_t)((o->range.stop - o->range.start + o->range.step - 1) / o->range.step);
        } else {
            if (o->range.start <= o->range.stop) return 0;
            return (size_t)((o->range.start - o->range.stop + (-o->range.step) - 1) / (-o->range.step));
        }
    }
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), PYS_INTERN_len);
        if (m != PYS_NONE) {
            VALUE av[1] = { v };
            VALUE r = pys_apply(c, m, 1, av);
            if (PYS_IS_FIXNUM(r)) return (size_t)PYS_FIXVAL(r);
        }
        // Fall back to primary value (built-in subclass instance).
        if (PYS_PTR(v)->inst.primary) return pys_seq_len(c, PYS_PTR(v)->inst.primary);
    }
    // Class object: look up __len__ on its __metaclass__ (matches
    // metaclass-driven `len(SomeEnum)` etc.).
    if (pys_is_class(v)) {
        VALUE meta = pys_class_lookup_method(v, PYS_INTERN_metaclass);
        if (meta != PYS_NONE && pys_is_class(meta)) {
            VALUE m = pys_class_lookup_method(meta, PYS_INTERN_len);
            if (m != PYS_NONE) {
                VALUE av[1] = { v };
                VALUE r = pys_apply(c, m, 1, av);
                if (PYS_IS_FIXNUM(r)) return (size_t)PYS_FIXVAL(r);
            }
        }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "object has no len()");
}

bool
pys_contains(CTX *c, VALUE container, VALUE v)
{
    if (pys_is_list(container) || pys_is_tuple(container)) {
        // Re-bound on each step: __eq__ may have shrunk the container
        // (bpo-39453: list/tuple.__contains__ holds strong refs in CPython).
        for (size_t i = 0; ; i++) {
            struct pysobj *o = PYS_PTR(container);
            if (i >= o->list.len) break;
            VALUE x = o->list.items[i];
            // Identity-equality short-circuit: handles `nan in [nan]`
            // (CPython semantics — same object equals itself even if
            // pys_eq returns False due to NaN).
            if (x == v) return true;
            bool eq = pys_eq_bool(c, x, v);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return false;
            if (eq) return true;
        }
        return false;
    }
    if (pys_is_dict(container) || pys_is_any_set(container)) {
        // CPython special case: `set() in {frozenset()}` works even
        // though set is unhashable.  set_contains catches the TypeError
        // and retries with a temporary frozenset.  Mirror that here.
        // SetSubclass instance keys also get unwrapped.
        VALUE u = pys_unwrap_primary(v);
        if (pys_is_set(u)) {
            VALUE fsk = pys_make_frozenset();
            struct pysdict *src = PYS_PTR(u)->dict;
            for (size_t i = 0; i < src->elen; i++)
                if (pydict_entry_live(src, i))
                    pys_dict_set(c, fsk, src->entries[i].key, PYS_NONE);
            return pys_dict_has(c, container, fsk);
        }
        return pys_dict_has(c, container, v);
    }
    if (pys_is_str(container)) {
        // CPython rejects non-str RHS with TypeError.
        if (!pys_is_str(v)) {
            PYS_RAISE_EXC(c, c->EXC_TypeError,
                "'in <string>' requires string as left operand");
            return false;
        }
        return memmem(PYS_PTR(container)->str.chars, PYS_PTR(container)->str.len,
                      PYS_PTR(v)->str.chars, PYS_PTR(v)->str.len) != NULL;
    }
    if (pys_is_byteseq(container)) {
        // bytes/bytearray: `b"a" in b"abc"` (substring) or `int in bytes`
        // (byte-value membership).
        if (pys_is_byteseq(v)) {
            return memmem(PYS_PTR(container)->str.chars, PYS_PTR(container)->str.len,
                          PYS_PTR(v)->str.chars, PYS_PTR(v)->str.len) != NULL;
        }
        if (pys_int_or_bool(v)) {
            int64_t b = pys_int_to_long(c, v);
            if (b < 0 || b > 255) return false;
            const char *s = PYS_PTR(container)->str.chars;
            size_t n = PYS_PTR(container)->str.len;
            for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] == (unsigned char)b) return true;
            return false;
        }
    }
    if (pys_is_range(container) && pys_int_or_bool(v)) {
        int64_t x = pys_int_to_long(c, v);
        struct pysobj *r = PYS_PTR(container);
        if (r->range.step > 0)
            return x >= r->range.start && x < r->range.stop &&
                   ((x - r->range.start) % r->range.step == 0);
        else
            return x <= r->range.start && x > r->range.stop &&
                   ((r->range.start - x) % (-r->range.step) == 0);
    }
    if (pys_is_class(container)) {
        VALUE meta = pys_class_lookup_method(container, PYS_INTERN_metaclass);
        if (meta != PYS_NONE && pys_is_class(meta)) {
            VALUE m = pys_class_lookup_method(meta, PYS_INTERN_contains);
            if (m != PYS_NONE) {
                VALUE av[2] = { container, v };
                VALUE r = pys_apply(c, m, 2, av);
                return pys_is_truthy(r);
            }
        }
    }
    if (pys_is_instance(container)) {
        VALUE cls = PYS_OBJ_VAL(PYS_PTR(container)->inst.cls);
        VALUE m = pys_class_lookup_method(cls, PYS_INTERN_contains);
        // CPython: `__contains__ = None` blocks the iter fallback.
        if (m != PYS_NONE) {
            VALUE av[2] = { container, v };
            VALUE r = pys_apply(c, m, 2, av);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return false;
            return pys_is_truthy(r);
        } else if (pys_class_has_method(cls, "__contains__")) {
            // The class explicitly defines __contains__ as None.
            extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
            VALUE av_t[1] = { container };
            VALUE tt = bi_type(c, 1, av_t);
            const char *tn = (pys_is_class(tt)) ? PYS_PTR(tt)->cls.name : "?";
            PYS_RAISE_EXC(c, c->EXC_TypeError, "argument of type '%s' is not iterable", tn);
            return false;
        }
        // Built-in subclass: forward to primary.
        if (PYS_PTR(container)->inst.primary)
            return pys_contains(c, PYS_PTR(container)->inst.primary, v);
        // Fall back: route through pys_iter_init (handles generators,
        // built-in iters, __getitem__ protocol, etc.).
        struct pys_iter it;
        pys_iter_init(c, &it, container);
        if (c->state != PYS_STATE_NORMAL) return false;
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
            if (c->state == PYS_STATE_RAISE) return false;
            if (x == v) return true;
            if (pys_eq_bool(c, x, v)) return true;
        }
        return false;
    }
    // Generic fallback: iterate via the iter protocol (covers generators,
    // dict/list/tuple iterators, user-iter classes already not caught above,
    // etc.).
    struct pys_iter it;
    pys_iter_init(c, &it, container);
    if (c->state != PYS_STATE_NORMAL) {
        extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
        VALUE av_t[1] = { container };
        VALUE tt = bi_type(c, 1, av_t);
        const char *tn = (pys_is_class(tt)) ? PYS_PTR(tt)->cls.name : "?";
        c->state = PYS_STATE_NORMAL;
        PYS_RAISE_EXC(c, c->EXC_TypeError,
                     "argument of type '%s' is not a container or iterable",
                     tn);
    }
    VALUE x;
    while (pys_iter_next(c, &it, &x)) {
        if (c->state == PYS_STATE_RAISE) return false;
        if (x == v) return true;
        if (pys_eq_bool(c, x, v)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Iter protocol.
// ---------------------------------------------------------------------------

void
pys_iter_init(CTX *c, struct pys_iter *it, VALUE iterable)
{
    // Defensive defaults: if a later path raises before setting kind /
    // end (e.g. non-iterable TypeError), an iter with kind=0 + end=0
    // returns false from pys_iter_next without dereferencing the
    // (possibly non-pointer) container.  Several call sites currently
    // forget to check c->state after pys_iter_init, so this prevents a
    // SEGV when they proceed into pys_iter_next.
    it->kind = 0;
    it->container = iterable;
    it->i = 0;
    it->end = 0;
    it->step = 1;
    if (pys_is_list(iterable) || pys_is_tuple(iterable)) {
        it->kind = 0;
        it->end = (int64_t)PYS_PTR(iterable)->list.len;
        return;
    }
    if (pys_is_str(iterable)) {
        it->kind = 1;
        it->end = (int64_t)PYS_PTR(iterable)->str.len;
        return;
    }
    if (pys_is_byteseq(iterable)) {
        it->kind = 6;       // bytes: yield int 0..255
        it->end = (int64_t)PYS_PTR(iterable)->str.len;
        return;
    }
    if (pys_is_range(iterable)) {
        it->kind = 2;
        it->i    = PYS_PTR(iterable)->range.start;
        it->end  = PYS_PTR(iterable)->range.stop;
        it->step = PYS_PTR(iterable)->range.step;
        return;
    }
    if (pys_is_dict(iterable) || pys_is_any_set(iterable)) {
        it->kind = 3;
        it->end = (int64_t)PYS_PTR(iterable)->dict->elen;
        // CPython parity:
        //   - dict: snapshot `version` (every mutation bumps); del+set
        //     during iteration must raise RuntimeError even when count
        //     is unchanged (test_mutating_iteration_delete).
        //   - set : snapshot `used` (live count only); a clear+refill
        //     back to the same count must NOT raise (test_iter_and_mutate,
        //     issue 24581).
        // High bit flags which mode iter_next should use.
        if (pys_is_dict(iterable))
            it->version_snapshot = PYS_PTR(iterable)->dict->version | (1ULL << 63);
        else
            it->version_snapshot = (uint64_t)PYS_PTR(iterable)->dict->used;
        return;
    }
    // Already-an-iterator (PYS_T_ITER created by `iter(seq)` builtin):
    // drive the wrapped state directly so `for x in it` advances the
    // ORIGINAL iterator (CPython semantics — `iter(it) is it`).  Copying
    // state here breaks `iter(a); for x in it: ...; next(it)` because
    // the for-loop only mutates the local copy.
    if (PYS_IS_PTR(iterable) && PYS_PTR(iterable)->type == PYS_T_ITER) {
        it->kind = 14;
        it->container = iterable;
        return;
    }
    if (pys_is_instance(iterable)) {
        VALUE cls = PYS_OBJ_VAL(PYS_PTR(iterable)->inst.cls);
        VALUE im = pys_class_lookup_method(cls, PYS_INTERN_iter);
        if (im != PYS_NONE) {
            VALUE av[1] = { iterable };
            VALUE iter_obj = pys_apply(c, im, 1, av);
            if (c->state != PYS_STATE_NORMAL) return;
            // If __iter__ returned a generator object, dispatch via the
            // generator path (kind 7) rather than the user-iterator
            // path (which expects __next__ on a class).
            if (PYS_IS_PTR(iter_obj) && PYS_PTR(iter_obj)->type == PYS_T_GEN) {
                it->kind = 7;
                it->container = iter_obj;
                return;
            }
            // If __iter__ returned a built-in iterator (PYS_T_ITER),
            // unwrap and use its state directly.
            if (PYS_IS_PTR(iter_obj) && PYS_PTR(iter_obj)->type == PYS_T_ITER) {
                *it = *PYS_PTR(iter_obj)->iter_state;
                return;
            }
            it->kind = 5;       // user iterator
            it->container = iter_obj;
            it->i = 0; it->end = 0; it->step = 0;
            // Resolve __next__ once so the hot loop doesn't re-scan.
            if (pys_is_instance(iter_obj)) {
                VALUE icls = PYS_OBJ_VAL(PYS_PTR(iter_obj)->inst.cls);
                it->next_m = pys_class_lookup_method(icls, PYS_INTERN_next);
            }
            return;
        }
        // Built-in subclass: iterate over primary.
        if (PYS_PTR(iterable)->inst.primary) {
            pys_iter_init(c, it, PYS_PTR(iterable)->inst.primary);
            return;
        }
        // Sequence protocol fallback: __getitem__ with integer indices.
        VALUE gm = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(iterable)->inst.cls), PYS_INTERN_getitem);
        if (gm != PYS_NONE) {
            it->kind = 13;          // __getitem__-based iterator
            it->container = iterable;
            it->i = 0; it->end = 0; it->step = 0;
            return;
        }
    }
    if (PYS_IS_PTR(iterable) && PYS_PTR(iterable)->type == PYS_T_GEN) {
        it->kind = 7;
        it->container = iterable;
        return;
    }
    if (PYS_IS_PTR(iterable) && PYS_PTR(iterable)->type == PYS_T_FILE) {
        it->kind = 12;
        it->container = iterable;
        return;
    }
    // Class with metaclass __iter__ — used for `for m in EnumClass:`.
    if (pys_is_class(iterable)) {
        VALUE meta = pys_class_lookup_method(iterable, PYS_INTERN_metaclass);
        if (meta != PYS_NONE && pys_is_class(meta)) {
            VALUE m = pys_class_lookup_method(meta, PYS_INTERN_iter);
            if (m != PYS_NONE) {
                VALUE av[1] = { iterable };
                VALUE iter_obj = pys_apply(c, m, 1, av);
                if (c->state != PYS_STATE_NORMAL) return;
                if (PYS_IS_PTR(iter_obj) && PYS_PTR(iter_obj)->type == PYS_T_ITER) {
                    *it = *PYS_PTR(iter_obj)->iter_state;
                    return;
                }
                it->kind = 5;
                it->container = iter_obj;
                it->i = 0; it->end = 0; it->step = 0;
                return;
            }
        }
    }
    {
        extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
        VALUE av_t[1] = { iterable };
        VALUE tt = bi_type(c, 1, av_t);
        const char *tn = (pys_is_class(tt)) ? PYS_PTR(tt)->cls.name : "?";
        PYS_RAISE_EXC(c, c->EXC_TypeError, "'%s' object is not iterable", tn);
    }
}

// User iterator (kind=5) hot path extracted into its own function so
// it has a small, dedicated stack frame instead of inheriting
// pys_iter_next's worst-case 440-byte frame for every case.  Called
// from pys_iter_next_inline (node.h) to bypass pys_iter_next's switch.
//
// no_stack_protector: -fstack-protector-strong triggers on any function
// that uses alloca (pys_apply inlines into us and alloca's the callee
// frame).  The canary read/write/check costs ~5 cycles per call which,
// at 15M iterations on for_range_pyrange, adds 7-8% to runtime.  This
// function never writes a stack array via untrusted indices, so the
// canary is defending against threats we don't have.
__attribute__((no_stack_protector))
bool
pys_iter_next_user(CTX *c, struct pys_iter *it, VALUE *out)
{
    VALUE iter_obj = it->container;
    VALUE nm = it->next_m;
    if (UNLIKELY(nm == 0 || nm == PYS_NONE)) {
        if (pys_is_instance(iter_obj)) {
            VALUE cls = PYS_OBJ_VAL(PYS_PTR(iter_obj)->inst.cls);
            nm = pys_class_lookup_method(cls, PYS_INTERN_next);
            if (nm == PYS_NONE)
                PYS_RAISE_EXC(c, c->EXC_TypeError, "iter object has no __next__");
            it->next_m = nm;
        } else {
            PYS_RAISE_EXC(c, c->EXC_TypeError, "iter object has no __next__");
        }
    }
    VALUE av[1] = { iter_obj };
    VALUE r = pys_apply(c, nm, 1, av);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) {
        if (pys_exc_matches(c, c->state_value, c->EXC_StopIteration)) {
            c->state = PYS_STATE_NORMAL;
            c->state_value = PYS_NONE;
        }
        return false;
    }
    *out = r;
    return true;
}

bool
pys_iter_next(CTX *c, struct pys_iter *it, VALUE *out)
{
    (void)c;
    switch (it->kind) {
      case 0: {
        // Always read the live length so list iterators see appends
        // performed after `iter(a)` was taken (CPython parity).  Once
        // the iter exhausts, mark it permanently sticky by setting
        // i = INT64_MAX so a subsequent append doesn't resurrect it
        // (test_exhausted_iterator's exhit must stay empty after
        // a.append(9) even though len now exceeds the consumed index).
        size_t live_len = PYS_PTR(it->container)->list.len;
        if ((uint64_t)it->i >= (uint64_t)live_len) {
            it->i = INT64_MAX;
            return false;
        }
        *out = PYS_PTR(it->container)->list.items[it->i++];
        return true;
      }
      case 1: {
        // String: it->i is a byte offset, it->end is the byte length.
        // Yield one codepoint per step.
        if (it->i >= it->end) return false;
        const char *s = PYS_PTR(it->container)->str.chars;
        unsigned char b = (unsigned char)s[it->i];
        int step_b;
        if (b < 0x80)               step_b = 1;
        else if ((b & 0xE0) == 0xC0) step_b = 2;
        else if ((b & 0xF0) == 0xE0) step_b = 3;
        else if ((b & 0xF8) == 0xF0) step_b = 4;
        else                          step_b = 1;
        *out = pys_make_str(s + it->i, (size_t)step_b);
        it->i += step_b;
        return true;
      }
      case 6:
        if (it->i >= it->end) return false;
        *out = PYS_FIX((unsigned char)PYS_PTR(it->container)->str.chars[it->i]);
        it->i++;
        return true;
      case 7: {
        extern VALUE pys_gen_next(CTX *c, VALUE g);
        VALUE r = pys_gen_next(c, it->container);
        if (c->state == PYS_STATE_RAISE) {
            VALUE exc = c->state_value;
            if (pys_exc_matches(c, exc, c->EXC_StopIteration)) {
                c->state = PYS_STATE_NORMAL;
                c->state_value = PYS_NONE;
                return false;
            }
            return false;
        }
        *out = r;
        return true;
      }
      case 2:
        if (it->step > 0 ? it->i >= it->end : it->i <= it->end) return false;
        *out = pys_make_int(it->i);
        it->i += it->step;
        return true;
      case 3: {
        struct pysdict *d = PYS_PTR(it->container)->dict;
        // High bit set ⇒ version snapshot (dict mode); else used count.
        bool use_version = (it->version_snapshot & (1ULL << 63)) != 0;
        uint64_t now = use_version
            ? (d->version | (1ULL << 63))
            : (uint64_t)d->used;
        if (UNLIKELY(now != it->version_snapshot)) {
            const char *kind = pys_is_any_set(it->container) ? "Set" : "dictionary";
            PYS_RAISE_EXC(c, c->EXC_RuntimeError,
                          "%s changed size during iteration", kind);
            return false;
        }
        while (it->i < it->end) {
            size_t i = (size_t)it->i++;
            if (pydict_entry_live(d, i)) {
                *out = d->entries[i].key;
                return true;
            }
        }
        return false;
      }
      case 5:
        return pys_iter_next_user(c, it, out);
      case 4: {
        // iter(callable, sentinel): call container() until result == sentinel.
        VALUE r = pys_apply(c, it->container, 0, NULL);
        if (c->state == PYS_STATE_RAISE) return false;
        if (pys_eq_bool(c, r, it->sentinel)) return false;
        *out = r;
        return true;
      }
      case 8: {
        // enumerate: yield (i, v).  inner[0] is the source.
        VALUE v;
        if (!pys_iter_next(c, &it->inner[0], &v)) return false;
        VALUE pair[2] = { PYS_FIX(it->i++), v };
        *out = pys_make_tuple(pair, 2);
        return true;
      }
      case 9: {
        // zip: yield tuple of one element from each inner.  Stops when
        // any inner is exhausted.  If strict (it->i != 0), raise if
        // others still produce.
        if (it->n_inner == 0) return false;  // zip() with no args is empty
        VALUE *vs = (VALUE *)alloca(sizeof(VALUE) * it->n_inner);
        for (int k = 0; k < it->n_inner; k++) {
            if (!pys_iter_next(c, &it->inner[k], &vs[k])) {
                if (c->state == PYS_STATE_RAISE) return false;
                if (it->i != 0) {
                    // Strict.  If k > 0, earlier iters already produced
                    // — they're "longer". If k == 0, check subsequent
                    // iters can produce one more — they're "longer".
                    if (k > 0) {
                        PYS_RAISE_EXC(c, c->EXC_ValueError,
                                     "zip() argument %d is shorter than argument %d",
                                     k + 1, k);
                        return false;
                    }
                    VALUE dummy;
                    for (int j = k + 1; j < it->n_inner; j++) {
                        if (pys_iter_next(c, &it->inner[j], &dummy)) {
                            PYS_RAISE_EXC(c, c->EXC_ValueError,
                                         "zip() argument %d is longer than argument %d",
                                         j + 1, k + 1);
                            return false;
                        }
                        if (c->state == PYS_STATE_RAISE) return false;
                    }
                }
                return false;
            }
            if (c->state == PYS_STATE_RAISE) return false;
        }
        *out = pys_make_tuple(vs, it->n_inner);
        return true;
      }
      case 10: {
        // map(fn, *iters): apply fn to one element from each inner.
        VALUE *vs = (VALUE *)alloca(sizeof(VALUE) * it->n_inner);
        for (int k = 0; k < it->n_inner; k++) {
            if (!pys_iter_next(c, &it->inner[k], &vs[k])) return false;
            if (c->state == PYS_STATE_RAISE) return false;
        }
        *out = pys_apply(c, it->container, it->n_inner, vs);
        return c->state == PYS_STATE_NORMAL;
      }
      case 11: {
        // filter(fn, it): yield v from inner where fn(v) is truthy.
        // fn==None means yield truthy v.
        for (;;) {
            VALUE v;
            if (!pys_iter_next(c, &it->inner[0], &v)) return false;
            if (c->state == PYS_STATE_RAISE) return false;
            VALUE keep;
            if (it->container == PYS_NONE) keep = v;
            else {
                VALUE av[1] = { v };
                keep = pys_apply(c, it->container, 1, av);
                if (c->state == PYS_STATE_RAISE) return false;
            }
            if (pys_is_truthy(keep)) {
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
        if (c->state == PYS_STATE_RAISE) return false;
        if (pys_is_str(line) && PYS_PTR(line)->str.len == 0) return false;
        *out = line;
        return true;
      }
      case 13: {
        // __getitem__ sequence protocol — catch IndexError /
        // StopIteration from the user-defined __getitem__ via state
        // check.
        VALUE gm = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(it->container)->inst.cls), PYS_INTERN_getitem);
        VALUE av[2] = { it->container, PYS_FIX(it->i) };
        VALUE r = pys_apply(c, gm, 2, av);
        if (c->state == PYS_STATE_RAISE) {
            if (pys_exc_matches(c, c->state_value, c->EXC_IndexError)
                || pys_exc_matches(c, c->state_value, c->EXC_StopIteration)) {
                c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
                return false;
            }
            return false;
        }
        it->i++;
        *out = r;
        return true;
      }
      case 14: {
        // Wrapped PYS_T_ITER: drive the underlying iter_state directly so
        // any `for x in it_value` mutates the original iterator's position.
        return pys_iter_next(c, PYS_PTR(it->container)->iter_state, out);
      }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Attribute access.  For instances, look up self.attrs first, then the
// class's method table (binding `self`).  For built-in types, look up
// in a per-type method registry.
// ---------------------------------------------------------------------------

struct type_method {
    const char *name; pys_builtin_fn fn;
    int min_argc, max_argc;
    int is_property;     // 1 → invoke immediately on attr access (no method binding)
};

// Forward decls.
static struct type_method str_methods[];
static struct type_method list_methods[];
static struct type_method dict_methods[];
static VALUE bi_dict_fromkeys(CTX *c, int argc, VALUE *argv);
static VALUE bi_set(CTX *c, int argc, VALUE *argv);
static VALUE
dm_fromkeys_bridge(CTX *c, int argc, VALUE *argv)
{
    return bi_dict_fromkeys(c, argc - 1, argv + 1);
}
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
    VALUE fn = pys_make_builtin(tm->name, tm->fn, tm->min_argc, tm->max_argc);
    return pys_make_bound(self, fn);
}

// Built-in dunder methods on container types.  Code like
// `frozenset(xs).__contains__` should yield a bound method.  We
// implement each dunder via a tiny shim builtin that delegates to the
// existing C-level operation (pys_contains, pys_seq_len, etc.).
static VALUE
bi_dunder_contains(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    return pys_contains(c, argv[0], argv[1]) ? PYS_TRUE : PYS_FALSE;
}
static VALUE
bi_dunder_len(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    return PYS_FIX((int64_t)pys_seq_len(c, argv[0]));
}
static VALUE
bi_dunder_iter(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    extern VALUE bi_iter(CTX *c, int argc, VALUE *argv);
    return bi_iter(c, 1, argv);
}
static VALUE
bi_dunder_getitem(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    return pys_list_get(c, argv[0], argv[1]);
}
static VALUE
bi_dunder_setitem(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    return pys_list_set(c, argv[0], argv[1], argv[2]);
}
static VALUE bi_pystro_del(CTX *c, int argc, VALUE *argv);
static VALUE
bi_dunder_delitem(CTX *c, int argc, VALUE *argv)
{
    return bi_pystro_del(c, argc, argv);
}
// __or__ for dict instances — returns NotImplemented for non-dict RHS
// so user-level fallback / TypeError surfaces correctly (test_merge_operator).
static VALUE
bi_dunder_dict_or(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_dict(argv[1])) {
        VALUE ni;
        if (pys_global_lookup(c, "NotImplemented", &ni)) return ni;
        return PYS_NONE;
    }
    return pys_bit_or(c, argv[0], argv[1]);
}

// dict.__ior__(self, other) — CPython parity: accepts any iterable that
// yields (key, value) pairs (`dict.update(other)` semantics), unlike
// __or__ which only accepts a dict.  Returns self.  Raises TypeError on
// None / non-iterable, ValueError if a pair has length != 2.
VALUE
bi_dunder_dict_ior(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE self = argv[0];
    VALUE other = argv[1];
    if (pys_is_dict(other)) {
        struct pysdict *src = PYS_PTR(other)->dict;
        for (size_t i = 0; i < src->elen; i++)
            if (pydict_entry_live(src, i))
                pys_dict_set(c, self, src->entries[i].key, src->entries[i].value);
        return self;
    }
    // Iterable of (key, value) pairs.  None / non-iterable → TypeError.
    struct pys_iter it; pys_iter_init(c, &it, other);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) {
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        if (!(pys_is_list(x) || pys_is_tuple(x)))
            PYS_RAISE_EXC(c, c->EXC_ValueError,
                "dictionary update sequence element is not a pair");
        if (PYS_PTR(x)->list.len != 2)
            PYS_RAISE_EXC(c, c->EXC_ValueError,
                "dictionary update sequence element has length %zu; 2 required",
                PYS_PTR(x)->list.len);
        pys_dict_set(c, self, PYS_PTR(x)->list.items[0], PYS_PTR(x)->list.items[1]);
    }
    return self;
}
// __next__ for iterators — exposes the iter's pys_iter_next as a method.
static VALUE
bi_dunder_next(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!(PYS_IS_PTR(argv[0]) && PYS_PTR(argv[0])->type == PYS_T_ITER)) {
        PYS_RAISE_EXC(c, c->EXC_TypeError, "not an iterator");
    }
    struct pys_iter *it = PYS_PTR(argv[0])->iter_state;
    VALUE out;
    bool ok = pys_iter_next(c, it, &out);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (!ok) PYS_RAISE_EXC(c, c->EXC_StopIteration, "");
    return out;
}
// __length_hint__ for iterators (CPython exposes this on list_iterator,
// set_iterator etc.).  Returns a non-negative int — exact when possible,
// else a lower bound; PEP 424 lets callers use it as a hint only.
static VALUE
bi_dunder_length_hint(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    if (PYS_IS_PTR(argv[0]) && PYS_PTR(argv[0])->type == PYS_T_ITER) {
        struct pys_iter *it = PYS_PTR(argv[0])->iter_state;
        switch (it->kind) {
          case 0: case 1: case 6: {  // list/tuple, str, bytes
            int64_t n = it->end - it->i;
            return pys_make_int(n > 0 ? n : 0);
          }
          case 2: {  // range
            int64_t s = it->step;
            int64_t n = (s > 0) ? ((it->end - it->i + s - 1) / s)
                                 : ((it->i - it->end + (-s) - 1) / (-s));
            return pys_make_int(n > 0 ? n : 0);
          }
          case 3: {  // dict/set: container live count, minus already-visited
            VALUE c2 = it->container;
            if (PYS_IS_PTR(c2) && PYS_PTR(c2)->dict) {
                struct pysdict *d = PYS_PTR(c2)->dict;
                int64_t n = (int64_t)d->used - it->i;
                return pys_make_int(n > 0 ? n : 0);
            }
            return PYS_FIX(0);
          }
          default:
            return PYS_FIX(0);
        }
    }
    return PYS_FIX(0);
}
// list.__iadd__(iter) — extend self and return self.  CPython's
// list.__iadd__ rejects non-iterables with TypeError (see test_iadd).
static VALUE
bi_dunder_iadd(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_list(argv[0])) {
        PYS_RAISE_EXC(c, c->EXC_TypeError, "__iadd__ requires a list");
    }
    struct pys_iter it; pys_iter_init(c, &it, argv[1]);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) {
        pys_list_append(c, argv[0], x);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    return argv[0];
}
static VALUE
bi_dunder_eq(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    return pys_eq_bool(c, argv[0], argv[1]) ? PYS_TRUE : PYS_FALSE;
}
static VALUE
bi_dunder_ne(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    return pys_eq_bool(c, argv[0], argv[1]) ? PYS_FALSE : PYS_TRUE;
}
static VALUE
bi_dunder_hash(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    return PYS_FIX((int64_t)pys_hash(c, argv[0]));
}
static VALUE
bi_dunder_repr(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)c;
    extern VALUE bi_repr(CTX *c, int argc, VALUE *argv);
    return bi_repr(c, 1, argv);
}
static VALUE
bi_dunder_str(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    return pys_to_str(c, argv[0]);
}
static VALUE
bi_dunder_bool(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)c;
    return pys_is_truthy(argv[0]) ? PYS_TRUE : PYS_FALSE;
}
static VALUE
bi_dunder_call(CTX *c, int argc, VALUE *argv)
{
    return pys_apply(c, argv[0], argc - 1, argv + 1);
}

// Default __reduce_ex__ for arbitrary objects: returns a tuple of
// (callable, args) such that callable(*args) reconstructs an
// equivalent instance.  copy.copy / pickle use this when a class has
// no explicit __reduce_ex__.  We use the simple form
//   (type(self), (list(self),))
// for sequence-like instances (which covers list/tuple/dict subclass
// patterns common in test_xml_dom_minicompat).
static VALUE
bi_dunder_reduce_ex(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE self = argv[0];
    extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
    VALUE av_t[1] = { self };
    VALUE cls = bi_type(c, 1, av_t);
    // For built-in subclass instances, dump the primary value as the
    // ctor arg.  E.g. NodeList(list) → (NodeList, (list_of_nodes,)).
    if (pys_is_instance(self) && PYS_PTR(self)->inst.primary) {
        VALUE av_arg = PYS_PTR(self)->inst.primary;
        VALUE av_args = pys_make_tuple(&av_arg, 1);
        VALUE av_pair[2] = { cls, av_args };
        return pys_make_tuple(av_pair, 2);
    }
    // For plain instances, create an empty new of cls then assign __dict__.
    VALUE av_args = pys_make_tuple(NULL, 0);
    VALUE av_pair[2] = { cls, av_args };
    return pys_make_tuple(av_pair, 2);
}

static VALUE
bi_dunder_reduce(CTX *c, int argc, VALUE *argv)
{
    VALUE av[2] = { argv[0], PYS_FIX(2) };
    return bi_dunder_reduce_ex(c, 2, av);
}

static VALUE
bi_dunder_sizeof(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    return PYS_FIX(0);
}

static VALUE
bi_dunder_class_getitem(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    return argv[0];     // Foo[x] → Foo (no parameterisation enforced)
}

static VALUE bi_dir(CTX *c, int argc, VALUE *argv);
static VALUE
bi_dunder_dir(CTX *c, int argc, VALUE *argv)
{
    return bi_dir(c, argc, argv);
}

static VALUE
bi_dunder_init_subclass(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    return PYS_NONE;
}

static VALUE
bi_dunder_subclasshook(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    // Look up the global `NotImplemented` singleton.
    VALUE ni;
    if (pys_global_lookup(c, "NotImplemented", &ni)) return ni;
    return PYS_NONE;
}

static VALUE
bi_dunder_format(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (pys_is_str(argv[1]) && PYS_PTR(argv[1])->str.len == 0)
        return pys_to_str(c, argv[0]);
    // bi_format is later in this file; just str() with empty fmt for now.
    return pys_to_str(c, argv[0]);
}

static VALUE
bi_dunder_getattribute(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[1]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "attribute name must be string");
    return pys_getattr(c, argv[0], PYS_PTR(argv[1])->str.chars);
}

VALUE
pys_dunder_bound(CTX *c, VALUE recv, const char *name)
{
    (void)c;
    bool is_container =
        pys_is_list(recv) || pys_is_tuple(recv) || pys_is_dict(recv) ||
        pys_is_set(recv) || pys_is_frozenset(recv) || pys_is_str(recv) ||
        pys_is_byteseq(recv) || pys_is_range(recv);
    bool is_iter_obj = PYS_IS_PTR(recv) && PYS_PTR(recv)->type == PYS_T_ITER;
    bool is_sized = is_container;
    bool is_subscriptable = pys_is_list(recv) || pys_is_tuple(recv) ||
        pys_is_dict(recv) || pys_is_str(recv) || pys_is_byteseq(recv) ||
        pys_is_range(recv);
    bool is_iterable = is_container || is_iter_obj;
    bool is_assignable = pys_is_list(recv) || pys_is_dict(recv) ||
        (PYS_IS_PTR(recv) && PYS_PTR(recv)->type == PYS_T_BYTEARRAY);
    bool is_callable = pys_is_func(recv) || pys_is_builtin(recv) ||
        pys_is_bound(recv) || pys_is_class(recv);

    static const struct {
        const char *name;
        VALUE (*fn)(CTX *, int, VALUE *);
        int min_argc, max_argc;
    } shims[] = {
        { "__contains__", bi_dunder_contains, 2, 2 },
        { "__len__",      bi_dunder_len,      1, 1 },
        { "__iter__",     bi_dunder_iter,     1, 1 },
        { "__getitem__",  bi_dunder_getitem,  2, 2 },
        { "__setitem__",  bi_dunder_setitem,  3, 3 },
        { "__delitem__",  bi_dunder_delitem,  2, 2 },
        { "__iadd__",     bi_dunder_iadd,     2, 2 },
        { "__length_hint__", bi_dunder_length_hint, 1, 1 },
        { "__next__",     bi_dunder_next,     1, 1 },
        { "__or__",       bi_dunder_dict_or,  2, 2 },
        { "__ior__",      bi_dunder_dict_ior, 2, 2 },
        { "__eq__",       bi_dunder_eq,       2, 2 },
        { "__ne__",       bi_dunder_ne,       2, 2 },
        { "__hash__",     bi_dunder_hash,     1, 1 },
        { "__repr__",     bi_dunder_repr,     1, 1 },
        { "__str__",      bi_dunder_str,      1, 1 },
        { "__bool__",     bi_dunder_bool,     1, 1 },
        { "__call__",     bi_dunder_call,     1, -1 },
        { "__reduce_ex__",    bi_dunder_reduce_ex, 2, 2 },
        { "__reduce__",       bi_dunder_reduce,    1, 1 },
        { "__sizeof__",       bi_dunder_sizeof,    1, 1 },
        { "__class_getitem__",bi_dunder_class_getitem, 2, 2 },
        { "__dir__",          bi_dunder_dir,       1, 1 },
        { "__init_subclass__",bi_dunder_init_subclass, 1, -1 },
        { "__subclasshook__", bi_dunder_subclasshook, 2, 2 },
        { "__format__",       bi_dunder_format,    2, 2 },
        { "__getattribute__", bi_dunder_getattribute, 2, 2 },
    };
    int idx = -1;
    for (size_t i = 0; i < (int)(sizeof(shims)/sizeof(shims[0])); i++) {
        if (strcmp(shims[i].name, name) == 0) { idx = (int)i; break; }
    }
    if (idx < 0) return PYS_NONE;
    // Type-eligibility filter — return PYS_NONE if the operation
    // doesn't apply, so hasattr() returns False as it should.
    bool ok = true;
    switch (idx) {
      case 0: ok = is_container; break;                 // __contains__
      case 1: ok = is_sized; break;                     // __len__
      case 2: ok = is_iterable; break;                  // __iter__
      case 3: ok = is_subscriptable; break;             // __getitem__
      case 4: ok = is_assignable; break;                // __setitem__
      case 5: ok = is_assignable; break;                // __delitem__
      case 6: ok = pys_is_list(recv); break;            // __iadd__ (list-only)
      case 7: ok = is_iter_obj; break;                  // __length_hint__
      case 8: ok = is_iter_obj; break;                  // __next__
      case 9: ok = pys_is_dict(recv); break;            // __or__ (dict-only shim)
      case 10: case 11: ok = true; break;               // __eq__/__ne__ — universal
      case 12: ok = true; break;                        // __hash__ — universal
      case 13: case 14: ok = true; break;               // __repr__/__str__ — universal
      case 15: ok = true; break;                        // __bool__ — universal
      case 16: ok = is_callable; break;                 // __call__
      case 17: case 18: ok = true; break;               // __reduce_ex__/__reduce__
      case 19: ok = true; break;                        // __sizeof__
      case 20: ok = pys_is_class(recv); break;          // __class_getitem__
      case 21: ok = true; break;                        // __dir__
      case 22: ok = pys_is_class(recv); break;          // __init_subclass__
      case 23: ok = pys_is_class(recv); break;          // __subclasshook__
      case 24: ok = true; break;                        // __format__
      case 25: ok = true; break;                        // __getattribute__
    }
    if (!ok) return PYS_NONE;
    VALUE fn = pys_make_builtin(name, shims[idx].fn,
                               shims[idx].min_argc, shims[idx].max_argc);
    // When recv is a class (`int.__hash__` style), return the unbound
    // function that takes self as first arg — CPython lets test code
    // do `int.__hash__(5)` to bypass instance __getattribute__.
    if (pys_is_class(recv)) return fn;
    return pys_make_bound(recv, fn);
}

VALUE
pys_builtin_method(CTX *c, VALUE recv, const char *name)
{
    struct type_method *tbl;
    if (pys_is_str(recv))      tbl = str_methods;
    else if (pys_is_list(recv)) tbl = list_methods;
    else if (pys_is_tuple(recv)) tbl = tuple_methods;
    else if (pys_is_range(recv)) tbl = range_methods;
    else if (pys_is_dict(recv)) tbl = dict_methods;
    else if (pys_is_set(recv))  tbl = set_methods;
    else if (pys_is_frozenset(recv)) tbl = frozenset_methods;
    else if (PYS_IS_PTR(recv) && PYS_PTR(recv)->type == PYS_T_GEN) tbl = gen_methods;
    else if (pys_is_byteseq(recv)) tbl = bytes_methods;
    else if (pys_is_file(recv))    { extern struct type_method file_methods[]; tbl = file_methods; }
    else if (pys_int_or_bool(recv)) tbl = int_methods;
    else if (pys_is_float(recv))    tbl = float_methods;
    else if (pys_is_complex(recv))  tbl = complex_methods;
    else { (void)c; return PYS_NONE; }
    for (int i = 0; tbl[i].name; i++) {
        if (strcmp(tbl[i].name, name) == 0) {
            if (tbl[i].is_property) {
                VALUE av[1] = { recv };
                return tbl[i].fn(c, 1, av);
            }
            return make_builtin_bound(recv, &tbl[i]);
        }
    }
    return PYS_NONE;
}

// Cold-path method resolution used by inline-cached `node_method_*`.
// On a builtin-type method hit, stamps the cache with (type_tag, fn) so
// subsequent calls take the inline fast path (no bound-method alloc, no
// table strcmp).  On instance methods or class-level lookups, returns
// the resolved callable as-is and clears the cache so the type_tag
// check on the next call falls through correctly.
VALUE
pys_method_resolve(CTX *c, VALUE recv, const char *name, struct method_cache *cache)
{
    // Builtin type method?
    if (PYS_IS_PTR(recv)) {
        int tag = PYS_PTR(recv)->type;
        struct type_method *tbl = NULL;
        if (tag == PYS_T_STR)       tbl = str_methods;
        else if (tag == PYS_T_LIST) tbl = list_methods;
        else if (tag == PYS_T_DICT) tbl = dict_methods;
        else if (tag == PYS_T_SET)  tbl = set_methods;
        else if (tag == PYS_T_FROZENSET) tbl = frozenset_methods;
        else if (tag == PYS_T_GEN)  tbl = gen_methods;
        else if (tag == PYS_T_BYTES || tag == PYS_T_BYTEARRAY) tbl = bytes_methods;
        else if (tag == PYS_T_FILE) { extern struct type_method file_methods[]; tbl = file_methods; }
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
        // User-class instance: stamp a polymorphic IC slot so the hot
        // path can skip MRO walk + strcmp + bound-method alloc.  Only
        // safe for plain PYS_T_FUNC methods — wrapped descriptors
        // (staticmethod / classmethod / property) need different
        // binding semantics and stay on the slow path.  Likewise,
        // instance-dict entries shadow class methods, so skip caching
        // when the same name lives on the instance.
        if (tag == PYS_T_INSTANCE) {
            struct pysobj *o = PYS_PTR(recv);
            bool shadowed = false;
            if (o->inst.attrs) {
                for (size_t i = 0; i < o->inst.attrs->elen; i++) {
                    VALUE k = o->inst.attrs->entries[i].key;
                    if (k == 0 || k == DICT_DELETED_KEY) continue;
                    if (pys_is_str(k) && PYS_PTR(k)->str.len == strlen(name)
                        && memcmp(PYS_PTR(k)->str.chars, name, PYS_PTR(k)->str.len) == 0) {
                        shadowed = true;
                        break;
                    }
                }
            }
            if (!shadowed) {
                VALUE cls = PYS_OBJ_VAL(o->inst.cls);
                VALUE m = pys_class_lookup_method(cls, name);
                if (m != PYS_NONE && PYS_IS_PTR(m) && PYS_PTR(m)->type == PYS_T_FUNC) {
                    void *cls_ptr = (void *)o->inst.cls;
                    // Find an existing slot for this cls (rewrite if
                    // the bound function changed) or shift entries
                    // down and insert at slot 0 (most-recent).
                    int found = -1;
                    for (int i = 0; i < PYS_METHOD_PIC_WAYS; i++) {
                        if (cache->u_cls[i] == cls_ptr) { found = i; break; }
                    }
                    if (found < 0) {
                        for (int i = PYS_METHOD_PIC_WAYS - 1; i > 0; i--) {
                            cache->u_cls[i] = cache->u_cls[i - 1];
                            cache->u_fn [i] = cache->u_fn [i - 1];
                        }
                        cache->u_cls[0] = cls_ptr;
                        cache->u_fn [0] = (void *)(intptr_t)m;
                    } else {
                        cache->u_fn[found] = (void *)(intptr_t)m;
                    }
                    cache->type_tag = PYS_T_INSTANCE;
                    cache->fn = NULL;     // discriminator: not primary-builtin
                    // Slow path (this call) returns a bound to satisfy
                    // existing callers; from next call we hit the IC.
                    return pys_make_bound(recv, m);
                }
                // Primary-builtin case: instance is a built-in subclass
                // (e.g. `class OrderedCollection(list): pass`).  Look up
                // in the primary's method tbl and stamp cache so the
                // hot path can dispatch via primary as recv.  deltablue
                // had OrderedCollection.append/pop = 350K slow lookups.
                if (m == PYS_NONE && o->inst.primary) {
                    VALUE primary = o->inst.primary;
                    if (PYS_IS_PTR(primary)) {
                        int pt = PYS_PTR(primary)->type;
                        struct type_method *ptbl = NULL;
                        if (pt == PYS_T_LIST)            ptbl = list_methods;
                        else if (pt == PYS_T_DICT)       ptbl = dict_methods;
                        else if (pt == PYS_T_SET)        ptbl = set_methods;
                        else if (pt == PYS_T_FROZENSET)  ptbl = frozenset_methods;
                        else if (pt == PYS_T_STR)        ptbl = str_methods;
                        else if (pt == PYS_T_BYTES || pt == PYS_T_BYTEARRAY) ptbl = bytes_methods;
                        if (ptbl) {
                            for (int i = 0; ptbl[i].name; i++) {
                                if (strcmp(ptbl[i].name, name) == 0) {
                                    cache->type_tag = PYS_T_INSTANCE;
                                    cache->fn = (void *)ptbl[i].fn;   // discriminator
                                    cache->u_cls[0] = (void *)o->inst.cls;
                                    return make_builtin_bound(primary, &ptbl[i]);
                                }
                            }
                        }
                    }
                }
            }
        }
        // Class method on the class itself (`Cls.cm(...)`): for
        // @classmethod-decorated methods, cache the wrapped function so
        // node_method_N's fast path can dispatch with cls prepended,
        // skipping pys_class_lookup_method + bound-method alloc on every
        // call.  deltablue's `Strength.weakest_of` / `cls.weaker` chain
        // hammered pys_getattr → pys_class_lookup_method otherwise.
        if (tag == PYS_T_CLASS) {
            VALUE m = pys_class_lookup_method(recv, name);
            if (m != PYS_NONE && PYS_IS_PTR(m) && PYS_PTR(m)->type == PYS_T_CLASSMETHOD) {
                VALUE wrapped = PYS_PTR(m)->wrap.wrapped;
                if (PYS_IS_PTR(wrapped) && PYS_PTR(wrapped)->type == PYS_T_FUNC) {
                    void *cls_ptr = (void *)PYS_PTR(recv);
                    int found = -1;
                    for (int i = 0; i < PYS_METHOD_PIC_WAYS; i++) {
                        if (cache->u_cls[i] == cls_ptr) { found = i; break; }
                    }
                    if (found < 0) {
                        for (int i = PYS_METHOD_PIC_WAYS - 1; i > 0; i--) {
                            cache->u_cls[i] = cache->u_cls[i - 1];
                            cache->u_fn [i] = cache->u_fn [i - 1];
                        }
                        cache->u_cls[0] = cls_ptr;
                        cache->u_fn [0] = (void *)(intptr_t)wrapped;
                    } else {
                        cache->u_fn[found] = (void *)(intptr_t)wrapped;
                    }
                    cache->type_tag = PYS_T_CLASS;
                    cache->fn = NULL;
                    // Slow path's own caller (pys_apply_slow with the
                    // bound method) handles this call's dispatch; the
                    // hot path picks up the IC from next call.
                    return pys_make_bound(recv, wrapped);
                }
            }
        }
        // Module method (e.g., `math.sqrt(x)`).  No self prepend.
        // Cache (module_ptr, resolved_method).  raytrace's per-pixel
        // math.sqrt was doing module-globals strcmp loop per call.
        if (tag == PYS_T_MODULE) {
            VALUE m = pys_getattr(c, recv, name);
            if (c->state == PYS_STATE_NORMAL && m != PYS_NONE && PYS_IS_PTR(m)) {
                int mt = PYS_PTR(m)->type;
                if (mt == PYS_T_FUNC || mt == PYS_T_BUILTIN) {
                    void *mod_ptr = (void *)PYS_PTR(recv);
                    int found = -1;
                    for (int i = 0; i < PYS_METHOD_PIC_WAYS; i++) {
                        if (cache->u_cls[i] == mod_ptr) { found = i; break; }
                    }
                    if (found < 0) {
                        for (int i = PYS_METHOD_PIC_WAYS - 1; i > 0; i--) {
                            cache->u_cls[i] = cache->u_cls[i - 1];
                            cache->u_fn [i] = cache->u_fn [i - 1];
                        }
                        cache->u_cls[0] = mod_ptr;
                        cache->u_fn [0] = (void *)(intptr_t)m;
                    } else {
                        cache->u_fn[found] = (void *)(intptr_t)m;
                    }
                    cache->type_tag = PYS_T_MODULE;
                    return m;       // caller invokes with [args] (no self)
                }
            }
            return m;
        }
    }
    // Instance / class method (no inline-cache-able fast path).
    cache->type_tag = -1;
    cache->fn = NULL;
    return pys_getattr(c, recv, name);
}

// Like pys_getattr but returns 0 (not raised) when the attr is missing.
VALUE
pys_getattr_optional(CTX *c, VALUE v, const char *name)
{
    if (pys_is_instance(v)) {
        struct pysobj *o = PYS_PTR(v);
        if (o->inst.attrs) {
            VALUE key = pys_make_str(name, strlen(name));
            uint64_t h = pys_hash(c, key);
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
pys_getattr(CTX *c, VALUE v, const char *name)
{
    if (pys_is_super(v)) {
        // Walk MRO from start_cls (exclusive) for a method named `name`.
        extern VALUE pys_super_lookup(CTX *c, VALUE self, VALUE start_after_cls, const char *name);
        VALUE self = PYS_PTR(v)->super_.self;
        VALUE start = PYS_PTR(v)->super_.start_cls;
        VALUE m = pys_super_lookup(c, self, start, name);
        if (m == PYS_NONE)
            PYS_RAISE_EXC(c, c->EXC_AttributeError,
                         "'super' object has no attribute '%s'", name);
        // If `m` is already a bound method (built-in dispatched via
        // primary in super_lookup), don't re-bind.
        if (pys_is_bound(m)) return m;
        // CPython treats `__new__` as implicit-staticmethod: super().__new__
        // is NOT bound to self.  Caller passes cls explicitly:
        // `super().__new__(cls, name, bases, ns)`.  Without this skip,
        // pystro auto-prepended self → 5-arg call with garbage av[0],
        // breaking `super().__new__(mcls, ...)` in metaclass __new__.
        if (strcmp(name, "__new__") == 0) return m;
        return pys_make_bound(self, m);
    }
    if (PYS_IS_PTR(v) && PYS_PTR(v)->type == PYS_T_SLICE) {
        if (strcmp(name, "start") == 0) return PYS_PTR(v)->slice_.start;
        if (strcmp(name, "stop") == 0)  return PYS_PTR(v)->slice_.stop;
        if (strcmp(name, "step") == 0)  return PYS_PTR(v)->slice_.step;
        PYS_RAISE_EXC(c, c->EXC_AttributeError,
                     "'slice' object has no attribute '%s'", name);
    }
    if (pys_is_range(v)) {
        struct pysobj *o = PYS_PTR(v);
        if (strcmp(name, "start") == 0) return pys_make_int(o->range.start);
        if (strcmp(name, "stop") == 0)  return pys_make_int(o->range.stop);
        if (strcmp(name, "step") == 0)  return pys_make_int(o->range.step);
        // Fall through to method lookup
    }
    if (PYS_IS_PTR(v) && PYS_PTR(v)->type == PYS_T_PROPERTY) {
        if (strcmp(name, "fget") == 0) return PYS_PTR(v)->wrap.wrapped;
        if (strcmp(name, "fset") == 0) return PYS_PTR(v)->wrap.setter;
        if (strcmp(name, "fdel") == 0) return PYS_PTR(v)->wrap.deleter;
        if (strcmp(name, "__doc__") == 0) {
            // Forward to the getter's __doc__ (the @property-decorated function).
            VALUE fg = PYS_PTR(v)->wrap.wrapped;
            if (fg != PYS_NONE && pys_is_func(fg)) return pys_getattr(c, fg, "__doc__");
            return PYS_NONE;
        }
        if (strcmp(name, "setter") == 0) {
            VALUE fn = pys_make_builtin("setter", bi_property_setter_call, 2, 2);
            return pys_make_bound(v, fn);
        }
        if (strcmp(name, "deleter") == 0) {
            VALUE fn = pys_make_builtin("deleter", bi_property_deleter_call, 2, 2);
            return pys_make_bound(v, fn);
        }
        if (strcmp(name, "getter") == 0) {
            VALUE fn = pys_make_builtin("getter", bi_property_getter_call, 2, 2);
            return pys_make_bound(v, fn);
        }
        PYS_RAISE_EXC(c, c->EXC_AttributeError,
                     "'property' object has no attribute '%s'", name);
    }
    if (pys_is_complex(v)) {
        if (strcmp(name, "real") == 0) return pys_make_float(PYS_PTR(v)->cpx.re);
        if (strcmp(name, "imag") == 0) return pys_make_float(PYS_PTR(v)->cpx.im);
        if (strcmp(name, "conjugate") == 0) {
            // Return a bound method-like — but pystro has no easy way
            // to bind a builtin to a complex.  Inline as a closure:
            // for v0 just raise (caller can use `complex(c.real, -c.imag)`).
        }
    }
    if (pys_is_module(v)) {
        const char *mname = PYS_PTR(v)->module.name;
        if (strcmp(name, "__name__") == 0)
            return pys_make_str(mname, strlen(mname));
        if (strcmp(name, "__doc__") == 0) return PYS_NONE;
        if (strcmp(name, "__dict__") == 0) {
            // Snapshot module globals as a dict (live mutation not
            // supported — CPython exposes the underlying mapping but
            // pystro's globals layout is private).
            struct pysglobals *g = PYS_PTR(v)->module.globals;
            struct pysdict *d = pydict_new();
            struct pysobj *dwrap = pys_alloc(PYS_T_DICT);
            dwrap->dict = d;
            VALUE dval = PYS_OBJ_VAL(dwrap);
            for (size_t i = 0; i < g->size; i++) {
                if (g->entries[i].defined) {
                    VALUE k = pys_make_str(g->entries[i].name,
                                          strlen(g->entries[i].name));
                    pys_dict_set(c, dval, k, g->entries[i].value);
                }
            }
            return dval;
        }
        struct pysglobals *g = PYS_PTR(v)->module.globals;
        for (size_t i = 0; i < g->size; i++)
            if (strcmp(g->entries[i].name, name) == 0 && g->entries[i].defined)
                return g->entries[i].value;
        PYS_RAISE_EXC(c, c->EXC_AttributeError, "module '%s' has no attribute '%s'",
                     PYS_PTR(v)->module.name, name);
    }
    if (pys_is_instance(v)) {
        struct pysobj *o = PYS_PTR(v);
        if (strcmp(name, "__class__") == 0) return PYS_OBJ_VAL(o->inst.cls);
        if (strcmp(name, "__dict__") == 0) {
            // Return a live alias dict over inst.attrs so mutations
            // (`obj.__dict__[k] = v`) actually persist on the instance.
            if (!o->inst.attrs) o->inst.attrs = pydict_new();
            struct pysobj *d = pys_alloc(PYS_T_DICT);
            d->dict = o->inst.attrs;
            return PYS_OBJ_VAL(d);
        }
        // __getattribute__: user-overridable hook called before any
        // lookup.  Only fires when the user class defines it (we don't
        // install a default on object) — this prevents infinite
        // recursion when user code calls object.__getattribute__.
        if (!pys_skip_getattribute_hook
            && (strncmp(name, "__", 2) != 0 || strcmp(name, "__class__") == 0
                || strcmp(name, "__dict__") == 0)) {
            VALUE ga = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), PYS_INTERN_getattribute);
            // Skip the default object.__getattribute__ — only user
            // overrides should fire the hook.
            if (ga != PYS_NONE
                && !(PYS_IS_PTR(ga) && PYS_PTR(ga)->type == PYS_T_BUILTIN
                     && PYS_PTR(ga)->builtin.fn == bi_object_getattribute)) {
                VALUE av[2] = { v, pys_make_str(name, strlen(name)) };
                return pys_apply(c, ga, 2, av);
            }
        }
        if (o->inst.attrs) {
            VALUE key = pys_make_str(name, strlen(name));
            uint64_t h = pys_hash(c, key);
            int32_t eidx = pydict_find(c, o->inst.attrs, key, h);
            if (eidx >= 0) return o->inst.attrs->entries[eidx].value;
        }
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), name);
        // Disambiguate "not found" from "found, value is None": a class
        // body that does `x = None` should still resolve to None.
        bool m_present = (m != PYS_NONE) ||
                         pys_class_has_method(PYS_OBJ_VAL(o->inst.cls), name);
        if (m_present) {
            if (PYS_IS_PTR(m)) {
                int t = PYS_PTR(m)->type;
                if (t == PYS_T_STATICMETHOD) return PYS_PTR(m)->wrap.wrapped;
                if (t == PYS_T_CLASSMETHOD) return pys_make_bound(PYS_OBJ_VAL(o->inst.cls), PYS_PTR(m)->wrap.wrapped);
                if (t == PYS_T_PROPERTY) {
                    VALUE av[1] = { v };
                    return pys_apply(c, PYS_PTR(m)->wrap.wrapped, 1, av);
                }
                if (t == PYS_T_FUNC || t == PYS_T_BUILTIN)
                    return pys_make_bound(v, m);
            }
            // Otherwise fall through.
        }
        // No user-class method found.  If the instance's class has a
        // built-in base AND the instance has a primary value, look up
        // the method on the primary's type and bind it.
        if (!m_present && o->inst.primary) {
            extern VALUE pys_builtin_method(CTX *c, VALUE recv, const char *name);
            VALUE bm = pys_builtin_method(c, o->inst.primary, name);
            if (bm != PYS_NONE) {
                // pys_builtin_method returns a bound built-in method
                // bound to the primary; that's exactly what we want.
                return bm;
            }
        }
        if (m_present) {
            if (PYS_IS_PTR(m)) {
                int t = PYS_PTR(m)->type;
                // User-defined descriptor: if `m` is itself an instance
                // with a `__get__` method, call it as a descriptor.
                if (t == PYS_T_INSTANCE) {
                    VALUE get_m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(m)->inst.cls), PYS_INTERN_get);
                    if (get_m != PYS_NONE) {
                        VALUE av[3] = { m, v, PYS_OBJ_VAL(o->inst.cls) };
                        return pys_apply(c, get_m, 3, av);
                    }
                }
                return m;     // class data attribute (str / list / etc.)
            }
            return m;         // immediate (fixnum, None, True, False, flonum)
        }
        // The class attribute genuinely exists with value None — return it.
        if (m_present) return PYS_NONE;
        // __getattr__ fallback (only when the regular lookup misses).
        VALUE getattr_m = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), PYS_INTERN_getattr);
        if (getattr_m != PYS_NONE) {
            VALUE av[2] = { v, pys_make_str(name, strlen(name)) };
            return pys_apply(c, getattr_m, 2, av);
        }
        // Universal dunders inherited from object: `__str__` / `__repr__`
        // / `__hash__` / `__eq__` / `__ne__` / `__bool__` are defined on
        // every object even when the user class doesn't override.  Return
        // a builtin shim that produces the default behaviour.  CPython
        // tests like `obj.__str__()` rely on this.
        {
            extern VALUE pys_dunder_bound(CTX *c, VALUE recv, const char *name);
            VALUE bm = pys_dunder_bound(c, v, name);
            if (bm != PYS_NONE) return bm;
        }
        if (strcmp(name, "__doc__") == 0)    return PYS_NONE;
        if (strcmp(name, "__module__") == 0) return pys_make_str("__main__", 8);
        PYS_RAISE_EXC(c, c->EXC_AttributeError, "'%s' object has no attribute '%s'",
                     o->inst.cls->cls.name, name);
    }
    if (pys_is_class(v)) {
        struct pysclass *cd = &PYS_PTR(v)->cls;
        if (strcmp(name, "__class__") == 0) {
            // Class of a class is its metaclass — typically `type`.
            VALUE meta = pys_class_lookup_method(v, PYS_INTERN_metaclass);
            return (meta != PYS_NONE && pys_is_class(meta)) ? meta : c->TYPE_type;
        }
        if (strcmp(name, "__name__") == 0)
            return pys_make_str(cd->name, strlen(cd->name));
        if (strcmp(name, "__doc__") == 0) {
            VALUE d = pys_class_lookup_method(v, "__doc__");
            return d;        // PYS_NONE if absent
        }
        if (strcmp(name, "__module__") == 0) return pys_make_str("__main__", 8);
        if (strcmp(name, "__bases__") == 0) {
            // CPython: a class with no explicit base has __bases__ ==
            // (object,).  Pystro stores nbases=0 in that case but the
            // MRO contains object — surface (object,) here.
            if (cd->nbases == 0 && v != c->TYPE_object) {
                return pys_make_tuple(&c->TYPE_object, 1);
            }
            return pys_make_tuple(cd->bases, cd->nbases);
        }
        if (strcmp(name, "__mro__") == 0) {
            return pys_make_tuple(cd->mro, cd->nmro);
        }
        // `cls.mro()` — list form (CPython convention).  Returns a fresh
        // list each call (mutable; tests sometimes append to it).
        if (strcmp(name, "mro") == 0) {
            extern VALUE bi_class_mro(CTX *c, int argc, VALUE *argv);
            return pys_make_bound(v, pys_make_builtin("mro", bi_class_mro, 1, 1));
        }
        if (strcmp(name, "__dict__") == 0) {
            VALUE d = pys_make_dict();
            for (int i = 0; i < cd->nmethods; i++) {
                VALUE k = pys_make_str(cd->methods[i].name, strlen(cd->methods[i].name));
                pys_dict_set(c, d, k, cd->methods[i].value);
            }
            // Expose synthetic slot descriptors for `__mro__` /
            // `__bases__` / `__dict__` so introspection tools that do
            // `type.__dict__['__mro__'].__get__(obj)` (e.g. CPython's
            // inspect._static_getmro) succeed.
            extern VALUE pys_make_pyslot_descriptor(const char *attr);
            VALUE k1 = pys_make_str("__mro__", 7);
            pys_dict_set(c, d, k1, pys_make_pyslot_descriptor("__mro__"));
            VALUE k2 = pys_make_str("__bases__", 9);
            pys_dict_set(c, d, k2, pys_make_pyslot_descriptor("__bases__"));
            VALUE k3 = pys_make_str("__dict__", 8);
            pys_dict_set(c, d, k3, pys_make_pyslot_descriptor("__dict__"));
            return d;
        }
        if (strcmp(name, "__qualname__") == 0)
            return pys_make_str(cd->name, strlen(cd->name));
        if (pys_class_has_method(v, name)) {
            VALUE m = pys_class_lookup_method(v, name);
            if (PYS_IS_PTR(m)) {
                int t = PYS_PTR(m)->type;
                if (t == PYS_T_STATICMETHOD) return PYS_PTR(m)->wrap.wrapped;
                if (t == PYS_T_CLASSMETHOD)  return pys_make_bound(v, PYS_PTR(m)->wrap.wrapped);
            }
            // CPython treats `__init_subclass__` and `__class_getitem__`
            // as implicit classmethods even when defined as plain `def`s.
            // Bind the class so `Cls.__init_subclass__()` works without
            // requiring the caller to pass `cls`.
            if (PYS_IS_PTR(m) && (PYS_PTR(m)->type == PYS_T_FUNC ||
                                   PYS_PTR(m)->type == PYS_T_BUILTIN) &&
                (strcmp(name, "__init_subclass__") == 0 ||
                 strcmp(name, "__class_getitem__") == 0)) {
                return pys_make_bound(v, m);
            }
            // If m is an instance whose class defines __get__, invoke
            // __get__(None, owner) — descriptor protocol at class level.
            if (pys_is_instance(m)) {
                VALUE getm = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(m)->inst.cls), PYS_INTERN_get);
                if (getm != PYS_NONE) {
                    VALUE av[3] = { m, PYS_NONE, v };
                    return pys_apply(c, getm, 3, av);
                }
            }
            return m;
        }
        // Built-in type class: look up method as unbound function via
        // type_method tables (e.g. `str.lower`, `list.append`).
        {
            int btag = cd->builtin_tag;
            struct type_method *tbl = NULL;
            if (btag == PYS_T_STR)        tbl = str_methods;
            else if (btag == PYS_T_LIST)  tbl = list_methods;
            else if (btag == PYS_T_DICT)  tbl = dict_methods;
            else if (btag == PYS_T_SET)   tbl = set_methods;
            else if (btag == PYS_T_FROZENSET) tbl = frozenset_methods;
            else if (btag == PYS_T_TUPLE) tbl = tuple_methods;
            else if (btag == PYS_T_BYTES || btag == PYS_T_BYTEARRAY) tbl = bytes_methods;
            else if (btag == PYS_T_BIGNUM) tbl = int_methods;
            else if (btag == PYS_T_FLOAT) tbl = float_methods;
            else if (btag == PYS_T_COMPLEX) tbl = complex_methods;
            else if (btag == PYS_T_RANGE) tbl = range_methods;
            if (tbl) {
                for (int i = 0; tbl[i].name; i++) {
                    if (strcmp(tbl[i].name, name) == 0) {
                        return pys_make_builtin(tbl[i].name, tbl[i].fn,
                                               tbl[i].min_argc, tbl[i].max_argc);
                    }
                }
            }
        }
        // Fall through to the metaclass: class-attr lookup walks
        // __metaclass__ so SingletonMeta._instances is reachable as
        // S._instances.  For an *instance method* on the metaclass —
        // i.e. CPython's `def register(cls, subclass)` on ABCMeta —
        // accessing `MM.register` (where type(MM)=ABCMeta) should
        // return a bound method with self=MM, so calling
        // `MM.register(Foo)` passes both cls and subclass.
        VALUE meta_v = pys_class_lookup_method(v, PYS_INTERN_metaclass);
        if (meta_v != PYS_NONE && pys_is_class(meta_v)) {
            if (pys_class_has_method(meta_v, name)) {
                VALUE m = pys_class_lookup_method(meta_v, name);
                if (PYS_IS_PTR(m)) {
                    int t = PYS_PTR(m)->type;
                    if (t == PYS_T_STATICMETHOD) return PYS_PTR(m)->wrap.wrapped;
                    if (t == PYS_T_CLASSMETHOD)  return pys_make_bound(v, PYS_PTR(m)->wrap.wrapped);
                    if (t == PYS_T_FUNC)         return pys_make_bound(v, m);
                }
                return m;
            }
        }
        // Inherited-from-object dunders.  CPython's `class.__ne__` etc.
        // are implicit on every type even when not defined locally; user
        // code (including CPython _collections_abc) does
        // `MutableMapping.__ne__ != ...` at module init.  Return a
        // builtin shim that delegates to the standard semantics.
        {
            extern VALUE pys_dunder_bound(CTX *c, VALUE recv, const char *name);
            VALUE bm = pys_dunder_bound(c, v, name);
            if (bm != PYS_NONE) return bm;
        }
        if (strcmp(name, "__doc__") == 0)         return PYS_NONE;
        if (strcmp(name, "__module__") == 0)      return pys_make_str("__main__", 8);
        PYS_RAISE_EXC(c, c->EXC_AttributeError, "type object '%s' has no attribute '%s'",
                     cd->name, name);
    }
    if (pys_is_func(v)) {
        struct pysobj *o = PYS_PTR(v);
        if (strcmp(name, "__name__") == 0 || strcmp(name, "__qualname__") == 0) {
            // If user set __name__ via setattr (e.g. functools.wraps),
            // honour the override; otherwise fall back to the original
            // function name.
            if (o->func.attrs) {
                VALUE k = pys_make_str(name, strlen(name));
                int32_t e = pydict_find(c, o->func.attrs, k, pys_hash(c, k));
                if (e >= 0) return o->func.attrs->entries[e].value;
            }
            const char *n = o->func.name ? o->func.name : "<func>";
            return pys_make_str(n, strlen(n));
        }
        if (strcmp(name, "__doc__") == 0) {
            if (o->func.attrs) {
                VALUE k = pys_make_str("__doc__", 7);
                int32_t e = pydict_find(c, o->func.attrs, k, pys_hash(c, k));
                if (e >= 0) return o->func.attrs->entries[e].value;
            }
            return PYS_NONE;
        }
        if (strcmp(name, "__module__") == 0) return pys_make_str("__main__", 8);
        if (strcmp(name, "__annotations__") == 0) {
            // Read from func.attrs if user set it (parser emits
            // `f.__annotations__ = {...}` after def for annotated funcs).
            if (o->func.attrs) {
                VALUE key = pys_make_str("__annotations__", 15);
                uint64_t h = pys_hash(c, key);
                int32_t e = pydict_find(c, o->func.attrs, key, h);
                if (e >= 0) return o->func.attrs->entries[e].value;
            }
            return pys_make_dict();
        }
        if (strcmp(name, "__defaults__") == 0) {
            // Tuple of trailing defaults for pos-or-kw params, or None.
            if (!o->func.defaults) return PYS_NONE;
            VALUE buf[32];
            int n = 0;
            for (int i = 0; i < o->func.n_pos_named && n < 32; i++) {
                VALUE d = o->func.defaults[i];
                if (d) buf[n++] = d;
            }
            if (n == 0) return PYS_NONE;
            return pys_make_tuple(buf, n);
        }
        if (strcmp(name, "__kwdefaults__") == 0) {
            if (!o->func.defaults) return pys_make_dict();
            VALUE r = pys_make_dict();
            for (int i = o->func.n_pos_named; i < o->func.nparams; i++) {
                VALUE d = o->func.defaults[i];
                if (d && o->func.param_names) {
                    pys_dict_set(c, r,
                                pys_make_str(o->func.param_names[i],
                                            strlen(o->func.param_names[i])), d);
                }
            }
            return r;
        }
        if (strcmp(name, "__code__") == 0) {
            // Return a minimal code-object-like instance carrying the
            // names CPython exposes most often: co_varnames, co_argcount,
            // co_posonlyargcount, co_kwonlyargcount, co_name, co_flags.
            // We build it from the function's stored param info.
            VALUE code = pys_make_instance(c->TYPE_object);
            int na = o->func.n_pos_named;
            int nva = o->func.has_varargs ? 1 : 0;
            int nkw = o->func.nparams - na - nva - (o->func.has_kwargs ? 1 : 0);
            VALUE *names = (VALUE *)alloca(sizeof(VALUE) * (o->func.nparams + 1));
            int nn = 0;
            if (o->func.param_names) {
                for (int i = 0; i < o->func.nparams; i++) {
                    if (o->func.param_names[i])
                        names[nn++] = pys_make_str(o->func.param_names[i],
                                                  strlen(o->func.param_names[i]));
                }
            }
            pys_setattr(c, code, "co_varnames", pys_make_tuple(names, nn));
            pys_setattr(c, code, "co_argcount", PYS_FIX(na));
            pys_setattr(c, code, "co_posonlyargcount", PYS_FIX(0));
            pys_setattr(c, code, "co_kwonlyargcount", PYS_FIX(nkw));
            pys_setattr(c, code, "co_name",
                       pys_make_str(o->func.name ? o->func.name : "<lambda>",
                                   o->func.name ? strlen(o->func.name) : 8));
            pys_setattr(c, code, "co_flags",
                       PYS_FIX((o->func.is_generator ? 0x20 : 0)
                              | (o->func.has_varargs ? 0x04 : 0)
                              | (o->func.has_kwargs  ? 0x08 : 0)));
            pys_setattr(c, code, "co_filename",
                       pys_make_str("<pystro>", 8));
            pys_setattr(c, code, "co_firstlineno", PYS_FIX(0));
            return code;
        }
        if (strcmp(name, "__globals__") == 0) return PYS_NONE;
        if (strcmp(name, "__closure__") == 0) {
            // CPython: nested function with free vars returns a tuple
            // of cell objects; module-level / non-capturing returns None.
            //
            // Pystro distinguishes by whether the function was defined
            // with a non-NULL env (which only happens for nested defs —
            // module-level def runs at c->env == NULL).  For nested
            // defs we synthesise a tuple of cell-class instances so
            // CPython's `types.CellType = type(_cell_factory())` works.
            // Cell contents snapshot the captured frame slot at lookup
            // time — no live binding back to the cell.  Adequate for
            // introspection.
            if (!o->func.env) return PYS_NONE;
            if (c->TYPE_cell == 0 || c->TYPE_cell == PYS_NONE) return PYS_NONE;
            int n = o->func.env->nslots;
            if (n <= 0) return PYS_NONE;
            if (n > 32) n = 32;
            VALUE *items = (VALUE *)alloca(sizeof(VALUE) * n);
            for (int i = 0; i < n; i++) {
                struct pysobj *cell = (struct pysobj *)GC_malloc(sizeof(struct pysobj));
                cell->type = PYS_T_INSTANCE;
                cell->inst.cls = PYS_PTR(c->TYPE_cell);
                cell->inst.attrs = pydict_new();
                cell->inst.primary = 0;
                VALUE k = pys_make_str("cell_contents", 13);
                pydict_set_h(c, cell->inst.attrs, k, pys_hash(c, k),
                            o->func.env->slots[i]);
                items[i] = PYS_OBJ_VAL(cell);
            }
            return pys_make_tuple(items, (size_t)n);
        }
        if (strcmp(name, "__dict__") == 0) {
            // Lazily allocate the function's attribute dict and expose
            // it as a real PYS_T_DICT — sharing storage so writes via
            // `f.__dict__["x"] = 1` are visible to subsequent `f.x`.
            if (!o->func.attrs) o->func.attrs = pydict_new();
            struct pysobj *d = pys_alloc(PYS_T_DICT);
            d->dict = o->func.attrs;
            return PYS_OBJ_VAL(d);
        }
        if (strcmp(name, "__class__") == 0) return c->TYPE_function;
        if (o->func.attrs) {
            VALUE key = pys_make_str(name, strlen(name));
            uint64_t h = pys_hash(c, key);
            int32_t eidx = pydict_find(c, o->func.attrs, key, h);
            if (eidx >= 0) return o->func.attrs->entries[eidx].value;
        }
        // Universal dunders (__hash__, __eq__, __repr__, __str__) — every
        // function object has these via inheritance from object.  CPython
        // tests like `inspect.isfunction(f) and hash(f.__hash__) ...` reach
        // for them; pystro previously raised AttributeError.
        {
            extern VALUE pys_dunder_bound(CTX *c, VALUE recv, const char *name);
            VALUE bm = pys_dunder_bound(c, v, name);
            if (bm != PYS_NONE) return bm;
        }
        PYS_RAISE_EXC(c, c->EXC_AttributeError, "function has no attribute '%s'", name);
    }
    if (pys_is_builtin(v)) {
        if (strcmp(name, "__name__") == 0
            || strcmp(name, "__qualname__") == 0) {
            const char *n = PYS_PTR(v)->builtin.name ? PYS_PTR(v)->builtin.name : "<builtin>";
            return pys_make_str(n, strlen(n));
        }
        if (strcmp(name, "__class__") == 0) return c->TYPE_builtin_function_or_method;
        if (strcmp(name, "__doc__") == 0) return PYS_NONE;
        if (strcmp(name, "__module__") == 0) return pys_make_str("builtins", 8);
        // `f.__get__` on a builtin descriptor — return the function
        // itself, so the caller's `f.__get__(obj)` invokes the
        // descriptor body with `obj` as its first argument.  Used by
        // CPython's `inspect._static_getmro = type.__dict__['__mro__'].__get__`.
        if (strcmp(name, "__get__") == 0) return v;
    }
    if (pys_is_bound(v)) {
        // Forward attribute lookup to the underlying func — covers
        // __doc__, __name__, etc. on bound methods.
        VALUE inner = PYS_PTR(v)->bound.func;
        if (strcmp(name, "__self__") == 0) return PYS_PTR(v)->bound.self;
        if (strcmp(name, "__func__") == 0) return inner;
        return pys_getattr(c, inner, name);
    }
    // classmethod / staticmethod / property descriptor objects expose
    // `__func__` (the wrapped callable) and `__wrapped__` (alias).
    // CPython's `inspect.unwrap()` and `functools.wraps` rely on these.
    if (PYS_IS_PTR(v)) {
        int t = PYS_PTR(v)->type;
        if (t == PYS_T_CLASSMETHOD || t == PYS_T_STATICMETHOD) {
            VALUE wrapped = PYS_PTR(v)->wrap.wrapped;
            if (strcmp(name, "__func__") == 0)     return wrapped;
            if (strcmp(name, "__wrapped__") == 0)  return wrapped;
            if (strcmp(name, "__class__") == 0)
                return t == PYS_T_CLASSMETHOD ? c->TYPE_classmethod
                                              : c->TYPE_staticmethod;
            // Forward most other attrs to the wrapped function (e.g.
            // __name__, __qualname__, __doc__, __dict__).
            if (PYS_IS_PTR(wrapped) && PYS_PTR(wrapped)->type == PYS_T_FUNC)
                return pys_getattr(c, wrapped, name);
        }
    }
    VALUE m = pys_builtin_method(c, v, name);
    if (m != PYS_NONE) return m;
    if (strcmp(name, "__class__") == 0) {
        extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
        VALUE av[1] = { v };
        return bi_type(c, 1, av);
    }
    // Built-in dunders that aren't in the per-type method tables:
    // expose `__contains__`, `__len__`, `__iter__`, `__getitem__`,
    // `__eq__`, `__ne__`, `__hash__`, `__repr__`, `__str__` as bound
    // builtin methods so code like `frozenset(xs).__contains__` works.
    {
        extern VALUE pys_dunder_bound(CTX *c, VALUE recv, const char *name);
        VALUE bm = pys_dunder_bound(c, v, name);
        if (bm != PYS_NONE) return bm;
    }
    extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
    VALUE av[1] = { v };
    VALUE t = bi_type(c, 1, av);
    // Common dunders any object should expose: `__doc__` / `__module__`
    // / `__class__`.  Real CPython has these as type-slot attributes;
    // pystro's per-type method tables don't carry them, so fall through
    // to a sensible default before raising.
    if (strcmp(name, "__doc__") == 0)         return PYS_NONE;
    if (strcmp(name, "__module__") == 0)      return pys_make_str("builtins", 8);
    if (strcmp(name, "__class__") == 0)       return t;
    const char *tname = "?";
    if (pys_is_class(t)) tname = PYS_PTR(t)->cls.name;
    PYS_RAISE_EXC(c, c->EXC_AttributeError, "'%s' object has no attribute '%s'", tname, name);
}

static __thread int pys_skip_setattr_hook = 0;

void
pys_setattr(CTX *c, VALUE v, const char *name, VALUE val)
{
    if (pys_is_instance(v)) {
        struct pysobj *o = PYS_PTR(v);
        // __setattr__ user override
        if (!pys_skip_setattr_hook) {
            VALUE sm = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), PYS_INTERN_setattr);
            if (sm != PYS_NONE
                && !(PYS_IS_PTR(sm) && PYS_PTR(sm)->type == PYS_T_BUILTIN
                     && PYS_PTR(sm)->builtin.fn == bi_object_setattr)) {
                pys_skip_setattr_hook++;
                VALUE av[3] = { v, pys_make_str(name, strlen(name)), val };
                pys_apply(c, sm, 3, av);
                pys_skip_setattr_hook--;
                return;
            }
        }
        // Data descriptor on the class (with __set__) intercepts.
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), name);
        if (m != PYS_NONE && PYS_IS_PTR(m)) {
            int t = PYS_PTR(m)->type;
            if (t == PYS_T_PROPERTY) {
                VALUE setter = PYS_PTR(m)->wrap.setter;
                if (setter == PYS_NONE)
                    PYS_RAISE_EXC(c, c->EXC_AttributeError,
                                 "property '%s' has no setter", name);
                VALUE av[2] = { v, val };
                pys_apply(c, setter, 2, av);
                return;
            }
            if (t == PYS_T_INSTANCE) {
                VALUE set_m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(m)->inst.cls), "__set__");
                if (set_m != PYS_NONE) {
                    VALUE av[3] = { m, v, val };
                    pys_apply(c, set_m, 3, av);
                    return;
                }
            }
        }
        // __slots__ enforcement.
        if (pys_class_has_slots_anywhere(PYS_OBJ_VAL(o->inst.cls))
            && !pys_class_slot_allowed(PYS_OBJ_VAL(o->inst.cls), name)) {
            PYS_RAISE_EXC(c, c->EXC_AttributeError,
                         "'%s' object has no attribute '%s'",
                         o->inst.cls->cls.name, name);
        }
        if (!o->inst.attrs) o->inst.attrs = pydict_new();
        VALUE key = pys_make_str(name, strlen(name));
        uint64_t h = pys_hash(c, key);
        pydict_set_h(c, o->inst.attrs, key, h, val);
        return;
    }
    if (pys_is_class(v)) {
        extern const char *intern_name(const char *s, size_t len);
        // Special-case __name__: update the C-level cls.name so
        // type() / repr / etc. see the new name.
        if (strcmp(name, "__name__") == 0 && pys_is_str(val)) {
            const char *nn = intern_name(PYS_PTR(val)->str.chars, PYS_PTR(val)->str.len);
            PYS_PTR(v)->cls.name = nn;
            return;
        }
        // Otherwise, store via method table.
        pys_class_add_method(c, v, intern_name(name, strlen(name)), val);
        SHARED_GLOBALS_SERIAL++;
        return;
    }
    if (pys_is_func(v)) {
        struct pysobj *o = PYS_PTR(v);
        if (!o->func.attrs) o->func.attrs = pydict_new();
        VALUE key = pys_make_str(name, strlen(name));
        uint64_t h = pys_hash(c, key);
        pydict_set_h(c, o->func.attrs, key, h, val);
        return;
    }
    if (pys_is_module(v)) {
        struct pysglobals *g = PYS_PTR(v)->module.globals;
        // Find or insert in module globals.
        for (size_t i = 0; i < g->size; i++) {
            if (strcmp(g->entries[i].name, name) == 0) {
                g->entries[i].value = val;
                g->entries[i].defined = true;
                return;
            }
        }
        // Append (uses the same growth rules as pys_global_define).
        struct pysglobals *saved = c->globals;
        c->globals = g;
        pys_global_define(c, name, val);
        c->globals = saved;
        return;
    }
    PYS_RAISE_EXC(c, c->EXC_AttributeError, "object does not support attribute assignment");
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
// non-NULL after pys_alloc.)
static VALUE
pys_apply_kw_func(CTX *c, VALUE fn, int argc, VALUE *argv,
                 int kwc, const char **kwnames, VALUE *kwvalues)
{
    struct pysobj *f = PYS_PTR(fn);
    int nparams = f->func.nparams;
    int n_pos_named = f->func.n_pos_named;
    bool has_va = f->func.has_varargs;
    bool has_kw = f->func.has_kwargs;
    int va_slot = has_va ? n_pos_named : -1;
    int kwonly_start = n_pos_named + (has_va ? 1 : 0);
    int n_kwonly = nparams - kwonly_start - (has_kw ? 1 : 0);
    int kw_slot = has_kw ? (kwonly_start + n_kwonly) : -1;

    struct pysframe *new_env = pys_new_frame(f->func.env, f->func.nlocals);
    new_env->slot_names = f->func.local_names;
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
        new_env->slots[va_slot] = pys_make_tuple(items, n_extra);
        filled[va_slot] = true;
    } else if (argc > n_pos_named) {
        PYS_RAISE_EXC(c, c->EXC_TypeError,
                     "%s() got %d positional arg(s), expected at most %d",
                     f->func.name ? f->func.name : "<anonymous>", argc, n_pos_named);
    }

    // **kwargs.
    if (has_kw) {
        new_env->slots[kw_slot] = pys_make_dict();
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
                PYS_RAISE_EXC(c, c->EXC_TypeError,
                             "%s() got multiple values for argument '%s'",
                             f->func.name ? f->func.name : "<anonymous>", kwnames[i]);
            }
            new_env->slots[slot] = kwvalues[i];
            filled[slot] = true;
        } else if (has_kw) {
            pys_dict_set(c, new_env->slots[kw_slot],
                        pys_make_str(kwnames[i], strlen(kwnames[i])), kwvalues[i]);
        } else {
            // Check if name matches a pos-only param: helpful diagnostic.
            if (f->func.param_names) {
                for (int j = 0; j < n_pos_only; j++) {
                    if (f->func.param_names[j] && strcmp(f->func.param_names[j], kwnames[i]) == 0) {
                        PYS_RAISE_EXC(c, c->EXC_TypeError,
                            "%s() got some positional-only arguments passed as keyword arguments: '%s'",
                            f->func.name ? f->func.name : "<anonymous>", kwnames[i]);
                    }
                }
            }
            PYS_RAISE_EXC(c, c->EXC_TypeError,
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
            PYS_RAISE_EXC(c, c->EXC_TypeError,
                         "%s() missing required argument '%s'",
                         f->func.name ? f->func.name : "<anonymous>",
                         (f->func.param_names && f->func.param_names[i])
                             ? f->func.param_names[i] : "?");
        }
        new_env->slots[i] = d;
    }

    struct pysframe *saved = c->env;
    VALUE saved_mc = c->method_class;
    struct pysglobals *saved_g = c->globals;
    c->env = new_env;
    c->method_class = f->func.defining_class;
    if (f->func.fglobals) c->globals = f->func.fglobals;
    EVAL(c, f->func.body);
    c->env = saved;
    c->method_class = saved_mc;
    c->globals = saved_g;
    if (c->state == PYS_STATE_RETURN) {
        VALUE r = c->state_value;
        c->state = PYS_STATE_NORMAL;
        c->state_value = PYS_NONE;
        return r;
    }
    if (c->state == PYS_STATE_RAISE) return 0;
    return PYS_NONE;
}

// Public entry for kwarg / *args expansion.  Handles bound methods
// (prepend self) and class calls (instantiate then call __init__);
// otherwise dispatches to pys_apply_kw_func or, when there are no
// kwargs / varargs, to the regular pys_apply slow path.
VALUE
pys_apply_kw(CTX *c, VALUE fn, int argc, VALUE *argv,
            int kwc, const char **kwnames, VALUE *kwvalues)
{
    if (pys_is_bound(fn)) {
        struct pysobj *bm = PYS_PTR(fn);
        VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
        av[0] = bm->bound.self;
        for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
        return pys_apply_kw(c, bm->bound.func, argc + 1, av, kwc, kwnames, kwvalues);
    }
    if (pys_is_instance(fn)) {
        VALUE call = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(fn)->inst.cls), PYS_INTERN_call);
        if (call != PYS_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = fn;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            return pys_apply_kw(c, call, argc + 1, av, kwc, kwnames, kwvalues);
        }
    }
    if (pys_is_class(fn)) {
        if (PYS_PTR(fn)->cls.builtin_ctor) {
            // Forward kwargs through the thread-local pointers used by
            // pys_bi_kwarg().
            int saved_kwc = PYS_BI_KWC;
            const char **saved_kn = PYS_BI_KWNAMES;
            VALUE *saved_kv = PYS_BI_KWVALUES;
            PYS_BI_KWC = kwc;
            PYS_BI_KWNAMES = (const char **)kwnames;
            PYS_BI_KWVALUES = kwvalues;
            VALUE r = PYS_PTR(fn)->cls.builtin_ctor(c, argc, argv);
            PYS_BI_KWC = saved_kwc;
            PYS_BI_KWNAMES = saved_kn;
            PYS_BI_KWVALUES = saved_kv;
            return r;
        }
        // Metaclass __call__ override: lets a metaclass intercept the
        // class call (e.g. singleton pattern).  type(cls).__call__(cls, ...)
        VALUE meta = pys_class_lookup_method(fn, PYS_INTERN_metaclass);
        if (meta != PYS_NONE && pys_is_class(meta)) {
            VALUE mc = pys_class_lookup_method(meta, PYS_INTERN_call);
            if (mc != PYS_NONE) {
                VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
                av[0] = fn;
                for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
                return pys_apply_kw(c, mc, argc + 1, av, kwc, kwnames, kwvalues);
            }
        }
        // Custom __new__: lets users intercept instance creation (singleton
        // pattern, immutable types, etc.).  Return value of __new__ becomes
        // the instance; if it's an instance of cls, __init__ runs on it.
        VALUE inst;
        struct pysclass *cd = &PYS_PTR(fn)->cls;
        if (UNLIKELY(!cd->slots_initialized)) pyclass_refresh_slots(fn);
        if (LIKELY(cd->fast_new)) {
            // Skip __new__ dispatch for the common "user class with default
            // object.__new__" — instantiation collapses to alloc + __init__.
            inst = pys_make_instance(fn);
        } else {
            VALUE new_m = cd->slot_new;
            if (new_m != PYS_NONE) {
                VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
                av[0] = fn;
                for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
                inst = pys_apply_kw(c, new_m, argc + 1, av, kwc, kwnames, kwvalues);
                if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            } else {
                inst = pys_make_instance(fn);
            }
            if (cd->is_exception) {
                pys_setattr(c, inst, "args", pys_make_tuple(argv, argc));
                if (argc >= 1 && pys_is_str(argv[0])) pys_setattr(c, inst, "message", argv[0]);
            }
        }
        VALUE init = cd->slot_init;
        if (init != PYS_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = inst;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            pys_apply_kw(c, init, argc + 1, av, kwc, kwnames, kwvalues);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        }
        return inst;
    }
    if (pys_is_func(fn)) {
        extern bool pys_func_is_generator(VALUE fn);
        extern VALUE pys_make_gen(CTX *c, VALUE fn, int argc, VALUE *argv, int kwc, const char **kwn, VALUE *kwv);
        if (pys_func_is_generator(fn))
            return pys_make_gen(c, fn, argc, argv, kwc, kwnames, kwvalues);
        // `async def` body — pystro doesn't run an event loop, so we
        // return a fake coroutine wrapper that satisfies the methods
        // CPython's stdlib calls at module init (`close()`, `__await__`,
        // `send()`, `throw()`).  The body is NOT actually executed
        // (CPython's `_collections_abc.py` / `types.py` etc. don't await
        // the result; they just call .close()).
        if (PYS_PTR(fn)->func.is_async) {
            extern VALUE pys_make_fake_coroutine(CTX *c);
            return pys_make_fake_coroutine(c);
        }
        return pys_apply_kw_func(c, fn, argc, argv, kwc, kwnames, kwvalues);
    }
    if (pys_is_builtin(fn)) {
        // ALWAYS set PYS_BI_KW* — even when kwc==0 — so nested
        // class/builtin calls inside the builtin don't pick up stale
        // thread-local kwargs from an outer caller.  Save/restore the
        // previous values so the outer scope's view is unaffected.
        extern int    PYS_BI_KWC;
        extern const char **PYS_BI_KWNAMES;
        extern VALUE *PYS_BI_KWVALUES;
        int saved_kwc = PYS_BI_KWC;
        const char **saved_kn = PYS_BI_KWNAMES;
        VALUE *saved_kv = PYS_BI_KWVALUES;
        PYS_BI_KWC = kwc;
        PYS_BI_KWNAMES = (const char **)kwnames;
        PYS_BI_KWVALUES = kwvalues;
        VALUE r = pys_apply_slow(c, fn, argc, argv);
        PYS_BI_KWC = saved_kwc;
        PYS_BI_KWNAMES = saved_kn;
        PYS_BI_KWVALUES = saved_kv;
        return r;
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "object is not callable");
}

// Slow-path apply: bound / class / builtin / func-with-defaults / wrong type.
// The closure-with-matching-arity fast path lives inline in `pys_apply` in
// node.h so SD code folds the call setup directly.
VALUE
pys_apply_slow(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    if (pys_is_bound(fn)) {
        struct pysobj *bm = PYS_PTR(fn);
        VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
        av[0] = bm->bound.self;
        for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
        return pys_apply(c, bm->bound.func, argc + 1, av);
    }
    if (pys_is_class(fn)) {
        // Built-in type class (int / list / str / ...): call its
        // C constructor directly.  Result is a primitive value, not a
        // PYS_T_INSTANCE, since the constructor returns int/list/etc.
        if (PYS_PTR(fn)->cls.builtin_ctor) {
            return PYS_PTR(fn)->cls.builtin_ctor(c, argc, argv);
        }
        // Metaclass __call__ override (singleton, etc.).
        VALUE meta_s = pys_class_lookup_method(fn, PYS_INTERN_metaclass);
        if (meta_s != PYS_NONE && pys_is_class(meta_s)) {
            VALUE mc = pys_class_lookup_method(meta_s, PYS_INTERN_call);
            if (mc != PYS_NONE) {
                VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
                av[0] = fn;
                for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
                // pys_apply has no kwargs — explicitly zero PYS_BI_KWC
                // around the dispatch so the metaclass __call__ (e.g.
                // bi_type_call) doesn't pick up stale kwargs from an
                // outer caller's frame.
                extern int PYS_BI_KWC;
                int saved_kwc = PYS_BI_KWC;
                PYS_BI_KWC = 0;
                VALUE r = pys_apply(c, mc, argc + 1, av);
                PYS_BI_KWC = saved_kwc;
                return r;
            }
        }
        // __new__ — always defined (object.__new__ is the default).
        // It returns the new instance and handles built-in subclass
        // primary value setup.
        VALUE inst;
        struct pysclass *cd_apply = &PYS_PTR(fn)->cls;
        if (UNLIKELY(!cd_apply->slots_initialized)) pyclass_refresh_slots(fn);
        if (LIKELY(cd_apply->fast_new)) {
            inst = pys_make_instance(fn);
        } else {
            VALUE new_m = cd_apply->slot_new;
            if (new_m != PYS_NONE) {
                VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
                av[0] = fn;
                for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
                inst = pys_apply(c, new_m, argc + 1, av);
                if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            } else {
                inst = pys_make_instance(fn);
                VALUE bin_base = pys_class_find_builtin_base(fn);
                if (bin_base != PYS_NONE) {
                    VALUE primary = PYS_PTR(bin_base)->cls.builtin_ctor(c, argc, argv);
                    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
                    PYS_PTR(inst)->inst.primary = primary;
                }
            }
        }
        if (PYS_PTR(fn)->cls.is_exception) {
            extern bool class_is_ancestor(VALUE cls, VALUE target);
            pys_setattr(c, inst, "args", pys_make_tuple(argv, argc));
            if (argc >= 1 && pys_is_str(argv[0])) pys_setattr(c, inst, "message", argv[0]);
            // ExceptionGroup(msg, [excs]) — also stash the exceptions list
            // as .exceptions so PEP 654 except* matching can split it.
            if (class_is_ancestor(fn, c->EXC_BaseExceptionGroup)
                    && argc >= 2 && (pys_is_list(argv[1]) || pys_is_tuple(argv[1]))) {
                pys_setattr(c, inst, "exceptions", argv[1]);
            }
        }
        // Use the pre-resolved slot directly — `pys_class_lookup_method`
        // would do the same load + branch but as an external call.  cd is
        // already in scope from above (slots_initialized was checked).
        VALUE init = cd_apply->slot_init;
        if (init != PYS_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = inst;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            pys_apply(c, init, argc + 1, av);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        }
        return inst;
    }
    if (pys_is_func(fn)) {
        extern bool pys_func_is_generator(VALUE fn);
        extern VALUE pys_make_gen(CTX *c, VALUE fn, int argc, VALUE *argv, int kwc, const char **kwn, VALUE *kwv);
        if (pys_func_is_generator(fn))
            return pys_make_gen(c, fn, argc, argv, 0, NULL, NULL);
        // All other cases (default args, *args, **kwargs, arity
        // mismatch) route through the keyword-aware dispatcher.
        return pys_apply_kw_func(c, fn, argc, argv, 0, NULL, NULL);
    }
    if (pys_is_builtin(fn)) {
        struct pysobj *f = PYS_PTR(fn);
        if (argc < f->builtin.min_argc ||
            (f->builtin.max_argc >= 0 && argc > f->builtin.max_argc))
            PYS_RAISE_EXC(c, c->EXC_TypeError,
                         "%s() takes %d-%d arguments but %d were given",
                         f->builtin.name, f->builtin.min_argc,
                         f->builtin.max_argc, argc);
        return f->builtin.fn(c, argc, argv);
    }
    // Instance with __call__ — dispatch to it with self prepended.
    if (pys_is_instance(fn)) {
        VALUE call = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(fn)->inst.cls), PYS_INTERN_call);
        if (call != PYS_NONE) {
            VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (argc + 1));
            av[0] = fn;
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            return pys_apply(c, call, argc + 1, av);
        }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "object is not callable");
}

// ---------------------------------------------------------------------------
// Display + repr.
// ---------------------------------------------------------------------------

// Visiting set for cycle-safe pys_display.  Static because pys_display
// is single-threaded (no concurrent prints) and reentrant — a list
// item that's the parent list itself would otherwise stack-overflow.
#define PYS_DISPLAY_MAX_DEPTH 64
static struct pysobj *pys_display_visit[PYS_DISPLAY_MAX_DEPTH];
static int           pys_display_visit_top = 0;

static inline bool
pys_display_seen(struct pysobj *o)
{
    for (int i = 0; i < pys_display_visit_top; i++)
        if (pys_display_visit[i] == o) return true;
    return false;
}

// Format a double using the shortest decimal that round-trips back to
// the same value (matches CPython's repr).  Tries %.NNg for NN=1..17
// and picks the smallest NN whose result parses back to the exact d.
// If the chosen format used scientific notation but |d| is within the
// "no-exponent" range, redo with %.NNe-derived %f-style.
static void
pys_fmt_double(char *buf, size_t bufsz, double d)
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
pys_display(FILE *fp, VALUE v, bool repr)
{
    if (PYS_IS_FIXNUM(v)) { fprintf(fp, "%ld", (long)PYS_FIXVAL(v)); return; }
    if (PYS_IS_FLONUM(v)) {
        double d = pys_flonum_to_double(v);
        char buf[64]; pys_fmt_double(buf, sizeof(buf), d);
        bool has_marker = false;
        for (const char *p = buf; *p; p++)
            if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') { has_marker = true; break; }
        fputs(buf, fp);
        if (!has_marker) fputs(".0", fp);
        return;
    }
    if (v == PYS_NONE)  { fputs("None", fp);  return; }
    if (v == PYS_TRUE)  { fputs("True", fp);  return; }
    if (v == PYS_FALSE) { fputs("False", fp); return; }
    struct pysobj *o = PYS_PTR(v);
    switch (o->type) {
      case PYS_T_FLOAT: {
        char buf[64];
        pys_fmt_double(buf, sizeof(buf), o->dbl);
        bool has_marker = false;
        for (const char *p = buf; *p; p++)
            if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') {
                has_marker = true; break;
            }
        fputs(buf, fp);
        if (!has_marker) fputs(".0", fp);
        return;
      }
      case PYS_T_BIGNUM: {
        char *s = mpz_get_str(NULL, 10, o->mpz);
        fputs(s, fp);
        return;
      }
      case PYS_T_COMPLEX: {
        char re[64], im[64];
        pys_fmt_double(re, sizeof(re), o->cpx.re);
        pys_fmt_double(im, sizeof(im), o->cpx.im);
        if (o->cpx.re == 0.0) {
            fprintf(fp, "%sj", im);
        } else {
            fprintf(fp, "(%s%s%sj)", re, (o->cpx.im >= 0) ? "+" : "", im);
        }
        return;
      }
      case PYS_T_STR:
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
      case PYS_T_BYTES:
      case PYS_T_BYTEARRAY: {
        if (o->type == PYS_T_BYTEARRAY) fputs("bytearray(", fp);
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
        if (o->type == PYS_T_BYTEARRAY) fputc(')', fp);
        return;
      }
      case PYS_T_LIST: {
        if (pys_display_seen(o)) { fputs("[...]", fp); return; }
        bool pushed = false;
        if (pys_display_visit_top < PYS_DISPLAY_MAX_DEPTH) {
            pys_display_visit[pys_display_visit_top++] = o;
            pushed = true;
        }
        fputc('[', fp);
        for (size_t i = 0; i < o->list.len; i++) {
            if (i) fputs(", ", fp);
            pys_display(fp, o->list.items[i], true);
        }
        fputc(']', fp);
        if (pushed) pys_display_visit_top--;
        return;
      }
      case PYS_T_TUPLE: {
        if (pys_display_seen(o)) { fputs("(...)", fp); return; }
        bool pushed = false;
        if (pys_display_visit_top < PYS_DISPLAY_MAX_DEPTH) {
            pys_display_visit[pys_display_visit_top++] = o;
            pushed = true;
        }
        fputc('(', fp);
        for (size_t i = 0; i < o->list.len; i++) {
            if (i) fputs(", ", fp);
            pys_display(fp, o->list.items[i], true);
        }
        if (o->list.len == 1) fputc(',', fp);
        fputc(')', fp);
        if (pushed) pys_display_visit_top--;
        return;
      }
      case PYS_T_DICT: {
        if (pys_display_seen(o)) { fputs("{...}", fp); return; }
        bool pushed = false;
        if (pys_display_visit_top < PYS_DISPLAY_MAX_DEPTH) {
            pys_display_visit[pys_display_visit_top++] = o;
            pushed = true;
        }
        fputc('{', fp);
        struct pysdict *d = o->dict;
        size_t printed = 0;
        extern CTX *pys_current_ctx;
        for (size_t i = 0; i < d->elen; i++) {
            if (!pydict_entry_live(d, i)) continue;
            if (printed++) fputs(", ", fp);
            pys_display(fp, d->entries[i].key, true);
            if (pys_current_ctx && pys_current_ctx->state == PYS_STATE_RAISE) {
                if (pushed) pys_display_visit_top--;
                return;
            }
            fputs(": ", fp);
            pys_display(fp, d->entries[i].value, true);
            if (pys_current_ctx && pys_current_ctx->state == PYS_STATE_RAISE) {
                if (pushed) pys_display_visit_top--;
                return;
            }
        }
        fputc('}', fp);
        if (pushed) pys_display_visit_top--;
        return;
      }
      case PYS_T_SET: {
        if (pys_display_seen(o)) { fputs("set(...)", fp); return; }
        struct pysdict *d = o->dict;
        if (d->used == 0) { fputs("set()", fp); return; }
        bool pushed = false;
        if (pys_display_visit_top < PYS_DISPLAY_MAX_DEPTH) {
            pys_display_visit[pys_display_visit_top++] = o;
            pushed = true;
        }
        fputc('{', fp);
        size_t printed = 0;
        for (size_t i = 0; i < d->elen; i++) {
            if (!pydict_entry_live(d, i)) continue;
            if (printed++) fputs(", ", fp);
            pys_display(fp, d->entries[i].key, true);
        }
        fputc('}', fp);
        if (pushed) pys_display_visit_top--;
        return;
      }
      case PYS_T_FROZENSET: {
        if (pys_display_seen(o)) { fputs("frozenset(...)", fp); return; }
        struct pysdict *d = o->dict;
        fputs("frozenset(", fp);
        if (d->used > 0) {
            bool pushed = false;
            if (pys_display_visit_top < PYS_DISPLAY_MAX_DEPTH) {
                pys_display_visit[pys_display_visit_top++] = o;
                pushed = true;
            }
            fputc('{', fp);
            size_t printed = 0;
            for (size_t i = 0; i < d->elen; i++) {
                if (!pydict_entry_live(d, i)) continue;
                if (printed++) fputs(", ", fp);
                pys_display(fp, d->entries[i].key, true);
            }
            fputc('}', fp);
            if (pushed) pys_display_visit_top--;
        }
        fputc(')', fp);
        return;
      }
      case PYS_T_RANGE:
        fprintf(fp, "range(%lld, %lld",
                (long long)o->range.start, (long long)o->range.stop);
        if (o->range.step != 1) fprintf(fp, ", %lld", (long long)o->range.step);
        fputc(')', fp);
        return;
      case PYS_T_FUNC:
        fprintf(fp, "<function %s>", o->func.name ? o->func.name : "?");
        return;
      case PYS_T_BUILTIN: {
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
      case PYS_T_BOUND_METHOD:
        fprintf(fp, "<bound method>");
        return;
      case PYS_T_SLICE: {
        fputs("slice(", fp);
        pys_display(fp, o->slice_.start, true);
        fputs(", ", fp);
        pys_display(fp, o->slice_.stop, true);
        fputs(", ", fp);
        pys_display(fp, o->slice_.step, true);
        fputc(')', fp);
        return;
      }
      case PYS_T_ELLIPSIS:
        fputs("Ellipsis", fp);
        return;
      case PYS_T_NOTIMPL:
        fputs("NotImplemented", fp);
        return;
      case PYS_T_MEMVIEW:
        fprintf(fp, "<memory>");
        return;
      case PYS_T_MODULE:
        fprintf(fp, "<module '%s'>", o->module.name);
        return;
      case PYS_T_CLASS:
        fprintf(fp, "<class '%s'>", o->cls.name);
        return;
      case PYS_T_INSTANCE: {
        extern bool class_is_ancestor(VALUE cls, VALUE target);
        // Defer to __str__ / __repr__ if defined.  We need a CTX to
        // call methods, but pys_display doesn't take one — work around
        // by stashing it in a TLS-ish "current ctx" pointer set by
        // bi_print / pys_to_str.  For v0, use a simpler approach: just
        // walk class methods directly via cached ctx.
        extern CTX *pys_current_ctx;
        if (pys_current_ctx) {
            const char *m_name = repr ? "__repr__" : "__str__";
            VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), m_name);
            if (m == PYS_NONE && !repr)
                m = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), PYS_INTERN_repr);
            if (m != PYS_NONE) {
                VALUE av[1] = { v };
                VALUE r = pys_apply(pys_current_ctx, m, 1, av);
                // __repr__/__str__ raised: bail out — caller (pys_to_repr /
                // pys_to_str) will surface the exception.  Don't dereference
                // r when state is RAISE (r is 0 / NULL).
                if (UNLIKELY(pys_current_ctx->state == PYS_STATE_RAISE)) return;
                if (pys_is_str(r)) { fwrite(PYS_PTR(r)->str.chars, 1, PYS_PTR(r)->str.len, fp); return; }
            }
            // Default str() / repr() for exception instances.  CPython
            // uses the .args tuple:
            //   0 args → ""           1 arg → str(args[0])
            //   N args → repr(args)   (i.e. "(a, b, ...)")
            // repr always produces ClassName(args...).
            if (pys_is_class(PYS_OBJ_VAL(o->inst.cls))
                && pys_current_ctx->EXC_Exception
                && class_is_ancestor(PYS_OBJ_VAL(o->inst.cls), pys_current_ctx->EXC_Exception)) {
                VALUE args = pys_getattr(pys_current_ctx, v, "args");
                if (pys_current_ctx->state != PYS_STATE_NORMAL) {
                    pys_current_ctx->state = PYS_STATE_NORMAL;
                    args = PYS_NONE;
                }
                if (pys_is_tuple(args)) {
                    size_t n = PYS_PTR(args)->list.len;
                    if (!repr) {
                        if (n == 0) return;
                        if (n == 1) {
                            pys_display(fp, PYS_PTR(args)->list.items[0], false);
                            return;
                        }
                        // Fall through: print tuple repr.
                        pys_display(fp, args, true);
                        return;
                    }
                    fprintf(fp, "%s(", o->inst.cls->cls.name);
                    for (size_t i = 0; i < n; i++) {
                        if (i) fputs(", ", fp);
                        pys_display(fp, PYS_PTR(args)->list.items[i], true);
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
            pys_display(fp, o->inst.primary, repr);
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
pys_to_str(CTX *c, VALUE v)
{
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), PYS_INTERN_str);
        if (m == PYS_NONE)
            m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), PYS_INTERN_repr);
        if (m != PYS_NONE) {
            VALUE av[1] = { v };
            VALUE r = pys_apply(c, m, 1, av);
            if (pys_is_str(r)) return r;
        }
    }
    if (pys_is_str(v)) return v;
    char buf[256];
    FILE *mfp = fmemopen(buf, sizeof(buf) - 1, "w");
    if (mfp) {
        pys_display(mfp, v, false);
        fflush(mfp);
        long len = ftell(mfp);
        fclose(mfp);
        if (len >= 0 && (size_t)len < sizeof(buf) - 1) return pys_make_str(buf, (size_t)len);
    }
    // fallback: large repr — alloc dynamically.
    size_t cap = 1024;
    char *big = (char *)GC_malloc_atomic(cap);
    FILE *bfp = open_memstream(&big, &cap);
    pys_display(bfp, v, false);
    fclose(bfp);
    VALUE r = pys_make_str(big, strlen(big));
    return r;
}

VALUE
pys_to_repr(CTX *c, VALUE v)
{
    if (pys_is_instance(v)) {
        // Track recursion via the same display-visit array used by
        // list/dict/set/tuple — a class with `__repr__` that returns
        // `repr(self.value)` where `self.value` contains self would
        // otherwise stack-overflow.
        struct pysobj *o = PYS_PTR(v);
        if (pys_display_seen(o)) {
            const char *cn = o->inst.cls->cls.name;
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "%s(...)", cn);
            return pys_make_str(buf, (size_t)n);
        }
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), PYS_INTERN_repr);
        if (m != PYS_NONE) {
            bool pushed = false;
            if (pys_display_visit_top < PYS_DISPLAY_MAX_DEPTH) {
                pys_display_visit[pys_display_visit_top++] = o;
                pushed = true;
            }
            VALUE av[1] = { v };
            VALUE r = pys_apply(c, m, 1, av);
            if (pushed) pys_display_visit_top--;
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            if (pys_is_str(r)) return r;
        }
    }
    char *big = NULL;
    size_t cap = 0;
    FILE *bfp = open_memstream(&big, &cap);
    pys_display(bfp, v, true);
    fclose(bfp);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) {
        free(big);
        return 0;
    }
    VALUE r = pys_make_str(big, strlen(big));
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
    if (cls == PYS_NONE || !pys_is_class(cls)) return false;
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    for (int i = 0; i < cd->nmro; i++) if (cd->mro[i] == target) return true;
    return false;
}

bool
pys_exc_matches(CTX *c, VALUE exc, VALUE cls)
{
    if (!pys_is_instance(exc)) return false;
    // Tuple of classes: match any.
    if (pys_is_tuple(cls)) {
        size_t n = PYS_PTR(cls)->list.len;
        for (size_t i = 0; i < n; i++)
            if (pys_exc_matches(c, exc, PYS_PTR(cls)->list.items[i])) return true;
        return false;
    }
    if (!pys_is_class(cls)) return false;
    return class_is_ancestor(PYS_OBJ_VAL(PYS_PTR(exc)->inst.cls), cls);
}

// ---------------------------------------------------------------------------
// Generators (ucontext-based lazy yield).
//
// A generator function (one whose body contains `yield`) is detected by
// the parser; calling it does NOT run the body — instead it builds a
// PYS_T_GEN object holding the captured args + a fresh stack + an
// uninitialised `body_ctx`.  next() / iter().__next__ swap into the
// body, which runs until it hits `yield expr`; yield stashes the value
// and swaps back.  The C stack used by the generator is a separate
// mmap-style allocation (we use GC_malloc atomic; Boehm scans the
// caller's stack but not pointers ON the gen's stack — to keep VALUEs
// alive we add the stack region to GC_add_roots before running).
// ---------------------------------------------------------------------------

#include <sys/mman.h>

#define PYGEN_STACK_SZ (256 * 1024)

struct pysgen {
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
    struct pysframe *gen_env;
    struct pysglobals *gen_globals;
    VALUE       gen_method_class;
    int         gen_state;
    VALUE       gen_state_value;
    // Send value carried into the body — yield expression evaluates to
    // it.  Default PYS_NONE for plain next().
    VALUE       send_value;
    // throw() / close() — when set, yield's swap-back raises this
    // exception inside the body.
    VALUE       throw_exc;
    bool        throw_pending;
    // Back-link to outer enclosing gen (NULL if outermost) so nested
    // gens compose.
    struct pysgen *prev_gen;
    // When this gen is suspended inside `yield from inner_gen`, this is
    // the inner generator.  Used to propagate close() / throw() through
    // the yield-from chain (matches CPython's yield-from cleanup).
    VALUE       yf_inner;
};

// makecontext takes only int args; pass the gen pointer through a
// process-global slot.
static struct pysgen *G_gen_to_start = NULL;

static VALUE pys_apply_gen_call(CTX *c, VALUE fn, int argc, VALUE *argv, int kwc, const char **kwn, VALUE *kwv);

static void
gen_entry(void)
{
    extern CTX *pys_current_ctx;
    CTX *c = pys_current_ctx;
    struct pysgen *g = G_gen_to_start;
    G_gen_to_start = NULL;

    // Same setup as pys_apply_kw_func: build a frame, fill from argv /
    // kwargs / defaults, EVAL the body.
    extern VALUE pys_apply_kw_func(CTX *c, VALUE fn, int argc, VALUE *argv,
                                  int kwc, const char **kwnames, VALUE *kwvalues);
    // We can't simply call pys_apply_kw_func because that would set up
    // CTX state including saving/restoring c->env around EVAL, but we
    // want the call-stack to STAY in the gen's context until done.
    // Instead emulate: build the frame, set CTX, run, set done.
    struct pysobj *f = PYS_PTR(g->func);
    int needed = f->func.nparams;
    int n_pos_named = f->func.n_pos_named;
    bool has_va = f->func.has_varargs;
    bool has_kw = f->func.has_kwargs;
    int va_slot = has_va ? n_pos_named : -1;
    int kwonly_start = n_pos_named + (has_va ? 1 : 0);
    int n_kwonly = needed - kwonly_start - (has_kw ? 1 : 0);
    int kw_slot = has_kw ? (kwonly_start + n_kwonly) : -1;

    struct pysframe *new_env = pys_new_frame(f->func.env, f->func.nlocals);
    new_env->slot_names = f->func.local_names;
    bool *filled = (bool *)alloca(sizeof(bool) * (needed > 0 ? needed : 1));
    for (int i = 0; i < needed; i++) filled[i] = false;
    int pos_into = g->argc < n_pos_named ? g->argc : n_pos_named;
    for (int i = 0; i < pos_into; i++) { new_env->slots[i] = g->argv[i]; filled[i] = true; }
    if (has_va) {
        int n_extra = g->argc - pos_into;
        VALUE *items = n_extra > 0 ? (VALUE *)alloca(sizeof(VALUE) * n_extra) : NULL;
        for (int i = 0; i < n_extra; i++) items[i] = g->argv[pos_into + i];
        new_env->slots[va_slot] = pys_make_tuple(items, n_extra);
        filled[va_slot] = true;
    }
    if (has_kw) {
        new_env->slots[kw_slot] = pys_make_dict();
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
        else if (has_kw) pys_dict_set(c, new_env->slots[kw_slot],
                                     pys_make_str(g->kwnames[i], strlen(g->kwnames[i])),
                                     g->kwvalues[i]);
    }
    for (int i = 0; i < needed; i++) {
        if (filled[i] || i == va_slot || i == kw_slot) continue;
        VALUE d = f->func.defaults[i];
        if (d == (VALUE)0) {
            // missing required — set None and let the body misbehave;
            // gen creation should have caught this.
            new_env->slots[i] = PYS_NONE;
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
    // here; the swapcontext-back path in pys_gen_next propagates it.
    // Capture uncaught exceptions inside the gen body via a local
    // State-based propagation only — body's raise/return is left in
    // c->state for pys_gen_next to consume after swapcontext-back.
    EVAL(c, f->func.body);

    // If gen body executed `return X`, capture X for StopIteration.value.
    if (c->state == PYS_STATE_RETURN) {
        g->return_value = c->state_value;
        c->state = PYS_STATE_NORMAL;
        c->state_value = PYS_NONE;
    } else {
        g->return_value = PYS_NONE;
    }
    g->done = true;
    c->current_gen = g->prev_gen;
    swapcontext(&g->body_ctx, &g->caller_ctx);
    // unreachable
}


// Builtin methods for the fake-coroutine type.  Pystro doesn't run an
// event loop; the methods just satisfy CPython stdlib's import-time
// "(async def f())().close()" / "type((async def f())())" probes.
static VALUE
bi_fake_coro_close(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    return PYS_NONE;
}

static VALUE
bi_fake_coro_send(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    PYS_RAISE_EXC(c, c->EXC_StopIteration, "fake coroutine sent into");
}

static VALUE
bi_fake_coro_throw(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (argc >= 2) PYS_RAISE_EXC(c, argv[1], "fake coroutine throw");
    PYS_RAISE_EXC(c, c->EXC_RuntimeError, "fake coroutine throw");
}

static VALUE
bi_fake_coro_await(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    return PYS_NONE;   // already-resolved
}

VALUE
pys_make_fake_coroutine(CTX *c)
{
    extern const char *intern_name(const char *s, size_t n);
    // Build an instance of a synthetic class so isinstance checks work.
    static VALUE coro_cls = 0;
    if (!coro_cls || coro_cls == PYS_NONE) {
        coro_cls = pys_make_class("coroutine", PYS_NONE, false);
        pys_class_add_method(c, coro_cls, intern_name("close", 5),
            pys_make_builtin("close", bi_fake_coro_close, 1, 1));
        pys_class_add_method(c, coro_cls, intern_name("send", 4),
            pys_make_builtin("send", bi_fake_coro_send, 1, 2));
        pys_class_add_method(c, coro_cls, intern_name("throw", 5),
            pys_make_builtin("throw", bi_fake_coro_throw, 2, 4));
        pys_class_add_method(c, coro_cls, intern_name("__await__", 9),
            pys_make_builtin("__await__", bi_fake_coro_await, 1, 1));
    }
    return pys_make_instance(coro_cls);
}


VALUE
pys_make_gen(CTX *c, VALUE fn, int argc, VALUE *argv, int kwc, const char **kwn, VALUE *kwv)
{
    (void)c;
    struct pysgen *g = (struct pysgen *)GC_malloc(sizeof(struct pysgen));
    g->stack = GC_malloc(PYGEN_STACK_SZ);
    g->started = false;
    g->done = false;
    g->send_value = PYS_NONE;
    g->throw_exc = PYS_NONE;
    g->throw_pending = false;
    g->yf_inner = PYS_NONE;
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
    struct pysobj *o = pys_alloc(PYS_T_GEN);
    o->gen = g;
    return PYS_OBJ_VAL(o);
}

VALUE
pys_gen_next(CTX *c, VALUE gen_v)
{
    struct pysgen *g = PYS_PTR(gen_v)->gen;
    if (g->done) {
        // Build the StopIteration *before* flipping state to RAISE —
        // pys_setattr / pys_make_instance run nested calls that bail
        // out early when c->state == RAISE, so setting it first would
        // strand the instance without .value / .args attributes.
        VALUE si = pys_make_instance(c->EXC_StopIteration);
        VALUE rv = g->return_value ? g->return_value : PYS_NONE;
        pys_setattr(c, si, "value", rv);
        // CPython's exception protocol expects __traceback__ /
        // __context__ / __cause__ on every Exception instance — even
        // those created without going through __init__.
        pys_setattr(c, si, "__traceback__", PYS_NONE);
        pys_setattr(c, si, "__context__", PYS_NONE);
        pys_setattr(c, si, "__cause__", PYS_NONE);
        pys_setattr(c, si, "__suppress_context__", PYS_FALSE);
        if (rv != PYS_NONE)
            pys_setattr(c, si, "args", pys_make_tuple(&rv, 1));
        else
            pys_setattr(c, si, "args", pys_make_tuple(NULL, 0));
        c->state = PYS_STATE_RAISE;
        c->state_value = si;
        return PYS_NONE;
    }

    // Save caller state on this stack frame.
    struct pysframe *saved_env = c->env;
    int saved_state = c->state;
    VALUE saved_sval = c->state_value;
    VALUE saved_mc = c->method_class;
    struct pysglobals *saved_g = c->globals;
    struct pysgen *saved_cg = c->current_gen;
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
    }
    swapcontext(&g->caller_ctx, &g->body_ctx);
    // Body yielded or finished.  Save gen-side CTX so resume restores it.
    g->gen_env = c->env;
    g->gen_state = c->state;
    g->gen_state_value = c->state_value;
    g->gen_method_class = c->method_class;
    g->gen_globals = c->globals;

    bool was_done = g->done;
    bool raised = (c->state == PYS_STATE_RAISE);
    VALUE exc = c->state_value;
    VALUE r = was_done ? PYS_NONE : g->yield_value;

    // Restore caller state.
    c->env = saved_env;
    c->state = saved_state;
    c->state_value = saved_sval;
    c->method_class = saved_mc;
    c->globals = saved_g;
    c->current_gen = saved_cg;

    if (raised) { c->state = PYS_STATE_RAISE; c->state_value = exc; return PYS_NONE; }
    if (was_done) {
        VALUE si = pys_make_instance(c->EXC_StopIteration);
        pys_setattr(c, si, "value", g->return_value);
        // args = (value,) when value is non-None, else ()
        if (g->return_value != PYS_NONE)
            pys_setattr(c, si, "args", pys_make_tuple(&g->return_value, 1));
        else
            pys_setattr(c, si, "args", pys_make_tuple(NULL, 0));
        pys_setattr(c, si, "__traceback__", PYS_NONE);
        pys_setattr(c, si, "__context__", PYS_NONE);
        pys_setattr(c, si, "__cause__", PYS_NONE);
        pys_setattr(c, si, "__suppress_context__", PYS_FALSE);
        c->state = PYS_STATE_RAISE;
        c->state_value = si;
        return PYS_NONE;
    }
    return r;
}

// Returns the value passed to .send() / .next() — yield expression
// evaluates to this.  When throw() was used, sets the pending exc
// state instead so the EVAL chain unwinds out of the yield site.
VALUE
pys_gen_yield(CTX *c, VALUE v)
{
    struct pysgen *g = c->current_gen;
    if (!g) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "yield outside of generator");
    g->yield_value = v;
    // Reset send_value / throw to defaults; pys_gen_send / pys_gen_throw
    // overwrite before swap.
    g->send_value = PYS_NONE;
    g->throw_pending = false;
    // pys_gen_next() does the save/restore around the swap.
    swapcontext(&g->body_ctx, &g->caller_ctx);
    // We're back in the body.  If the caller threw, raise inside body.
    if (g->throw_pending) {
        g->throw_pending = false;
        c->state = PYS_STATE_RAISE;
        c->state_value = g->throw_exc;
        // If we're suspended in `yield from inner`, propagate the
        // close/throw to the inner gen first (CPython's yield-from
        // cleanup protocol).  GeneratorExit → inner.close().
        // Other exceptions → inner.throw(exc); if inner yields a
        // replacement value, yield that from the outer (the throw
        // becomes a value).
        if (g->yf_inner != (VALUE)0 && g->yf_inner != PYS_NONE
            && PYS_IS_PTR(g->yf_inner) && PYS_PTR(g->yf_inner)->type == PYS_T_GEN) {
            VALUE inner = g->yf_inner;
            g->yf_inner = PYS_NONE;
            VALUE saved_exc = c->state_value;
            extern VALUE pys_gen_close(CTX *c, VALUE g);
            extern VALUE pys_gen_throw(CTX *c, VALUE g, VALUE exc);
            extern bool class_is_ancestor(VALUE cls, VALUE target);
            bool is_gen_exit = pys_is_instance(saved_exc) &&
                class_is_ancestor(PYS_OBJ_VAL(PYS_PTR(saved_exc)->inst.cls),
                                  c->EXC_GeneratorExit);
            c->state = PYS_STATE_NORMAL;
            c->state_value = PYS_NONE;
            if (is_gen_exit) {
                pys_gen_close(c, inner);
                c->state = PYS_STATE_RAISE;
                c->state_value = saved_exc;
            } else {
                VALUE r = pys_gen_throw(c, inner, saved_exc);
                if (c->state == PYS_STATE_NORMAL) {
                    // Inner caught and yielded r — re-yield it from outer.
                    g->yf_inner = inner;     // restore so further yield
                                             // chains keep working
                    return pys_gen_yield(c, r);
                }
                // Inner raised — already in state RAISE, fall through
                // and propagate.
            }
        }
        // State-based propagation — gen_entry sees state RAISE
        // and exits, marking done.
        return 0;
    }
    return g->send_value;
}

// `yield from iter` — yield each value from the iterable, return the
// final StopIteration.value when exhausted.  Used as an expression so
// the value can be bound: `result = yield from gen()`.
VALUE
pys_gen_yield_from(CTX *c, VALUE iter)
{
    extern VALUE pys_gen_close(CTX *c, VALUE g);
    extern VALUE pys_gen_throw(CTX *c, VALUE g, VALUE exc);
    struct pys_iter it;
    pys_iter_init(c, &it, iter);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE x;
    VALUE result = PYS_NONE;
    // Mark the active yield-from inner gen on the enclosing generator
    // so a close() / throw() on the outer can propagate.
    struct pysgen *outer_g = c->current_gen;
    VALUE saved_yf = outer_g ? outer_g->yf_inner : PYS_NONE;
    if (outer_g) outer_g->yf_inner = iter;
    while (pys_iter_next(c, &it, &x)) {
        pys_gen_yield(c, x);
        if (c->state != PYS_STATE_NORMAL) {
            if (outer_g) outer_g->yf_inner = saved_yf;
            return PYS_NONE;
        }
    }
    if (outer_g) outer_g->yf_inner = saved_yf;
    // Inner exhausted normally.  If the source is a generator with a
    // captured return-value, surface it as our expression value.
    if (PYS_IS_PTR(iter) && PYS_PTR(iter)->type == PYS_T_GEN) {
        struct pysgen *gg = PYS_PTR(iter)->gen;
        if (gg->return_value) result = gg->return_value;
    }
    return result;
}

VALUE
pys_gen_send(CTX *c, VALUE gen_v, VALUE v)
{
    struct pysgen *g = PYS_PTR(gen_v)->gen;
    if (!g->started && v != PYS_NONE) {
        PYS_RAISE_EXC(c, c->EXC_TypeError,
                     "can't send non-None value to a just-started generator");
        return PYS_NONE;
    }
    g->send_value = v;
    return pys_gen_next(c, gen_v);
}

VALUE
pys_gen_throw(CTX *c, VALUE gen_v, VALUE exc)
{
    // Materialise a class into an instance, invoking the class as a
    // constructor so .args/.message etc. get properly initialised.
    if (pys_is_class(exc)) {
        VALUE inst = pys_apply(c, exc, 0, NULL);
        if (UNLIKELY(!inst)) return PYS_NONE;
        exc = inst;
    }
    struct pysgen *g = PYS_PTR(gen_v)->gen;
    if (g->done) {
        c->state = PYS_STATE_RAISE;
        c->state_value = exc;
        return PYS_NONE;
    }
    g->throw_pending = true;
    g->throw_exc = exc;
    if (!g->started) {
        // Throw before first yield — body never gets to run; just raise.
        g->done = true;
        c->state = PYS_STATE_RAISE;
        c->state_value = g->throw_exc;
        return PYS_NONE;
    }
    return pys_gen_next(c, gen_v);
}

VALUE
pys_gen_close(CTX *c, VALUE gen_v)
{
    extern bool class_is_ancestor(VALUE cls, VALUE target);
    struct pysgen *g = PYS_PTR(gen_v)->gen;
    if (g->done) return PYS_NONE;
    if (!g->started) { g->done = true; return PYS_NONE; }
    g->throw_pending = true;
    g->throw_exc = pys_make_instance(c->EXC_GeneratorExit);
    pys_setattr(c, g->throw_exc, "message", pys_make_str("GeneratorExit", 13));
    pys_gen_next(c, gen_v);
    g->done = true;
    // After close: swallow GeneratorExit / StopIteration (expected); but
    // any *other* exception raised by the generator (e.g. from a
    // `finally` block) propagates to the close() caller — matches CPython.
    if (c->state == PYS_STATE_RAISE) {
        VALUE exc = c->state_value;
        if (pys_is_instance(exc)) {
            VALUE ec = PYS_OBJ_VAL(PYS_PTR(exc)->inst.cls);
            if (class_is_ancestor(ec, c->EXC_GeneratorExit)
                || class_is_ancestor(ec, c->EXC_StopIteration)) {
                c->state = PYS_STATE_NORMAL;
                c->state_value = PYS_NONE;
                return PYS_NONE;
            }
        }
        // Other exception — propagate.
        return PYS_NONE;
    }
    return PYS_NONE;
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
pys_pat_match(CTX *c, int pat_idx, VALUE v)
{
    struct pyspat *p = &PYS_PATTERNS[pat_idx];
    switch (p->kind) {
      case PYPAT_WILDCARD:
        return true;
      case PYPAT_LITERAL: {
        VALUE lit = EVAL(c, p->literal);
        if (UNLIKELY(!lit)) return false;
        return pys_eq(c, v, lit) == PYS_TRUE;
      }
      case PYPAT_VALUE: {
        VALUE val = EVAL(c, p->literal);
        if (UNLIKELY(!val)) return false;
        return v == val;     // identity (Python uses == here, close enough)
      }
      case PYPAT_CAPTURE:
        if (p->slot >= 0) c->env->slots[p->slot] = v;
        else              pys_global_set(c, p->name, v);
        return true;
      case PYPAT_OR:
        for (int i = 0; i < p->nchildren; i++)
            if (pys_pat_match(c, p->first_child + i, v)) return true;
        return false;
      case PYPAT_AS: {
        // Inner pattern must match.  If it does, bind the name.
        if (!pys_pat_match(c, p->first_child, v)) return false;
        const char *name = p->attrs[0];
        // Look up local slot for `name`.  Slot index isn't pre-stored in
        // PYPAT_AS so resolve via env walk: walk frame's locals checking
        // names...  Simpler: try pys_global_set as fallback.  Actually
        // the parser already registered name as a local of cur_scope, so
        // we need its slot index.
        // In runtime, c->env points to the executing frame.  We don't
        // have direct access to "slot for name X", so use a global set.
        // Wait — PYPAT_CAPTURE works via p->slot.  Let me reuse that.
        // PYPAT_AS lives alongside; encode the slot in the same field.
        if (p->slot >= 0) c->env->slots[p->slot] = v;
        else              pys_global_set(c, name, v);
        return true;
      }
      case PYPAT_SEQUENCE: {
        if (!(pys_is_list(v) || pys_is_tuple(v))) return false;
        // Locate any PYPAT_STAR within the children.
        int star_idx = -1;
        for (int i = 0; i < p->nchildren; i++)
            if (PYS_PATTERNS[p->first_child + i].kind == PYPAT_STAR) {
                if (star_idx >= 0) return false;  // only one star allowed
                star_idx = i;
            }
        size_t len = PYS_PTR(v)->list.len;
        if (star_idx < 0) {
            if ((int)len != p->nchildren) return false;
            for (int i = 0; i < p->nchildren; i++)
                if (!pys_pat_match(c, p->first_child + i, PYS_PTR(v)->list.items[i]))
                    return false;
            return true;
        }
        // With star: prefix is star_idx items; suffix is (nchildren-1-star_idx) items.
        int prefix = star_idx;
        int suffix = p->nchildren - star_idx - 1;
        if ((int)len < prefix + suffix) return false;
        for (int i = 0; i < prefix; i++)
            if (!pys_pat_match(c, p->first_child + i, PYS_PTR(v)->list.items[i]))
                return false;
        for (int i = 0; i < suffix; i++)
            if (!pys_pat_match(c, p->first_child + star_idx + 1 + i,
                              PYS_PTR(v)->list.items[len - suffix + i]))
                return false;
        // Bind the star to the rest as a list.
        struct pyspat *sp = &PYS_PATTERNS[p->first_child + star_idx];
        size_t mid_len = len - prefix - suffix;
        VALUE rest = pys_make_list(PYS_PTR(v)->list.items + prefix, mid_len);
        if (sp->slot >= 0) c->env->slots[sp->slot] = rest;
        else if (sp->name) pys_global_set(c, sp->name, rest);
        return true;
      }
      case PYPAT_STAR:
        // Standalone (not inside SEQUENCE) — treat as wildcard.
        return true;
      case PYPAT_CLASS: {
        VALUE cls = EVAL(c, p->literal);
        if (UNLIKELY(!cls)) return false;
        if (!pys_is_class(cls)) return false;
        // Built-in type pattern (int, str, float, list, ...): match by type tag.
        extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
        VALUE av[1] = { v };
        VALUE actual_cls = bi_type(c, 1, av);
        if (UNLIKELY(!actual_cls)) return false;
        if (actual_cls == cls) return true;
        if (pys_is_class(actual_cls))
            return class_is_ancestor(actual_cls, cls);
        return false;
      }
      case PYPAT_CLASS_ARGS: {
        VALUE cls = EVAL(c, p->literal);
        if (UNLIKELY(!cls)) return false;
        if (!pys_is_class(cls)) return false;
        if (!pys_is_instance(v)) return false;
        if (!class_is_ancestor(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), cls)) return false;
        // Positional sub-patterns (attrs[i] == NULL) need __match_args__
        // resolved against the matched class to map index → attr name.
        VALUE match_args = pys_class_lookup_method(cls, "__match_args__");
        for (int i = 0; i < p->nchildren; i++) {
            const char *attr_name = p->attrs[i];
            if (attr_name == NULL) {
                // Positional → look up __match_args__[i].
                if (!pys_is_tuple(match_args) && !pys_is_list(match_args))
                    return false;
                if ((size_t)i >= PYS_PTR(match_args)->list.len) return false;
                VALUE n = PYS_PTR(match_args)->list.items[i];
                if (!pys_is_str(n)) return false;
                attr_name = PYS_PTR(n)->str.chars;
            }
            // Read attribute via the dict (avoids dunder fall-throughs
            // which could fail).  If the attr is missing, no match.
            struct pysobj *o = PYS_PTR(v);
            VALUE attr_val = (VALUE)0;
            if (o->inst.attrs) {
                VALUE k = pys_make_str(attr_name, strlen(attr_name));
                uint64_t h = pys_hash(c, k);
                int32_t eidx = pydict_find(c, o->inst.attrs, k, h);
                if (eidx >= 0) attr_val = o->inst.attrs->entries[eidx].value;
            }
            if (!attr_val) return false;
            if (!pys_pat_match(c, p->first_child + i, attr_val)) return false;
        }
        return true;
      }
      case PYPAT_MAPPING: {
        if (!pys_is_dict(v)) return false;
        for (int i = 0; i < p->nchildren; i++) {
            VALUE key = EVAL(c, p->keys[i]);
            if (UNLIKELY(!key)) return false;
            if (!pys_dict_has(c, v, key)) return false;
            VALUE val = pys_dict_get(c, v, key);
            if (UNLIKELY(!val)) return false;
            if (!pys_pat_match(c, p->first_child + i, val)) return false;
        }
        return true;
      }
    }
    return false;
}

// ---------------------------------------------------------------------------
// PEP 654 helper: split an ExceptionGroup `eg` by `type` predicate.
// Returns the matched and unmatched subgroups in *matched / *unmatched.
// Either may be set to PYS_NONE if empty.  An exception that is itself
// the type matches; nested ExceptionGroups are recursed.
static VALUE
pys_eg_make(CTX *c, const char *msg, VALUE excs)
{
    VALUE av[2] = {
        pys_make_str(msg, strlen(msg)),
        excs,
    };
    // pys_eg_make is called from inside the try-catch path where
    // c->state == RAISE.  pys_apply early-exits when state==RAISE, so
    // temporarily clear and restore.
    int sst = c->state; VALUE sv = c->state_value;
    c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
    VALUE r = pys_apply(c, c->EXC_ExceptionGroup, 2, av);
    if (c->state == PYS_STATE_NORMAL) { c->state = sst; c->state_value = sv; }
    return r;
}

static void
pys_eg_split(CTX *c, VALUE eg, VALUE type, VALUE *matched_out, VALUE *unmatched_out)
{
    *matched_out = PYS_NONE;
    *unmatched_out = PYS_NONE;
    if (!pys_is_instance(eg)) return;
    if (!class_is_ancestor(PYS_OBJ_VAL(PYS_PTR(eg)->inst.cls), c->EXC_BaseExceptionGroup))
        return;
    int sst = c->state; VALUE sval = c->state_value;
    c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
    VALUE excs = pys_getattr(c, eg, "exceptions");
    c->state = sst; c->state_value = sval;
    if (!pys_is_list(excs) && !pys_is_tuple(excs)) return;
    VALUE matched_list = pys_make_list(NULL, 0);
    VALUE unmatched_list = pys_make_list(NULL, 0);
    size_t n = PYS_PTR(excs)->list.len;
    for (size_t i = 0; i < n; i++) {
        VALUE e = PYS_PTR(excs)->list.items[i];
        // Nested group: recurse.
        if (pys_is_instance(e)
                && class_is_ancestor(PYS_OBJ_VAL(PYS_PTR(e)->inst.cls), c->EXC_BaseExceptionGroup)) {
            VALUE sub_m, sub_u;
            pys_eg_split(c, e, type, &sub_m, &sub_u);
            if (sub_m != PYS_NONE) pys_list_append(c, matched_list, sub_m);
            if (sub_u != PYS_NONE) pys_list_append(c, unmatched_list, sub_u);
            continue;
        }
        if (pys_exc_matches(c, e, type)) {
            pys_list_append(c, matched_list, e);
        } else {
            pys_list_append(c, unmatched_list, e);
        }
    }
    // Read message arg (first arg) if available.
    int sst2 = c->state; VALUE sval2 = c->state_value;
    c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
    VALUE msg_v = pys_getattr(c, eg, "message");
    c->state = sst2; c->state_value = sval2;
    const char *msg = (pys_is_str(msg_v)) ? PYS_PTR(msg_v)->str.chars : "";
    if (PYS_PTR(matched_list)->list.len > 0) {
        *matched_out = pys_eg_make(c, msg, matched_list);
    }
    if (PYS_PTR(unmatched_list)->list.len > 0) {
        *unmatched_out = pys_eg_make(c, msg, unmatched_list);
    }
}

// try/except/finally driver.  Sits behind `node_try` so the setjmp lives
// in a function the C compiler doesn't try to inline (EVAL_node_try is
// generated with always_inline by ASTroGen).
// ---------------------------------------------------------------------------

VALUE
pys_run_try(CTX *c, NODE *body, uint32_t handlers_idx, uint32_t nhandlers, NODE *else_body, NODE *finally_body)
{
    // State-based: pys_apply/etc. restore env on return, so no manual
    // save/restore needed.  We just observe c->state after the body.
    EVAL(c, body);
    bool caught_raise = (c->state == PYS_STATE_RAISE);

    if (caught_raise) {
        VALUE exc = c->state_value;
        // Detect except* (PEP 654) handlers — if any present, take a
        // separate path that splits ExceptionGroup by type.
        bool has_star = false;
        for (uint32_t i = 0; i < nhandlers; i++)
            if (PYS_HANDLERS[handlers_idx + i].is_star) { has_star = true; break; }
        if (has_star) {
            // CPython: `except* T` only catches BaseExceptionGroup.  A
            // bare exception falls through to be re-raised.
            if (!pys_is_instance(exc)
                    || !class_is_ancestor(PYS_OBJ_VAL(PYS_PTR(exc)->inst.cls),
                                          c->EXC_BaseExceptionGroup)) {
                c->state = PYS_STATE_RAISE;
                c->state_value = exc;
                goto run_finally;
            }
            VALUE remaining = exc;
            bool any_raised = false;
            VALUE last_raised = PYS_NONE;
            for (uint32_t i = 0; i < nhandlers; i++) {
                struct pyshandler *h = &PYS_HANDLERS[handlers_idx + i];
                if (remaining == PYS_NONE) break;
                VALUE cls_val = PYS_NONE;
                if (h->exc_class) {
                    int sst = c->state; VALUE sval = c->state_value;
                    c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
                    cls_val = EVAL(c, h->exc_class);
                    if (c->state != PYS_STATE_NORMAL) goto run_finally;
                    c->state = sst; c->state_value = sval;
                }
                VALUE matched = PYS_NONE, unmatched = PYS_NONE;
                if (h->exc_class) {
                    pys_eg_split(c, remaining, cls_val, &matched, &unmatched);
                } else {
                    matched = remaining;
                }
                if (matched != PYS_NONE) {
                    c->state = PYS_STATE_NORMAL;
                    c->state_value = matched;
                    VALUE saved_handling = c->current_handling_exc;
                    c->current_handling_exc = matched;
                    if (h->name) {
                        if (h->name_is_global) pys_global_set(c, h->name, matched);
                        else                   c->env->slots[h->name_slot] = matched;
                    }
                    EVAL(c, h->body);
                    c->current_handling_exc = saved_handling;
                    if (c->state == PYS_STATE_RAISE) {
                        any_raised = true;
                        last_raised = c->state_value;
                    } else if (c->state == PYS_STATE_NORMAL) {
                        c->state_value = PYS_NONE;
                    }
                }
                remaining = unmatched;
            }
            // After all star handlers: if any handler raised, that
            // exception propagates.  Otherwise re-raise leftover (if any).
            if (any_raised) {
                c->state = PYS_STATE_RAISE;
                c->state_value = last_raised;
            } else if (remaining != PYS_NONE) {
                c->state = PYS_STATE_RAISE;
                c->state_value = remaining;
            } else {
                c->state = PYS_STATE_NORMAL;
                c->state_value = PYS_NONE;
            }
            goto run_finally;
        }
        for (uint32_t i = 0; i < nhandlers; i++) {
            struct pyshandler *h = &PYS_HANDLERS[handlers_idx + i];
            VALUE cls_val = PYS_NONE;
            if (h->exc_class) {
                int sst = c->state; VALUE sval = c->state_value;
                c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
                cls_val = EVAL(c, h->exc_class);
                if (c->state != PYS_STATE_NORMAL) goto run_finally;
                c->state = sst; c->state_value = sval;
            }
            if (!h->exc_class || pys_exc_matches(c, exc, cls_val)) {
                c->state = PYS_STATE_NORMAL;
                // Keep `exc` in state_value so a bare `raise` inside
                // the handler can re-raise the active exception.
                c->state_value = exc;
                VALUE saved_handling = c->current_handling_exc;
                c->current_handling_exc = exc;
                if (h->name) {
                    if (h->name_is_global) pys_global_set(c, h->name, exc);
                    else                   c->env->slots[h->name_slot] = exc;
                }
                EVAL(c, h->body);
                c->current_handling_exc = saved_handling;
                // If the body did not itself raise, clear the active
                // exception so it doesn't leak past the handler.
                if (c->state == PYS_STATE_NORMAL) c->state_value = PYS_NONE;
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
        c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
        EVAL(c, finally_body);
        if (c->state == PYS_STATE_NORMAL) { c->state = sst; c->state_value = sval; }
    }
    return c->state == PYS_STATE_NORMAL ? PYS_NONE : 0;
}

// `with EXPR as NAME: body` runner.  `cm` is the already-evaluated
// context manager (its __enter__ has already been called by the desugar).
// Runs `body` with proper exception protocol: on raise, calls
// `cm.__exit__(type, value, None)`; if it returns truthy the exception
// is suppressed.  On normal exit, calls `cm.__exit__(None, None, None)`.
VALUE
pys_run_with(CTX *c, VALUE cm, NODE *body)
{
    EVAL(c, body);
    bool caught = (c->state == PYS_STATE_RAISE);

    if (caught) {
        VALUE exc = c->state_value;
        VALUE etype = pys_is_instance(exc) ? PYS_OBJ_VAL(PYS_PTR(exc)->inst.cls) : PYS_NONE;
        VALUE av[3] = { etype, exc, PYS_NONE };
        c->state = PYS_STATE_NORMAL;
        c->state_value = PYS_NONE;
        VALUE exit_m = pys_getattr(c, cm, "__exit__");
        if (UNLIKELY(!exit_m)) return 0;
        VALUE r = pys_apply(c, exit_m, 3, av);
        if (c->state == PYS_STATE_RAISE) {
            // __exit__ raised — set the new exc's __context__ to the
            // original (CPython chains exceptions this way).  Clear
            // RAISE first so pys_setattr's internal method lookups /
            // applies don't bail out early; restore it afterwards.
            VALUE new_exc = c->state_value;
            if (pys_is_instance(new_exc) && new_exc != exc) {
                c->state = PYS_STATE_NORMAL;
                c->state_value = PYS_NONE;
                pys_setattr(c, new_exc, "__context__", exc);
                c->state = PYS_STATE_RAISE;
                c->state_value = new_exc;
            }
            return 0;
        }
        if (c->state != PYS_STATE_NORMAL) return 0;
        if (!pys_is_truthy(r)) {
            // Re-raise the original exception.
            c->state = PYS_STATE_RAISE;
            c->state_value = exc;
            return 0;
        }
    } else {
        // Body completed normally or executed `return X` / `break` /
        // `continue` — preserve that state across the __exit__ call so
        // the caller sees the return / loop-control verbatim.  When
        // restoring a non-NORMAL state we also return 0 so that
        // node_seq's `if (!eval(first)) return 0` short-circuit kicks
        // in and the caller's state inspection runs.
        int saved_state = c->state;
        VALUE saved_value = c->state_value;
        c->state = PYS_STATE_NORMAL;
        c->state_value = PYS_NONE;
        VALUE av[3] = { PYS_NONE, PYS_NONE, PYS_NONE };
        VALUE exit_m = pys_getattr(c, cm, "__exit__");
        if (UNLIKELY(!exit_m)) return 0;
        pys_apply(c, exit_m, 3, av);
        if (c->state != PYS_STATE_NORMAL) return 0;
        if (saved_state != PYS_STATE_NORMAL) {
            c->state = saved_state;
            c->state_value = saved_value;
            return 0;
        }
    }
    return PYS_NONE;
}

// ---------------------------------------------------------------------------
// Tuple-unpacking assignment (struct pyunpack_target in context.h).
// ---------------------------------------------------------------------------

void
pys_unpack_assign(CTX *c, struct pyunpack_target *targets, uint32_t n, VALUE rhs)
{
    // Materialise rhs into an array.
    VALUE *items = NULL;
    size_t nitems = 0;
    if (pys_is_list(rhs) || pys_is_tuple(rhs)) {
        items = PYS_PTR(rhs)->list.items;
        nitems = PYS_PTR(rhs)->list.len;
    } else {
        // any iterable
        struct pys_iter it; pys_iter_init(c, &it, rhs);
        if (c->state != PYS_STATE_NORMAL) return;
        size_t cap = 8; nitems = 0;
        items = (VALUE *)GC_malloc(sizeof(VALUE) * cap);
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
            if (nitems == cap) { cap *= 2; items = (VALUE *)GC_realloc(items, sizeof(VALUE) * cap); }
            items[nitems++] = x;
        }
    }
    // Find a starred target if any (at most one allowed).
    int star_idx = -1;
    for (uint32_t i = 0; i < n; i++) if (targets[i].is_starred) { star_idx = (int)i; break; }
    if (star_idx < 0) {
        if (nitems != n)
            PYS_RAISE_EXC(c, c->EXC_ValueError,
                         "expected %u values, got %zu", n, nitems);
        for (uint32_t i = 0; i < n; i++) {
            VALUE v = items[i];
            if (targets[i].is_local) c->env->slots[targets[i].slot] = v;
            else                     pys_global_set(c, targets[i].global_name, v);
        }
        return;
    }
    // Starred form: prefix [0..star_idx), starred [star_idx..len-suffix],
    // suffix [len-suffix..len) where suffix length = n - star_idx - 1.
    int n_pre = star_idx;
    int n_suf = (int)n - star_idx - 1;
    if ((int)nitems < n_pre + n_suf)
        PYS_RAISE_EXC(c, c->EXC_ValueError,
                     "not enough values to unpack (expected at least %d)", n_pre + n_suf);
    // Pre.
    for (int i = 0; i < n_pre; i++) {
        VALUE v = items[i];
        if (targets[i].is_local) c->env->slots[targets[i].slot] = v;
        else                     pys_global_set(c, targets[i].global_name, v);
    }
    // Starred = list of middle items.
    int rest_len = (int)nitems - n_pre - n_suf;
    VALUE *rest_items = rest_len > 0 ? &items[n_pre] : NULL;
    VALUE rest = pys_make_list(rest_items, (size_t)rest_len);
    if (targets[star_idx].is_local) c->env->slots[targets[star_idx].slot] = rest;
    else                            pys_global_set(c, targets[star_idx].global_name, rest);
    // Suffix.
    for (int j = 0; j < n_suf; j++) {
        VALUE v = items[nitems - n_suf + j];
        struct pyunpack_target *t = &targets[star_idx + 1 + j];
        if (t->is_local) c->env->slots[t->slot] = v;
        else             pys_global_set(c, t->global_name, v);
    }
}

// ---------------------------------------------------------------------------
// Built-in type methods.
// ---------------------------------------------------------------------------

static VALUE
sm_split(CTX *c, int argc, VALUE *argv)
{
    VALUE self = argv[0];
    if (!pys_is_str(self)) PYS_RAISE_EXC(c, c->EXC_TypeError, "split: not str");
    const char *s = PYS_PTR(self)->str.chars;
    size_t len = PYS_PTR(self)->str.len;
    VALUE result = pys_make_list(NULL, 0);
    // sep=None or absent → split on whitespace runs (CPython behaviour).
    if (argc == 1 || argv[1] == PYS_NONE) {
        int64_t maxsplit = (argc >= 3) ? pys_int_to_long(c, argv[2]) : -1;
        int64_t splits = 0;
        size_t i = 0;
        while (i < len) {
            while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            if (i >= len) break;
            if (maxsplit >= 0 && splits >= maxsplit) {
                pys_list_append(c, result, pys_make_str_borrow(s + i, len - i));
                return result;
            }
            size_t j = i;
            while (j < len && !(s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
            pys_list_append(c, result, pys_make_str_borrow(s + i, j - i));
            splits++;
            i = j;
        }
        return result;
    }
    if (!pys_is_str(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "split sep must be str");
    const char *sep = PYS_PTR(argv[1])->str.chars;
    size_t slen = PYS_PTR(argv[1])->str.len;
    if (slen == 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "empty separator");
    int64_t maxsplit = (argc >= 3) ? pys_int_to_long(c, argv[2]) : -1;
    size_t i = 0;
    int64_t splits = 0;
    while (i <= len) {
        if (maxsplit >= 0 && splits >= maxsplit) {
            pys_list_append(c, result, pys_make_str_borrow(s + i, len - i));
            break;
        }
        const char *p = i + slen <= len ? memmem(s + i, len - i, sep, slen) : NULL;
        if (!p) { pys_list_append(c, result, pys_make_str_borrow(s + i, len - i)); break; }
        pys_list_append(c, result, pys_make_str_borrow(s + i, (size_t)(p - (s + i))));
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
    const char *sep = PYS_PTR(self)->str.chars;
    size_t slen = PYS_PTR(self)->str.len;
    // Materialise iterable into a (possibly heap-allocated) list of strings.
    VALUE  fixed[64];
    VALUE *items = fixed;
    size_t cap = 64;
    int n = 0;
    if (pys_is_list(seq) || pys_is_tuple(seq)) {
        size_t sn = PYS_PTR(seq)->list.len;
        if (sn > cap) {
            cap = sn;
            items = (VALUE *)GC_malloc(sizeof(VALUE) * cap);
        }
        for (size_t i = 0; i < sn; i++) items[n++] = PYS_PTR(seq)->list.items[i];
    } else {
        struct pys_iter it; pys_iter_init(c, &it, seq);
        if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
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
        if (!pys_is_str(e)) PYS_RAISE_EXC(c, c->EXC_TypeError, "join element must be str");
        total += PYS_PTR(e)->str.len;
        if (i) total += slen;
    }
    char *buf = (char *)GC_malloc_atomic(total + 1);
    char *p = buf;
    for (int i = 0; i < n; i++) {
        if (i) { memcpy(p, sep, slen); p += slen; }
        VALUE e = items[i];
        memcpy(p, PYS_PTR(e)->str.chars, PYS_PTR(e)->str.len);
        p += PYS_PTR(e)->str.len;
    }
    *p = '\0';
    return pys_make_str_take(buf, total);
}

static VALUE
sm_upper(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) buf[i] = (char)toupper((unsigned char)o->str.chars[i]);
    buf[o->str.len] = '\0';
    return pys_make_str_take(buf, o->str.len);
}

static VALUE
sm_lower(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) buf[i] = (char)tolower((unsigned char)o->str.chars[i]);
    buf[o->str.len] = '\0';
    return pys_make_str_take(buf, o->str.len);
}

// Helper: byte length of UTF-8 codepoint at s[i].  Returns 1 for invalid.
static inline int pys_utf8_step(const char *s, size_t i) {
    unsigned char b = (unsigned char)s[i];
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}
// Helper: byte length of the UTF-8 codepoint that ends at byte j-1 (i.e.
// the last codepoint in s[..j]).  j must be > 0.
static inline int pys_utf8_back_step(const char *s, size_t j) {
    // Walk back over continuation bytes (10xxxxxx).
    size_t k = j - 1;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    return (int)(j - k);
}
// True if codepoint at cps[ci..ci+nbytes] appears as a codepoint in strip
// set.  We compare by exact byte match, since UTF-8 is canonical here.
static bool
pys_strip_set_contains(const char *set, size_t setlen,
                      const char *cp, int cp_bytes)
{
    for (size_t k = 0; k < setlen; ) {
        int s_bytes = pys_utf8_step(set, k);
        if (s_bytes == cp_bytes &&
            memcmp(set + k, cp, (size_t)cp_bytes) == 0) return true;
        k += (size_t)s_bytes;
    }
    return false;
}

static VALUE
sm_strip(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *o = PYS_PTR(argv[0]);
    size_t i = 0, j = o->str.len;
    const char *s = o->str.chars;
    if (argc >= 2 && pys_is_str(argv[1])) {
        const char *cs = PYS_PTR(argv[1])->str.chars;
        size_t cn = PYS_PTR(argv[1])->str.len;
        // Forward: walk codepoints; stop when not in set.
        while (i < j) {
            int nb = pys_utf8_step(s, i);
            if (!pys_strip_set_contains(cs, cn, s + i, nb)) break;
            i += (size_t)nb;
        }
        // Backward: walk codepoints from end.
        while (j > i) {
            int nb = pys_utf8_back_step(s, j);
            if (!pys_strip_set_contains(cs, cn, s + j - nb, nb)) break;
            j -= (size_t)nb;
        }
    } else {
        while (i < j && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        while (j > i && (s[j-1] == ' ' || s[j-1] == '\t' || s[j-1] == '\n' || s[j-1] == '\r')) j--;
    }
    return pys_make_str(o->str.chars + i, j - i);
}

static VALUE
sm_startswith(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *s = PYS_PTR(argv[0]);
    VALUE arg = argv[1];
    int64_t cp_len = (int64_t)pys_str_cp_count(s->str.chars, s->str.len);
    int64_t cp_start = 0, cp_end = cp_len;
    if (argc >= 3 && argv[2] != PYS_NONE) cp_start = pys_int_to_long(c, argv[2]);
    if (argc >= 4 && argv[3] != PYS_NONE) cp_end = pys_int_to_long(c, argv[3]);
    { if (cp_start < 0) cp_start += cp_len; if (cp_start < 0) cp_start = 0; if (cp_start > cp_len) cp_start = cp_len; }
    { if (cp_end < 0) cp_end += cp_len; if (cp_end < 0) cp_end = 0; if (cp_end > cp_len) cp_end = cp_len; }
    int64_t bstart = (int64_t)pys_str_cp_to_byte(s->str.chars, s->str.len, cp_start);
    int64_t bend = (int64_t)pys_str_cp_to_byte(s->str.chars, s->str.len, cp_end);
    int64_t span = bend - bstart;
    if (span < 0) span = 0;
    const char *base = s->str.chars + bstart;
    if (pys_is_tuple(arg)) {
        size_t n = PYS_PTR(arg)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE p = PYS_PTR(arg)->list.items[i];
            if (!pys_is_str(p)) continue;
            struct pysobj *pp = PYS_PTR(p);
            if ((int64_t)pp->str.len > span) continue;
            if (memcmp(base, pp->str.chars, pp->str.len) == 0) return PYS_TRUE;
        }
        return PYS_FALSE;
    }
    if (!pys_is_str(arg)) PYS_RAISE_EXC(c, c->EXC_TypeError, "startswith: not str/tuple");
    struct pysobj *p = PYS_PTR(arg);
    if ((int64_t)p->str.len > span) return PYS_FALSE;
    return memcmp(base, p->str.chars, p->str.len) == 0 ? PYS_TRUE : PYS_FALSE;
}

static VALUE
sm_endswith(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *s = PYS_PTR(argv[0]);
    VALUE arg = argv[1];
    int64_t cp_len = (int64_t)pys_str_cp_count(s->str.chars, s->str.len);
    int64_t cp_start = 0, cp_end = cp_len;
    if (argc >= 3 && argv[2] != PYS_NONE) cp_start = pys_int_to_long(c, argv[2]);
    if (argc >= 4 && argv[3] != PYS_NONE) cp_end = pys_int_to_long(c, argv[3]);
    { if (cp_start < 0) cp_start += cp_len; if (cp_start < 0) cp_start = 0; if (cp_start > cp_len) cp_start = cp_len; }
    { if (cp_end < 0) cp_end += cp_len; if (cp_end < 0) cp_end = 0; if (cp_end > cp_len) cp_end = cp_len; }
    int64_t bstart = (int64_t)pys_str_cp_to_byte(s->str.chars, s->str.len, cp_start);
    int64_t bend = (int64_t)pys_str_cp_to_byte(s->str.chars, s->str.len, cp_end);
    int64_t span = bend - bstart;
    if (span < 0) span = 0;
    const char *tail_end = s->str.chars + bend;
    if (pys_is_tuple(arg)) {
        size_t n = PYS_PTR(arg)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE p = PYS_PTR(arg)->list.items[i];
            if (!pys_is_str(p)) continue;
            struct pysobj *pp = PYS_PTR(p);
            if ((int64_t)pp->str.len > span) continue;
            if (memcmp(tail_end - pp->str.len, pp->str.chars, pp->str.len) == 0) return PYS_TRUE;
        }
        return PYS_FALSE;
    }
    if (!pys_is_str(arg)) PYS_RAISE_EXC(c, c->EXC_TypeError, "endswith: not str/tuple");
    struct pysobj *p = PYS_PTR(arg);
    if ((int64_t)p->str.len > span) return PYS_FALSE;
    return memcmp(tail_end - p->str.len, p->str.chars, p->str.len) == 0 ? PYS_TRUE : PYS_FALSE;
}

VALUE
sm_find(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *s = PYS_PTR(argv[0]);
    if (!pys_is_str(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "find: not str");
    struct pysobj *p = PYS_PTR(argv[1]);
    // start / end are codepoint indices; convert to byte offsets.
    int64_t cp_len = (int64_t)pys_str_cp_count(s->str.chars, s->str.len);
    int64_t cp_start = (argc >= 3) ? pys_int_to_long(c, argv[2]) : 0;
    int64_t cp_end   = (argc >= 4) ? pys_int_to_long(c, argv[3]) : cp_len;
    if (cp_start < 0) cp_start += cp_len;
    if (cp_start < 0) cp_start = 0;
    if (cp_end < 0) cp_end += cp_len;
    if (cp_end > cp_len) cp_end = cp_len;
    if (cp_start > cp_end) return PYS_FIX(-1);
    size_t boff = pys_str_cp_to_byte(s->str.chars, s->str.len, cp_start);
    size_t eoff = pys_str_cp_to_byte(s->str.chars, s->str.len, cp_end);
    if (p->str.len == 0) return PYS_FIX(cp_start);
    if (eoff - boff < p->str.len) return PYS_FIX(-1);
    void *r = memmem(s->str.chars + boff, eoff - boff,
                     p->str.chars, p->str.len);
    if (!r) return PYS_FIX(-1);
    // Convert byte offset back to codepoint index.
    return PYS_FIX((int64_t)pys_str_byte_to_cp(s->str.chars,
                                              (size_t)((char *)r - s->str.chars)));
}

static VALUE
sm_replace(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *s = PYS_PTR(argv[0]);
    if (!pys_is_str(argv[1]) || !pys_is_str(argv[2]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "replace args must be str");
    struct pysobj *o = PYS_PTR(argv[1]);
    struct pysobj *n = PYS_PTR(argv[2]);
    int64_t max_count = (argc >= 4) ? pys_int_to_long(c, argv[3]) : -1;
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
    return pys_make_str_take(buf, len);
}

static VALUE
sm_count(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *s = PYS_PTR(argv[0]);
    if (!pys_is_str(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "count: not str");
    struct pysobj *p = PYS_PTR(argv[1]);
    // start / end are codepoint indices.
    int64_t cp_len = (int64_t)pys_str_cp_count(s->str.chars, s->str.len);
    int64_t cp_start = 0, cp_end = cp_len;
    if (argc >= 3 && argv[2] != PYS_NONE) cp_start = pys_int_to_long(c, argv[2]);
    if (argc >= 4 && argv[3] != PYS_NONE) cp_end = pys_int_to_long(c, argv[3]);
    { if (cp_start < 0) cp_start += cp_len; if (cp_start < 0) cp_start = 0; if (cp_start > cp_len) cp_start = cp_len; }
    { if (cp_end < 0) cp_end += cp_len; if (cp_end < 0) cp_end = 0; if (cp_end > cp_len) cp_end = cp_len; }
    int64_t boff = (int64_t)pys_str_cp_to_byte(s->str.chars, s->str.len, cp_start);
    int64_t eoff = (int64_t)pys_str_cp_to_byte(s->str.chars, s->str.len, cp_end);
    if (p->str.len == 0) return PYS_FIX(cp_end - cp_start + 1);
    int64_t n = 0;
    int64_t i = boff;
    while (i + (int64_t)p->str.len <= eoff) {
        if (memcmp(s->str.chars + i, p->str.chars, p->str.len) == 0) { n++; i += (int64_t)p->str.len; }
        else i++;
    }
    return PYS_FIX(n);
}

static VALUE
sm_encode(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    return pys_make_bytes(o->str.chars, o->str.len);
}

static VALUE bi_format(CTX *c, int argc, VALUE *argv);  // forward
// `"{} {}".format(a, b)` — auto / positional indices + format spec.
// Kwargs (`{name}`) are not supported in this v0; use f-strings instead.
static VALUE
sm_format(CTX *c, int argc, VALUE *argv)
{
    VALUE self = argv[0];
    const char *src = PYS_PTR(self)->str.chars;
    size_t srclen = PYS_PTR(self)->str.len;
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
            if (j >= srclen) PYS_RAISE_EXC(c, c->EXC_ValueError, "unterminated '{' in format");
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
                extern int    PYS_BI_KWC;
                extern const char **PYS_BI_KWNAMES;
                extern VALUE *PYS_BI_KWVALUES;
                int found = -1;
                for (int ki = 0; ki < PYS_BI_KWC; ki++) {
                    const char *nm = PYS_BI_KWNAMES[ki];
                    if (strlen(nm) == nm_len && memcmp(nm, body + nm_start, nm_len) == 0) {
                        found = ki; break;
                    }
                }
                if (found < 0) PYS_RAISE_EXC(c, c->EXC_KeyError, "format: missing kwarg");
                val = PYS_BI_KWVALUES[found];
                goto have_val;
            }
            if (idx < 0 || idx + 1 > argc - 1)
                PYS_RAISE_EXC(c, c->EXC_IndexError, "format: index out of range");
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
                    val = pys_getattr(c, val, nm);
                    if (c->state == PYS_STATE_RAISE) return 0;
                } else {
                    k++;  // [
                    size_t s = k;
                    while (k < bn && body[k] != ']') k++;
                    if (k >= bn) PYS_RAISE_EXC(c, c->EXC_ValueError, "unmatched '[' in format");
                    // Try integer first, else string key.
                    bool is_int = true;
                    int64_t iv = 0;
                    for (size_t p = s; p < k; p++) {
                        if (body[p] < '0' || body[p] > '9') { is_int = false; break; }
                        iv = iv * 10 + (body[p] - '0');
                    }
                    if (is_int && k > s) {
                        val = pys_list_get(c, val, PYS_FIX(iv));
                    } else {
                        VALUE key = pys_make_str(body + s, k - s);
                        val = pys_list_get(c, val, key);
                    }
                    if (c->state == PYS_STATE_RAISE) return 0;
                    k++;  // ]
                }
            }
            // Optional `!conv`.
            if (k < bn && body[k] == '!') {
                k++;
                if (k >= bn) PYS_RAISE_EXC(c, c->EXC_ValueError, "expected conversion");
                char conv = body[k++];
                if (conv == 'r' || conv == 'a') val = pys_to_repr(c, val);
                else if (conv == 's')           val = pys_to_str(c, val);
                else PYS_RAISE_EXC(c, c->EXC_ValueError, "bad conversion");
            }
            // Optional `:spec`.
            VALUE spec_val = pys_make_str("", 0);
            if (k < bn && body[k] == ':') {
                k++;
                spec_val = pys_make_str(body + k, bn - k);
            }
            // Apply.
            VALUE av[2] = { val, spec_val };
            VALUE rendered = bi_format(c, 2, av);
            if (!pys_is_str(rendered)) rendered = pys_to_str(c, rendered);
            OUT_PUT(PYS_PTR(rendered)->str.chars, PYS_PTR(rendered)->str.len);
            i = j;
        } else if (ch == '}') {
            if (i + 1 < srclen && src[i+1] == '}') { OUT_CH('}'); i++; continue; }
            PYS_RAISE_EXC(c, c->EXC_ValueError, "single '}' in format");
        } else {
            OUT_CH(ch);
        }
    }
    return pys_make_str(out, out_len);
#undef OUT_CH
#undef OUT_PUT
}

static VALUE
sm_lstrip(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *o = PYS_PTR(argv[0]);
    size_t i = 0, j = o->str.len;
    const char *s = o->str.chars;
    if (argc >= 2 && pys_is_str(argv[1])) {
        const char *cs = PYS_PTR(argv[1])->str.chars;
        size_t cn = PYS_PTR(argv[1])->str.len;
        while (i < j) {
            int nb = pys_utf8_step(s, i);
            if (!pys_strip_set_contains(cs, cn, s + i, nb)) break;
            i += (size_t)nb;
        }
    } else {
        while (i < j && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    return pys_make_str(o->str.chars + i, j - i);
}

static VALUE
sm_rstrip(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *o = PYS_PTR(argv[0]);
    size_t i = 0, j = o->str.len;
    const char *s = o->str.chars;
    if (argc >= 2 && pys_is_str(argv[1])) {
        const char *cs = PYS_PTR(argv[1])->str.chars;
        size_t cn = PYS_PTR(argv[1])->str.len;
        while (j > i) {
            int nb = pys_utf8_back_step(s, j);
            if (!pys_strip_set_contains(cs, cn, s + j - nb, nb)) break;
            j -= (size_t)nb;
        }
    } else {
        while (j > i && (s[j-1] == ' ' || s[j-1] == '\t' || s[j-1] == '\n' || s[j-1] == '\r')) j--;
    }
    return pys_make_str(o->str.chars + i, j - i);
}

static VALUE
sm_zfill(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    int w = (int)pys_int_to_long(c, argv[1]);
    int cp_len = (int)pys_str_cp_count(o->str.chars, o->str.len);
    if (cp_len >= w) return pys_make_str(o->str.chars, o->str.len);
    int pad = w - cp_len;
    size_t total = o->str.len + (size_t)pad;
    char *buf = (char *)GC_malloc_atomic(total + 1);
    int off = 0;
    if (o->str.len > 0 && (o->str.chars[0] == '+' || o->str.chars[0] == '-')) {
        buf[0] = o->str.chars[0]; off = 1;
        for (int i = 0; i < pad; i++) buf[off + i] = '0';
        memcpy(buf + off + pad, o->str.chars + 1, o->str.len - 1);
    } else {
        for (int i = 0; i < pad; i++) buf[i] = '0';
        memcpy(buf + pad, o->str.chars, o->str.len);
    }
    buf[total] = '\0';
    return pys_make_str_take(buf, total);
}

// Extract fill codepoint (default ' ') as bytes/length pair.
static inline void
pys_just_fill(int argc, VALUE *argv, const char **fbuf, int *flen)
{
    static const char sp[1] = { ' ' };
    if (argc >= 3 && pys_is_str(argv[2]) && PYS_PTR(argv[2])->str.len >= 1) {
        const char *s = PYS_PTR(argv[2])->str.chars;
        *fbuf = s;
        *flen = pys_utf8_step(s, 0);
    } else {
        *fbuf = sp;
        *flen = 1;
    }
}

static VALUE
sm_center(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    int w = (int)pys_int_to_long(c, argv[1]);
    int cp_len = (int)pys_str_cp_count(o->str.chars, o->str.len);
    if (cp_len >= w) return pys_make_str(o->str.chars, o->str.len);
    const char *fbuf; int flen;
    pys_just_fill(argc, argv, &fbuf, &flen);
    int pad = w - cp_len;
    int left = pad / 2, right = pad - left;
    size_t total = (size_t)(left + right) * (size_t)flen + o->str.len;
    char *buf = (char *)GC_malloc_atomic(total + 1);
    size_t off = 0;
    for (int i = 0; i < left; i++) { memcpy(buf + off, fbuf, (size_t)flen); off += (size_t)flen; }
    memcpy(buf + off, o->str.chars, o->str.len); off += o->str.len;
    for (int i = 0; i < right; i++) { memcpy(buf + off, fbuf, (size_t)flen); off += (size_t)flen; }
    buf[off] = '\0';
    return pys_make_str_take(buf, off);
}

static VALUE
sm_ljust(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    int w = (int)pys_int_to_long(c, argv[1]);
    int cp_len = (int)pys_str_cp_count(o->str.chars, o->str.len);
    if (cp_len >= w) return pys_make_str(o->str.chars, o->str.len);
    const char *fbuf; int flen;
    pys_just_fill(argc, argv, &fbuf, &flen);
    int pad = w - cp_len;
    size_t total = o->str.len + (size_t)pad * (size_t)flen;
    char *buf = (char *)GC_malloc_atomic(total + 1);
    memcpy(buf, o->str.chars, o->str.len);
    size_t off = o->str.len;
    for (int i = 0; i < pad; i++) { memcpy(buf + off, fbuf, (size_t)flen); off += (size_t)flen; }
    buf[off] = '\0';
    return pys_make_str_take(buf, off);
}

static VALUE
sm_rjust(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    int w = (int)pys_int_to_long(c, argv[1]);
    int cp_len = (int)pys_str_cp_count(o->str.chars, o->str.len);
    if (cp_len >= w) return pys_make_str(o->str.chars, o->str.len);
    const char *fbuf; int flen;
    pys_just_fill(argc, argv, &fbuf, &flen);
    int pad = w - cp_len;
    size_t total = (size_t)pad * (size_t)flen + o->str.len;
    char *buf = (char *)GC_malloc_atomic(total + 1);
    size_t off = 0;
    for (int i = 0; i < pad; i++) { memcpy(buf + off, fbuf, (size_t)flen); off += (size_t)flen; }
    memcpy(buf + off, o->str.chars, o->str.len); off += o->str.len;
    buf[off] = '\0';
    return pys_make_str_take(buf, off);
}

static VALUE
sm_title(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
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
    return pys_make_str_take(buf, o->str.len);
}

static VALUE
sm_capitalize(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->str.len == 0) return pys_make_str("", 0);
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
    return pys_make_str_take(buf, o->str.len);
}

static VALUE
sm_rfind(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (!pys_is_str(argv[1])) return PYS_FIX(-1);
    struct pysobj *needle = PYS_PTR(argv[1]);
    int64_t cp_len = (int64_t)pys_str_cp_count(o->str.chars, o->str.len);
    int64_t cp_start = 0, cp_end = cp_len;
    if (argc >= 3 && argv[2] != PYS_NONE) cp_start = pys_int_to_long(c, argv[2]);
    if (argc >= 4 && argv[3] != PYS_NONE) cp_end = pys_int_to_long(c, argv[3]);
    { if (cp_start < 0) cp_start += cp_len; if (cp_start < 0) cp_start = 0; if (cp_start > cp_len) cp_start = cp_len; }
    { if (cp_end < 0) cp_end += cp_len; if (cp_end < 0) cp_end = 0; if (cp_end > cp_len) cp_end = cp_len; }
    size_t boff = pys_str_cp_to_byte(o->str.chars, o->str.len, cp_start);
    size_t eoff = pys_str_cp_to_byte(o->str.chars, o->str.len, cp_end);
    if (needle->str.len == 0) return PYS_FIX(cp_end);
    if (needle->str.len > eoff - boff) return PYS_FIX(-1);
    // Search backwards in the byte range.  Only match at codepoint
    // boundaries — but since our needle is also a UTF-8 string, any byte
    // match starting at a position whose preceding byte is a codepoint
    // start is fine; here we just scan all byte positions and check.
    for (int64_t i = (int64_t)eoff - (int64_t)needle->str.len; i >= (int64_t)boff; i--) {
        if (memcmp(o->str.chars + i, needle->str.chars, needle->str.len) == 0) {
            // Verify i is at a codepoint boundary (byte at i is not a
            // UTF-8 continuation 10xxxxxx).
            unsigned char b = (unsigned char)o->str.chars[i];
            if ((b & 0xC0) == 0x80) continue;
            return PYS_FIX((int64_t)pys_str_byte_to_cp(o->str.chars, (size_t)i));
        }
    }
    return PYS_FIX(-1);
}

static VALUE
sm_rindex(CTX *c, int argc, VALUE *argv)
{
    VALUE r = sm_rfind(c, argc, argv);
    if (PYS_IS_FIXNUM(r) && PYS_FIXVAL(r) == -1)
        PYS_RAISE_EXC(c, c->EXC_ValueError, "substring not found");
    return r;
}

static VALUE
sm_index(CTX *c, int argc, VALUE *argv)
{
    extern VALUE sm_find(CTX *c, int argc, VALUE *argv);
    VALUE r = sm_find(c, argc, argv);
    if (PYS_IS_FIXNUM(r) && PYS_FIXVAL(r) == -1)
        PYS_RAISE_EXC(c, c->EXC_ValueError, "substring not found");
    return r;
}

static VALUE
sm_isnumeric(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->str.len == 0) return PYS_FALSE;
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        if (!(ch >= '0' && ch <= '9')) return PYS_FALSE;
    }
    return PYS_TRUE;
}

static VALUE
sm_isascii(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    for (size_t i = 0; i < o->str.len; i++)
        if ((unsigned char)o->str.chars[i] > 127) return PYS_FALSE;
    return PYS_TRUE;
}

static VALUE
sm_isidentifier(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->str.len == 0) return PYS_FALSE;
    char ch = o->str.chars[0];
    if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
        return PYS_FALSE;
    for (size_t i = 1; i < o->str.len; i++) {
        ch = o->str.chars[i];
        if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9')))
            return PYS_FALSE;
    }
    return PYS_TRUE;
}

static VALUE
sm_isprintable(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    for (size_t i = 0; i < o->str.len; i++) {
        unsigned char ch = (unsigned char)o->str.chars[i];
        if (ch < 32 || ch == 127) return PYS_FALSE;
    }
    return PYS_TRUE;
}

static VALUE
sm_istitle(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    bool seen_alpha = false;
    bool prev_alpha = false;
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        bool is_upper = (ch >= 'A' && ch <= 'Z');
        bool is_lower = (ch >= 'a' && ch <= 'z');
        bool is_alpha = is_upper || is_lower;
        if (is_alpha) seen_alpha = true;
        if (is_upper && prev_alpha) return PYS_FALSE;
        if (is_lower && !prev_alpha) return PYS_FALSE;
        prev_alpha = is_alpha;
    }
    return seen_alpha ? PYS_TRUE : PYS_FALSE;
}

static VALUE
sm_swapcase(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        else if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[o->str.len] = '\0';
    return pys_make_str_take(buf, o->str.len);
}

static VALUE
sm_casefold(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    for (size_t i = 0; i < o->str.len; i++) {
        char ch = o->str.chars[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[o->str.len] = '\0';
    return pys_make_str_take(buf, o->str.len);
}

static VALUE
sm_splitlines(CTX *c, int argc, VALUE *argv)
{
    bool keepends = (argc >= 2) && pys_is_truthy(argv[1]);
    struct pysobj *o = PYS_PTR(argv[0]);
    VALUE r = pys_make_list(NULL, 0);
    size_t i = 0;
    while (i < o->str.len) {
        size_t j = i;
        while (j < o->str.len && o->str.chars[j] != '\n' && o->str.chars[j] != '\r') j++;
        size_t end = j;
        if (j < o->str.len && o->str.chars[j] == '\r' && j + 1 < o->str.len && o->str.chars[j+1] == '\n') j += 2;
        else if (j < o->str.len) j++;
        size_t out_end = keepends ? j : end;
        pys_list_append(c, r, pys_make_str(o->str.chars + i, out_end - i));
        i = j;
    }
    return r;
}

static VALUE
sm_removeprefix(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (!pys_is_str(argv[1])) return pys_make_str(o->str.chars, o->str.len);
    struct pysobj *p = PYS_PTR(argv[1]);
    if (o->str.len >= p->str.len && memcmp(o->str.chars, p->str.chars, p->str.len) == 0)
        return pys_make_str(o->str.chars + p->str.len, o->str.len - p->str.len);
    return pys_make_str(o->str.chars, o->str.len);
}

static VALUE
sm_removesuffix(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (!pys_is_str(argv[1])) return pys_make_str(o->str.chars, o->str.len);
    struct pysobj *p = PYS_PTR(argv[1]);
    if (o->str.len >= p->str.len &&
        memcmp(o->str.chars + o->str.len - p->str.len, p->str.chars, p->str.len) == 0)
        return pys_make_str(o->str.chars, o->str.len - p->str.len);
    return pys_make_str(o->str.chars, o->str.len);
}

#define _STR_PRED(name, expr) \
    static VALUE sm_##name(CTX *c, int argc, VALUE *argv) { \
        (void)c; (void)argc; \
        struct pysobj *o = PYS_PTR(argv[0]); \
        if (o->str.len == 0) return PYS_FALSE; \
        for (size_t i = 0; i < o->str.len; i++) { \
            unsigned char ch = (unsigned char)o->str.chars[i]; \
            if (!(expr)) return PYS_FALSE; \
        } \
        return PYS_TRUE; \
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
    struct pysobj *o = PYS_PTR(argv[0]);
    if (!pys_is_str(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "partition needs str");
    struct pysobj *sep = PYS_PTR(argv[1]);
    if (sep->str.len == 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "empty separator");
    const char *p = (const char *)memmem(o->str.chars, o->str.len, sep->str.chars, sep->str.len);
    if (!p) {
        VALUE items[3] = {
            pys_make_str(o->str.chars, o->str.len),
            pys_make_str("", 0),
            pys_make_str("", 0),
        };
        return pys_make_tuple(items, 3);
    }
    size_t hl = (size_t)(p - o->str.chars);
    VALUE items[3] = {
        pys_make_str(o->str.chars, hl),
        pys_make_str(sep->str.chars, sep->str.len),
        pys_make_str(p + sep->str.len, o->str.len - hl - sep->str.len),
    };
    return pys_make_tuple(items, 3);
}

static VALUE
sm_rpartition(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (!pys_is_str(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "rpartition needs str");
    struct pysobj *sep = PYS_PTR(argv[1]);
    if (sep->str.len == 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "empty separator");
    if (sep->str.len > o->str.len) {
        VALUE items[3] = {
            pys_make_str("", 0), pys_make_str("", 0),
            pys_make_str(o->str.chars, o->str.len),
        };
        return pys_make_tuple(items, 3);
    }
    ssize_t hit = -1;
    for (ssize_t i = (ssize_t)(o->str.len - sep->str.len); i >= 0; i--) {
        if (memcmp(o->str.chars + i, sep->str.chars, sep->str.len) == 0) {
            hit = i; break;
        }
    }
    if (hit < 0) {
        VALUE items[3] = {
            pys_make_str("", 0), pys_make_str("", 0),
            pys_make_str(o->str.chars, o->str.len),
        };
        return pys_make_tuple(items, 3);
    }
    VALUE items[3] = {
        pys_make_str(o->str.chars, (size_t)hit),
        pys_make_str(sep->str.chars, sep->str.len),
        pys_make_str(o->str.chars + hit + sep->str.len,
                    o->str.len - hit - sep->str.len),
    };
    return pys_make_tuple(items, 3);
}

static VALUE
sm_rsplit(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    int maxsplit = (argc >= 3) ? (int)pys_int_to_long(c, argv[2]) : -1;
    VALUE r = pys_make_list(NULL, 0);
    if (argc < 2 || argv[1] == PYS_NONE) {
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
            if (np >= 256) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "rsplit too many parts");
            pieces[np++] = pys_make_str(o->str.chars + i, (size_t)(end - i));
            splits++;
        }
        if (i > 0 && maxsplit >= 0 && splits >= maxsplit) {
            // remainder
            // strip trailing ws from index i? Actually keep it.
            pieces[np++] = pys_make_str(o->str.chars, (size_t)i);
        }
        // pieces are right-to-left; reverse.
        for (int k = np - 1; k >= 0; k--) pys_list_append(c, r, pieces[k]);
        return r;
    }
    if (!pys_is_str(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "rsplit sep must be str");
    struct pysobj *sep = PYS_PTR(argv[1]);
    if (sep->str.len == 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "empty separator");
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
        if (np >= 256) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "rsplit too many");
        pieces[np++] = pys_make_str(o->str.chars + hit + sep->str.len,
                                    (size_t)(i - hit - sep->str.len));
        i = hit;
        splits++;
    }
    if (np >= 256) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "rsplit too many");
    pieces[np++] = pys_make_str(o->str.chars, (size_t)i);
    for (int k = np - 1; k >= 0; k--) pys_list_append(c, r, pieces[k]);
    return r;
}

static VALUE
sm_format_map(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    // Reuse sm_format by injecting a dict's items as kwargs is hard;
    // simpler: implement directly using subscript on the mapping.
    extern VALUE bi_format(CTX *c, int argc, VALUE *argv);
    struct pysobj *o = PYS_PTR(argv[0]);
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
            if (j >= srclen) PYS_RAISE_EXC(c, c->EXC_ValueError, "unterminated '{'");
            const char *body = src + i + 1;
            size_t bn = j - i - 1;
            size_t k = 0;
            VALUE val;
            if (bn == 0 || body[0] == ':' || body[0] == '!') {
                // auto: use auto_idx
                PYS_RAISE_EXC(c, c->EXC_KeyError, "format_map: positional fields not supported");
                (void)auto_idx;
            }
            if (body[0] >= '0' && body[0] <= '9') {
                PYS_RAISE_EXC(c, c->EXC_KeyError, "format_map: positional fields not supported");
            }
            // Named field: lookup in map_v.
            size_t nm_start = 0;
            while (k < bn && body[k] != ':' && body[k] != '!') k++;
            VALUE key = pys_make_str(body + nm_start, k);
            val = pys_list_get(c, map_v, key);
            // Optional !conv.
            if (k < bn && body[k] == '!') {
                k++;
                if (k >= bn) PYS_RAISE_EXC(c, c->EXC_ValueError, "expected conversion");
                char conv = body[k++];
                if (conv == 'r' || conv == 'a') val = pys_to_repr(c, val);
                else if (conv == 's')           val = pys_to_str(c, val);
            }
            VALUE spec_val = pys_make_str("", 0);
            if (k < bn && body[k] == ':') {
                k++; spec_val = pys_make_str(body + k, bn - k);
            }
            VALUE av[2] = { val, spec_val };
            VALUE rendered = bi_format(c, 2, av);
            if (!pys_is_str(rendered)) rendered = pys_to_str(c, rendered);
            size_t rl = PYS_PTR(rendered)->str.len;
            while (out_len + rl > out_capa) { out_capa *= 2; char *no = GC_malloc_atomic(out_capa); memcpy(no, out, out_len); out = no; }
            memcpy(out + out_len, PYS_PTR(rendered)->str.chars, rl);
            out_len += rl;
            i = j;
        } else if (ch == '}') {
            if (i + 1 < srclen && src[i+1] == '}') {
                if (out_len + 1 > out_capa) { out_capa *= 2; char *no = GC_malloc_atomic(out_capa); memcpy(no, out, out_len); out = no; }
                out[out_len++] = '}'; i++; continue;
            }
            PYS_RAISE_EXC(c, c->EXC_ValueError, "single '}' in format");
        } else {
            if (out_len + 1 > out_capa) { out_capa *= 2; char *no = GC_malloc_atomic(out_capa); memcpy(no, out, out_len); out = no; }
            out[out_len++] = ch;
        }
    }
    return pys_make_str(out, out_len);
}

// str.maketrans — classmethod-like (called via str.maketrans(...)).
// Forms: maketrans(dict) or maketrans(from, to[, drop]).
static VALUE
bi_str_maketrans(CTX *c, int argc, VALUE *argv)
{
    VALUE r = pys_make_dict();
    if (argc == 1 && pys_is_dict(argv[0])) {
        struct pysdict *d = PYS_PTR(argv[0])->dict;
        for (size_t i = 0; i < d->elen; i++) {
            if (!pydict_entry_live(d, i)) continue;
            VALUE k = d->entries[i].key;
            VALUE v = d->entries[i].value;
            if (pys_is_str(k) && PYS_PTR(k)->str.len == 1) {
                VALUE intk = PYS_FIX((unsigned char)PYS_PTR(k)->str.chars[0]);
                pys_dict_set(c, r, intk, v);
            } else if (PYS_IS_FIXNUM(k)) {
                pys_dict_set(c, r, k, v);
            }
        }
        return r;
    }
    if (argc >= 2 && pys_is_str(argv[0]) && pys_is_str(argv[1])) {
        struct pysobj *a = PYS_PTR(argv[0]);
        struct pysobj *b = PYS_PTR(argv[1]);
        if (a->str.len != b->str.len)
            PYS_RAISE_EXC(c, c->EXC_ValueError, "maketrans: from and to differ in length");
        for (size_t i = 0; i < a->str.len; i++) {
            VALUE k = PYS_FIX((unsigned char)a->str.chars[i]);
            VALUE v = PYS_FIX((unsigned char)b->str.chars[i]);
            pys_dict_set(c, r, k, v);
        }
        if (argc >= 3 && pys_is_str(argv[2])) {
            struct pysobj *d = PYS_PTR(argv[2]);
            for (size_t i = 0; i < d->str.len; i++) {
                VALUE k = PYS_FIX((unsigned char)d->str.chars[i]);
                pys_dict_set(c, r, k, PYS_NONE);
            }
        }
        return r;
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "maketrans: bad args");
}

static VALUE
sm_translate(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (!pys_is_dict(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "translate: dict required");
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    size_t len = 0;
    for (size_t i = 0; i < o->str.len; i++) {
        unsigned char ch = (unsigned char)o->str.chars[i];
        VALUE k = PYS_FIX(ch);
        VALUE v = pys_dict_has(c, argv[1], k) ? pys_dict_get(c, argv[1], k) : k;
        if (v == PYS_NONE) continue;
        if (PYS_IS_FIXNUM(v)) {
            int64_t cv = PYS_FIXVAL(v);
            if (cv < 0 || cv > 255) PYS_RAISE_EXC(c, c->EXC_ValueError, "translate: out of range");
            buf[len++] = (char)cv;
        } else if (pys_is_str(v)) {
            struct pysobj *sv = PYS_PTR(v);
            if (len + sv->str.len + 1 > o->str.len + 1) {
                buf = (char *)GC_realloc(buf, len + sv->str.len + 16);
            }
            memcpy(buf + len, sv->str.chars, sv->str.len);
            len += sv->str.len;
        }
    }
    return pys_make_str(buf, len);
}

static VALUE
sm_expandtabs(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    int w = (argc >= 2) ? (int)pys_int_to_long(c, argv[1]) : 8;
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
    return pys_make_str(buf, len);
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
    { "encode",        sm_encode,        1, 3 },
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
    pys_list_append(c, argv[0], argv[1]);
    return PYS_NONE;
}

// list.__init__([iterable]) — CPython parity: clear in place, then extend
// from iterable.  `a.__init__()` after `a = [1,2,3]` leaves `a == []`.
// For built-in subclass instances, operate on the primary list value
// (so `class Sub(list)` followed by `super().__init__(seq)` populates).
static VALUE
lm_init(CTX *c, int argc, VALUE *argv)
{
    VALUE target = argv[0];
    if (PYS_IS_PTR(target) && PYS_PTR(target)->type == PYS_T_INSTANCE
        && PYS_PTR(target)->inst.primary
        && pys_is_list(PYS_PTR(target)->inst.primary))
        target = PYS_PTR(target)->inst.primary;
    struct pysobj *o = PYS_PTR(target);
    o->list.len = 0;
    if (argc >= 2) {
        struct pys_iter it; pys_iter_init(c, &it, argv[1]);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
            pys_list_append(c, target, x);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        }
    }
    return PYS_NONE;
}

static VALUE
lm_pop(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t i = (argc >= 2) ? pys_int_to_long(c, argv[1]) : (int64_t)o->list.len - 1;
    if (i < 0) i += (int64_t)o->list.len;
    if (i < 0 || i >= (int64_t)o->list.len)
        PYS_RAISE_EXC(c, c->EXC_IndexError, "pop from empty / out-of-range list");
    VALUE v = o->list.items[i];
    // Bulk shift via memmove — was per-element loop, ~6× slower due to
    // missed VPMOVZX-style auto-vectorisation by the compiler.  deltablue's
    // OrderedCollection.pop(0) was 6.7% of total runtime; memmove drops
    // it ~5×.
    size_t tail = o->list.len - (size_t)i - 1;
    if (tail > 0)
        memmove(&o->list.items[i], &o->list.items[i + 1], tail * sizeof(VALUE));
    o->list.len--;
    return v;
}

static VALUE
lm_extend(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE other = argv[1];
    struct pys_iter it; pys_iter_init(c, &it, other);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) pys_list_append(c, argv[0], x);
    return PYS_NONE;
}

static VALUE
lm_insert(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t i = pys_int_to_long(c, argv[1]);
    if (i < 0) i += (int64_t)o->list.len;
    if (i < 0) i = 0;
    if (i > (int64_t)o->list.len) i = (int64_t)o->list.len;
    pys_list_append(c, argv[0], PYS_NONE);  // grow
    for (size_t j = o->list.len - 1; j > (size_t)i; j--) o->list.items[j] = o->list.items[j - 1];
    o->list.items[i] = argv[2];
    return PYS_NONE;
}

static VALUE
lm_index(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t start = (argc >= 3) ? pys_int_to_long(c, argv[2]) : 0;
    int64_t stop  = (argc >= 4) ? pys_int_to_long(c, argv[3]) : (int64_t)o->list.len;
    if (start < 0) start += (int64_t)o->list.len;
    if (start < 0) start = 0;
    if (stop < 0) stop += (int64_t)o->list.len;
    if (stop > (int64_t)o->list.len) stop = (int64_t)o->list.len;
    for (int64_t i = start; i < stop; i++) {
        // Re-bound on every step: the user's __eq__ may have shrunk the
        // list (test_count_index_remove_crashes / bpo-38610).
        if (i >= (int64_t)o->list.len) break;
        VALUE elt = o->list.items[i];
        // Identity short-circuit so `nan in [nan]` / .index(nan) work
        // (CPython uses PyObject_RichCompareBool which checks identity first).
        if (elt == argv[1]) return PYS_FIX(i);
        bool eq = pys_eq_bool(c, elt, argv[1]);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        if (eq) return PYS_FIX(i);
    }
    PYS_RAISE_EXC(c, c->EXC_ValueError, "value not in list");
}

static VALUE
lm_count(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t n = 0;
    for (size_t i = 0; ; i++) {
        if (i >= o->list.len) break;
        VALUE elt = o->list.items[i];
        if (elt == argv[1]) { n++; continue; }
        bool eq = pys_eq_bool(c, elt, argv[1]);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        if (eq) n++;
    }
    return PYS_FIX(n);
}

static VALUE
lm_reverse(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    for (size_t i = 0, j = o->list.len; i + 1 < j; i++, j--) {
        VALUE t = o->list.items[i];
        o->list.items[i] = o->list.items[j - 1];
        o->list.items[j - 1] = t;
    }
    return PYS_NONE;
}

static VALUE
lm_sort(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    VALUE key_fn = pys_bi_kwarg("key");
    VALUE rev_v  = pys_bi_kwarg("reverse");
    bool reverse = (rev_v == PYS_TRUE);
    // Pre-compute sort keys when key_fn is set (Schwartzian transform).
    VALUE *keys = NULL;
    if (key_fn) {
        keys = (VALUE *)GC_malloc(sizeof(VALUE) * (o->list.len ? o->list.len : 1));
        for (size_t i = 0; i < o->list.len; i++) {
            keys[i] = pys_apply(c, key_fn, 1, &o->list.items[i]);
            if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
        }
    }
    // Insertion sort over items[] using keys[] for comparison.
    for (size_t i = 1; i < o->list.len; i++) {
        VALUE xv = o->list.items[i];
        VALUE xk = keys ? keys[i] : xv;
        size_t j = i;
        while (j > 0) {
            VALUE prev_k = keys ? keys[j - 1] : o->list.items[j - 1];
            int cmp = pys_cmp(c, prev_k, xk);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
            if (reverse ? (cmp >= 0) : (cmp <= 0)) break;
            o->list.items[j] = o->list.items[j - 1];
            if (keys) keys[j] = keys[j - 1];
            j--;
        }
        o->list.items[j] = xv;
        if (keys) keys[j] = xk;
    }
    return PYS_NONE;
}

// list.remove(x) — remove first occurrence of x.
static VALUE
lm_remove(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    for (size_t i = 0; ; i++) {
        // Re-check len each step: __eq__ may have shrunk the list
        // (bpo-38610).
        if (i >= o->list.len) break;
        VALUE elt = o->list.items[i];
        bool eq = pys_eq_bool(c, elt, argv[1]);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        if (eq) {
            // The list may have been shrunk by __eq__ — re-check before
            // computing slide bounds.
            if (i >= o->list.len) break;
            for (size_t j = i; j + 1 < o->list.len; j++)
                o->list.items[j] = o->list.items[j+1];
            o->list.len--;
            return PYS_NONE;
        }
    }
    PYS_RAISE_EXC(c, c->EXC_ValueError, "list.remove(x): x not in list");
}

// list.copy() / list.clear().
static VALUE
lm_copy(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    return pys_make_list(o->list.items, o->list.len);
}
static VALUE
lm_clear(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    PYS_PTR(argv[0])->list.len = 0;
    return PYS_NONE;
}

static struct type_method list_methods[] = {
    { "append",  lm_append,  2, 2 },
    { "__init__", lm_init,   1, 2 },
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
    VALUE dflt = (argc >= 3) ? argv[2] : PYS_NONE;
    if (pys_dict_has(c, d, k)) return pys_dict_get(c, d, k);
    return dflt;
}

static VALUE
dm_keys(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysdict *d = PYS_PTR(argv[0])->dict;
    VALUE r = pys_make_list(NULL, 0);
    for (size_t i = 0; i < d->elen; i++) {
        if (!pydict_entry_live(d, i)) continue;
        pys_list_append(c, r, d->entries[i].key);
    }
    return r;
}

static VALUE
dm_values(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysdict *d = PYS_PTR(argv[0])->dict;
    VALUE r = pys_make_list(NULL, 0);
    for (size_t i = 0; i < d->elen; i++) {
        if (!pydict_entry_live(d, i)) continue;
        pys_list_append(c, r, d->entries[i].value);
    }
    return r;
}

static VALUE
dm_items(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysdict *d = PYS_PTR(argv[0])->dict;
    VALUE r = pys_make_list(NULL, 0);
    for (size_t i = 0; i < d->elen; i++) {
        if (!pydict_entry_live(d, i)) continue;
        VALUE pair[2] = { d->entries[i].key, d->entries[i].value };
        pys_list_append(c, r, pys_make_tuple(pair, 2));
    }
    return r;
}

static VALUE
dm_pop(CTX *c, int argc, VALUE *argv)
{
    VALUE d = argv[0], k = argv[1];
    bool has = pys_dict_has(c, d, k);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (has) {
        VALUE v = pys_dict_get(c, d, k);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        pys_dict_remove(c, d, k);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        return v;
    }
    if (argc >= 3) return argv[2];
    // CPython: KeyError(key) — args[0] is the missing key.
    VALUE inst = pys_make_instance(c->EXC_KeyError);
    pys_setattr(c, inst, "args", pys_make_tuple(&k, 1));
    pys_setattr(c, inst, "__context__", c->current_handling_exc ? c->current_handling_exc : PYS_NONE);
    pys_setattr(c, inst, "__cause__", PYS_NONE);
    pys_setattr(c, inst, "__suppress_context__", PYS_FALSE);
    c->state = PYS_STATE_RAISE;
    c->state_value = inst;
    return 0;
}

static VALUE
dm_update(CTX *c, int argc, VALUE *argv)
{
    VALUE dst = argv[0];
    if (argc >= 2) {
        VALUE src = argv[1];
        if (pys_is_dict(src)) {
            struct pysdict *sd = PYS_PTR(src)->dict;
            for (size_t i = 0; i < sd->elen; i++)
                if (pydict_entry_live(sd, i))
                    pys_dict_set(c, dst, sd->entries[i].key, sd->entries[i].value);
        } else if (pys_is_instance(src)
                   && pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(src)->inst.cls), "keys") != PYS_NONE) {
            // Mapping protocol: src.keys() + src[key] for each.  CPython
            // dict.update consults `keys` before falling to (k,v)-pair iteration.
            VALUE km = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(src)->inst.cls), "keys");
            VALUE av[1] = { src };
            VALUE keys = pys_apply(c, km, 1, av);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
            struct pys_iter it; pys_iter_init(c, &it, keys);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
            VALUE k;
            while (pys_iter_next(c, &it, &k)) {
                VALUE v = pys_list_get(c, src, k);
                if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
                pys_dict_set(c, dst, k, v);
                if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
            }
        } else {
            struct pys_iter it; pys_iter_init(c, &it, src);
            if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
            VALUE pair;
            while (pys_iter_next(c, &it, &pair)) {
                if (!pys_is_tuple(pair) && !pys_is_list(pair)) PYS_RAISE_EXC(c, c->EXC_TypeError, "update: pair");
                if (PYS_PTR(pair)->list.len != 2) PYS_RAISE_EXC(c, c->EXC_ValueError, "dictionary update sequence element has length %zu; 2 is required", PYS_PTR(pair)->list.len);
                pys_dict_set(c, dst, PYS_PTR(pair)->list.items[0], PYS_PTR(pair)->list.items[1]);
                if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
            }
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
        }
    }
    // Also pull in kwargs.
    extern int    PYS_BI_KWC;
    extern const char **PYS_BI_KWNAMES;
    extern VALUE *PYS_BI_KWVALUES;
    for (int i = 0; i < PYS_BI_KWC; i++) {
        VALUE k = pys_make_str(PYS_BI_KWNAMES[i], strlen(PYS_BI_KWNAMES[i]));
        pys_dict_set(c, dst, k, PYS_BI_KWVALUES[i]);
    }
    return PYS_NONE;
}

static VALUE
dm_setdefault(CTX *c, int argc, VALUE *argv)
{
    VALUE d = argv[0], k = argv[1];
    VALUE dflt = (argc >= 3) ? argv[2] : PYS_NONE;
    if (pys_dict_has(c, d, k)) return pys_dict_get(c, d, k);
    pys_dict_set(c, d, k, dflt);
    return dflt;
}

static VALUE
dm_popitem(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysdict *d = PYS_PTR(argv[0])->dict;
    if (d->used == 0) PYS_RAISE_EXC(c, c->EXC_KeyError, "popitem: empty dict");
    // Find the LAST live entry (Python 3.7+ semantic: LIFO).
    for (size_t i = d->elen; i > 0; ) {
        i--;
        if (pydict_entry_live(d, i)) {
            VALUE pair[2] = { d->entries[i].key, d->entries[i].value };
            pys_dict_remove(c, argv[0], pair[0]);
            return pys_make_tuple(pair, 2);
        }
    }
    PYS_RAISE_EXC(c, c->EXC_KeyError, "popitem: empty");
}

static VALUE
dm_clear(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysdict *d = PYS_PTR(argv[0])->dict;
    // Re-init the dict.
    d->elen = 0;
    d->used = 0;
    d->fill = 0;
    d->version++;       // bpo-46615: clear invalidates active iterators
    for (size_t i = 0; i < d->icapa; i++) d->indices[i] = DICT_EMPTY_IDX;
    return PYS_NONE;
}

static VALUE
dm_copy(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysdict *d = PYS_PTR(argv[0])->dict;
    VALUE r = pys_make_dict();
    for (size_t i = 0; i < d->elen; i++)
        if (pydict_entry_live(d, i))
            pys_dict_set(c, r, d->entries[i].key, d->entries[i].value);
    return r;
}

// Dunder dispatchers used so that built-in subclasses can call
// super().__setitem__ etc. via pys_super_lookup → pys_builtin_method.
static VALUE
dm_setitem(CTX *c, int argc, VALUE *argv)
{ (void)argc; pys_dict_set(c, argv[0], argv[1], argv[2]); return PYS_NONE; }
static VALUE
dm_getitem(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    bool has = pys_dict_has(c, argv[0], argv[1]);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (!has) {
        // CPython: KeyError(key) — args[0] is the missing key.
        VALUE inst = pys_make_instance(c->EXC_KeyError);
        pys_setattr(c, inst, "args", pys_make_tuple(&argv[1], 1));
        pys_setattr(c, inst, "__context__", c->current_handling_exc ? c->current_handling_exc : PYS_NONE);
        pys_setattr(c, inst, "__cause__", PYS_NONE);
        pys_setattr(c, inst, "__suppress_context__", PYS_FALSE);
        c->state = PYS_STATE_RAISE;
        c->state_value = inst;
        return 0;
    }
    return pys_dict_get(c, argv[0], argv[1]);
}
static VALUE
dm_delitem(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    bool removed = pys_dict_remove(c, argv[0], argv[1]);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    if (!removed) {
        VALUE inst = pys_make_instance(c->EXC_KeyError);
        pys_setattr(c, inst, "args", pys_make_tuple(&argv[1], 1));
        pys_setattr(c, inst, "__context__", c->current_handling_exc ? c->current_handling_exc : PYS_NONE);
        pys_setattr(c, inst, "__cause__", PYS_NONE);
        pys_setattr(c, inst, "__suppress_context__", PYS_FALSE);
        c->state = PYS_STATE_RAISE;
        c->state_value = inst;
        return 0;
    }
    return PYS_NONE;
}
static VALUE
dm_contains(CTX *c, int argc, VALUE *argv)
{ (void)argc; return pys_dict_has(c, argv[0], argv[1]) ? PYS_TRUE : PYS_FALSE; }
static VALUE
dm_len(CTX *c, int argc, VALUE *argv)
{ (void)c; (void)argc; return PYS_FIX((int64_t)PYS_PTR(argv[0])->dict->used); }

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
    { "fromkeys",    dm_fromkeys_bridge,  2, 3 },
    { NULL, NULL, 0, 0 }
};

// Set methods.
static VALUE
sm_add(CTX *c, int argc, VALUE *argv) { (void)argc; pys_dict_set(c, argv[0], argv[1], PYS_NONE); return PYS_NONE; }
// CPython parity: set.{remove,discard,contains} convert an unhashable set
// key to a frozenset and retry, so `{frozenset([1])}.remove({1})` works.
// SetSubclass instances are unwrapped first.
static inline VALUE pys_set_key_for_lookup(CTX *c, VALUE k) {
    VALUE u = pys_unwrap_primary(k);
    if (pys_is_set(u)) {
        VALUE fsk = pys_make_frozenset();
        struct pysdict *src = PYS_PTR(u)->dict;
        for (size_t i = 0; i < src->elen; i++)
            if (pydict_entry_live(src, i))
                pys_dict_set(c, fsk, src->entries[i].key, PYS_NONE);
        return fsk;
    }
    return k;
}
static VALUE
sm_discard(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE k = pys_set_key_for_lookup(c, argv[1]);
    pys_dict_remove(c, argv[0], k);
    return PYS_NONE;
}
static VALUE
sm_remove(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE k = pys_set_key_for_lookup(c, argv[1]);
    if (pys_dict_remove(c, argv[0], k)) return PYS_NONE;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    // CPython parity: KeyError(key) — `e.args[0]` must be the missing key
    // (original argv[1], not the converted frozenset).
    VALUE inst = pys_make_instance(c->EXC_KeyError);
    pys_setattr(c, inst, "args", pys_make_tuple(&argv[1], 1));
    pys_setattr(c, inst, "__context__", c->current_handling_exc ? c->current_handling_exc : PYS_NONE);
    pys_setattr(c, inst, "__cause__", PYS_NONE);
    pys_setattr(c, inst, "__suppress_context__", PYS_FALSE);
    c->state = PYS_STATE_RAISE;
    c->state_value = inst;
    return 0;
}
static VALUE
sm_set_pop(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE sv = argv[0];
    struct pysdict *d = PYS_PTR(sv)->dict;
    for (size_t i = 0; i < d->elen; i++) {
        if (pydict_entry_live(d, i)) {
            VALUE k = d->entries[i].key;
            pys_dict_remove(c, sv, k);
            return k;
        }
    }
    PYS_RAISE_EXC(c, c->EXC_KeyError, "pop from an empty set");
}
static VALUE
sm_union(CTX *c, int argc, VALUE *argv) {
    VALUE r = pys_make_set_like(argv[0]);
    VALUE av0 = pys_unwrap_primary(argv[0]);
    struct pysdict *a = PYS_PTR(av0)->dict;
    for (size_t i = 0; i < a->elen; i++)
        if (pydict_entry_live(a, i)) pys_dict_set(c, r, a->entries[i].key, PYS_NONE);
    // CPython: set.union(*others) — accept any number of iterables.
    for (int j = 1; j < argc; j++) {
        VALUE av_j = pys_unwrap_primary(argv[j]);
        if (pys_is_set(av_j) || pys_is_frozenset(av_j)) {
            struct pysdict *b = PYS_PTR(av_j)->dict;
            for (size_t i = 0; i < b->elen; i++)
                if (pydict_entry_live(b, i)) pys_dict_set(c, r, b->entries[i].key, PYS_NONE);
        } else {
            struct pys_iter it; pys_iter_init(c, &it, argv[j]);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            VALUE x;
            while (pys_iter_next(c, &it, &x)) {
                pys_dict_set(c, r, x, PYS_NONE);
                if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
            }
        }
    }
    return r;
}
// Materialize an iterable into a set so `pys_contains` can be called
// repeatedly with arbitrary key types.  set/frozenset/dict are returned
// as-is (already keyed by hash).  list/tuple/str/bytes/generator are
// drained — for str/bytes this is critical because `pys_contains(str, key)`
// raises TypeError unless key is also a str (which is wrong for set
// operations where we just want membership-by-value).
static VALUE pys_set_op_materialize(CTX *c, VALUE v) {
    VALUE u = pys_unwrap_primary(v);
    if (pys_is_any_set(u) || pys_is_dict(u))
        return v;
    // list/tuple/str/bytes/generator/user-iter: drain into a set.
    VALUE acc = pys_make_set();
    struct pys_iter it; pys_iter_init(c, &it, v);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) {
        pys_dict_set(c, acc, x, PYS_NONE);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    return acc;
}
static VALUE
sm_intersection(CTX *c, int argc, VALUE *argv) {
    VALUE r = pys_make_set_like(argv[0]);
    VALUE av0 = pys_unwrap_primary(argv[0]);
    struct pysdict *a = PYS_PTR(av0)->dict;
    // s.intersection(*others) — keep keys present in ALL others.  Each
    // `other` may be a generator that pys_contains would consume in one
    // pass; materialize first.
    VALUE *others = argc > 1 ? (VALUE *)alloca(sizeof(VALUE) * argc) : NULL;
    for (int j = 1; j < argc; j++) {
        others[j] = pys_set_op_materialize(c, argv[j]);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    for (size_t i = 0; i < a->elen; i++) {
        if (!pydict_entry_live(a, i)) continue;
        VALUE k = a->entries[i].key;
        bool ok = true;
        for (int j = 1; j < argc; j++) {
            if (!pys_contains(c, others[j], k)) { ok = false; break; }
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        }
        if (ok) pys_dict_set(c, r, k, PYS_NONE);
    }
    return r;
}
static VALUE
sm_difference(CTX *c, int argc, VALUE *argv) {
    VALUE r = pys_make_set_like(argv[0]);
    VALUE av0 = pys_unwrap_primary(argv[0]);
    struct pysdict *a = PYS_PTR(av0)->dict;
    VALUE *others = argc > 1 ? (VALUE *)alloca(sizeof(VALUE) * argc) : NULL;
    for (int j = 1; j < argc; j++) {
        others[j] = pys_set_op_materialize(c, argv[j]);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    // s.difference(*others) — drop keys present in ANY other.
    for (size_t i = 0; i < a->elen; i++) {
        if (!pydict_entry_live(a, i)) continue;
        VALUE k = a->entries[i].key;
        bool keep = true;
        for (int j = 1; j < argc; j++) {
            if (pys_contains(c, others[j], k)) { keep = false; break; }
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        }
        if (keep) pys_dict_set(c, r, k, PYS_NONE);
    }
    return r;
}

static VALUE
sm_symmetric_difference(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE a = pys_unwrap_primary(argv[0]);
    // Materialize argv[1] so we can scan it twice (gen would be exhausted).
    VALUE b_input = pys_set_op_materialize(c, argv[1]);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    VALUE b = pys_unwrap_primary(b_input);
    VALUE r = pys_make_set_like(argv[0]);
    struct pysdict *aa = PYS_PTR(a)->dict;
    for (size_t i = 0; i < aa->elen; i++)
        if (pydict_entry_live(aa, i) && !pys_contains(c, b, aa->entries[i].key))
            pys_dict_set(c, r, aa->entries[i].key, PYS_NONE);
    if (pys_is_any_set(b) || pys_is_dict(b)) {
        struct pysdict *bb = PYS_PTR(b)->dict;
        for (size_t i = 0; i < bb->elen; i++)
            if (pydict_entry_live(bb, i) && !pys_contains(c, a, bb->entries[i].key))
                pys_dict_set(c, r, bb->entries[i].key, PYS_NONE);
    } else {
        struct pys_iter it; pys_iter_init(c, &it, b);
        VALUE x;
        while (pys_iter_next(c, &it, &x))
            if (!pys_contains(c, a, x))
                pys_dict_set(c, r, x, PYS_NONE);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    return r;
}
static VALUE
sm_issubset(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    struct pysdict *a = PYS_PTR(argv[0])->dict;
    for (size_t i = 0; i < a->elen; i++)
        if (pydict_entry_live(a, i) && !pys_contains(c, argv[1], a->entries[i].key))
            return PYS_FALSE;
    return PYS_TRUE;
}
static VALUE
sm_issuperset(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    struct pys_iter it; pys_iter_init(c, &it, argv[1]);
    VALUE x;
    while (pys_iter_next(c, &it, &x))
        if (!pys_contains(c, argv[0], x)) return PYS_FALSE;
    return PYS_TRUE;
}
static VALUE
sm_isdisjoint(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    struct pys_iter it; pys_iter_init(c, &it, argv[1]);
    VALUE x;
    while (pys_iter_next(c, &it, &x))
        if (pys_contains(c, argv[0], x)) return PYS_FALSE;
    return PYS_TRUE;
}
static VALUE
sm_set_copy(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    VALUE r = pys_make_set();
    struct pysdict *src = PYS_PTR(argv[0])->dict;
    for (size_t i = 0; i < src->elen; i++)
        if (pydict_entry_live(src, i))
            pys_dict_set(c, r, src->entries[i].key, PYS_NONE);
    return r;
}
static VALUE
sm_set_clear(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc;
    // In-place clear (matches dm_clear) so active iterators detect the
    // mutation via d->version rather than dereferencing a stale entries
    // pointer when we swap to a fresh pydict_new().
    struct pysdict *d = PYS_PTR(argv[0])->dict;
    d->elen = 0;
    d->used = 0;
    d->fill = 0;
    d->version++;
    for (size_t i = 0; i < d->icapa; i++) d->indices[i] = DICT_EMPTY_IDX;
    return PYS_NONE;
}
static VALUE
sm_set_update(CTX *c, int argc, VALUE *argv) {
    VALUE a = pys_unwrap_primary(argv[0]);
    for (int j = 1; j < argc; j++) {
        struct pys_iter it; pys_iter_init(c, &it, argv[j]);
        if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
            pys_dict_set(c, a, x, PYS_NONE);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
        }
    }
    return PYS_NONE;
}

static VALUE
sm_difference_update(CTX *c, int argc, VALUE *argv)
{
    VALUE a = pys_unwrap_primary(argv[0]);
    for (int j = 1; j < argc; j++) {
        struct pys_iter it; pys_iter_init(c, &it, argv[j]);
        if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
        VALUE x;
        while (pys_iter_next(c, &it, &x)) pys_dict_remove(c, a, x);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
    }
    return PYS_NONE;
}
static VALUE
sm_intersection_update(CTX *c, int argc, VALUE *argv)
{
    VALUE a = pys_unwrap_primary(argv[0]);
    // CPython: `intersection_update(it1, it2, ...)` intersects with all
    // iterables, raising TypeError on unhashable elements BEFORE mutation.
    // Materialise each iterable into a set first.
    for (int k = 1; k < argc; k++) {
        VALUE b = argv[k];
        VALUE bu = pys_unwrap_primary(b);
        if (!pys_is_any_set(bu)) {
            VALUE av[1] = { b };
            bu = bi_set(c, 1, av);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
        }
        struct pysdict *aa = PYS_PTR(a)->dict;
        VALUE *to_remove = (VALUE *)alloca(sizeof(VALUE) * aa->elen);
        int n = 0;
        for (size_t i = 0; i < aa->elen; i++) {
            if (!pydict_entry_live(aa, i)) continue;
            if (!pys_contains(c, bu, aa->entries[i].key))
                to_remove[n++] = aa->entries[i].key;
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
        }
        for (int i = 0; i < n; i++) {
            pys_dict_remove(c, a, to_remove[i]);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
        }
    }
    return PYS_NONE;
}
static VALUE
sm_symmetric_difference_update(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE a = pys_unwrap_primary(argv[0]);
    VALUE b = argv[1];
    // CPython: materialise `b` into a set first so unhashable items raise
    // TypeError before any mutation happens.
    VALUE bu = pys_unwrap_primary(b);
    if (!pys_is_any_set(bu)) {

        VALUE av[1] = { b };
        bu = bi_set(c, 1, av);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
    }
    struct pys_iter it; pys_iter_init(c, &it, bu);
    VALUE x;
    while (pys_iter_next(c, &it, &x)) {
        if (pys_contains(c, a, x)) pys_dict_remove(c, a, x);
        else pys_dict_set(c, a, x, PYS_NONE);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
    }
    return PYS_NONE;
}

// set.__init__([iterable]) — CPython parity: clear in place, then add
// every element of the iterable.  Forwards through to bi_set after
// clearing.
static VALUE
sm_set_init(CTX *c, int argc, VALUE *argv)
{
    if (PYS_BI_KWC > 0)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "set() does not take keyword arguments");
    if (argc > 2)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "set expected at most 1 argument, got %d", argc - 1);
    VALUE target = pys_unwrap_primary(argv[0]);
    if (!pys_is_set(target))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "descriptor '__init__' requires a 'set' object");
    // Clear in place (mirrors sm_set_clear).
    struct pysdict *d = PYS_PTR(target)->dict;
    d->elen = 0;
    d->used = 0;
    d->fill = 0;
    d->version++;
    for (size_t i = 0; i < d->icapa; i++) d->indices[i] = DICT_EMPTY_IDX;
    if (argc < 2) return PYS_NONE;
    struct pys_iter it; pys_iter_init(c, &it, argv[1]);
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) {
        pys_dict_set(c, target, x, PYS_NONE);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    return PYS_NONE;
}

static struct type_method set_methods[] = {
    { "__init__",             sm_set_init,             1, -1 },
    { "add",                  sm_add,                  2, 2 },
    { "discard",              sm_discard,              2, 2 },
    { "remove",               sm_remove,               2, 2 },
    { "pop",                  sm_set_pop,              1, 1 },
    { "clear",                sm_set_clear,            1, 1 },
    { "copy",                 sm_set_copy,             1, 1 },
    { "update",               sm_set_update,           1, -1 },
    { "union",                sm_union,                1, -1 },
    { "intersection",         sm_intersection,         1, -1 },
    { "difference",           sm_difference,           1, -1 },
    { "symmetric_difference", sm_symmetric_difference, 2, 2 },
    { "intersection_update",  sm_intersection_update,  1, -1 },
    { "difference_update",    sm_difference_update,    1, -1 },
    { "symmetric_difference_update", sm_symmetric_difference_update, 2, 2 },
    { "issubset",             sm_issubset,             2, 2 },
    { "issuperset",           sm_issuperset,           2, 2 },
    { "isdisjoint",           sm_isdisjoint,           2, 2 },
    { NULL, NULL, 0, 0 }
};

// frozenset: read-only ops only.
static struct type_method frozenset_methods[] = {
    { "copy",                 sm_set_copy,             1, 1 },
    { "union",                sm_union,                1, -1 },
    { "intersection",         sm_intersection,         1, -1 },
    { "difference",           sm_difference,           1, -1 },
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
    struct pysobj *o = PYS_PTR(argv[0]);
    return pys_make_str(o->str.chars, o->str.len);
}

static VALUE
bm_encode(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    return pys_make_bytes(o->str.chars, o->str.len);
}

// (find / endswith with start/end defined later in file)
static VALUE
bm_startswith(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *s = PYS_PTR(argv[0]);
    int64_t slen = (int64_t)s->str.len;
    int64_t start = (argc >= 3 && argv[2] != PYS_NONE) ? pys_int_to_long(c, argv[2]) : 0;
    int64_t end   = (argc >= 4 && argv[3] != PYS_NONE) ? pys_int_to_long(c, argv[3]) : slen;
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    int64_t span = end - start;
    if (span < 0) span = 0;
    const char *base = s->str.chars + start;
    VALUE arg = argv[1];
    if (pys_is_tuple(arg)) {
        size_t n = PYS_PTR(arg)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE p = PYS_PTR(arg)->list.items[i];
            if (!pys_is_byteseq(p)) continue;
            struct pysobj *pp = PYS_PTR(p);
            if ((int64_t)pp->str.len > span) continue;
            if (memcmp(base, pp->str.chars, pp->str.len) == 0) return PYS_TRUE;
        }
        return PYS_FALSE;
    }
    if (!pys_is_byteseq(arg)) PYS_RAISE_EXC(c, c->EXC_TypeError, "startswith: not bytes/tuple");
    struct pysobj *p = PYS_PTR(arg);
    if ((int64_t)p->str.len > span) return PYS_FALSE;
    return memcmp(base, p->str.chars, p->str.len) == 0 ? PYS_TRUE : PYS_FALSE;
}

static VALUE
bm_split(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *s = PYS_PTR(argv[0]);
    VALUE result = pys_make_list(NULL, 0);
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
            pys_list_append(c, result, pys_make_bytes(s->str.chars + i, j - i));
            i = j;
        }
        return result;
    }
    if (!pys_is_byteseq(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "bytes.split sep must be bytes");
    struct pysobj *sep = PYS_PTR(argv[1]);
    if (sep->str.len == 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "empty separator");
    size_t i = 0;
    while (i <= s->str.len) {
        const char *p = i + sep->str.len <= s->str.len
            ? memmem(s->str.chars + i, s->str.len - i, sep->str.chars, sep->str.len) : NULL;
        if (!p) { pys_list_append(c, result, pys_make_bytes(s->str.chars + i, s->str.len - i)); break; }
        pys_list_append(c, result, pys_make_bytes(s->str.chars + i, (size_t)(p - (s->str.chars + i))));
        i = (size_t)(p - s->str.chars) + sep->str.len;
    }
    return result;
}

static VALUE
bm_replace(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    if (!pys_is_byteseq(argv[1]) || !pys_is_byteseq(argv[2]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "bytes.replace args must be bytes");
    struct pysobj *o = PYS_PTR(argv[1]);
    struct pysobj *r = PYS_PTR(argv[2]);
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
    struct pysobj *out = pys_alloc(PYS_T_BYTES);
    out->str.chars = buf; out->str.len = len;
    return PYS_OBJ_VAL(out);
}

static VALUE
bm_hex(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *s = PYS_PTR(argv[0]);
    static const char hexd[] = "0123456789abcdef";
    // Optional sep + bytes_per_sep (CPython 3.8+).
    char sep = 0;
    int  bps = 1;
    if (argc >= 2) {
        if (!pys_is_str(argv[1]) || PYS_PTR(argv[1])->str.len != 1)
            PYS_RAISE_EXC(c, c->EXC_TypeError, "sep must be a 1-char string");
        sep = PYS_PTR(argv[1])->str.chars[0];
    }
    if (argc >= 3) {
        bps = (int)pys_int_to_long(c, argv[2]);
        if (bps == 0) bps = 1;
    }
    size_t L = s->str.len;
    size_t out_cap = L * 2 + (L > 0 && sep ? L : 0) + 1;
    char *buf = (char *)GC_malloc_atomic(out_cap);
    size_t bi = 0;
    int abs_bps = bps < 0 ? -bps : bps;
    for (size_t i = 0; i < L; i++) {
        if (sep && i > 0) {
            // bps > 0: count groups from the right; bps < 0: from left.
            int idx_from_right = (int)(L - i);
            int idx_from_left  = (int)i;
            int boundary = (bps > 0) ? (idx_from_right % abs_bps == 0)
                                     : (idx_from_left  % abs_bps == 0);
            if (boundary) buf[bi++] = sep;
        }
        unsigned char ch = (unsigned char)s->str.chars[i];
        buf[bi++] = hexd[ch >> 4];
        buf[bi++] = hexd[ch & 0xf];
    }
    buf[bi] = '\0';
    return pys_make_str_take(buf, bi);
}

static VALUE
bm_append(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (PYS_PTR(argv[0])->type != PYS_T_BYTEARRAY)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "append on bytes (not bytearray)");
    int64_t b = pys_int_to_long(c, argv[1]);
    if (b < 0 || b > 255) PYS_RAISE_EXC(c, c->EXC_ValueError, "byte must be 0..255");
    struct pysobj *o = PYS_PTR(argv[0]);
    size_t L = o->str.len;
    char *nb = (char *)GC_malloc_atomic(L + 2);
    if (L > 0) memcpy(nb, o->str.chars, L);
    nb[L] = (char)b;
    nb[L + 1] = '\0';
    o->str.chars = nb;
    o->str.len = L + 1;
    return PYS_NONE;
}

static VALUE
bm_insert(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (PYS_PTR(argv[0])->type != PYS_T_BYTEARRAY)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "insert on bytes (not bytearray)");
    int64_t i = pys_int_to_long(c, argv[1]);
    int64_t b = pys_int_to_long(c, argv[2]);
    if (b < 0 || b > 255) PYS_RAISE_EXC(c, c->EXC_ValueError, "byte must be 0..255");
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t L = (int64_t)o->str.len;
    if (i < 0) i += L;
    if (i < 0) i = 0;
    if (i > L) i = L;
    char *nb = (char *)GC_malloc_atomic(L + 2);
    if (i > 0) memcpy(nb, o->str.chars, (size_t)i);
    nb[i] = (char)b;
    if (L - i > 0) memcpy(nb + i + 1, o->str.chars + i, (size_t)(L - i));
    nb[L + 1] = '\0';
    o->str.chars = nb;
    o->str.len = (size_t)(L + 1);
    return PYS_NONE;
}

static VALUE
bm_pop(CTX *c, int argc, VALUE *argv)
{
    if (PYS_PTR(argv[0])->type != PYS_T_BYTEARRAY)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "pop on bytes (not bytearray)");
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t L = (int64_t)o->str.len;
    if (L == 0) PYS_RAISE_EXC(c, c->EXC_IndexError, "pop from empty bytearray");
    int64_t i = (argc >= 2) ? pys_int_to_long(c, argv[1]) : (L - 1);
    if (i < 0) i += L;
    if (i < 0 || i >= L) PYS_RAISE_EXC(c, c->EXC_IndexError, "bytearray pop out of range");
    int byte = (unsigned char)o->str.chars[i];
    char *nb = (char *)GC_malloc_atomic(L);
    if (i > 0) memcpy(nb, o->str.chars, (size_t)i);
    if (L - i - 1 > 0) memcpy(nb + i, o->str.chars + i + 1, (size_t)(L - i - 1));
    nb[L - 1] = '\0';
    o->str.chars = nb;
    o->str.len = (size_t)(L - 1);
    return PYS_FIX(byte);
}

static VALUE
bm_remove(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (PYS_PTR(argv[0])->type != PYS_T_BYTEARRAY)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "remove on bytes (not bytearray)");
    int64_t target = pys_int_to_long(c, argv[1]);
    if (target < 0 || target > 255)
        PYS_RAISE_EXC(c, c->EXC_ValueError, "byte must be 0..255");
    struct pysobj *o = PYS_PTR(argv[0]);
    size_t L = o->str.len;
    for (size_t i = 0; i < L; i++) {
        if ((unsigned char)o->str.chars[i] == (unsigned char)target) {
            char *nb = (char *)GC_malloc_atomic(L);
            if (i > 0) memcpy(nb, o->str.chars, i);
            if (L - i - 1 > 0) memcpy(nb + i, o->str.chars + i + 1, L - i - 1);
            nb[L - 1] = '\0';
            o->str.chars = nb;
            o->str.len = L - 1;
            return PYS_NONE;
        }
    }
    PYS_RAISE_EXC(c, c->EXC_ValueError, "value not in bytearray");
}

static VALUE
bm_reverse(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    if (PYS_PTR(argv[0])->type != PYS_T_BYTEARRAY)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "reverse on bytes (not bytearray)");
    struct pysobj *o = PYS_PTR(argv[0]);
    size_t L = o->str.len;
    for (size_t i = 0; i < L / 2; i++) {
        char tmp = o->str.chars[i];
        o->str.chars[i] = o->str.chars[L - 1 - i];
        o->str.chars[L - 1 - i] = tmp;
    }
    return PYS_NONE;
}

static VALUE
bm_clear(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    if (PYS_PTR(argv[0])->type != PYS_T_BYTEARRAY)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "clear on bytes (not bytearray)");
    struct pysobj *o = PYS_PTR(argv[0]);
    o->str.chars = (char *)GC_malloc_atomic(1);
    o->str.chars[0] = '\0';
    o->str.len = 0;
    return PYS_NONE;
}

static VALUE
bm_extend(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (PYS_PTR(argv[0])->type != PYS_T_BYTEARRAY)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "extend on bytes (not bytearray)");
    struct pysobj *o = PYS_PTR(argv[0]);
    if (pys_is_byteseq(argv[1])) {
        size_t add = PYS_PTR(argv[1])->str.len;
        size_t L = o->str.len;
        char *nb = (char *)GC_malloc_atomic(L + add + 1);
        if (L > 0) memcpy(nb, o->str.chars, L);
        memcpy(nb + L, PYS_PTR(argv[1])->str.chars, add);
        nb[L + add] = '\0';
        o->str.chars = nb;
        o->str.len = L + add;
        return PYS_NONE;
    }
    // Iterable of ints.
    struct pys_iter it; pys_iter_init(c, &it, argv[1]);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) {
        VALUE av[2] = { argv[0], x };
        bm_append(c, 2, av);
    }
    return PYS_NONE;
}

// int methods.  Operate on fixnums and bignums.
static VALUE
im_bit_length(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (PYS_IS_FIXNUM(v)) {
        int64_t x = PYS_FIXVAL(v);
        if (x < 0) x = -x;
        int n = 0;
        while (x) { n++; x >>= 1; }
        return PYS_FIX(n);
    }
    if (v == PYS_TRUE)  return PYS_FIX(1);
    if (v == PYS_FALSE) return PYS_FIX(0);
    if (pys_is_bignum(v)) {
        mpz_t z; pys_to_mpz(c, v, z);
        size_t n = mpz_sizeinbase(z, 2);
        // mpz_sizeinbase returns 1 for 0; CPython returns 0.
        if (mpz_sgn(z) == 0) n = 0;
        mpz_clear(z);
        return PYS_FIX((int64_t)n);
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "bit_length: int required");
}

static VALUE
im_bit_count(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (PYS_IS_FIXNUM(v)) {
        uint64_t x = (uint64_t)(PYS_FIXVAL(v) < 0 ? -PYS_FIXVAL(v) : PYS_FIXVAL(v));
        return PYS_FIX(__builtin_popcountll(x));
    }
    if (v == PYS_TRUE)  return PYS_FIX(1);
    if (v == PYS_FALSE) return PYS_FIX(0);
    if (pys_is_bignum(v)) {
        mpz_t z; pys_to_mpz(c, v, z);
        if (mpz_sgn(z) < 0) mpz_neg(z, z);
        mp_bitcnt_t n = mpz_popcount(z);
        mpz_clear(z);
        return PYS_FIX((int64_t)n);
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "bit_count: int required");
}

static VALUE
im_to_bytes(CTX *c, int argc, VALUE *argv)
{
    // self.to_bytes(length, byteorder='big', signed=False)
    VALUE self = argv[0];
    int64_t length = pys_int_to_long(c, argv[1]);
    const char *order = "big";
    if (argc >= 3) {
        if (!pys_is_str(argv[2])) PYS_RAISE_EXC(c, c->EXC_TypeError, "byteorder must be str");
        order = PYS_PTR(argv[2])->str.chars;
    }
    bool is_signed = false;
    VALUE sk = pys_bi_kwarg("signed");
    if (sk) is_signed = pys_is_truthy(sk);
    VALUE bk = pys_bi_kwarg("byteorder");
    if (bk && pys_is_str(bk)) order = PYS_PTR(bk)->str.chars;
    bool big = strcmp(order, "big") == 0;
    if (length < 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "length must be non-negative");
    char *buf = (char *)GC_malloc_atomic(length + 1);
    memset(buf, 0, length);
    mpz_t z; pys_to_mpz(c, self, z);
    bool neg = mpz_sgn(z) < 0;
    if (neg) {
        if (!is_signed) {
            mpz_clear(z);
            PYS_RAISE_EXC(c, c->EXC_OverflowError, "can't convert negative int to unsigned");
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
            PYS_RAISE_EXC(c, c->EXC_OverflowError, "int too big to convert");
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
    VALUE r = pys_make_bytes(buf, (size_t)length);
    return r;
}

static VALUE
im_to_bytes_method(CTX *c, int argc, VALUE *argv) { return im_to_bytes(c, argc, argv); }

static VALUE
im_int_index(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

static VALUE
im_real(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

static VALUE
im_imag(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return PYS_FIX(0); }

static VALUE
im_numerator(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

static VALUE
im_denominator(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return PYS_FIX(1); }

static VALUE
im_conjugate(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

static struct type_method int_methods[] = {
    { "bit_length",  im_bit_length, 1, 1, 0 },
    { "bit_count",   im_bit_count,  1, 1, 0 },
    { "to_bytes",    im_to_bytes_method, 2, 3, 0 },
    { "__index__",   im_int_index,  1, 1, 0 },
    { "__int__",     im_int_index,  1, 1, 0 },
    { "real",        im_real,       1, 1, 1 },
    { "imag",        im_imag,       1, 1, 1 },
    { "numerator",   im_numerator,  1, 1, 1 },
    { "denominator", im_denominator,1, 1, 1 },
    { "conjugate",   im_conjugate,  1, 1, 0 },
    { NULL, NULL, 0, 0, 0 }
};

// float methods
static VALUE
fm_is_integer(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    double d = PYS_IS_FLONUM(v) ? pys_flonum_to_double(v) : PYS_PTR(v)->dbl;
    if (d != d) return PYS_FALSE;          // NaN
    if (d == (double)(int64_t)d) return PYS_TRUE;
    return PYS_FALSE;
}

static VALUE
fm_hex(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    double d = PYS_IS_FLONUM(v) ? pys_flonum_to_double(v) : PYS_PTR(v)->dbl;
    char buf[64];
    snprintf(buf, sizeof(buf), "%a", d);
    // CPython uses "0x1.8p+0" form, glibc %a matches.
    return pys_make_str(buf, strlen(buf));
}

static VALUE
fm_real_f(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }
static VALUE
fm_imag_f(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return pys_make_float(0.0); }
static VALUE
fm_conj_f(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return argv[0]; }

// (a, b) such that float == a/b exactly, with b > 0 and gcd(a, b) == 1.
// Mirrors CPython's float.as_integer_ratio.
static VALUE
fm_as_integer_ratio(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    double d = pys_to_double(c, argv[0]);
    if (d != d) PYS_RAISE_EXC(c, c->EXC_ValueError, "cannot convert NaN to ratio");
    if (d != 0.0 && d == d * 0.5)
        PYS_RAISE_EXC(c, c->EXC_OverflowError, "cannot convert Infinity to ratio");
    if (d == 0.0) {
        VALUE pair[2] = { PYS_FIX(0), PYS_FIX(1) };
        return pys_make_tuple(pair, 2);
    }
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
    pair[0] = pys_normalise_int(num);
    pair[1] = pys_normalise_int(den);
    mpz_clear(num); mpz_clear(den);
    return pys_make_tuple(pair, 2);
}

static struct type_method float_methods[] = {
    { "is_integer", fm_is_integer, 1, 1, 0 },
    { "hex",        fm_hex,        1, 1, 0 },
    { "as_integer_ratio", fm_as_integer_ratio, 1, 1, 0 },
    { "real",       fm_real_f,     1, 1, 1 },
    { "imag",       fm_imag_f,     1, 1, 1 },
    { "conjugate",  fm_conj_f,     1, 1, 0 },
    { NULL, NULL, 0, 0, 0 }
};

// complex methods
static VALUE
cm_real(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return pys_make_float(PYS_PTR(argv[0])->cpx.re); }
static VALUE
cm_imag(CTX *c, int argc, VALUE *argv) { (void)c; (void)argc; return pys_make_float(PYS_PTR(argv[0])->cpx.im); }
static VALUE
cm_conj(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    return pys_make_complex(PYS_PTR(argv[0])->cpx.re, -PYS_PTR(argv[0])->cpx.im);
}
static struct type_method complex_methods[] = {
    { "real",      cm_real, 1, 1, 1 },
    { "imag",      cm_imag, 1, 1, 1 },
    { "conjugate", cm_conj, 1, 1, 0 },
    { NULL, NULL, 0, 0, 0 }
};

// tuple methods (read-only)
static VALUE
tm_index(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t start = (argc >= 3) ? pys_int_to_long(c, argv[2]) : 0;
    int64_t stop  = (argc >= 4) ? pys_int_to_long(c, argv[3]) : (int64_t)o->list.len;
    if (start < 0) start += (int64_t)o->list.len;
    if (start < 0) start = 0;
    if (stop < 0) stop += (int64_t)o->list.len;
    if (stop > (int64_t)o->list.len) stop = (int64_t)o->list.len;
    for (int64_t i = start; i < stop; i++) {
        if (i >= (int64_t)o->list.len) break;
        VALUE elt = o->list.items[i];
        if (elt == argv[1]) return PYS_FIX(i);
        bool eq = pys_eq_bool(c, elt, argv[1]);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        if (eq) return PYS_FIX(i);
    }
    PYS_RAISE_EXC(c, c->EXC_ValueError, "value not in tuple");
}
static VALUE
tm_count(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t n = 0;
    for (size_t i = 0; ; i++) {
        if (i >= o->list.len) break;
        VALUE elt = o->list.items[i];
        if (elt == argv[1]) { n++; continue; }
        bool eq = pys_eq_bool(c, elt, argv[1]);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        if (eq) n++;
    }
    return PYS_FIX(n);
}
static struct type_method tuple_methods[] = {
    { "index", tm_index, 2, 4 },
    { "count", tm_count, 2, 2 },
    { NULL, NULL, 0, 0 }
};

// range methods + start/stop/step (also exposed via pys_getattr below)
static VALUE
rm_index(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    int64_t target = pys_int_to_long(c, argv[1]);
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t start = o->range.start, stop = o->range.stop, step = o->range.step;
    if (step > 0) {
        if (target < start || target >= stop) PYS_RAISE_EXC(c, c->EXC_ValueError, "not in range");
        if ((target - start) % step != 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "not in range");
        return PYS_FIX((target - start) / step);
    } else {
        if (target > start || target <= stop) PYS_RAISE_EXC(c, c->EXC_ValueError, "not in range");
        if ((start - target) % (-step) != 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "not in range");
        return PYS_FIX((start - target) / step);
    }
}
static VALUE
rm_count(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_int_or_bool(argv[1])) return PYS_FIX(0);
    int64_t target = pys_int_to_long(c, argv[1]);
    struct pysobj *o = PYS_PTR(argv[0]);
    int64_t start = o->range.start, stop = o->range.stop, step = o->range.step;
    if (step > 0) {
        if (target < start || target >= stop) return PYS_FIX(0);
        return ((target - start) % step == 0) ? PYS_FIX(1) : PYS_FIX(0);
    } else {
        if (target > start || target <= stop) return PYS_FIX(0);
        return ((start - target) % (-step) == 0) ? PYS_FIX(1) : PYS_FIX(0);
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
    if (argc < 1) PYS_RAISE_EXC(c, c->EXC_TypeError, "from_bytes: missing argument");
    const char *order = "big";
    if (argc >= 2 && pys_is_str(argv[1])) order = PYS_PTR(argv[1])->str.chars;
    bool is_signed = false;
    VALUE sk = pys_bi_kwarg("signed");
    if (sk) is_signed = pys_is_truthy(sk);
    VALUE bk = pys_bi_kwarg("byteorder");
    if (bk && pys_is_str(bk)) order = PYS_PTR(bk)->str.chars;
    bool big = strcmp(order, "big") == 0;

    // Resolve buffer: bytes / bytearray / memoryview / list / tuple.
    const char *buf = NULL;
    size_t n = 0;
    unsigned char *tmp = NULL;
    VALUE v = argv[0];
    if (pys_is_bytes(v) || pys_is_bytearray(v)) {
        buf = PYS_PTR(v)->str.chars;
        n = PYS_PTR(v)->str.len;
    } else if (PYS_IS_PTR(v) && PYS_PTR(v)->type == PYS_T_MEMVIEW) {
        struct pysobj *mv = PYS_PTR(v);
        buf = PYS_PTR(mv->memview.source)->str.chars + mv->memview.off;
        n = mv->memview.len;
    } else if (PYS_IS_PTR(v) && (PYS_PTR(v)->type == PYS_T_LIST ||
                                  PYS_PTR(v)->type == PYS_T_TUPLE)) {
        struct pysobj *seq = PYS_PTR(v);
        n = seq->list.len;
        tmp = (unsigned char *)GC_malloc_atomic(n + 1);
        for (size_t i = 0; i < n; i++) {
            VALUE iv = seq->list.items[i];
            if (!pys_is_int(iv))
                PYS_RAISE_EXC(c, c->EXC_TypeError, "from_bytes: items must be ints");
            long b = pys_int_to_long(c, iv);
            if (b < 0 || b > 255)
                PYS_RAISE_EXC(c, c->EXC_ValueError, "from_bytes: byte must be in range(0, 256)");
            tmp[i] = (unsigned char)b;
        }
        buf = (const char *)tmp;
    } else {
        // Generic iterable fallback: walk via __iter__/__next__ and
        // collect ints in [0,256).  Used by `int.from_bytes(map(...))`.
        struct pys_iter it; pys_iter_init(c, &it, v);
        if (c->state == PYS_STATE_RAISE) return 0;
        size_t cap = 32;
        tmp = (unsigned char *)GC_malloc_atomic(cap);
        n = 0;
        VALUE iv;
        while (pys_iter_next(c, &it, &iv)) {
            if (!pys_is_int(iv))
                PYS_RAISE_EXC(c, c->EXC_TypeError, "from_bytes: items must be ints");
            long b = pys_int_to_long(c, iv);
            if (b < 0 || b > 255)
                PYS_RAISE_EXC(c, c->EXC_ValueError, "from_bytes: byte must be in range(0, 256)");
            if (n + 1 >= cap) {
                cap *= 2;
                unsigned char *nb = (unsigned char *)GC_malloc_atomic(cap);
                memcpy(nb, tmp, n);
                tmp = nb;
            }
            tmp[n++] = (unsigned char)b;
        }
        if (c->state == PYS_STATE_RAISE) return 0;
        buf = (const char *)tmp;
    }

    mpz_t z; mpz_init(z);
    for (size_t i = 0; i < n; i++) {
        size_t k = big ? i : (n - 1 - i);
        unsigned char b = (unsigned char)buf[k];
        mpz_mul_ui(z, z, 256);
        mpz_add_ui(z, z, b);
    }
    if (is_signed && n > 0) {
        unsigned char first = (unsigned char)buf[big ? 0 : n - 1];
        if (first & 0x80) {
            mpz_t cap; mpz_init(cap);
            mpz_ui_pow_ui(cap, 2, (unsigned long)(8 * n));
            mpz_sub(z, z, cap);
            mpz_clear(cap);
        }
    }
    VALUE r = pys_normalise_int(z);
    mpz_clear(z);
    return r;
}

// bytes.fromhex(s)
static VALUE
bi_bytes_fromhex(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "fromhex: str required");
    const char *s = PYS_PTR(argv[0])->str.chars;
    size_t n = PYS_PTR(argv[0])->str.len;
    char *buf = (char *)GC_malloc_atomic(n / 2 + 1);
    size_t out = 0;
    int hi = -1;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\n') continue;
        int d = (ch >= '0' && ch <= '9') ? ch - '0'
              : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
              : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (d < 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "non-hex digit in fromhex");
        if (hi < 0) hi = d;
        else { buf[out++] = (char)((hi << 4) | d); hi = -1; }
    }
    if (hi >= 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "odd-length hex string");
    return pys_make_bytes(buf, out);
}

// float.fromhex(s)
static VALUE
bi_float_fromhex(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "fromhex: str required");
    char *end;
    double d = strtod(PYS_PTR(argv[0])->str.chars, &end);
    return pys_make_float(d);
}

// float.__getformat__("double" | "float") — CPython exposes the host's
// IEEE 754 layout as a string.  We check endianness at runtime.
// Treated as a class-level static call: argv[0] is the typecode arg.
static VALUE
bi_float_getformat(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "__getformat__: str required");
    union { uint32_t u; uint8_t b[4]; } probe = { 0x01020304 };
    const char *be = "IEEE, big-endian";
    const char *le = "IEEE, little-endian";
    const char *r = (probe.b[0] == 1) ? be : le;
    return pys_make_str(r, strlen(r));
}

// dict.fromkeys([cls,] iter[, default=None]).  Accepts both the
// classmethod form (called as `dict.fromkeys(cls, iter, default)`) and
// the bare-call form (`dict.fromkeys(iter, default)`).  For user
// subclasses (e.g. `class dictlike(dict)`), instantiate cls() so the
// return value is an instance of the subclass — CPython parity.
static VALUE
bi_dict_fromkeys(CTX *c, int argc, VALUE *argv)
{
    VALUE cls = 0;
    if (argc >= 2 && pys_is_class(argv[0])) {
        cls = argv[0];
        argv++;
        argc--;
    }
    if (argc < 1) {
        PYS_RAISE_EXC(c, c->EXC_TypeError, "fromkeys: missing iterable arg");
    }
    VALUE def = argc >= 2 ? argv[1] : PYS_NONE;
    // Result container: user subclass → instantiate via cls() so
    // `MyDictSubclass.fromkeys('a')` returns a MyDictSubclass instance.
    VALUE r;
    if (cls && cls != c->TYPE_dict) {
        r = pys_apply(c, cls, 0, NULL);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    } else {
        r = pys_make_dict();
    }
    struct pys_iter it; pys_iter_init(c, &it, argv[0]);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE k;
    while (pys_iter_next(c, &it, &k)) {
        // Use list_set so user __setitem__ overrides take effect.
        pys_list_set(c, r, k, def);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
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
    const char *sep = PYS_PTR(self)->str.chars;
    size_t slen = PYS_PTR(self)->str.len;
    VALUE items[256]; int n = 0;
    if (pys_is_list(iter) || pys_is_tuple(iter)) {
        size_t sn = PYS_PTR(iter)->list.len;
        if (sn > 256) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "bytes.join too many");
        for (size_t i = 0; i < sn; i++) items[n++] = PYS_PTR(iter)->list.items[i];
    } else {
        struct pys_iter it; pys_iter_init(c, &it, iter);
        if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
            if (n >= 256) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "bytes.join too many");
            items[n++] = x;
        }
    }
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        if (!pys_is_byteseq(items[i]))
            PYS_RAISE_EXC(c, c->EXC_TypeError, "bytes.join element must be bytes-like");
        total += PYS_PTR(items[i])->str.len;
        if (i) total += slen;
    }
    char *buf = (char *)GC_malloc_atomic(total + 1);
    char *p = buf;
    for (int i = 0; i < n; i++) {
        if (i) { memcpy(p, sep, slen); p += slen; }
        memcpy(p, PYS_PTR(items[i])->str.chars, PYS_PTR(items[i])->str.len);
        p += PYS_PTR(items[i])->str.len;
    }
    *p = '\0';
    return pys_is_bytearray(self) ? pys_make_bytearray(buf, total) : pys_make_bytes(buf, total);
}

// bytes.count(sub)
static VALUE
bm_count(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    if (!pys_is_byteseq(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "bytes.count: bytes-like required");
    struct pysobj *sub = PYS_PTR(argv[1]);
    if (sub->str.len == 0) return PYS_FIX((int64_t)s->str.len + 1);
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
    return PYS_FIX(count);
}

// bytes.upper / lower
static VALUE
bm_upper(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    return pys_is_bytearray(argv[0]) ? pys_make_bytearray(buf, s->str.len)
                                    : pys_make_bytes(buf, s->str.len);
}

static VALUE
bm_lower(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    return pys_is_bytearray(argv[0]) ? pys_make_bytearray(buf, s->str.len)
                                    : pys_make_bytes(buf, s->str.len);
}

// bytes additional methods.
static VALUE
bm_title(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
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
    return pys_is_bytearray(argv[0]) ? pys_make_bytearray(buf, s->str.len)
                                    : pys_make_bytes(buf, s->str.len);
}

static VALUE
bm_capitalize(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    if (s->str.len == 0)
        return pys_is_bytearray(argv[0]) ? pys_make_bytearray("", 0)
                                        : pys_make_bytes("", 0);
    char *buf = (char *)GC_malloc_atomic(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (i == 0) {
            if (ch >= 'a' && ch <= 'z') ch -= 32;
        } else if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    return pys_is_bytearray(argv[0]) ? pys_make_bytearray(buf, s->str.len)
                                    : pys_make_bytes(buf, s->str.len);
}

static VALUE
bm_swapcase(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    char *buf = (char *)GC_malloc_atomic(s->str.len + 1);
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        else if (ch >= 'A' && ch <= 'Z') ch += 32;
        buf[i] = ch;
    }
    buf[s->str.len] = '\0';
    return pys_is_bytearray(argv[0]) ? pys_make_bytearray(buf, s->str.len)
                                    : pys_make_bytes(buf, s->str.len);
}

static VALUE
bm_zfill(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    int64_t w = pys_int_to_long(c, argv[1]);
    if ((int64_t)s->str.len >= w)
        return pys_is_bytearray(argv[0]) ? pys_make_bytearray(s->str.chars, s->str.len)
                                        : pys_make_bytes(s->str.chars, s->str.len);
    char *buf = (char *)GC_malloc_atomic(w + 1);
    int sign = 0;
    if (s->str.len > 0 && (s->str.chars[0] == '-' || s->str.chars[0] == '+')) {
        buf[0] = s->str.chars[0]; sign = 1;
    }
    int64_t pad = w - (int64_t)s->str.len;
    for (int64_t i = 0; i < pad; i++) buf[sign + i] = '0';
    memcpy(buf + sign + pad, s->str.chars + sign, s->str.len - sign);
    buf[w] = '\0';
    return pys_is_bytearray(argv[0]) ? pys_make_bytearray(buf, w)
                                    : pys_make_bytes(buf, w);
}

static VALUE
bm_pad_helper(CTX *c, int argc, VALUE *argv, int align)
{
    (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    int64_t w = pys_int_to_long(c, argv[1]);
    char fill = ' ';
    if (argc >= 3 && pys_is_byteseq(argv[2])
        && PYS_PTR(argv[2])->str.len == 1) fill = PYS_PTR(argv[2])->str.chars[0];
    if ((int64_t)s->str.len >= w)
        return pys_is_bytearray(argv[0]) ? pys_make_bytearray(s->str.chars, s->str.len)
                                        : pys_make_bytes(s->str.chars, s->str.len);
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
    return pys_is_bytearray(argv[0]) ? pys_make_bytearray(buf, w)
                                    : pys_make_bytes(buf, w);
}

static VALUE bm_ljust(CTX *c, int argc, VALUE *argv)  { return bm_pad_helper(c, argc, argv, 0); }
static VALUE bm_rjust(CTX *c, int argc, VALUE *argv)  { return bm_pad_helper(c, argc, argv, 1); }
static VALUE bm_center(CTX *c, int argc, VALUE *argv) { return bm_pad_helper(c, argc, argv, 2); }

static VALUE
bm_isalpha(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    if (s->str.len == 0) return PYS_FALSE;
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) return PYS_FALSE;
    }
    return PYS_TRUE;
}

static VALUE
bm_isdigit(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    if (s->str.len == 0) return PYS_FALSE;
    for (size_t i = 0; i < s->str.len; i++)
        if (s->str.chars[i] < '0' || s->str.chars[i] > '9') return PYS_FALSE;
    return PYS_TRUE;
}

static VALUE
bm_isspace(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    if (s->str.len == 0) return PYS_FALSE;
    for (size_t i = 0; i < s->str.len; i++) {
        char ch = s->str.chars[i];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\v' && ch != '\f')
            return PYS_FALSE;
    }
    return PYS_TRUE;
}

static VALUE
bm_find(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    if (!pys_is_byteseq(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "find: not bytes");
    struct pysobj *p = PYS_PTR(argv[1]);
    int64_t slen = (int64_t)s->str.len;
    int64_t start = (argc >= 3 && argv[2] != PYS_NONE) ? pys_int_to_long(c, argv[2]) : 0;
    int64_t end   = (argc >= 4 && argv[3] != PYS_NONE) ? pys_int_to_long(c, argv[3]) : slen;
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    if (start > end) return PYS_FIX(-1);
    if (p->str.len == 0) return PYS_FIX(start);
    if ((size_t)(end - start) < p->str.len) return PYS_FIX(-1);
    void *r = memmem(s->str.chars + start, (size_t)(end - start), p->str.chars, p->str.len);
    return r ? PYS_FIX((int64_t)((char *)r - s->str.chars)) : PYS_FIX(-1);
}

static VALUE
bm_rfind(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *s = PYS_PTR(argv[0]);
    if (!pys_is_byteseq(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "rfind: not bytes");
    struct pysobj *p = PYS_PTR(argv[1]);
    int64_t slen = (int64_t)s->str.len;
    int64_t start = (argc >= 3 && argv[2] != PYS_NONE) ? pys_int_to_long(c, argv[2]) : 0;
    int64_t end   = (argc >= 4 && argv[3] != PYS_NONE) ? pys_int_to_long(c, argv[3]) : slen;
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    if (p->str.len == 0) return PYS_FIX(end);
    if ((int64_t)p->str.len > end - start) return PYS_FIX(-1);
    for (int64_t i = end - (int64_t)p->str.len; i >= start; i--)
        if (memcmp(s->str.chars + i, p->str.chars, p->str.len) == 0) return PYS_FIX(i);
    return PYS_FIX(-1);
}

static VALUE
bm_index(CTX *c, int argc, VALUE *argv)
{
    VALUE r = bm_find(c, argc, argv);
    if (PYS_IS_FIXNUM(r) && PYS_FIXVAL(r) == -1)
        PYS_RAISE_EXC(c, c->EXC_ValueError, "subsection not found");
    return r;
}

static VALUE
bm_rindex(CTX *c, int argc, VALUE *argv)
{
    VALUE r = bm_rfind(c, argc, argv);
    if (PYS_IS_FIXNUM(r) && PYS_FIXVAL(r) == -1)
        PYS_RAISE_EXC(c, c->EXC_ValueError, "subsection not found");
    return r;
}

static VALUE
bm_endswith(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *s = PYS_PTR(argv[0]);
    int64_t slen = (int64_t)s->str.len;
    int64_t start = (argc >= 3 && argv[2] != PYS_NONE) ? pys_int_to_long(c, argv[2]) : 0;
    int64_t end   = (argc >= 4 && argv[3] != PYS_NONE) ? pys_int_to_long(c, argv[3]) : slen;
    { if (start < 0) start += slen; if (start < 0) start = 0; if (start > slen) start = slen; }
    { if (end < 0) end += slen; if (end < 0) end = 0; if (end > slen) end = slen; }
    int64_t span = end - start;
    if (span < 0) span = 0;
    const char *tail_end = s->str.chars + end;
    VALUE arg = argv[1];
    if (pys_is_tuple(arg)) {
        size_t n = PYS_PTR(arg)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE p = PYS_PTR(arg)->list.items[i];
            if (!pys_is_byteseq(p)) continue;
            struct pysobj *pp = PYS_PTR(p);
            if ((int64_t)pp->str.len > span) continue;
            if (memcmp(tail_end - pp->str.len, pp->str.chars, pp->str.len) == 0) return PYS_TRUE;
        }
        return PYS_FALSE;
    }
    if (!pys_is_byteseq(arg)) PYS_RAISE_EXC(c, c->EXC_TypeError, "endswith: not bytes/tuple");
    struct pysobj *p = PYS_PTR(arg);
    if ((int64_t)p->str.len > span) return PYS_FALSE;
    return memcmp(tail_end - p->str.len, p->str.chars, p->str.len) == 0 ? PYS_TRUE : PYS_FALSE;
}

// bytes.strip / lstrip / rstrip — whitespace by default, or any byte
// in the bytes-like arg passed as argv[1].
static VALUE
bm_strip_impl(CTX *c, int argc, VALUE *argv, bool left, bool right)
{
    (void)c;
    struct pysobj *s = PYS_PTR(argv[0]);
    const unsigned char *chars = (const unsigned char *)s->str.chars;
    size_t start = 0, end = s->str.len;
    const unsigned char *set = NULL;
    size_t nset = 0;
    if (argc >= 2 && argv[1] != PYS_NONE) {
        if (!pys_is_byteseq(argv[1]))
            PYS_RAISE_EXC(c, c->EXC_TypeError, "strip: a bytes-like object is required");
        struct pysobj *sp = PYS_PTR(argv[1]);
        set = (const unsigned char *)sp->str.chars;
        nset = sp->str.len;
    }
    #define IS_STRIP(ch) (set ? ({                              \
        bool _hit = false;                                      \
        for (size_t _i = 0; _i < nset; _i++)                    \
            if (set[_i] == (ch)) { _hit = true; break; }        \
        _hit;                                                   \
    }) : ((ch) == ' ' || (ch) == '\t' || (ch) == '\n' || (ch) == '\r'))
    if (left)  while (start < end && IS_STRIP(chars[start])) start++;
    if (right) while (end > start && IS_STRIP(chars[end - 1])) end--;
    #undef IS_STRIP
    char *buf = (char *)GC_malloc_atomic(end - start + 1);
    memcpy(buf, chars + start, end - start);
    buf[end - start] = '\0';
    return pys_is_bytearray(argv[0]) ? pys_make_bytearray(buf, end - start)
                                    : pys_make_bytes(buf, end - start);
}
static VALUE bm_strip(CTX *c, int argc, VALUE *argv)  { return bm_strip_impl(c, argc, argv, true, true); }
static VALUE bm_lstrip(CTX *c, int argc, VALUE *argv) { return bm_strip_impl(c, argc, argv, true, false); }
static VALUE bm_rstrip(CTX *c, int argc, VALUE *argv) { return bm_strip_impl(c, argc, argv, false, true); }

// bytes.maketrans(from, to) → 256-byte translation table.
static VALUE
bi_bytes_maketrans(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_byteseq(argv[0]) || !pys_is_byteseq(argv[1]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "bytes.maketrans requires bytes-like args");
    struct pysobj *a = PYS_PTR(argv[0]);
    struct pysobj *b = PYS_PTR(argv[1]);
    if (a->str.len != b->str.len)
        PYS_RAISE_EXC(c, c->EXC_ValueError, "maketrans: from and to differ in length");
    char *table = (char *)GC_malloc_atomic(256);
    for (int i = 0; i < 256; i++) table[i] = (char)i;
    for (size_t i = 0; i < a->str.len; i++) {
        unsigned char fk = (unsigned char)a->str.chars[i];
        table[fk] = b->str.chars[i];
    }
    return pys_make_bytes(table, 256);
}

// bytes.translate(table[, delete]) — bytes-like translation.
static VALUE
bm_translate(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    const char *table = NULL;
    if (argv[1] != PYS_NONE) {
        if (!pys_is_byteseq(argv[1]))
            PYS_RAISE_EXC(c, c->EXC_TypeError, "translate: table must be bytes-like or None");
        struct pysobj *t = PYS_PTR(argv[1]);
        if (t->str.len != 256)
            PYS_RAISE_EXC(c, c->EXC_ValueError, "translate: table must be 256 bytes");
        table = t->str.chars;
    }
    bool drop[256] = { false };
    if (argc >= 3 && argv[2] != PYS_NONE) {
        if (!pys_is_byteseq(argv[2]))
            PYS_RAISE_EXC(c, c->EXC_TypeError, "translate: delete must be bytes-like");
        struct pysobj *d = PYS_PTR(argv[2]);
        for (size_t i = 0; i < d->str.len; i++)
            drop[(unsigned char)d->str.chars[i]] = true;
    }
    char *buf = (char *)GC_malloc_atomic(o->str.len + 1);
    size_t out = 0;
    for (size_t i = 0; i < o->str.len; i++) {
        unsigned char ch = (unsigned char)o->str.chars[i];
        if (drop[ch]) continue;
        buf[out++] = table ? table[ch] : (char)ch;
    }
    // Preserve original type (bytes vs bytearray).
    if (PYS_PTR(argv[0])->type == PYS_T_BYTEARRAY) {
        VALUE r = pys_make_bytes(buf, out);
        PYS_PTR(r)->type = PYS_T_BYTEARRAY;
        return r;
    }
    return pys_make_bytes(buf, out);
}

// bytes/bytearray.copy() → shallow copy. Both immutable bytes and
// mutable bytearray expose this in CPython 3.
static VALUE
bm_copy(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    VALUE r = pys_make_bytes(o->str.chars, o->str.len);
    PYS_PTR(r)->type = o->type;
    return r;
}

static struct type_method bytes_methods[] = {
    { "decode",     bm_decode,     1, 3 },
    { "encode",     bm_encode,     1, 3 },
    { "startswith", bm_startswith, 2, 4 },
    { "split",      bm_split,      1, 3 },
    { "replace",    bm_replace,    3, 3 },
    { "hex",        bm_hex,        1, 3 },
    { "append",     bm_append,     2, 2 },
    { "extend",     bm_extend,     2, 2 },
    { "insert",     bm_insert,     3, 3 },
    { "pop",        bm_pop,        1, 2 },
    { "remove",     bm_remove,     2, 2 },
    { "reverse",    bm_reverse,    1, 1 },
    { "clear",      bm_clear,      1, 1 },
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
    { "translate",  bm_translate,  2, 3 },
    { "copy",       bm_copy,       1, 1 },
    { NULL, NULL, 0, 0 }
};

// generator methods: send / throw / close + __next__ / __iter__.
extern VALUE pys_gen_next(CTX *c, VALUE g);
extern VALUE pys_gen_send(CTX *c, VALUE g, VALUE v);
extern VALUE pys_gen_throw(CTX *c, VALUE g, VALUE exc);
extern VALUE pys_gen_close(CTX *c, VALUE g);

static VALUE gm_send(CTX *c, int argc, VALUE *argv)  { (void)argc; return pys_gen_send(c, argv[0], argv[1]); }
static VALUE
gm_throw(CTX *c, int argc, VALUE *argv)
{
    // Two-arg form: throw(exc) where exc is a class or instance.
    // Three-arg form: throw(type, value [, traceback]) — instantiate.
    VALUE exc = argv[1];
    if (argc >= 3 && pys_is_class(exc)) {
        VALUE av[1] = { argv[2] };
        exc = pys_apply(c, exc, 1, av);
    }
    return pys_gen_throw(c, argv[0], exc);
}
static VALUE gm_close(CTX *c, int argc, VALUE *argv) { (void)argc; return pys_gen_close(c, argv[0]); }
static VALUE gm_next(CTX *c, int argc, VALUE *argv)  { (void)argc; return pys_gen_next(c, argv[0]); }
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
    VALUE sep_v = pys_bi_kwarg("sep");
    VALUE end_v = pys_bi_kwarg("end");
    VALUE file_v = pys_bi_kwarg("file");
    const char *sep = " ";  size_t sep_len = 1;
    const char *end = "\n"; size_t end_len = 1;
    if (sep_v && pys_is_str(sep_v)) {
        sep = PYS_PTR(sep_v)->str.chars;
        sep_len = PYS_PTR(sep_v)->str.len;
    }
    if (end_v && pys_is_str(end_v)) {
        end = PYS_PTR(end_v)->str.chars;
        end_len = PYS_PTR(end_v)->str.len;
    }
    FILE *fp = stdout;
    if (file_v && pys_is_file(file_v)) fp = (FILE *)PYS_PTR(file_v)->file.fp;
    for (int i = 0; i < argc; i++) {
        if (i) fwrite(sep, 1, sep_len, fp);
        pys_display(fp, argv[i], false);
    }
    fwrite(end, 1, end_len, fp);
    return PYS_NONE;
}

static VALUE
bi_str(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return pys_make_str("", 0);
    // CPython: `str(bytes, encoding[, errors])` decodes via the given codec.
    // pystro previously fell through to repr(bytes) which yielded
    // `"b'...'"` and broke pickle's load_short_binunicode path.
    if (argc >= 2 && pys_is_byteseq(argv[0])) {
        // Just route through bytes.decode("utf-8") for simplicity.
        // Most callers use utf-8; other codecs aren't supported anyway.
        struct pysobj *b = PYS_PTR(argv[0]);
        return pys_make_str(b->str.chars, b->str.len);
    }
    return pys_to_str(c, argv[0]);
}

VALUE
bi_repr(CTX *c, int argc, VALUE *argv) { (void)argc; return pys_to_repr(c, argv[0]); }

static VALUE
bi_int(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return PYS_FIX(0);
    VALUE v = argv[0];
    if (PYS_IS_FIXNUM(v) || pys_is_bignum(v)) return v;
    if (v == PYS_TRUE)    return PYS_FIX(1);
    if (v == PYS_FALSE)   return PYS_FIX(0);
    if (pys_is_float(v)) {
        double d = PYS_IS_FLONUM(v) ? pys_flonum_to_double(v) : PYS_PTR(v)->dbl;
        if (d != d) PYS_RAISE_EXC(c, c->EXC_ValueError, "cannot convert NaN to int");
        if (d == 1.0/0.0 || d == -1.0/0.0)
            PYS_RAISE_EXC(c, c->EXC_OverflowError, "cannot convert inf to int");
        if (d >= (double)PYS_FIXNUM_MIN && d <= (double)PYS_FIXNUM_MAX)
            return PYS_FIX((int64_t)d);
        mpz_t z; mpz_init(z); mpz_set_d(z, d);
        VALUE r = pys_normalise_int(z); mpz_clear(z); return r;
    }
    if (pys_is_str(v) || pys_is_byteseq(v)) {
        // String may be a slice-borrow (no NUL-terminator within bounds);
        // copy into a stack buffer.  bytes/bytearray accepted too:
        // CPython's int() takes both ASCII bytes and str.
        size_t L = PYS_PTR(v)->str.len;
        char small[64];
        char *buf = (L < sizeof(small)) ? small : (char *)GC_malloc_atomic(L + 1);
        memcpy(buf, PYS_PTR(v)->str.chars, L);
        buf[L] = '\0';
        // strip leading/trailing whitespace.
        char *p = buf;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        char *end_p = buf + strlen(buf);
        while (end_p > p && (end_p[-1] == ' ' || end_p[-1] == '\t' || end_p[-1] == '\n' || end_p[-1] == '\r')) end_p--;
        *end_p = '\0';
        int base = 10;
        VALUE bk = pys_bi_kwarg("base");
        if (argc >= 2) {
            base = (int)pys_int_to_long(c, argv[1]);
            if (base != 0 && (base < 2 || base > 36))
                PYS_RAISE_EXC(c, c->EXC_ValueError, "int() base must be 2..36 or 0");
        } else if (bk) {
            base = (int)pys_int_to_long(c, bk);
            if (base != 0 && (base < 2 || base > 36))
                PYS_RAISE_EXC(c, c->EXC_ValueError, "int() base must be 2..36 or 0");
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
            mpz_clear(z); PYS_RAISE_EXC(c, c->EXC_ValueError, "invalid literal for int()");
        }
        VALUE r = pys_normalise_int(z); mpz_clear(z); return r;
    }
    if (pys_is_instance(v)) {
        // For built-in subclasses (`class TrapInt(int)`), if the
        // instance has a primary value (the underlying int), return
        // it directly to short-circuit `int(self)` recursion in
        // user-defined __int__ / __index__ methods.
        struct pysobj *o = PYS_PTR(v);
        if (o->inst.primary
            && (PYS_IS_FIXNUM(o->inst.primary) || pys_is_bignum(o->inst.primary)
                || o->inst.primary == PYS_TRUE || o->inst.primary == PYS_FALSE))
            return o->inst.primary;
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), "__int__");
        if (m != PYS_NONE) { VALUE av[1] = { v }; return pys_apply(c, m, 1, av); }
        m = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), PYS_INTERN_index);
        if (m != PYS_NONE) { VALUE av[1] = { v }; return pys_apply(c, m, 1, av); }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "int() argument type not supported");
}

static VALUE
bi_float(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return pys_make_float(0.0);
    VALUE v = argv[0];
    if (pys_is_float(v)) return v;
    if (PYS_IS_FIXNUM(v)) return pys_make_float((double)PYS_FIXVAL(v));
    if (pys_is_bignum(v)) return pys_make_float(mpz_get_d(PYS_PTR(v)->mpz));
    if (v == PYS_TRUE)    return pys_make_float(1.0);
    if (v == PYS_FALSE)   return pys_make_float(0.0);
    if (pys_is_str(v)) {
        size_t L = PYS_PTR(v)->str.len;
        char small[64];
        char *buf = (L < sizeof(small)) ? small : (char *)GC_malloc_atomic(L + 1);
        memcpy(buf, PYS_PTR(v)->str.chars, L);
        buf[L] = '\0';
        // strip underscores
        char *q = buf, *w = buf;
        while (*q) { if (*q != '_') *w++ = *q; q++; }
        *w = '\0';
        // Accept "inf", "Infinity", "nan" (case-insensitive).
        char *end;
        double d = strtod(buf, &end);
        if (end == buf) PYS_RAISE_EXC(c, c->EXC_ValueError, "could not convert string to float");
        return pys_make_float(d);
    }
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), "__float__");
        if (m != PYS_NONE) { VALUE av[1] = { v }; return pys_apply(c, m, 1, av); }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "float() argument type not supported");
}

static VALUE
bi_complex(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return pys_make_complex(0, 0);
    double re = 0, im = 0;
    if (argc >= 1) {
        if (pys_is_complex(argv[0])) {
            re = PYS_PTR(argv[0])->cpx.re;
            im = PYS_PTR(argv[0])->cpx.im;
        } else if (pys_is_str(argv[0])) {
            // Parse "a+bj" / "a-bj" / "bj" / "a" forms.  Strip surrounding
            // parens and whitespace, then walk the chars.
            const char *s = PYS_PTR(argv[0])->str.chars;
            size_t L = PYS_PTR(argv[0])->str.len;
            // strip whitespace
            while (L > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) { s++; L--; }
            while (L > 0 && (s[L-1] == ' ' || s[L-1] == '\t' || s[L-1] == '\n' || s[L-1] == '\r')) L--;
            if (L >= 2 && s[0] == '(' && s[L-1] == ')') { s++; L -= 2; }
            // Find the j/J at the end.
            bool ends_j = (L > 0 && (s[L-1] == 'j' || s[L-1] == 'J'));
            // Find an internal +/- (but not the leading sign or one after e/E).
            int split = -1;
            for (size_t i = 1; i < L; i++) {
                if ((s[i] == '+' || s[i] == '-') && s[i-1] != 'e' && s[i-1] != 'E') {
                    split = (int)i;
                }
            }
            char *re_str = NULL;
            char *im_str = NULL;
            if (ends_j) {
                if (split > 0) {
                    re_str = (char *)GC_malloc_atomic(split + 1);
                    memcpy(re_str, s, split); re_str[split] = '\0';
                    im_str = (char *)GC_malloc_atomic(L - split);
                    memcpy(im_str, s + split, L - split - 1);
                    im_str[L - split - 1] = '\0';  // strip 'j'
                } else {
                    // pure imaginary
                    im_str = (char *)GC_malloc_atomic(L);
                    memcpy(im_str, s, L - 1); im_str[L - 1] = '\0';
                    if (im_str[0] == '\0' || (im_str[0] == '+' && im_str[1] == '\0')) {
                        im = 1.0;
                    } else if (im_str[0] == '-' && im_str[1] == '\0') {
                        im = -1.0;
                    } else {
                        im = strtod(im_str, NULL);
                    }
                    return pys_make_complex(0, im);
                }
            } else {
                // pure real
                re_str = (char *)GC_malloc_atomic(L + 1);
                memcpy(re_str, s, L); re_str[L] = '\0';
            }
            if (re_str) re = strtod(re_str, NULL);
            if (im_str) {
                if (im_str[0] == '\0' || (im_str[0] == '+' && im_str[1] == '\0')) {
                    im = 1.0;
                } else if (im_str[0] == '-' && im_str[1] == '\0') {
                    im = -1.0;
                } else {
                    im = strtod(im_str, NULL);
                }
            }
            return pys_make_complex(re, im);
        } else {
            re = pys_to_double(c, argv[0]);
        }
    }
    if (argc >= 2) {
        if (pys_is_complex(argv[1])) {
            im += PYS_PTR(argv[1])->cpx.re;
            re -= PYS_PTR(argv[1])->cpx.im;
        } else {
            im += pys_to_double(c, argv[1]);
        }
    }
    return pys_make_complex(re, im);
}

static VALUE
bi_bool(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return PYS_FALSE;
    VALUE v = argv[0];
    if (pys_is_instance(v)) {
        VALUE cls = PYS_OBJ_VAL(PYS_PTR(v)->inst.cls);
        VALUE m = pys_class_lookup_method(cls, PYS_INTERN_bool);
        if (m != PYS_NONE) {
            VALUE av[1] = { v };
            VALUE r = pys_apply(c, m, 1, av);
            return pys_is_truthy(r) ? PYS_TRUE : PYS_FALSE;
        }
        VALUE lm = pys_class_lookup_method(cls, PYS_INTERN_len);
        if (lm != PYS_NONE) {
            VALUE av[1] = { v };
            VALUE r = pys_apply(c, lm, 1, av);
            return pys_int_to_long(c, r) != 0 ? PYS_TRUE : PYS_FALSE;
        }
    }
    return pys_is_truthy(v) ? PYS_TRUE : PYS_FALSE;
}

static VALUE
bi_len(CTX *c, int argc, VALUE *argv) { (void)argc; return PYS_FIX((int64_t)pys_seq_len(c, argv[0])); }

static VALUE
bi_abs(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (v == PYS_TRUE) return PYS_FIX(1);
    if (v == PYS_FALSE) return PYS_FIX(0);
    if (PYS_IS_FIXNUM(v)) {
        int64_t x = PYS_FIXVAL(v);
        return PYS_FIX(x < 0 ? -x : x);
    }
    if (pys_is_bignum(v)) {
        mpz_t z; mpz_init_set(z, PYS_PTR(v)->mpz);
        mpz_abs(z, z);
        VALUE r = pys_normalise_int(z); mpz_clear(z); return r;
    }
    if (pys_is_float(v)) {
        double d = PYS_IS_FLONUM(v) ? pys_flonum_to_double(v) : PYS_PTR(v)->dbl;
        return pys_make_float(fabs(d));
    }
    if (pys_is_complex(v)) {
        double re = PYS_PTR(v)->cpx.re;
        double im = PYS_PTR(v)->cpx.im;
        return pys_make_float(sqrt(re*re + im*im));
    }
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), "__abs__");
        if (m != PYS_NONE) {
            VALUE av[1] = { v };
            return pys_apply(c, m, 1, av);
        }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "bad operand type for abs()");
}

static VALUE
bi_range(CTX *c, int argc, VALUE *argv)
{
    int64_t start = 0, stop = 0, step = 1;
    if (argc == 1) stop = pys_int_to_long(c, argv[0]);
    else if (argc == 2) { start = pys_int_to_long(c, argv[0]); stop = pys_int_to_long(c, argv[1]); }
    else { start = pys_int_to_long(c, argv[0]); stop = pys_int_to_long(c, argv[1]); step = pys_int_to_long(c, argv[2]); }
    if (step == 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "range() arg 3 must not be zero");
    return pys_make_range(start, stop, step);
}

static VALUE
bi_list(CTX *c, int argc, VALUE *argv)
{
    // CPython: list() takes no keyword arguments.
    if (PYS_BI_KWC > 0)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "list() takes no keyword arguments");
    if (argc == 0) return pys_make_list(NULL, 0);
    VALUE r = pys_make_list(NULL, 0);
    // CPython: iter objects are single-use; `list(it)` consumes it.
    // Drive the original iter_state in place so a subsequent `next(it)`
    // raises StopIteration instead of restarting from the beginning.
    if (PYS_IS_PTR(argv[0]) && PYS_PTR(argv[0])->type == PYS_T_ITER) {
        struct pys_iter *it = PYS_PTR(argv[0])->iter_state;
        VALUE x;
        while (pys_iter_next(c, it, &x)) {
            pys_list_append(c, r, x);
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        }
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
        return r;
    }
    struct pys_iter it; pys_iter_init(c, &it, argv[0]);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) pys_list_append(c, r, x);
    return r;
}

static VALUE
bi_tuple(CTX *c, int argc, VALUE *argv)
{
    if (PYS_BI_KWC > 0)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "tuple() takes no keyword arguments");
    if (argc == 0) return pys_make_tuple(NULL, 0);
    VALUE l = bi_list(c, argc, argv);
    return pys_make_tuple(PYS_PTR(l)->list.items, PYS_PTR(l)->list.len);
}

static VALUE
bi_dict(CTX *c, int argc, VALUE *argv)
{
    VALUE r = pys_make_dict();
    // dict(dict-subclass-instance) — unwrap primary.
    VALUE src_dict = (VALUE)0;
    if (argc >= 1 && pys_is_dict(argv[0])) {
        src_dict = argv[0];
    } else if (argc >= 1 && pys_is_instance(argv[0])
               && PYS_PTR(argv[0])->inst.primary
               && pys_is_dict(PYS_PTR(argv[0])->inst.primary)) {
        src_dict = PYS_PTR(argv[0])->inst.primary;
    }
    if (src_dict) {
        struct pysdict *src = PYS_PTR(src_dict)->dict;
        for (size_t i = 0; i < src->elen; i++)
            if (pydict_entry_live(src, i))
                pys_dict_set(c, r, src->entries[i].key, src->entries[i].value);
    }
    // dict(mapping_like) — instance with `keys()` method.
    else if (argc >= 1 && pys_is_instance(argv[0])) {
        VALUE keys_m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls), "keys");
        if (keys_m != PYS_NONE) {
            VALUE av0[1] = { argv[0] };
            VALUE keys = pys_apply(c, keys_m, 1, av0);
            if (UNLIKELY(!keys)) return PYS_NONE;
            struct pys_iter it; pys_iter_init(c, &it, keys);
            if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
            VALUE k;
            while (pys_iter_next(c, &it, &k)) {
                VALUE v = pys_list_get(c, argv[0], k);
                if (UNLIKELY(!v)) return PYS_NONE;
                pys_dict_set(c, r, k, v);
            }
        } else {
            // Treat as iterable of (k, v) pairs.
            struct pys_iter it; pys_iter_init(c, &it, argv[0]);
            if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
            VALUE x;
            while (pys_iter_next(c, &it, &x)) {
                if (pys_is_tuple(x) || pys_is_list(x)) {
                    if (PYS_PTR(x)->list.len != 2)
                        PYS_RAISE_EXC(c, c->EXC_ValueError, "dict update: pair must be length 2");
                    pys_dict_set(c, r, PYS_PTR(x)->list.items[0], PYS_PTR(x)->list.items[1]);
                } else {
                    PYS_RAISE_EXC(c, c->EXC_TypeError, "dict update: not a pair");
                }
            }
        }
    }
    // dict([(k, v), ...]) — from iterable of pairs.
    else if (argc >= 1) {
        struct pys_iter it; pys_iter_init(c, &it, argv[0]);
        if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
            if (pys_is_tuple(x) || pys_is_list(x)) {
                if (PYS_PTR(x)->list.len != 2)
                    PYS_RAISE_EXC(c, c->EXC_ValueError, "dict update: pair must be length 2");
                pys_dict_set(c, r, PYS_PTR(x)->list.items[0], PYS_PTR(x)->list.items[1]);
            } else {
                PYS_RAISE_EXC(c, c->EXC_TypeError, "dict update: not a pair");
            }
        }
    }
    // **kwargs from caller.
    extern int    PYS_BI_KWC;
    extern const char **PYS_BI_KWNAMES;
    extern VALUE *PYS_BI_KWVALUES;
    for (int i = 0; i < PYS_BI_KWC; i++) {
        VALUE k = pys_make_str(PYS_BI_KWNAMES[i], strlen(PYS_BI_KWNAMES[i]));
        pys_dict_set(c, r, k, PYS_BI_KWVALUES[i]);
    }
    return r;
}

static VALUE
bi_set(CTX *c, int argc, VALUE *argv)
{
    if (PYS_BI_KWC > 0)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "set() takes no keyword arguments");
    if (argc > 1)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "set expected at most 1 argument, got %d", argc);
    VALUE r = pys_make_set();
    if (argc == 0) return r;
    struct pys_iter it; pys_iter_init(c, &it, argv[0]);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) pys_dict_set(c, r, x, PYS_NONE);
    return r;
}

static VALUE
bi_bytes(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return pys_make_bytes("", 0);
    VALUE v = argv[0];
    if (PYS_IS_FIXNUM(v)) {
        int64_t n = PYS_FIXVAL(v);
        if (n < 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "negative count");
        char *buf = (char *)GC_malloc_atomic(n + 1);
        memset(buf, 0, n + 1);
        struct pysobj *o = pys_alloc(PYS_T_BYTES);
        o->str.chars = buf; o->str.len = (size_t)n;
        return PYS_OBJ_VAL(o);
    }
    if (pys_is_byteseq(v)) return pys_make_bytes(PYS_PTR(v)->str.chars, PYS_PTR(v)->str.len);
    if (pys_is_str(v))     return pys_make_bytes(PYS_PTR(v)->str.chars, PYS_PTR(v)->str.len);
    if (PYS_IS_PTR(v) && PYS_PTR(v)->type == PYS_T_MEMVIEW) {
        struct pysobj *mv = PYS_PTR(v);
        const char *p = PYS_PTR(mv->memview.source)->str.chars + mv->memview.off;
        return pys_make_bytes(p, mv->memview.len);
    }
    // iterable of ints
    struct pys_iter it; pys_iter_init(c, &it, v);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    size_t cap = 16, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    VALUE x;
    while (pys_iter_next(c, &it, &x)) {
        int64_t b = pys_int_to_long(c, x);
        if (b < 0 || b > 255) PYS_RAISE_EXC(c, c->EXC_ValueError, "byte must be 0..255");
        if (len == cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
        buf[len++] = (char)b;
    }
    return pys_make_bytes(buf, len);
}

static VALUE
bi_bytearray(CTX *c, int argc, VALUE *argv)
{
    VALUE r = bi_bytes(c, argc, argv);
    if (pys_is_bytes(r)) PYS_PTR(r)->type = PYS_T_BYTEARRAY;
    return r;
}

static VALUE
bi_frozenset(CTX *c, int argc, VALUE *argv)
{
    if (PYS_BI_KWC > 0)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "frozenset() takes no keyword arguments");
    if (argc > 1)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "frozenset expected at most 1 argument, got %d", argc);
    VALUE r = pys_make_frozenset();
    if (argc == 0) return r;
    struct pys_iter it; pys_iter_init(c, &it, argv[0]);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) pys_dict_set(c, r, x, PYS_NONE);
    return r;
}

// Look up a builtin by name and return its VALUE (the same object the
// user gets via the global name).  Used by `type()` to make
// `type(5) is int` true.
static VALUE
type_lookup_builtin(CTX *c, const char *name)
{
    int i = pys_global_index(c, name);
    if (i >= 0 && c->globals->entries[i].defined)
        return c->globals->entries[i].value;
    return pys_make_str(name, strlen(name));
}

// type.__call__(cls, *args, **kwargs) — default class-call protocol that
// bypasses metaclass __call__ (so a metaclass __call__ can delegate to
// the standard new/init flow).
static VALUE
bi_type_call(CTX *c, int argc, VALUE *argv)
{
    if (argc < 1 || !pys_is_class(argv[0]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "type.__call__(cls, ...) needs cls");
    VALUE cls = argv[0];
    int n = argc - 1;
    VALUE *cargv = n > 0 ? &argv[1] : NULL;
    // Capture kwargs the caller passed via the BI thread-local (set by
    // pys_apply_kw before invoking this builtin).  Forward to __new__ /
    // __init__ so dataclass-style `cls(a=1, b=2)` gets its kwargs.
    int kwc = PYS_BI_KWC;
    const char **kwn = (const char **)PYS_BI_KWNAMES;
    VALUE *kwv = PYS_BI_KWVALUES;
    if (PYS_PTR(cls)->cls.builtin_ctor)
        return PYS_PTR(cls)->cls.builtin_ctor(c, n, cargv);
    VALUE inst;
    VALUE new_m = pys_class_lookup_method(cls, PYS_INTERN_new);
    if (new_m != PYS_NONE) {
        VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (n + 1));
        av[0] = cls;
        for (int i = 0; i < n; i++) av[i + 1] = cargv[i];
        inst = pys_apply_kw(c, new_m, n + 1, av, kwc, kwn, kwv);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    } else {
        inst = pys_make_instance(cls);
    }
    VALUE init = pys_class_lookup_method(cls, PYS_INTERN_init);
    if (init != PYS_NONE) {
        VALUE *av = (VALUE *)alloca(sizeof(VALUE) * (n + 1));
        av[0] = inst;
        for (int i = 0; i < n; i++) av[i + 1] = cargv[i];
        pys_apply_kw(c, init, n + 1, av, kwc, kwn, kwv);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    return inst;
}

// type.__new__(mcls, name, bases, dict) — invoked from CPython
// metaclass `def __new__(mcls, name, bases, ns)` body when it calls
// `super().__new__(mcls, ...)`.  Strips the leading mcls and forwards
// to bi_type's 3-arg class-construction form, then stamps mcls as
// the new class's metaclass via __metaclass__.
VALUE
bi_type_new(CTX *c, int argc, VALUE *argv)
{
    extern VALUE bi_type(CTX *c, int argc, VALUE *argv);
    if (argc == 2) {
        // type.__new__(type, x) — type query form via type's __call__
        // dispatch.  Just return type(x).
        VALUE one[1] = { argv[1] };
        return bi_type(c, 1, one);
    }
    if (argc < 4) {
        // 0/1-arg edge cases shouldn't reach here, but be safe.
        extern VALUE bi_object_new(CTX *c, int argc, VALUE *argv);
        return bi_object_new(c, argc, argv);
    }
    VALUE mcls = argv[0];
    VALUE three[3] = { argv[1], argv[2], argv[3] };
    VALUE cls = bi_type(c, 3, three);
    if (c->state == PYS_STATE_RAISE) return 0;
    // Stamp mcls as the new class's metaclass so type(C) returns mcls.
    if (pys_is_class(cls) && pys_is_class(mcls) && mcls != c->TYPE_type) {
        extern const char *intern_name(const char *s, size_t len);
        pys_class_add_method(c, cls, intern_name("__metaclass__", 13), mcls);
        pyclass_refresh_slots(cls);
    }
    return cls;
}

VALUE
bi_type(CTX *c, int argc, VALUE *argv)
{
    // 3-arg form: type(name, bases, attrs) creates a new class.
    if (argc == 3) {
        if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "type(): name must be str");
        const char *name = PYS_PTR(argv[0])->str.chars;
        // Bases tuple/list.
        VALUE bases = argv[1];
        VALUE first_base = PYS_NONE;
        int nbases = 0;
        if (pys_is_tuple(bases) || pys_is_list(bases)) nbases = (int)PYS_PTR(bases)->list.len;
        if (nbases > 0) first_base = PYS_PTR(bases)->list.items[0];
        VALUE cls = pys_make_class(name, first_base, false);
        if (nbases > 1) {
            VALUE *bv = (VALUE *)alloca(sizeof(VALUE) * nbases);
            for (int i = 0; i < nbases; i++) bv[i] = PYS_PTR(bases)->list.items[i];
            extern void pys_class_set_bases(VALUE cls, VALUE *bases, int n);
            pys_class_set_bases(cls, bv, nbases);
        }
        // Pour attrs into the class.
        if (pys_is_dict(argv[2])) {
            struct pysdict *d = PYS_PTR(argv[2])->dict;
            for (size_t i = 0; i < d->elen; i++) {
                if (!pydict_entry_live(d, i)) continue;
                VALUE k = d->entries[i].key;
                if (!pys_is_str(k)) continue;
                extern const char *intern_name(const char *s, size_t len);
                pys_class_add_method(c, cls, intern_name(PYS_PTR(k)->str.chars, PYS_PTR(k)->str.len),
                                    d->entries[i].value);
            }
        }
        return cls;
    }
    VALUE v = argv[0];
    if (PYS_IS_FIXNUM(v)) return c->TYPE_int;
    if (PYS_IS_FLONUM(v)) return c->TYPE_float;
    if (v == PYS_NONE)  return c->TYPE_NoneType;
    if (v == PYS_TRUE || v == PYS_FALSE) return c->TYPE_bool;
    if (pys_is_bignum(v)) return c->TYPE_int;
    struct pysobj *o = PYS_PTR(v);
    switch (o->type) {
      case PYS_T_FLOAT: return c->TYPE_float;
      case PYS_T_STR:   return c->TYPE_str;
      case PYS_T_BYTES: return c->TYPE_bytes;
      case PYS_T_BYTEARRAY: return c->TYPE_bytearray;
      case PYS_T_LIST:  return c->TYPE_list;
      case PYS_T_TUPLE: return c->TYPE_tuple;
      case PYS_T_DICT:  return c->TYPE_dict;
      case PYS_T_SET:   return c->TYPE_set;
      case PYS_T_FROZENSET: return c->TYPE_frozenset;
      case PYS_T_RANGE: return c->TYPE_range;
      case PYS_T_COMPLEX: return c->TYPE_complex;
      case PYS_T_FUNC: return c->TYPE_function;
      case PYS_T_BUILTIN: return c->TYPE_builtin_function_or_method;
      case PYS_T_BOUND_METHOD: return c->TYPE_method;
      case PYS_T_MODULE: return c->TYPE_module;
      case PYS_T_SLICE: return c->TYPE_slice;
      case PYS_T_ELLIPSIS: return c->TYPE_ellipsis;
      case PYS_T_NOTIMPL: return c->TYPE_NotImplementedType;
      case PYS_T_MEMVIEW: return c->TYPE_memoryview;
      case PYS_T_GEN:   return c->TYPE_generator;
      case PYS_T_PROPERTY: return c->TYPE_property;
      case PYS_T_STATICMETHOD: return c->TYPE_staticmethod;
      case PYS_T_CLASSMETHOD: return c->TYPE_classmethod;
      case PYS_T_SUPER: return c->TYPE_super;
      case PYS_T_CLASS: {
          // type(C) where C has metaclass=M returns M, not type.  CPython
          // tests like `MutableMapping.register(deque)` rely on this:
          // MutableMapping has metaclass=ABCMeta, so type(MutableMapping)
          // is ABCMeta, and `MutableMapping.register` finds register on
          // the ABCMeta class and binds with cls=MutableMapping.
          struct pysclass *cd_t = &o->cls;
          if (!cd_t->slots_initialized) pyclass_refresh_slots(v);
          if (cd_t->slot_metaclass != PYS_NONE
              && pys_is_class(cd_t->slot_metaclass))
              return cd_t->slot_metaclass;
          return c->TYPE_type;
      }
      case PYS_T_INSTANCE: return PYS_OBJ_VAL(o->inst.cls);
      case PYS_T_FILE:  return c->TYPE_object;
      default: return c->TYPE_object;
    }
}

static VALUE
bi_id(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    int64_t id = PYS_IS_PTR(v) ? (int64_t)(uintptr_t)PYS_PTR(v) : (int64_t)v;
    // Return as a (potentially big) int — the high bit is fine for fixnum.
    return pys_make_int(id);
}

static VALUE
bi_dir(CTX *c, int argc, VALUE *argv)
{
    // User-defined __dir__ override.
    if (argc == 1 && pys_is_instance(argv[0])) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls), "__dir__");
        if (m != PYS_NONE) {
            VALUE result = pys_apply(c, m, 1, argv);
            if (c->state == PYS_STATE_RAISE) return 0;
            if (pys_is_list(result)) {
                VALUE av[1] = { result };
                lm_sort(c, 1, av);
            }
            return result;
        }
    }
    VALUE r = pys_make_list(NULL, 0);
    if (argc == 0) {
        // dir() with no args: list current frame's local names.  We
        // don't track param-names per pysframe well, so return globals.
        struct pysglobals *g = c->globals;
        for (size_t i = 0; i < g->size; i++)
            if (g->entries[i].defined)
                pys_list_append(c, r, pys_make_str(g->entries[i].name, strlen(g->entries[i].name)));
    } else {
        VALUE v = argv[0];
        if (pys_is_module(v)) {
            struct pysglobals *g = PYS_PTR(v)->module.globals;
            for (size_t i = 0; i < g->size; i++)
                if (g->entries[i].defined)
                    pys_list_append(c, r, pys_make_str(g->entries[i].name, strlen(g->entries[i].name)));
        } else if (pys_is_class(v)) {
            // Walk MRO; collect unique method names.
            struct pysclass *cd = &PYS_PTR(v)->cls;
            for (int j = 0; j < cd->nmro; j++) {
                struct pysclass *kd = &PYS_PTR(cd->mro[j])->cls;
                for (int i = 0; i < kd->nmethods; i++) {
                    const char *nm = kd->methods[i].name;
                    bool dup = false;
                    size_t rl = PYS_PTR(r)->list.len;
                    for (size_t k = 0; k < rl; k++) {
                        VALUE existing = PYS_PTR(r)->list.items[k];
                        if (pys_is_str(existing) &&
                            strcmp(PYS_PTR(existing)->str.chars, nm) == 0) {
                            dup = true; break;
                        }
                    }
                    if (!dup) pys_list_append(c, r, pys_make_str(nm, strlen(nm)));
                }
            }
        } else if (pys_is_instance(v)) {
            struct pysobj *o = PYS_PTR(v);
            if (o->inst.attrs) {
                struct pysdict *d = o->inst.attrs;
                for (size_t i = 0; i < d->elen; i++)
                    if (pydict_entry_live(d, i) && pys_is_str(d->entries[i].key))
                        pys_list_append(c, r, d->entries[i].key);
            }
            // Plus class methods (walk MRO).
            struct pysclass *cd = &PYS_PTR(o->inst.cls)->cls;
            for (int j = 0; j < cd->nmro; j++) {
                struct pysclass *kd = &PYS_PTR(cd->mro[j])->cls;
                for (int i = 0; i < kd->nmethods; i++) {
                    const char *nm = kd->methods[i].name;
                    bool dup = false;
                    size_t rl = PYS_PTR(r)->list.len;
                    for (size_t k = 0; k < rl; k++) {
                        VALUE existing = PYS_PTR(r)->list.items[k];
                        if (pys_is_str(existing) &&
                            strcmp(PYS_PTR(existing)->str.chars, nm) == 0) {
                            dup = true; break;
                        }
                    }
                    if (!dup) pys_list_append(c, r, pys_make_str(nm, strlen(nm)));
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
    VALUE d = pys_make_dict();
    struct pysglobals *g = c->globals;
    for (size_t i = 0; i < g->size; i++) {
        if (g->entries[i].defined) {
            VALUE k = pys_make_str(g->entries[i].name, strlen(g->entries[i].name));
            pys_dict_set(c, d, k, g->entries[i].value);
        }
    }
    return d;
}

static VALUE
bi_locals(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    // At module scope, fall back to globals.
    if (!c->env) return bi_globals(c, 0, NULL);
    VALUE d = pys_make_dict();
    struct pysframe *env = c->env;
    const char **ln = env ? env->slot_names : NULL;
    if (!ln) return d;
    for (int i = 0; i < env->nslots; i++) {
        const char *name = ln[i];
        if (!name) continue;
        // Skip undefined / never-assigned slots — CPython locals() omits
        // names that haven't been bound yet (slot reads as 0 sentinel).
        if (env->slots[i] == (VALUE)0) continue;
        // Skip pystro's compiler-synthesised slot names (tuple-unpack /
        // for-tuple / comprehension / aug-assign temps).  All have the
        // pattern `__<letters><digits>__` or contain `$`.  Real Python
        // dunders never carry digits or `$`.
        size_t nlen = strlen(name);
        if (nlen >= 4 && name[0] == '_' && name[1] == '_'
            && name[nlen-1] == '_' && name[nlen-2] == '_') {
            bool has_digit = false, has_dollar = false;
            for (size_t j = 2; j + 2 < nlen; j++) {
                if (name[j] >= '0' && name[j] <= '9') has_digit = true;
                if (name[j] == '$') has_dollar = true;
            }
            if (has_digit || has_dollar) continue;
        }
        VALUE k = pys_make_str(name, nlen);
        pys_dict_set(c, d, k, env->slots[i]);
    }
    return d;
}

static VALUE
bi_vars(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) return bi_locals(c, 0, NULL);
    VALUE v = argv[0];
    if (pys_is_module(v)) {
        struct pysglobals *saved = c->globals;
        c->globals = PYS_PTR(v)->module.globals;
        VALUE r = bi_globals(c, 0, NULL);
        c->globals = saved;
        return r;
    }
    if (pys_is_instance(v)) {
        VALUE d = pys_make_dict();
        struct pysobj *o = PYS_PTR(v);
        if (o->inst.attrs) {
            struct pysdict *src = o->inst.attrs;
            for (size_t i = 0; i < src->elen; i++)
                if (pydict_entry_live(src, i))
                    pys_dict_set(c, d, src->entries[i].key, src->entries[i].value);
        }
        return d;
    }
    if (pys_is_class(v)) {
        // Return a dict of {name: value} for all methods on the class.
        VALUE d = pys_make_dict();
        struct pysclass *cd = &PYS_PTR(v)->cls;
        for (int i = 0; i < cd->nmethods; i++) {
            pys_dict_set(c, d,
                pys_make_str(cd->methods[i].name, strlen(cd->methods[i].name)),
                cd->methods[i].value);
        }
        return d;
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "vars() argument must be a module or instance");
}

static VALUE
bi_hasattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "hasattr name must be str");
    // Copy out of slice-borrow into NUL-terminated buf.
    size_t L = PYS_PTR(argv[1])->str.len;
    char *namebuf = (char *)GC_malloc_atomic(L + 1);
    memcpy(namebuf, PYS_PTR(argv[1])->str.chars, L);
    namebuf[L] = '\0';
    const char *name = namebuf;
    // State-based: pys_getattr sets state on raise; we just observe.
    int saved_state = c->state;
    VALUE saved_value = c->state_value;
    pys_getattr(c, argv[0], name);
    bool ok = (c->state != PYS_STATE_RAISE);
    c->state = saved_state;
    c->state_value = saved_value;
    return ok ? PYS_TRUE : PYS_FALSE;
}

static VALUE
bi_getattr(CTX *c, int argc, VALUE *argv)
{
    // Accept str subclass instances (their primary is a str).
    VALUE name_v = argv[1];
    if (!pys_is_str(name_v)) {
        VALUE up = pys_unwrap_primary(name_v);
        if (pys_is_str(up)) name_v = up;
        else PYS_RAISE_EXC(c, c->EXC_TypeError, "getattr name must be str");
    }
    size_t L = PYS_PTR(name_v)->str.len;
    char *namebuf = (char *)GC_malloc_atomic(L + 1);
    memcpy(namebuf, PYS_PTR(name_v)->str.chars, L);
    namebuf[L] = '\0';
    const char *name = namebuf;
    if (argc < 3) return pys_getattr(c, argv[0], name);
    // With default: state-based AttributeError catch.
    int saved_state = c->state;
    VALUE saved_value = c->state_value;
    VALUE r = pys_getattr(c, argv[0], name);
    if (c->state == PYS_STATE_RAISE) {
        c->state = saved_state; c->state_value = saved_value;
        return argv[2];
    }
    return r;
}

static VALUE
bi_setattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE name_v = argv[1];
    if (!pys_is_str(name_v)) {
        VALUE up = pys_unwrap_primary(name_v);
        if (pys_is_str(up)) name_v = up;
        else PYS_RAISE_EXC(c, c->EXC_TypeError, "setattr name must be str");
    }
    // String may be slice-borrowed (no NUL terminator within bounds);
    // copy to a small heap buffer so strlen sees the right length.
    size_t L = PYS_PTR(name_v)->str.len;
    char *buf = (char *)GC_malloc_atomic(L + 1);
    memcpy(buf, PYS_PTR(name_v)->str.chars, L);
    buf[L] = '\0';
    pys_setattr(c, argv[0], buf, argv[2]);
    return PYS_NONE;
}

// File I/O — `open(path, mode)` returns a PYS_T_FILE.  Supports the
// usual context-manager protocol (`with open(...) as f:` works because
// __enter__/__exit__ are class methods).
static VALUE
bi_open(CTX *c, int argc, VALUE *argv)
{
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "open: path must be str");
    const char *modestr = "r";
    if (argc >= 2) {
        if (!pys_is_str(argv[1])) PYS_RAISE_EXC(c, c->EXC_TypeError, "open: mode must be str");
        modestr = PYS_PTR(argv[1])->str.chars;
    }
    bool binary = strchr(modestr, 'b') != NULL;
    char libcmode[8] = {0};
    int mi = 0;
    for (const char *p = modestr; *p && mi < 7; p++)
        if (*p != 't') libcmode[mi++] = *p;
    libcmode[mi] = '\0';
    size_t L = PYS_PTR(argv[0])->str.len;
    char *pbuf = (char *)alloca(L + 1);
    memcpy(pbuf, PYS_PTR(argv[0])->str.chars, L); pbuf[L] = '\0';
    FILE *fp = fopen(pbuf, libcmode);
    if (!fp) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "open: cannot open '%s'", pbuf);
    struct pysobj *o = pys_alloc(PYS_T_FILE);
    o->file.fp = fp;
    o->file.path = (char *)GC_malloc_atomic(L + 1);
    memcpy(o->file.path, pbuf, L + 1);
    o->file.binary = binary;
    o->file.closed = false;
    return PYS_OBJ_VAL(o);
}

static VALUE
fm_read(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->type != PYS_T_FILE || o->file.closed) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "read on closed file");
    FILE *fp = (FILE *)o->file.fp;
    long limit = -1;
    if (argc >= 2) limit = (long)pys_int_to_long(c, argv[1]);
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
        struct pysobj *bo = pys_alloc(PYS_T_BYTES);
        bo->str.chars = buf;
        bo->str.len = len;
        return PYS_OBJ_VAL(bo);
    }
    return pys_make_str(buf, len);
}

VALUE
fm_readline(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->type != PYS_T_FILE || o->file.closed) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "readline on closed file");
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
    return pys_make_str(buf, len);
}

static VALUE
fm_readlines(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE r = pys_make_list(NULL, 0);
    for (;;) {
        VALUE line = fm_readline(c, 1, argv);
        if (!pys_is_str(line) || PYS_PTR(line)->str.len == 0) break;
        pys_list_append(c, r, line);
    }
    return r;
}

static VALUE
fm_write(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->type != PYS_T_FILE || o->file.closed) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "write on closed file");
    FILE *fp = (FILE *)o->file.fp;
    VALUE v = argv[1];
    const char *src; size_t L;
    if (pys_is_str(v) || pys_is_byteseq(v)) {
        src = PYS_PTR(v)->str.chars;
        L = PYS_PTR(v)->str.len;
    } else {
        VALUE s = pys_to_str(c, v);
        src = PYS_PTR(s)->str.chars;
        L = PYS_PTR(s)->str.len;
    }
    fwrite(src, 1, L, fp);
    return pys_make_int((int64_t)L);
}

static VALUE
fm_close(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->type == PYS_T_FILE && !o->file.closed) {
        fclose((FILE *)o->file.fp);
        o->file.closed = true;
    }
    return PYS_NONE;
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
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->type == PYS_T_FILE && !o->file.closed) fflush((FILE *)o->file.fp);
    return PYS_NONE;
}

static VALUE
fm_tell(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->type != PYS_T_FILE || o->file.closed)
        PYS_RAISE_EXC(c, c->EXC_RuntimeError, "tell on closed file");
    return pys_make_int((int64_t)ftell((FILE *)o->file.fp));
}

static VALUE
fm_seek(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->type != PYS_T_FILE || o->file.closed)
        PYS_RAISE_EXC(c, c->EXC_RuntimeError, "seek on closed file");
    int64_t off = pys_int_to_long(c, argv[1]);
    int whence = (argc >= 3) ? (int)pys_int_to_long(c, argv[2]) : 0;
    int sw = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
    if (fseek((FILE *)o->file.fp, off, sw) != 0)
        PYS_RAISE_EXC(c, c->EXC_OSError, "seek failed");
    return pys_make_int((int64_t)ftell((FILE *)o->file.fp));
}

static VALUE
fm_readable(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    return (o->type == PYS_T_FILE && !o->file.closed) ? PYS_TRUE : PYS_FALSE;
}

static VALUE
fm_writable(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = PYS_PTR(argv[0]);
    return (o->type == PYS_T_FILE && !o->file.closed) ? PYS_TRUE : PYS_FALSE;
}

static VALUE
fm_seekable(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    return PYS_TRUE;
}

static VALUE
fm_truncate(CTX *c, int argc, VALUE *argv)
{
    struct pysobj *o = PYS_PTR(argv[0]);
    if (o->type != PYS_T_FILE || o->file.closed)
        PYS_RAISE_EXC(c, c->EXC_RuntimeError, "truncate on closed file");
    int64_t size = (argc >= 2) ? pys_int_to_long(c, argv[1])
                               : (int64_t)ftell((FILE *)o->file.fp);
    int fd = fileno((FILE *)o->file.fp);
    extern int ftruncate(int fd, off_t length);
    if (ftruncate(fd, (off_t)size) != 0)
        PYS_RAISE_EXC(c, c->EXC_OSError, "truncate failed");
    return pys_make_int(size);
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

extern const char *intern_name(const char *s, size_t len);
// Inject names from `g` (a dict) into c->globals, returning a list of
// (name, prev_value, prev_existed) so the binding can be undone.
struct ns_save_entry {
    const char *name;
    VALUE prev;
    bool existed;
};
struct ns_save {
    struct ns_save_entry *entries;
    size_t n;
};
static void
ns_inject(CTX *c, VALUE g, struct ns_save *out)
{
    out->entries = NULL;
    out->n = 0;
    if (g == PYS_NONE || !pys_is_dict(g)) return;
    struct pysdict *d = PYS_PTR(g)->dict;
    out->entries = (struct ns_save_entry *)
        GC_malloc(sizeof(struct ns_save_entry) * d->elen);
    for (size_t i = 0; i < d->elen; i++) {
        if (!pydict_entry_live(d, i)) continue;
        VALUE k = d->entries[i].key;
        if (!pys_is_str(k)) continue;
        const char *name = intern_name(PYS_PTR(k)->str.chars, PYS_PTR(k)->str.len);
        VALUE prev = PYS_NONE;
        bool ex = pys_global_lookup(c, name, &prev);
        out->entries[out->n].name = name;
        out->entries[out->n].prev = prev;
        out->entries[out->n].existed = ex;
        pys_global_set(c, name, d->entries[i].value);
        out->n++;
    }
}
static void
ns_restore(CTX *c, struct ns_save *s)
{
    for (size_t i = 0; i < s->n; i++) {
        if (s->entries[i].existed) pys_global_set(c, s->entries[i].name, s->entries[i].prev);
        else pys_global_undef(c, s->entries[i].name);
    }
}

static VALUE
bi_eval(CTX *c, int argc, VALUE *argv)
{
    const char *code_chars = NULL;
    size_t L = 0;
    if (pys_is_str(argv[0])) {
        code_chars = PYS_PTR(argv[0])->str.chars;
        L = PYS_PTR(argv[0])->str.len;
    } else if (pys_is_bytes(argv[0]) || pys_is_bytearray(argv[0])) {
        code_chars = PYS_PTR(argv[0])->str.chars;
        L = PYS_PTR(argv[0])->str.len;
    } else {
        PYS_RAISE_EXC(c, c->EXC_TypeError, "eval: code must be str or bytes");
    }
    char *src = (char *)GC_malloc_atomic(L + 2);
    memcpy(src, code_chars, L);
    src[L] = '\n'; src[L+1] = '\0';
    extern void *lexer_save_alloc(void);
    extern void  lexer_restore_free(void *s);
    extern void *parser_save_alloc(void);
    extern void  parser_restore_free(void *s);
    extern jmp_buf *parse_error_jmp;
    extern char parse_error_msg[];
    void *lexsave = lexer_save_alloc();
    void *parsesave = parser_save_alloc();
    jmp_buf jb;
    jmp_buf *saved_jmp = parse_error_jmp;
    parse_error_jmp = &jb;
    NODE *expr = NULL;
    if (setjmp(jb) == 0) {
        tokenize(src, "<eval>");
        expr = parse_eval_expr();
    }
    parse_error_jmp = saved_jmp;
    lexer_restore_free(lexsave);
    parser_restore_free(parsesave);
    if (!expr) {
        PYS_RAISE_EXC(c, c->EXC_SyntaxError, "%s", parse_error_msg);
    }
    // When the caller supplies a globals dict (e.g. namedtuple's
    // `eval(code, {'_tuple_new': tuple.__new__})`), the resulting
    // function value's `fglobals` must point to a struct that contains
    // those entries even after we return — otherwise the lambda's later
    // gref lookups miss `_tuple_new`.  ns_inject mutates the active
    // globals in place and ns_restore reverts; that's wrong here.
    // Instead build a fresh pysglobals (copy of current + ns entries),
    // swap it in for the EVAL call, then restore.  Any function created
    // during EVAL captures this fresh struct via `c->globals`.
    struct pysglobals *saved_g = c->globals;
    struct pysglobals *eval_g = saved_g;
    if (argc >= 2 && pys_is_dict(argv[1])) {
        extern struct pysglobals *pys_globals_new(void);
        eval_g = pys_globals_new();
        // Copy current globals, then overlay ns entries.
        for (size_t i = 0; i < saved_g->size; i++) {
            int idx = (int)eval_g->size;
            if (eval_g->size == eval_g->capa) {
                size_t cap = eval_g->capa ? eval_g->capa * 2 : 32;
                eval_g->entries = (struct gentry *)GC_realloc(eval_g->entries, cap * sizeof(struct gentry));
                eval_g->capa = cap;
            }
            eval_g->entries[idx] = saved_g->entries[i];
            eval_g->size++;
        }
        c->globals = eval_g;
        struct pysdict *d = PYS_PTR(argv[1])->dict;
        for (size_t i = 0; i < d->elen; i++) {
            if (!pydict_entry_live(d, i)) continue;
            VALUE k = d->entries[i].key;
            if (!pys_is_str(k)) continue;
            const char *name = intern_name(PYS_PTR(k)->str.chars, PYS_PTR(k)->str.len);
            pys_global_define(c, name, d->entries[i].value);
        }
    }
    struct ns_save sl = { 0 };
    if (argc >= 3) ns_inject(c, argv[2], &sl);
    VALUE r = EVAL(c, expr);
    ns_restore(c, &sl);
    c->globals = saved_g;
    return r;
}

// Snapshot every defined global name so we can find names newly added
// (or changed) during exec/eval, and write them back to the user's
// supplied namespace dict.
static void
ns_snapshot(CTX *c, struct pysdict **out)
{
    *out = NULL;
    VALUE d = pys_make_dict();
    struct pysglobals *g = c->globals;
    for (size_t i = 0; i < g->size; i++) {
        if (!g->entries[i].defined) continue;
        VALUE k = pys_make_str(g->entries[i].name, strlen(g->entries[i].name));
        pys_dict_set(c, d, k, g->entries[i].value);
    }
    *out = PYS_PTR(d)->dict;
}

// After exec/eval, copy back into ns_dict any globals that are now
// defined or whose values changed since the snapshot.
static void
ns_writeback(CTX *c, VALUE ns_dict, struct pysdict *snapshot)
{
    if (ns_dict == PYS_NONE || !pys_is_dict(ns_dict) || !snapshot) return;
    struct pysglobals *g = c->globals;
    for (size_t i = 0; i < g->size; i++) {
        if (!g->entries[i].defined) continue;
        VALUE k = pys_make_str(g->entries[i].name, strlen(g->entries[i].name));
        uint64_t h = pys_hash(c, k);
        int32_t prev = pydict_find(c, snapshot, k, h);
        VALUE prev_v = prev >= 0 ? snapshot->entries[prev].value : (VALUE)0;
        if (prev < 0 || prev_v != g->entries[i].value) {
            pys_dict_set(c, ns_dict, k, g->entries[i].value);
        }
    }
}

static VALUE
bi_exec(CTX *c, int argc, VALUE *argv)
{
    // Accept str or bytes — `compile(...)` is a pass-through so a code
    // arg may have been bytes from the start.
    const char *code_chars = NULL;
    size_t L = 0;
    if (pys_is_str(argv[0])) {
        code_chars = PYS_PTR(argv[0])->str.chars;
        L = PYS_PTR(argv[0])->str.len;
    } else if (pys_is_bytes(argv[0]) || pys_is_bytearray(argv[0])) {
        code_chars = PYS_PTR(argv[0])->str.chars;
        L = PYS_PTR(argv[0])->str.len;
    } else {
        PYS_RAISE_EXC(c, c->EXC_TypeError, "exec: code must be str or bytes");
    }
    char *src = (char *)GC_malloc_atomic(L + 2);
    memcpy(src, code_chars, L);
    src[L] = '\n'; src[L+1] = '\0';
    extern void *lexer_save_alloc(void);
    extern void  lexer_restore_free(void *s);
    extern void *parser_save_alloc(void);
    extern void  parser_restore_free(void *s);
    extern jmp_buf *parse_error_jmp;
    extern char parse_error_msg[];
    void *lexsave = lexer_save_alloc();
    void *parsesave = parser_save_alloc();
    jmp_buf jb;
    jmp_buf *saved_jmp = parse_error_jmp;
    parse_error_jmp = &jb;
    NODE *body = NULL;
    if (setjmp(jb) == 0) {
        tokenize(src, "<exec>");
        body = parse_program();
    }
    parse_error_jmp = saved_jmp;
    lexer_restore_free(lexsave);
    parser_restore_free(parsesave);
    if (!body) {
        PYS_RAISE_EXC(c, c->EXC_SyntaxError, "%s", parse_error_msg);
    }
    struct ns_save sg = { 0 }, sl = { 0 };
    if (argc >= 2) ns_inject(c, argv[1], &sg);
    if (argc >= 3) ns_inject(c, argv[2], &sl);
    struct pysdict *snap = NULL;
    if (argc >= 2 && pys_is_dict(argv[1])) ns_snapshot(c, &snap);
    EVAL(c, body);
    // CPython convention: when both globals and locals are passed,
    // names defined at the exec'd-code top level land in locals (not
    // globals).  Without a separate locals dict, fall back to globals.
    if (snap) {
        VALUE wb = (argc >= 3 && pys_is_dict(argv[2])) ? argv[2] : argv[1];
        ns_writeback(c, wb, snap);
    }
    ns_restore(c, &sl);
    ns_restore(c, &sg);
    return PYS_NONE;
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
    if (pys_is_func(v) || pys_is_builtin(v) || pys_is_bound(v) || pys_is_class(v)) return PYS_TRUE;
    if (pys_is_instance(v)) {
        VALUE call = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), PYS_INTERN_call);
        return call != PYS_NONE ? PYS_TRUE : PYS_FALSE;
    }
    return PYS_FALSE;
}

// issubclass(cls, classinfo) — classinfo can be a class or tuple of classes.
static VALUE
bi_issubclass(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE cls = argv[0], info = argv[1];
    if (pys_is_tuple(info)) {
        size_t n = PYS_PTR(info)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE av[2] = { cls, PYS_PTR(info)->list.items[i] };
            if (bi_issubclass(c, 2, av) == PYS_TRUE) return PYS_TRUE;
        }
        return PYS_FALSE;
    }
    if (!pys_is_class(cls)) PYS_RAISE_EXC(c, c->EXC_TypeError, "issubclass() arg 1 must be a class");
    if (!pys_is_class(info)) PYS_RAISE_EXC(c, c->EXC_TypeError, "issubclass() arg 2 must be a class");
    // Dispatch via metaclass __subclasscheck__ (not for ABCMeta — that
    // path's classmethod call mechanics don't survive pys_apply, so
    // ABCMeta uses the _abc_registry walk below).
    {
        VALUE meta = pys_class_lookup_method(info, PYS_INTERN_metaclass);
        if (meta != PYS_NONE && pys_is_class(meta)) {
            const char *meta_name = PYS_PTR(meta)->cls.name;
            bool is_abcmeta = (meta_name && strcmp(meta_name, "ABCMeta") == 0);
            if (!is_abcmeta) {
                VALUE m = pys_class_lookup_method(meta, "__subclasscheck__");
                if (m != PYS_NONE) {
                    VALUE av[2] = { info, cls };
                    VALUE r = pys_apply(c, m, 2, av);
                    if (UNLIKELY(!r)) return false;
                    return pys_is_truthy(r) ? PYS_TRUE : PYS_FALSE;
                }
            }
        }
    }
    if (class_is_ancestor(cls, info)) return PYS_TRUE;
    // ABCMeta virtual registry: `info._abc_registry` (a set) lists the
    // virtual subclasses of `info`.  Look it up *own-only* (not via
    // MRO) so a base class's registry doesn't leak into the subclass —
    // complex is registered on Number, but that does NOT make complex a
    // subclass of Integral.
    {
        struct pysclass *icd = &PYS_PTR(info)->cls;
        VALUE reg = PYS_NONE;
        for (int j = 0; j < icd->nmethods; j++)
            if (strcmp(icd->methods[j].name, "_abc_registry") == 0) {
                reg = icd->methods[j].value; break;
            }
        if (reg != PYS_NONE && pys_is_any_set(reg)) {
            struct pysdict *dr = PYS_PTR(reg)->dict;
            for (size_t i = 0; i < dr->ecapa; i++) {
                if (!pydict_entry_live(dr, i)) continue;
                VALUE r_cls = dr->entries[i].key;
                if (pys_is_class(r_cls)) {
                    if (cls == r_cls || class_is_ancestor(cls, r_cls)) return PYS_TRUE;
                }
            }
        }
    }
    return PYS_FALSE;
}

// Match `v`'s Python type against a class.  Handles both built-in
// type classes (matched via cls.builtin_tag) and user classes
// (matched via instance.cls's MRO).
static bool
pys_isinstance_check(CTX *c, VALUE v, VALUE cls)
{
    if (!pys_is_class(cls)) {
        PYS_RAISE_EXC(c, c->EXC_TypeError, "isinstance() second arg must be class");
    }
    // Dispatch via metaclass __instancecheck__ if defined (and not the
    // pystro-stub abc.py one — that path's classmethod call mechanics
    // don't survive pys_apply, so for ABCMeta we drop straight into the
    // _abc_registry walk).
    {
        VALUE meta = pys_class_lookup_method(cls, PYS_INTERN_metaclass);
        if (meta != PYS_NONE && pys_is_class(meta)) {
            const char *meta_name = PYS_PTR(meta)->cls.name;
            bool is_abcmeta = (meta_name && strcmp(meta_name, "ABCMeta") == 0);
            if (!is_abcmeta) {
                VALUE m = pys_class_lookup_method(meta, "__instancecheck__");
                if (m != PYS_NONE) {
                    VALUE av[2] = { cls, v };
                    VALUE r = pys_apply(c, m, 2, av);
                    if (UNLIKELY(!r)) return false;
                    return pys_is_truthy(r);
                }
            }
        }
    }
    // ABCMeta virtual subclass: see if `type(v)` is a registered virtual
    // subclass of `cls`.  Reuses bi_issubclass's MRO + _abc_registry
    // walk so isinstance(5, numbers.Integral) works.
    {
        VALUE av[1] = { v };
        VALUE vtype = bi_type(c, 1, av);
        if (pys_is_class(vtype) && c->state == PYS_STATE_NORMAL) {
            VALUE iv[2] = { vtype, cls };
            VALUE r = bi_issubclass(c, 2, iv);
            if (c->state == PYS_STATE_NORMAL && r == PYS_TRUE) return true;
            if (c->state != PYS_STATE_NORMAL) {
                c->state = PYS_STATE_NORMAL;
                c->state_value = PYS_NONE;
            }
        }
    }
    struct pysclass *cd = &PYS_PTR(cls)->cls;
    // Quick: object accepts everything.
    if (cls == c->TYPE_object) return true;
    // Built-in type class — check value's tag.
    // First find the value's "type class" and walk its MRO.
    {
        VALUE av[1] = { v };
        VALUE vtype = bi_type(c, 1, av);
        if (pys_is_class(vtype) && vtype != cls) {
            // Walk MRO.
            struct pysclass *vd = &PYS_PTR(vtype)->cls;
            for (int i = 0; i < vd->nmro; i++) if (vd->mro[i] == cls) return true;
        } else if (vtype == cls) {
            return true;
        }
    }
    if (cd->builtin_ctor) {
        const char *nm = cd->name;
        if (strcmp(nm, "int")        == 0) return pys_is_int(v) || v == PYS_TRUE || v == PYS_FALSE;
        if (strcmp(nm, "float")      == 0) return pys_is_float(v);
        if (strcmp(nm, "str")        == 0) return pys_is_str(v);
        if (strcmp(nm, "bool")       == 0) return (v == PYS_TRUE || v == PYS_FALSE);
        if (strcmp(nm, "list")       == 0) return pys_is_list(v);
        if (strcmp(nm, "tuple")      == 0) return pys_is_tuple(v);
        if (strcmp(nm, "dict")       == 0) return pys_is_dict(v);
        if (strcmp(nm, "set")        == 0) return pys_is_set(v);
        if (strcmp(nm, "frozenset")  == 0) return pys_is_frozenset(v);
        if (strcmp(nm, "bytes")      == 0) return pys_is_bytes(v);
        if (strcmp(nm, "bytearray")  == 0) return pys_is_bytearray(v);
        if (strcmp(nm, "range")      == 0) return pys_is_range(v);
        if (strcmp(nm, "complex")    == 0) return pys_is_complex(v);
        if (strcmp(nm, "type")       == 0) return pys_is_class(v);
        return false;
    }
    // User class — `v` must be an instance whose class has cls in MRO.
    if (!pys_is_instance(v)) return false;
    return class_is_ancestor(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), cls);
}

static VALUE
bi_isinstance(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0], cls = argv[1];
    if (pys_is_tuple(cls)) {
        size_t n = PYS_PTR(cls)->list.len;
        for (size_t i = 0; i < n; i++) {
            if (pys_isinstance_check(c, v, PYS_PTR(cls)->list.items[i])) return PYS_TRUE;
        }
        return PYS_FALSE;
    }
    return pys_isinstance_check(c, v, cls) ? PYS_TRUE : PYS_FALSE;
}

static VALUE
bi_min(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) PYS_RAISE_EXC(c, c->EXC_TypeError, "min() needs args");
    VALUE key_fn = pys_bi_kwarg("key");
    VALUE deflt  = pys_bi_kwarg("default");
    VALUE best = PYS_NONE, best_key = PYS_NONE;
    bool started = false;
    if (argc == 1) {
        struct pys_iter it; pys_iter_init(c, &it, argv[0]);
        if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
            VALUE xk = key_fn ? pys_apply(c, key_fn, 1, &x) : x;
            if (!started || pys_cmp(c, xk, best_key) < 0) {
                best = x; best_key = xk; started = true;
            }
        }
        if (!started) {
            if (deflt) return deflt;
            PYS_RAISE_EXC(c, c->EXC_ValueError, "min() empty");
        }
        return best;
    }
    for (int i = 0; i < argc; i++) {
        VALUE xk = key_fn ? pys_apply(c, key_fn, 1, &argv[i]) : argv[i];
        if (!started || pys_cmp(c, xk, best_key) < 0) {
            best = argv[i]; best_key = xk; started = true;
        }
    }
    return best;
}

static VALUE
bi_max(CTX *c, int argc, VALUE *argv)
{
    if (argc == 0) PYS_RAISE_EXC(c, c->EXC_TypeError, "max() needs args");
    VALUE key_fn = pys_bi_kwarg("key");
    VALUE deflt  = pys_bi_kwarg("default");
    VALUE best = PYS_NONE, best_key = PYS_NONE;
    bool started = false;
    if (argc == 1) {
        struct pys_iter it; pys_iter_init(c, &it, argv[0]);
        if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
        VALUE x;
        while (pys_iter_next(c, &it, &x)) {
            VALUE xk = key_fn ? pys_apply(c, key_fn, 1, &x) : x;
            if (!started || pys_cmp(c, xk, best_key) > 0) {
                best = x; best_key = xk; started = true;
            }
        }
        if (!started) {
            if (deflt) return deflt;
            PYS_RAISE_EXC(c, c->EXC_ValueError, "max() empty");
        }
        return best;
    }
    for (int i = 0; i < argc; i++) {
        VALUE xk = key_fn ? pys_apply(c, key_fn, 1, &argv[i]) : argv[i];
        if (!started || pys_cmp(c, xk, best_key) > 0) {
            best = argv[i]; best_key = xk; started = true;
        }
    }
    return best;
}

static VALUE
bi_sum(CTX *c, int argc, VALUE *argv)
{
    VALUE acc = (argc >= 2) ? argv[1] : PYS_FIX(0);
    // CPython explicitly forbids sum() with str / bytes / bytearray
    // because the obvious implementation is O(N²).  Match that even
    // when start is the default 0 — `sum(["a","b"])` is TypeError.
    if (argc < 2 || acc == PYS_FIX(0)) {
        // Peek the iterable's first element type without consuming it
        // is awkward; instead, gate on the canonical user error: when
        // the start value is implicitly 0 and the first element is str
        // / bytes / bytearray, the resulting `0 + "a"` would already
        // raise.  Pre-empt with the CPython message.
        if (pys_is_str(argv[0]))
            PYS_RAISE_EXC(c, c->EXC_TypeError,
                         "sum() can't sum strings [use ''.join(seq) instead]");
        if (pys_is_bytes(argv[0]) || pys_is_bytearray(argv[0]))
            PYS_RAISE_EXC(c, c->EXC_TypeError,
                         "sum() can't sum bytes [use b''.join(seq) instead]");
    }
    struct pys_iter it; pys_iter_init(c, &it, argv[0]);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE x;
    bool first = true;
    while (pys_iter_next(c, &it, &x)) {
        if (first && (argc < 2)) {
            if (pys_is_str(x))
                PYS_RAISE_EXC(c, c->EXC_TypeError,
                             "sum() can't sum strings [use ''.join(seq) instead]");
            if (pys_is_bytes(x) || pys_is_bytearray(x))
                PYS_RAISE_EXC(c, c->EXC_TypeError,
                             "sum() can't sum bytes [use b''.join(seq) instead]");
            first = false;
        }
        acc = pys_add(c, acc, x);
        if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    }
    return acc;
}

static VALUE
bi_sorted(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    // Suppress bi_list's kwarg check — sorted() forwards its key=/reverse=
    // kwargs to lm_sort, but bi_list now raises TypeError on any kwarg.
    int saved_kwc = PYS_BI_KWC;
    PYS_BI_KWC = 0;
    VALUE r = bi_list(c, 1, argv);
    PYS_BI_KWC = saved_kwc;
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return 0;
    lm_sort(c, 1, &r);
    return r;
}

static VALUE
bi_enumerate(CTX *c, int argc, VALUE *argv)
{
    VALUE start_v = pys_bi_kwarg("start");
    int64_t i = start_v ? pys_int_to_long(c, start_v)
                        : (argc >= 2 ? pys_int_to_long(c, argv[1]) : 0);
    struct pysobj *o = pys_alloc(PYS_T_ITER);
    o->iter_state = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter));
    o->iter_state->kind = 8;
    o->iter_state->i = i;
    o->iter_state->inner = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter));
    pys_iter_init(c, &o->iter_state->inner[0], argv[0]);
    o->iter_state->n_inner = 1;
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_zip(CTX *c, int argc, VALUE *argv)
{
    VALUE strict = pys_bi_kwarg("strict");
    struct pysobj *o = pys_alloc(PYS_T_ITER);
    o->iter_state = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter));
    o->iter_state->kind = 9;
    o->iter_state->n_inner = argc;
    o->iter_state->i = (strict == PYS_TRUE) ? 1 : 0;  // strict flag
    if (argc == 0) {
        o->iter_state->inner = NULL;
        return PYS_OBJ_VAL(o);
    }
    o->iter_state->inner = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter) * argc);
    for (int i = 0; i < argc; i++) {
        pys_iter_init(c, &o->iter_state->inner[i], argv[i]);
        if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    }
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_chr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    int64_t k = pys_int_to_long(c, argv[0]);
    if (k < 0 || k > 0x10FFFF) PYS_RAISE_EXC(c, c->EXC_ValueError, "chr() out of range");
    // Encode as UTF-8.
    unsigned char b[5];
    int n;
    if (k < 0x80) {
        b[0] = (unsigned char)k; n = 1;
    } else if (k < 0x800) {
        b[0] = 0xC0 | (k >> 6);
        b[1] = 0x80 | (k & 0x3F);
        n = 2;
    } else if (k < 0x10000) {
        b[0] = 0xE0 | (k >> 12);
        b[1] = 0x80 | ((k >> 6) & 0x3F);
        b[2] = 0x80 | (k & 0x3F);
        n = 3;
    } else {
        b[0] = 0xF0 | (k >> 18);
        b[1] = 0x80 | ((k >> 12) & 0x3F);
        b[2] = 0x80 | ((k >> 6) & 0x3F);
        b[3] = 0x80 | (k & 0x3F);
        n = 4;
    }
    b[n] = 0;
    return pys_make_str((char *)b, (size_t)n);
}

static VALUE
bi_ord(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "ord() expected str");
    const unsigned char *s = (const unsigned char *)PYS_PTR(argv[0])->str.chars;
    size_t L = PYS_PTR(argv[0])->str.len;
    if (L == 1) return PYS_FIX((unsigned char)s[0]);
    // UTF-8 decode for multi-byte single character.
    int n = 0; int64_t cp = 0;
    if ((s[0] & 0x80) == 0) { cp = s[0]; n = 1; }
    else if ((s[0] & 0xE0) == 0xC0 && L >= 2) {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); n = 2;
    } else if ((s[0] & 0xF0) == 0xE0 && L >= 3) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); n = 3;
    } else if ((s[0] & 0xF8) == 0xF0 && L >= 4) {
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12)
           | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); n = 4;
    }
    if (n == 0 || (size_t)n != L)
        PYS_RAISE_EXC(c, c->EXC_TypeError, "ord() expected single character");
    return pys_make_int(cp);
}

static VALUE
bi_hex(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    // Coerce via __index__ if available.
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), PYS_INTERN_index);
        if (m != PYS_NONE) {
            VALUE av[1] = { v };
            v = pys_apply(c, m, 1, av);
            if (c->state == PYS_STATE_RAISE) return 0;
        }
    }
    if (!pys_int_or_bool(v)) PYS_RAISE_EXC(c, c->EXC_TypeError, "hex() needs int");
    mpz_t z; pys_to_mpz(c, v, z);
    char *s = mpz_get_str(NULL, 16, z);
    char *r;
    int an = (s[0] == '-') ? asprintf(&r, "-0x%s", s + 1) : asprintf(&r, "0x%s", s);
    (void)an;
    VALUE rv = pys_make_str(r, strlen(r));
    free(r); mpz_clear(z); return rv;
}

static VALUE
bi_bin(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), PYS_INTERN_index);
        if (m != PYS_NONE) { VALUE av[1] = { v }; v = pys_apply(c, m, 1, av);
            if (c->state == PYS_STATE_RAISE) return 0; }
    }
    if (!pys_int_or_bool(v)) PYS_RAISE_EXC(c, c->EXC_TypeError, "bin() needs int");
    mpz_t z; pys_to_mpz(c, v, z);
    char *s = mpz_get_str(NULL, 2, z);
    char *r;
    int an = (s[0] == '-') ? asprintf(&r, "-0b%s", s + 1) : asprintf(&r, "0b%s", s);
    (void)an;
    VALUE rv = pys_make_str(r, strlen(r));
    free(r); mpz_clear(z); return rv;
}

static VALUE
bi_oct(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), PYS_INTERN_index);
        if (m != PYS_NONE) { VALUE av[1] = { v }; v = pys_apply(c, m, 1, av);
            if (c->state == PYS_STATE_RAISE) return 0; }
    }
    if (!pys_int_or_bool(v)) PYS_RAISE_EXC(c, c->EXC_TypeError, "oct() needs int");
    mpz_t z; pys_to_mpz(c, v, z);
    char *s = mpz_get_str(NULL, 8, z);
    char *r;
    int an = (s[0] == '-') ? asprintf(&r, "-0o%s", s + 1) : asprintf(&r, "0o%s", s);
    (void)an;
    VALUE rv = pys_make_str(r, strlen(r));
    free(r); mpz_clear(z); return rv;
}

// slice(stop) / slice(start, stop) / slice(start, stop, step)
static VALUE
bi_slice(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *o = pys_alloc(PYS_T_SLICE);
    if (argc == 1) {
        o->slice_.start = PYS_NONE;
        o->slice_.stop  = argv[0];
        o->slice_.step  = PYS_NONE;
    } else if (argc == 2) {
        o->slice_.start = argv[0];
        o->slice_.stop  = argv[1];
        o->slice_.step  = PYS_NONE;
    } else {
        o->slice_.start = argv[0];
        o->slice_.stop  = argv[1];
        o->slice_.step  = argv[2];
    }
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_memoryview(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    VALUE v = argv[0];
    if (!(pys_is_bytes(v) || pys_is_bytearray(v)))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "memoryview: bytes-like required");
    struct pysobj *o = pys_alloc(PYS_T_MEMVIEW);
    o->memview.source = v;
    o->memview.off = 0;
    o->memview.len = PYS_PTR(v)->str.len;
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_breakpoint(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    // No-op stub; pystro has no debugger.  Honour PYTHONBREAKPOINT=0.
    return PYS_NONE;
}

// `compile(source, filename, mode)` — pystro doesn't expose a syntax
// tree object; return the source string for `exec` / `eval` to re-tokenize.
// We DO try to parse the source so SyntaxError surfaces here (CPython's
// behaviour) rather than later inside exec/eval.
static VALUE
bi_compile(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    const char *code_chars = NULL;
    size_t L = 0;
    if (pys_is_str(argv[0])) {
        code_chars = PYS_PTR(argv[0])->str.chars;
        L = PYS_PTR(argv[0])->str.len;
    } else if (pys_is_bytes(argv[0]) || pys_is_bytearray(argv[0])) {
        code_chars = PYS_PTR(argv[0])->str.chars;
        L = PYS_PTR(argv[0])->str.len;
    } else {
        return argv[0];        // unknown type — pass through
    }
    const char *mode = "exec";
    if (argc >= 3 && pys_is_str(argv[2])) mode = PYS_PTR(argv[2])->str.chars;
    char *src = (char *)GC_malloc_atomic(L + 2);
    memcpy(src, code_chars, L);
    src[L] = '\n'; src[L+1] = '\0';
    extern void *lexer_save_alloc(void);
    extern void  lexer_restore_free(void *s);
    extern void *parser_save_alloc(void);
    extern void  parser_restore_free(void *s);
    extern jmp_buf *parse_error_jmp;
    extern char parse_error_msg[];
    void *lexsave = lexer_save_alloc();
    void *parsesave = parser_save_alloc();
    jmp_buf jb;
    jmp_buf *saved_jmp = parse_error_jmp;
    parse_error_jmp = &jb;
    bool ok = false;
    if (setjmp(jb) == 0) {
        tokenize(src, "<compile>");
        if (strcmp(mode, "eval") == 0) {
            (void)parse_eval_expr();
        } else {
            (void)parse_program();
        }
        ok = true;
    }
    parse_error_jmp = saved_jmp;
    lexer_restore_free(lexsave);
    parser_restore_free(parsesave);
    if (!ok) {
        PYS_RAISE_EXC(c, c->EXC_SyntaxError, "%s", parse_error_msg);
    }
    return argv[0];
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
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), "__format__");
        if (m != PYS_NONE) {
            VALUE spec = (argc >= 2 && pys_is_str(argv[1])) ? argv[1] : pys_make_str("", 0);
            VALUE av[2] = { v, spec };
            return pys_apply(c, m, 2, av);
        }
    }
    if (argc < 2 || !pys_is_str(argv[1]) || PYS_PTR(argv[1])->str.len == 0)
        return pys_to_str(c, v);
    const char *s = PYS_PTR(argv[1])->str.chars;
    size_t n = PYS_PTR(argv[1])->str.len;
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
        if (PYS_IS_FIXNUM(v)) iv = PYS_FIXVAL(v);
        else if (v == PYS_TRUE) iv = 1;
        else if (v == PYS_FALSE) iv = 0;
        else if (pys_is_bignum(v)) {
            char *bs = mpz_get_str(NULL, type_ch == 'b' ? 2 : type_ch == 'o' ? 8 :
                                          (type_ch == 'x' || type_ch == 'X') ? 16 : 10,
                                   PYS_PTR(v)->mpz);
            snprintf(body, sizeof(body), "%s", bs);
            goto pad;
        }
        else iv = (long long)pys_to_double(c, v);
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
        double d = pys_to_double(c, v);
        if (precision >= 0) snprintf(fmt, sizeof(fmt), "%%.%d%c", precision, type_ch);
        else                snprintf(fmt, sizeof(fmt), "%%%c", type_ch);
        snprintf(body, sizeof(body), fmt, d);
    } else if (type_ch == '%') {
        double d = pys_to_double(c, v) * 100.0;
        if (precision >= 0) snprintf(fmt, sizeof(fmt), "%%.%df%%%%", precision);
        else                snprintf(fmt, sizeof(fmt), "%%f%%%%");
        snprintf(body, sizeof(body), fmt, d);
    } else if (type_ch == 's' || type_ch == 0) {
        // No type AND numeric value with sign/width/precision/comma → use 'd'/'g' path.
        if (type_ch == 0 && (PYS_IS_FIXNUM(v) || pys_is_bignum(v) || v == PYS_TRUE || v == PYS_FALSE)
            && (sign_ch != 0 || comma_sep)) {
            // Render as decimal int.
            long long iv = PYS_IS_FIXNUM(v) ? PYS_FIXVAL(v)
                          : v == PYS_TRUE ? 1 : v == PYS_FALSE ? 0 : 0;
            if (pys_is_bignum(v)) {
                char *bs = mpz_get_str(NULL, 10, PYS_PTR(v)->mpz);
                snprintf(body, sizeof(body), "%s", bs);
            } else {
                snprintf(body, sizeof(body), "%lld", iv);
            }
            // sign handled in pad path.
        } else if (type_ch == 0 && pys_is_float(v) && (sign_ch != 0 || comma_sep)) {
            double d = pys_to_double(c, v);
            int prec = precision < 0 ? 6 : precision;
            snprintf(body, sizeof(body), "%.*g", prec, d);
        } else {
            VALUE sv = pys_to_str(c, v);
            if (pys_is_str(sv)) {
                size_t L = PYS_PTR(sv)->str.len;
                if (L >= sizeof(body)) L = sizeof(body) - 1;
                memcpy(body, PYS_PTR(sv)->str.chars, L);
                body[L] = '\0';
                if (precision >= 0 && (size_t)precision < L) body[precision] = '\0';
            }
        }
    } else {
        return pys_to_str(c, v);    // unknown type — fall back
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
                        (type_ch == 0 && (PYS_IS_FIXNUM(v) || pys_is_bignum(v) ||
                                          v == PYS_TRUE || v == PYS_FALSE));
        bool float_like = (type_ch == 'f' || type_ch == 'g' || type_ch == 'e'
                           || type_ch == 'E' || type_ch == 'F'
                           || (type_ch == 0 && pys_is_float(v)));
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
                          (type_ch == 0 && (PYS_IS_FIXNUM(v) || pys_is_bignum(v)
                                            || pys_is_float(v) || v == PYS_TRUE || v == PYS_FALSE)));
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
        if ((int)bl >= width) return pys_make_str(body, bl);
        size_t pad = (size_t)width - bl;
        char *out = (char *)GC_malloc_atomic(width + 1);
        char eff_fill = zero_pad ? '0' : fill;
        if (align == 0) {
            // Default: numbers right-align, strings left-align.  For type_ch==0,
            // numeric values still right-align (matching CPython).
            bool numeric_v = PYS_IS_FIXNUM(v) || pys_is_bignum(v) || pys_is_float(v)
                             || v == PYS_TRUE || v == PYS_FALSE;
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
        return pys_make_str_take(out, (size_t)width);
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
pys_str_pct_format(CTX *c, VALUE fmt, VALUE args)
{
    const char *src = PYS_PTR(fmt)->str.chars;
    size_t srclen = PYS_PTR(fmt)->str.len;
    bool args_is_tuple = pys_is_tuple(args);
    size_t nargs = args_is_tuple ? PYS_PTR(args)->list.len : 1;
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
    bool args_is_dict = pys_is_dict(args);
    for (size_t i = 0; i < srclen; i++) {
        char ch = src[i];
        if (ch != '%') { OUT_CH(ch); continue; }
        i++;
        if (i >= srclen) PYS_RAISE_EXC(c, c->EXC_ValueError, "incomplete format");
        // %(name)X — mapping key.
        VALUE key_arg = PYS_NONE;
        if (src[i] == '(') {
            if (!args_is_dict)
                PYS_RAISE_EXC(c, c->EXC_TypeError, "format requires a mapping");
            i++;
            size_t ks = i;
            int depth = 1;
            while (i < srclen && depth > 0) {
                if (src[i] == '(') depth++;
                else if (src[i] == ')') { depth--; if (depth == 0) break; }
                i++;
            }
            if (i >= srclen) PYS_RAISE_EXC(c, c->EXC_ValueError, "unmatched '('");
            VALUE keystr = pys_make_str(src + ks, i - ks);
            i++;  // skip ')'
            uint64_t kh = pys_hash(c, keystr);
            int32_t kidx = pydict_find(c, PYS_PTR(args)->dict, keystr, kh);
            if (kidx < 0) PYS_RAISE_EXC(c, c->EXC_KeyError, "%s",
                                        PYS_PTR(keystr)->str.chars);
            key_arg = PYS_PTR(args)->dict->entries[kidx].value;
        }
        // Flags.
        bool flag_minus = false, flag_plus = false, flag_zero = false, flag_space = false, flag_hash = false;
        for (;; i++) {
            if (i >= srclen) PYS_RAISE_EXC(c, c->EXC_ValueError, "incomplete format");
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
            if (argi >= nargs) PYS_RAISE_EXC(c, c->EXC_TypeError, "not enough args");
            VALUE wv = args_is_tuple ? PYS_PTR(args)->list.items[argi++] : args;
            if (!args_is_tuple) argi++;
            width = (int)pys_int_to_long(c, wv);
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
                if (argi >= nargs) PYS_RAISE_EXC(c, c->EXC_TypeError, "not enough args");
                VALUE pv = args_is_tuple ? PYS_PTR(args)->list.items[argi++] : args;
                if (!args_is_tuple) argi++;
                precision = (int)pys_int_to_long(c, pv);
                i++;
            } else {
                precision = 0;
                while (i < srclen && src[i] >= '0' && src[i] <= '9') {
                    precision = precision * 10 + (src[i] - '0'); i++;
                }
            }
        }
        if (i >= srclen) PYS_RAISE_EXC(c, c->EXC_ValueError, "incomplete format");
        char conv = src[i];
        if (conv == '%') { OUT_CH('%'); continue; }
        // Get next arg.
        VALUE arg;
        if (key_arg != PYS_NONE || args_is_dict) {
            arg = key_arg;
        } else if (args_is_tuple) {
            if (argi >= nargs) PYS_RAISE_EXC(c, c->EXC_TypeError, "not enough args");
            arg = PYS_PTR(args)->list.items[argi++];
        } else {
            if (argi > 0) PYS_RAISE_EXC(c, c->EXC_TypeError, "not all args converted");
            arg = args; argi++;
        }
        // Render `arg` into `body`.
        char body[512];
        body[0] = '\0';
        size_t bl = 0;
        if (conv == 'd' || conv == 'i' || conv == 'u') {
            if (pys_is_bignum(arg)) {
                char *bs = mpz_get_str(NULL, 10, PYS_PTR(arg)->mpz);
                bl = strlen(bs);
                if (bl >= sizeof(body)) bl = sizeof(body) - 1;
                memcpy(body, bs, bl); body[bl] = '\0';
            } else {
                long long iv = pys_int_to_long(c, arg);
                if (flag_plus && iv >= 0)       bl = snprintf(body, sizeof(body), "+%lld", iv);
                else if (flag_space && iv >= 0) bl = snprintf(body, sizeof(body), " %lld", iv);
                else                            bl = snprintf(body, sizeof(body), "%lld", iv);
            }
        } else if (conv == 'x' || conv == 'X' || conv == 'o') {
            long long iv = pys_int_to_long(c, arg);
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
            long long iv = pys_int_to_long(c, arg);
            unsigned long long u = (unsigned long long)(iv < 0 ? -iv : iv);
            char buf[80]; int p = 0;
            if (u == 0) buf[p++] = '0';
            while (u) { buf[p++] = (u & 1) + '0'; u >>= 1; }
            int j = 0;
            if (iv < 0) body[j++] = '-';
            for (int k = p - 1; k >= 0; k--) body[j++] = buf[k];
            body[j] = '\0'; bl = (size_t)j;
        } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
            double d = pys_to_double(c, arg);
            char fmtb[16];
            int p = precision < 0 ? 6 : precision;
            snprintf(fmtb, sizeof(fmtb), "%%%s%s.%d%c",
                     flag_plus ? "+" : (flag_space ? " " : ""),
                     flag_hash ? "#" : "", p, conv);
            bl = snprintf(body, sizeof(body), fmtb, d);
        } else if (conv == 's') {
            VALUE sv = pys_to_str(c, arg);
            if (!pys_is_str(sv)) sv = pys_make_str("?", 1);
            bl = PYS_PTR(sv)->str.len;
            if (precision >= 0 && (size_t)precision < bl) bl = (size_t)precision;
            if (bl >= sizeof(body)) bl = sizeof(body) - 1;
            memcpy(body, PYS_PTR(sv)->str.chars, bl);
            body[bl] = '\0';
        } else if (conv == 'r' || conv == 'a') {
            VALUE sv = pys_to_repr(c, arg);
            if (!pys_is_str(sv)) sv = pys_make_str("?", 1);
            bl = PYS_PTR(sv)->str.len;
            if (precision >= 0 && (size_t)precision < bl) bl = (size_t)precision;
            if (bl >= sizeof(body)) bl = sizeof(body) - 1;
            memcpy(body, PYS_PTR(sv)->str.chars, bl);
            body[bl] = '\0';
        } else if (conv == 'c') {
            if (PYS_IS_FIXNUM(arg)) {
                int cv = (int)PYS_FIXVAL(arg);
                body[0] = (char)cv; body[1] = '\0'; bl = 1;
            } else if (pys_is_str(arg) && PYS_PTR(arg)->str.len == 1) {
                body[0] = PYS_PTR(arg)->str.chars[0]; body[1] = '\0'; bl = 1;
            } else PYS_RAISE_EXC(c, c->EXC_TypeError, "%%c needs int or 1-char str");
        } else {
            PYS_RAISE_EXC(c, c->EXC_ValueError, "unsupported format character '%c'", conv);
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
        PYS_RAISE_EXC(c, c->EXC_TypeError, "not all arguments converted");
    return pys_make_str(out, out_len);
#undef OUT_RESERVE
#undef OUT_PUT
#undef OUT_CH
}

static VALUE
bi_staticmethod(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = pys_alloc(PYS_T_STATICMETHOD);
    o->wrap.wrapped = argv[0];
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_classmethod(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc;
    struct pysobj *o = pys_alloc(PYS_T_CLASSMETHOD);
    o->wrap.wrapped = argv[0];
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_property(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    struct pysobj *o = pys_alloc(PYS_T_PROPERTY);
    o->wrap.wrapped = argc >= 1 ? argv[0] : PYS_NONE;
    o->wrap.setter  = argc >= 2 ? argv[1] : PYS_NONE;
    o->wrap.deleter = argc >= 3 ? argv[2] : PYS_NONE;
    // 4th arg = doc; ignored (no slot to store it on PYS_T_PROPERTY).
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_property_setter_call(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)c;
    struct pysobj *src = PYS_PTR(argv[0]);
    struct pysobj *o = pys_alloc(PYS_T_PROPERTY);
    o->wrap.wrapped = src->wrap.wrapped;
    o->wrap.setter  = argv[1];
    o->wrap.deleter = src->wrap.deleter;
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_property_deleter_call(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)c;
    struct pysobj *src = PYS_PTR(argv[0]);
    struct pysobj *o = pys_alloc(PYS_T_PROPERTY);
    o->wrap.wrapped = src->wrap.wrapped;
    o->wrap.setter  = src->wrap.setter;
    o->wrap.deleter = argv[1];
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_property_getter_call(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)c;
    struct pysobj *src = PYS_PTR(argv[0]);
    struct pysobj *o = pys_alloc(PYS_T_PROPERTY);
    o->wrap.wrapped = argv[1];
    o->wrap.setter  = src->wrap.setter;
    o->wrap.deleter = src->wrap.deleter;
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_input(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    if (argc >= 1 && pys_is_str(argv[0])) {
        fwrite(PYS_PTR(argv[0])->str.chars, 1, PYS_PTR(argv[0])->str.len, stdout);
        fflush(stdout);
    }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return pys_make_str("", 0);
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n') n--;
    return pys_make_str(buf, n);
}

static VALUE
bi_hash(CTX *c, int argc, VALUE *argv) {
    (void)argc;
    // CPython parity: hash returns Py_hash_t (signed).  Don't mask the
    // sign bit — if user code stashes hash(x) and feeds it back to
    // __hash__, the dict lookup hashes must compare equal (otherwise a
    // sign-bit-set value computed at storage time wouldn't match the
    // unmasked computation at lookup time).
    return PYS_FIX((int64_t)pys_hash(c, argv[0]));
}

static __thread int pys_skip_delattr_hook = 0;

static VALUE
bi_pystro_delattr(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE obj = argv[0];
    VALUE name = argv[1];
    if (!pys_is_str(name)) PYS_RAISE_EXC(c, c->EXC_TypeError, "delattr name must be str");
    if (pys_is_instance(obj)) {
        struct pysobj *o = PYS_PTR(obj);
        // Property deleter: if a property with fdel matches, call it.
        VALUE pm = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), PYS_PTR(name)->str.chars);
        if (pm != PYS_NONE && PYS_IS_PTR(pm) && PYS_PTR(pm)->type == PYS_T_PROPERTY) {
            VALUE deleter = PYS_PTR(pm)->wrap.deleter;
            if (deleter != PYS_NONE) {
                VALUE av[1] = { obj };
                pys_apply(c, deleter, 1, av);
                return PYS_NONE;
            }
            PYS_RAISE_EXC(c, c->EXC_AttributeError,
                         "property '%s' has no deleter", PYS_PTR(name)->str.chars);
        }
        // __delattr__ user override.
        if (!pys_skip_delattr_hook) {
            VALUE dm = pys_class_lookup_method(PYS_OBJ_VAL(o->inst.cls), "__delattr__");
            if (dm != PYS_NONE
                && !(PYS_IS_PTR(dm) && PYS_PTR(dm)->type == PYS_T_BUILTIN
                     && PYS_PTR(dm)->builtin.fn == bi_object_delattr)) {
                pys_skip_delattr_hook++;
                VALUE av[2] = { obj, name };
                pys_apply(c, dm, 2, av);
                pys_skip_delattr_hook--;
                return PYS_NONE;
            }
        }
        if (o->inst.attrs) {
            uint64_t h = pys_hash(c, name);
            size_t bucket; int32_t eidx; ssize_t ft;
            pydict_indices_lookup(c, o->inst.attrs, name, h, &bucket, &eidx, &ft);
            if (eidx >= 0) {
                o->inst.attrs->indices[bucket] = DICT_TOMB_IDX;
                o->inst.attrs->entries[eidx].key = DICT_DELETED_KEY;
                o->inst.attrs->entries[eidx].value = PYS_NONE;
                o->inst.attrs->used--;
                return PYS_NONE;
            }
        }
        PYS_RAISE_EXC(c, c->EXC_AttributeError, "no such attribute '%s'", PYS_PTR(name)->str.chars);
    }
    if (pys_is_class(obj)) {
        struct pysclass *cd = &PYS_PTR(obj)->cls;
        const char *cname = PYS_PTR(name)->str.chars;
        for (int i = 0; i < cd->nmethods; i++) {
            if (strcmp(cd->methods[i].name, cname) == 0) {
                for (int j = i; j + 1 < cd->nmethods; j++)
                    cd->methods[j] = cd->methods[j + 1];
                cd->nmethods--;
                SHARED_GLOBALS_SERIAL++;
                return PYS_NONE;
            }
        }
        PYS_RAISE_EXC(c, c->EXC_AttributeError, "no such attribute '%s'", cname);
    }
    if (pys_is_module(obj)) {
        struct pysglobals *g = PYS_PTR(obj)->module.globals;
        const char *cname = PYS_PTR(name)->str.chars;
        for (size_t i = 0; i < g->size; i++) {
            if (strcmp(g->entries[i].name, cname) == 0 && g->entries[i].defined) {
                g->entries[i].defined = false;
                g->entries[i].value = PYS_NONE;
                return PYS_NONE;
            }
        }
        PYS_RAISE_EXC(c, c->EXC_AttributeError, "no such attribute '%s'", cname);
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "delattr: object does not support attribute deletion");
}

static VALUE
bi_pystro_delglobal(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "name must be str");
    const char *name = PYS_PTR(argv[0])->str.chars;
    int i = pys_global_index(c, name);
    if (i < 0 || !c->globals->entries[i].defined)
        PYS_RAISE_EXC(c, c->EXC_NameError, "name '%s' is not defined", name);
    c->globals->entries[i].defined = false;
    c->globals->entries[i].value = PYS_NONE;
    c->globals->serial = ++SHARED_GLOBALS_SERIAL;     // structural change
    return PYS_NONE;
}

extern void install_builtins(CTX *c);

// Cache of already-imported modules (name → module pysobj).
// Lives in `c->globals` of the main module... actually use a static
// global so re-import shares the same instance regardless of which
// module triggers the import.
static VALUE PYS_MODULES = 0;     // PYS_T_DICT — initialised lazily

static VALUE
modules_dict(CTX *c)
{
    if (!PYS_MODULES) PYS_MODULES = pys_make_dict();
    (void)c;
    return PYS_MODULES;
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

// Like bi_import but swallows ModuleNotFoundError (returns None).
// Used by `from a.b import c` desugar where c may be either an
// attribute or a submodule.
static VALUE bi_try_import(CTX *c, int argc, VALUE *argv);

static VALUE
bi_import(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "import name must be str");
    VALUE mod_dict = modules_dict(c);
    if (pys_dict_has(c, mod_dict, argv[0])) {
        VALUE cached = pys_dict_get(c, mod_dict, argv[0]);
        // Re-attach to parent if needed: a previous import of `a.b` may
        // have run before `a` was loaded, so `a.b` was never set on `a`'s
        // globals.  Now that `a` is in sys.modules (or about to be), wire
        // the link up.
        const char *cname = PYS_PTR(argv[0])->str.chars;
        size_t cname_len = PYS_PTR(argv[0])->str.len;
        const char *clast = NULL;
        for (size_t i = 0; i < cname_len; i++)
            if (cname[i] == '.') clast = &cname[i];
        if (clast) {
            VALUE pname = pys_make_str(cname, (size_t)(clast - cname));
            if (pys_dict_has(c, mod_dict, pname)) {
                VALUE par = pys_dict_get(c, mod_dict, pname);
                if (pys_is_module(par) && pys_is_module(cached)) {
                    struct pysglobals *saved = c->globals;
                    c->globals = PYS_PTR(par)->module.globals;
                    VALUE existing = PYS_NONE;
                    if (!pys_global_lookup(c, clast + 1, &existing)
                        || existing != cached) {
                        pys_global_set(c, clast + 1, cached);
                    }
                    c->globals = saved;
                }
            }
        }
        return cached;
    }

    const char *name = PYS_PTR(argv[0])->str.chars;
    size_t name_len = PYS_PTR(argv[0])->str.len;
    // Relative-import resolution: parser encodes leading-dot count as
    // `\1<n>\1<rest>` where <n> is the number of dots.  Convert to an
    // absolute name based on current module's __package__ (a global
    // string set when the module was loaded).
    if (name_len > 0 && name[0] == '\1') {
        const char *p2 = name + 1;
        int n_dots = 0;
        while (*p2 >= '0' && *p2 <= '9') { n_dots = n_dots * 10 + (*p2 - '0'); p2++; }
        if (*p2 == '\1') p2++;
        // Look up __package__ for the current module.
        VALUE pkg = PYS_NONE;
        pys_global_lookup(c, "__package__", &pkg);
        const char *pkg_str = (pkg != PYS_NONE && pys_is_str(pkg)) ? PYS_PTR(pkg)->str.chars : "";
        size_t pkg_len = strlen(pkg_str);
        // Strip (n_dots - 1) trailing path segments from package.
        for (int i = 0; i < n_dots - 1; i++) {
            const char *last = NULL;
            for (size_t j = 0; j < pkg_len; j++) if (pkg_str[j] == '.') last = pkg_str + j;
            if (!last) { pkg_len = 0; break; }
            pkg_len = (size_t)(last - pkg_str);
        }
        // Build absolute name: pkg_str[0..pkg_len] + "." + p2 (rest).
        // p2 may itself start with '.' when the side-import path was
        // built as `<rel>.submod` from a relative form whose rest was
        // empty (e.g. `from . import aliases` → side-import name
        // "\11\1.aliases"); strip the leading dot to avoid producing
        // "encodings..aliases".
        if (*p2 == '.') p2++;
        char abs[1024];
        size_t al = 0;
        if (pkg_len > 0) { memcpy(abs, pkg_str, pkg_len); al = pkg_len; }
        if (*p2) {
            if (al > 0) abs[al++] = '.';
            size_t rl = strlen(p2);
            if (al + rl + 1 >= sizeof(abs))
                PYS_RAISE_EXC(c, c->EXC_RuntimeError, "import: name too long");
            memcpy(abs + al, p2, rl);
            al += rl;
        }
        abs[al] = '\0';
        VALUE absv = pys_make_str(abs, al);
        VALUE av[1] = { absv };
        return bi_import(c, 1, av);
    }
    // Translate `a.b.c` into `a/b/c.py` for module form, and
    // `a.b.c/__init__.py` for package form (CPython-style packages).
    char modpath[1024], pkgpath[1024];
    if (name_len + 16 >= sizeof(modpath))
        PYS_RAISE_EXC(c, c->EXC_RuntimeError, "import: name too long");
    {
        char *q = modpath;
        for (size_t i = 0; i < name_len; i++)
            *q++ = (name[i] == '.') ? '/' : name[i];
        *q++ = '.'; *q++ = 'p'; *q++ = 'y'; *q = '\0';
    }
    {
        char *q = pkgpath;
        for (size_t i = 0; i < name_len; i++)
            *q++ = (name[i] == '.') ? '/' : name[i];
        strcpy(q, "/__init__.py");
    }
    // Search each base directory for either form (module first, then package).
    char path[1024];
    char *src = NULL;
    #define TRY_BOTH(BASE_FMT, BASE) do { \
        if (!src) { snprintf(path, sizeof(path), BASE_FMT, BASE, modpath); src = read_file_into_buf(path); } \
        if (!src) { snprintf(path, sizeof(path), BASE_FMT, BASE, pkgpath); src = read_file_into_buf(path); } \
    } while (0)
    // Modules where pystro's stub MUST take precedence over CPython's
    // version reachable via PYTHONPATH.  These depend on CPython-internal
    // C structure (real coroutines for `_collections_abc.py`'s
    // `(async def)().close()`, code-object cell internals for
    // `types.py`'s `_cell_factory`).  Pystro's smaller stubs are
    // sufficient for user code that only touches the public API.
    // Use CPython's pure-Python stdlib via PYTHONPATH wherever possible.
    // The narrow exception list below is for modules that depend on
    // CPython-internal C semantics pystro can't simulate yet:
    //
    //   types.py — line 57 `FrameType = type(exc.__traceback__.tb_frame)`.
    //     pystro's __traceback__ is a list of frame-name strings, not
    //     a real TracebackType with tb_frame / tb_lineno / tb_lasti /
    //     tb_next.  Until tracebacks are reified, override.
    //
    // (`_collections_abc.py`'s `(async def)().close()` and `f.__closure__`
    // issues are handled at the runtime level — async def returns a
    // fake coroutine, __closure__ returns synthetic cells.)
    static const char *pystro_first_modules[] = {
        // types.py — `tb_frame` access on non-reified __traceback__.
        "types.py",
        // abc.py / _py_abc.py — CPython's ABCMeta.__new__ uses
        // `super().__new__(mcls, ..., **kwargs)` which pystro's super()
        // doesn't spread correctly across builtin __new__ today.  Use
        // pystro's simpler ABCMeta until super-spread is fixed.
        "abc.py",
        "_py_abc.py",
        // enum.py — CPython's EnumMeta.__prepare__ returns a custom
        // _EnumDict that pystro can't propagate to the class-body
        // namespace (__prepare__ hook not implemented).  pystro's
        // enum.py works enough for `class C(Enum): A = 1`.
        "enum.py",
        // re.py — CPython's re is a package (re/_compiler.py /
        // re/_parser.py / re/__init__.py) and uses bytecode-level
        // pattern compilation.  pystro's re.py is a small Python
        // matcher; supports the common cases we need.
        "re.py",
        // importlib — CPython's is a package with C-level bootstrap;
        // pystro's flat stub module is enough for `import_module` /
        // `find_spec` / `_bootstrap_external` attr access.
        "importlib.py",
        // signal — CPython's pulls in `_signal` C accelerator; pystro
        // is single-threaded with no signal delivery model, so the
        // bundled no-op stub is sufficient.
        "signal.py",
        // sysconfig — CPython's pulls in `_sysconfigdata_*_linux*`
        // (build-time generated); pystro's stub provides plausible
        // defaults for `get_config_var` / `get_paths`.
        "sysconfig.py",
        // hashlib — CPython's pulls in `_hashlib` C accelerator and
        // builtin {md5, sha1, ...}; pystro only has md5 + sha256 via
        // __pystro_*__ builtins.  Stub maps the rest to sha256 so
        // `hashlib.new("shake_256")` doesn't ValueError.
        "hashlib.py",
        // socket — CPython's pulls in `_socket` C extension; pystro
        // is single-process / no networking, the bundled stub exposes
        // constants + a non-functional socket class for isinstance().
        "socket.py",
        // typing — CPython's typing.py is huge and uses
        // `super().__init__(...)` chains 3+ levels deep + descriptor
        // tricks pystro can't replicate.  Bundled stub provides the
        // common surface (List, Dict, Optional, Union, ...) for
        // annotation usage without enforcement.
        "typing.py",
        // ipaddress — CPython's module-level fixtures
        // (`IPv6Network('fe80::/10')`) trigger a pystro
        // attribute-access specialization quirk that only repros in
        // ipaddress.py's __slots__-heavy class layout.  Bundled
        // pure-Python stub provides the public surface used by
        // urllib.parse / shutil / pathlib chains.
        "ipaddress.py",
        // asyncio — CPython's is a multi-file package (base_events
        // pulls in selector_events + _overlapped on Windows etc.) and
        // relies on real coroutines.  Pystro is sync-only; the bundled
        // single-file stub covers `asyncio.run` / `gather` / Lock /
        // Queue / staggered well enough for test-suite import-time.
        "asyncio.py",
        // unittest — CPython's is a package whose mock.py and async_case
        // submodules pull on real coroutines / descriptor edge cases
        // pystro can't replicate.  Pystro ships a small package
        // (stdlib/unittest/) with __init__.py providing TestCase and
        // mock.py providing the API surface used by `from unittest
        // import mock` / `from unittest.mock import MagicMock`.
        "unittest.py",
        "unittest/mock.py",
        // multiprocessing — CPython's is a 30+ file package that
        // assumes fork/spawn semantics pystro doesn't have.  Pystro
        // ships an importable stub package so tests can import
        // multiprocessing.context / Manager / Process without crashing
        // (they SkipTest at runtime instead).
        "multiprocessing.py",
        "multiprocessing/context.py",
        // tempfile — CPython's queries the candidate dirs by writing a
        // probe file, which can fail under sandbox (e.g. /tmp visible
        // but not writable as the test user).  Pystro's stub honours
        // $TMPDIR / $TEMP / $TMP env vars and falls back to /tmp
        // without probing.
        "tempfile.py",
        NULL,
    };
    bool pystro_wins = false;
    for (const char **pf = pystro_first_modules; *pf; pf++) {
        if (strcmp(modpath, *pf) == 0) { pystro_wins = true; break; }
    }
    // Search order:
    //   1. CWD-relative (script-local helpers / packages)
    //   2. (pystro_wins) Bundled stdlib first
    //   3. PYTHONPATH (user packages + CPython stdlib at `cpython/Lib`)
    //   4. Bundled stdlib (`<bindir>/stdlib/`) — fallback for everything
    //      else.  Last so user code under PYTHONPATH can override.
    if (!src) src = read_file_into_buf(modpath);
    if (!src) src = read_file_into_buf(pkgpath);
    if (pystro_wins && !src) {
        extern const char *PYS_BINDIR;
        if (PYS_BINDIR) {
            snprintf(path, sizeof(path), "%s/stdlib/%s", PYS_BINDIR, modpath);
            src = read_file_into_buf(path);
            if (!src) {
                snprintf(path, sizeof(path), "%s/stdlib/%s", PYS_BINDIR, pkgpath);
                src = read_file_into_buf(path);
            }
        }
    }
    if (!src) {
        const char *pp = getenv("PYTHONPATH");
        while (pp && *pp && !src) {
            const char *colon = strchr(pp, ':');
            size_t plen = colon ? (size_t)(colon - pp) : strlen(pp);
            if (plen + strlen(modpath) + 2 < sizeof(path)) {
                memcpy(path, pp, plen); path[plen] = '/';
                strcpy(path + plen + 1, modpath);
                src = read_file_into_buf(path);
                if (!src) {
                    memcpy(path, pp, plen); path[plen] = '/';
                    strcpy(path + plen + 1, pkgpath);
                    src = read_file_into_buf(path);
                }
            }
            pp = colon ? colon + 1 : NULL;
        }
    }
    if (!src) {
        extern const char *PYS_BINDIR;
        if (PYS_BINDIR) {
            snprintf(path, sizeof(path), "%s/stdlib/%s", PYS_BINDIR, modpath);
            src = read_file_into_buf(path);
            if (!src) {
                snprintf(path, sizeof(path), "%s/stdlib/%s", PYS_BINDIR, pkgpath);
                src = read_file_into_buf(path);
            }
        }
    }
    #undef TRY_BOTH
    if (!src) {
        // For dotted names (os.path), if a file isn't found try to resolve
        // `tail` as an attribute of the parent module (`os.path` → os.path).
        const char *last_dot = NULL;
        for (size_t i = 0; i < name_len; i++) if (name[i] == '.') last_dot = &name[i];
        if (last_dot) {
            size_t parent_len = (size_t)(last_dot - name);
            VALUE parent_name = pys_make_str(name, parent_len);
            VALUE av[1] = { parent_name };
            VALUE parent = bi_import(c, 1, av);
            if (c->state == PYS_STATE_NORMAL && parent != PYS_NONE) {
                VALUE attr = pys_getattr(c, parent, last_dot + 1);
                if (c->state == PYS_STATE_NORMAL && attr != PYS_NONE) {
                    pys_dict_set(c, mod_dict, argv[0], attr);
                    return attr;
                }
                c->state = PYS_STATE_NORMAL;
            }
        }
        PYS_RAISE_EXC(c, c->EXC_ModuleNotFoundError, "No module named '%s'", name);
    }

    // Build a fresh globals namespace and install the same builtins so
    // user code can call print() etc.  Save the caller's exception
    // classes — install_builtins overwrites c->EXC_* with fresh classes,
    // and we want the imported module to reuse the same exception
    // classes as the caller (so `except` matches across modules).
    struct pysglobals *new_g = pys_globals_new();
    struct pysglobals *saved_g = c->globals;
    // A caller in the middle of a class body (e.g. `class M: from X
    // import Y`) leaves c->current_class non-NONE.  Module-level code
    // inside X.py — `def`, `class`, name assigns — would then mis-attach
    // to the caller's class instead of the module's globals.  Force
    // PYS_NONE for the nested module init.
    VALUE saved_current_class = c->current_class;
    c->current_class = PYS_NONE;
#define SAVE_EXC(name) VALUE saved_EXC_##name = c->EXC_##name;
    PYS_EXC_LIST(SAVE_EXC)
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
    // Synthetic non-constructable types whose identity must be preserved
    // across module boundaries (so isinstance(x, types.MethodType) etc. work).
    VALUE saved_TYPE_slice     = c->TYPE_slice;
    VALUE saved_TYPE_function  = c->TYPE_function;
    VALUE saved_TYPE_method    = c->TYPE_method;
    VALUE saved_TYPE_NoneType  = c->TYPE_NoneType;
    VALUE saved_TYPE_module    = c->TYPE_module;
    VALUE saved_TYPE_generator = c->TYPE_generator;
    c->globals = new_g;
    install_builtins(c);
    // Re-bind the imported module's globals to the caller's exception
    // classes so exceptions raised inside cross module boundary match
    // by identity.
#define RESTORE_EXC(name) c->EXC_##name = saved_EXC_##name;
    PYS_EXC_LIST(RESTORE_EXC)
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
    c->TYPE_slice     = saved_TYPE_slice;
    c->TYPE_function  = saved_TYPE_function;
    c->TYPE_method    = saved_TYPE_method;
    c->TYPE_NoneType  = saved_TYPE_NoneType;
    c->TYPE_module    = saved_TYPE_module;
    c->TYPE_generator = saved_TYPE_generator;
    pys_global_set(c, "int",        c->TYPE_int);
    pys_global_set(c, "float",      c->TYPE_float);
    pys_global_set(c, "complex",    c->TYPE_complex);
    pys_global_set(c, "bool",       c->TYPE_bool);
    pys_global_set(c, "str",        c->TYPE_str);
    pys_global_set(c, "bytes",      c->TYPE_bytes);
    pys_global_set(c, "bytearray",  c->TYPE_bytearray);
    pys_global_set(c, "list",       c->TYPE_list);
    pys_global_set(c, "tuple",      c->TYPE_tuple);
    pys_global_set(c, "dict",       c->TYPE_dict);
    pys_global_set(c, "set",        c->TYPE_set);
    pys_global_set(c, "frozenset",  c->TYPE_frozenset);
    pys_global_set(c, "range",      c->TYPE_range);
    pys_global_set(c, "type",       c->TYPE_type);
    pys_global_set(c, "object",     c->TYPE_object);
    pys_global_set(c, "slice",      c->TYPE_slice);
#define SET_EXC_GLOBAL(name) pys_global_set(c, #name, c->EXC_##name);
    PYS_EXC_LIST(SET_EXC_GLOBAL)
#undef SET_EXC_GLOBAL
    pys_global_set(c, "IOError",              c->EXC_OSError);

    // Set __name__ + __package__ in the module's globals so relative
    // imports work and the module knows its identity.  Package name is
    // the dotted prefix up to the last dot (or the whole name if the
    // import was a package itself).
    pys_global_set(c, "__name__", pys_make_str(name, strlen(name)));
    pys_global_set(c, "__file__", pys_make_str(path, strlen(path)));
    // CPython modules always have __doc__ defined (None when no docstring
    // is parsed).  Tests like pdb.py do `__doc__ += ...` at module level
    // and rely on the name being bound.
    pys_global_set(c, "__doc__", PYS_NONE);
    // __builtins__ is exposed for `exec()` etc.
    pys_global_set(c, "__builtins__", PYS_NONE);
    // __spec__ is set by importlib in CPython 3.4+.
    pys_global_set(c, "__spec__", PYS_NONE);
    // __loader__ also from PEP 302.
    pys_global_set(c, "__loader__", PYS_NONE);
    // __cached__ — bytecode cache path; we don't compile so leave None.
    pys_global_set(c, "__cached__", PYS_NONE);
    {
        // Package: parent of the dotted name, OR name itself if loaded
        // as `<pkg>/__init__.py`.  We approximate by checking whether
        // the loaded path ends with `__init__.py`.
        const char *last_dot = NULL;
        for (size_t i = 0; i < name_len; i++)
            if (name[i] == '.') last_dot = &name[i];
        bool is_pkg = false;
        size_t plen = strlen(path);
        if (plen >= 11 && strcmp(path + plen - 11, "__init__.py") == 0) is_pkg = true;
        if (is_pkg) {
            pys_global_set(c, "__package__", pys_make_str(name, name_len));
        } else if (last_dot) {
            size_t pkg_len = (size_t)(last_dot - name);
            pys_global_set(c, "__package__", pys_make_str(name, pkg_len));
        } else {
            pys_global_set(c, "__package__", pys_make_str("", 0));
        }
    }

    // tokenize + parse the module file.  Nested imports (one module's
    // import triggering another's load) re-enter tokenize/parse_program;
    // both lexer and parser keep state in file-static globals, so we
    // serialise the whole state via opaque save/restore helpers.
    extern void *lexer_save_alloc(void);
    extern void  lexer_restore_free(void *s);
    extern void *parser_save_alloc(void);
    extern void  parser_restore_free(void *s);
    void *lexsave = lexer_save_alloc();
    void *parsesave = parser_save_alloc();
    tokenize(src, path);
    NODE *body = parse_program();
    lexer_restore_free(lexsave);
    parser_restore_free(parsesave);

    // Cache a placeholder module BEFORE executing the body, so a
    // self-referential import (e.g. test/support/__init__.py doing
    // `from test.support import os_helper`) finds the in-progress
    // module in sys.modules instead of re-loading recursively.
    // (also see bi_try_import below)
    struct pysobj *placeholder_mo = pys_alloc(PYS_T_MODULE);
    placeholder_mo->module.name = name;
    placeholder_mo->module.globals = new_g;
    VALUE placeholder = PYS_OBJ_VAL(placeholder_mo);
    pys_dict_set(c, mod_dict, argv[0], placeholder);

    // Run module body; state-based propagation observes raise.
    EVAL(c, body);
    if (c->state == PYS_STATE_RAISE) {
        // Module init raised — restore caller's globals first, then
        // re-raise so the caller's try/except (running on the original
        // globals) sees it.  Remove the placeholder so a retry won't
        // see the half-initialised module.  pys_dict_remove bails out
        // when state == RAISE, so clear it across the remove call.
        VALUE exc = c->state_value;
        c->state = PYS_STATE_NORMAL;
        c->state_value = PYS_NONE;
        pys_dict_remove(c, mod_dict, argv[0]);
        c->globals = saved_g;
        c->current_class = saved_current_class;
        free(src);
        c->state = PYS_STATE_RAISE;
        c->state_value = exc;
        return 0;
    }
    c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;

    // Reuse the placeholder we cached above (its globals dict is the
    // same `new_g` that the body just initialised).
    VALUE mod = placeholder;

    c->globals = saved_g;
    c->current_class = saved_current_class;
    pys_dict_set(c, mod_dict, argv[0], mod);
    // Post-load injection for modules that mutate `globals()` to define
    // module attrs — pystro's `globals()` returns a snapshot dict so
    // those mutations are lost.  Hand-inject the well-known cases.
    if (PYS_PTR(argv[0])->str.len == 7
        && memcmp(PYS_PTR(argv[0])->str.chars, "inspect", 7) == 0) {
        // CPython's inspect.py does
        //   for k, v in dis.COMPILER_FLAG_NAMES.items():
        //     mod_dict["CO_" + v] = k
        // Inject the standard 3.12 set explicitly.
        struct pysglobals *saved = c->globals;
        c->globals = PYS_PTR(mod)->module.globals;
        pys_global_set(c, "CO_OPTIMIZED",         PYS_FIX(0x0001));
        pys_global_set(c, "CO_NEWLOCALS",         PYS_FIX(0x0002));
        pys_global_set(c, "CO_VARARGS",           PYS_FIX(0x0004));
        pys_global_set(c, "CO_VARKEYWORDS",       PYS_FIX(0x0008));
        pys_global_set(c, "CO_NESTED",            PYS_FIX(0x0010));
        pys_global_set(c, "CO_GENERATOR",         PYS_FIX(0x0020));
        pys_global_set(c, "CO_NOFREE",            PYS_FIX(0x0040));
        pys_global_set(c, "CO_COROUTINE",         PYS_FIX(0x0080));
        pys_global_set(c, "CO_ITERABLE_COROUTINE",PYS_FIX(0x0100));
        pys_global_set(c, "CO_ASYNC_GENERATOR",   PYS_FIX(0x0200));
        c->globals = saved;
    }
    // For dotted modules `a.b.c`: ensure each ancestor (`a`, `a.b`) is
    // loaded and that the immediate parent has the leaf attached as an
    // attribute, so `import a.b.c; a.b.c.foo` resolves end-to-end.
    {
        const char *last_dot = NULL;
        for (size_t i = 0; i < name_len; i++)
            if (name[i] == '.') last_dot = &name[i];
        if (last_dot) {
            size_t parent_len = (size_t)(last_dot - name);
            VALUE parent_name = pys_make_str(name, parent_len);
            // If parent not yet cached, import it now so it gets a
            // module object we can attach onto.
            if (!pys_dict_has(c, mod_dict, parent_name)) {
                VALUE av[1] = { parent_name };
                bi_import(c, 1, av);
                // If parent init raised, propagate.
                if (c->state == PYS_STATE_RAISE) return 0;
            }
            if (pys_dict_has(c, mod_dict, parent_name)) {
                VALUE parent = pys_dict_get(c, mod_dict, parent_name);
                if (pys_is_module(parent)) {
                    const char *child = last_dot + 1;
                    struct pysglobals *saved = c->globals;
                    c->globals = PYS_PTR(parent)->module.globals;
                    pys_global_set(c, child, mod);
                    c->globals = saved;
                }
            }
        }
    }
    free(src);
    return mod;
}

static VALUE
bi_modules(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    return modules_dict(c);
}

// Best-effort submodule import: returns the imported module or None if
// not found.  Used to auto-load submodules in `from a.b import c`.
static VALUE
bi_try_import(CTX *c, int argc, VALUE *argv)
{
    int sst = c->state; VALUE sv = c->state_value;
    c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
    VALUE r = bi_import(c, argc, argv);
    if (c->state == PYS_STATE_RAISE) r = PYS_NONE;
    c->state = sst; c->state_value = sv;
    return r;
}

// Synthetic Exception.__init__(self, *args) — sets self.args and
// self.message (= args[0] if there's exactly one arg).
VALUE
bi_exception_init(CTX *c, int argc, VALUE *argv)
{
    VALUE self = argv[0];
    int nargs = argc - 1;
    VALUE args_tuple = pys_make_tuple(argc > 1 ? &argv[1] : NULL, nargs);
    pys_setattr(c, self, "args", args_tuple);
    if (nargs == 1) {
        pys_setattr(c, self, "message", argv[1]);
    } else if (nargs == 0) {
        pys_setattr(c, self, "message", pys_make_str("", 0));
    }
    // StopIteration(value) — expose .value (used by `yield from` /
    // generator return).  Harmless for other Exception subclasses.
    pys_setattr(c, self, "value", nargs >= 1 ? argv[1] : PYS_NONE);
    // SystemExit(code) — CPython exposes .code; for simplicity we set
    // it on every Exception (matches the .value pattern above and is a
    // no-op for non-SystemExit subclasses).
    pys_setattr(c, self, "code", nargs >= 1 ? argv[1] : PYS_NONE);
    // CPython always exposes __cause__ / __context__ / __traceback__ /
    // __suppress_context__ — initialise to None / False on construction.
    pys_setattr(c, self, "__cause__", PYS_NONE);
    pys_setattr(c, self, "__context__", PYS_NONE);
    pys_setattr(c, self, "__traceback__", PYS_NONE);
    pys_setattr(c, self, "__suppress_context__", PYS_FALSE);
    // SyntaxError exposes filename / lineno / offset / text / msg /
    // end_lineno / end_offset (None when not set).  Setting them
    // unconditionally is harmless on other Exception subclasses and
    // saves a class-check.
    pys_setattr(c, self, "filename",   PYS_NONE);
    pys_setattr(c, self, "lineno",     PYS_NONE);
    pys_setattr(c, self, "offset",     PYS_NONE);
    pys_setattr(c, self, "text",       PYS_NONE);
    pys_setattr(c, self, "end_lineno", PYS_NONE);
    pys_setattr(c, self, "end_offset", PYS_NONE);
    pys_setattr(c, self, "msg",        nargs >= 1 ? argv[1] : PYS_NONE);
    // OSError exposes errno / strerror / filename / filename2 / winerror.
    pys_setattr(c, self, "errno",     PYS_NONE);
    pys_setattr(c, self, "strerror",  PYS_NONE);
    pys_setattr(c, self, "filename2", PYS_NONE);
    pys_setattr(c, self, "winerror",  PYS_NONE);
    // ImportError / ModuleNotFoundError expose name / path.
    pys_setattr(c, self, "name", PYS_NONE);
    pys_setattr(c, self, "path", PYS_NONE);
    // UnicodeError siblings — encoding / object / start / end / reason.
    pys_setattr(c, self, "encoding", PYS_NONE);
    pys_setattr(c, self, "object",   PYS_NONE);
    pys_setattr(c, self, "start",    PYS_NONE);
    pys_setattr(c, self, "end",      PYS_NONE);
    pys_setattr(c, self, "reason",   PYS_NONE);
    // KeyError / AttributeError / NameError expose .name; the latter
    // two may also have .obj (AttributeError).  Setting `obj` to None
    // keeps `e.obj` access alive.
    pys_setattr(c, self, "obj", PYS_NONE);
    return PYS_NONE;
}

// Exception.with_traceback(tb) — `raise X.with_traceback(Y)` sets the
// exception's __traceback__ and returns the same instance for chaining.
VALUE
bi_exception_with_traceback(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    pys_setattr(c, argv[0], "__traceback__", argv[1]);
    return argv[0];
}

// Exception.add_note(s) — append to self.__notes__ list (CPython 3.11+).
VALUE
bi_exception_add_note(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[1]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "add_note: argument must be a str");
    VALUE notes = pys_getattr_optional(c, argv[0], "__notes__");
    if (!notes || notes == PYS_NONE) {
        notes = pys_make_list(NULL, 0);
        pys_setattr(c, argv[0], "__notes__", notes);
    }
    pys_list_append(c, notes, argv[1]);
    return PYS_NONE;
}

// Math primitives surfaced as `__pystro_*__` and wrapped by `math.py`.
static VALUE
bi_pystro_sqrt(CTX *c, int argc, VALUE *argv)
{ (void)argc; double d = pys_to_double(c, argv[0]);
  if (d < 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "math domain error");
  return pys_make_float(sqrt(d));
}
static VALUE
bi_pystro_sin(CTX *c, int argc, VALUE *argv)
{ (void)argc; return pys_make_float(sin(pys_to_double(c, argv[0]))); }
static VALUE
bi_pystro_cos(CTX *c, int argc, VALUE *argv)
{ (void)argc; return pys_make_float(cos(pys_to_double(c, argv[0]))); }
static VALUE
bi_pystro_tan(CTX *c, int argc, VALUE *argv)
{ (void)argc; return pys_make_float(tan(pys_to_double(c, argv[0]))); }
static VALUE
bi_pystro_log(CTX *c, int argc, VALUE *argv)
{
    double x = pys_to_double(c, argv[0]);
    if (x <= 0) PYS_RAISE_EXC(c, c->EXC_ValueError, "math domain error");
    if (argc >= 2) {
        double base = pys_to_double(c, argv[1]);
        if (base <= 0 || base == 1) PYS_RAISE_EXC(c, c->EXC_ValueError, "math domain error");
        return pys_make_float(log(x) / log(base));
    }
    return pys_make_float(log(x));
}
static VALUE
bi_pystro_exp(CTX *c, int argc, VALUE *argv)
{ (void)argc; return pys_make_float(exp(pys_to_double(c, argv[0]))); }
static VALUE
bi_pystro_floor(CTX *c, int argc, VALUE *argv)
{
    if (pys_is_instance(argv[0])) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls), "__floor__");
        if (m != PYS_NONE) return pys_apply(c, m, 1, argv);
    }
    (void)argc; return pys_make_int((int64_t)floor(pys_to_double(c, argv[0])));
}
static VALUE
bi_pystro_ceil(CTX *c, int argc, VALUE *argv)
{
    if (pys_is_instance(argv[0])) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls), "__ceil__");
        if (m != PYS_NONE) return pys_apply(c, m, 1, argv);
    }
    (void)argc; return pys_make_int((int64_t)ceil(pys_to_double(c, argv[0])));
}
static VALUE
bi_pystro_trunc_dispatch(CTX *c, int argc, VALUE *argv)
{
    if (pys_is_instance(argv[0])) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls), "__trunc__");
        if (m != PYS_NONE) return pys_apply(c, m, 1, argv);
    }
    (void)argc;
    double d = pys_to_double(c, argv[0]);
    return pys_make_int((int64_t)d);
}
static VALUE
bi_pystro_atan2(CTX *c, int argc, VALUE *argv)
{ (void)argc;
  return pys_make_float(atan2(pys_to_double(c, argv[0]), pys_to_double(c, argv[1]))); }
static VALUE
bi_pystro_pow(CTX *c, int argc, VALUE *argv)
{ (void)argc;
  return pys_make_float(pow(pys_to_double(c, argv[0]), pys_to_double(c, argv[1]))); }

// Time / OS primitives surfaced through `time.py` and `os.py`.
static VALUE
bi_pystro_time(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return pys_make_float(ts.tv_sec + ts.tv_nsec / 1e9);
}
static VALUE
bi_pystro_sleep(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    double d = pys_to_double(c, argv[0]);
    if (d <= 0) return PYS_NONE;
    struct timespec req = { (time_t)d, (long)((d - (time_t)d) * 1e9) };
    nanosleep(&req, NULL);
    return PYS_NONE;
}
static VALUE
bi_pystro_perf_counter(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return pys_make_float(ts.tv_sec + ts.tv_nsec / 1e9);
}
static VALUE
bi_pystro_getenv(CTX *c, int argc, VALUE *argv)
{
    if (!pys_is_str(argv[0])) {
        return argc >= 2 ? argv[1] : PYS_NONE;
    }
    size_t L = PYS_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PYS_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    const char *v = getenv(buf);
    if (v) return pys_make_str(v, strlen(v));
    return argc >= 2 ? argv[1] : PYS_NONE;
}
static VALUE
bi_pystro_getcwd(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return pys_make_str("", 0);
    return pys_make_str(buf, strlen(buf));
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
    if (!pys_is_str(argv[0]) && !pys_is_byteseq(argv[0]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "md5 needs str or bytes");
    const unsigned char *src = (const unsigned char *)PYS_PTR(argv[0])->str.chars;
    size_t L = PYS_PTR(argv[0])->str.len;
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
    return pys_make_str(hex, 32);
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
    if (!pys_is_str(argv[0]) && !pys_is_byteseq(argv[0]))
        PYS_RAISE_EXC(c, c->EXC_TypeError, "sha256 needs str or bytes");
    const unsigned char *src = (const unsigned char *)PYS_PTR(argv[0])->str.chars;
    size_t L = PYS_PTR(argv[0])->str.len;
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
    return pys_make_str(hex, 64);
}

static VALUE
bi_pystro_path_exists(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "exists: path must be str");
    size_t L = PYS_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PYS_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    return access(buf, F_OK) == 0 ? PYS_TRUE : PYS_FALSE;
}

static VALUE
bi_pystro_listdir(CTX *c, int argc, VALUE *argv)
{
    extern int closedir(DIR *);
    extern DIR *opendir(const char *);
    extern struct dirent *readdir(DIR *);
    const char *p = ".";
    char buf[1024];
    if (argc >= 1 && pys_is_str(argv[0])) {
        size_t L = PYS_PTR(argv[0])->str.len;
        if (L >= sizeof(buf)) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "listdir: path too long");
        memcpy(buf, PYS_PTR(argv[0])->str.chars, L); buf[L] = '\0';
        p = buf;
    }
    DIR *d = opendir(p);
    if (!d) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "listdir: %s", p);
    VALUE r = pys_make_list(NULL, 0);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        pys_list_append(c, r, pys_make_str(de->d_name, strlen(de->d_name)));
    }
    closedir(d);
    return r;
}

static VALUE
bi_pystro_remove(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "remove: path must be str");
    size_t L = PYS_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PYS_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    if (unlink(buf) != 0) {
        // Map errno to the same exception subclass CPython would raise
        // (CPython routes through PyErr_SetFromErrnoWithFilenameObject).
        VALUE cls;
        switch (errno) {
          case ENOENT:  cls = c->EXC_FileNotFoundError; break;
          case EISDIR:  cls = c->EXC_IsADirectoryError; break;
          case EACCES:
          case EPERM:   cls = c->EXC_PermissionError;   break;
          default:      cls = c->EXC_OSError;           break;
        }
        PYS_RAISE_EXC(c, cls, "[Errno %d] %s: '%s'", errno, strerror(errno), buf);
    }
    return PYS_NONE;
}

static VALUE
bi_pystro_rmdir(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "rmdir: path must be str");
    size_t L = PYS_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PYS_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    if (rmdir(buf) != 0) {
        VALUE cls;
        switch (errno) {
          case ENOENT:  cls = c->EXC_FileNotFoundError; break;
          case ENOTDIR: cls = c->EXC_NotADirectoryError; break;
          case ENOTEMPTY: cls = c->EXC_OSError; break;
          case EACCES:
          case EPERM:   cls = c->EXC_PermissionError; break;
          default:      cls = c->EXC_OSError; break;
        }
        PYS_RAISE_EXC(c, cls, "[Errno %d] %s: '%s'", errno, strerror(errno), buf);
    }
    return PYS_NONE;
}

static VALUE
bi_pystro_makedirs(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "makedirs: path must be str");
    size_t L = PYS_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PYS_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    bool exist_ok = (argc >= 2 && argv[1] == PYS_TRUE);
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
                    else if (errno != EEXIST) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "makedirs failed: %s", path);
                }
            }
            if (buf[i] == '/' && plen + 1 < sizeof(path)) {
                path[plen++] = '/'; path[plen] = '\0';
            }
        } else {
            if (plen + 1 < sizeof(path)) { path[plen++] = buf[i]; path[plen] = '\0'; }
        }
    }
    return PYS_NONE;
}

static VALUE
bi_pystro_isdir(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "isdir: path must be str");
    size_t L = PYS_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PYS_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    struct stat st;
    if (stat(buf, &st) != 0) return PYS_FALSE;
    return S_ISDIR(st.st_mode) ? PYS_TRUE : PYS_FALSE;
}

static VALUE
bi_pystro_isfile(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "isfile: path must be str");
    size_t L = PYS_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PYS_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    struct stat st;
    if (stat(buf, &st) != 0) return PYS_FALSE;
    return S_ISREG(st.st_mode) ? PYS_TRUE : PYS_FALSE;
}

// gc.collect() — force Boehm full GC and return bytes freed.
static VALUE
bi_pystro_gc_collect(CTX *c, int argc, VALUE *argv)
{
    (void)c; (void)argc; (void)argv;
    GC_gcollect();
    return pys_make_int(0);
}

// posix.stat()-equivalent — returns a tuple of (st_mode, st_ino,
// st_dev, st_nlink, st_uid, st_gid, st_size, st_atime, st_mtime,
// st_ctime).  CPython's `os.stat_result` is a structseq; the tuple
// form satisfies the common consumers (`st.st_mode`, indexing).
static VALUE
bi_pystro_stat(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "stat: path must be str");
    size_t L = PYS_PTR(argv[0])->str.len;
    char *buf = (char *)alloca(L + 1);
    memcpy(buf, PYS_PTR(argv[0])->str.chars, L); buf[L] = '\0';
    struct stat st;
    bool follow = true;
    VALUE fk = pys_bi_kwarg("follow_symlinks");
    if (fk) follow = pys_is_truthy(fk);
    int rc = follow ? stat(buf, &st) : lstat(buf, &st);
    if (rc != 0) PYS_RAISE_EXC(c, c->EXC_OSError, "stat: %s: %s", buf, strerror(errno));
    VALUE items[10];
    items[0] = pys_make_int((long)st.st_mode);
    items[1] = pys_make_int((long)st.st_ino);
    items[2] = pys_make_int((long)st.st_dev);
    items[3] = pys_make_int((long)st.st_nlink);
    items[4] = pys_make_int((long)st.st_uid);
    items[5] = pys_make_int((long)st.st_gid);
    items[6] = pys_make_int((long)st.st_size);
    items[7] = pys_make_float((double)st.st_atime);
    items[8] = pys_make_float((double)st.st_mtime);
    items[9] = pys_make_float((double)st.st_ctime);
    return pys_make_tuple(items, 10);
}

static VALUE
bi_pystro_abspath(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "abspath: path must be str");
    size_t L = PYS_PTR(argv[0])->str.len;
    char in[1024];
    if (L >= sizeof(in)) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "abspath: path too long");
    memcpy(in, PYS_PTR(argv[0])->str.chars, L); in[L] = '\0';
    char out[4096];
    if (in[0] == '/') {
        snprintf(out, sizeof(out), "%s", in);
    } else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof(cwd))) PYS_RAISE_EXC(c, c->EXC_RuntimeError, "abspath: getcwd");
        snprintf(out, sizeof(out), "%s/%s", cwd, in);
    }
    return pys_make_str(out, strlen(out));
}

// Process info / control surfaced through the `sys` module (sys.py).
extern int    PYS_ARGC;
extern char **PYS_ARGV;
static VALUE
bi_pystro_argv(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    VALUE r = pys_make_list(NULL, 0);
    for (int i = 0; i < PYS_ARGC; i++)
        pys_list_append(c, r, pys_make_str(PYS_ARGV[i], strlen(PYS_ARGV[i])));
    return r;
}
static VALUE
bi_pystro_exit(CTX *c, int argc, VALUE *argv)
{
    (void)c;
    int code = 0;
    if (argc >= 1 && PYS_IS_FIXNUM(argv[0])) code = (int)PYS_FIXVAL(argv[0]);
    fflush(stdout); fflush(stderr);
    exit(code);
}

static VALUE
bi_pystro_current_exc(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    if (c->current_handling_exc && c->current_handling_exc != PYS_NONE)
        return c->current_handling_exc;
    return PYS_NONE;
}

static VALUE
bi_pystro_get_recursion_limit(CTX *c, int argc, VALUE *argv)
{
    (void)argc; (void)argv;
    return pys_make_int((int64_t)c->recursion_limit);
}

static VALUE
bi_pystro_set_recursion_limit(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    int64_t n = pys_int_to_long(c, argv[0]);
    if (n < 1) PYS_RAISE_EXC(c, c->EXC_ValueError, "recursion limit must be >= 1");
    c->recursion_limit = (int)n;
    return PYS_NONE;
}

static VALUE
bi_pystro_localtime(CTX *c, int argc, VALUE *argv)
{
    time_t t;
    if (argc >= 1) {
        if (PYS_IS_FIXNUM(argv[0])) t = (time_t)PYS_FIXVAL(argv[0]);
        else t = (time_t)pys_to_double(c, argv[0]);
    } else {
        t = time(NULL);
    }
    struct tm tm_;
    localtime_r(&t, &tm_);
    static VALUE struct_time_cls = (VALUE)0;
    if (!struct_time_cls) struct_time_cls = pys_make_class("struct_time", PYS_NONE, false);
    VALUE inst = pys_make_instance(struct_time_cls);
    pys_setattr(c, inst, "tm_year",  pys_make_int(tm_.tm_year + 1900));
    pys_setattr(c, inst, "tm_mon",   pys_make_int(tm_.tm_mon + 1));
    pys_setattr(c, inst, "tm_mday",  pys_make_int(tm_.tm_mday));
    pys_setattr(c, inst, "tm_hour",  pys_make_int(tm_.tm_hour));
    pys_setattr(c, inst, "tm_min",   pys_make_int(tm_.tm_min));
    pys_setattr(c, inst, "tm_sec",   pys_make_int(tm_.tm_sec));
    pys_setattr(c, inst, "tm_wday",  pys_make_int((tm_.tm_wday + 6) % 7));
    pys_setattr(c, inst, "tm_yday",  pys_make_int(tm_.tm_yday + 1));
    pys_setattr(c, inst, "tm_isdst", pys_make_int(tm_.tm_isdst));
    return inst;
}

static VALUE
bi_pystro_gmtime(CTX *c, int argc, VALUE *argv)
{
    time_t t;
    if (argc >= 1) {
        if (PYS_IS_FIXNUM(argv[0])) t = (time_t)PYS_FIXVAL(argv[0]);
        else t = (time_t)pys_to_double(c, argv[0]);
    } else {
        t = time(NULL);
    }
    struct tm tm_;
    gmtime_r(&t, &tm_);
    static VALUE struct_time_cls = (VALUE)0;
    if (!struct_time_cls) struct_time_cls = pys_make_class("struct_time", PYS_NONE, false);
    VALUE inst = pys_make_instance(struct_time_cls);
    pys_setattr(c, inst, "tm_year",  pys_make_int(tm_.tm_year + 1900));
    pys_setattr(c, inst, "tm_mon",   pys_make_int(tm_.tm_mon + 1));
    pys_setattr(c, inst, "tm_mday",  pys_make_int(tm_.tm_mday));
    pys_setattr(c, inst, "tm_hour",  pys_make_int(tm_.tm_hour));
    pys_setattr(c, inst, "tm_min",   pys_make_int(tm_.tm_min));
    pys_setattr(c, inst, "tm_sec",   pys_make_int(tm_.tm_sec));
    pys_setattr(c, inst, "tm_wday",  pys_make_int((tm_.tm_wday + 6) % 7));
    pys_setattr(c, inst, "tm_yday",  pys_make_int(tm_.tm_yday + 1));
    pys_setattr(c, inst, "tm_isdst", pys_make_int(0));
    return inst;
}

static VALUE
bi_pystro_strftime(CTX *c, int argc, VALUE *argv)
{
    if (!pys_is_str(argv[0])) PYS_RAISE_EXC(c, c->EXC_TypeError, "strftime: format must be str");
    struct tm tm_;
    memset(&tm_, 0, sizeof(tm_));
    if (argc >= 2 && pys_is_instance(argv[1])) {
        VALUE t = argv[1];
        VALUE y = pys_getattr(c, t, "tm_year");
        VALUE m = pys_getattr(c, t, "tm_mon");
        VALUE d = pys_getattr(c, t, "tm_mday");
        VALUE H = pys_getattr(c, t, "tm_hour");
        VALUE M = pys_getattr(c, t, "tm_min");
        VALUE S = pys_getattr(c, t, "tm_sec");
        if (pys_int_or_bool(y)) tm_.tm_year = (int)pys_int_to_long(c, y) - 1900;
        if (pys_int_or_bool(m)) tm_.tm_mon  = (int)pys_int_to_long(c, m) - 1;
        if (pys_int_or_bool(d)) tm_.tm_mday = (int)pys_int_to_long(c, d);
        if (pys_int_or_bool(H)) tm_.tm_hour = (int)pys_int_to_long(c, H);
        if (pys_int_or_bool(M)) tm_.tm_min  = (int)pys_int_to_long(c, M);
        if (pys_int_or_bool(S)) tm_.tm_sec  = (int)pys_int_to_long(c, S);
        time_t tt = mktime(&tm_);
        if (tt != (time_t)-1) localtime_r(&tt, &tm_);
    } else {
        time_t tt = time(NULL);
        localtime_r(&tt, &tm_);
    }
    char buf[256];
    size_t n = strftime(buf, sizeof(buf), PYS_PTR(argv[0])->str.chars, &tm_);
    return pys_make_str(buf, n);
}

// Reinterpret a float as its IEEE-754 bits.  Returns int (may be a
// bignum for double's 64-bit pattern).
static VALUE
bi_pystro_float_to_bits(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    double d = pys_to_double(c, argv[0]);
    bool is_double = (argc < 2) || pys_is_truthy(argv[1]);
    if (is_double) {
        uint64_t b;
        memcpy(&b, &d, 8);
        // Build via mpz to avoid sign issues with int64_t.
        struct pysobj *o = pys_alloc(PYS_T_BIGNUM);
        mpz_init(o->mpz);
        // Use two limbs: hi 32 bits, lo 32 bits.
        mpz_set_ui(o->mpz, (unsigned long)(b >> 32));
        mpz_mul_2exp(o->mpz, o->mpz, 32);
        mpz_add_ui(o->mpz, o->mpz, (unsigned long)(b & 0xFFFFFFFFu));
        return PYS_OBJ_VAL(o);
    }
    float f = (float)d;
    uint32_t b;
    memcpy(&b, &f, 4);
    return pys_make_int((int64_t)b);
}

// Inverse of float_to_bits.
static VALUE
bi_pystro_bits_to_float(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    bool is_double = (argc < 2) || pys_is_truthy(argv[1]);
    if (is_double) {
        // Get the int as a 64-bit pattern.
        uint64_t b = 0;
        if (pys_is_bignum(argv[0])) {
            mpz_t tmp; mpz_init(tmp);
            mpz_set(tmp, PYS_PTR(argv[0])->mpz);
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
            int64_t s = pys_int_to_long(c, argv[0]);
            b = (uint64_t)s;
        }
        double d;
        memcpy(&d, &b, 8);
        return pys_make_float(d);
    }
    uint32_t b = (uint32_t)pys_int_to_long(c, argv[0]);
    float f;
    memcpy(&f, &b, 4);
    return pys_make_float((double)f);
}

// `from m import *` — copy all non-underscore names from the module's
// globals into the current frame's globals.  If the module defines
// `__all__` (a list of names), only those are exported.
static VALUE
bi_import_star(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE mod = argv[0];
    if (!pys_is_module(mod)) PYS_RAISE_EXC(c, c->EXC_TypeError, "import * needs a module");
    struct pysglobals *g = PYS_PTR(mod)->module.globals;
    // Honour __all__ if present and is a list/tuple of strings.
    VALUE all = 0;
    for (size_t i = 0; i < g->size; i++) {
        if (g->entries[i].defined && strcmp(g->entries[i].name, "__all__") == 0) {
            all = g->entries[i].value; break;
        }
    }
    if (all != 0 && (pys_is_list(all) || pys_is_tuple(all))) {
        size_t n = PYS_PTR(all)->list.len;
        for (size_t i = 0; i < n; i++) {
            VALUE nm = PYS_PTR(all)->list.items[i];
            if (!pys_is_str(nm)) continue;
            const char *cname = PYS_PTR(nm)->str.chars;
            for (size_t j = 0; j < g->size; j++) {
                if (g->entries[j].defined && strcmp(g->entries[j].name, cname) == 0) {
                    pys_global_set(c, cname, g->entries[j].value);
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
            pys_global_set(c, nm, g->entries[i].value);
        }
    }
    return PYS_NONE;
}

static VALUE
bi_pystro_yield_from(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    extern VALUE pys_gen_yield_from(CTX *c, VALUE iter);
    return pys_gen_yield_from(c, argv[0]);
}

// Unary + dispatch: call __pos__ on instances; identity for numeric;
// TypeError for str / bytes / None / classes / etc.
static VALUE
bi_pystro_pos(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE v = argv[0];
    if (pys_is_instance(v)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(v)->inst.cls), "__pos__");
        if (m != PYS_NONE) return pys_apply(c, m, 1, argv);
        // Fall through to type-check on primary if subclass of numeric.
        if (PYS_PTR(v)->inst.primary) v = PYS_PTR(v)->inst.primary;
    }
    if (pys_int_or_bool(v) || pys_is_float(v) || pys_is_complex(v)) return v;
    PYS_RAISE_EXC(c, c->EXC_TypeError, "bad operand type for unary +");
}

static VALUE
bi_pystro_del(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    VALUE container = argv[0], key = argv[1];
    if (pys_is_dict(container)) {
        if (!pys_dict_remove(c, container, key)) {
            VALUE r = pys_to_repr(c, key);
            PYS_RAISE_EXC(c, c->EXC_KeyError, "%s",
                         pys_is_str(r) ? PYS_PTR(r)->str.chars : "?");
        }
        return PYS_NONE;
    }
    if (pys_is_list(container)) {
        int64_t i = pys_int_to_long(c, key);
        struct pysobj *o = PYS_PTR(container);
        if (i < 0) i += (int64_t)o->list.len;
        if (i < 0 || i >= (int64_t)o->list.len)
            PYS_RAISE_EXC(c, c->EXC_IndexError, "del: index out of range");
        for (size_t j = (size_t)i; j + 1 < o->list.len; j++)
            o->list.items[j] = o->list.items[j + 1];
        o->list.len--;
        return PYS_NONE;
    }
    if (pys_is_set(container)) {
        pys_dict_remove(c, container, key);
        return PYS_NONE;
    }
    if (pys_is_instance(container)) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(container)->inst.cls), "__delitem__");
        if (m != PYS_NONE) {
            VALUE av[2] = { container, key };
            return pys_apply(c, m, 2, av);
        }
        // Built-in subclass (e.g., class OD(dict)): forward to primary.
        if (PYS_PTR(container)->inst.primary)
            return bi_pystro_del(c, argc, (VALUE[]){PYS_PTR(container)->inst.primary, key});
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "del: unsupported container type");
}

static VALUE
bi_all(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pys_iter it; pys_iter_init(c, &it, argv[0]);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) if (!pys_is_truthy(x)) return PYS_FALSE;
    return PYS_TRUE;
}

static VALUE
bi_any(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pys_iter it; pys_iter_init(c, &it, argv[0]);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    VALUE x;
    while (pys_iter_next(c, &it, &x)) if (pys_is_truthy(x)) return PYS_TRUE;
    return PYS_FALSE;
}

static VALUE
bi_divmod(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    // Dispatch to __divmod__ on instances.
    if (pys_is_instance(argv[0])) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls), "__divmod__");
        if (m != PYS_NONE) {
            return pys_apply(c, m, 2, argv);
        }
    }
    VALUE q = pys_fdiv(c, argv[0], argv[1]);
    VALUE r = pys_mod (c, argv[0], argv[1]);
    VALUE pair[2] = { q, r };
    return pys_make_tuple(pair, 2);
}

static VALUE
bi_round(CTX *c, int argc, VALUE *argv)
{
    // Dispatch to user-class __round__.
    if (pys_is_instance(argv[0])) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls), "__round__");
        if (m != PYS_NONE) {
            return pys_apply(c, m, argc, argv);
        }
    }
    int ndig = (argc >= 2) ? (int)pys_int_to_long(c, argv[1]) : 0;
    double d = pys_to_double(c, argv[0]);
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
    if (argc < 2) return PYS_FIX((int64_t)rounded);
    if (PYS_IS_FIXNUM(argv[0]) || pys_is_bignum(argv[0])) return PYS_FIX((int64_t)r);
    return pys_make_float(r);
}

static VALUE
bi_pow(CTX *c, int argc, VALUE *argv)
{
    // User class with __pow__: forward all args.
    if (pys_is_instance(argv[0])) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls), "__pow__");
        if (m != PYS_NONE) return pys_apply(c, m, argc, argv);
    }
    if (argc == 3) {
        // a ** b mod m — only int int int for v0.
        if (!pys_int_or_bool(argv[0]) || !pys_int_or_bool(argv[1]) || !pys_int_or_bool(argv[2]))
            PYS_RAISE_EXC(c, c->EXC_TypeError, "pow() with 3 args needs ints");
        mpz_t a, b, m, r;
        pys_to_mpz(c, argv[0], a); pys_to_mpz(c, argv[1], b); pys_to_mpz(c, argv[2], m);
        mpz_init(r); mpz_powm(r, a, b, m);
        VALUE rv = pys_normalise_int(r);
        mpz_clear(a); mpz_clear(b); mpz_clear(m); mpz_clear(r);
        return rv;
    }
    return pys_pow(c, argv[0], argv[1]);
}

// Wrap a fresh list as a PYS_T_ITER so reversed() returns a proper
// iterator (matches CPython's listreverseiterator etc.).
static VALUE pys_wrap_list_as_iter(CTX *c, VALUE lst) {
    struct pysobj *it_obj = pys_alloc(PYS_T_ITER);
    it_obj->iter_state = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter));
    pys_iter_init(c, it_obj->iter_state, lst);
    return PYS_OBJ_VAL(it_obj);
}
static VALUE
bi_reversed(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    // CPython returns a list_reverseiterator / tuple_reverseiterator.
    // Build the reversed sequence into a fresh list and wrap it as a
    // PYS_T_ITER (kind=0 = list iter) so callers' next() / list() work.
    if (pys_is_list(argv[0]) || pys_is_tuple(argv[0])) {
        struct pysobj *o = PYS_PTR(argv[0]);
        VALUE r = pys_make_list(NULL, 0);
        for (size_t i = o->list.len; i > 0; i--) pys_list_append(c, r, o->list.items[i - 1]);
        return pys_wrap_list_as_iter(c, r);
    }
    VALUE r = pys_make_list(NULL, 0);
    if (pys_is_str(argv[0])) {
        // Reverse codepoint by codepoint, not byte by byte.
        struct pysobj *o = PYS_PTR(argv[0]);
        const char *s = o->str.chars;
        size_t bytelen = o->str.len;
        // Build forward codepoint table.
        size_t cp_count = pys_str_cp_count(s, bytelen);
        size_t *off = (size_t *)GC_malloc_atomic(sizeof(size_t) * (cp_count + 1));
        size_t bi = 0; size_t ci = 0;
        off[0] = 0;
        while (bi < bytelen) {
            int step_b = pys_utf8_step(s, bi);
            bi += (size_t)step_b;
            ci++;
            off[ci] = bi;
        }
        for (size_t k = cp_count; k > 0; k--) {
            size_t b0 = off[k - 1];
            size_t b1 = off[k];
            pys_list_append(c, r, pys_make_str(s + b0, b1 - b0));
        }
        return pys_wrap_list_as_iter(c, r);
    }
    if (pys_is_byteseq(argv[0])) {
        struct pysobj *o = PYS_PTR(argv[0]);
        for (size_t i = o->str.len; i > 0; i--)
            pys_list_append(c, r, pys_make_int((int64_t)(unsigned char)o->str.chars[i - 1]));
        return pys_wrap_list_as_iter(c, r);
    }
    if (pys_is_range(argv[0])) {
        struct pysobj *o = PYS_PTR(argv[0]);
        int64_t s = o->range.start, e = o->range.stop, st = o->range.step;
        int64_t last;
        if (st > 0 && s < e) last = s + ((e - s - 1) / st) * st;
        else if (st < 0 && s > e) last = s + ((s - e - 1) / -st) * st;
        else return pys_wrap_list_as_iter(c, r);
        for (int64_t v = last; (st > 0 ? v >= s : v <= s); v -= st)
            pys_list_append(c, r, pys_make_int(v));
        return pys_wrap_list_as_iter(c, r);
    }
    if (pys_is_dict(argv[0])) {
        struct pysdict *d = PYS_PTR(argv[0])->dict;
        for (size_t i = d->elen; i > 0; i--)
            if (pydict_entry_live(d, i - 1)) pys_list_append(c, r, d->entries[i - 1].key);
        return pys_wrap_list_as_iter(c, r);
    }
    if (pys_is_instance(argv[0])) {
        VALUE m = pys_class_lookup_method(PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls), "__reversed__");
        if (m != PYS_NONE) {
            VALUE av[1] = { argv[0] };
            return pys_apply(c, m, 1, av);
        }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "argument to reversed() must be a sequence");
}

static VALUE
bi_map(CTX *c, int argc, VALUE *argv)
{
    if (argc < 2) PYS_RAISE_EXC(c, c->EXC_TypeError, "map() needs >=2 args");
    int n_iters = argc - 1;
    struct pysobj *o = pys_alloc(PYS_T_ITER);
    o->iter_state = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter));
    o->iter_state->kind = 10;
    o->iter_state->container = argv[0];
    o->iter_state->inner = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter) * n_iters);
    o->iter_state->n_inner = n_iters;
    for (int i = 0; i < n_iters; i++) {
        pys_iter_init(c, &o->iter_state->inner[i], argv[i + 1]);
        if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    }
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_filter(CTX *c, int argc, VALUE *argv)
{
    (void)argc;
    struct pysobj *o = pys_alloc(PYS_T_ITER);
    o->iter_state = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter));
    o->iter_state->kind = 11;
    o->iter_state->container = argv[0];   // None or callable
    o->iter_state->inner = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter));
    o->iter_state->n_inner = 1;
    pys_iter_init(c, &o->iter_state->inner[0], argv[1]);
    if (c->state != PYS_STATE_NORMAL) return PYS_NONE;
    return PYS_OBJ_VAL(o);
}

VALUE
bi_iter(CTX *c, int argc, VALUE *argv)
{
    if (argc == 2) {
        // iter(callable, sentinel)
        struct pysobj *o = pys_alloc(PYS_T_ITER);
        o->iter_state = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter));
        o->iter_state->kind = 4;
        o->iter_state->container = argv[0];
        o->iter_state->sentinel  = argv[1];
        return PYS_OBJ_VAL(o);
    }
    if (PYS_IS_PTR(argv[0]) && PYS_PTR(argv[0])->type == PYS_T_GEN) return argv[0];
    if (PYS_IS_PTR(argv[0]) && PYS_PTR(argv[0])->type == PYS_T_ITER) return argv[0];
    if (pys_is_instance(argv[0])) {
        VALUE cls = PYS_OBJ_VAL(PYS_PTR(argv[0])->inst.cls);
        VALUE im = pys_class_lookup_method(cls, PYS_INTERN_iter);
        if (im != PYS_NONE) {
            VALUE av[1] = { argv[0] };
            return pys_apply(c, im, 1, av);
        }
    }
    struct pysobj *o = pys_alloc(PYS_T_ITER);
    o->iter_state = (struct pys_iter *)GC_malloc(sizeof(struct pys_iter));
    pys_iter_init(c, o->iter_state, argv[0]);
    return PYS_OBJ_VAL(o);
}

static VALUE
bi_next(CTX *c, int argc, VALUE *argv)
{
    VALUE it = argv[0];
    if (PYS_IS_PTR(it) && PYS_PTR(it)->type == PYS_T_GEN) {
        if (argc >= 2 && PYS_PTR(it)->gen->done) return argv[1];
        VALUE r = pys_gen_next(c, it);
        if (c->state == PYS_STATE_RAISE && argc >= 2) {
            VALUE exc = c->state_value;
            if (pys_exc_matches(c, exc, c->EXC_StopIteration)) {
                c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
                return argv[1];
            }
        }
        return r;
    }
    if (PYS_IS_PTR(it) && PYS_PTR(it)->type == PYS_T_ITER) {
        VALUE r;
        if (pys_iter_next(c, PYS_PTR(it)->iter_state, &r)) return r;
        if (argc >= 2) return argv[1];
        // Set RAISE without longjmp so the caller (e.g. user __next__
        // bridging built-in iterators) can propagate StopIteration via
        // the standard RAISE state path rather than crossing setjmp
        // boundaries.
        VALUE si = pys_make_instance(c->EXC_StopIteration);
        pys_setattr(c, si, "args", pys_make_tuple(NULL, 0));
        pys_setattr(c, si, "value", PYS_NONE);
        pys_setattr(c, si, "__traceback__", PYS_NONE);
        pys_setattr(c, si, "__context__", PYS_NONE);
        pys_setattr(c, si, "__cause__", PYS_NONE);
        pys_setattr(c, si, "__suppress_context__", PYS_FALSE);
        c->state = PYS_STATE_RAISE;
        c->state_value = si;
        return 0;
    }
    if (pys_is_instance(it)) {
        VALUE cls = PYS_OBJ_VAL(PYS_PTR(it)->inst.cls);
        VALUE nm = pys_class_lookup_method(cls, PYS_INTERN_next);
        if (nm != PYS_NONE) {
            VALUE av[1] = { it };
            return pys_apply(c, nm, 1, av);
        }
    }
    PYS_RAISE_EXC(c, c->EXC_TypeError, "next() argument is not an iterator");
}

// Pre-interned dunder names.  Filled by install_builtins() so all
// pys_class_lookup_method calls in runtime.c can pass these globals
// instead of raw string literals.  Pointer equality with the names
// stored in pysclass_method (which the parser interns) makes the
// hot-loop comparison strcmp-free on hits.
const char *PYS_INTERN_init;
const char *PYS_INTERN_new;
const char *PYS_INTERN_eq;
const char *PYS_INTERN_lt;
const char *PYS_INTERN_hash;
const char *PYS_INTERN_setattr;
const char *PYS_INTERN_getattr;
const char *PYS_INTERN_getattribute;
const char *PYS_INTERN_bool;
const char *PYS_INTERN_len;
const char *PYS_INTERN_getitem;
const char *PYS_INTERN_setitem;
const char *PYS_INTERN_index;
const char *PYS_INTERN_invert;
const char *PYS_INTERN_neg;
const char *PYS_INTERN_metaclass;
const char *PYS_INTERN_set_name;
const char *PYS_INTERN_iter;
const char *PYS_INTERN_next;
const char *PYS_INTERN_call;
const char *PYS_INTERN_get;
const char *PYS_INTERN_repr;
const char *PYS_INTERN_str;
const char *PYS_INTERN_contains;
const char *PYS_INTERN_add, *PYS_INTERN_sub, *PYS_INTERN_mul;
const char *PYS_INTERN_truediv, *PYS_INTERN_floordiv, *PYS_INTERN_mod;
const char *PYS_INTERN_pow, *PYS_INTERN_or, *PYS_INTERN_and;
const char *PYS_INTERN_xor, *PYS_INTERN_lshift, *PYS_INTERN_rshift;
const char *PYS_INTERN_radd, *PYS_INTERN_rsub, *PYS_INTERN_rmul;
const char *PYS_INTERN_iadd, *PYS_INTERN_isub, *PYS_INTERN_imul;
const char *PYS_INTERN_le, *PYS_INTERN_gt, *PYS_INTERN_ge;
const char *PYS_INTERN_ne;

extern const char *intern_name(const char *s, size_t len);

static void
install_interned_names(void)
{
    PYS_INTERN_init        = intern_name("__init__", 8);
    PYS_INTERN_new         = intern_name("__new__", 7);
    PYS_INTERN_eq          = intern_name("__eq__", 6);
    PYS_INTERN_lt          = intern_name("__lt__", 6);
    PYS_INTERN_hash        = intern_name("__hash__", 8);
    PYS_INTERN_setattr     = intern_name("__setattr__", 11);
    PYS_INTERN_getattr     = intern_name("__getattr__", 11);
    PYS_INTERN_getattribute = intern_name("__getattribute__", 16);
    PYS_INTERN_bool        = intern_name("__bool__", 8);
    PYS_INTERN_len         = intern_name("__len__", 7);
    PYS_INTERN_getitem     = intern_name("__getitem__", 11);
    PYS_INTERN_setitem     = intern_name("__setitem__", 11);
    PYS_INTERN_index       = intern_name("__index__", 9);
    PYS_INTERN_invert      = intern_name("__invert__", 10);
    PYS_INTERN_neg         = intern_name("__neg__", 7);
    PYS_INTERN_metaclass   = intern_name("__metaclass__", 13);
    PYS_INTERN_set_name    = intern_name("__set_name__", 12);
    PYS_INTERN_iter        = intern_name("__iter__", 8);
    PYS_INTERN_next        = intern_name("__next__", 8);
    PYS_INTERN_call        = intern_name("__call__", 8);
    PYS_INTERN_get         = intern_name("__get__", 7);
    PYS_INTERN_repr        = intern_name("__repr__", 8);
    PYS_INTERN_str         = intern_name("__str__", 7);
    PYS_INTERN_contains    = intern_name("__contains__", 12);
    PYS_INTERN_add         = intern_name("__add__", 7);
    PYS_INTERN_sub         = intern_name("__sub__", 7);
    PYS_INTERN_mul         = intern_name("__mul__", 7);
    PYS_INTERN_truediv     = intern_name("__truediv__", 11);
    PYS_INTERN_floordiv    = intern_name("__floordiv__", 12);
    PYS_INTERN_mod         = intern_name("__mod__", 7);
    PYS_INTERN_pow         = intern_name("__pow__", 7);
    PYS_INTERN_or          = intern_name("__or__", 6);
    PYS_INTERN_and         = intern_name("__and__", 7);
    PYS_INTERN_xor         = intern_name("__xor__", 7);
    PYS_INTERN_lshift      = intern_name("__lshift__", 10);
    PYS_INTERN_rshift      = intern_name("__rshift__", 10);
    PYS_INTERN_radd        = intern_name("__radd__", 8);
    PYS_INTERN_rsub        = intern_name("__rsub__", 8);
    PYS_INTERN_rmul        = intern_name("__rmul__", 8);
    PYS_INTERN_iadd        = intern_name("__iadd__", 8);
    PYS_INTERN_isub        = intern_name("__isub__", 8);
    PYS_INTERN_imul        = intern_name("__imul__", 8);
    PYS_INTERN_le          = intern_name("__le__", 6);
    PYS_INTERN_gt          = intern_name("__gt__", 6);
    PYS_INTERN_ge          = intern_name("__ge__", 6);
    PYS_INTERN_ne          = intern_name("__ne__", 6);
}

void
install_builtins(CTX *c)
{
    install_interned_names();
    pys_global_define(c, "print",      pys_make_builtin("print",      bi_print,      0, -1));
    pys_global_define(c, "repr",       pys_make_builtin("repr",       bi_repr,       1,  1));
    pys_global_define(c, "ascii",      pys_make_builtin("ascii",      bi_repr,       1,  1));
    // Built-in type classes — proper class objects whose constructors
    // are the C bi_* functions.  isinstance(5, int), type(5) is int,
    // and class M(int): pass all work via these.
    c->TYPE_int       = pys_make_builtin_class("int",       bi_int,       PYS_T_BIGNUM);
    pys_class_add_method(c, c->TYPE_int, "from_bytes",
        pys_make_builtin("from_bytes", bi_int_from_bytes, 1, 3));
    c->TYPE_float     = pys_make_builtin_class("float",     bi_float,     PYS_T_FLOAT);
    pys_class_add_method(c, c->TYPE_float, "fromhex",
        pys_make_builtin("fromhex", bi_float_fromhex, 1, 1));
    pys_class_add_method(c, c->TYPE_float, "__getformat__",
        pys_make_builtin("__getformat__", bi_float_getformat, 1, 1));
    c->TYPE_complex   = pys_make_builtin_class("complex",   bi_complex,   PYS_T_COMPLEX);
    c->TYPE_bool      = pys_make_builtin_class("bool",      bi_bool,      -1);
    c->TYPE_str       = pys_make_builtin_class("str",       bi_str,       PYS_T_STR);
    pys_class_add_method(c, c->TYPE_str, "maketrans",
        pys_make_builtin("maketrans", bi_str_maketrans, 1, 3));
    c->TYPE_bytes     = pys_make_builtin_class("bytes",     bi_bytes,     PYS_T_BYTES);
    pys_class_add_method(c, c->TYPE_bytes, "fromhex",
        pys_make_builtin("fromhex", bi_bytes_fromhex, 1, 1));
    pys_class_add_method(c, c->TYPE_bytes, "maketrans",
        pys_make_builtin("maketrans", bi_bytes_maketrans, 2, 2));
    c->TYPE_bytearray = pys_make_builtin_class("bytearray", bi_bytearray, PYS_T_BYTEARRAY);
    pys_class_add_method(c, c->TYPE_bytearray, "fromhex",
        pys_make_builtin("fromhex", bi_bytes_fromhex, 1, 1));
    pys_class_add_method(c, c->TYPE_bytearray, "maketrans",
        pys_make_builtin("maketrans", bi_bytes_maketrans, 2, 2));
    c->TYPE_list      = pys_make_builtin_class("list",      bi_list,      PYS_T_LIST);
    // Register list.__init__ on the class so super().__init__(seq) from a
    // user-defined `class S(list)` resolves here (not to object.__init__).
    pys_class_add_method(c, c->TYPE_list, "__init__",
        pys_make_builtin("__init__", lm_init, 1, 2));
    c->TYPE_tuple     = pys_make_builtin_class("tuple",     bi_tuple,     PYS_T_TUPLE);
    c->TYPE_dict      = pys_make_builtin_class("dict",      bi_dict,      PYS_T_DICT);
    {
        // dict.fromkeys is a classmethod in CPython — wrapping it here
        // keeps `t.fromkeys = dict.fromkeys; t.fromkeys(iter)` from
        // re-binding `self=t` and corrupting the call (test_set's
        // dict.fromkeys constructor pattern depends on this).
        VALUE fk = pys_make_builtin("fromkeys", bi_dict_fromkeys, 1, 3);
        struct pysobj *cm = pys_alloc(PYS_T_CLASSMETHOD);
        cm->wrap.wrapped = fk;
        pys_class_add_method(c, c->TYPE_dict, "fromkeys", PYS_OBJ_VAL(cm));
    }
    c->TYPE_set       = pys_make_builtin_class("set",       bi_set,       PYS_T_SET);
    c->TYPE_frozenset = pys_make_builtin_class("frozenset", bi_frozenset, PYS_T_FROZENSET);
    c->TYPE_range     = pys_make_builtin_class("range",     bi_range,     PYS_T_RANGE);
    c->TYPE_type      = pys_make_builtin_class("type",      bi_type,      PYS_T_CLASS);
    {
        // type.__call__(cls, *args) — default construction protocol so
        // metaclass __call__ overrides can delegate via type.__call__(cls, ...).
        extern const char *intern_name(const char *s, size_t len);
        VALUE call = pys_make_builtin("__call__", bi_type_call, 1, -1);
        pys_class_add_method(c, c->TYPE_type, intern_name("__call__", 8), call);
        // type.__new__(mcls, name, bases, dict) — class construction
        // protocol.  CPython metaclass `__new__` typically does
        // `super().__new__(mcls, ...)` which lands here.
        extern VALUE bi_type_new(CTX *c, int argc, VALUE *argv);
        VALUE new_b = pys_make_builtin("__new__", bi_type_new, 1, -1);
        pys_class_add_method(c, c->TYPE_type, intern_name("__new__", 7), new_b);
    }
    c->TYPE_object    = pys_make_class("object", PYS_NONE, false);
    {
        // object.__new__(cls, *args, **kwargs) — default implementation
        // that just allocates a new instance of `cls`.
        extern VALUE bi_object_new(CTX *c, int argc, VALUE *argv);
        pys_class_add_method(c, c->TYPE_object, "__new__",
            pys_make_builtin("__new__", bi_object_new, 1, -1));
        pys_class_add_method(c, c->TYPE_object, "__getattribute__",
            pys_make_builtin("__getattribute__", bi_object_getattribute, 2, 2));
        pys_class_add_method(c, c->TYPE_object, "__setattr__",
            pys_make_builtin("__setattr__", bi_object_setattr, 3, 3));
        pys_class_add_method(c, c->TYPE_object, "__delattr__",
            pys_make_builtin("__delattr__", bi_object_delattr, 2, 2));
        pys_class_add_method(c, c->TYPE_object, "__init__",
            pys_make_builtin("__init__", bi_object_init, 1, -1));
        // object.__init_subclass__ — no-op default so `super().__init_subclass__()`
        // in a __init_subclass__ chain terminates.
        pys_class_add_method(c, c->TYPE_object, "__init_subclass__",
            pys_make_builtin("__init_subclass__", bi_object_init, 1, -1));
    }
    // Synthetic type classes for built-in non-constructable types.
    c->TYPE_NoneType                    = pys_make_class("NoneType",                     PYS_NONE, false);
    c->TYPE_function                    = pys_make_class("function",                     PYS_NONE, false);
    c->TYPE_builtin_function_or_method  = pys_make_class("builtin_function_or_method",   PYS_NONE, false);
    c->TYPE_method                      = pys_make_class("method",                       PYS_NONE, false);
    c->TYPE_module                      = pys_make_class("module",                       PYS_NONE, false);
    c->TYPE_slice                       = pys_make_builtin_class("slice", bi_slice, PYS_T_SLICE);
    c->TYPE_ellipsis                    = pys_make_class("ellipsis",                     PYS_NONE, false);
    c->TYPE_NotImplementedType          = pys_make_class("NotImplementedType",           PYS_NONE, false);
    c->TYPE_memoryview                  = pys_make_class("memoryview",                   PYS_NONE, false);
    c->TYPE_generator                   = pys_make_class("generator",                    PYS_NONE, false);
    c->TYPE_property                    = pys_make_class("property",                     PYS_NONE, false);
    c->TYPE_staticmethod                = pys_make_class("staticmethod",                 PYS_NONE, false);
    c->TYPE_classmethod                 = pys_make_class("classmethod",                  PYS_NONE, false);
    c->TYPE_super                       = pys_make_class("super",                        PYS_NONE, false);
    c->TYPE_cell                        = pys_make_class("cell",                         PYS_NONE, false);
    c->TYPE_traceback                   = pys_make_class("traceback",                    PYS_NONE, false);
    c->TYPE_frame                       = pys_make_class("frame",                        PYS_NONE, false);
    // Wire up base classes so issubclass/isinstance walk MRO properly.
    // bool < int < object; everything else < object.  bytes/bytearray
    // share an ancestor (object) — pystro doesn't model the C-level
    // bytes-like protocol.
    {
        VALUE base_obj[1] = { c->TYPE_object };
        VALUE base_int[1] = { c->TYPE_int };
        pys_class_set_bases(c->TYPE_int, base_obj, 1);
        pys_class_set_bases(c->TYPE_float, base_obj, 1);
        pys_class_set_bases(c->TYPE_complex, base_obj, 1);
        pys_class_set_bases(c->TYPE_bool, base_int, 1);     // bool subclasses int
        pys_class_set_bases(c->TYPE_str, base_obj, 1);
        pys_class_set_bases(c->TYPE_bytes, base_obj, 1);
        pys_class_set_bases(c->TYPE_bytearray, base_obj, 1);
        pys_class_set_bases(c->TYPE_list, base_obj, 1);
        pys_class_set_bases(c->TYPE_tuple, base_obj, 1);
        pys_class_set_bases(c->TYPE_dict, base_obj, 1);
        pys_class_set_bases(c->TYPE_set, base_obj, 1);
        pys_class_set_bases(c->TYPE_frozenset, base_obj, 1);
        pys_class_set_bases(c->TYPE_range, base_obj, 1);
        pys_class_set_bases(c->TYPE_type, base_obj, 1);
        pys_class_set_bases(c->TYPE_NoneType, base_obj, 1);
        pys_class_set_bases(c->TYPE_function, base_obj, 1);
        pys_class_set_bases(c->TYPE_builtin_function_or_method, base_obj, 1);
        pys_class_set_bases(c->TYPE_method, base_obj, 1);
        pys_class_set_bases(c->TYPE_module, base_obj, 1);
        pys_class_set_bases(c->TYPE_slice, base_obj, 1);
        pys_class_set_bases(c->TYPE_ellipsis, base_obj, 1);
        pys_class_set_bases(c->TYPE_NotImplementedType, base_obj, 1);
        pys_class_set_bases(c->TYPE_memoryview, base_obj, 1);
        pys_class_set_bases(c->TYPE_generator, base_obj, 1);
        pys_class_set_bases(c->TYPE_property, base_obj, 1);
        pys_class_set_bases(c->TYPE_staticmethod, base_obj, 1);
        pys_class_set_bases(c->TYPE_classmethod, base_obj, 1);
        pys_class_set_bases(c->TYPE_super, base_obj, 1);
    }
    pys_global_define(c, "int",        c->TYPE_int);
    pys_global_define(c, "float",      c->TYPE_float);
    pys_global_define(c, "complex",    c->TYPE_complex);
    pys_global_define(c, "bool",       c->TYPE_bool);
    pys_global_define(c, "str",        c->TYPE_str);
    pys_global_define(c, "bytes",      c->TYPE_bytes);
    pys_global_define(c, "bytearray",  c->TYPE_bytearray);
    pys_global_define(c, "list",       c->TYPE_list);
    pys_global_define(c, "tuple",      c->TYPE_tuple);
    pys_global_define(c, "dict",       c->TYPE_dict);
    // 3.14 frozendict (used by some CPython tests).  Aliased to dict.
    pys_global_define(c, "frozendict",  c->TYPE_dict);
    pys_global_define(c, "set",        c->TYPE_set);
    pys_global_define(c, "frozenset",  c->TYPE_frozenset);
    pys_global_define(c, "range",      c->TYPE_range);
    pys_global_define(c, "type",       c->TYPE_type);
    pys_global_define(c, "object",     c->TYPE_object);
    // `super` is normally handled at parse-time as a special call form,
    // but plain references (`f = super`, `isinstance(x, super)`) need
    // the class to be reachable by name.  Expose the synthetic class.
    pys_global_define(c, "super",      c->TYPE_super);
    // int/float/complex/bool/str/bytes/bytearray/list/tuple/dict/set/
    // frozenset/range/type are registered as TYPE classes above.
    pys_global_define(c, "len",        pys_make_builtin("len",        bi_len,        1,  1));
    pys_global_define(c, "abs",        pys_make_builtin("abs",        bi_abs,        1,  1));
    pys_global_define(c, "isinstance", pys_make_builtin("isinstance", bi_isinstance, 2,  2));
    pys_global_define(c, "issubclass", pys_make_builtin("issubclass", bi_issubclass, 2,  2));
    pys_global_define(c, "id",         pys_make_builtin("id",         bi_id,         1,  1));
    pys_global_define(c, "dir",        pys_make_builtin("dir",        bi_dir,        0,  1));
    pys_global_define(c, "globals",    pys_make_builtin("globals",    bi_globals,    0,  0));
    pys_global_define(c, "locals",     pys_make_builtin("locals",     bi_locals,     0,  0));
    pys_global_define(c, "vars",       pys_make_builtin("vars",       bi_vars,       0,  1));
    pys_global_define(c, "hasattr",    pys_make_builtin("hasattr",    bi_hasattr,    2,  2));
    pys_global_define(c, "getattr",    pys_make_builtin("getattr",    bi_getattr,    2,  3));
    pys_global_define(c, "setattr",    pys_make_builtin("setattr",    bi_setattr,    3,  3));
    pys_global_define(c, "delattr",    pys_make_builtin("delattr",    bi_delattr,    2,  2));
    pys_global_define(c, "callable",   pys_make_builtin("callable",   bi_callable,   1,  1));
    pys_global_define(c, "open",       pys_make_builtin("open",       bi_open,       1,  2));
    pys_global_define(c, "eval",       pys_make_builtin("eval",       bi_eval,       1,  3));
    pys_global_define(c, "exec",       pys_make_builtin("exec",       bi_exec,       1,  3));
    pys_global_define(c, "min",        pys_make_builtin("min",        bi_min,        1, -1));
    pys_global_define(c, "max",        pys_make_builtin("max",        bi_max,        1, -1));
    pys_global_define(c, "sum",        pys_make_builtin("sum",        bi_sum,        1,  2));
    pys_global_define(c, "sorted",     pys_make_builtin("sorted",     bi_sorted,     1,  1));
    pys_global_define(c, "enumerate",  pys_make_builtin("enumerate",  bi_enumerate,  1,  2));
    pys_global_define(c, "zip",        pys_make_builtin("zip",        bi_zip,        0, -1));
    pys_global_define(c, "chr",        pys_make_builtin("chr",        bi_chr,        1,  1));
    pys_global_define(c, "ord",        pys_make_builtin("ord",        bi_ord,        1,  1));
    pys_global_define(c, "hex",        pys_make_builtin("hex",        bi_hex,        1,  1));
    pys_global_define(c, "bin",        pys_make_builtin("bin",        bi_bin,        1,  1));
    pys_global_define(c, "oct",        pys_make_builtin("oct",        bi_oct,        1,  1));
    pys_global_define(c, "slice",      c->TYPE_slice);
    pys_global_define(c, "memoryview", pys_make_builtin("memoryview", bi_memoryview, 1,  1));
    pys_global_define(c, "breakpoint", pys_make_builtin("breakpoint", bi_breakpoint, 0, -1));
    pys_global_define(c, "compile",    pys_make_builtin("compile",    bi_compile,    3,  6));
    {
        struct pysobj *e = pys_alloc(PYS_T_ELLIPSIS);
        pys_global_define(c, "Ellipsis",       PYS_OBJ_VAL(e));
        struct pysobj *n = pys_alloc(PYS_T_NOTIMPL);
        pys_global_define(c, "NotImplemented", PYS_OBJ_VAL(n));
    }
    pys_global_define(c, "input",      pys_make_builtin("input",      bi_input,      0,  1));
    pys_global_define(c, "hash",       pys_make_builtin("hash",       bi_hash,       1,  1));
    pys_global_define(c, "format",     pys_make_builtin("format",     bi_format,     1,  2));
    pys_global_define(c, "staticmethod",pys_make_builtin("staticmethod",bi_staticmethod, 1, 1));
    pys_global_define(c, "classmethod", pys_make_builtin("classmethod", bi_classmethod, 1, 1));
    pys_global_define(c, "property",    pys_make_builtin("property",    bi_property,    0, 4));
    pys_global_define(c, "all",         pys_make_builtin("all",         bi_all,        1, 1));
    pys_global_define(c, "any",         pys_make_builtin("any",         bi_any,        1, 1));
    pys_global_define(c, "divmod",      pys_make_builtin("divmod",      bi_divmod,     2, 2));
    pys_global_define(c, "round",       pys_make_builtin("round",       bi_round,      1, 2));
    pys_global_define(c, "pow",         pys_make_builtin("pow",         bi_pow,        2, 3));
    pys_global_define(c, "reversed",    pys_make_builtin("reversed",    bi_reversed,   1, 1));
    pys_global_define(c, "map",         pys_make_builtin("map",         bi_map,        2,-1));
    pys_global_define(c, "filter",      pys_make_builtin("filter",      bi_filter,     2, 2));
    pys_global_define(c, "iter",        pys_make_builtin("iter",        bi_iter,       1, 2));
    pys_global_define(c, "next",        pys_make_builtin("next",        bi_next,       1, 2));

    // Synthetic Exception.__init__: sets self.args and self.message.
    // Lives as a builtin function on the class so user-defined
    // exception subclasses can `super().__init__(msg)`.
    extern VALUE bi_exception_init(CTX *c, int argc, VALUE *argv);
    // Built-in exception classes.  Hierarchy is BaseException root, but
    // for now everything inherits Exception → no base.
    c->EXC_BaseException    = pys_make_class("BaseException",    PYS_NONE, true);
    {
        VALUE init = pys_make_builtin("__init__", bi_exception_init, 1, -1);
        pys_class_add_method(c, c->EXC_BaseException, "__init__", init);
        // exc.with_traceback(tb) — sets self.__traceback__ and returns self.
        extern VALUE bi_exception_with_traceback(CTX *c, int argc, VALUE *argv);
        pys_class_add_method(c, c->EXC_BaseException, "with_traceback",
            pys_make_builtin("with_traceback", bi_exception_with_traceback, 2, 2));
        // exc.add_note(s) — append to self.__notes__ list (CPython 3.11+).
        extern VALUE bi_exception_add_note(CTX *c, int argc, VALUE *argv);
        pys_class_add_method(c, c->EXC_BaseException, "add_note",
            pys_make_builtin("add_note", bi_exception_add_note, 2, 2));
    }
    c->EXC_Exception         = pys_make_class("Exception",        c->EXC_BaseException, true);
    c->EXC_SystemExit        = pys_make_class("SystemExit",        c->EXC_BaseException, true);
    c->EXC_KeyboardInterrupt = pys_make_class("KeyboardInterrupt", c->EXC_BaseException, true);
    c->EXC_GeneratorExit     = pys_make_class("GeneratorExit",     c->EXC_BaseException, true);
    c->EXC_TypeError        = pys_make_class("TypeError",        c->EXC_Exception, true);
    c->EXC_ValueError       = pys_make_class("ValueError",       c->EXC_Exception, true);
    c->EXC_NameError        = pys_make_class("NameError",        c->EXC_Exception, true);
    c->EXC_UnboundLocalError = pys_make_class("UnboundLocalError", c->EXC_NameError, true);
    c->EXC_SystemError       = pys_make_class("SystemError",      c->EXC_Exception, true);
    c->EXC_Warning           = pys_make_class("Warning",           c->EXC_Exception, true);
    c->EXC_DeprecationWarning = pys_make_class("DeprecationWarning", c->EXC_Warning, true);
    c->EXC_PendingDeprecationWarning = pys_make_class("PendingDeprecationWarning", c->EXC_Warning, true);
    c->EXC_UserWarning       = pys_make_class("UserWarning",       c->EXC_Warning, true);
    c->EXC_FutureWarning     = pys_make_class("FutureWarning",     c->EXC_Warning, true);
    c->EXC_RuntimeWarning    = pys_make_class("RuntimeWarning",    c->EXC_Warning, true);
    c->EXC_SyntaxWarning     = pys_make_class("SyntaxWarning",     c->EXC_Warning, true);
    c->EXC_ImportWarning     = pys_make_class("ImportWarning",     c->EXC_Warning, true);
    c->EXC_UnicodeWarning    = pys_make_class("UnicodeWarning",    c->EXC_Warning, true);
    c->EXC_BytesWarning      = pys_make_class("BytesWarning",      c->EXC_Warning, true);
    c->EXC_ResourceWarning   = pys_make_class("ResourceWarning",   c->EXC_Warning, true);
    c->EXC_BaseExceptionGroup = pys_make_class("BaseExceptionGroup", c->EXC_BaseException, true);
    c->EXC_ExceptionGroup    = pys_make_class("ExceptionGroup",   c->EXC_BaseExceptionGroup, true);
    c->EXC_LookupError       = pys_make_class("LookupError",       c->EXC_Exception, true);
    c->EXC_IndexError       = pys_make_class("IndexError",       c->EXC_LookupError, true);
    c->EXC_KeyError         = pys_make_class("KeyError",         c->EXC_LookupError, true);
    c->EXC_ArithmeticError   = pys_make_class("ArithmeticError",  c->EXC_Exception, true);
    c->EXC_ZeroDivisionError= pys_make_class("ZeroDivisionError",c->EXC_ArithmeticError, true);
    c->EXC_OverflowError     = pys_make_class("OverflowError",    c->EXC_ArithmeticError, true);
    c->EXC_FloatingPointError= pys_make_class("FloatingPointError",c->EXC_ArithmeticError, true);
    c->EXC_AttributeError   = pys_make_class("AttributeError",   c->EXC_Exception, true);
    c->EXC_RuntimeError     = pys_make_class("RuntimeError",     c->EXC_Exception, true);
    c->EXC_NotImplementedError= pys_make_class("NotImplementedError", c->EXC_RuntimeError, true);
    c->EXC_RecursionError    = pys_make_class("RecursionError",   c->EXC_RuntimeError, true);
    c->EXC_StopIteration    = pys_make_class("StopIteration",    c->EXC_Exception, true);
    c->EXC_StopAsyncIteration= pys_make_class("StopAsyncIteration",c->EXC_Exception, true);
    c->EXC_AssertionError   = pys_make_class("AssertionError",   c->EXC_Exception, true);
    c->EXC_ImportError       = pys_make_class("ImportError",       c->EXC_Exception, true);
    c->EXC_ModuleNotFoundError= pys_make_class("ModuleNotFoundError", c->EXC_ImportError, true);
    c->EXC_OSError           = pys_make_class("OSError",          c->EXC_Exception, true);
    c->EXC_FileNotFoundError = pys_make_class("FileNotFoundError",c->EXC_OSError, true);
    c->EXC_FileExistsError   = pys_make_class("FileExistsError",  c->EXC_OSError, true);
    c->EXC_PermissionError   = pys_make_class("PermissionError",  c->EXC_OSError, true);
    c->EXC_NotADirectoryError= pys_make_class("NotADirectoryError",c->EXC_OSError, true);
    c->EXC_IsADirectoryError = pys_make_class("IsADirectoryError",c->EXC_OSError, true);
    c->EXC_TimeoutError      = pys_make_class("TimeoutError",     c->EXC_OSError, true);
    c->EXC_BrokenPipeError   = pys_make_class("BrokenPipeError",  c->EXC_OSError, true);
    c->EXC_InterruptedError  = pys_make_class("InterruptedError", c->EXC_OSError, true);
    c->EXC_ProcessLookupError= pys_make_class("ProcessLookupError",c->EXC_OSError, true);
    c->EXC_ConnectionError   = pys_make_class("ConnectionError",  c->EXC_OSError, true);
    c->EXC_ConnectionAbortedError = pys_make_class("ConnectionAbortedError", c->EXC_ConnectionError, true);
    c->EXC_ConnectionRefusedError = pys_make_class("ConnectionRefusedError", c->EXC_ConnectionError, true);
    c->EXC_ConnectionResetError   = pys_make_class("ConnectionResetError",   c->EXC_ConnectionError, true);
    c->EXC_BlockingIOError   = pys_make_class("BlockingIOError",  c->EXC_OSError, true);
    c->EXC_ChildProcessError = pys_make_class("ChildProcessError",c->EXC_OSError, true);
    c->EXC_UnicodeError      = pys_make_class("UnicodeError",     c->EXC_ValueError, true);
    c->EXC_UnicodeDecodeError= pys_make_class("UnicodeDecodeError",c->EXC_UnicodeError, true);
    c->EXC_UnicodeEncodeError= pys_make_class("UnicodeEncodeError",c->EXC_UnicodeError, true);
    c->EXC_MemoryError       = pys_make_class("MemoryError",      c->EXC_Exception, true);
    c->EXC_BufferError       = pys_make_class("BufferError",      c->EXC_Exception, true);
    c->EXC_ReferenceError    = pys_make_class("ReferenceError",   c->EXC_Exception, true);
    c->EXC_SyntaxError       = pys_make_class("SyntaxError",      c->EXC_Exception, true);
    c->EXC_IndentationError  = pys_make_class("IndentationError", c->EXC_SyntaxError, true);
    c->EXC_TabError          = pys_make_class("TabError",         c->EXC_IndentationError, true);
    c->EXC_EOFError          = pys_make_class("EOFError",         c->EXC_Exception, true);

    pys_global_define(c, "Exception",        c->EXC_Exception);
    pys_global_define(c, "TypeError",        c->EXC_TypeError);
    pys_global_define(c, "ValueError",       c->EXC_ValueError);
    pys_global_define(c, "NameError",        c->EXC_NameError);
    pys_global_define(c, "IndexError",       c->EXC_IndexError);
    pys_global_define(c, "KeyError",         c->EXC_KeyError);
    pys_global_define(c, "ZeroDivisionError",c->EXC_ZeroDivisionError);
    pys_global_define(c, "AttributeError",   c->EXC_AttributeError);
    pys_global_define(c, "RuntimeError",     c->EXC_RuntimeError);
    pys_global_define(c, "StopIteration",    c->EXC_StopIteration);
    pys_global_define(c, "AssertionError",   c->EXC_AssertionError);
    pys_global_define(c, "ImportError",          c->EXC_ImportError);
    pys_global_define(c, "ModuleNotFoundError",  c->EXC_ModuleNotFoundError);
    pys_global_define(c, "NotImplementedError",  c->EXC_NotImplementedError);
    pys_global_define(c, "ArithmeticError",      c->EXC_ArithmeticError);
    pys_global_define(c, "OverflowError",        c->EXC_OverflowError);
    pys_global_define(c, "OSError",              c->EXC_OSError);
    pys_global_define(c, "FileNotFoundError",    c->EXC_FileNotFoundError);
    pys_global_define(c, "FileExistsError",      c->EXC_FileExistsError);
    pys_global_define(c, "ProcessLookupError",   c->EXC_ProcessLookupError);
    pys_global_define(c, "ConnectionAbortedError", c->EXC_ConnectionAbortedError);
    pys_global_define(c, "ConnectionRefusedError", c->EXC_ConnectionRefusedError);
    pys_global_define(c, "ConnectionResetError",   c->EXC_ConnectionResetError);
    pys_global_define(c, "IOError",              c->EXC_OSError);    // alias
    pys_global_define(c, "EnvironmentError",     c->EXC_OSError);    // alias
    pys_global_define(c, "BaseException",        c->EXC_BaseException);
    pys_global_define(c, "SystemExit",           c->EXC_SystemExit);
    pys_global_define(c, "KeyboardInterrupt",    c->EXC_KeyboardInterrupt);
    pys_global_define(c, "GeneratorExit",        c->EXC_GeneratorExit);
    pys_global_define(c, "LookupError",          c->EXC_LookupError);
    pys_global_define(c, "FloatingPointError",   c->EXC_FloatingPointError);
    pys_global_define(c, "RecursionError",       c->EXC_RecursionError);
    pys_global_define(c, "StopAsyncIteration",   c->EXC_StopAsyncIteration);
    pys_global_define(c, "PermissionError",      c->EXC_PermissionError);
    pys_global_define(c, "NotADirectoryError",   c->EXC_NotADirectoryError);
    pys_global_define(c, "IsADirectoryError",    c->EXC_IsADirectoryError);
    pys_global_define(c, "TimeoutError",         c->EXC_TimeoutError);
    pys_global_define(c, "BrokenPipeError",      c->EXC_BrokenPipeError);
    pys_global_define(c, "InterruptedError",     c->EXC_InterruptedError);
    pys_global_define(c, "ConnectionError",      c->EXC_ConnectionError);
    pys_global_define(c, "BlockingIOError",      c->EXC_BlockingIOError);
    pys_global_define(c, "ChildProcessError",    c->EXC_ChildProcessError);
    pys_global_define(c, "UnicodeError",         c->EXC_UnicodeError);
    pys_global_define(c, "UnicodeDecodeError",   c->EXC_UnicodeDecodeError);
    pys_global_define(c, "UnicodeEncodeError",   c->EXC_UnicodeEncodeError);
    pys_global_define(c, "MemoryError",          c->EXC_MemoryError);
    pys_global_define(c, "BufferError",          c->EXC_BufferError);
    pys_global_define(c, "ReferenceError",       c->EXC_ReferenceError);
    pys_global_define(c, "SyntaxError",          c->EXC_SyntaxError);
    pys_global_define(c, "IndentationError",     c->EXC_IndentationError);
    pys_global_define(c, "TabError",             c->EXC_TabError);
    pys_global_define(c, "EOFError",             c->EXC_EOFError);
    pys_global_define(c, "UnboundLocalError",    c->EXC_UnboundLocalError);
    pys_global_define(c, "SystemError",          c->EXC_SystemError);
    pys_global_define(c, "Warning",              c->EXC_Warning);
    pys_global_define(c, "DeprecationWarning",   c->EXC_DeprecationWarning);
    pys_global_define(c, "PendingDeprecationWarning", c->EXC_PendingDeprecationWarning);
    pys_global_define(c, "UserWarning",          c->EXC_UserWarning);
    pys_global_define(c, "FutureWarning",        c->EXC_FutureWarning);
    pys_global_define(c, "RuntimeWarning",       c->EXC_RuntimeWarning);
    pys_global_define(c, "SyntaxWarning",        c->EXC_SyntaxWarning);
    pys_global_define(c, "ImportWarning",        c->EXC_ImportWarning);
    pys_global_define(c, "UnicodeWarning",       c->EXC_UnicodeWarning);
    pys_global_define(c, "BytesWarning",         c->EXC_BytesWarning);
    pys_global_define(c, "ResourceWarning",      c->EXC_ResourceWarning);
    pys_global_define(c, "BaseExceptionGroup",   c->EXC_BaseExceptionGroup);
    pys_global_define(c, "ExceptionGroup",       c->EXC_ExceptionGroup);
    pys_global_define(c, "__pystro_del__",   pys_make_builtin("__pystro_del__", bi_pystro_del, 2, 2));
    pys_global_define(c, "__pystro_yield_from__", pys_make_builtin("__pystro_yield_from__", bi_pystro_yield_from, 1, 1));
    pys_global_define(c, "__pystro_pos__", pys_make_builtin("__pystro_pos__", bi_pystro_pos, 1, 1));
    pys_global_define(c, "__pystro_delattr__",   pys_make_builtin("__pystro_delattr__", bi_pystro_delattr, 2, 2));
    pys_global_define(c, "__pystro_delglobal__", pys_make_builtin("__pystro_delglobal__", bi_pystro_delglobal, 1, 1));
    pys_global_define(c, "__pystro_import__",    pys_make_builtin("__pystro_import__", bi_import, 1, 1));
    pys_global_define(c, "__import__",           pys_make_builtin("__import__", bi_import, 1, 5));
    pys_global_define(c, "__pystro_try_import__",
        pys_make_builtin("__pystro_try_import__", bi_try_import, 1, 1));
    pys_global_define(c, "__pystro_modules__",
        pys_make_builtin("__pystro_modules__", bi_modules, 0, 0));
    pys_global_define(c, "__name__",             pys_make_str("__main__", 8));
    // CPython 3.x exposes __debug__ unconditionally as True (False only
    // when started with `-O`).  Several stdlib modules (incl. test/.) read
    // it module-level; absence triggers NameError at parse-time eval.
    pys_global_define(c, "__debug__",            PYS_TRUE);
    pys_global_define(c, "__pystro_import_star__", pys_make_builtin("__pystro_import_star__", bi_import_star, 1, 1));
    // C-level math primitives, surfaced through the `math` module (math.py).
    pys_global_define(c, "__pystro_sqrt__",  pys_make_builtin("__pystro_sqrt__",  bi_pystro_sqrt,  1, 1));
    pys_global_define(c, "__pystro_sin__",   pys_make_builtin("__pystro_sin__",   bi_pystro_sin,   1, 1));
    pys_global_define(c, "__pystro_cos__",   pys_make_builtin("__pystro_cos__",   bi_pystro_cos,   1, 1));
    pys_global_define(c, "__pystro_tan__",   pys_make_builtin("__pystro_tan__",   bi_pystro_tan,   1, 1));
    pys_global_define(c, "__pystro_log__",   pys_make_builtin("__pystro_log__",   bi_pystro_log,   1, 2));
    pys_global_define(c, "__pystro_exp__",   pys_make_builtin("__pystro_exp__",   bi_pystro_exp,   1, 1));
    pys_global_define(c, "__pystro_floor__", pys_make_builtin("__pystro_floor__", bi_pystro_floor, 1, 1));
    pys_global_define(c, "__pystro_ceil__",  pys_make_builtin("__pystro_ceil__",  bi_pystro_ceil,  1, 1));
    pys_global_define(c, "__pystro_atan2__", pys_make_builtin("__pystro_atan2__", bi_pystro_atan2, 2, 2));
    pys_global_define(c, "__pystro_pow__",   pys_make_builtin("__pystro_pow__",   bi_pystro_pow,   2, 2));
    pys_global_define(c, "__pystro_argv__",  pys_make_builtin("__pystro_argv__",  bi_pystro_argv,  0, 0));
    pys_global_define(c, "__pystro_exit__",  pys_make_builtin("__pystro_exit__",  bi_pystro_exit,  0, 1));
    pys_global_define(c, "__pystro_current_exc__",
                     pys_make_builtin("__pystro_current_exc__", bi_pystro_current_exc, 0, 0));
    pys_global_define(c, "__pystro_get_recursion_limit__",
                     pys_make_builtin("__pystro_get_recursion_limit__",
                                     bi_pystro_get_recursion_limit, 0, 0));
    pys_global_define(c, "__pystro_set_recursion_limit__",
                     pys_make_builtin("__pystro_set_recursion_limit__",
                                     bi_pystro_set_recursion_limit, 1, 1));
    pys_global_define(c, "__pystro_localtime__",
                     pys_make_builtin("__pystro_localtime__", bi_pystro_localtime, 0, 1));
    pys_global_define(c, "__pystro_gmtime__",
                     pys_make_builtin("__pystro_gmtime__", bi_pystro_gmtime, 0, 1));
    pys_global_define(c, "__pystro_strftime__",
                     pys_make_builtin("__pystro_strftime__", bi_pystro_strftime, 1, 2));
    pys_global_define(c, "__pystro_float_to_bits__",
                     pys_make_builtin("__pystro_float_to_bits__", bi_pystro_float_to_bits, 1, 2));
    pys_global_define(c, "__pystro_bits_to_float__",
                     pys_make_builtin("__pystro_bits_to_float__", bi_pystro_bits_to_float, 1, 2));
    pys_global_define(c, "__pystro_time__",      pys_make_builtin("__pystro_time__",      bi_pystro_time,      0, 0));
    pys_global_define(c, "__pystro_sleep__",     pys_make_builtin("__pystro_sleep__",     bi_pystro_sleep,     1, 1));
    pys_global_define(c, "__pystro_perf_counter__", pys_make_builtin("__pystro_perf_counter__", bi_pystro_perf_counter, 0, 0));
    pys_global_define(c, "__pystro_getenv__",    pys_make_builtin("__pystro_getenv__",    bi_pystro_getenv,    1, 2));
    pys_global_define(c, "__pystro_getcwd__",    pys_make_builtin("__pystro_getcwd__",    bi_pystro_getcwd,    0, 0));
    pys_global_define(c, "__pystro_path_exists__", pys_make_builtin("__pystro_path_exists__", bi_pystro_path_exists, 1, 1));
    pys_global_define(c, "__pystro_md5__",     pys_make_builtin("__pystro_md5__",     bi_pystro_md5,     1, 1));
    pys_global_define(c, "__pystro_sha256__",  pys_make_builtin("__pystro_sha256__",  bi_pystro_sha256,  1, 1));
    pys_global_define(c, "__pystro_listdir__",     pys_make_builtin("__pystro_listdir__",     bi_pystro_listdir,     0, 1));
    pys_global_define(c, "__pystro_remove__",      pys_make_builtin("__pystro_remove__",      bi_pystro_remove,      1, 1));
    pys_global_define(c, "__pystro_rmdir__",       pys_make_builtin("__pystro_rmdir__",       bi_pystro_rmdir,       1, 1));
    pys_global_define(c, "__pystro_makedirs__",    pys_make_builtin("__pystro_makedirs__",    bi_pystro_makedirs,    1, 2));
    pys_global_define(c, "__pystro_isdir__",       pys_make_builtin("__pystro_isdir__",       bi_pystro_isdir,       1, 1));
    pys_global_define(c, "__pystro_isfile__",      pys_make_builtin("__pystro_isfile__",      bi_pystro_isfile,      1, 1));
    pys_global_define(c, "__pystro_stat__",        pys_make_builtin("__pystro_stat__",        bi_pystro_stat,        1, 1));
    pys_global_define(c, "__pystro_abspath__",     pys_make_builtin("__pystro_abspath__",     bi_pystro_abspath,     1, 1));
    pys_global_define(c, "__pystro_gc_collect__",  pys_make_builtin("__pystro_gc_collect__",  bi_pystro_gc_collect,  0, 0));

    c->current_class = PYS_NONE;
}
