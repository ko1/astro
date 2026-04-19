#include "builtin.h"

// Convert a stack-allocated abruby_block into a heap Proc.  Defined
// in abruby.c so it has access to rb_cAbRubyNode and the data type
// table.
extern VALUE abruby_block_to_proc(CTX *c, const struct abruby_block *blk, bool is_lambda);

// Bind a Proc's argv to its parameter slots in the proc's env, using
// the same Proc-style binding semantics as abruby_yield (auto-splat
// when n_params >= 2 and argv is a single Array; pad missing slots
// with nil; drop extras).
static void
proc_bind_args(CTX *c, struct abruby_proc *p, unsigned int argc, VALUE *argv)
{
    unsigned int n_params = p->params_cnt;
    if (n_params == 0) return;
    VALUE * restrict dst = p->env + p->param_base;
    if (n_params >= 2 && argc == 1 &&
            AB_CLASS_OF(c, argv[0]) == c->abm->array_class) {
        VALUE rb_ary = RARY(argv[0]);
        long len = RARRAY_LEN(rb_ary);
        unsigned int n_copy = ((unsigned long)len < n_params) ? (unsigned int)len : n_params;
        for (unsigned int i = 0; i < n_copy; i++) {
            dst[i] = RARRAY_AREF(rb_ary, i);
        }
        for (unsigned int i = n_copy; i < n_params; i++) {
            dst[i] = Qnil;
        }
    }
    else {
        unsigned int n_copy = argc < n_params ? argc : n_params;
        for (unsigned int i = 0; i < n_copy; i++) {
            dst[i] = argv[i];
        }
        for (unsigned int i = n_copy; i < n_params; i++) {
            dst[i] = Qnil;
        }
    }
}

// Proc.new / proc / lambda — find the block to wrap.  Two cases:
//   1) Block at the call site (`Proc.new { ... }`): the cfunc itself
//      was dispatched via the with_block path so its frame has `.block`
//      set directly.
//   2) Bare `Proc.new` inside a method that received a block: walk
//      back through the cfunc frame to the enclosing method and grab
//      *its* block.
static const struct abruby_block *
current_block_for_proc_new(CTX *c)
{
    if (!c->current_frame) return NULL;
    if (c->current_frame->block) return c->current_frame->block;
    const struct abruby_frame *caller = c->current_frame->prev;
    if (!caller) return NULL;
    return caller->block;
}

RESULT ab_proc_new(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc; (void)argv;
    const struct abruby_block *blk = current_block_for_proc_new(c);
    if (!blk) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "tried to create Proc object without a block"));
        return (RESULT){exc, RESULT_RAISE};
    }
    return RESULT_OK(abruby_block_to_proc(c, blk, /*is_lambda=*/false));
}

