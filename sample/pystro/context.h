#ifndef PYSTRO_CONTEXT_H
#define PYSTRO_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <gmp.h>
#include <ucontext.h>

// Boehm-Demers-Weiser conservative GC.
extern void *GC_malloc(size_t);
extern void *GC_malloc_atomic(size_t);
extern void *GC_realloc(void *, size_t);
extern void  GC_free(void *);
extern void  GC_init(void);
extern void  GC_gcollect(void);
extern int   GC_expand_hp(size_t);
extern void  GC_set_free_space_divisor(unsigned);

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

// VALUE encoding (CRuby / ascheme / luastro family):
//   xxxx_xxx1 → fixnum (signed 63-bit, shift-left + OR 1)
//   xxxx_xx10 → flonum (IEEE-754 double encoded inline; 3-bit rotate)
//   xxxx_x000 → ptr to `struct pyobj` (8-byte aligned)
//
// Flonum encoding: doubles whose IEEE-754 exponent top-3-bits ∈ {0b011,
// 0b100} (magnitudes ~[1e-77, 1e+77]) round-trip through a left-rotate-
// by-3 + bit-1 tag.  Everything else (0.0, denormals, NaN/inf, very
// large/small) falls back to the heap PY_T_FLOAT path.  This eliminates
// per-arithmetic-op heap allocation for the typical numeric workload.
//
// True / False / None are static singleton pyobj's; their addresses are
// the literal VALUE constants PY_TRUE / PY_FALSE / PY_NONE.
typedef int64_t VALUE;

#define PY_FIXNUM_MAX  ((int64_t)((1LL << 62) - 1))
#define PY_FIXNUM_MIN  ((int64_t)(-(1LL << 62)))
#define PY_IS_FIXNUM(v) ((int64_t)(v) & 1LL)
#define PY_FIX(n)       (((VALUE)(int64_t)(n) << 1) | 1LL)
#define PY_FIXVAL(v)    ((int64_t)(v) >> 1)

#define PY_FLONUM_MASK   3LL
#define PY_FLONUM_TAG    2LL
#define PY_IS_FLONUM(v)  (((int64_t)(v) & PY_FLONUM_MASK) == PY_FLONUM_TAG)

#define PY_IS_PTR(v)    (((int64_t)(v) & PY_FLONUM_MASK) == 0)
#define PY_PTR(v)       ((struct pyobj *)(uintptr_t)(v))
#define PY_OBJ_VAL(p)   ((VALUE)(uintptr_t)(p))

static inline uint64_t py_rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }
static inline uint64_t py_rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

// Try to inline-encode `d`.  Returns 0 if `d` falls outside the encodable
// range (caller must heap-box).
static inline VALUE
py_try_flonum(double d)
{
    union { double d; uint64_t u; } pun;
    pun.d = d;
    int bits = (int)((pun.u >> 60) & 0x7);
    if (__builtin_expect(d == 0.0 || (bits != 3 && bits != 4), 0)) return 0;
    return (VALUE)((py_rotl64(pun.u, 3) & ~(uint64_t)1) | PY_FLONUM_TAG);
}

static inline double
py_flonum_to_double(VALUE v)
{
    union { double d; uint64_t u; } pun;
    uint64_t b63 = ((uint64_t)v >> 63) & 1;
    pun.u = py_rotr64((2 - b63) | ((uint64_t)v & ~(uint64_t)3), 3);
    return pun.d;
}

enum pyobj_type {
    PY_T_NONE = 0,
    PY_T_BOOL,
    PY_T_FLOAT,
    PY_T_BIGNUM,            // GMP mpz
    PY_T_COMPLEX,           // complex (real + imag double pair)
    PY_T_STR,
    PY_T_BYTES,             // immutable byte sequence (b"...")
    PY_T_BYTEARRAY,         // mutable byte sequence
    PY_T_MODULE,            // imported module (has own globals)

