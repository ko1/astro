#ifndef ABRUBY_CONTEXT_H
#define ABRUBY_CONTEXT_H 1

#include <ruby.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include "ab_id_table.h"

// VALUE is defined by ruby.h

#ifndef ABRUBY_DEBUG
#define ABRUBY_DEBUG 0
#endif

#ifndef ABRUBY_PROFILE
#define ABRUBY_PROFILE 0
#endif

#if ABRUBY_DEBUG
#define ABRUBY_ASSERT(expr) do { \
    if (!(expr)) { \
        rb_bug("ABRUBY_ASSERT failed: %s (%s:%d)", #expr, __FILE__, __LINE__); \
    } \
} while (0)
#else
#define ABRUBY_ASSERT(expr) ((void)0)
#endif


struct abruby_option {
    bool no_compiled_code;
    bool compiled_only;  // set dispatcher to NULL in ALLOC (crash if uncompiled node is called)
    bool record_all;
    bool verbose;
};

extern struct abruby_option OPTION;

// RESULT: two-value return type for non-local exit support.
// Fits in two registers (rax + rdx), no memory access needed.
// Partial evaluation eliminates state checks via constant propagation.
//
// State values are bit flags so that "catch state X and demote to NORMAL" at a
// boundary (method return catch, loop break catch) can be done with a single
// AND instruction instead of a branching check:
//
//   r.state &= ~RESULT_RETURN;  // at method boundary: RETURN -> NORMAL, others unchanged
//   r.state &= ~RESULT_BREAK;   // at while boundary:  BREAK  -> NORMAL, others unchanged
//
// NORMAL is 0 so the common "is state normal?" test is still a plain zero check.
//
// Non-local return from a block uses the high bits of `state` as a "skip
// count": how many method boundaries a RESULT_RETURN should cross before being
// caught.  `node_return` computes the skip count by walking from c->current_frame
// up to c->current_block->defining_frame (a rare, one-shot cost paid only at
// block-level `return`).  Every method boundary checks: if RETURN bit is set
// and skip count == 0 this is the true target and clears RETURN; otherwise
// decrement the skip count and keep propagating.  Normal (non-block) `return`
// has skip count 0 and is caught immediately at the nearest method boundary,
// which means non-block code pays zero per-frame cost for this mechanism.

// Low bits — exceptional state flags (bit-flag, can be OR'd):
#define RESULT_NORMAL 0u
#define RESULT_RETURN 1u  // bit 0
#define RESULT_RAISE  2u  // bit 1
#define RESULT_BREAK  4u  // bit 2
#define RESULT_NEXT   8u  // bit 3 — block `next` (caught by yield site)
// bits 4..15 reserved for future states.

// High bits — skip count (only meaningful when RESULT_RETURN bit is also set).
#define RESULT_SKIP_SHIFT 16u
#define RESULT_SKIP_UNIT  (1u << RESULT_SKIP_SHIFT)
#define RESULT_SKIP_MASK  (~((1u << RESULT_SKIP_SHIFT) - 1u))

// Mask used when catching a non-local return: clears the RETURN flag AND any
// leftover skip-count bits, so the caller sees a clean NORMAL state (plus any
// other simultaneously-set flags, which should not happen for RETURN).
#define RESULT_RETURN_CATCH_MASK (RESULT_RETURN | RESULT_SKIP_MASK)

typedef struct {
    VALUE value;
    unsigned int state;
} RESULT;

#if ABRUBY_DEBUG
static inline RESULT _result_ok_checked(VALUE v, const char *file, int line) {
    if (v != 0 && !RB_SPECIAL_CONST_P(v) && !RB_TYPE_P(v, T_DATA)) {
        rb_bug("RESULT_OK: non-VALUE %p at %s:%d", (void*)v, file, line);
    }
    return (RESULT){v, RESULT_NORMAL};
}
#define RESULT_OK(v) _result_ok_checked((v), __FILE__, __LINE__)
#else
#define RESULT_OK(v) ((RESULT){(v), RESULT_NORMAL})
#endif

// abruby class system

typedef struct CTX_struct CTX;
typedef RESULT (*abruby_cfunc_t)(CTX *c, VALUE self, unsigned int argc, VALUE *argv);

struct abruby_method;  // forward decl (abruby_entry refers to it)

