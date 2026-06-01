#ifndef KORUBY_CONTEXT_H
#define KORUBY_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Precise GC framework — provides AroObjectHeader (= every heap obj
 * starts with this 8 B header).  Sample's heap struct (= RBasic
 * derivatives) embeds it at offset 0. */
#include "precise_gc/gc_types.h"

#ifndef KORUBY_DEBUG
#define KORUBY_DEBUG 0
#endif

#if KORUBY_DEBUG
#define KORUBY_ASSERT(expr) assert(expr)
#else
#define KORUBY_ASSERT(expr) ((void)0)
#endif

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

/* ----------------------------------------------------------------------
 * VALUE encoding (CRuby-compatible, x86_64 + USE_FLONUM=1).
 *
 *   Qfalse  = 0x00      ...0000 0000
 *   Qnil    = 0x08      ...0000 1000
 *   Qtrue   = 0x14      ...0001 0100
 *   Qundef  = 0x34      ...0011 0100
 *   FIXNUM  = ...x01    low bit  = 1
 *   FLONUM  = ...x10    low 2 bits = 0b10  (encoded double)
 *   SYMBOL  = ...0c     low 8 bits = 0x0c  (static symbol)
 *   pointer = ...000    low 3 bits = 0
 * ---------------------------------------------------------------------- */

typedef uintptr_t VALUE;
#define ARO_GC_VALUE_TYPEDEFED 1   /* tell precise_gc/gc.h to skip its forward decl */
typedef uintptr_t ID;

#define Qfalse      ((VALUE)0x00)
#define Qnil        ((VALUE)0x08)
#define Qtrue       ((VALUE)0x14)
#define Qundef      ((VALUE)0x34)

#define FIXNUM_FLAG ((VALUE)0x01)
#define FLONUM_MASK ((VALUE)0x03)
#define FLONUM_FLAG ((VALUE)0x02)
#define SYMBOL_MASK ((VALUE)0xff)
#define SYMBOL_FLAG ((VALUE)0x0c)
#define IMMEDIATE_MASK ((VALUE)0x07)

#define FIXNUM_P(v)   (((VALUE)(v)) & FIXNUM_FLAG)
#define FLONUM_P(v)   (((((VALUE)(v)) & FLONUM_MASK)) == FLONUM_FLAG)
/* True for both FLONUM (immediate Float) and heap T_FLOAT.  Use this
 * everywhere a `BUILTIN_TYPE(v) == T_FLOAT` check used to live. */
#define KORB_IS_FLOAT(v) (FLONUM_P(v) || (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_FLOAT))
#define SYMBOL_P(v)   ((((VALUE)(v)) & SYMBOL_MASK) == SYMBOL_FLAG)
#define NIL_P(v)      ((v) == Qnil)
#define TRUE_P(v)     ((v) == Qtrue)
#define FALSE_P(v)    ((v) == Qfalse)
#define UNDEF_P(v)    ((v) == Qundef)
#define SPECIAL_CONST_P(v) (((VALUE)(v)) & IMMEDIATE_MASK || (v) == Qfalse || (v) == Qnil)
#define IMMEDIATE_P(v)     (((VALUE)(v)) & IMMEDIATE_MASK)
#define RTEST(v)      (((VALUE)(v)) & ~Qnil)

#define INT2FIX(i)    ((VALUE)(((intptr_t)(i)) << 1) | FIXNUM_FLAG)
#define FIX2LONG(v)   ((long)((intptr_t)(v) >> 1))

/* FLONUM (immediate Float) — CRuby-compatible encoding.
 * Rotate-left-by-3 of the IEEE-754 double bits, mask LSB, OR FLONUM_FLAG.
 * Doubles whose top 3 exponent bits aren't 011 or 100 (i.e. NaN / Inf /
 * very large / very small / denorm / 0.0) cannot be encoded; in that
 * case korb_float_new heap-boxes via struct korb_float. */
#define KORB_BIT_ROTL64(x, n)  (((VALUE)(x) << (n)) | ((VALUE)(x) >> (64 - (n))))
#define KORB_BIT_ROTR64(x, n)  (((VALUE)(x) >> (n)) | ((VALUE)(x) << (64 - (n))))