    PY_T_LIST,
    PY_T_TUPLE,
    PY_T_DICT,
    PY_T_SET,
    PY_T_FROZENSET,         // immutable + hashable variant of SET
    PY_T_RANGE,
    PY_T_FUNC,
    PY_T_BUILTIN,
    PY_T_BOUND_METHOD,
    PY_T_CLASS,
    PY_T_INSTANCE,
    PY_T_STATICMETHOD,    // wraps a func; bypasses self binding
    PY_T_CLASSMETHOD,     // wraps a func; binds the class instead of self
    PY_T_PROPERTY,        // wraps a getter func; called on attribute read
    PY_T_ITER,            // built-in iterator wrapper around a struct py_iter
    PY_T_GEN,             // generator (ucontext-backed lazy yield)
    PY_T_FILE,            // text/binary file object (FILE* under the hood)
    PY_T_SUPER,           // bound super proxy: (start_class, self)
    PY_T_SLICE,           // slice(start, stop, step)
    PY_T_ELLIPSIS,        // ...
    PY_T_NOTIMPL,         // NotImplemented sentinel
    PY_T_MEMVIEW,         // memoryview wrapping a bytes/bytearray
};

struct pyobj;
struct pyframe;
struct pyclass;
struct pydict;
struct pygen;
struct CTX_struct;
struct Node;

typedef VALUE (*py_builtin_fn)(struct CTX_struct *c, int argc, VALUE *argv);

// Class definition: name + method table + base class.  Methods are
// stored as a small array of (name, value) — fine until classes start
// having dozens of methods, at which point upgrading to a hash table
// is straightforward.  `is_exception` distinguishes user/builtin
// exception classes for `except` matching.
struct pyclass_method {
    const char *name;
    VALUE       value;
};
struct pyclass {
    const char *name;
    struct pyclass_method *methods;
    int  nmethods, methods_capa;
    bool is_exception;
    VALUE base;             // first base, or PY_NONE — kept for super()
    VALUE *bases;           // direct bases (length nbases)
    int   nbases;
    VALUE *mro;             // C3-linearised MRO including self at [0]
    int   nmro;
    // For built-in type classes (int / str / list / ...), `builtin_ctor`
    // is the C constructor function that py_apply calls when this class
    // is invoked.  NULL for user classes (which use __init__).
    py_builtin_fn builtin_ctor;
    // The PY_T_* tag of values produced by this class.  -1 for user classes.
    int builtin_tag;
};

// CPython-style "compact" dict.  Indices table is open-addressed and
// holds dense indices into the entries array; entries[] is appended
// to in insertion order, so `for k in d` simply walks entries[].
//
//   indices[0..icapa)  : int32_t bucket → entry index (DICT_EMPTY=-1, DICT_TOMB=-2)
//   entries[0..elen)   : (key, value, hash) — dense, append-only on new keys.
//                        On delete we mark indices[bucket]=DICT_TOMB and zero out
//                        entries[i].hash → 0 with DICT_DELETED_KEY sentinel as key.
struct pydict_entry {
    VALUE     key;          // 0 or DICT_DELETED_KEY = deleted slot
    VALUE     value;
    uint64_t  hash;
};
#define DICT_EMPTY_IDX  ((int32_t)-1)
#define DICT_TOMB_IDX   ((int32_t)-2)
#define DICT_DELETED_KEY ((VALUE)0xDEADBEEF1)   // never produced by py_make_*
struct pydict {
    int32_t  *indices;      // open-addressed bucket → entries index
    struct pydict_entry *entries;
    size_t   icapa;         // power of 2; #buckets in indices[]
    size_t   elen;          // # of slots in entries[] (incl. deleted)
    size_t   ecapa;
    size_t   used;          // live entries (= elen - deleted)
    size_t   fill;          // indices[] used (= used + tombstones)
};

