// Fiber implementation for abruby — CRuby fiber API based.
//
// Each non-main fiber is backed by a CRuby Fiber (rb_fiber_new).  Using
// CRuby's own fiber scheduler ensures:
//   - GC's machine-stack scan automatically covers the right C-stack range
//   - No manual mmap / ucontext / swapcontext is needed
//
// Resume / yield delegate to rb_fiber_resume / rb_fiber_yield.

#include "builtin.h"

extern VALUE abruby_block_to_proc(CTX *c, const struct abruby_block *blk, bool is_lambda);

// Callback struct passed as the TypedData argument to rb_fiber_new.
// We store the abruby fiber's rb_wrapper VALUE (not a raw pointer to
// the fiber struct) so that GC sweep order does not matter: the mark
// function marks the VALUE, and crb_fiber_body recovers the fiber
// via ABRUBY_DATA_PTR on the wrapper.
struct crb_fiber_callback {
    VALUE rb_wrapper;   // T_DATA VALUE wrapping the abruby_fiber struct
};

static void crb_fiber_callback_mark(void *ptr) {
    // Mark the abruby fiber's rb_wrapper to keep it (and the fiber
    // struct it wraps) alive while the CRuby fiber is alive.  CRuby's
    // cont_mark scans the fiber's saved machine stack and may find
    // abruby VALUES there; if the fiber struct were freed, those
    // VALUES would be stale and crash rb_gc_mark_locations.
    //
    // This creates a reference cycle (CRuby fiber -> callback ->
    // rb_wrapper -> abruby_fiber_mark -> crb_fiber -> CRuby fiber) but
    // mark-sweep GC handles cycles correctly: once nothing roots the
    // CRuby fiber, the entire cluster is collected together.
    const struct crb_fiber_callback *cb = (const struct crb_fiber_callback *)ptr;
    if (cb->rb_wrapper && !RB_SPECIAL_CONST_P(cb->rb_wrapper)) {
        rb_gc_mark(cb->rb_wrapper);
    }
}
static void crb_fiber_callback_free(void *ptr) {
    // The callback struct is a small heap allocation owned by the CRuby
    // fiber; free it when the CRuby fiber is collected.
    ruby_xfree(ptr);
}
static const rb_data_type_t crb_fiber_callback_type = {
    "AbRuby::FiberCallback",
    { crb_fiber_callback_mark, crb_fiber_callback_free, NULL },
    0, 0, 0
};

// Trampoline that runs inside the CRuby fiber.
// rb_block_call_func_t signature: yielded_arg is the first resume arg,
// callback_obj is the value passed as the 2nd argument to rb_fiber_new.
static VALUE
crb_fiber_body(VALUE yielded_arg, VALUE callback_obj, int argc, const VALUE *argv, VALUE blockarg)
{
    (void)yielded_arg; (void)argc; (void)argv; (void)blockarg;
    struct crb_fiber_callback *cb =
        (struct crb_fiber_callback *)ABRUBY_DATA_PTR(callback_obj);
    struct abruby_fiber *f =
        (struct abruby_fiber *)ABRUBY_DATA_PTR(cb->rb_wrapper);

    // The first resume delivers nargs/argv packed by fiber_switch_to into
    // transfer_value as a Ruby Array [nargs, arg0, arg1, ...].
    VALUE packed = f->transfer_value;
    f->transfer_value = Qnil;

    unsigned int nargs = 0;
    VALUE local_argv[16];
    if (RB_TYPE_P(packed, T_ARRAY)) {
        long n = RARRAY_LEN(packed);
        if (n > 0) {
            // first element is nargs as Fixnum
            nargs = (unsigned int)FIX2ULONG(RARRAY_AREF(packed, 0));
            if (nargs > 15) nargs = 15;
            for (unsigned int i = 0; i < nargs; i++) {
                local_argv[i] = RARRAY_AREF(packed, (long)(i + 1));
            }
        }
    }

    CTX *c = &f->ctx;
    // Place argv on the fiber's VALUE stack (not C stack) so
    // ab_proc_call's `new_fp = argv + argc` stays inside the CTX
    // stack — otherwise fp would point into the CRuby fiber's
    // machine stack, causing abm_mark to scan an invalid range.
    for (unsigned int i = 0; i < nargs; i++) c->current_frame->fp[i] = local_argv[i];

    extern RESULT ab_proc_call(CTX *, VALUE, unsigned int, VALUE *);
    RESULT r = ab_proc_call(c, f->proc_value, nargs, c->current_frame->fp);

    // Body finished.
    f->done_state = r.state;
    f->state = ABRUBY_FIBER_DONE;

    VALUE final = (r.state == RESULT_NORMAL || (r.state & RESULT_RAISE))
                  ? r.value : Qnil;

    // Return the final value to the resumer (rb_fiber_resume returns it).
    return final;
}

