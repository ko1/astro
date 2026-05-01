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

/* CFUNC: just call the C function, then handle break-from-block. */
static inline __attribute__((always_inline)) VALUE
prologue_cfunc_inl(CTX *c, struct Node *callsite, VALUE recv,
                   uint32_t argc, uint32_t arg_index,
                   struct korb_proc *block, struct method_cache *mc)
{
    (void)callsite;
    VALUE *argv = &c->fp[arg_index];
    struct korb_proc *prev_block = current_block;
    current_block = block;
    VALUE prev_self = c->self;
    c->self = recv;
    VALUE r = mc->cfunc(c, recv, argc, argv);
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
 * each argc-specialized variant unrolls the locals-fill loop. */
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
    struct korb_proc *prev_block = current_block;
    struct korb_cref *prev_cref = c->cref;
    current_block = block;

    VALUE *new_fp = prev_fp + arg_index;
    c->fp = new_fp;
    if (new_fp + mc->locals_cnt > c->sp) c->sp = new_fp + mc->locals_cnt;
    if (mc->def_cref) c->cref = mc->def_cref;

    for (uint32_t i = total; i < mc->locals_cnt; i++) {
        new_fp[i] = Qnil;
    }
    c->self = recv;

    struct korb_frame frame;
    frame.prev = c->current_frame;
    frame.method = mc->method;
    frame.self = recv;
    frame.block = block;
    c->current_frame = &frame;
    VALUE r = mc->dispatcher(c, mc->body);
    c->current_frame = frame.prev;
    c->fp = prev_fp;
    c->self = prev_self;
    c->cref = prev_cref;
    current_block = prev_block;

    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

#endif