// Execution entry point — the boundary at which a body is kicked off.
// Shared by method, class/module body, top-level, proc, etc.
// frame->entry points here so cref is always derivable without per-frame state.
//
// entry->method carries the owning method pointer for method frames (AST
// and cfunc).  For synthetic entries (class/module body, proc, top-level,
// require) method is NULL — the frame is not running inside any named
// method.  This lets us keep a single source of truth for "which method
// owns this frame" and drop the redundant frame.method field.
struct abruby_entry {
    const struct abruby_cref *cref;       // lexical constant scope
    const char *source_file;              // where this entry was defined
    const struct abruby_method *method;   // NULL for non-method frames
    uint32_t stack_limit;                 // max VALUE slots from fp for GC scan
};

enum abruby_method_type {
    ABRUBY_METHOD_AST,
    ABRUBY_METHOD_CFUNC,
    ABRUBY_METHOD_IVAR_GETTER,  // body was { @name } — inlined in dispatch hot path
    ABRUBY_METHOD_IVAR_SETTER,  // body was { @name = arg } — inlined
};

struct abruby_method {
    ID name;
    enum abruby_method_type type;
    // Class where this method was defined (for super chain resolution).
    // Set by abruby_class_add_method. Preserved across per-instance clones so
    // that super dispatch works correctly without storing klass in every frame.
    const struct abruby_class *defining_class;
    // Shared by every method type.  method->entry.method == method (self-
    // referential).  Frames point at this entry directly, so reading
    // frame->entry->method yields this method.
    struct abruby_entry entry;
    union {
        struct {
            struct Node *body;
            unsigned int required_params_cnt;  // # of required pre params
            unsigned int params_cnt;           // # of pre required + optional (before rest)
            unsigned int post_params_cnt;      // # of post required params (after rest)
            int rest_index;                    // slot index for *rest parameter, -1 if none
            struct Node **opt_pc;              // entry points for optional params (NULL if none)
                                               // opt_pc[i] = entry when `i` optionals are filled from argv
                                               // size = (params_cnt - required_params_cnt) + 1
            unsigned int locals_cnt;
        } ast;
        struct {
            abruby_cfunc_t func;
            unsigned int params_cnt;
        } cfunc;
        struct {
            ID ivar_name;  // e.g. @x — looked up in receiver's klass->ivar_shape at call time
        } ivar_accessor;
    } u;
};

/*
 * abruby VALUE invariant:
 *
 * Every abruby VALUE must be one of:
 *   1. CRuby immediate: Fixnum, Symbol, true, false, nil
 *   2. T_DATA (abruby_data_type) with abruby_header at offset 0
 *
 * Raw CRuby heap types (T_BIGNUM, T_FLOAT, T_STRING, etc.) must NOT be
 * used directly. Bignum is wrapped in abruby_bignum, Float in abruby_float.
 * This allows AB_CLASS_OF() to resolve the class by checking immediates
 * first, then reading klass from the T_DATA header.
 */

// Object type tag — used by GC mark to determine marking strategy
// without comparing class pointers (which are per-instance).
enum abruby_obj_type {
    ABRUBY_OBJ_GENERIC,    // abruby_object (ivars)
    ABRUBY_OBJ_STRING,
    ABRUBY_OBJ_ARRAY,
    ABRUBY_OBJ_HASH,
    ABRUBY_OBJ_RANGE,
    ABRUBY_OBJ_REGEXP,
    ABRUBY_OBJ_BIGNUM,
    ABRUBY_OBJ_FLOAT,
    ABRUBY_OBJ_RATIONAL,
    ABRUBY_OBJ_COMPLEX,
    ABRUBY_OBJ_CLASS,
    ABRUBY_OBJ_MODULE,
    ABRUBY_OBJ_EXCEPTION,
    ABRUBY_OBJ_BOUND_METHOD,
    ABRUBY_OBJ_PROC,
    ABRUBY_OBJ_FIBER,
    ABRUBY_OBJ_SYMBOL,
};

// Common layout: ALL abruby T_DATA objects have klass at offset 0,
// followed by obj_type at offset sizeof(void*).  The obj_type tag
// lets abruby_data_free/mark dispatch without dereferencing klass
// (which may already be freed during GC sweep).
struct abruby_header {
    struct abruby_class *klass;
    enum abruby_obj_type obj_type;
};