static inline VALUE
korb_double_to_flonum(double d) {
    union { double d; VALUE v; } t;
    t.d = d;
    /* CRuby's encoding: only doubles whose top 3 exponent bits are
     * 011 or 100 fit (bits ∈ {3, 4}).  Everything else — 0.0, NaN, Inf,
     * denorms, very large / very small — heap-boxes via the caller. */
    int bits = (int)((t.v >> 60) & 0x7);
    if (((bits - 3) & ~0x01) != 0) return 0;
    /* Special-case 1.72723e-77 (all-zero mantissa with bits=3) — CRuby
     * reserves that pattern for +0.0 encoded as flonum.  We just heap. */
    if (t.v == 0x3000000000000000ULL) return 0;
    return (KORB_BIT_ROTL64(t.v, 3) & ~(VALUE)0x01) | FLONUM_FLAG;
}

static inline double
korb_flonum_to_double(VALUE v) {
    union { double d; VALUE v; } t;
    VALUE b63 = v >> 63;
    t.v = KORB_BIT_ROTR64((2 - b63) | (v & ~(VALUE)0x03), 3);
    return t.d;
}

#define FIXNUM_MAX  ((intptr_t)((((uintptr_t)1) << (sizeof(VALUE)*8 - 2)) - 1))
#define FIXNUM_MIN  (-FIXNUM_MAX - 1)
#define POSFIXABLE(i) ((i) <= FIXNUM_MAX)
#define NEGFIXABLE(i) ((i) >= FIXNUM_MIN)
#define FIXABLE(i)    (POSFIXABLE(i) && NEGFIXABLE(i))

/* heap object types */
enum korb_type {
    T_NONE = 0,
    T_OBJECT,
    T_CLASS,
    T_MODULE,
    T_FLOAT,
    T_BIGNUM,
    T_STRING,
    T_ARRAY,
    T_HASH,
    T_SYMBOL,
    T_PROC,
    T_RANGE,
    T_NODE,
    T_DATA,
    /* T_ARY_BACKING — payload buffer object for T_ARRAY.  Layout:
     * AroObjectHeader head; VALUE items[N] inline.  N is derived from
     * head.gc_size at scan time (= (gc_size - sizeof header)/sizeof VALUE),
     * so koruby_scan_edges walks items[] via the backing's OWN header,
     * keeping reader and data co-located across moving backends.  The owning
     * struct korb_array holds a VALUE reference (`backing`) to it.  See
     * docs/array_payload_value.md. */
    T_ARY_BACKING,
    T_LAST
};

#define T_MASK 0x1f
#define FL_FROZEN ((uint16_t)0x20)

/* Heap object base — every aro_gc_alloc'd struct starts with `head`
 * (= 8 B AroObjectHeader, framework managed) at offset 0.  `klass` is
 * the sample-side per-instance class pointer; ARO_GC_EDGE qualifies it
 * for compile-time WB audit (= sample must use ARO_STORE to update).
 *
 * head.flags low 5 bits hold T_* (= type tag); higher bits carry sample
 * flags like FL_FROZEN (= 16 bit width is enough for koruby's flag set). */
struct RBasic {
    AroObjectHeader head;
    /* TODO: ARO_GC_EDGE qualify klass once Phase 3 (SCAN_EDGES) is in
     * place — until then mutator code initializes klass via direct
     * assignment in many sites, which the const audit would reject. */
    struct korb_class *klass;
};

/* BUILTIN_TYPE returns the heap-object type, or T_NONE for any immediate
 * value.  This lets callers safely check `BUILTIN_TYPE(v) == T_FOO` without
 * a separate SPECIAL_CONST_P guard. */
#define BUILTIN_TYPE(v) (SPECIAL_CONST_P(v) ? T_NONE : (enum korb_type)((((struct RBasic *)(v))->head.flags) & T_MASK))
#define RBASIC(v)       ((struct RBasic *)(v))

/* forward */
struct Node;
struct korb_class;
struct korb_method;
struct korb_array;
struct korb_string;
struct korb_hash;
struct korb_bignum;
struct korb_object;
struct korb_proc;

/* options */
struct koruby_option {
    bool no_compiled_code;
    bool dump_ast;
    bool quiet;
    bool verbose;
    bool jit;
    bool record_all;
};
extern struct koruby_option OPTION;

/* serial for method cache */
typedef uint64_t state_serial_t;

/* =====================================================================
 * Control-flow state codes (KORB_NORMAL .. KORB_THROW).
 *
 * Used by both the legacy c->state side-channel and the new RESULT-typed
 * return-propagation path.  When state != KORB_NORMAL, callers must
 * propagate without using the value.
 * ===================================================================== */