struct pyobj {
    int type;
    union {
        bool b;
        double dbl;
        mpz_t mpz;
        struct { char *chars; size_t len; } str;
        struct { VALUE *items; size_t len; size_t capa; } list;        // also tuple
        struct pydict *dict;
        struct { int64_t start, stop, step; } range;
        struct {
            struct Node *body;
            struct pyframe *env;
            const char *name;
            int nparams;            // total slots: pos-or-kw + *args + kwonly + **kw
            int n_pos_named;        // # of pos-or-kw params (before any *args)
            VALUE *defaults;        // length nparams; (VALUE)0 ⇒ "required" sentinel
            const char **param_names;
            int nlocals;
            bool leaf;
            bool has_varargs;       // a `*args` slot is present
            bool has_kwargs;        // a `**kwargs` slot is present
            bool is_generator;      // body contains `yield` — call returns
                                    // a PY_T_GEN, body runs lazily
            VALUE defining_class;   // class this method was defined on
                                    // (PY_NONE for non-method funcs) —
                                    // used for cooperative super()
            struct pyglobals *fglobals;  // captured globals at def time
            struct pydict *attrs;   // user-set attributes (`f.x = 5`); lazy
        } func;
        // PY_T_MODULE: name + its globals.
        struct {
            const char *name;
            struct pyglobals *globals;
        } module;
        struct {
            py_builtin_fn fn;
            const char *name;
            int min_argc, max_argc;
        } builtin;
        struct {
            VALUE self;
            VALUE func;             // a func or builtin
        } bound;
        // staticmethod / classmethod / property wrap a single func.
        // For PY_T_PROPERTY: `wrapped` is the getter; `setter` is the optional setter.
        struct { VALUE wrapped; VALUE setter; VALUE deleter; } wrap;
        // PY_T_ITER: holds a `struct py_iter` for stateful iteration.
        struct py_iter *iter_state;
        // PY_T_GEN: lazy generator (ucontext + body func + saved state).
        struct pygen *gen;
        // PY_T_FILE: a libc FILE* wrapper.
        struct {
            void *fp;          // FILE *
            char *path;        // duplicated for repr/__name
            bool  binary;
            bool  closed;
        } file;
        // PY_T_COMPLEX: real + imag pair (CPython same).
        struct { double re; double im; } cpx;
        // PY_T_SUPER: bound super proxy.  start_cls = the class to walk
        // FROM (exclusive); self = the bound instance.
        struct { VALUE start_cls; VALUE self; } super_;
        // PY_T_SLICE: slice(start, stop, step).  Each VALUE may be PY_NONE.
        struct { VALUE start; VALUE stop; VALUE step; } slice_;
        // PY_T_MEMVIEW: borrowed view over bytes/bytearray.
        struct { VALUE source; size_t off; size_t len; } memview;
        struct pyclass cls;
        struct {
            struct pyobj *cls;
            struct pydict *attrs;
            // For instances of a class that subclasses a built-in type
            // (e.g. `class M(list)`), `primary` holds the underlying
            // built-in value (list/dict/str/etc.).  Method dispatch on
            // builtin-inherited methods passes `primary` as self.
            VALUE primary;
        } inst;
    };
};

struct pyframe {
    struct pyframe *parent;
    int nslots;
    VALUE slots[];
};

enum py_state {
    PY_STATE_NORMAL = 0,
    PY_STATE_RETURN,
    PY_STATE_RAISE,
    PY_STATE_BREAK,
    PY_STATE_CONTINUE,
};

struct pystro_option {
    bool quiet;
    bool dump_ast;
    bool no_compiled_code;
    bool compile_first;
    bool aot_only;
    bool record_all;
};
extern struct pystro_option OPTION;

struct gentry {
    const char *name;
    VALUE value;
    bool defined;
};

// Globals namespace (module-level).  Each module has its own
// `pyglobals`; functions capture a pointer to the one in scope at
// def-time, so cross-module calls see the right names.
struct pyglobals {
    struct gentry *entries;
    size_t size, capa;
    uint64_t serial;
};

// Inline cache stamped at every node_gref / node_gset call site.  The
// cache holds (serial, value): hot path is two 8-byte loads + a compare.
// Mutated by `py_global_set` / `py_global_define` via globals_serial bump.
struct gref_cache {
    uint64_t serial;
    int      idx;          // index into c->globals (-1 if not yet resolved)
};

// Inline cache for `o.method(args)`.  Hot path: if `recv` has the same
// type-tag as `type_tag`, call `fn` directly with `recv` prepended to
// argv.  This skips both the bound-method heap allocation and the
// strcmp scan through the per-type method registry.  Cache miss falls
// through to `py_getattr` + `py_apply`.
struct method_cache {
    int   type_tag;        // PY_T_xxx; -1 ⇒ uninitialised
    void *fn;              // py_builtin_fn (or NULL for slow-only)
};