struct abruby_class {
    struct abruby_class *klass;           // offset 0: metaclass (per-instance class_class or module_class)
    enum abruby_obj_type obj_type;        // offset 8: this struct's own type (CLASS or MODULE) — used by GC dispatch
    enum abruby_obj_type instance_obj_type; // offset 12: type of instances this class creates — replaces 4-byte padding
    ID name;
    struct abruby_class *super;
    struct ab_id_table methods;    // key=method_name, val=(VALUE)(struct abruby_method*)
    VALUE rb_wrapper;
    struct ab_id_table constants;  // key=const_name, val=const_value
    // Shape: ivar_name -> slot index in abruby_object::ivars (as LONG2FIX(slot)).
    // First seen names are assigned sequential slots. The IC caches (klass, slot)
    // and reads/writes obj->ivars[slot] directly with no per-object table lookup.
    struct ab_id_table ivar_shape;
    // shape_id cache: shape_ids_by_cnt[k] is the shape_id for
    // (this class, k ivars set).  0 means unassigned — first observation
    // allocates a new id via abruby_shape_for.
    uint32_t *shape_ids_by_cnt;
    uint32_t  shape_ids_by_cnt_capa;
};

// Direct-indexed instance variables.
// Slots are assigned by the class shape (see abruby_class::ivar_shape) and
// accessed as obj->ivars[slot] with no per-object hash table.
//
// Up to ABRUBY_OBJECT_INLINE_IVARS fit in the inline `ivars` array — common
// cases (binary_trees 2, Point 2, Counter 1) avoid any extra allocation.
// Classes with more ivars (nbody Body 7) use heap extra_ivars for slots >= 4.
#define ABRUBY_OBJECT_INLINE_IVARS 4

struct abruby_object {
    struct abruby_class *klass;                // offset 0
    enum abruby_obj_type obj_type;
    uint32_t ivar_cnt;                         // number of live slots (total across inline + extra)
    VALUE *extra_ivars;                        // NULL if cnt <= INLINE; else slots [INLINE .. cnt)
    VALUE ivars[ABRUBY_OBJECT_INLINE_IVARS];
};

// Unified accessor for ivar slots (read).
static inline VALUE
abruby_object_ivar_read(const struct abruby_object *obj, unsigned int slot)
{
    if (slot < ABRUBY_OBJECT_INLINE_IVARS) return obj->ivars[slot];
    return obj->extra_ivars[slot - ABRUBY_OBJECT_INLINE_IVARS];
}

// Bignum/Float are wrapped in T_DATA with abruby_header, not raw CRuby
// T_BIGNUM/T_FLOAT.  This ensures uniform class resolution via AB_CLASS_OF().

struct abruby_bignum {
    struct abruby_class *klass;  // offset 0: per-instance integer_class
    enum abruby_obj_type obj_type;
    VALUE rb_bignum;             // inner CRuby Bignum (T_BIGNUM)
};

struct abruby_float {
    struct abruby_class *klass;  // offset 0: per-instance float_class
    enum abruby_obj_type obj_type;
    VALUE rb_float;              // inner CRuby Float (Flonum or T_FLOAT)
};

struct abruby_string {
    struct abruby_class *klass;  // offset 0: per-instance string_class
    enum abruby_obj_type obj_type;
    VALUE rb_str;                // inner CRuby String
};

struct abruby_symbol {
    struct abruby_class *klass;  // offset 0: per-instance symbol_class
    enum abruby_obj_type obj_type;
    VALUE rb_sym;                // inner CRuby Symbol (ID2SYM)
};

struct abruby_array {
    struct abruby_class *klass;  // offset 0
    enum abruby_obj_type obj_type;
    VALUE rb_ary;
};

struct abruby_hash {
    struct abruby_class *klass;  // offset 0
    enum abruby_obj_type obj_type;
    VALUE rb_hash;
};

struct abruby_range {
    struct abruby_class *klass;  // offset 0
    enum abruby_obj_type obj_type;
    VALUE begin;
    VALUE end;
    bool exclude_end;            // true for ..., false for ..
};

struct abruby_regexp {
    struct abruby_class *klass;  // offset 0
    enum abruby_obj_type obj_type;
    VALUE rb_regexp;             // inner CRuby Regexp
};

// Proc — heap-allocated escaped block.  Created by Proc.new, lambda,
// proc, &-conversion, Fiber.new (block argument), etc.  Holds a
// snapshot of the enclosing method's local environment so the body can
// still access closure locals after the method returns.
//
// `env` is a heap array of size `env_size`.  When the Proc was created
// from a stack-bound abruby_block, the slots [0..env_size) were copied
// from the original captured_fp.
//
// `defining_method_serial` records the method_serial value at creation
// time.  Real Ruby uses this kind of marker to detect "method already
// returned" for Proc#return — abruby's blocks currently don't have a
// generic non-local-return-from-escaped-Proc, so it's mostly metadata
// for debugging.
struct abruby_proc {
    struct abruby_class *klass;        // offset 0
    enum abruby_obj_type obj_type;
    struct Node *body;
    VALUE *env;                        // ruby_xcalloc(env_size, sizeof(VALUE))
    uint32_t env_size;
    uint32_t params_cnt;
    uint32_t param_base;
    bool is_lambda;
    VALUE captured_self;
    const struct abruby_cref *cref;
};