#define KORB_NORMAL 0
#define KORB_RAISE  1
#define KORB_RETURN 2
#define KORB_BREAK  3
#define KORB_NEXT   4
#define KORB_RETRY  5
#define KORB_REDO   6
#define KORB_THROW  7

/* =====================================================================
 * RESULT type — sp-based / Lua-style ABI for cfunc and C API helpers.
 *
 * Convention summary (Phase 2+):
 *   cfunc:           RESULT cf(CTX *c, int argc, VALUE *sp)
 *                    sp[-argc-1] = self, sp[-argc..-1] = args, sp[0..] = scratch
 *   C API helper:    RESULT h (CTX *c, VALUE *sp)  with sp[-N..-1] = args
 *   EVAL_node body:  RESULT EVAL_node_X(CTX *c, NODE *n, VALUE *sp, ...)
 *                    same sp meaning (parent's staging top)
 *
 * RESULT carries value + state byte so raise/break/next/return/throw/redo
 * propagate via early return rather than via c->state side-channel.  Sized
 * to fit in two registers (rax/rdx) on x86_64.  Modeled after
 * baruby_precise / castro / abruby; same idea as Lua's stack-based C API.
 * ===================================================================== */
typedef struct {
    VALUE   value;
    uint8_t state;
} RESULT;

/* Mark RESULT-returning functions so the compiler enforces that callers
 * actually use the returned value (UNWRAP / CHECK / inline handling).
 * Without it, a forgotten UNWRAP would silently drop exception
 * propagation.  With -Werror=unused-result this becomes a build failure.
 *
 * Apply to function declarations / definitions, e.g.
 *   RESULT_FN RESULT my_func(CTX *c, ...);
 *
 * GCC's warn_unused_result attribute only applies to function types, not
 * to the return type itself, so this is the cleanest way to apply it. */
#define RESULT_FN __attribute__((warn_unused_result))

/* Discharge a RESULT explicitly without propagating it.  Rarely used now
 * (Phase 8d-R5 RESULT-ized all helpers); kept for the cases where the
 * caller really wants to ignore a raise. */
#define DROP_RESULT(call) do {                                  \
    RESULT _drop_r = (call);                                    \
    (void)_drop_r.value;                                        \
} while (0)

#define RESULT_OK(v)        ((RESULT){(v), KORB_NORMAL})
#define RESULT_RAISE_R(v)   ((RESULT){(v), KORB_RAISE})
#define RESULT_RETURN_R(v)  ((RESULT){(v), KORB_RETURN})
#define RESULT_BREAK_R(v)   ((RESULT){(v), KORB_BREAK})
#define RESULT_NEXT_R(v)    ((RESULT){(v), KORB_NEXT})
#define RESULT_THROW_R(v)   ((RESULT){(v), KORB_THROW})
#define RESULT_REDO_R(v)    ((RESULT){(v), KORB_REDO})
#define RESULT_RETRY_R(v)   ((RESULT){(v), KORB_RETRY})

/* UNWRAP — extract VALUE from RESULT, propagate non-NORMAL via early
 * return.  Caller's function must return RESULT.  Uses GNU statement
 * expression. */
#define UNWRAP(call) ({                                   \
    RESULT _r = (call);                                   \
    if (__builtin_expect(_r.state != KORB_NORMAL, 0))     \
        return _r;                                        \
    _r.value;                                             \
})

/* CHECK — same as UNWRAP but discards the value (for side-effect calls). */
#define CHECK(call) ({                                    \
    RESULT _r = (call);                                   \
    if (__builtin_expect(_r.state != KORB_NORMAL, 0))     \
        return _r;                                        \
    (void)_r;                                             \
})

/* LIFT_C_STATE / SINK_RESULT / LIFT_C_STATE_OR_OK macros removed in
 * Phase 8d-R5: c->state side-channel field deleted, all bridges gone.
 *
 * KORB_SYNC_SP also removed: sp staging now writes c->sp_top directly
 * inside alloc helpers (= "alloc 関数が c, sp を受け取り、その中で c->sp = sp"
 * design rule).  Callers don't sync. */

