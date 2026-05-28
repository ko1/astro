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
    /* Empty `**` kwsplat to a no-kwargs method: silently drop the
     * empty kwargs hash (CRuby 3.0+).  Detect via FL_KWARGS tag.  Note:
     * mc->kwh_save_slot < 0 always for "simple" methods, so we only
     * need to check the trailing-arg flag. */
    if (UNLIKELY(argc > 0)) {
        VALUE last = c->fp[arg_index + argc - 1];
        if (!SPECIAL_CONST_P(last) && BUILTIN_TYPE(last) == T_HASH &&
            (RBASIC(last)->head.flags & FL_KWARGS) &&
            ((struct korb_hash *)last)->size == 0) {
            argc--;
        }
    }
    /* Simple methods have no opt / rest / post / kwargs — argc must
     * exactly match.  Too-few raises ArgumentError just like too-many. */
    if (UNLIKELY(argc != total)) {
        VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eArg,
                   "wrong number of arguments (given %u, expected %u)",
                   argc, total);
        return Qnil;
    }
    VALUE *prev_fp = c->fp;
    VALUE *prev_sp = c->sp;
    VALUE prev_self = c->self;

    VALUE *new_fp = prev_fp + arg_index;
    c->fp = new_fp;
    if (new_fp + mc->locals_cnt > c->sp) c->sp = new_fp + mc->locals_cnt;

    /* Heavy state save/restore only when method body actually uses it. */
    bool simple = mc->is_simple_frame;
    struct korb_proc *prev_block = NULL;
    struct korb_cref *prev_cref = NULL;
    /* Always push a minimal frame for backtrace.  Heavy state save
     * (block/cref/current_block) only when the body actually uses it.
     * fp + locals_cnt are recorded too so Kernel#binding /
     * __capture_lvars__ can read the active method's slots. */
    struct korb_frame frame;
    frame.prev = c->current_frame;
    frame.method = mc->method;
    frame.self = recv;
    frame.block = block;
    frame.caller_node = callsite;
    frame.fp = new_fp;
    frame.locals_cnt = mc->locals_cnt;
    /* Normal call: method's defining_class is hit for the first time.
     * super-from-here should walk past the FIRST occurrence. */
    frame.super_skip_n = 0;
    extern uint64_t korb_g_next_frame_id;
    frame.frame_id = ++korb_g_next_frame_id;
    frame.bindings_head = NULL;
    /* $_ / $~ are method-scoped — fresh slot per call.  We always init
     * because callees (e.g. kernel_print without args, or any method
     * that gets / =~ s its way into $_) reach for the current frame's
     * slot, even when the calling method body itself never references
     * $_ directly. */
    frame.last_line = Qnil;
    frame.last_match = Qnil;
    /* Capture the block whose body is calling us, so backtrace can
     * synthesize a "block in <enclosing>" entry above this frame. */
    extern struct korb_proc *running_block;
    frame.caller_running_block = running_block;
    c->current_frame = &frame;
    /* Entering a method body — no block is "running" at this point.
     * If we don't reset, a `return` inside a method called from within
     * a block would be interpreted as non-local (target_fp = block's
     * enclosing). */
    struct korb_proc *prev_running = running_block;
    running_block = NULL;
    /* cref must reflect the method's definition site so cref-dependent
     * operations (Kernel#binding, class-variable access, `class C`
     * keyword) see the lexical class.  Most calls have c->cref ==
     * mc->def_cref already (top-level fib, ack, etc.) so guard the
     * swap to skip 4 memory ops per call on the hot path. */
    bool cref_swapped = (mc->def_cref != NULL && c->cref != mc->def_cref);
    if (UNLIKELY(cref_swapped)) {
        prev_cref = c->cref;
        c->cref = mc->def_cref;
    }
    if (UNLIKELY(!simple)) {
        prev_block = current_block;
        current_block = block;
    }

    for (uint32_t i = total; i < mc->locals_cnt; i++) {
        new_fp[i] = Qnil;
    }
    c->self = recv;

    VALUE r = mc->dispatcher(c, mc->body, new_fp);

    c->current_frame = frame.prev;
    running_block = prev_running;
    if (UNLIKELY(cref_swapped)) c->cref = prev_cref;
    if (UNLIKELY(!simple)) {
        current_block = prev_block;
    }
    /* If we're returning a Proc whose env points into the about-to-be-
     * popped frame, heap-snapshot it so the next stack push doesn't
     * trash the closure's captured state. */
    korb_proc_snapshot_env_maybe(r, new_fp, new_fp + mc->locals_cnt);
    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        korb_proc_snapshot_env_maybe(c->state_value, new_fp, new_fp + mc->locals_cnt);
    }
    /* Bindings created in this frame's lifetime: copy fp slots into
     * their heap snapshots so they hold final values after the frame
     * pops (CRuby heap-promote approximation). */
    if (UNLIKELY(frame.bindings_head != NULL)) {
        korb_binding_snapshot_frame(&frame);
    }
    c->fp = prev_fp;
    /* Always restore sp.  Without this, every method call leaves sp
     * higher than before — long-running loops with many calls hit
     * stack_end and false-overflow.  Cheap (one store per call).
     *
     * Zero-fill the popped slots [prev_sp, c->sp) before lowering sp.
     * Subsequent sp-up by a sibling/later call will re-expose those
     * addresses; without the zero-clear the new frame would inherit
     * this method's stale heap pointers (= a frame's last write to a
     * local survives the pop, visit_roots picks it up later when sp
     * grows back, and forwards a long-since-moved obj). */
    for (VALUE *p = prev_sp; p < c->sp; p++) *p = Qnil;
    c->sp = prev_sp;
    c->self = prev_self;

    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        bool consume_return = (c->state == KORB_RETURN &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        /* break with NULL target: legacy "any method consumes" path
         * (yield-style break from a cfunc-driven loop, or break that
         * already escaped its inner while/loop).  break with concrete
         * target: only the matching frame consumes (set in proc_call
         * for &block-yield style escapes). */
        bool consume_break = (c->state == KORB_BREAK &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        if (consume_break || consume_return) {
            r = c->state_value;
            c->state = KORB_NORMAL;
            c->state_value = Qnil;
            c->state_target_frame = NULL;
        }
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
    /* Simple methods have no opt / rest / post / kwargs — argc must
     * exactly match.  Too-few raises ArgumentError just like too-many. */
    if (UNLIKELY(argc != total)) {
        VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eArg,
                   "wrong number of arguments (given %u, expected %u)",
                   argc, total);
        return Qnil;
    }
    VALUE *prev_fp = c->fp;
    VALUE *prev_sp = c->sp;
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
    frame.fp = new_fp;
    frame.locals_cnt = mc->locals_cnt;
    /* Normal call: method's defining_class is hit for the first time.
     * super-from-here should walk past the FIRST occurrence. */
    frame.super_skip_n = 0;
    extern uint64_t korb_g_next_frame_id;
    frame.frame_id = ++korb_g_next_frame_id;
    frame.bindings_head = NULL;
    /* $_ / $~ are method-scoped — fresh slot per call.  We always init
     * because callees (e.g. kernel_print without args, or any method
     * that gets / =~ s its way into $_) reach for the current frame's
     * slot, even when the calling method body itself never references
     * $_ directly. */
    frame.last_line = Qnil;
    frame.last_match = Qnil;
    /* Capture the block whose body is calling us, so backtrace can
     * synthesize a "block in <enclosing>" entry above this frame. */
    extern struct korb_proc *running_block;
    frame.caller_running_block = running_block;
    c->current_frame = &frame;
    /* Entering a method body — no block is "running" at this point.
     * If we don't reset, a `return` inside a method called from within
     * a block would be interpreted as non-local (target_fp = block's
     * enclosing). */
    struct korb_proc *prev_running = running_block;
    running_block = NULL;
    /* cref must reflect the method's definition site so cref-dependent
     * operations (Kernel#binding, class-variable access, `class C`
     * keyword) see the lexical class.  Most calls have c->cref ==
     * mc->def_cref already (top-level fib, ack, etc.) so guard the
     * swap to skip 4 memory ops per call on the hot path. */
    bool cref_swapped = (mc->def_cref != NULL && c->cref != mc->def_cref);
    if (UNLIKELY(cref_swapped)) {
        prev_cref = c->cref;
        c->cref = mc->def_cref;
    }
    if (UNLIKELY(!simple)) {
        prev_block = current_block;
        current_block = block;
    }

    for (uint32_t i = total; i < mc->locals_cnt; i++) {
        new_fp[i] = Qnil;
    }
    c->self = recv;

    /* Direct call: linker resolves static_disp to a concrete SD_*
     * symbol; gcc emits a direct call instead of going through
     * mc->dispatcher (one indirect load + indirect call removed). */
    VALUE r = static_disp(c, mc->body, new_fp);

    c->current_frame = frame.prev;
    running_block = prev_running;
    if (UNLIKELY(cref_swapped)) c->cref = prev_cref;
    if (UNLIKELY(!simple)) {
        current_block = prev_block;
    }
    korb_proc_snapshot_env_maybe(r, new_fp, new_fp + mc->locals_cnt);
    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        korb_proc_snapshot_env_maybe(c->state_value, new_fp, new_fp + mc->locals_cnt);
    }
    if (UNLIKELY(frame.bindings_head != NULL)) {
        korb_binding_snapshot_frame(&frame);
    }
    c->fp = prev_fp;
    /* Zero-fill popped slots so a sibling call's sp-grow doesn't re-expose
     * stale heap pointers from this frame's locals. */
    for (VALUE *p = prev_sp; p < c->sp; p++) *p = Qnil;
    c->sp = prev_sp;
    c->self = prev_self;

    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        bool consume_return = (c->state == KORB_RETURN &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        /* break with NULL target: legacy "any method consumes" path
         * (yield-style break from a cfunc-driven loop, or break that
         * already escaped its inner while/loop).  break with concrete
         * target: only the matching frame consumes (set in proc_call
         * for &block-yield style escapes). */
        bool consume_break = (c->state == KORB_BREAK &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        if (consume_break || consume_return) {
            r = c->state_value;
            c->state = KORB_NORMAL;
            c->state_value = Qnil;
            c->state_target_frame = NULL;
        }
    }
    return r;
}

#endif