// Method object — produced by Object#method(:name).  Wraps a (receiver,
// method_name) pair and exposes `call` / `[]` as a way to dispatch the
// underlying method later.  No real Method class hierarchy in abruby;
// these are just plain objects of the per-instance method_class with a
// custom data layout.
struct abruby_bound_method {
    struct abruby_class *klass;  // offset 0
    enum abruby_obj_type obj_type;
    VALUE recv;
    ID method_name;
};

struct abruby_rational {
    struct abruby_class *klass;  // offset 0: per-instance rational_class
    enum abruby_obj_type obj_type;
    VALUE rb_rational;           // inner CRuby Rational
};

struct abruby_complex {
    struct abruby_class *klass;  // offset 0: per-instance complex_class
    enum abruby_obj_type obj_type;
    VALUE rb_complex;            // inner CRuby Complex
};

// Built-in abruby classes are per-instance (stored in abruby_machine).
// Access via c->abm->integer_class etc. at runtime.

extern const rb_data_type_t abruby_data_type;
extern const rb_data_type_t abruby_node_type;

static inline void
ab_verify(VALUE obj)
{
    if (ABRUBY_DEBUG) {
        // immediate values are always valid.  Static symbols (immediate)
        // pass through; dynamic symbols are wrapped in T_DATA (abruby_symbol).
        if (FIXNUM_P(obj) || RB_STATIC_SYM_P(obj) || RB_FLONUM_P(obj) ||
            obj == Qtrue || obj == Qfalse || obj == Qnil) {
            return;
        }
        // Everything else must be T_DATA with abruby_data_type and non-NULL klass.
        // Raw CRuby T_BIGNUM, T_FLOAT, T_STRING etc. are NOT valid abruby values.
        if (!RB_TYPE_P(obj, T_DATA)) {
            rb_bug("ab_verify: expected immediate or T_DATA, got type %d (%s:%d)", rb_type(obj), __FILE__, __LINE__);
        }
        if (!RTYPEDDATA_P(obj)) {
            rb_bug("ab_verify: T_DATA is not TypedData (%s:%d)", __FILE__, __LINE__);
        }
        if (RTYPEDDATA_TYPE(obj) != &abruby_data_type) {
            rb_bug("ab_verify: wrong data type '%s', expected abruby_data_type (%s:%d)",
                   RTYPEDDATA_TYPE(obj)->wrap_struct_name, __FILE__, __LINE__);
        }
        const struct abruby_class *klass =
            ((const struct abruby_header *)RTYPEDDATA_GET_DATA(obj))->klass;
        if (klass == NULL) {
            rb_bug("ab_verify: klass is NULL (%s:%d)", __FILE__, __LINE__);
        }
    }
}

// Type check by obj_type (instance-independent, works with per-instance classes).
// Equivalent to AB_CLASS_OF(obj)->obj_type == type, but doesn't need a CTX.
// Hardcodes the immediate→obj_type mapping (it's fixed across all machines).
static inline bool
ab_obj_type_p(VALUE obj, enum abruby_obj_type type)
{
    if (obj == 0)                return false;                      // NULL/uninitialized
    if (FIXNUM_P(obj))           return type == ABRUBY_OBJ_BIGNUM;  // integer_class.obj_type
    if (RB_FLONUM_P(obj))        return type == ABRUBY_OBJ_FLOAT;
    if (RB_STATIC_SYM_P(obj))    return type == ABRUBY_OBJ_SYMBOL;
    if (RB_SPECIAL_CONST_P(obj)) return type == ABRUBY_OBJ_GENERIC; // true/false/nil
    if (RB_BUILTIN_TYPE(obj) != T_DATA) return false;
    const struct abruby_header *h = (const struct abruby_header *)RTYPEDDATA_GET_DATA(obj);
    return h->klass && h->obj_type == type;
}

// AB_CLASS_OF / AB_CLASS_OF_IMM are defined below after struct CTX_struct / abruby_machine.

// Global variables are stored in an ab_id_table in abruby_machine.
// (struct abruby_gvar_table removed)

