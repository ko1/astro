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

/* c->current_block is the active block-for-yield (held on CTX so future
 * Fiber/Thread support gets a per-ctx field for free). */

/* Forward declared from object.c — heap-snapshots a returned Proc's
 * env if it points into the about-to-be-popped frame. */
void korb_proc_snapshot_env_if_in_frame(VALUE v, VALUE *fp_lo, VALUE *fp_hi);

/* CFUNC (NEW sp-based RESULT-returning prologue).
 *
 * Caller has already staged self and args on the value stack:
 *   sp[-argc-1] = self  (= recv)
 *   sp[-argc..-1] = args
 *   c->sp_top == sp  (= post-staging top)
 *
 * The cfunc body receives the same sp and returns RESULT.  State
 * propagation is in-band (no c->state side-channel).  Frame is pushed
 * with frame.self = recv so visit_roots tracks it through the chain.
 *
 * Used when mc->cfunc_r is non-NULL.  Phase 2 transitional; eventually
 * the only cfunc path. */
static inline __attribute__((always_inline)) RESULT
prologue_cfunc_r_inl(CTX *c, struct Node *callsite,
                     int argc, VALUE *sp,
                     struct korb_proc *block, struct method_cache *mc)
{
    VALUE recv = sp[-argc - 1];
    struct korb_proc *prev_block = c->current_block;
    c->current_block = block;
    struct korb_frame cfr = {
        .prev = c->current_frame,
        .self = recv,
        .method = mc->method,
        .block = block,
        .caller_node = callsite,
        .fp = c->current_frame->fp,
        .locals_cnt = 0,
        .super_skip_n = 0,
        .last_line = Qnil,
        .last_match = Qnil,
        .caller_running_block = NULL,
        .frame_id = 0,
        .bindings_head = NULL,
    };
    c->current_frame = &cfr;
    struct Node *prev_cs = c->last_cfunc_callsite;
    c->last_cfunc_callsite = callsite;
    RESULT r = mc->cfunc_r(c, argc, sp);
    c->last_cfunc_callsite = prev_cs;
    c->current_frame = cfr.prev;
    c->current_block = prev_block;
    /* break-from-block: convert KORB_BREAK back to NORMAL with the carried value. */
    if (UNLIKELY(block && r.state == KORB_BREAK)) {
        r = RESULT_OK(r.value);
    }
    return r;
}

/* AST-method prologue, parameterized by PARAMS_KNOWN at compile time so
 * each argc-specialized variant unrolls the locals-fill loop.  The
 * SIMPLE_FRAME flag (set on methods whose body has no super / yield /
 * block_given? / const access / blocked call) lets us skip c->current_block
 * save/restore, cref save/restore, and frame chain setup — about 10
 * stores per call on tight recursive paths (fib / ack / tak / incr). */