// Inline cache for `o.attr` on instances.  Stamped per node_attr_get
// site.  Hot path: same class as last time → look at the cached entry
// index in inst.attrs; if entries[eidx].key matches by pointer, return
// entries[eidx].value.  Cache miss falls through to py_getattr.
struct attr_cache {
    void *cls_ptr;          // PY_PTR(class) of last seen instance, or NULL
    int32_t eidx;           // index into inst.attrs->entries[]
    uint64_t attrs_id;      // identity of the attrs dict (pointer cast to int)
                             // — invalidated when the class's method table changes
};

typedef struct CTX_struct {
    struct pyframe *env;

    // Currently-active globals (= the running module's namespace).
    // Switched on cross-module function call.
    struct pyglobals *globals;

    int    state;
    VALUE  state_value;             // return value / raised exception
    VALUE  current_handling_exc;    // exception being handled (for __context__ chaining)
    int    raise_line;              // source line of the raise (best-effort)

    // While executing a `class C:` body, `current_class` holds the
    // class object so nested `def` ALLOC nodes register methods there
    // instead of globals.  PY_NONE outside class-body scope.
    VALUE  current_class;

    // While executing a method body, `method_class` is the class on
    // which the method was lexically defined — read by node_super
    // for cooperative MRO walking.  Saved/restored across nested
    // method calls.
    VALUE  method_class;

    jmp_buf err_jmp;
    int     err_jmp_active;

    // Stack of jmp_buf set up by `node_try`.  `py_raise_exc` longjmps
    // to the innermost frame so dispatch unwinds without every node on
    // the path checking `c->state`.  Empty stack ⇒ longjmp to err_jmp.
    jmp_buf *try_stack[64];
    int      try_top;

    // Currently-executing generator (NULL if not inside one).  yield
    // expressions read this to know which gen to swap back to.
    struct pygen *current_gen;

    // Built-in exception classes (constructed once at install_builtins).
    // PYSTRO_EXC_LIST(X) below enumerates every EXC_* field — useful for
    // save/restore across module imports (so caught exceptions match
    // by identity).
    VALUE EXC_Exception;
    VALUE EXC_TypeError;
    VALUE EXC_ValueError;
    VALUE EXC_NameError;
    VALUE EXC_IndexError;
    VALUE EXC_KeyError;
    VALUE EXC_ZeroDivisionError;
    VALUE EXC_AttributeError;
    VALUE EXC_RuntimeError;
    VALUE EXC_StopIteration;
    VALUE EXC_AssertionError;
    VALUE EXC_ImportError;
    VALUE EXC_ModuleNotFoundError;
    VALUE EXC_NotImplementedError;
    VALUE EXC_OSError;
    VALUE EXC_FileNotFoundError;
    VALUE EXC_OverflowError;
    VALUE EXC_ArithmeticError;
    VALUE EXC_BaseException;
    VALUE EXC_SystemExit;
    VALUE EXC_KeyboardInterrupt;
    VALUE EXC_GeneratorExit;
    VALUE EXC_UnicodeError;
    VALUE EXC_UnicodeDecodeError;
    VALUE EXC_UnicodeEncodeError;
    VALUE EXC_LookupError;
    VALUE EXC_FloatingPointError;
    VALUE EXC_ZeroDivisionError2;   // alias slot — unused
    VALUE EXC_RecursionError;
    VALUE EXC_MemoryError;
    VALUE EXC_BufferError;
    VALUE EXC_ReferenceError;
    VALUE EXC_SyntaxError;
    VALUE EXC_IndentationError;
    VALUE EXC_TabError;
    VALUE EXC_PermissionError;
    VALUE EXC_NotADirectoryError;
    VALUE EXC_IsADirectoryError;
    VALUE EXC_TimeoutError;
    VALUE EXC_BrokenPipeError;
    VALUE EXC_InterruptedError;
    VALUE EXC_ConnectionError;
    VALUE EXC_BlockingIOError;
    VALUE EXC_ChildProcessError;
    VALUE EXC_EOFError;
    VALUE EXC_StopAsyncIteration;

#define PYSTRO_EXC_LIST(X) \
    X(BaseException) X(Exception) X(SystemExit) X(KeyboardInterrupt) \
    X(GeneratorExit) X(StopIteration) X(StopAsyncIteration) \
    X(ArithmeticError) X(OverflowError) X(ZeroDivisionError) \
    X(FloatingPointError) X(AssertionError) X(AttributeError) \
    X(BufferError) X(EOFError) X(ImportError) X(ModuleNotFoundError) \
    X(LookupError) X(IndexError) X(KeyError) X(MemoryError) \
    X(NameError) X(RuntimeError) X(NotImplementedError) X(RecursionError) \
    X(SyntaxError) X(IndentationError) X(TabError) \
    X(TypeError) X(ValueError) X(UnicodeError) X(UnicodeDecodeError) \
    X(UnicodeEncodeError) X(ReferenceError) X(OSError) X(FileNotFoundError) \
    X(PermissionError) X(NotADirectoryError) X(IsADirectoryError) \
    X(TimeoutError) X(BrokenPipeError) X(InterruptedError) \
    X(ConnectionError) X(BlockingIOError) X(ChildProcessError)

    // Synthetic type classes for things that don't have a builtin_ctor:
    VALUE TYPE_NoneType;
    VALUE TYPE_function;
    VALUE TYPE_builtin_function_or_method;
    VALUE TYPE_method;          // bound method
    VALUE TYPE_module;
    VALUE TYPE_slice;
    VALUE TYPE_ellipsis;
    VALUE TYPE_NotImplementedType;
    VALUE TYPE_memoryview;
    VALUE TYPE_generator;
    VALUE TYPE_property;
    VALUE TYPE_staticmethod;
    VALUE TYPE_classmethod;
    VALUE TYPE_super;

    // True class objects for built-in types — set by install_builtins.
    // type(5) returns TYPE_int; isinstance(5, int) compares against
    // TYPE_int; class M(int): pass takes TYPE_int as its base.  These
    // are stored both here (for fast access) and in `globals` under
    // their normal names ("int", "str", ...).
    VALUE TYPE_type;
    VALUE TYPE_object;
    VALUE TYPE_int;
    VALUE TYPE_float;
    VALUE TYPE_complex;
    VALUE TYPE_bool;
    VALUE TYPE_str;
    VALUE TYPE_bytes;
    VALUE TYPE_bytearray;
    VALUE TYPE_list;
    VALUE TYPE_tuple;
    VALUE TYPE_dict;
    VALUE TYPE_set;
    VALUE TYPE_frozenset;
    VALUE TYPE_range;

    // Mini call-stack for traceback on uncaught exception.  py_apply
    // pushes the function name on entry and pops on exit.  Capped to
    // 1024 frames; deeper recursion just truncates.
    const char *call_stack[1024];
    int         call_top;
} CTX;

