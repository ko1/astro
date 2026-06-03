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

/* Array = handle (this struct, fixed-size, address-stable) + a separate
 * backing payload object (T_ARY_BACKING) holding the VALUE elements.
 * `backing` is an ordinary heap VALUE reference (GC forwards it like any
 * edge).  `len` (logical length) lives on the handle; capacity is derived
 * from the backing's header gc_size.  See docs/array_payload_value.md. */
struct korb_array {
    struct RBasic basic;
    VALUE backing;   /* T_ARY_BACKING object: header + VALUE items[capa] */
    long  len;       /* logical length (handle-owned; <= capa) */
};

/* Backing payload object: AroObjectHeader head + VALUE items[capa] inline.
 * capa is derived from head.gc_size.  Not user-visible. */
struct korb_ary_backing {
    struct RBasic basic;   /* head + (unused for backing) klass slot */
    VALUE items[];         /* flexible: capa = (gc_size - offsetof(items))/8 */
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
    /* GC generation at which an identity hash was last rehashed.  Identity
     * hashing keys on the raw (moving) address, so after a moving GC forwards
     * the keys, each entry's cached hash + bucket placement go stale.  When
     * korb_g_gc_gen advances past this, the next access rehashes from the
     * forwarded keys.  Unused for non-identity hashes. */
    uint64_t identity_rehash_gen;
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
    /* GC visit generation: visit_method_table forwards each method's heap
     * edges (defining_class / def_cref->klass / proc) exactly once per GC
     * cycle by stamping this with korb_g_gc_gen.  Replaces the older
     * "depth-0 entries only" dedup, which silently skipped a method whose
     * only table appearances are at include_depth>=1 (e.g. a method defined
     * directly on a class that is reached only via an including class's
     * flattened table) — its def_cref->klass then never got forwarded and
     * went stale (Hash#to_h `instance_of?(Hash)` SEGV under STRESS+PURGE).
     * calloc-zeroed at allocation; korb_g_gc_gen is >=1 by first use. */
    uint64_t gc_visit_gen;
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
            /* New sp-based RESULT-returning cfunc — set when registered via
             * korb_class_add_method_cfunc_r.  When non-NULL, dispatch uses
             * prologue_cfunc_r_inl instead.  Phase 2-4 transition. */
            korb_cfunc_r_t func_r;
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
    /* The block lexically enclosing this one — i.e. c->running_block at
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

    /* Cached frozen singleton strings — CRuby semantics for true.to_s /
     * false.to_s / nil.to_s require identity-equal return value, so we
     * stash one frozen instance each on the vm and hand it out. */
    VALUE frozen_true_str;
    VALUE frozen_false_str;
    VALUE frozen_nil_str;

    /* The currently-executing CTX (set by main).  Used by
     * korb_hash_value to invoke user-defined #hash on custom-class
     * keys.  Single-threaded, so a single global is fine. */
    struct CTX_struct *current_ctx;

    /* Generic ivar side table — for heap types (T_STRING, T_ARRAY,
     * T_HASH, T_RANGE, ...) that don't have an ivars[] field in their
     * struct.  Keyed by obj pointer (cast to VALUE), value is a Hash
     * of (Symbol -> VALUE).  Lazily allocated on first ivar set.
     * Safe because those types are libc-allocated and don't move. */
    VALUE generic_ivars;
};

extern struct korb_vm *korb_vm;

/* ---------- API ---------- */

CTX *korb_runtime_init(void);

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
size_t korb_id_name_len(ID id);

/* class system */
VALUE korb_class_of(VALUE v);
struct korb_class *korb_class_of_class_slow(VALUE v); /* immediate fallbacks */
/* Hot path: heap T_OBJECT load.  Called per method dispatch.
 *
 * For libc-allocated heap objects (T_ARRAY / T_STRING / T_HASH /
 * T_RANGE / T_BIGNUM / T_FLOAT / T_PROC), basic.klass holds an
 * arena class pointer that is NOT auto-updated when GC moves the
 * class (= libc objs are not in any visit_roots / scan_edges chain).
 * For these, the canonical class lives on korb_vm and is GC-tracked,
 * so we redirect by type rather than read the stale field.
 *
 * For T_OBJECT, basic.klass is the user-defined class which is
 * arena-allocated.  We fall back to reading it directly; user-class
 * instances under STRESS are a separate, harder problem (= the
 * libc-obj klass-update gap remains for them). */