/* =====================================================================
 * Dispatcher / prologue / cfunc function-pointer typedefs.
 *
 * Two parallel ABIs exist during the Phase 2-4 migration:
 *   - Legacy: `VALUE (*)(...)` based, state side-channel via c->state.
 *   - New:    `RESULT (*)(...)` based, sp-staging, in-band state.
 *
 * Method-cache slots for both are kept (`cfunc` + `cfunc_r`); the
 * dispatcher picks the available one.  After the sweep, the legacy
 * fields will be removed.
 * ===================================================================== */
struct CTX_struct;
struct method_cache;

/* Legacy EVAL_node dispatcher: returns VALUE, propagates state via c->state. */
typedef RESULT (*korb_dispatcher_t)(struct CTX_struct *c, struct Node *n, VALUE *sp);

/* Legacy prologue: chosen at method_cache fill time
 * (ast_simple / ast_general / cfunc). */
typedef RESULT (*korb_prologue_t)(struct CTX_struct *c, struct Node *callsite,
                                 VALUE recv, uint32_t argc, uint32_t arg_index,
                                 struct korb_proc *block, struct method_cache *mc);

/* New cfunc signature (sp-based, RESULT-returning, Lua-style). */
typedef RESULT (*korb_cfunc_r_t)(struct CTX_struct *c, int argc, VALUE *sp);

/* New EVAL_node dispatcher signature (RESULT-returning). */
typedef RESULT (*korb_dispatcher_r_t)(struct CTX_struct *c, struct Node *n, VALUE *sp);

struct method_cache {
    state_serial_t serial;
    uint64_t       gen;           /* GC generation when filled — see ivar_cache */
    struct korb_class *klass;
    struct korb_method *method;
    struct Node *body;             /* cached body NODE for AST methods */
    korb_dispatcher_t dispatcher;  /* body's dispatcher (specialized SD or default) */
    korb_prologue_t prologue;      /* selected at fill time — see above */
    uint32_t locals_cnt;
    uint32_t required_params_cnt;
    uint32_t total_params_cnt;     /* required + optional + rest(0/1) + post */
    int      rest_slot;            /* -1 if no *rest */
    int      block_slot;           /* -1 if no &blk */
    uint32_t post_params_cnt;      /* params after *rest */
    int      kwh_save_slot;        /* slot to stash kwargs hash; -1 if no kwargs */
    uint8_t  type;                 /* 0=AST, 1=CFUNC */
    bool     is_simple_frame;      /* method body has no yield/super/block_given/_block — slim prologue */
    VALUE (*cfunc)(struct CTX_struct *, VALUE, int, VALUE *);
    /* New sp-based RESULT-returning cfunc.  When non-NULL, prologue_cfunc_r_inl
     * is used instead of the old `cfunc` path.  Phase 2-4 transition field;
     * eventually replaces `cfunc`. */
    korb_cfunc_r_t cfunc_r;
    struct korb_cref *def_cref;    /* lexical cref captured at def-time */
    /* param_position → fp slot.  NULL = identity (the common case).
     * Mirrors korb_method->u.ast.param_holder_slots; cached so the
     * prologue doesn't dereference mc->method on the hot path. */
    int     *param_holder_slots;
};

/* call cache for func calls (similar) */
struct call_cache {
    state_serial_t serial;
    struct Node *body;
    uint32_t locals_cnt;
    uint32_t params_cnt;
};

/* inline ivar cache: each ivar AST node carries one of these.  The cache
 * is monomorphic on the receiver's class — same class ⇒ same slot.
 *
 * `gen` records the GC generation at fill-time.  Under moving GC, two
 * different classes can sequentially occupy the same address (= old A
 * moves away, then B is placed there), so pointer equality alone can
 * mis-match.  visit_roots bumps `korb_g_gc_gen` each cycle; mismatch
 * forces a fresh ivar_slot() lookup via the slow path. */
struct ivar_cache {
    struct korb_class *klass;
    int32_t slot;             /* -1 if name not present in klass */
    uint64_t gen;
};

extern uint64_t korb_g_gc_gen;

/* lexical constant scope: chain of currently-nested classes/modules */
struct korb_cref {
    struct korb_class *klass;
    struct korb_cref *prev;
};

/* current_frame chain (for backtrace + GC root).  Defined BEFORE
 * struct CTX_struct so CTX can embed a sentinel_frame by value (= no
 * per-CTX global). */