extern struct pyobj PY_NONE_OBJ, PY_TRUE_OBJ, PY_FALSE_OBJ;
#define PY_NONE  PY_OBJ_VAL(&PY_NONE_OBJ)
#define PY_TRUE  PY_OBJ_VAL(&PY_TRUE_OBJ)
#define PY_FALSE PY_OBJ_VAL(&PY_FALSE_OBJ)

// Type predicates.
static inline bool py_is_none(VALUE v)    { return v == PY_NONE; }
static inline bool py_is_bool(VALUE v)    { return v == PY_TRUE || v == PY_FALSE; }
static inline bool py_is_fix(VALUE v)     { return PY_IS_FIXNUM(v); }
static inline bool py_is_heap_float(VALUE v) { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_FLOAT; }
static inline bool py_is_float(VALUE v)   { return PY_IS_FLONUM(v) || py_is_heap_float(v); }
static inline bool py_is_bignum(VALUE v)  { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_BIGNUM; }
static inline bool py_is_int(VALUE v)     { return py_is_fix(v) || py_is_bignum(v); }
static inline bool py_is_str(VALUE v)     { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_STR; }
static inline bool py_is_bytes(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_BYTES; }
static inline bool py_is_bytearray(VALUE v){ return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_BYTEARRAY; }
static inline bool py_is_byteseq(VALUE v) { return py_is_bytes(v) || py_is_bytearray(v); }
static inline bool py_is_module(VALUE v)  { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_MODULE; }
static inline bool py_is_list(VALUE v)    { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_LIST; }
static inline bool py_is_tuple(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_TUPLE; }
static inline bool py_is_dict(VALUE v)    { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_DICT; }
static inline bool py_is_set(VALUE v)     { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_SET; }
static inline bool py_is_frozenset(VALUE v){ return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_FROZENSET; }
static inline bool py_is_any_set(VALUE v) { return py_is_set(v) || py_is_frozenset(v); }
static inline bool py_is_range(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_RANGE; }
static inline bool py_is_file(VALUE v)    { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_FILE; }
static inline bool py_is_complex(VALUE v) { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_COMPLEX; }
static inline bool py_is_super(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_SUPER; }
static inline bool py_is_func(VALUE v)    { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_FUNC; }
static inline bool py_is_builtin(VALUE v) { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_BUILTIN; }
static inline bool py_is_bound(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_BOUND_METHOD; }
static inline bool py_is_class(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_CLASS; }
static inline bool py_is_instance(VALUE v){ return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_INSTANCE; }
static inline bool py_is_callable(VALUE v) {
    return py_is_func(v) || py_is_builtin(v) || py_is_bound(v) || py_is_class(v);
}

