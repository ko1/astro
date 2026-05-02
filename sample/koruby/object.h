#ifndef KORUBY_OBJECT_H
#define KORUBY_OBJECT_H 1

#include "context.h"

/* heap object structures (CRuby-inspired) */

struct korb_object {
    struct RBasic basic;
    uint32_t ivar_cnt;
    uint32_t ivar_capa;
    VALUE *ivars;
};

struct korb_string {
    struct RBasic basic;
    char *ptr;
    long len;
    long capa;
};

struct korb_array {
    struct RBasic basic;
    VALUE *ptr;
    long len;
    long capa;
};

struct korb_hash_entry {
    VALUE key;
    VALUE value;
    uint64_t hash;
    struct korb_hash_entry *next;        /* insertion order chain */
    struct korb_hash_entry *bucket_next; /* per-bucket collision chain */
};

struct korb_hash {
    struct RBasic basic;
    struct korb_hash_entry **buckets;
    uint32_t bucket_cnt;
    uint32_t size;
    struct korb_hash_entry *first;  /* insertion order */
    struct korb_hash_entry *last;
    VALUE default_value;
    VALUE default_proc;             /* Proc for Hash.new { |h, k| ... }; Qnil otherwise */
    bool compare_by_identity;       /* keys compared by object identity */
};

struct korb_range {
    struct RBasic basic;
    VALUE begin;
    VALUE end;
    bool exclude_end;
};

struct korb_bignum {
    struct RBasic basic;
    void *mpz; /* mpz_t actually (mpz_struct[1]) */
};

struct korb_float {
    struct RBasic basic;
    double value;
};

enum korb_visibility {
    KORB_VIS_PUBLIC = 0,
    KORB_VIS_PRIVATE = 1,
    KORB_VIS_PROTECTED = 2,
};

struct korb_method {
    enum {
        KORB_METHOD_AST,
        KORB_METHOD_CFUNC,
        KORB_METHOD_PROC,        /* `define_method(:n) { ... }` — body is a proc */
    } type;
    ID name;
    struct korb_class *defining_class;
    struct korb_cref *def_cref;   /* lexical cref captured at def-time */
    bool is_simple_frame;         /* AST methods only: body has no yield/super/block_given/const/blocked-call */
    enum korb_visibility visibility;
    union {
        struct {
            struct Node *body;
            uint32_t required_params_cnt;  /* mandatory pre params */
            uint32_t total_params_cnt;     /* required + optional + rest(0/1) + post + (kwh?) */
            uint32_t locals_cnt;
            int rest_slot;                 /* -1 if no *rest */
            int block_slot;                /* -1 if no &blk */
            uint32_t post_params_cnt;      /* params after *rest (def f(a, *r, b)) */
            int kwh_save_slot;             /* slot to stash peeled kwargs hash (-1 if no kwargs) */
        } ast;
        struct {
            VALUE (*func)(CTX *c, VALUE self, int argc, VALUE *argv);
            int argc; /* -1 for varargs */
        } cfunc;
        struct {
            struct korb_proc *proc;        /* captured block: env + body + param_base */
        } proc;
    } u;
};

struct korb_method_table_entry {
    ID name;
    struct korb_method *method;
    struct korb_method_table_entry *next;
};

struct korb_method_table {
    struct korb_method_table_entry **buckets;
    uint32_t bucket_cnt;
    uint32_t size;
};

struct korb_const_entry {
    ID name;
    VALUE value;
    struct korb_const_entry *next;
};

struct korb_class {
    struct RBasic basic;       /* flags = T_CLASS or T_MODULE */
    enum korb_type instance_type; /* type of instances of this class */
    ID name;
    struct korb_class *super;
    struct korb_method_table methods;
    struct korb_const_entry *constants;
    /* ivar shape: name -> slot (linear table) */
    ID *ivar_names;
    uint32_t ivar_count;
    uint32_t ivar_capa;
    /* Modules `include`d into this class (in include-order, last include
     * first in lookup).  Tracked so `ancestors` / `is_a?` see them; the
     * actual method lookup still uses the flatten-copy in `methods`. */
    struct korb_class **includes;
    uint32_t includes_cnt;
    uint32_t includes_capa;
    /* Modules `prepend`ed.  Their methods are also flattened into
     * `methods` (overriding the class's own) but the prepend list is
     * kept so that `ancestors` orders them BEFORE the class itself. */
    struct korb_class **prepends;
    uint32_t prepends_cnt;
    uint32_t prepends_capa;
    /* `private` / `protected` / `public` with no args inside a class
     * body sets this default; subsequent `def`s pick it up. */
    enum korb_visibility default_visibility;
};