// Heap-alloc + initialise an abruby_fiber.  proc_value is the Proc
// returned by Fiber.new's block argument.  The created fiber is in NEW
// state until first resume.
static struct abruby_fiber *
fiber_alloc(struct abruby_machine *abm, VALUE proc_value)
{
    struct abruby_fiber *f = (struct abruby_fiber *)ruby_xcalloc(1, sizeof(struct abruby_fiber));
    f->state = ABRUBY_FIBER_NEW;
    f->is_main = false;
    f->obj_type = ABRUBY_OBJ_FIBER;
    f->proc_value = proc_value;
    f->transfer_value = Qnil;
    f->done_state = 0;
    f->resumer = NULL;
    f->abm = abm;
    f->rb_wrapper = Qnil;
    f->crb_fiber = Qnil;
    // Bootstrap the fiber's CTX from the current fiber so cross-fiber
    // reads of e.g. ids work straight away.  ctx.abm / ctx.ids are const;
    // cast for init-only assignment.
    *(struct abruby_machine **)&f->ctx.abm = abm;
    // sp removed — GC uses frame-walk + entry->stack_limit
    f->ctx.current_class = NULL;
    // Set up root frame for this fiber
    memset(&f->root_frame, 0, sizeof(struct abruby_frame));
    f->root_frame.fp = f->ctx.stack;
    f->root_frame.self = abm->current_fiber->ctx.current_frame->self;
    extern struct abruby_entry abruby_empty_entry; f->root_frame.entry = &abruby_empty_entry;
    f->ctx.current_frame = &f->root_frame;
    *(const struct abruby_id_cache **)&f->ctx.ids = abm->current_fiber->ctx.ids;
    f->ctx.current_block = NULL;
    f->ctx.current_block_frame = NULL;
    return f;
}

// Switch from the current fiber to `target`, transferring `argc/argv`
// to it.  On first resume the arg list becomes the block params; on
// subsequent resumes only argv[0] is used (becomes Fiber.yield's
// return value).  The function returns whatever value the target
// eventually transfers back to us.
static RESULT
fiber_switch_to(CTX *c, struct abruby_fiber *target, unsigned int argc, VALUE *argv)
{
    struct abruby_machine *abm = c->abm;
    struct abruby_fiber *cur = abm->current_fiber;

    if (target->state == ABRUBY_FIBER_DONE) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "dead fiber called"));
        return (RESULT){exc, RESULT_RAISE};
    }

    target->resumer = cur;
    target->state = ABRUBY_FIBER_RUNNING;
    if (cur->state == ABRUBY_FIBER_RUNNING) {
        cur->state = ABRUBY_FIBER_SUSPENDED;
    }

    abm->current_fiber = target;

    VALUE ret_val;
    bool first = (target->crb_fiber == Qnil);
    if (first) {
        // First resume: pack argc/argv into transfer_value for crb_fiber_body
        // to unpack after the CRuby fiber starts.
        VALUE packed = rb_ary_new_capa((long)(argc + 1));
        rb_ary_push(packed, ULONG2NUM(argc));
        for (unsigned int i = 0; i < argc; i++) rb_ary_push(packed, argv[i]);
        RB_OBJ_WRITE(target->rb_wrapper, &target->transfer_value, packed);

        // Create the CRuby fiber.  Pass a typed-data wrapper of a small
        // callback struct so crb_fiber_body can find the abruby_fiber.
        // The callback stores the fiber's rb_wrapper VALUE (not a raw
        // pointer) so GC sweep order between the fiber wrapper and the
        // CRuby fiber does not cause use-after-free.
        struct crb_fiber_callback *cb =
            (struct crb_fiber_callback *)ruby_xcalloc(1, sizeof(struct crb_fiber_callback));
        cb->rb_wrapper = target->rb_wrapper;
        VALUE cb_val = TypedData_Wrap_Struct(rb_cObject, &crb_fiber_callback_type, cb);
        RB_OBJ_WRITE(target->rb_wrapper, &target->crb_fiber, rb_fiber_new(crb_fiber_body, cb_val));

        // Start the fiber.  The return value is whatever crb_fiber_body returns
        // (the body's final value) or whatever rb_fiber_yield delivers on
        // subsequent yields.
        ret_val = rb_fiber_resume(target->crb_fiber, 0, NULL);
    } else {
        // Subsequent resume: pass the single value as-is.
        VALUE resume_arg = (argc >= 1) ? argv[0] : Qnil;
        ret_val = rb_fiber_resume(target->crb_fiber, 1, &resume_arg);
    }

    // Returned from the resume (either the fiber yielded or completed).
    abm->current_fiber = cur;
    cur->state = ABRUBY_FIBER_RUNNING;

    // If the target finished by raising, surface that on the caller.
    if (target->state == ABRUBY_FIBER_DONE && (target->done_state & RESULT_RAISE)) {
        // ret_val is the exception VALUE
        return (RESULT){ret_val, RESULT_RAISE};
    }
    return RESULT_OK(ret_val);
}