// Proc#call(*args) — execute the proc body with args bound to params.
//
// Implementation notes:
//   - We swap c->current_frame->fp to the proc's heap env (so lvar reads/writes inside
//     the body see the closure environment)
//   - Push a frame so backtraces and `return` from the body have a
//     boundary to land at
//   - Install the captured cref so bare-constant lookups work
//   - Save/restore current_block context so a `yield` from the body
//     references whatever block was passed *to* the proc... but Proc
//     doesn't take a block, so we set current_block to NULL while
//     executing the body
RESULT ab_proc_call(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_proc *p = (struct abruby_proc *)ABRUBY_DATA_PTR(self);

    proc_bind_args(c, p, argc, argv);

    const struct abruby_block *save_current_block = c->current_block;
    const struct abruby_frame *save_current_block_frame = c->current_block_frame;
    struct abruby_frame *save_frame = c->current_frame;

    // Copy captured env onto the CTX's VALUE stack so method calls
    // inside the body have headroom to grow fp upward.
    VALUE *new_fp = argv + argc;
    ABRUBY_ASSERT(new_fp + p->env_size <= c->stack + ABRUBY_STACK_SIZE);
    for (uint32_t i = 0; i < p->env_size; i++) new_fp[i] = p->env[i];

    // Push a frame pointing at the proc's stable entry (inherited from
    // the block literal it was captured from).  Bump dispatch_count so
    // proc invocations count toward PG profile the same way yields do.
    // Fallback: if the proc somehow has no entry (shouldn't happen),
    // build a synthetic one so EVAL still works.
    struct abruby_entry fallback_entry;
    struct abruby_entry *proc_entry = p->entry;
    if (proc_entry == NULL) {
        fallback_entry.kind = ABRUBY_ENTRY_BLOCK;
        fallback_entry.body = p->body;
        fallback_entry.cref = p->cref;
        fallback_entry.source_file = "(proc)";
        fallback_entry.stack_limit = p->env_size;
        fallback_entry.dispatch_count = 0;
        fallback_entry.u.block.params_cnt = p->params_cnt;
        fallback_entry.u.block.param_base = p->param_base;
        fallback_entry.u.block.env_size   = p->env_size;
        proc_entry = &fallback_entry;
    }
    proc_entry->cref = p->cref;
    proc_entry->dispatch_count++;
    struct abruby_frame proc_frame;
    proc_frame.prev = save_frame;
    proc_frame.caller_node = NULL;
    proc_frame.block = NULL;
    proc_frame.self = p->captured_self;
    proc_frame.fp = new_fp;
    proc_frame.entry = proc_entry;
    c->current_block = NULL;
    c->current_block_frame = NULL;
    c->current_frame = &proc_frame;

    RESULT r = EVAL(c, p->body);

    // Propagate closure-local writes back to the heap env so other
    // escapes of the same closure observe them.
    for (uint32_t i = 0; i < p->env_size; i++) p->env[i] = new_fp[i];
    c->current_block = save_current_block;
    c->current_block_frame = save_current_block_frame;
    c->current_frame = save_frame;

    // Demote NEXT (used by `next` inside the proc body) to NORMAL.
    r.state &= ~(unsigned)RESULT_NEXT;
    if (r.state & RESULT_RETURN) {
        if (p->is_lambda) {
            // `return` in a lambda exits just the lambda.
            r.state &= ~(unsigned)RESULT_RETURN_CATCH_MASK;
        } else {
            // Non-lambda Proc: `return` propagates up through this
            // cfunc's dispatch boundary so the *enclosing* user method
            // is what gets exited.  Bump the skip count by one so the
            // dispatch_method_frame around this cfunc decrements it
            // back to whatever the user wrote (typically 0) instead of
            // catching here.
            r.state += RESULT_SKIP_UNIT;
        }
    }
    return r;
}

RESULT ab_proc_lambda_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    const struct abruby_proc *p = (const struct abruby_proc *)ABRUBY_DATA_PTR(self);
    return RESULT_OK(p->is_lambda ? Qtrue : Qfalse);
}

RESULT ab_proc_arity(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    const struct abruby_proc *p = (const struct abruby_proc *)ABRUBY_DATA_PTR(self);
    // Lambdas report exact arity; ordinary Procs report -required-1
    // for any non-zero count of optional/rest, but we only support
    // required params, so it's just `params_cnt` either way.
    return RESULT_OK(LONG2FIX((long)p->params_cnt));
}

RESULT ab_proc_to_proc(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    return RESULT_OK(self);
}

// Kernel#proc { ... } — same as Proc.new for non-lambda procs.
RESULT ab_kernel_proc(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc; (void)argv;
    const struct abruby_block *blk = current_block_for_proc_new(c);
    if (!blk) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "tried to create Proc object without a block"));
        return (RESULT){exc, RESULT_RAISE};
    }
    return RESULT_OK(abruby_block_to_proc(c, blk, /*is_lambda=*/false));
}

// Kernel#lambda { ... } — same with stricter return semantics.
RESULT ab_kernel_lambda(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc; (void)argv;
    const struct abruby_block *blk = current_block_for_proc_new(c);
    if (!blk) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "tried to create Proc object without a block"));
        return (RESULT){exc, RESULT_RAISE};
    }
    return RESULT_OK(abruby_block_to_proc(c, blk, /*is_lambda=*/true));
}