struct korb_proc {
    struct RBasic basic;
    struct Node *body;
    VALUE *env;             /* shared/captured locals */
    uint32_t env_size;      /* slots covered by env (absolute high-water of body) */
    uint32_t params_cnt;
    uint32_t param_base;    /* absolute slot where block's params begin */
    int rest_slot;          /* absolute slot for *rest, or -1 */
    int kwh_save_slot;      /* absolute slot for peeled kwargs hash, or -1 */
    /* Enclosing method's block as seen at proc creation time.  When the
     * block's body itself does `yield`, dispatch to this enclosing block
     * rather than the block being currently executed.  Captures the
     * lexical-block-target semantics CRuby uses. */
    struct korb_proc *enclosing_block;
    VALUE self;
    bool is_lambda;
    /* Set at parse time when the body contains a `proc { }`/lambda/`->()`
     * literal.  Yield uses this to switch to a fresh-env-with-writeback
     * path so each iteration captures its own block-local slots —
     * without the flag, every iteration's captured proc would alias
     * the same env memory and see the last iter's values. */
    bool creates_proc;
};

/* Method object: a bound (receiver, method) pair, callable via #call/#[] */
struct korb_method_obj {
    struct RBasic basic;
    VALUE receiver;
    ID name;
};

/* global VM */
struct korb_vm {
    state_serial_t method_serial;

    /* core classes */
    struct korb_class *object_class;
    struct korb_class *class_class;
    struct korb_class *module_class;
    struct korb_class *integer_class;
    struct korb_class *float_class;
    struct korb_class *string_class;
    struct korb_class *array_class;
    struct korb_class *hash_class;
    struct korb_class *symbol_class;
    struct korb_class *true_class;
    struct korb_class *false_class;
    struct korb_class *nil_class;
    struct korb_class *proc_class;
    struct korb_class *range_class;
    struct korb_class *kernel_module;
    struct korb_class *comparable_module;
    struct korb_class *enumerable_module;
    struct korb_class *numeric_class;
    struct korb_class *fiber_class;
    struct korb_class *method_class;

    /* globals */
    struct korb_method_table globals;

    /* topframe class (for top-level def, top-level constants) */
    struct korb_class *main_obj_class; /* the singleton-of-main-obj */
    VALUE main_obj;

    /* The currently-executing CTX (set by main).  Used by
     * korb_hash_value to invoke user-defined #hash on custom-class
     * keys.  Single-threaded, so a single global is fine. */
    struct CTX_struct *current_ctx;
};

extern struct korb_vm *korb_vm;

/* ---------- API ---------- */

void korb_runtime_init(void);

/* memory */
void *korb_xmalloc(size_t size);
void *korb_xmalloc_atomic(size_t size); /* no-pointer mem (e.g., string char buffer) */
void *korb_xcalloc(size_t n, size_t sz);
void *korb_xrealloc(void *p, size_t newsize);
void  korb_xfree(void *p);

/* ID */
ID korb_intern(const char *str);
ID korb_intern_n(const char *str, long len);
const char *korb_id_name(ID id);

/* class system */
VALUE korb_class_of(VALUE v);
struct korb_class *korb_class_of_class_slow(VALUE v); /* immediate fallbacks */
/* Hot path: heap T_OBJECT load.  Called per method dispatch. */
static inline __attribute__((always_inline)) struct korb_class *
korb_class_of_class(VALUE v) {
    if (LIKELY(!SPECIAL_CONST_P(v))) {
        return (struct korb_class *)((struct RBasic *)v)->klass;
    }
    return korb_class_of_class_slow(v);
}
struct korb_class *korb_class_new(ID name, struct korb_class *super, enum korb_type instance_type);
struct korb_class *korb_module_new(ID name);
void korb_class_add_method_ast(struct korb_class *klass, ID name, struct Node *body, uint32_t params_cnt, uint32_t locals_cnt);
void korb_class_add_method_ast_full(struct korb_class *klass, ID name, struct Node *body,
                                    uint32_t required_params, uint32_t total_params,
                                    int rest_slot, uint32_t locals_cnt);
