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
        KORB_METHOD_UNDEF,       /* `undef name` — blocks ancestor lookup */
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
            ID *local_names;               /* slot index → name ID; len=locals_cnt; NULL when none */
            /* param_position → fp slot.  NULL = identity (slot[i]=i),
             * which is the common case.  Non-NULL is needed when the
             * params include a multi_target like `def m(a, (b, c), d)`
             * — (b, c) shares b's slot under the naive identity mapping,
             * which clobbers d's slot for the 3rd caller arg.  Holds
             * total_params_cnt entries; default identity for non-shadowed
             * params.  Synthetic slots are allocated past locals_cnt. */
            int *param_holder_slots;
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
    /* MRO depth: 0 = defined directly on the class (or cfunc registered
     * via DEF), 1+ = imported from an included module.  Newer includes
     * may override older include entries (depth>0) but never a depth-0
     * entry. */
    uint16_t include_depth;
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
    bool is_private;  /* set by Module#private_constant; lookup raises
                       * NameError when accessed via explicit qualifier
                       * from outside the defining module. */
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
    /* Class-level @ivars (`class Foo; @count = 0; ...`).  Stored as a
     * tiny linear (name, value) list — class-level ivars are typically
     * very few (counters, configuration), so a list beats a hashtable. */
    struct korb_class_ivar {
        ID name;
        VALUE value;
    } *class_ivars;
    uint32_t class_ivar_cnt;
    uint32_t class_ivar_capa;
    /* Class variables (@@var).  Shared across the class hierarchy:
     * lookup walks `super` chain, write targets the highest ancestor
     * that already has the cvar (else the current class).  Same flat
     * (name, value) list as class_ivars. */
    struct korb_class_cvar {
        ID name;
        VALUE value;
    } *cvars;
    uint32_t cvar_cnt;
    uint32_t cvar_capa;
    /* For an anonymous module/class first named via parent::Const = self
     * BEFORE the parent itself was named, remember that linkage so when
     * the parent eventually gets a real name we can recompute this
     * module's name to "Parent::Const".  NULL once the name is finalized
     * (i.e. once anon_parent itself is no longer anonymous). */
    struct korb_class *anon_parent;
    ID anon_name_in_parent;
};

struct korb_proc {
    struct RBasic basic;
    struct Node *body;
    /* Frame ID of the lexical method where this block was created.
     * Used by node_break / node_return to detect stale-frame escapes:
     * when this block's enclosing method has already returned, the
     * frame_id at its slot won't match this snapshot, and we raise
     * LocalJumpError. */
    uint64_t enclosing_frame_id;
    VALUE *env;             /* shared/captured locals */
    uint32_t env_size;      /* slots covered by env (absolute high-water of body) */
    uint32_t params_cnt;    /* total positional params: required + optional */
    uint32_t opt_cnt;       /* of params_cnt, how many are optional (have a default).
                             * proc_call uses this to fill missing optional slots
                             * with Qundef (vs Qnil for required) so the body
                             * prologue's node_default_init can detect them. */
    uint32_t param_base;    /* absolute slot where block's params begin */
    int rest_slot;          /* absolute slot for *rest, or -1 */
    int kwh_save_slot;      /* absolute slot for peeled kwargs hash, or -1 */
    int block_slot;         /* absolute slot for &blk parameter, or -1 */
    uint32_t post_cnt;      /* required params after *rest (def f(*r, b, c)) */
    /* Enclosing method's block as seen at proc creation time.  When the
     * block's body itself does `yield`, dispatch to this enclosing block
     * rather than the block being currently executed.  Captures the
     * lexical-block-target semantics CRuby uses. */
    struct korb_proc *enclosing_block;
    VALUE self;
    bool is_lambda;
    /* `|a, |` trailing comma is `PM_IMPLICIT_REST_NODE` in prism, with
     * the same autosplat-blocking effect as anonymous `*` (proc) but
     * NO actual rest absorption — for lambda arity it must be strict.
     * We allocate rest_slot anyway (so autosplat / proc-style absorb
     * still work), and use this flag to make lambda's arity check
     * treat the block as if rest_slot < 0. */
    bool implicit_rest;
    /* Set at parse time when the body contains a `proc { }`/lambda/`->()`
     * literal.  Yield uses this to switch to a fresh-env-with-writeback
     * path so each iteration captures its own block-local slots —
     * without the flag, every iteration's captured proc would alias
     * the same env memory and see the last iter's values. */
    bool creates_proc;
    /* Lexical class nesting captured at proc creation time so constant
     * lookups inside the body resolve in the enclosing class scope
     * (CRuby semantics: `class C; X = 1; proc { X }; end` finds C::X). */
    struct korb_cref *cref;
    /* The enclosing method's korb_frame pointer at proc creation time.
     * Non-local `return` from inside a non-lambda proc/block targets
     * this frame.  Stored as a void* — the frame may be popped by the
     * time we read it; we only ever compare it to the live frame's
     * address (the popped frame's address won't match anything live). */
    void *return_target_frame;
    /* `super` inside a block dispatches to the LEXICALLY enclosing
     * method's super (CRuby semantics).  Capture the method seen at
     * proc creation time; super inside the block reads this instead
     * of c->current_frame->method. */
    struct korb_method *defining_method;
    /* The block lexically enclosing this one — i.e. running_block at
     * the moment this proc was created.  Used by Binding (and any
     * lvar-walk in the future) to traverse the lexical chain.  NULL
     * when this block was created outside any block. */
    struct korb_proc *lexical_parent_block;
};