static inline __attribute__((always_inline)) RESULT
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
        VALUE last = c->current_frame->fp[arg_index + argc - 1];
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
        return korb_raise(c, (struct korb_class *)eArg,
                          "wrong number of arguments (given %u, expected %u)",
                          argc, total);
    }
    /* Save outer fp only for new_fp computation; no C-local save of
     * self — outer frame's self is preserved via the frame chain
     * (visit_roots walks &outer_frame->self).  Writing back a C-local
     * "prev_self" after body would overwrite the freshly-updated outer
     * frame.self with a stale pointer that GC has long since moved. */
    VALUE *prev_sp = c->sp_top;
    VALUE *prev_fp = c->current_frame->fp;
    VALUE *new_fp = prev_fp + arg_index;
    /* Grow c->sp_top + zero-fill the newly-exposed slots so stale heap
     * pointers left over in those addresses (from prior frames popped
     * at this location) don't get treated as live roots by the next
     * visit_roots. */
    {
        VALUE *new_sp = new_fp + mc->locals_cnt;
        if (new_sp > c->sp_top) {
            for (VALUE *p = c->sp_top; p < new_sp; p++) *p = Qnil;
            c->sp_top = new_sp;
        }
    }

    /* Heavy state save/restore only when method body actually uses it. */
    bool simple = mc->is_simple_frame;
    struct korb_proc *prev_block = NULL;
    /* Capture outer cref BEFORE pushing the frame — the original code
     * captured it AFTER push by reading c->current_frame->cref (= the
     * new frame's UNINIT cref field on C stack), which then later
     * wrote that uninit-garbage value back to outer.cref on exit (=
     * silently broke the outer cref chain → PURGE-mode SEGV on next
     * class lookup). */
    struct korb_cref *prev_cref = c->current_frame->cref;
    /* Always push a minimal frame for backtrace.  Heavy state save
     * (block/cref/c->current_block) only when the body actually uses it.
     * fp + locals_cnt are recorded too so Kernel#binding /
     * __capture_lvars__ can read the active method's slots. */
    struct korb_frame frame;
    frame.prev = c->current_frame;
    frame.method = mc->method;
    frame.self = recv;
    frame.block = block;
    frame.caller_node = callsite;
    frame.fp = new_fp;
    /* Inherit cref/current_class/current_file so visit_roots phase (c)
     * walking c->current_frame->cref reaches the outer chain. */
    frame.cref = prev_cref;
    frame.current_class = c->current_frame->current_class;
    frame.current_file = c->current_frame->current_file;
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
    frame.caller_running_block = c->running_block;
    c->current_frame = &frame;
    /* Entering a method body — no block is "running" at this point.
     * If we don't reset, a `return` inside a method called from within
     * a block would be interpreted as non-local (target_fp = block's
     * enclosing). */
    struct korb_proc *prev_running = c->running_block;
    c->running_block = NULL;
    /* cref must reflect the method's definition site so cref-dependent
     * operations (Kernel#binding, class-variable access, `class C`
     * keyword) see the lexical class.  Most calls have c->current_frame->cref ==
     * mc->def_cref already (top-level fib, ack, etc.) so guard the
     * swap to skip 4 memory ops per call on the hot path. */
    bool cref_swapped = (mc->def_cref != NULL && c->current_frame->cref != mc->def_cref);
    if (UNLIKELY(cref_swapped)) {
        /* prev_cref already captured BEFORE push — just swap. */
        c->current_frame->cref = mc->def_cref;
    }
    if (UNLIKELY(!simple)) {
        prev_block = c->current_block;
        c->current_block = block;
    }

    for (uint32_t i = total; i < mc->locals_cnt; i++) {
        new_fp[i] = Qnil;
    }
    c->current_frame->self = recv;

    /* baruby convention: body's `sp` parameter = frame TOP (= fp +
     * locals_cnt).  Body's local `i` is accessed as `sp[i - locals_cnt]`
     * (= negative offset, baked by walker). */
    RESULT _br = EVAL(c, mc->body, new_fp + mc->locals_cnt);

    c->current_frame = frame.prev;
    c->running_block = prev_running;
    if (UNLIKELY(cref_swapped)) c->current_frame->cref = prev_cref;
    if (UNLIKELY(!simple)) {
        c->current_block = prev_block;
    }
    /* If we're returning a Proc whose env points into the about-to-be-
     * popped frame, heap-snapshot it so the next stack push doesn't
     * trash the closure's captured state. */
    korb_proc_snapshot_env_maybe(_br.value, new_fp, new_fp + mc->locals_cnt);
    /* Bindings created in this frame's lifetime: copy fp slots into
     * their heap snapshots so they hold final values after the frame
     * pops (CRuby heap-promote approximation). */
    if (UNLIKELY(frame.bindings_head != NULL)) {
        korb_binding_snapshot_frame(&frame);
    }
    /* Always restore sp.  Without this, every method call leaves sp
     * higher than before — long-running loops with many calls hit
     * stack_end and false-overflow.  Cheap (one store per call).
     *
     * Zero-fill the popped slots [prev_sp, c->sp_top) before lowering sp.
     * Subsequent sp-up by a sibling/later call will re-expose those
     * addresses; without the zero-clear the new frame would inherit
     * this method's stale heap pointers. */
    for (VALUE *p = prev_sp; p < c->sp_top; p++) *p = Qnil;
    c->sp_top = prev_sp;

    if (UNLIKELY(_br.state == KORB_RETURN || _br.state == KORB_BREAK)) {
        bool consume_return = (_br.state == KORB_RETURN &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        /* break with NULL target: legacy "any method consumes" path
         * (yield-style break from a cfunc-driven loop, or break that
         * already escaped its inner while/loop).  break with concrete
         * target: only the matching frame consumes (set in proc_call
         * for &block-yield style escapes). */
        bool consume_break = (_br.state == KORB_BREAK &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        if (consume_break || consume_return) {
            c->state_target_frame = NULL;
            return RESULT_OK(_br.value);
        }
    }
    return _br;
}

/* AOT-baked variant: dispatcher is supplied as an argument.  The C
 * compiler then sees a known target at the call site (when STATIC_DISP
 * is a constant function symbol), turning the indirect `mc->dispatcher`
 * call into a direct call which gcc can in turn inline at -O3.  Used
 * by the AOT specializer when it can statically determine that a hot
 * call site always reaches a specific method body. */
static inline __attribute__((always_inline)) RESULT
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
        return korb_raise(c, (struct korb_class *)eArg,
                          "wrong number of arguments (given %u, expected %u)",
                          argc, total);
    }
    /* Save outer fp only for new_fp computation; no C-local save of
     * self — outer frame's self is preserved via the frame chain
     * (visit_roots walks &outer_frame->self).  Writing back a C-local
     * "prev_self" after body would overwrite the freshly-updated outer
     * frame.self with a stale pointer that GC has long since moved. */
    VALUE *prev_sp = c->sp_top;
    VALUE *prev_fp = c->current_frame->fp;
    VALUE *new_fp = prev_fp + arg_index;
    /* Grow c->sp_top + zero-fill the newly-exposed slots so stale heap
     * pointers left over in those addresses (from prior frames popped
     * at this location) don't get treated as live roots by the next
     * visit_roots. */
    {
        VALUE *new_sp = new_fp + mc->locals_cnt;
        if (new_sp > c->sp_top) {
            for (VALUE *p = c->sp_top; p < new_sp; p++) *p = Qnil;
            c->sp_top = new_sp;
        }
    }

    bool simple = mc->is_simple_frame;
    struct korb_proc *prev_block = NULL;
    /* Capture outer cref BEFORE push (same fix as simple_inl). */
    struct korb_cref *prev_cref = c->current_frame->cref;
    struct korb_frame frame;
    frame.prev = c->current_frame;
    frame.method = mc->method;
    frame.self = recv;
    frame.block = block;
    frame.caller_node = callsite;
    frame.fp = new_fp;
    frame.cref = prev_cref;
    frame.current_class = c->current_frame->current_class;
    frame.current_file = c->current_frame->current_file;
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
    frame.caller_running_block = c->running_block;
    c->current_frame = &frame;
    /* Entering a method body — no block is "running" at this point.
     * If we don't reset, a `return` inside a method called from within
     * a block would be interpreted as non-local (target_fp = block's
     * enclosing). */
    struct korb_proc *prev_running = c->running_block;
    c->running_block = NULL;
    /* cref must reflect the method's definition site so cref-dependent
     * operations (Kernel#binding, class-variable access, `class C`
     * keyword) see the lexical class.  Most calls have c->current_frame->cref ==
     * mc->def_cref already (top-level fib, ack, etc.) so guard the
     * swap to skip 4 memory ops per call on the hot path. */
    bool cref_swapped = (mc->def_cref != NULL && c->current_frame->cref != mc->def_cref);
    if (UNLIKELY(cref_swapped)) {
        /* prev_cref already captured BEFORE push — just swap. */
        c->current_frame->cref = mc->def_cref;
    }
    if (UNLIKELY(!simple)) {
        prev_block = c->current_block;
        c->current_block = block;
    }

    for (uint32_t i = total; i < mc->locals_cnt; i++) {
        new_fp[i] = Qnil;
    }
    c->current_frame->self = recv;

    /* Direct call: linker resolves static_disp to a concrete SD_*
     * symbol; gcc emits a direct call instead of going through
     * mc->dispatcher (one indirect load + indirect call removed). */
    RESULT _br = static_disp(c, mc->body, new_fp + mc->locals_cnt);

    c->current_frame = frame.prev;
    c->running_block = prev_running;
    if (UNLIKELY(cref_swapped)) c->current_frame->cref = prev_cref;
    if (UNLIKELY(!simple)) {
        c->current_block = prev_block;
    }
    korb_proc_snapshot_env_maybe(_br.value, new_fp, new_fp + mc->locals_cnt);
    if (UNLIKELY(frame.bindings_head != NULL)) {
        korb_binding_snapshot_frame(&frame);
    }
    /* Zero-fill popped slots so a sibling call's sp-grow doesn't re-expose
     * stale heap pointers from this frame's locals. */
    for (VALUE *p = prev_sp; p < c->sp_top; p++) *p = Qnil;
    c->sp_top = prev_sp;

    if (UNLIKELY(_br.state == KORB_RETURN || _br.state == KORB_BREAK)) {
        bool consume_return = (_br.state == KORB_RETURN &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        bool consume_break = (_br.state == KORB_BREAK &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        if (consume_break || consume_return) {
            c->state_target_frame = NULL;
            return RESULT_OK(_br.value);
        }
    }
    return _br;
}

#endif