void korb_class_add_method_ast_full_cref(struct korb_class *klass, ID name, struct Node *body,
                                          uint32_t required_params, uint32_t total_params,
                                          int rest_slot, uint32_t locals_cnt,
                                          struct korb_cref *def_cref);
struct korb_cref *korb_cref_dup(struct korb_cref *src);
void korb_class_add_method_cfunc(struct korb_class *klass, ID name, VALUE (*func)(CTX *, VALUE, int, VALUE *), int argc);
void korb_class_set_method_block_slot(struct korb_class *klass, ID name, int slot);
void korb_class_set_method_post_params_cnt(struct korb_class *klass, ID name, uint32_t cnt);
void korb_class_set_method_kwh_save_slot(struct korb_class *klass, ID name, int slot);
void korb_class_alias_method(struct korb_class *klass, ID new_name, struct korb_method *m);
struct korb_method *korb_class_find_method(const struct korb_class *klass, ID name);
struct korb_method *korb_class_find_super_method(const struct korb_class *receiver_klass,
                                                 const struct korb_class *defining_class,
                                                 ID name);
void korb_module_include(struct korb_class *klass, struct korb_class *mod);
struct korb_class *korb_singleton_class_of(struct korb_class *klass);

/* constants */
void korb_const_set(struct korb_class *klass, ID name, VALUE value);
VALUE korb_const_get(struct korb_class *klass, ID name);
bool korb_const_has(struct korb_class *klass, ID name);

/* objects */
VALUE korb_object_new(struct korb_class *klass);
VALUE korb_ivar_get(VALUE obj, ID name);
void  korb_ivar_set(VALUE obj, ID name, VALUE value);
VALUE korb_ivar_get_ic_slow(VALUE obj, ID name, struct ivar_cache *cache);
void  korb_ivar_set_ic_slow(VALUE obj, ID name, VALUE val, struct ivar_cache *cache);

/* Fast inline ivar getter — caches (klass, slot) on the AST node.  Cache
 * miss + non-T_OBJECT goes through the out-of-line slow path. */
static inline __attribute__((always_inline)) VALUE
korb_ivar_get_ic(VALUE obj, ID name, struct ivar_cache *cache) {
    if (UNLIKELY(SPECIAL_CONST_P(obj))) return Qnil;
    if (UNLIKELY(BUILTIN_TYPE(obj) != T_OBJECT)) return Qnil;
    struct korb_object *o = (struct korb_object *)obj;
    if (LIKELY(cache->klass == (struct korb_class *)o->basic.klass && cache->slot >= 0)) {
        uint32_t s = (uint32_t)cache->slot;
        if (LIKELY(s < o->ivar_cnt)) return o->ivars[s];
        return Qnil;
    }
    return korb_ivar_get_ic_slow(obj, name, cache);
}

/* Fast inline ivar setter — same monomorphic cache pattern.  Cache miss
 * (different klass / unset slot / first write past current capa) goes
 * through the slow path which handles growth + slot assignment. */
static inline __attribute__((always_inline)) void
korb_ivar_set_ic(VALUE obj, ID name, VALUE val, struct ivar_cache *cache) {
    if (UNLIKELY(SPECIAL_CONST_P(obj))) return;
    if (UNLIKELY(BUILTIN_TYPE(obj) != T_OBJECT)) return;
    struct korb_object *o = (struct korb_object *)obj;
    if (LIKELY(cache->klass == (struct korb_class *)o->basic.klass && cache->slot >= 0)) {
        uint32_t s = (uint32_t)cache->slot;
        if (LIKELY(s < o->ivar_cnt)) {
            o->ivars[s] = val;
            return;
        }
    }
    korb_ivar_set_ic_slow(obj, name, val, cache);
}

/* string */
VALUE korb_str_new(const char *p, long len);
VALUE korb_str_new_cstr(const char *cstr);
VALUE korb_str_dup(VALUE s);
VALUE korb_str_concat(VALUE a, VALUE b);
VALUE korb_str_inspect(VALUE s);
const char *korb_str_cstr(VALUE s); /* terminates */
long  korb_str_len(VALUE s);