/* Method object: a bound (receiver, method) pair, callable via #call/#[] */
struct korb_method_obj {
    struct RBasic basic;
    VALUE receiver;
    ID name;
    /* Captured method record: when `Module#instance_method(:foo)` /
     * `Object#method(:foo)` is called we resolve the method right then
     * and freeze the result, so subsequent `define_method`-based
     * rebinding doesn't make `m.call` re-dispatch through the new
     * definition (which would loop forever if the new body calls back
     * through the captured method, see test_super_in_module_unbound_method).
     * NULL means "use the receiver's current binding" (legacy path,
     * for code paths that haven't been migrated). */
    struct korb_method *captured_method;
    /* Class that owned the resolved method at capture time — needed by
     * `super` walks inside the captured body. */
    struct korb_class *captured_owner;
};

/* Binding — captures a frame's lexical state at the point `binding` is
 * called.  fp points into the live caller frame's slot area; while the
 * caller is alive, gets/sets read & write the frame directly so changes
 * are bidirectional (CRuby's heap-promoted env semantics).  Once the
 * caller returns, fp becomes stale — we don't track that, but typical
 * use is bind-then-eval-then-discard.
 *
 * names[i] is the lvar name at fp[base + i].  Initially populated from
 * the caller's local_names; binding.local_variable_set or eval can
 * append new names (their slots land in the temp area past caller's
 * locals_cnt — best-effort, may collide with caller's transient temps).
 */
struct korb_binding {
    struct RBasic basic;
    VALUE *fp;            /* heap snapshot — primary storage for slot 0..names_cnt-1 */
    uint32_t base;        /* offset from fp to slot 0 (always 0 with heap snapshot) */
    ID *names;            /* dynamic array; names[i] at fp[base + i] */
    uint32_t names_cnt;
    uint32_t names_capa;
    /* Live-frame write-through: if the caller's frame is still alive
     * at the time of get/set, mirror reads/writes there too so the
     * outer method sees binding-introduced changes (CRuby compat). */
    VALUE *live_fp;       /* original caller fp at binding-creation time */
    uint32_t live_base;   /* original base */
    uint64_t live_frame_id;  /* frame_id of the live frame; 0 if N/A */
    VALUE self;
    struct korb_cref *cref;
    /* Frame ID + method_name for source_location / __method__. */
    ID method_name;
    /* Source file/line of the `binding` call site (for source_location). */
    const char *source_file;
    int source_line;
    /* For new-local extension via local_variable_set / eval: when fp
     * may not have free slots, we fall back to this Hash. */
    VALUE extra_vars;
    /* Lexical-parent storage: lvars from outer block / method scopes
     * that are visible to the binding but live in their own frames.
     * Stored separately from extra_vars because extras (set-introduced)
     * appear at the FRONT of local_variables, while these outer names
     * appear at the END (CRuby orders innermost-first). */
    VALUE outer_vars;
    /* Number of names at the END of names[] that are lexical-parent
     * (outer) entries.  names[0..names_cnt-outer_names_cnt) are
     * primary (scope_locals); names[names_cnt-outer_names_cnt..]
     * are outer parent names. */
    uint32_t outer_names_cnt;
    /* Linkage for the per-frame "bindings created here" list.  At
     * frame epilogue we walk this list and snapshot the live fp into
     * each binding's heap so the binding survives the frame return. */
    struct korb_binding *next_in_frame;
};

/* Run at method epilogue: snapshot fp slots into each registered
 * binding so they hold final values rather than the moment-of-take
 * snapshot.  Called from prologue_ast_*_inl just before c->current_frame->fp is
 * restored to the previous frame. */