struct korb_frame {
    struct korb_frame *prev;
    struct Node *caller_node;  /* for backtrace */
    struct korb_method *method;
    VALUE self;
    VALUE *fp;
    /* Scope-bound state — these used to live in CTX but now live here
     * so save/restore = frame push/pop.  visit_roots walks the frame
     * chain and updates heap pointers automatically. */
    struct korb_cref *cref;            /* lexical const scope */
    struct korb_class *current_class;  /* def-target class */
    const char *current_file;          /* for backtrace + require_relative */
    uint32_t locals_cnt;
    struct korb_proc *block;   /* block passed to this method (NULL if none) */
    /* $_ (last_line) and $~ (last_match) are method-scoped pseudo-globals.
     * Blocks and lambdas defined inside a method share the surrounding
     * method's $_ — we cooperate by NOT pushing a frame for yields/proc
     * calls, so c->current_frame->last_line is naturally the enclosing
     * method's slot.  Method dispatches push a fresh frame and reset
     * these to Qnil. */
    VALUE last_line;
    VALUE last_match;
    /* The block / proc / lambda that was running when this frame was
     * pushed (i.e. the block whose body called THIS method).  NULL when
     * called outside any block.  Used by korb_build_backtrace to
     * synthesize a "block in <enclosing>" entry between this frame and
     * its caller, since blocks don't get their own frame in koruby. */
    void *caller_running_block;
    /* Number of times the method's defining_class has already been seen
     * in the receiver's MRO before reaching this frame's method.  For a
     * normal dispatch this is 0 (we found the method's first occurrence).
     * `super` from a frame inside a module that's both prepended AND
     * included on the same class needs to know which occurrence the
     * current frame came from — without it, `super` lands back on the
     * first occurrence and loops forever. */
    uint16_t super_skip_n;
    /* Monotonic frame ID — captured on every method-frame push.  Block
     * literals snapshot this at creation; on break/return they verify
     * the target frame's current ID matches.  When a method returns,
     * its stack slot may be reused by a future call — that future
     * frame gets a new (different) ID, so a stale block sees the
     * mismatch and we can raise LocalJumpError instead of corrupting
     * the active method.  Bumped by korb_alloc_frame_id(). */
    uint64_t frame_id;
    /* Singly-linked list of bindings created INSIDE this frame's
     * lifetime.  At frame epilogue we walk the chain and copy the
     * current fp slots into each binding's heap snapshot — so
     * `bind = binding; ...; b = 1; @ret = bind` returns a binding
     * that sees b's final value (CRuby heap-promote semantics). */
    void *bindings_head;
    /* Only set on the synthetic top_frame korb_eval_string pushes (prev=NULL
     * for control-flow isolation).  Links active eval/require top_frames into
     * a CTX-resident stack (c->eval_frame_chain) so visit_roots can keep their
     * `self` (= main_obj) and cref alive while a DEEPER nested eval is current
     * — the prev=NULL cut would otherwise disconnect them from the head chain
     * and let main_obj go stale across the inner eval's GCs. */
    struct korb_frame *eval_prev;
};

/* korb_yield self-save node — C-stack-allocated per yield, linked into
 * c->yield_self_chain so visit_roots can keep the saved enclosing self
 * forwarded across the block body's GC.  See CTX.yield_self_chain. */
struct korb_yield_self_save {
    VALUE self;
    struct korb_yield_self_save *prev;
};