/* array */
VALUE korb_ary_new_capa(long capa);
VALUE korb_ary_new(void);
VALUE korb_ary_new_from_values(long n, const VALUE *vals);
void  korb_ary_push(VALUE ary, VALUE v);
VALUE korb_ary_pop(VALUE ary);
void  korb_ary_aset(VALUE ary, long i, VALUE v);

/* korb_ary_aref / korb_ary_len: inlined into SDs.  Hot in optcarrot
 * (`@output_color[pixel]`, `sprite[2]`, etc.).  Both are tiny and
 * struct korb_array is fully visible above. */
static inline __attribute__((always_inline)) VALUE
korb_ary_aref(VALUE av, long i) {
    struct korb_array *a = (struct korb_array *)av;
    if (i < 0) i += a->len;
    if ((unsigned long)i >= (unsigned long)a->len) return Qnil;
    return a->ptr[i];
}
static inline __attribute__((always_inline)) long
korb_ary_len(VALUE av) {
    return ((struct korb_array *)av)->len;
}

/* hash */
VALUE korb_hash_new(void);
VALUE korb_hash_aref_slow(VALUE h, VALUE key);

/* korb_hash_aref: inlined fast path for FIXNUM / SYMBOL keys (the
 * common case in optcarrot's @sp_map[@hclk]).  Strings and
 * compare_by_identity tables go through korb_hash_aref_slow.
 * bucket_cnt is always a power of 2 (init=8, resize doubles), so
 * `& (bucket_cnt-1)` replaces modulo. */
static inline __attribute__((always_inline)) VALUE
korb_hash_aref(VALUE hv, VALUE key) {
    struct korb_hash *h = (struct korb_hash *)hv;
    if (UNLIKELY(h->compare_by_identity)) return korb_hash_aref_slow(hv, key);
    uint64_t hh;
    if (LIKELY(FIXNUM_P(key))) {
        hh = (uint64_t)key * 11400714819323198485ULL;
    } else if (SYMBOL_P(key)) {
        hh = (uint64_t)key * 2654435761ULL;
    } else {
        return korb_hash_aref_slow(hv, key);
    }
    uint32_t b = (uint32_t)hh & (h->bucket_cnt - 1);
    for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
        if (e->hash == hh && e->key == key) return e->value;
    }
    return h->default_value;
}
VALUE korb_hash_aset(VALUE h, VALUE key, VALUE val);
long  korb_hash_size(VALUE h);

/* symbol */
VALUE korb_id2sym(ID id);
ID    korb_sym2id(VALUE sym);
VALUE korb_str_to_sym(VALUE str);

/* float / bignum */
/* korb_float_new: try FLONUM-encode (fast inline), heap-allocate via
 * out-of-line slow path otherwise.  Inlined so that mandelbrot-style
 * Float-heavy hot loops don't pay a cross-.so call per arithmetic
 * intermediate. */
VALUE korb_float_new_heap(double d);
static inline __attribute__((always_inline)) VALUE
korb_float_new(double d) {
    VALUE flo = korb_double_to_flonum(d);
    if (LIKELY(flo)) return flo;
    return korb_float_new_heap(d);
}

/* korb_num2dbl: same.  Most calls hit FLONUM/FIXNUM paths and bail out
 * before touching the slow heap-Float / Bignum branches. */
double korb_num2dbl_slow(VALUE v);
static inline __attribute__((always_inline)) double
korb_num2dbl(VALUE v) {
    if (LIKELY(FLONUM_P(v))) return korb_flonum_to_double(v);
    if (LIKELY(FIXNUM_P(v))) return (double)FIX2LONG(v);
    return korb_num2dbl_slow(v);
}
VALUE korb_bignum_new_str(const char *str, int base);
VALUE korb_bignum_new_long(long v);
VALUE korb_int_plus(VALUE a, VALUE b);
VALUE korb_int_minus(VALUE a, VALUE b);
VALUE korb_int_mul(VALUE a, VALUE b);
VALUE korb_int_div(VALUE a, VALUE b);
VALUE korb_int_mod(VALUE a, VALUE b);
VALUE korb_int_lshift(VALUE a, VALUE b);
VALUE korb_int_rshift(VALUE a, VALUE b);
VALUE korb_int_and(VALUE a, VALUE b);
VALUE korb_int_or(VALUE a, VALUE b);
VALUE korb_int_xor(VALUE a, VALUE b);
int   korb_int_cmp(VALUE a, VALUE b);
bool  korb_int_eq(VALUE a, VALUE b);