void korb_binding_snapshot_frame(struct korb_frame *f);

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
    struct korb_class *binding_class;

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
void korb_class_set_method_param_holder_slots(struct korb_class *klass, ID name, int *slots);
void korb_class_add_method_ast_full_cref(struct korb_class *klass, ID name, struct Node *body,
                                          uint32_t required_params, uint32_t total_params,
                                          int rest_slot, uint32_t locals_cnt,
                                          struct korb_cref *def_cref);
struct korb_cref *korb_cref_dup(struct korb_cref *src);
void korb_class_add_method_cfunc(struct korb_class *klass, ID name, VALUE (*func)(CTX *, VALUE, int, VALUE *), int argc);
void korb_class_set_method_block_slot(struct korb_class *klass, ID name, int slot);
void korb_class_set_method_local_names(struct korb_class *klass, ID name, ID *names);
void korb_register_body_local_names(struct Node *body, ID *names);
ID *korb_body_local_names(struct Node *body);
void korb_register_body_param_holder_slots(struct Node *body, int *slots);
int *korb_body_param_holder_slots(struct Node *body);
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
/* Walks include / super chain (CRuby `Sub::CONST` semantics). */
VALUE korb_const_get_inherited(struct korb_class *klass, ID name);
VALUE korb_const_get_inherited_stop_at_object(struct korb_class *klass, ID name);
bool  korb_const_has_inherited(struct korb_class *klass, ID name);
bool korb_const_has(struct korb_class *klass, ID name);

/* objects */
VALUE korb_object_new(struct korb_class *klass);
VALUE korb_ivar_get(VALUE obj, ID name);
void  korb_ivar_set(VALUE obj, ID name, VALUE value);
bool  korb_ivar_defined(VALUE obj, ID name);
VALUE korb_ivar_get_ic_slow(VALUE obj, ID name, struct ivar_cache *cache);
void  korb_ivar_set_ic_slow(VALUE obj, ID name, VALUE val, struct ivar_cache *cache);

/* Fast inline ivar getter — caches (klass, slot) on the AST node.  Cache
 * miss + non-T_OBJECT goes through the out-of-line slow path. */