static inline __attribute__((always_inline)) struct korb_class *
korb_class_of_class(VALUE v) {
    if (LIKELY(!SPECIAL_CONST_P(v))) {
        struct RBasic *b = (struct RBasic *)v;
        if (korb_vm) {
            int t = (int)(b->head.flags & T_MASK);
            /* For container types, basic.klass may be a user subclass
             * (e.g. `class MyHash < Hash; end` then MyHash.new sets
             * basic.klass = MyHash).  Honor it when it's set to anything
             * other than the canonical class.  Otherwise redirect to the
             * vm-tracked canonical class so libc-obj stale-klass issues
             * don't bite us. */
            switch (t) {
                case T_ARRAY: {
                    struct korb_class *k = (struct korb_class *)b->klass;
                    return (k && k != korb_vm->array_class) ? k : korb_vm->array_class;
                }
                case T_STRING: {
                    struct korb_class *k = (struct korb_class *)b->klass;
                    return (k && k != korb_vm->string_class) ? k : korb_vm->string_class;
                }
                case T_HASH: {
                    struct korb_class *k = (struct korb_class *)b->klass;
                    return (k && k != korb_vm->hash_class) ? k : korb_vm->hash_class;
                }
                case T_RANGE:  return korb_vm->range_class;
                case T_PROC:   return korb_vm->proc_class;
                case T_FLOAT:  return korb_vm->float_class;
                case T_BIGNUM: return korb_vm->integer_class;
                /* T_OBJECT / T_CLASS / T_MODULE fall through to
                 * basic.klass read. */
            }
        }
        return (struct korb_class *)b->klass;
    }
    return korb_class_of_class_slow(v);
}
struct korb_class *korb_class_new(CTX *c, VALUE *sp, ID name, struct korb_class *super, enum korb_type instance_type);
struct korb_class *korb_module_new(CTX *c, VALUE *sp, ID name);
void korb_class_add_method_ast(CTX *c, struct korb_class *klass, ID name, struct Node *body, uint32_t params_cnt, uint32_t locals_cnt);
void korb_class_add_method_ast_full(CTX *c, struct korb_class *klass, ID name, struct Node *body,
                                    uint32_t required_params, uint32_t total_params,
                                    int rest_slot, uint32_t locals_cnt);
void korb_class_set_method_param_holder_slots(struct korb_class *klass, ID name, int *slots);
void korb_class_add_method_ast_full_cref(CTX *c, struct korb_class *klass, ID name, struct Node *body,
                                          uint32_t required_params, uint32_t total_params,
                                          int rest_slot, uint32_t locals_cnt,
                                          struct korb_cref *def_cref);
struct korb_cref *korb_cref_dup(struct korb_cref *src);

/* Resolve the "host class" for a top-level lookup / def / const set.
 * Walks the cref chain skipping NULL klass entries (= forwarded to NULL
 * by GC's stale-to-space branch in PURGE mode), then falls back to
 * frame.current_class and finally Object.  Guaranteed non-NULL.
 *
 * Without this helper, callers like `cref ? cref->klass : current_class`
 * SEGV when forward_payload NULL'd the top cref's klass field. */