/* equality / inspect */
bool  korb_eq(VALUE a, VALUE b);
bool  korb_eql(VALUE a, VALUE b);
uint64_t korb_hash_value(VALUE v);
VALUE korb_inspect(VALUE v);
VALUE korb_inspect_dispatch(CTX *c, VALUE v);
VALUE korb_to_s(VALUE v);
VALUE korb_to_s_dispatch(CTX *c, VALUE v);
void  korb_p(VALUE v); /* writes to stdout with newline */

/* errors / exceptions */
VALUE korb_exc_new(struct korb_class *klass, const char *msg);
void  korb_raise(CTX *c, struct korb_class *klass, const char *fmt, ...);
VALUE korb_build_backtrace(CTX *c, int raise_line);
void  korb_exc_set_backtrace(CTX *c, VALUE exc, int raise_line);

/* method dispatch helper */
VALUE korb_funcall(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv);
VALUE korb_funcall_with_block(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv, VALUE block);
VALUE korb_dispatch_call(CTX *c, struct Node *callsite, VALUE recv, ID name, uint32_t argc, uint32_t arg_index, struct korb_proc *block, struct method_cache *mc);

extern state_serial_t korb_g_method_serial;  /* mirrored from korb_vm->method_serial */

/* True once user code has redefined a method on Integer/Float/Array/...
 * Fast paths in node.def consult this flag; it stays true for the rest
 * of the run. */
extern bool korb_g_basic_op_redefined;
void korb_check_basic_op_redef(struct korb_class *target, ID name);

/* Stable function-pointer addresses for mc->prologue — used as kind tags
 * in the guarded direct call below (compared by name, then dispatched
 * inline via the static-inline body in prologues.h). */
VALUE prologue_ast_simple_0(CTX *c, struct Node *callsite, VALUE recv,
                            uint32_t argc, uint32_t arg_index,
                            struct korb_proc *block, struct method_cache *mc);
VALUE prologue_ast_simple_1(CTX *c, struct Node *callsite, VALUE recv,
                            uint32_t argc, uint32_t arg_index,
                            struct korb_proc *block, struct method_cache *mc);
VALUE prologue_ast_simple_2(CTX *c, struct Node *callsite, VALUE recv,
                            uint32_t argc, uint32_t arg_index,
                            struct korb_proc *block, struct method_cache *mc);
VALUE prologue_ast_simple_3(CTX *c, struct Node *callsite, VALUE recv,
                            uint32_t argc, uint32_t arg_index,
                            struct korb_proc *block, struct method_cache *mc);
VALUE prologue_cfunc(CTX *c, struct Node *callsite, VALUE recv,
                     uint32_t argc, uint32_t arg_index,
                     struct korb_proc *block, struct method_cache *mc);

/* Inline cache-hit fast path for method dispatch.  On cache hit (LIKELY),
 * directly call mc->prologue — no function call into the slower path.
 * Cache miss falls through to korb_dispatch_call which fills mc and
 * dispatches.
 *
 * Guarded direct call: compare mc->prologue to the hottest variants and
 * dispatch via the inline body when matched.  prologues.h provides the
 * inline implementations so each TU gets its own copy and gcc can fully
 * inline the prologue body into the SD that includes us. */
#include "prologues.h"

/* Cold path: resolved-but-non-public method, raise NoMethodError per
 * Ruby semantics.  Defined out-of-line in object.c. */
extern VALUE korb_dispatch_visibility_raise(CTX *c, struct korb_method *m,
                                            ID name, struct korb_class *klass,
                                            VALUE recv);