static inline __attribute__((always_inline)) VALUE
korb_ivar_get_ic(VALUE obj, ID name, struct ivar_cache *cache) {
    if (UNLIKELY(SPECIAL_CONST_P(obj))) return Qnil;
    if (UNLIKELY(BUILTIN_TYPE(obj) != T_OBJECT)) return korb_ivar_get_ic_slow(obj, name, cache);
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
extern void korb_raise_frozen_modification(VALUE obj);
static inline __attribute__((always_inline)) void
korb_ivar_set_ic(VALUE obj, ID name, VALUE val, struct ivar_cache *cache) {
    if (UNLIKELY(SPECIAL_CONST_P(obj))) {
        /* true / false / nil / Integer / Float / Symbol can't have
         * ivars — CRuby raises FrozenError on attempts. */
        korb_raise_frozen_modification(obj);
        return;
    }
    if (UNLIKELY(BUILTIN_TYPE(obj) != T_OBJECT)) {
        korb_ivar_set_ic_slow(obj, name, val, cache);
        return;
    }
    if (UNLIKELY(((struct RBasic *)obj)->head.flags & FL_FROZEN)) {
        korb_raise_frozen_modification(obj);
        return;
    }
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
VALUE korb_dbl2int(double v);
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
void  korb_raise_type_error(CTX *c, const char *fmt, ...);
void  korb_raise_argument_error(CTX *c, const char *fmt, ...);
void  korb_raise_range_error(CTX *c, const char *fmt, ...);
void  korb_raise_index_error(CTX *c, const char *fmt, ...);
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
/* Array#<< has its own redef flag so an Array redef doesn't trip the
 * Integer/Float arithmetic fast paths (and vice versa).  Only the
 * Array fast path in node_lshift consults this flag. */
extern bool korb_g_array_op_redefined;
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

/* FL_KWARGS used by prologue inline functions below — must be visible
 * before "prologues.h".  Real definition in terms of FL_USER is below;
 * we forward-define the bit value here. */
#define FL_USER_SHIFT 12
#define FL_USER(n)    ((uint16_t)((uint16_t)1 << (FL_USER_SHIFT + (n))))
#define FL_SINGLETON FL_USER(0)
#define FL_KWARGS    FL_USER(1)
/* FL_CHILLED: a String literal that's "chilled" — frozen by virtue of
 * being a literal but transparently unfrozen on first mutation
 * (CRuby 3.4+ default).  `frozen?` returns false; `+@` returns a
 * fresh mutable copy. */
#define FL_CHILLED   FL_USER(2)
/* FL_HAS_PROC_IVARS: set on a korb_class when any instance of that
 * class has ever stored a Proc-typed ivar.  Used by Class#new's
 * post-initialize bookkeeping to skip the per-ivar walk that detaches
 * proc envs pointing into the dying frame.  The walk is a no-op for
 * classes that never receive proc ivars (most of optcarrot's hot
 * classes), and the flag eliminates a 3% overhead on Class.new. */
#define FL_HAS_PROC_IVARS FL_USER(3)

/* Inline cache-hit fast path for method dispatch.  On cache hit (LIKELY),
 * directly call mc->prologue — no function call into the slower path.
 * Cache miss falls through to korb_dispatch_call which fills mc and
 * dispatches.
 *
 * Guarded direct call: compare mc->prologue to the hottest variants and
 * dispatch via the inline body when matched.  prologues.h provides the
 * inline implementations so each TU gets its own copy and gcc can fully
 * inline the prologue body into the SD that includes us. */

/* korb_proc_snapshot_env_if_in_frame is declared in prologues.h but
 * must also be visible as a name here, before prologues.h itself uses
 * the inline gate `korb_proc_snapshot_env_maybe` defined just below. */
void korb_proc_snapshot_env_if_in_frame(VALUE v, VALUE *fp_lo, VALUE *fp_hi);

/* Inline gate: skip the CALL to korb_proc_snapshot_env_if_in_frame
 * when it would obviously be a no-op.  The function call itself was
 * 3% of optcarrot's runtime even for the early-return case.  Three
 * tiers of fast discriminator handled inline:
 *   1. SPECIAL_CONST_P (Fixnum / Float / Symbol / nil / true / false)
 *      — return immediately.
 *   2. T_OBJECT with class missing FL_HAS_PROC_IVARS — return
 *      immediately.  The flag is lifted lazily on the first proc-ivar
 *      assignment in korb_ivar_set / korb_ivar_set_ic_slow.
 *   3. T_PROC / T_OBJECT-with-flag / T_CLASS / T_MODULE — call the
 *      full function.  Other heap types (T_ARRAY, T_STRING, T_HASH,
 *      T_RANGE, T_BIGNUM, T_FLOAT) also short-circuit here since
 *      their layout has no proc fields. */
static inline __attribute__((always_inline)) void
korb_proc_snapshot_env_maybe(VALUE v, VALUE *fp_lo, VALUE *fp_hi) {
    if (SPECIAL_CONST_P(v)) return;
    enum korb_type t = BUILTIN_TYPE(v);
    if (LIKELY(t == T_OBJECT)) {
        struct korb_class *k =
            (struct korb_class *)((struct RBasic *)v)->klass;
        if (LIKELY(k && !(k->basic.head.flags & FL_HAS_PROC_IVARS))) return;
        korb_proc_snapshot_env_if_in_frame(v, fp_lo, fp_hi);
        return;
    }
    if (LIKELY(t != T_PROC && t != T_CLASS && t != T_MODULE)) return;
    korb_proc_snapshot_env_if_in_frame(v, fp_lo, fp_hi);
}

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
         * (recv == c->current_frame->self).  Protected methods need the caller's class
         * to include the target's class in its hierarchy. */
        if (UNLIKELY(mc->method && mc->method->visibility != KORB_VIS_PUBLIC)) {
            if (mc->method->visibility == KORB_VIS_PRIVATE && recv != c->current_frame->self) {
                return korb_dispatch_visibility_raise(c, mc->method, name, klass, recv);
            }
            if (mc->method->visibility == KORB_VIS_PROTECTED) {
                struct korb_class *caller_klass = korb_class_of_class(c->current_frame->self);
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

/* The block/proc/lambda whose body is currently executing.  Updated by
 * korb_yield (set to blk) and proc_call (set to p), restored on return.
 * Used by node_return to determine whether `return` is non-local
 * (running_block != NULL && !is_lambda → target enclosing method) or
 * local (lambda or method body). */
extern struct korb_proc *running_block;

/* Fast path: hot in `ary.each { |x| ... }` style code (Array#each,
 * Hash#each, etc.) — argc and params_cnt are usually 1, no
 * auto-destructure, no need to copy more than 1 arg.  Inlined into
 * builtins.c iterators (ary_each etc.) so the cross-.so dispatcher
 * call disappears. */
static inline __attribute__((always_inline)) VALUE
korb_yield(CTX *c, uint32_t argc, VALUE *argv) {
    struct korb_proc *blk = current_block;
    if (UNLIKELY(!blk)) {
        VALUE eLJE = korb_const_get(korb_vm->object_class, korb_intern("LocalJumpError"));
        korb_raise(c, (struct korb_class *)eLJE, "no block given (yield)");
        return Qnil;
    }
    /* Symbol-proc shim — fall to slow path. */
    if (UNLIKELY(blk->body == NULL)) return korb_yield_slow(c, blk, argc, argv);
    /* Block creates a Proc inside its body — needs per-iteration env
     * (via the slow path's fresh-env-with-writeback) so each captured
     * proc has its own block-locals. */
    if (UNLIKELY(blk->creates_proc)) return korb_yield_slow(c, blk, argc, argv);
    /* Common case: single arg, single param, no destructure.  Inline.
     * Skip when post params or rest are present — those need destructure. */
    if (LIKELY(argc == 1 && blk->params_cnt == 1 && blk->post_cnt == 0 &&
               blk->rest_slot < 0 && blk->kwh_save_slot < 0)) {
        VALUE arg = argv[0];  /* snapshot before fp swap */
        VALUE *prev_fp = c->current_frame->fp;
        VALUE prev_self = c->current_frame->self;
        struct korb_cref *prev_cref = c->current_frame->cref;
        struct korb_proc *prev_block = current_block;
        VALUE *bfp = blk->env;
        bfp[blk->param_base] = arg;
        c->current_frame->self = blk->self;
        c->current_frame->fp = bfp;
        if (blk->cref) c->current_frame->cref = blk->cref;
        /* Lexical block target: yield inside this block goes to the
         * enclosing method's block, not back to this block itself. */
        current_block = blk->enclosing_block;
        struct korb_proc *prev_running = running_block;
        running_block = blk;
        VALUE r;
    redo_yield:
        /* sp = bfp + env_size matches the bake walker's sp_offset
         * convention for the block body (lvar_set/lvar_get inside the
         * block are baked relative to env_size, just as method-body
         * dispatches pass fp + locals_cnt). */
        r = EVAL(c, blk->body, bfp + blk->env_size);
        if (UNLIKELY(c->state == KORB_REDO)) {
            c->state = KORB_NORMAL; c->state_value = Qnil;
            goto redo_yield;
        }
        c->current_frame->fp = prev_fp;
        c->current_frame->self = prev_self;
        c->current_frame->cref = prev_cref;
        current_block = prev_block;
        running_block = prev_running;
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
bool  korb_gvar_defined(ID name);

/* $_ / $~ — method-scoped pseudo-globals (read/write the current frame's
 * last_line / last_match slot, falling back to the global table at
 * top-level).  Use these instead of korb_gvar_{get,set} for $_ and $~. */
VALUE korb_last_line_get(CTX *c);
void  korb_last_line_set(CTX *c, VALUE v);
VALUE korb_last_match_get(CTX *c);
void  korb_last_match_set(CTX *c, VALUE v);

/* const lookup along current scope (uses CTX->current_class) */
VALUE korb_const_lookup(CTX *c, ID name);

/* range */
VALUE korb_range_new(VALUE begin, VALUE end, bool exclude_end);

/* proc */
VALUE korb_proc_new(struct Node *body, VALUE *fp, uint32_t env_size, uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda);
VALUE korb_proc_new_with_cref(struct Node *body, VALUE *fp, uint32_t env_size, uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda, struct korb_cref *cref);
/* korb_proc_snapshot_env_if_in_frame and the inline gate
 * `korb_proc_snapshot_env_maybe` are declared above (before the
 * #include "prologues.h" block) so the inlined prologues can use them. */

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

/* object FLAGS — definitions hoisted above "prologues.h" include so
 * the inline prologues can read FL_KWARGS.  See the block before that
 * include for FL_USER_SHIFT / FL_USER / FL_SINGLETON / FL_KWARGS. */

/* Frozen-object guard.  Inserted at the entry of mutating cfuncs so
 * `frozen_str << "x"` etc. raise FrozenError instead of silently
 * mutating.  Skipped for immediates (Fixnum / Symbol / nil/true/false
 * are inherently frozen but we don't track FL_FROZEN on them and
 * none of the cfuncs that call this take them as `self` anyway). */
static inline bool korb_obj_frozen_p(VALUE v) {
    if (SPECIAL_CONST_P(v)) return true;
    return (RBASIC(v)->head.flags & FL_FROZEN) != 0;
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