static inline __attribute__((always_inline)) struct korb_class *
korb_host_class(CTX * restrict c) {
    /* Skip NULL / special-const (Qnil = 0x8 from a clobbered cref) klass and
     * stop at a special-const prev: a corrupted cref chain otherwise returns
     * 0x8 as a "class", which the caller deref's (k->super) and SEGVs. */
    for (struct korb_cref *cr = c->current_frame->cref;
         cr && !SPECIAL_CONST_P((VALUE)cr); cr = cr->prev) {
        if (cr->klass && !SPECIAL_CONST_P((VALUE)cr->klass)) return cr->klass;
    }
    if (c->current_frame->current_class) return c->current_frame->current_class;
    return korb_vm->object_class;
}
void korb_class_add_method_cfunc(struct korb_class *klass, ID name, VALUE (*func)(CTX *, VALUE, int, VALUE *), int argc);
/* Register a method whose body uses the new sp-based RESULT-returning
 * cfunc signature `(CTX *c, int argc, VALUE *sp) → RESULT`.  Dispatch
 * picks prologue_cfunc_r_inl when mc->cfunc_r is non-NULL. */
void korb_class_add_method_cfunc_r(struct korb_class *klass, ID name, korb_cfunc_r_t func_r, int argc);
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
struct korb_class *korb_singleton_class_of(CTX *c, VALUE *sp, struct korb_class *klass);
struct korb_class *korb_singleton_class_of_value(CTX *c, VALUE *sp, VALUE v);

/* constants */
void korb_const_set(struct korb_class *klass, ID name, VALUE value);
VALUE korb_const_get(struct korb_class *klass, ID name);
/* Walks include / super chain (CRuby `Sub::CONST` semantics). */
VALUE korb_const_get_inherited(struct korb_class *klass, ID name);
VALUE korb_const_get_inherited_stop_at_object(struct korb_class *klass, ID name);
bool  korb_const_has_inherited(struct korb_class *klass, ID name);
bool korb_const_has(struct korb_class *klass, ID name);

/* objects */
VALUE korb_object_new(CTX *c, VALUE *sp, struct korb_class *klass);
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
    if (LIKELY(cache->gen == korb_g_gc_gen &&
               cache->klass == (struct korb_class *)o->basic.klass && cache->slot >= 0)) {
        uint32_t s = (uint32_t)cache->slot;
        if (LIKELY(s < o->ivar_cnt)) return o->ivars[s];
        return Qnil;
    }
    return korb_ivar_get_ic_slow(obj, name, cache);
}

/* Fast inline ivar setter — same monomorphic cache pattern.  Cache miss
 * (different klass / unset slot / first write past current capa) goes
 * through the slow path which handles growth + slot assignment. */
extern RESULT_FN RESULT korb_raise_frozen_modification(CTX *c, VALUE obj);
static inline __attribute__((always_inline)) RESULT
korb_ivar_set_ic(CTX *c, VALUE obj, ID name, VALUE val, struct ivar_cache *cache) {
    if (UNLIKELY(SPECIAL_CONST_P(obj))) {
        /* true / false / nil / Integer / Float / Symbol can't have
         * ivars — CRuby raises FrozenError on attempts. */
        return korb_raise_frozen_modification(c, obj);
    }
    if (UNLIKELY(BUILTIN_TYPE(obj) != T_OBJECT)) {
        korb_ivar_set_ic_slow(obj, name, val, cache);
        return RESULT_OK(Qnil);
    }
    if (UNLIKELY(((struct RBasic *)obj)->head.flags & FL_FROZEN)) {
        return korb_raise_frozen_modification(c, obj);
    }
    struct korb_object *o = (struct korb_object *)obj;
    if (LIKELY(cache->gen == korb_g_gc_gen &&
               cache->klass == (struct korb_class *)o->basic.klass && cache->slot >= 0)) {
        uint32_t s = (uint32_t)cache->slot;
        if (LIKELY(s < o->ivar_cnt)) {
            o->ivars[s] = val;
            return RESULT_OK(Qnil);
        }
    }
    korb_ivar_set_ic_slow(obj, name, val, cache);
    return RESULT_OK(Qnil);
}