// Fiber.new { ... } — create a Fiber object backed by the given block.
RESULT ab_fiber_new(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc; (void)argv;

    // The block at the call site has been pushed onto the cfunc frame
    // (.block).  Resolve it the same way Proc.new does.
    extern const struct abruby_block *abruby_current_block_for_proc_new(CTX *c);
    const struct abruby_block *blk = NULL;
    if (c->current_frame) {
        blk = c->current_frame->block;
        if (!blk && c->current_frame->prev) {
            blk = c->current_frame->prev->block;
        }
    }
    if (!blk) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "tried to create Fiber object without a block"));
        return (RESULT){exc, RESULT_RAISE};
    }
    VALUE proc_val = abruby_block_to_proc(c, blk, /*is_lambda=*/false);

    extern VALUE abruby_fiber_wrap(struct abruby_machine *abm, struct abruby_fiber *f);
    struct abruby_fiber *f = fiber_alloc(c->abm, proc_val);
    return RESULT_OK(abruby_fiber_wrap(c->abm, f));
}

// Fiber#resume(*args) — first call starts the fiber's body, subsequent
// calls return from the most recent Fiber.yield with the args.
RESULT ab_fiber_resume(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_fiber *f = (struct abruby_fiber *)ABRUBY_DATA_PTR(self);
    if (f->state == ABRUBY_FIBER_DONE) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "dead fiber called"));
        return (RESULT){exc, RESULT_RAISE};
    }
    return fiber_switch_to(c, f, argc, argv);
}

// Fiber.yield(*args) — suspend the running fiber, hand control back
// to its resumer.
RESULT ab_fiber_yield(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self;
    struct abruby_fiber *cur = c->abm->current_fiber;
    if (cur == NULL || cur->resumer == NULL) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "can't yield from root fiber"));
        return (RESULT){exc, RESULT_RAISE};
    }

    // Mark fiber as suspended and update abm->current_fiber to the resumer.
    struct abruby_machine *abm = c->abm;
    struct abruby_fiber *resumer = cur->resumer;
    cur->resumer = NULL;
    cur->state = ABRUBY_FIBER_SUSPENDED;
    abm->current_fiber = resumer;
    resumer->state = ABRUBY_FIBER_RUNNING;

    // Yield back to the resumer.  The value passed here becomes the
    // return value of rb_fiber_resume on the resumer's side.
    VALUE yield_val = (argc >= 1) ? argv[0] : Qnil;
    VALUE resumed_val = rb_fiber_yield(1, &yield_val);

    // We've been resumed again.  Restore current_fiber.
    abm->current_fiber = cur;
    cur->state = ABRUBY_FIBER_RUNNING;
    cur->resumer = resumer;

    return RESULT_OK(resumed_val);
}

// Fiber#alive? — returns true unless the fiber finished.
RESULT ab_fiber_alive_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    const struct abruby_fiber *f = (const struct abruby_fiber *)ABRUBY_DATA_PTR(self);
    return RESULT_OK(f->state == ABRUBY_FIBER_DONE ? Qfalse : Qtrue);
}

// Mark / free for fibers — called from abruby.c via the obj_type dispatch.
void abruby_fiber_mark(struct abruby_fiber *f) {
    if (!RB_SPECIAL_CONST_P(f->proc_value))    rb_gc_mark(f->proc_value);
    if (!RB_SPECIAL_CONST_P(f->transfer_value)) rb_gc_mark(f->transfer_value);
    if (f->ctx.current_frame && !RB_SPECIAL_CONST_P(f->ctx.current_frame->self))
        rb_gc_mark(f->ctx.current_frame->self);
    // Mark the CRuby fiber backing this abruby fiber (keeps it alive).
    if (!RB_SPECIAL_CONST_P(f->crb_fiber))      rb_gc_mark(f->crb_fiber);
    // Mark the suspended VALUE stack via frame-walk + entry->stack_limit.
    extern void abm_mark_fiber_stack(const CTX *ctx);
    abm_mark_fiber_stack(&f->ctx);
}

void abruby_fiber_free(struct abruby_fiber *f) {
    // The callback struct stores rb_wrapper (a VALUE), not a raw pointer
    // to this fiber, so there is no dangling-pointer issue regardless of
    // GC sweep order.  Nothing to do here; the struct itself is freed by
    // abruby_data_free's ruby_xfree.
    (void)f;
}