// Python truthiness.
extern bool py_is_truthy_instance(VALUE v);     // dispatches __bool__/__len__
static inline bool
py_is_truthy(VALUE v)
{
    if (v == PY_NONE || v == PY_FALSE) return false;
    if (PY_IS_FIXNUM(v)) return PY_FIXVAL(v) != 0;
    if (PY_IS_FLONUM(v)) return py_flonum_to_double(v) != 0.0;
    struct pyobj *o = PY_PTR(v);
    switch (o->type) {
      case PY_T_FLOAT:  return o->dbl != 0.0;
      case PY_T_BIGNUM: return mpz_sgn(o->mpz) != 0;
      case PY_T_COMPLEX: return o->cpx.re != 0.0 || o->cpx.im != 0.0;
      case PY_T_STR:    return o->str.len != 0;
      case PY_T_BYTES:
      case PY_T_BYTEARRAY: return o->str.len != 0;
      case PY_T_LIST:
      case PY_T_TUPLE:  return o->list.len != 0;
      case PY_T_DICT:   return o->dict->used != 0;
      case PY_T_SET:
      case PY_T_FROZENSET: return o->dict->used != 0;
      case PY_T_INSTANCE: return py_is_truthy_instance(v);
      default:          return true;
    }
}

// Allocators / builders.
struct pyobj *py_alloc(int type);
VALUE py_make_float (double d);
VALUE py_make_int   (int64_t v);                  // fixnum if fits, else bignum
VALUE py_make_bignum(mpz_srcptr z);
VALUE py_make_str   (const char *s, size_t len);
VALUE py_make_str_take(char *s, size_t len);      // takes ownership
VALUE py_make_bytes (const char *s, size_t len);
VALUE py_make_bytearray(const char *s, size_t len);
VALUE py_make_list  (VALUE *items, size_t n);     // copies items; capa=max(n,4)
VALUE py_make_tuple (VALUE *items, size_t n);
VALUE py_make_dict  (void);
VALUE py_make_set   (void);
VALUE py_make_frozenset(void);
VALUE py_make_range (int64_t start, int64_t stop, int64_t step);
VALUE py_make_func  (struct Node *body, struct pyframe *env,
                     const char *name, int nparams, int n_pos_named,
                     int nlocals, VALUE *defaults_per_slot, bool leaf,
                     const char **param_names,
                     bool has_varargs, bool has_kwargs,
                     bool is_generator);
VALUE py_make_builtin(const char *name, py_builtin_fn fn, int min_argc, int max_argc);
VALUE py_make_bound (VALUE self, VALUE func);
VALUE py_make_class (const char *name, VALUE base, bool is_exception);
VALUE py_make_instance(VALUE cls);

// Frame.
struct pyframe *py_new_frame(struct pyframe *parent, int nslots);