/* execution context */
typedef struct CTX_struct {
    /* precise GC instance — see runtime/precise_gc/gc.h.  The framework
     * accesses this field via ARO_GC_INSTANCE / ARO_GC_COMMON; sample
     * must declare it as the first field by contract. */
    struct ASTroGC *astro_gc;

    VALUE *stack_base;
    VALUE *stack_end;
    VALUE *sp_top;        /* high-water mark for GC scanning. Only alloc
                           * helpers write here, immediately before the
                           * GC trigger. Other code MUST NOT write it. */
    VALUE *env;           /* root scan lower bound — set at init (= stack_base) */
    /* `self`, `fp`, `cref`, `current_class`, `current_file` all live in
     * `current_frame->*` — the frame chain is the authoritative source
     * for scope-bound state.  GC root scan (visit_roots phase d) walks
     * the chain and updates heap pointers automatically; save/restore
     * across body GC is just a frame push/pop, no C-local intermediates
     * needed. */
    /* Per-CTX sentinel frame + top cref (= no globals, so multiple
     * interpreters can coexist).  visit_roots walks these directly so
     * top_cref.klass stays fresh even when a method's frame.cref is
     * mc->def_cref (= SEPARATE chain via korb_cref_dup). */
    struct korb_frame sentinel_frame;
    struct korb_cref  top_cref;
    /* Head of the active eval/require top_frame stack (linked via
     * korb_frame.eval_prev).  Walked by visit_roots so suspended eval
     * frames' self/cref stay forwarded across nested-eval GCs. */
    struct korb_frame *eval_frame_chain;
    /* Head of the korb_yield self-save stack.  korb_yield overwrites
     * c->current_frame->self with the block's self for the body and must
     * restore the enclosing self afterwards — but a bare C-local would go
     * stale across the body's GC (the enclosing self is reachable only via
     * the overwritten frame slot / a no-longer-scanned sp slot).  Each yield
     * registers a C-stack save here; visit_roots walks the chain so the saved
     * self stays forwarded.  Nested yields chain via the per-call `prev`. */
    struct korb_yield_self_save *yield_self_chain;

    /* Per-CTX pointer to the interpreter's machine state.  Naming follows
     * abruby's `c->abm` / `struct abruby_machine` convention (rather than
     * "vm" which doesn't fit a tree-walking AST interpreter).
     * Transitional: the global `korb_vm` still exists and `c->mch` is
     * initialized to it at setup.  Future code should prefer `c->mch->X`
     * over `korb_vm->X` so the global can eventually be removed and
     * multi-interpreter embedding (one CTX per interpreter) becomes
     * possible.
     * Note: the `struct korb_vm` type name itself is also up for rename
     * (planned: `struct korb_machine`); kept as-is for this incremental
     * patch to avoid touching 1300+ references in one go.
     * See memory note: feedback_result_and_vm_priorities. */
    struct korb_vm *mch;

    state_serial_t method_serial;

    /* For KORB_RETURN: target frame pointer.  When non-NULL, the return
     * is non-local (block/proc → enclosing method); each method dispatch
     * checks `state_target_frame == &my_frame` and only consumes when
     * matched.  NULL for plain method-local return (lambda body, def
     * body).  Not bridged through c->state — set by node_return and
     * read by prologue at consume time. */
    void *state_target_frame;

    /* for call site & frame info */
    struct korb_frame *current_frame;

    /* When non-NULL, the currently-executing code is a Binding#eval body
     * — Kernel#local_variables / __method__ / etc. consult this to
     * report the binding's view rather than the caller frame's. */
    void *current_eval_binding;
    /* When non-NULL, the currently-executing code is an eval body (with
     * or without binding).  Holds the parsed program node so `binding`
     * inside the eval body can pick up eval-introduced lvars.  Distinct
     * from current_eval_binding, which is only set for Binding#eval. */
    struct Node *current_eval_program_body;

    /* Most-recent callsite of a cfunc dispatch — set by prologue_cfunc_inl
     * before calling the cfunc, so cfunc bodies (e.g. kernel_raise) can
     * record the line of the call into a backtrace. */
    struct Node *last_cfunc_callsite;

    /* Moving-GC root for the receiver during a method prologue's argument
     * processing.  The receiver arrives as a bare C-local `recv` and is held
     * across GC points (rest-array build, kwargs hash_new) before being stored
     * into the new frame's self.  visit_roots scans this slot so recv survives
     * those GCs.  Each prologue saves the previous value on the C stack and
     * restores it on exit, so nested dispatch works.  0 when no prologue is
     * mid-argument-processing. */
    VALUE dispatch_recv_root;

    /* Block currently in scope for yield.  Distinct from
     * current_frame->block (which is the block PASSED to the current
     * method): when a proc body executes, current_block is swapped to
     * the proc's enclosing block so that `yield` inside the proc fires
     * the right block.  Saved/restored by dispatcher + proc_call. */
    struct korb_proc *current_block;

    /* The block/proc/lambda whose body is currently being executed —
     * distinct from current_block (= block to be yielded to).  Used by
     * super (to know the lexical method of the block being run) and by
     * backtrace.  Saved/restored at proc_call / yield boundaries. */
    struct korb_proc *running_block;
} CTX;

/* Per-CTX machine access.  Transition macro: callers should prefer
 * `KORB_VM(c)->X` over `korb_vm->X`.  Both currently resolve to the
 * same struct, but after the global is removed only this form will
 * remain.  See feedback_result_and_vm_priorities memory note. */
#define KORB_VM(c) ((c)->mch)