/* string */
VALUE korb_str_new(CTX *c, VALUE *sp, const char *p, long len);
VALUE korb_str_new_cstr(CTX *c, VALUE *sp, const char *cstr);
VALUE korb_str_dup(CTX *c, VALUE *sp, VALUE s);
VALUE korb_str_concat(CTX *c, VALUE *sp, VALUE a, VALUE b);
VALUE korb_str_inspect(VALUE s);
const char *korb_str_cstr(VALUE s); /* terminates */
long  korb_str_len(VALUE s);

/* array */
VALUE korb_ary_new_capa(CTX *c, VALUE *sp, long capa);
VALUE korb_ary_new(CTX *c, VALUE *sp);
VALUE korb_ary_new_from_values(CTX *c, VALUE *sp, long n, const VALUE *vals);
/* push/aset implementations: ary at sp[-2], v at sp[-1].  The inline
 * wrappers below park ary/v into the caller-provided sp staging slots and
 * call the _sp body with sp+2 as the new staging base — so a grow's inner
 * alloc forwards ary/v (they sit just below sp+2) without the caller ever
 * touching c->sp_top.  See docs/array_payload_value.md. */
void  korb_ary_push_sp(CTX *c, VALUE *sp);
void  korb_ary_aset_sp(CTX *c, VALUE *sp, long i);
VALUE korb_ary_pop(VALUE ary);

/* korb_ary_push(c, sp, ary, v): sp = free value-stack top with >=2 scratch
 * slots.  Parks ary/v, then runs the body staging at sp+2. */
static inline void
korb_ary_push(CTX *c, VALUE *sp, VALUE ary, VALUE v) {
    sp[0] = ary;
    sp[1] = v;
    korb_ary_push_sp(c, sp + 2);
}
static inline void
korb_ary_aset(CTX *c, VALUE *sp, VALUE ary, long i, VALUE v) {
    sp[0] = ary;
    sp[1] = v;
    korb_ary_aset_sp(c, sp + 2, i);
}

/* Array payload accessors (payload-as-VALUE design, docs/array_payload_value.md).
 * The VALUE elements live in a separate T_ARY_BACKING object referenced by
 * a->backing; the inline items[] starts right after that object's header.
 *
 * korb_ary_items(a) returns the writeable VALUE* base of the elements.  It
 * is the single chokepoint replacing the old `a->ptr` field: any code that
 * read `a->ptr[i]` now reads `korb_ary_items(a)[i]`.  The pointer is only
 * valid until the next GC point (the backing may move) — callers that span
 * a GC point must re-derive it (same discipline the libc-malloc ptr never
 * needed, but moving payloads do). */
static inline __attribute__((always_inline)) VALUE *
korb_ary_items(const struct korb_array *a) {
    return a->backing ? ((struct korb_ary_backing *)a->backing)->items : NULL;
}
/* Capacity is derived from the backing object's header gc_size. */
static inline __attribute__((always_inline)) long
korb_ary_capa(const struct korb_array *a) {
    if (!a->backing) return 0;
    uint32_t sz = ((struct RBasic *)a->backing)->head.gc_size;
    return (long)((sz - offsetof(struct korb_ary_backing, items)) / sizeof(VALUE));
}

/* korb_ary_aref / korb_ary_len: inlined into SDs.  Hot in optcarrot
 * (`@output_color[pixel]`, `sprite[2]`, etc.). */
static inline __attribute__((always_inline)) VALUE
korb_ary_aref(VALUE av, long i) {
    struct korb_array *a = (struct korb_array *)av;
    if (i < 0) i += a->len;
    if ((unsigned long)i >= (unsigned long)a->len) return Qnil;
    return korb_ary_items(a)[i];
}
static inline __attribute__((always_inline)) long
korb_ary_len(VALUE av) {
    return ((struct korb_array *)av)->len;
}

/* hash */
VALUE korb_hash_new(CTX *c, VALUE *sp);
VALUE korb_hash_aref_slow(CTX *c, VALUE h, VALUE key);
/* Rehash an identity (compare_by_identity) hash if a moving GC has run since
 * its last rehash — its keys are hashed by raw address, which the GC changes.
 * Call before any bucket lookup/insert on a hash that may be identity-based. */
