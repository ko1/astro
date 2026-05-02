#ifndef KORUBY_PROLOGUES_H
#define KORUBY_PROLOGUES_H 1

/* Per-callsite specialized method-dispatch prologues.
 *
 * Defined here as `static inline __attribute__((always_inline))` so that
 * each TU that includes us — main koruby and every code_store/SD_*.c —
 * gets its own copy.  The cost of duplication is small (each is ~80
 * insns), and the win is large: when a SD's EVAL_node_method_call calls
 * a prologue by name, the C compiler inlines the body inside the SD,
 * eliminating the cross-.so indirect call.
 *
 * abruby uses an identical pattern (DEFINE_AST_SIMPLE_PROLOGUE macro). */

#include "context.h"
#include "object.h"
#include "node.h"

/* current_block is a thread-local in object.c — declared here so the
 * inline prologues can read/write it directly. */
extern struct korb_proc *current_block;

/* Forward declared from object.c — heap-snapshots a returned Proc's
 * env if it points into the about-to-be-popped frame. */
void korb_proc_snapshot_env_if_in_frame(VALUE v, VALUE *fp_lo, VALUE *fp_hi);

/* CFUNC: just call the C function, then handle break-from-block. */
static inline __attribute__((always_inline)) VALUE
prologue_cfunc_inl(CTX *c, struct Node *callsite, VALUE recv,
                   uint32_t argc, uint32_t arg_index,
                   struct korb_proc *block, struct method_cache *mc)
{
    VALUE *argv = &c->fp[arg_index];
    struct korb_proc *prev_block = current_block;
    current_block = block;
    VALUE prev_self = c->self;
    c->self = recv;
    struct Node *prev_cs = c->last_cfunc_callsite;
    c->last_cfunc_callsite = callsite;
    VALUE r = mc->cfunc(c, recv, argc, argv);
    c->last_cfunc_callsite = prev_cs;
    c->self = prev_self;
    current_block = prev_block;
    if (UNLIKELY(block && c->state == KORB_BREAK)) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

/* AST-method prologue, parameterized by PARAMS_KNOWN at compile time so
 * each argc-specialized variant unrolls the locals-fill loop.  The
 * SIMPLE_FRAME flag (set on methods whose body has no super / yield /
 * block_given? / const access / blocked call) lets us skip current_block
 * save/restore, cref save/restore, and frame chain setup — about 10
 * stores per call on tight recursive paths (fib / ack / tak / incr). */
static inline __attribute__((always_inline)) VALUE
prologue_ast_simple_inl(CTX *c, struct Node *callsite, VALUE recv,
                        uint32_t argc, uint32_t arg_index,
                        struct korb_proc *block, struct method_cache *mc,
                        int PARAMS_KNOWN)
{
    uint32_t total = (PARAMS_KNOWN >= 0) ? (uint32_t)PARAMS_KNOWN : mc->total_params_cnt;
    if (UNLIKELY(argc > total)) {
        korb_raise(c, NULL, "wrong number of arguments (given %u, expected %u)",
                   argc, total);
        return Qnil;
    }
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;

    VALUE *new_fp = prev_fp + arg_index;
    c->fp = new_fp;
    if (new_fp + mc->locals_cnt > c->sp) c->sp = new_fp + mc->locals_cnt;

    /* Heavy state save/restore only when method body actually uses it. */
    bool simple = mc->is_simple_frame;
    struct korb_proc *prev_block = NULL;
    struct korb_cref *prev_cref = NULL;
    /* Always push a minimal frame for backtrace.  Heavy state save
     * (block/cref/current_block) only when the body actually uses it. */
    struct korb_frame frame;
    frame.prev = c->current_frame;
    frame.method = mc->method;
    frame.self = recv;
    frame.block = block;
    frame.caller_node = callsite;
    c->current_frame = &frame;
    if (UNLIKELY(!simple)) {
        prev_block = current_block;
        prev_cref = c->cref;
        current_block = block;
        if (mc->def_cref) c->cref = mc->def_cref;
    }

    for (uint32_t i = total; i < mc->locals_cnt; i++) {
        new_fp[i] = Qnil;
    }
    c->self = recv;

    VALUE r = mc->dispatcher(c, mc->body);

    c->current_frame = frame.prev;
    if (UNLIKELY(!simple)) {
        c->cref = prev_cref;
        current_block = prev_block;
    }
    /* If we're returning a Proc whose env points into the about-to-be-
     * popped frame, heap-snapshot it so the next stack push doesn't
     * trash the closure's captured state. */
    korb_proc_snapshot_env_if_in_frame(r, new_fp, new_fp + mc->locals_cnt);
    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        korb_proc_snapshot_env_if_in_frame(c->state_value, new_fp, new_fp + mc->locals_cnt);
    }
    c->fp = prev_fp;
    c->self = prev_self;

    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

/* AOT-baked variant: dispatcher is supplied as an argument.  The C
 * compiler then sees a known target at the call site (when STATIC_DISP
 * is a constant function symbol), turning the indirect `mc->dispatcher`
 * call into a direct call which gcc can in turn inline at -O3.  Used
 * by the AOT specializer when it can statically determine that a hot
 * call site always reaches a specific method body. */
static inline __attribute__((always_inline)) VALUE
prologue_ast_simple_static_inl(CTX *c, struct Node *callsite, VALUE recv,
                               uint32_t argc, uint32_t arg_index,
                               struct korb_proc *block,
                               struct method_cache *mc,
                               int PARAMS_KNOWN,
                               korb_dispatcher_t static_disp)
{
    uint32_t total = (PARAMS_KNOWN >= 0) ? (uint32_t)PARAMS_KNOWN : mc->total_params_cnt;
    if (UNLIKELY(argc > total)) {
        korb_raise(c, NULL, "wrong number of arguments (given %u, expected %u)",
                   argc, total);
        return Qnil;
    }
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;

    VALUE *new_fp = prev_fp + arg_index;
    c->fp = new_fp;
    if (new_fp + mc->locals_cnt > c->sp) c->sp = new_fp + mc->locals_cnt;

    bool simple = mc->is_simple_frame;
    struct korb_proc *prev_block = NULL;
    struct korb_cref *prev_cref = NULL;
    struct korb_frame frame;
    frame.prev = c->current_frame;
    frame.method = mc->method;
    frame.self = recv;
    frame.block = block;
    frame.caller_node = callsite;
    c->current_frame = &frame;
    if (UNLIKELY(!simple)) {
        prev_block = current_block;
        prev_cref = c->cref;
        current_block = block;
        if (mc->def_cref) c->cref = mc->def_cref;
    }

    for (uint32_t i = total; i < mc->locals_cnt; i++) {
        new_fp[i] = Qnil;
    }
    c->self = recv;

    /* Direct call: linker resolves static_disp to a concrete SD_*
     * symbol; gcc emits a direct call instead of going through
     * mc->dispatcher (one indirect load + indirect call removed). */
    VALUE r = static_disp(c, mc->body);

    c->current_frame = frame.prev;
    if (UNLIKELY(!simple)) {
        c->cref = prev_cref;
        current_block = prev_block;
    }
    korb_proc_snapshot_env_if_in_frame(r, new_fp, new_fp + mc->locals_cnt);
    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        korb_proc_snapshot_env_if_in_frame(c->state_value, new_fp, new_fp + mc->locals_cnt);
    }
    c->fp = prev_fp;
    c->self = prev_self;

    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

#endif