static inline __attribute__((always_inline)) VALUE
korb_dispatch_call_cached(CTX * restrict c, struct Node * restrict callsite,
                          VALUE recv, ID name, uint32_t argc,
                          uint32_t arg_index, struct korb_proc *block,
                          struct method_cache *mc)
{
    struct korb_class *klass = korb_class_of_class(recv);
    if (LIKELY(mc && mc->serial == korb_g_method_serial && mc->klass == klass)) {
        /* Visibility check: private methods need an implicit-self call
         * (recv == c->self).  Protected methods need the caller's class
         * to include the target's class in its hierarchy. */
        if (UNLIKELY(mc->method && mc->method->visibility != KORB_VIS_PUBLIC)) {
            if (mc->method->visibility == KORB_VIS_PRIVATE && recv != c->self) {
                return korb_dispatch_visibility_raise(c, mc->method, name, klass, recv);
            }
            if (mc->method->visibility == KORB_VIS_PROTECTED) {
                struct korb_class *caller_klass = korb_class_of_class(c->self);
                struct korb_class *target = mc->method->defining_class;
                bool ok = false;
                for (struct korb_class *k = caller_klass; k; k = k->super) {
                    if (k == target) { ok = true; break; }
                }
                if (!ok) return korb_dispatch_visibility_raise(c, mc->method, name, klass, recv);
            }
        }
        korb_prologue_t p = mc->prologue;
        if (p == prologue_ast_simple_0) return prologue_ast_simple_inl(c, callsite, recv, argc, arg_index, block, mc, 0);
        if (p == prologue_ast_simple_1) return prologue_ast_simple_inl(c, callsite, recv, argc, arg_index, block, mc, 1);
        if (p == prologue_ast_simple_2) return prologue_ast_simple_inl(c, callsite, recv, argc, arg_index, block, mc, 2);
        if (p == prologue_ast_simple_3) return prologue_ast_simple_inl(c, callsite, recv, argc, arg_index, block, mc, 3);
        if (p == prologue_cfunc)        return prologue_cfunc_inl     (c, callsite, recv, argc, arg_index, block, mc);
        return p(c, callsite, recv, argc, arg_index, block, mc);
    }
    return korb_dispatch_call(c, callsite, recv, name, argc, arg_index, block, mc);
}
VALUE korb_dispatch_binop(CTX *c, VALUE recv, ID name, int argc, VALUE *argv);

/* Cold tails for fast-path NODEs.  Bodies live in object.c and are
 * called via PLT/GOT from each SD.so, instead of being inlined into
 * every SD that uses node_plus / node_aref / etc.  Trades a tiny
 * extra call (only on the slow path) for a substantially smaller
 * all.so and lower compile time. */