void korb_hash_rehash_identity_if_stale(struct korb_hash *h);

/* korb_hash_aref: inlined fast path for FIXNUM / SYMBOL keys (the
 * common case in optcarrot's @sp_map[@hclk]).  Strings and
 * compare_by_identity tables go through korb_hash_aref_slow.
 * bucket_cnt is always a power of 2 (init=8, resize doubles), so
 * `& (bucket_cnt-1)` replaces modulo. */
static inline __attribute__((always_inline)) VALUE
korb_hash_aref(CTX *c, VALUE hv, VALUE key) {
    struct korb_hash *h = (struct korb_hash *)hv;
    if (UNLIKELY(h->compare_by_identity)) return korb_hash_aref_slow(c, hv, key);
    uint64_t hh;
    if (LIKELY(FIXNUM_P(key))) {
        hh = (uint64_t)key * 11400714819323198485ULL;
    } else if (SYMBOL_P(key)) {
        hh = (uint64_t)key * 2654435761ULL;
    } else {
        return korb_hash_aref_slow(c, hv, key);
    }
    uint32_t b = (uint32_t)hh & (h->bucket_cnt - 1);
    for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
        if (e->hash == hh && e->key == key) return e->value;
    }
    return h->default_value;
}
VALUE korb_hash_aset(CTX *c, VALUE h, VALUE key, VALUE val);
long  korb_hash_size(VALUE h);

/* symbol */
VALUE korb_id2sym(ID id);
ID    korb_sym2id(VALUE sym);
VALUE korb_str_to_sym(VALUE str);

/* float / bignum */
/* korb_float_new: try FLONUM-encode (fast inline; no alloc, so the c/sp
 * args are unused on the fast path).  Heap-allocate via out-of-line slow
 * path otherwise — sync c->sp_top = sp there. */