// Globals.
void  py_global_define(CTX *c, const char *name, VALUE v);
VALUE py_global_ref   (CTX *c, const char *name);
void  py_global_set   (CTX *c, const char *name, VALUE v);
bool  py_global_has   (CTX *c, const char *name);

// Apply.  The fast path (closure with matching arity) is `static inline`
// in node.h; everything else routes here.
VALUE py_apply_slow(CTX *c, VALUE fn, int argc, VALUE *argv);

// Display + repr.
void  py_display(FILE *fp, VALUE v, bool repr);
VALUE py_to_str(CTX *c, VALUE v);
VALUE py_to_repr(CTX *c, VALUE v);

// Error / raise.
__attribute__((noreturn,format(printf,2,3)))
void py_error(CTX *c, const char *fmt, ...);
__attribute__((noreturn,format(printf,3,4)))
void py_raise_exc(CTX *c, VALUE cls, const char *fmt, ...);

// Numeric tower.
VALUE py_add (CTX *c, VALUE a, VALUE b);
VALUE py_sub (CTX *c, VALUE a, VALUE b);
VALUE py_mul (CTX *c, VALUE a, VALUE b);
VALUE py_truediv(CTX *c, VALUE a, VALUE b);
VALUE py_fdiv(CTX *c, VALUE a, VALUE b);
VALUE py_mod (CTX *c, VALUE a, VALUE b);
VALUE py_pow (CTX *c, VALUE a, VALUE b);
VALUE py_neg (CTX *c, VALUE a);
VALUE py_bit_and(CTX *c, VALUE a, VALUE b);
VALUE py_bit_or (CTX *c, VALUE a, VALUE b);
VALUE py_bit_xor(CTX *c, VALUE a, VALUE b);
VALUE py_bit_inv(CTX *c, VALUE a);
VALUE py_lshift (CTX *c, VALUE a, VALUE b);
VALUE py_rshift (CTX *c, VALUE a, VALUE b);
int   py_cmp (CTX *c, VALUE a, VALUE b);
VALUE py_eq  (CTX *c, VALUE a, VALUE b);
bool  py_eq_bool(CTX *c, VALUE a, VALUE b);

// Containers.
VALUE py_list_get   (CTX *c, VALUE list, VALUE idx);    // also tuple/str
VALUE py_list_set   (CTX *c, VALUE list, VALUE idx, VALUE val);
VALUE py_list_slice (CTX *c, VALUE seq, VALUE start, VALUE stop, VALUE step);
void  py_list_append(CTX *c, VALUE list, VALUE v);
size_t py_seq_len   (CTX *c, VALUE v);
VALUE py_dict_get   (CTX *c, VALUE d, VALUE key);
void  py_dict_set   (CTX *c, VALUE d, VALUE key, VALUE val);
bool  py_dict_has   (CTX *c, VALUE d, VALUE key);
bool  py_dict_remove(CTX *c, VALUE d, VALUE key);
uint64_t py_hash    (CTX *c, VALUE v);

// Membership: `x in y`.
bool py_contains(CTX *c, VALUE container, VALUE v);

// Attribute access.
VALUE py_getattr(CTX *c, VALUE v, const char *name);
void  py_setattr(CTX *c, VALUE v, const char *name, VALUE val);

// Method-call support: `o.m(...)` resolves to a callable; for instance
// methods we wrap as bound; for built-in type methods (str.split, ...)
// we look up via an opaque per-type table.
VALUE py_builtin_method(CTX *c, VALUE recv, const char *name);

// Iteration: returns an opaque iterator handle; `py_iter_next` returns
// PY_NONE on stop (and sets *done=true), else the next element.
struct py_iter {
    int kind;               // 0=list/tuple, 1=str, 2=range, 3=dict, 4=callable+sentinel,
                            // 8=enumerate, 9=zip, 10=map, 11=filter
    VALUE container;        // callable for kind=4; inner func for kind=10/11
    int64_t i;
    int64_t end;
    int64_t step;
    VALUE sentinel;         // kind=4 only
    // For wrapping iterators (enumerate/zip/map/filter): inner py_iter array.
    struct py_iter *inner;  // NULL for non-wrapping kinds
    int n_inner;            // # of inner iters (zip)
};
void py_iter_init(CTX *c, struct py_iter *it, VALUE iterable);
bool py_iter_next(CTX *c, struct py_iter *it, VALUE *out);