/* push/pop frame helpers via macro */
#define KORB_PUSH_FRAME(c, mtd, fp_, locals_, caller) \
    struct korb_frame _frame_ = {                     \
        .prev = (c)->current_frame,                 \
        .caller_node = (caller),                    \
        .method = (mtd),                            \
        .self = (c)->self,                          \
        .fp = (fp_),                                \
        .locals_cnt = (locals_),                    \
        .last_line = Qnil,                          \
        .last_match = Qnil,                         \
        .caller_running_block = NULL,               \
    };                                              \
    (c)->current_frame = &_frame_;                  \
    do{}while(0)

#define KORB_POP_FRAME(c) \
    do { (c)->current_frame = _frame_.prev; } while (0)

/* ARO_GC_INSTANCE(c) — framework reads this to access the per-CTX GC
 * instance.  Must match the c->astro_gc field declared above. */
#define ARO_GC_INSTANCE(c)  ((c)->astro_gc)

/* ---------------------------------------------------------------------------
 * Precise GC contract macros (= sample-side hooks the framework calls).
 *
 * Phase 1 stubs: most GC paths in koruby still leak via libc.  These
 * macros let the framework headers compile, but the runtime audit is
 * NOT yet complete — Phases 2-3 will properly populate root scan +
 * SCAN_EDGES dispatch.
 * ------------------------------------------------------------------------- */

/* AROH_IS_GC_OBJECT(v) — true iff v is a heap-managed pointer.  Excludes
 * fixnums (low bit 1), flonums (low 2 bits 10), symbols (low 8 bits 0c),
 * Qfalse/Qnil/Qtrue/Qundef. */
#define AROH_IS_GC_OBJECT(v) (!SPECIAL_CONST_P(v))

/* Root-stack contract for ARO_ROOT_SCOPE_* in runtime/precise_gc/gc.h.
 * koruby_precise uses c->stack_base..c->sp_top as the single VALUE stack
 * (= eval stack + precise root spill stack).  AROH_VISIT_ROOTS walks
 * the same range plus CTX-held VALUEs / cref chain / current_frame
 * chain / korb_vm globals. */
#define AROH_ROOT_STACK_TOP(c)        ((c)->sp_top)
#define AROH_ROOT_STACK_SET_TOP(c, p) ((c)->sp_top = (p))
#define AROH_ROOT_STACK_LIMIT(c)      ((c)->stack_end)

/* Phase 2-3: root visitor + per-type edge dispatch.  Both forwarded to
 * out-of-line functions in koruby_runtime.c so the heavy switch and chain
 * walks live in one place (= keeps every framework backend's translation
 * unit small).  Sample owns the bodies via the function definitions; this
 * header only declares the dispatch surface. */
typedef void (*koruby_edge_fn)(void *ctx, void **slot);
void koruby_visit_roots(CTX *c, void *ctx, koruby_edge_fn fn);
/* Scan a Fiber's saved roots (suspended fiber stack/frames, or — for the
 * running fiber — its suspended resumer's stack/frames).  Defined in object.c
 * where the korb_fiber struct is visible; called from the registry walk. */
void korb_scan_fiber_roots(VALUE fibv, void *ctx, koruby_edge_fn fn);
void koruby_scan_edges(void *payload, size_t payload_size,
                       void *ctx, koruby_edge_fn fn);

#define AROH_VISIT_ROOTS(c, ctx, edge_visit) \
    koruby_visit_roots((c), (ctx), (koruby_edge_fn)(edge_visit))

#define AROH_SCAN_EDGES(payload, payload_size, ctx, edge_visit) \
    koruby_scan_edges((payload), (payload_size), (ctx), \
                      (koruby_edge_fn)(edge_visit))

/* AROH_INIT_PAYLOAD — zero-fill post-head region after alloc.  koruby's
 * heap objects work fine with this default (= same as ascheme_precise). */
#define AROH_INIT_PAYLOAD(payload, size_bytes) \
    memset((char *)(payload) + sizeof(AroObjectHeader), 0, \
           (size_bytes) - sizeof(AroObjectHeader))
#define AROH_INIT_BYTE_PAYLOAD(payload, size_bytes) ((void)0)

/* AROH_FINALIZE — Phase 1 stub.  Phase 5 wires GMP `mpz_clear` for
 * dead Bignum objects via aro_gc_finalize_register. */
#define AROH_FINALIZE(payload) ((void)(payload))

#endif /* KORUBY_CONTEXT_H */