VALUE korb_float_new_heap(CTX *c, VALUE *sp, double d);
static inline __attribute__((always_inline)) VALUE
korb_float_new(CTX *c, VALUE *sp, double d) {
    VALUE flo = korb_double_to_flonum(d);
    if (LIKELY(flo)) return flo;
    return korb_float_new_heap(c, sp, d);
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
VALUE korb_bignum_new_str(CTX *c, VALUE *sp, const char *str, int base);
VALUE korb_bignum_new_long(CTX *c, VALUE *sp, long v);
VALUE korb_dbl2int(CTX *c, VALUE *sp, double v);
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
VALUE korb_int_not(VALUE a);
int   korb_int_cmp(VALUE a, VALUE b);
bool  korb_int_eq(VALUE a, VALUE b);

/* equality / inspect */
bool  korb_eq(CTX *c, VALUE a, VALUE b);
bool  korb_eql(CTX *c, VALUE a, VALUE b);
uint64_t korb_hash_value(CTX *c, VALUE v);
VALUE korb_inspect(CTX *c, VALUE *sp, VALUE v);
VALUE korb_inspect_dispatch(CTX *c, VALUE v);
VALUE korb_to_s(CTX *c, VALUE *sp, VALUE v);
VALUE korb_to_s_dispatch(CTX *c, VALUE v);
void  korb_p(CTX *c, VALUE v); /* writes to stdout with newline */

/* errors / exceptions */
VALUE korb_exc_new(CTX *c, VALUE *sp, struct korb_class *klass, const char *msg);
RESULT_FN RESULT korb_raise(CTX *c, struct korb_class *klass, const char *fmt, ...);
RESULT_FN RESULT korb_raise_type_error(CTX *c, const char *fmt, ...);
RESULT_FN RESULT korb_raise_argument_error(CTX *c, const char *fmt, ...);
RESULT_FN RESULT korb_raise_range_error(CTX *c, const char *fmt, ...);
RESULT_FN RESULT korb_raise_index_error(CTX *c, const char *fmt, ...);
VALUE korb_build_backtrace(CTX *c, int raise_line);
void  korb_exc_set_backtrace(CTX *c, VALUE *sp, VALUE exc, int raise_line);

/* method dispatch helper */
RESULT korb_funcall(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv);
RESULT korb_funcall_with_block(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv, VALUE block);
RESULT korb_dispatch_call(CTX *c, struct Node *callsite, VALUE recv, ID name, uint32_t argc, uint32_t arg_index, struct korb_proc *block, struct method_cache *mc);

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
RESULT prologue_ast_simple_0(CTX *c, struct Node *callsite, VALUE recv,
                             uint32_t argc, uint32_t arg_index,
                             struct korb_proc *block, struct method_cache *mc);
RESULT prologue_ast_simple_1(CTX *c, struct Node *callsite, VALUE recv,
                             uint32_t argc, uint32_t arg_index,
                             struct korb_proc *block, struct method_cache *mc);
RESULT prologue_ast_simple_2(CTX *c, struct Node *callsite, VALUE recv,
                             uint32_t argc, uint32_t arg_index,
                             struct korb_proc *block, struct method_cache *mc);
RESULT prologue_ast_simple_3(CTX *c, struct Node *callsite, VALUE recv,
                             uint32_t argc, uint32_t arg_index,
                             struct korb_proc *block, struct method_cache *mc);
RESULT prologue_cfunc(CTX *c, struct Node *callsite, VALUE recv,
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
extern RESULT korb_dispatch_visibility_raise(CTX *c, struct korb_method *m,
                                             ID name, struct korb_class *klass,
                                             VALUE recv);

static inline __attribute__((always_inline)) RESULT
korb_dispatch_call_cached(CTX * restrict c, struct Node * restrict callsite,
                          VALUE recv, ID name, uint32_t argc,
                          uint32_t arg_index, struct korb_proc *block,
                          struct method_cache *mc)
{
    struct korb_class *klass = korb_class_of_class(recv);
    if (LIKELY(mc && mc->serial == korb_g_method_serial && mc->gen == korb_g_gc_gen && mc->klass == klass)) {
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
        if (p == prologue_cfunc) {
            /* sp-based RESULT ABI: stage self + args at the top of the
             * value stack and call prologue_cfunc_r_inl.  c->sp_top is NOT
             * touched here — the cfunc itself syncs `c->sp_top = sp` just
             * before any alloc (see runtime.md §12.3).  All cfuncs use
             * the func_r ABI now; legacy prologue_cfunc_inl is gone. */
            VALUE *sp = c->sp_top;
            sp[0] = recv;
            for (uint32_t i = 0; i < argc; i++) sp[1 + i] = c->current_frame->fp[arg_index + i];
            return prologue_cfunc_r_inl(c, callsite, (int)argc, sp + 1 + argc, block, mc);
        }
        return p(c, callsite, recv, argc, arg_index, block, mc);
    }
    return korb_dispatch_call(c, callsite, recv, name, argc, arg_index, block, mc);
}
RESULT korb_dispatch_binop(CTX *c, VALUE recv, ID name, int argc, VALUE *argv);

/* Cold tails for fast-path NODEs.  Bodies live in object.c and are
 * called via PLT/GOT from each SD.so, instead of being inlined into
 * every SD that uses node_plus / node_aref / etc.  Trades a tiny
 * extra call (only on the slow path) for a substantially smaller
 * all.so and lower compile time. */
RESULT korb_node_plus_slow  (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_minus_slow (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_mul_slow   (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_div_slow   (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_mod_slow   (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_uminus_slow(CTX *c, VALUE v);
RESULT korb_node_band_slow  (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_bor_slow   (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_bxor_slow  (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_lshift_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_rshift_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_lt_slow    (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_le_slow    (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_gt_slow    (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_ge_slow    (CTX *c, VALUE l, VALUE r, uint32_t arg_index);
RESULT korb_node_aref_slow  (CTX *c, VALUE r, VALUE i, uint32_t arg_index);
RESULT korb_node_aset_slow  (CTX *c, VALUE r, VALUE i, VALUE v, uint32_t arg_index);

/* Cold tail of korb_yield: handles auto-destructure (block has N>1
 * params, called with single Array of size M), variable argc paths,
 * and the param/argc-mismatch slow case. */
RESULT korb_yield_slow(CTX *c, struct korb_proc *blk, uint32_t argc, VALUE *argv);

/* RESULT-returning bridges around korb_funcall / korb_funcall_with_block.
 * Used in new-ABI cfuncs / helpers (where UNWRAP propagates the state). */
RESULT_FN RESULT korb_funcall_r(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv);
/* korb_yield_r — defined as a static inline below near korb_yield. */
RESULT_FN RESULT korb_funcall_with_block_r(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv, VALUE block);



/* The block/proc/lambda whose body is currently executing.  Updated by
 * korb_yield (set to blk) and proc_call (set to p), restored on return.
 * Used by node_return to determine whether `return` is non-local
 * (c->running_block != NULL && !is_lambda → target enclosing method) or
 * local (lambda or method body). */


/* Fast path: hot in `ary.each { |x| ... }` style code (Array#each,
 * Hash#each, etc.) — argc and params_cnt are usually 1, no
 * auto-destructure, no need to copy more than 1 arg.  Inlined into
 * builtins.c iterators (ary_each etc.) so the cross-.so dispatcher
 * call disappears. */
static inline __attribute__((always_inline, warn_unused_result)) RESULT
korb_yield(CTX *c, uint32_t argc, VALUE *argv) {
    struct korb_proc *blk = c->current_block;
    if (UNLIKELY(!blk)) {
        VALUE eLJE = korb_const_get(korb_vm->object_class, korb_intern("LocalJumpError"));
        return korb_raise(c, (struct korb_class *)eLJE, "no block given (yield)");
    }
    /* Symbol-proc shim — fall to slow path. */
    if (UNLIKELY(blk->body == NULL)) return korb_yield_slow(c, blk, argc, argv);
    /* Block creates a Proc inside its body — needs per-iteration env
     * (via the slow path's fresh-env-with-writeback) so each captured
     * proc has its own block-locals. */
    if (UNLIKELY(blk->creates_proc)) return korb_yield_slow(c, blk, argc, argv);
    /* Method-overlaps-env: when the active method's frame sits ABOVE the
     * block's captured env (yielding method called from a deeper frame
     * than the one that created the block), running body at env aliases
     * the active method's locals.  Fall to slow path which has the
     * fresh-env clone fix.  See korb_yield_slow comment for details. */
    if (UNLIKELY(c->current_frame->fp && c->current_frame->fp > blk->env)) {
        return korb_yield_slow(c, blk, argc, argv);
    }
    /* Common case: single arg, single param, no destructure.  Inline.
     * Skip when post params or rest are present — those need destructure. */
    if (LIKELY(argc == 1 && blk->params_cnt == 1 && blk->post_cnt == 0 &&
               blk->rest_slot < 0 && blk->kwh_save_slot < 0)) {
        VALUE arg = argv[0];  /* snapshot before fp swap */
        VALUE *prev_fp = c->current_frame->fp;
        /* Save the enclosing self in a GC-walked chain (not a bare C-local):
         * the body's GC can move the enclosing-self object, and it's reachable
         * only via the frame slot we're about to overwrite, so a C-local would
         * go stale and we'd restore a dangling pointer. */
        struct korb_yield_self_save _ys = { .self = c->current_frame->self,
                                            .prev = c->yield_self_chain };
        c->yield_self_chain = &_ys;
        struct korb_cref *prev_cref = c->current_frame->cref;
        struct korb_proc *prev_block = c->current_block;
        VALUE *bfp = blk->env;
        bfp[blk->param_base] = arg;
        c->current_frame->self = blk->self;
        c->current_frame->fp = bfp;
        if (blk->cref) c->current_frame->cref = blk->cref;
        /* Lexical block target: yield inside this block goes to the
         * enclosing method's block, not back to this block itself. */
        c->current_block = blk->enclosing_block;
        struct korb_proc *prev_running = c->running_block;
        c->running_block = blk;
        RESULT _br;
    redo_yield:
        /* sp = bfp + env_size matches the bake walker's sp_offset
         * convention for the block body (lvar_set/lvar_get inside the
         * block are baked relative to env_size, just as method-body
         * dispatches pass fp + locals_cnt).  RESULT-native — body
         * state propagates via _br.state, no c->state inside. */
        _br = EVAL(c, blk->body, bfp + blk->env_size);
        if (UNLIKELY(_br.state == KORB_REDO)) {
            goto redo_yield;
        }
        c->current_frame->fp = prev_fp;
        c->current_frame->self = _ys.self;   /* forwarded across the body GC */
        c->yield_self_chain = _ys.prev;
        c->current_frame->cref = prev_cref;
        c->current_block = prev_block;
        c->running_block = prev_running;
        if (UNLIKELY(_br.state == KORB_NEXT)) return RESULT_OK(_br.value);
        if (UNLIKELY(_br.state != KORB_NORMAL)) return _br;
        return RESULT_OK(_br.value);
    }
    return korb_yield_slow(c, blk, argc, argv);
}

/* Legacy alias for korb_yield — kept for callers not yet migrated to
 * use korb_yield directly.  korb_yield is now RESULT-returning natively. */
static inline __attribute__((always_inline, warn_unused_result)) RESULT
korb_yield_r(CTX *c, uint32_t argc, VALUE *argv) {
    return korb_yield(c, argc, argv);
}

bool korb_block_given(CTX *c);

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
RESULT korb_const_lookup(CTX *c, ID name);

/* range */
VALUE korb_range_new(CTX *c, VALUE *sp, VALUE begin, VALUE end, bool exclude_end);

/* proc */
VALUE korb_proc_new(CTX *c, VALUE *sp, struct Node *body, VALUE *fp, uint32_t env_size, uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda);
VALUE korb_proc_new_with_cref(CTX *c, VALUE *sp, struct Node *body, VALUE *fp, uint32_t env_size, uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda, struct korb_cref *cref);
/* korb_proc_snapshot_env_if_in_frame and the inline gate
 * `korb_proc_snapshot_env_maybe` are declared above (before the
 * #include "prologues.h" block) so the inlined prologues can use them. */

/* Builtins init */
void korb_init_builtins(CTX *c);

/* Fiber */
struct korb_fiber;
VALUE korb_fiber_new(CTX *c, VALUE *sp, struct korb_proc *block);
RESULT korb_fiber_resume(CTX *c, VALUE fib, int argc, VALUE *argv);
RESULT korb_fiber_yield(CTX *c, int argc, VALUE *argv);

/* file load (parse + eval) */
RESULT korb_load_file(CTX *c, const char *path);
RESULT korb_eval_string(CTX *c, const char *src, size_t len, const char *filename);

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
/* RESULT-returning frozen check: short-circuits the enclosing RESULT function
 * with a raised FrozenError. */
#define CHECK_FROZEN_R(c, self) do { \
    if (UNLIKELY(korb_obj_frozen_p(self))) { \
        VALUE _eFrozen = korb_const_get(korb_vm->object_class, korb_intern("FrozenError")); \
        return korb_raise((c), (struct korb_class *)_eFrozen, "can't modify frozen object"); \
    } \
} while (0)

/* well-known IDs */
extern ID id_initialize, id_to_s, id_inspect, id_call, id_each, id_new;
extern ID id_op_plus, id_op_minus, id_op_mul, id_op_div, id_op_mod;
extern ID id_op_eq, id_op_neq, id_op_lt, id_op_le, id_op_gt, id_op_ge;
extern ID id_op_aref, id_op_aset, id_op_lshift, id_op_rshift, id_op_and, id_op_or, id_op_xor;

#endif /* KORUBY_OBJECT_H */