// Try-handler descriptor.  The parser packs an array of these into the
// global PYSTRO_HANDLERS table; node_try walks `nhandlers` of them
// starting at `handlers_idx`.
struct pyhandler {
    struct Node *exc_class;     // NULL ⇒ bare except (catch all)
    struct Node *body;
    const char  *name;          // NULL ⇒ no `as name`
    bool         name_is_global;
    int          name_slot;
};

// Unpack-assignment target.
struct pyunpack_target {
    bool        is_local;
    bool        is_starred;     // `*rest` — captures the slice
    int         slot;
    const char *global_name;
};

bool py_exc_matches(CTX *c, VALUE exc, VALUE cls);
void py_unpack_assign(CTX *c, struct pyunpack_target *t, uint32_t n, VALUE rhs);
void py_class_add_method(CTX *c, VALUE cls, const char *name, VALUE fn);

// Side tables populated by the parser, consumed by node_eval.c.
struct Node;
extern struct Node            **PYSTRO_NODE_TABLE;
extern struct pyhandler        *PYSTRO_HANDLERS;
extern struct pyunpack_target  *PYSTRO_UNPACK_TARGETS;
extern const char             **PYSTRO_NAME_TABLE;     // bag of param name lists for node_def

// One kwarg entry: a name and the AST node producing the value.  Both
// node_call_kw and the `**dict` expansion path share the type.
struct pykwarg {
    const char *name;
    struct Node *value;
};
extern struct pykwarg          *PYSTRO_KWARGS;

// Call-site argument with spread support.  kind:
//   0 = positional (single value)
//   1 = `*expr` — expand iterable into positional slots
//   2 = `name=expr` (single kwarg)
//   3 = `**expr` — expand dict into kwargs
struct pyspread_arg {
    int          kind;
    const char  *name;
    struct Node *node;
};
extern struct pyspread_arg     *PYSTRO_SPREADS;

// One default-arg entry: a slot index in the param list plus the AST
// node producing the default value.  Used by node_def / node_lambda.
struct pydefault {
    int          slot;
    struct Node *expr;
};
extern struct pydefault        *PYSTRO_DEFAULTS;

// match / case patterns.  The parser packs a tree of patterns into
// PYSTRO_PATTERNS and gives node_match a starting index per case.
enum py_pat_kind {
    PYPAT_LITERAL = 0,    // pre-evaluated literal value
    PYPAT_CAPTURE,        // name binding (any value matches)
    PYPAT_WILDCARD,       // `_`
    PYPAT_OR,             // children union — first matching wins
    PYPAT_SEQUENCE,       // [a, b, ...] / (a, b, ...)
    PYPAT_CLASS,          // ClassName() — isinstance check
    PYPAT_CLASS_ARGS,     // ClassName(attr=pat, ...) — isinstance + attr check
    PYPAT_MAPPING,        // {"k": pat, ...} — dict with required keys
    PYPAT_VALUE,          // dotted name read at match time (e.g. Color.RED)
    PYPAT_STAR,           // *NAME inside a sequence — captures the rest
};
struct pypat {
    int kind;
    struct Node *literal;       // PYPAT_LITERAL / PYPAT_VALUE / PYPAT_CLASS / PYPAT_CLASS_ARGS
    int slot;                   // PYPAT_CAPTURE: local slot, -1 if global
    const char *name;           // PYPAT_CAPTURE: global name (when slot=-1)
    int first_child;            // OR / SEQUENCE / CLASS_ARGS / MAPPING: index into PYSTRO_PATTERNS
    int nchildren;
    struct Node **keys;         // PYPAT_MAPPING: key NODE *exprs, length nchildren
    const char **attrs;         // PYPAT_CLASS_ARGS: attr name per child
};
extern struct pypat *PYSTRO_PATTERNS;

struct pycase {
    int          pat_idx;       // root pattern in PYSTRO_PATTERNS
    struct Node *guard;         // optional `if guard` (NULL = none)
    struct Node *body;
};
extern struct pycase *PYSTRO_CASES;

#endif // PYSTRO_CONTEXT_H