// A block literal captured at a call site.  Lives on the C stack of the call
// path that set it; does NOT outlive that call.  (Proc.new / lambda / &blk
// parameters — which would require copying this into a heap object — are
// deliberately out of scope for Phase 1 block support.)
//
// defining_frame is the caller's frame at the point the block was created
// — the "enclosing method frame" in Ruby terms.  While the block body is
// running, current_frame is swapped to this frame so that `yield` inside
// the block refers to the *defining method's* block (Ruby semantics:
// `yield` is tied to the method where the block syntactically appears,
// not the method currently executing the block).  The `super` call inside
// a block also walks from the defining method's class, which matches
// Ruby's non-local semantics.
struct abruby_block {
    struct Node *body;                  // block body AST (also pinned via the AST)
    VALUE *captured_fp;                 // caller's fp at call time (closure env)
    VALUE captured_self;                // caller's self
    struct abruby_frame *defining_frame; // enclosing method frame at capture time
    const struct abruby_cref *cref;     // lexical const scope at capture time
    uint32_t params_cnt;                // number of required params (|x, y|) — MVP has no opt/rest
    uint32_t param_base;                // block params live at captured_fp[param_base..+params_cnt]
    uint32_t env_size;                  // # of usable slots starting at captured_fp;
                                        // used when escaping to a heap Proc
};

// Prologue function pointer types: method-type-specific dispatch functions.
// Stored in method_cache and called directly from the call site, eliminating
// the method-type switch in dispatch_method_frame.
struct method_cache;  // forward declaration

// Method-type-specialized prologue function type.
// Stored in method_cache; dispatch_method_frame dispatches via this pointer,
// avoiding runtime method-type branching at each call site.
typedef RESULT (*method_prologue_t)(
    CTX *c, struct Node *call_site,
    const struct method_cache *mc, VALUE recv_self,
    unsigned int argc, uint32_t arg_index);

// call frame for backtrace support
// method != NULL: normal method frame
// method == NULL: <main>/<top (required)>
//
// block is the block passed in at this call (NULL if the method was called
// without one).  A non-NULL block tells the method boundary to also demote
// RESULT_BREAK — that is where `break` from the block lands (Ruby semantics).
//
// Non-local returns from blocks no longer use a per-frame id: instead
// `node_return` encodes the number of method boundaries to skip in the
// high bits of RESULT.state (see RESULT_SKIP_SHIFT).  Plain (non-block)
// `return` uses skip count 0 and is caught at the nearest method boundary,
// so this path imposes zero per-frame cost outside block return.
struct abruby_frame {
    struct abruby_frame *prev;
    const struct Node *caller_node;  // call site in the caller (set at push time, NULL for <main>)
    const struct abruby_block *block;    // block received by this call, or NULL
    // Per-frame execution state (moved from CTX).
    // Pop automatically restores the caller's state via prev->self/fp.
    VALUE self;
    VALUE *fp;
    // entry->cref is the lexical constant scope; entry->method (may be NULL)
    // identifies the method that owns this frame for backtrace / super walk.
    const struct abruby_entry *entry;
};

// Cached interned IDs for operator methods and common names
struct abruby_id_cache {
    ID op_plus, op_minus, op_mul, op_div;
    ID op_lt, op_le, op_gt, op_ge;
    ID op_eq, op_mod;
    ID op_aref, op_aset, op_ltlt;
    ID op_and, op_or, op_xor, op_gtgt;
    ID method_missing;
    ID initialize;
};

struct abruby_machine;  // forward declaration

#define ABRUBY_STACK_SIZE 10000

// Forward decls for the helpers below.
struct CTX_struct;
struct abruby_frame;

// "Am I currently executing a block body?"
//
// c->current_block_frame is the physical frame that was active when the
// innermost enclosing `yield` entered the block body — i.e. the yielding
// method's frame.  While that same frame is the top of the physical chain
// (= c->current_frame) we are running block body code.  Method calls made
// from within the block body push a new frame, shifting c->current_frame
// away from c->current_block_frame, and the check flips to false for the
// duration of that method's body — exactly what we want because `yield` /
// `return` inside that callee refers to its own method, not the block.
//
// This design lets dispatch_method_frame stay completely untouched: no
// per-call save/restore of block-execution state, no CTX writes on the
// non-block hot path.  Only node_yield / abruby_yield pay the cost of
// pushing/popping current_block / current_block_frame (a rare event).
static inline bool
abruby_in_block(const struct CTX_struct *c);

// Lexical context frame: the method frame whose `yield` / `super` / enclosing
// scope a piece of code at the current execution point refers to.
//
//   - Outside a block body: the current physical frame.
//   - Inside a block body: the defining frame of the currently-executing
//     block (the method where the block literal lexically appears).
//
// We intentionally keep `c->current_frame` as the physical call stack chain
// (the one that frame pushes link together) so that non-local return skip
// counting, backtrace, and inline-exception location all see real frames.
static inline const struct abruby_frame *
abruby_context_frame(const struct CTX_struct *c);