VALUE korb_node_plus_slow  (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_minus_slow (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_mul_slow   (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_div_slow   (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_mod_slow   (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_uminus_slow(CTX *c, VALUE v);
VALUE korb_node_band_slow  (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_bor_slow   (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_bxor_slow  (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_lshift_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_rshift_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_lt_slow    (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_le_slow    (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_gt_slow    (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_ge_slow    (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
VALUE korb_node_aref_slow  (CTX *c, VALUE r, VALUE i, uint32_t arg_index);
VALUE korb_node_aset_slow  (CTX *c, VALUE r, VALUE i, VALUE v, uint32_t arg_index);

/* Cold tail of korb_yield: handles auto-destructure (block has N>1
 * params, called with single Array of size M), variable argc paths,
 * and the param/argc-mismatch slow case. */
VALUE korb_yield_slow(CTX *c, struct korb_proc *blk, uint32_t argc, VALUE *argv);

extern struct korb_proc *current_block;

/* Fast path: hot in `ary.each { |x| ... }` style code (Array#each,
 * Hash#each, etc.) — argc and params_cnt are usually 1, no
 * auto-destructure, no need to copy more than 1 arg.  Inlined into
 * builtins.c iterators (ary_each etc.) so the cross-.so dispatcher
 * call disappears. */
static inline __attribute__((always_inline)) VALUE
korb_yield(CTX *c, uint32_t argc, VALUE *argv) {
    struct korb_proc *blk = current_block;
    if (UNLIKELY(!blk)) {
        korb_raise(c, NULL, "no block given (yield)");
        return Qnil;
    }
    /* Symbol-proc shim — fall to slow path. */
    if (UNLIKELY(blk->body == NULL)) return korb_yield_slow(c, blk, argc, argv);
    /* Block creates a Proc inside its body — needs per-iteration env
     * (via the slow path's fresh-env-with-writeback) so each captured
     * proc has its own block-locals. */
    if (UNLIKELY(blk->creates_proc)) return korb_yield_slow(c, blk, argc, argv);
    /* Common case: single arg, single param, no destructure.  Inline. */
    if (LIKELY(argc == 1 && blk->params_cnt == 1)) {
        VALUE arg = argv[0];  /* snapshot before fp swap */
        VALUE *prev_fp = c->fp;
        VALUE prev_self = c->self;
        struct korb_proc *prev_block = current_block;
        VALUE *bfp = blk->env;
        bfp[blk->param_base] = arg;
        c->self = blk->self;
        c->fp = bfp;
        /* Lexical block target: yield inside this block goes to the
         * enclosing method's block, not back to this block itself. */
        current_block = blk->enclosing_block;
        VALUE r;
    redo_yield:
        r = blk->body->head.dispatcher(c, blk->body);
        if (UNLIKELY(c->state == KORB_REDO)) {
            c->state = KORB_NORMAL; c->state_value = Qnil;
            goto redo_yield;
        }
        c->fp = prev_fp;
        c->self = prev_self;
        current_block = prev_block;
        if (UNLIKELY(c->state == KORB_NEXT)) {
            VALUE nv = c->state_value;
            c->state = KORB_NORMAL; c->state_value = Qnil;
            return nv;
        }
        return r;
    }
    return korb_yield_slow(c, blk, argc, argv);
}

bool korb_block_given(void);

/* gvar */
VALUE korb_gvar_get(ID name);
void  korb_gvar_set(ID name, VALUE v);

/* const lookup along current scope (uses CTX->current_class) */
VALUE korb_const_lookup(CTX *c, ID name);

/* range */
VALUE korb_range_new(VALUE begin, VALUE end, bool exclude_end);

/* proc */
VALUE korb_proc_new(struct Node *body, VALUE *fp, uint32_t env_size, uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda);
void korb_proc_snapshot_env_if_in_frame(VALUE v, VALUE *fp_lo, VALUE *fp_hi);

/* Builtins init */
void korb_init_builtins(void);

/* Fiber */
struct korb_fiber;
VALUE korb_fiber_new(struct korb_proc *block);
VALUE korb_fiber_resume(CTX *c, VALUE fib, int argc, VALUE *argv);
VALUE korb_fiber_yield(CTX *c, int argc, VALUE *argv);

/* file load (parse + eval) */
VALUE korb_load_file(CTX *c, const char *path);
VALUE korb_eval_string(CTX *c, const char *src, size_t len, const char *filename);

/* path resolution for require_relative */
char *korb_dirname(const char *path);
char *korb_join_path(const char *dir, const char *name);
bool korb_file_exists(const char *path);
char *korb_resolve_relative(const char *current_file, const char *name);


/* booleans */
#define KORB_BOOL(b) ((b) ? Qtrue : Qfalse)

/* object FLAGS access */
#define FL_USER_SHIFT 12
#define FL_USER(n)    ((VALUE)1 << (FL_USER_SHIFT + (n)))

/* Frozen-object guard.  Inserted at the entry of mutating cfuncs so
 * `frozen_str << "x"` etc. raise FrozenError instead of silently
 * mutating.  Skipped for immediates (Fixnum / Symbol / nil/true/false
 * are inherently frozen but we don't track FL_FROZEN on them and
 * none of the cfuncs that call this take them as `self` anyway). */
static inline bool korb_obj_frozen_p(VALUE v) {
    if (SPECIAL_CONST_P(v)) return true;
    return (RBASIC(v)->flags & FL_FROZEN) != 0;
}
#define CHECK_FROZEN_RET(c, self, ret) do { \
    if (UNLIKELY(korb_obj_frozen_p(self))) { \
        VALUE _eFrozen = korb_const_get(korb_vm->object_class, korb_intern("FrozenError")); \
        korb_raise((c), (struct korb_class *)_eFrozen, "can't modify frozen object"); \
        return (ret); \
    } \
} while (0)

/* well-known IDs */
extern ID id_initialize, id_to_s, id_inspect, id_call, id_each, id_new;
extern ID id_op_plus, id_op_minus, id_op_mul, id_op_div, id_op_mod;
extern ID id_op_eq, id_op_neq, id_op_lt, id_op_le, id_op_gt, id_op_ge;
extern ID id_op_aref, id_op_aset, id_op_lshift, id_op_rshift, id_op_and, id_op_or, id_op_xor;

#endif /* KORUBY_OBJECT_H */