// Lexical scope frame for constant lookup.  Allocated on the heap
// by class/module definition so AST methods defined inside that body
// can capture it (pointer is stored on the method's abruby_method
// struct and is re-installed on c->cref at invocation time).
// Lifetime: never freed — classes are never freed either.
struct abruby_cref {
    struct abruby_class *klass;
    const struct abruby_cref *outer;
};

struct CTX_struct {
    // Hot fields first — all fit in one 64-byte L1 cache line.
    // Every SD entry's first instruction loads current_frame; keeping it
    // at a small offset from the CTX base eliminates an 80KB displacement
    // and guarantees the cache line is already warm from the CTX pointer.
    struct abruby_machine *abm;          // +0  per-instance machine (owner)
    struct abruby_frame *current_frame;  // +8  head of call frame linked list
    struct abruby_class *current_class;  // +16 set during class body eval
    const struct abruby_id_cache *ids;   // +24 cached rb_intern results
    const struct abruby_block *current_block;       // +32
    const struct abruby_frame *current_block_frame; // +40
    // --- end of first cache line (48 bytes used of 64) ---
    VALUE stack[ABRUBY_STACK_SIZE];      // +48  VALUE stack (locals + args)
};

// ctx_update_sp removed: GC uses per-frame entry->stack_limit.
// Slots within stack_limit are zero-filled at scope/prologue entry
// so GC always sees valid VALUEs.

// Derive cref from the current frame's entry.
static inline const struct abruby_cref *
ctx_cref(const struct CTX_struct *c)
{
    const struct abruby_entry *e = c->current_frame->entry;
    return e ? e->cref : NULL;
}

// Definitions of the helpers (forward-declared earlier).
static inline bool
abruby_in_block(const struct CTX_struct *c)
{
    return c->current_block_frame == c->current_frame;
}

static inline const struct abruby_frame *
abruby_context_frame(const struct CTX_struct *c)
{
    return abruby_in_block(c) ? c->current_block->defining_frame : c->current_frame;
}

// Fiber: owns a CTX (execution context with its own VALUE stack) and,
// for non-main fibers, a separate C call stack + ucontext for resume /
// yield switching.
//
// State machine:
//   NEW       -- created via Fiber.new, not yet resumed
//   RUNNING   -- this is the currently executing fiber
//   SUSPENDED -- yielded, waiting for the next resume
//   DONE      -- the body returned (or raised) — further resumes raise
//
// transfer_value carries the value across a swap in either direction:
//   resume(arg)  -- caller writes arg into current fiber's transfer_value
//                   then swapcontext() to the resumed fiber, which reads
//                   it as Fiber.yield's return value
//   yield(arg)   -- the suspending fiber writes arg, swapcontext()s back
//                   to resumer which reads it as resume's return value
//
// resumer is the fiber that called .resume on us; we yield/return back
// to it.  For the main fiber it stays NULL.
struct abruby_fiber;

enum abruby_fiber_state {
    ABRUBY_FIBER_NEW,
    ABRUBY_FIBER_RUNNING,
    ABRUBY_FIBER_SUSPENDED,
    ABRUBY_FIBER_DONE,
};

struct abruby_fiber {
    struct abruby_class *klass;               // offset 0: per-instance fiber_class
    enum abruby_obj_type obj_type;
    CTX ctx;                                  // execution context (own VALUE stack)
    struct abruby_frame root_frame;           // bottom frame (self/fp/cref live here initially)
    enum abruby_fiber_state state;
    bool is_main;                             // true for the bootstrap fiber

    // Block to run.  Set by Fiber.new; consumed on first resume.  Owned
    // VALUE (T_DATA Proc) so the GC keeps it alive.
    VALUE proc_value;

    // Carries the value across swapcontext in both directions.
    VALUE transfer_value;
    // RESULT state of the body when DONE (RAISE / NORMAL).
    unsigned int done_state;

    // CRuby fiber VALUE backing this abruby fiber.  Using CRuby's
    // fiber API ensures GC's machine-stack scan covers the right range.
    // Main fiber leaves this as Qnil (no CRuby fiber needed).
    VALUE crb_fiber;

    // The fiber that called .resume on us.  We swap back here on yield
    // or completion.
    struct abruby_fiber *resumer;

    // Backref to the VM (so the entry function can find it).
    struct abruby_machine *abm;
    // Self pointer for GC marking via the wrapper VALUE.
    VALUE rb_wrapper;
};

struct abruby_machine {
    uint32_t method_serial;              // method version (for inline cache invalidation)
    uint8_t basic_op_redefined;          // nonzero → fixnum/array/hash specializations must fallback
    struct abruby_fiber *current_fiber;  // currently running fiber
    struct abruby_class main_class_body; // per-instance Object subclass (temporary)
    struct ab_id_table gvars;            // global variables
    struct abruby_id_cache id_cache;     // cached rb_intern results
    VALUE rb_self;                       // Ruby-level AbRuby instance
    VALUE current_file;                  // current file path
    VALUE loaded_files;                  // loaded file paths
    VALUE loaded_asts;                   // AST objects kept alive for the
                                         // lifetime of the VM. Required
                                         // because methods defined by
                                         // `require` / `eval` reference
                                         // NODE pointers embedded in these
                                         // ASTs; without a retention root
                                         // the GC sweeps the NODE T_DATA
                                         // and abruby_method bodies become
                                         // dangling (optimization-dependent
                                         // crashes when loading large
                                         // files like optcarrot).
    // Per-instance class pointers (each instance has its own class hierarchy)
    struct abruby_class *object_class;
    struct abruby_class *integer_class;
    struct abruby_class *float_class;
    struct abruby_class *string_class;
    struct abruby_class *symbol_class;
    struct abruby_class *array_class;
    struct abruby_class *hash_class;
    struct abruby_class *range_class;
    struct abruby_class *regexp_class;
    struct abruby_class *rational_class;
    struct abruby_class *complex_class;
    struct abruby_class *true_class;
    struct abruby_class *false_class;
    struct abruby_class *nil_class;
    struct abruby_class *kernel_module;
    struct abruby_class *module_class;
    struct abruby_class *class_class;
    struct abruby_class *runtime_error_class;
    struct abruby_class *method_class;       // for Object#method results
    struct abruby_class *proc_class;         // for Proc.new / lambda / & conversion
    struct abruby_class *fiber_class;        // for Fiber.new / resume / yield
    struct abruby_fiber *root_fiber;         // the bootstrap (main) fiber

    // Shape table.  A shape_id identifies a (class, ivar_cnt) pair.  It
    // lives in an object's T_DATA flags field and is the key that
    // struct ivar_cache guards on, replacing a raw class pointer so the
    // specializer can bake it as a literal.
    //
    // shape_id 0 is reserved for "no shape" / pre-init.  Valid ids are
    // 1..ABRUBY_SHAPE_MAX.
    struct abruby_shape *shapes;   // indexed by shape_id
    uint32_t shape_count;          // 1-based valid count (entry 0 unused)
    uint32_t shape_capa;
};

struct abruby_shape {
    struct abruby_class *klass;
    uint32_t ivar_cnt;
};

// exception object
struct abruby_exception {
    struct abruby_class *klass;  // offset 0: per-instance runtime_error_class
    enum abruby_obj_type obj_type;
    VALUE message;               // abruby VALUE (usually abruby string)
    VALUE backtrace;             // Ruby Array of Strings, or Qnil
};

// flags field bits for struct method_cache
enum {
    MC_PROLOGUE_POLY = 1u << 0,  // mc->prologue has changed at least once;
                                 // PGO specializer refuses to bake at this site
};

// Stable, per-build tag for each prologue function.  Stored in method_cache
// so the PGO resolver and SD_ guards can identify which prologue is active
// without comparing function pointers (each `static inline` prologue has a
// distinct address in abruby.so vs each loaded SD_ .so image — pointer
// equality fails across the dlopen boundary).
enum abruby_prologue_kind {
    PROLOGUE_KIND_NONE = 0,
    PROLOGUE_KIND_AST_SIMPLE_0,
    PROLOGUE_KIND_AST_SIMPLE_1,
    PROLOGUE_KIND_AST_SIMPLE_2,
    PROLOGUE_KIND_AST_SIMPLE_N,
    PROLOGUE_KIND_AST_COMPLEX,
    PROLOGUE_KIND_CFUNC,
    PROLOGUE_KIND_IVAR_GETTER,
    PROLOGUE_KIND_IVAR_SETTER,
};

// inline method cache per call site
struct method_cache {
    const struct abruby_class *klass;    // read-only after fill
    const struct abruby_method *method;  // read-only after fill
    uint32_t serial;
    uint32_t ivar_slot;                 // for IVAR_GETTER/SETTER: cached slot in receiver's shape
    struct Node *body;                  // cached method->u.ast.body (NULL for CFUNC)
    RESULT (*dispatcher)(struct CTX_struct *, struct Node *); // cached body->head.dispatcher
    method_prologue_t prologue;          // method-type-specialized dispatch fn
    // Polymorphism tracking: incremented each time a specialized call
    // dispatcher demotes back to the generic kind because of a klass
    // mismatch.  Once this exceeds a small threshold, the call node is
    // considered polymorphic and no longer re-specialized — we save the
    // swap_dispatcher + re-fill overhead per call in that case.
    uint8_t demote_cnt;
    uint8_t flags;                       // MC_PROLOGUE_POLY etc.
    uint8_t prologue_kind;               // enum abruby_prologue_kind (stable across SO images)
};

// inline ivar cache per node_ivar_get / node_ivar_set site.
//
// Guard: shape_id matches obj's shape_id on arrival (pre-op).  slot is
// the direct index into obj->ivars.  For ivar_set, the pre-op shape may
// differ from the post-op shape when the write grows ivar_cnt — in that
// case post_shape_id carries the shape to write back onto obj after the
// store.  For ivar_get, post_shape_id is unused (and ends up equal to
// shape_id in the common no-transition fill).
//
// shape_id == 0 means not filled.
struct ivar_cache {
    uint32_t shape_id;
    uint32_t post_shape_id;
    unsigned int slot;
};

// Shape_id lives in the T_DATA flags field, bits 12-31 (CRuby's
// FL_USER0..FL_USER19 range — guaranteed unused by core for T_DATA).
// Upper 32 bits of flags are off-limits: recent CRuby GC uses them for
// IMEMO type tags and other per-type bookkeeping, and writing into
// them corrupts the mark phase.  20 bits ⇒ 1M shape_ids, plenty for
// any abruby program.  0 is reserved for "no shape assigned".
#define ABRUBY_SHAPE_BITS   20u
#define ABRUBY_SHAPE_SHIFT  12u
#define ABRUBY_SHAPE_MAX    (((uint32_t)1 << ABRUBY_SHAPE_BITS) - 1u)
#define ABRUBY_SHAPE_FLAG_MASK \
    (((uint64_t)ABRUBY_SHAPE_MAX) << ABRUBY_SHAPE_SHIFT)

static inline uint32_t
abruby_shape_id_read(VALUE obj)
{
    return (uint32_t)((RBASIC(obj)->flags >> ABRUBY_SHAPE_SHIFT) & ABRUBY_SHAPE_MAX);
}

static inline void
abruby_shape_id_write(VALUE obj, uint32_t shape_id)
{
    RBASIC(obj)->flags =
        (RBASIC(obj)->flags & ~ABRUBY_SHAPE_FLAG_MASK) |
        ((uint64_t)(shape_id & ABRUBY_SHAPE_MAX) << ABRUBY_SHAPE_SHIFT);
}

// Look up (or allocate) the shape_id for (klass, ivar_cnt).  Returns 0
// only if the shape table is exhausted (extremely unlikely — 1M slots).
uint32_t abruby_shape_for(struct abruby_machine *abm,
                          struct abruby_class *klass, uint32_t ivar_cnt);


#define LIKELY(expr) __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

// Class resolution helpers — defined after abruby_machine so that c->abm->xxx_class
// can be dereferenced inline.

static __attribute__((unused)) struct abruby_class *
AB_CLASS_OF_IMM(const CTX *c, VALUE obj)
{
    if (FIXNUM_P(obj))       return c->abm->integer_class;
    else if (RB_FLONUM_P(obj)) return c->abm->float_class;
    else if (RB_STATIC_SYM_P(obj)) return c->abm->symbol_class;
    else if (obj == Qtrue)   return c->abm->true_class;
    else if (obj == Qfalse)  return c->abm->false_class;
    else                     return c->abm->nil_class;
}

/*
 * AB_CLASS_OF(c, obj): resolve the abruby class of a VALUE.
 *
 * Heap objects (T_DATA) are checked first since they are the most common
 * receivers in node_method_call — Fixnum arithmetic goes through
 * type-specialized nodes (fixnum_plus, etc.) and rarely reaches here.
 * The abruby VALUE invariant guarantees obj is either a CRuby immediate
 * or T_DATA with abruby_header at offset 0.
 */
static inline struct abruby_class *
AB_CLASS_OF(const CTX *c, VALUE obj)
{
    ab_verify(obj);

    // All non-immediate abruby values are T_DATA with abruby_header.
    // Dynamic symbols are wrapped in abruby_symbol (T_DATA), so no
    // T_SYMBOL check is needed here.
    if (RB_LIKELY(!RB_SPECIAL_CONST_P(obj))) {
        return ((struct abruby_header *)RTYPEDDATA_GET_DATA(obj))->klass;
    }
    else {
        return AB_CLASS_OF_IMM(c, obj);
    }
}

#endif
