// Fiber implementation for abruby.
//
// Each non-main fiber owns:
//   - a heap C call stack (mmap-allocated, with a guard page so a
//     stack overflow is a clean SIGSEGV instead of corrupting the
//     adjacent fiber)
//   - a `ucontext_t` for save / restore of the C-level execution state
//   - its own abruby CTX (VALUE stack + frame chain) — when this fiber
//     is current, `vm->current_fiber->ctx` is the CTX the dispatcher
//     reads
//
// Resume / yield switch the fiber pointer (vm->current_fiber) and the
// C call stack (swapcontext) atomically with respect to the rest of
// the interpreter.

#include "builtin.h"
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

// Default per-fiber C stack size.  Optcarrot's main_loop is deep
// (large state machine + cached locals) so 2 MB is the safe default.
#define ABRUBY_FIBER_CSTACK_SIZE (2 * 1024 * 1024)

// Hold the ucontext separately so context.h doesn't need to include
// <ucontext.h>.
struct abruby_fiber_uctx {
    ucontext_t uctx;
};

extern VALUE abruby_block_to_proc(CTX *c, const struct abruby_block *blk, bool is_lambda);

// Heap-alloc + initialise an abruby_fiber.  proc_value is the Proc
// returned by Fiber.new's block argument.  The created fiber is in NEW
// state until first resume.
static struct abruby_fiber *
fiber_alloc(struct abruby_machine *vm, VALUE proc_value)
{
    struct abruby_fiber *f = (struct abruby_fiber *)ruby_xcalloc(1, sizeof(struct abruby_fiber));
    f->state = ABRUBY_FIBER_NEW;
    f->is_main = false;
    f->proc_value = proc_value;
    f->transfer_value = Qnil;
    f->done_state = 0;
    f->resumer = NULL;
    f->abm = vm;
    f->rb_wrapper = Qnil;
    // Bootstrap the fiber's CTX from the current fiber so cross-fiber
    // reads of e.g. ids work straight away.
    f->ctx.abm = vm;
    f->ctx.fp = f->ctx.stack;
    f->ctx.self = vm->current_fiber->ctx.self;
    f->ctx.current_class = NULL;
    f->ctx.cref = NULL;
    f->ctx.current_frame = NULL;
    f->ctx.ids = vm->current_fiber->ctx.ids;
    f->ctx.current_block = NULL;
    f->ctx.current_block_frame = NULL;
    return f;
}

static void
fiber_alloc_cstack(struct abruby_fiber *f)
{
    f->cstack_size = ABRUBY_FIBER_CSTACK_SIZE;
    // mmap with a guard page below so a stack overflow gets a real
    // SIGSEGV instead of trampling the next allocation.
    long pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize <= 0) pagesize = 4096;
    size_t total = f->cstack_size + (size_t)pagesize;
    void *p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        rb_raise(rb_eNoMemError, "fiber: mmap failed for stack");
    }
    // Mark the bottom page as a guard.
    mprotect(p, (size_t)pagesize, PROT_NONE);
    f->cstack = (char *)p + pagesize;
    // ucontext lives heap-side too so context.h can stay clean.
    f->uctx = (struct abruby_fiber_uctx *)ruby_xcalloc(1, sizeof(struct abruby_fiber_uctx));
}

static void
fiber_free_cstack(struct abruby_fiber *f)
{
    if (f->cstack) {
        long pagesize = sysconf(_SC_PAGESIZE);
        if (pagesize <= 0) pagesize = 4096;
        void *base = (char *)f->cstack - pagesize;
        munmap(base, f->cstack_size + (size_t)pagesize);
        f->cstack = NULL;
    }
    if (f->uctx) {
        ruby_xfree(f->uctx);
        f->uctx = NULL;
    }
}

// Trampoline that runs on the new fiber's C stack.  ucontext entry
// points must take int args, so we stash the actual fiber pointer in
// a static-locked spot before swapcontext.  See fiber_first_resume.
static struct abruby_fiber *fiber_starting_self;

// First-resume args, also stashed before swapcontext.
static unsigned int fiber_starting_argc;
static VALUE *fiber_starting_argv;

static void
fiber_entry(void)
{
    struct abruby_fiber *f = fiber_starting_self;
    fiber_starting_self = NULL;
    unsigned int argc = fiber_starting_argc;
    // Copy argv into a local buffer on the fiber's own C stack — the
    // caller's argv lives on its stack which we just swapped away from.
    VALUE local_argv[16];
    if (argc > 16) argc = 16;
    for (unsigned int i = 0; i < argc; i++) local_argv[i] = fiber_starting_argv[i];
    fiber_starting_argv = NULL;
    fiber_starting_argc = 0;

    CTX *c = &f->ctx;
    extern RESULT ab_proc_call(CTX *, VALUE, unsigned int, VALUE *);
    RESULT r = ab_proc_call(c, f->proc_value, argc, local_argv);

    // Body finished.  Stash the result on the *resumer*'s transfer
    // slot so when its swap-from returns, it reads the right value.
    f->done_state = r.state;
    f->state = ABRUBY_FIBER_DONE;

    struct abruby_fiber *resumer = f->resumer;
    f->resumer = NULL;
    VALUE final = (r.state == RESULT_NORMAL || (r.state & RESULT_RAISE))
                  ? r.value : Qnil;
    resumer->transfer_value = final;
    f->abm->current_fiber = resumer;
    swapcontext(&f->uctx->uctx, &resumer->uctx->uctx);
    // Should never return here — once DONE, we are never resumed
    // again.  But just in case the kernel does, loop forever.
    for (;;) { /* unreachable */ }
}

// Set up the ucontext for a brand-new fiber so its first swapcontext
// lands at fiber_entry on the heap stack.
static void
fiber_first_resume_setup(struct abruby_fiber *f)
{
    fiber_alloc_cstack(f);
    getcontext(&f->uctx->uctx);
    f->uctx->uctx.uc_stack.ss_sp   = f->cstack;
    f->uctx->uctx.uc_stack.ss_size = f->cstack_size;
    f->uctx->uctx.uc_link          = NULL;
    fiber_starting_self = f;
    makecontext(&f->uctx->uctx, fiber_entry, 0);
}

// Switch from the current fiber to `target`, transferring `argc/argv`
// to it.  On first resume the arg list becomes the block params; on
// subsequent resumes only argv[0] is used (becomes Fiber.yield's
// return value).  The function returns whatever value the target
// eventually transfers back to us.
static RESULT
fiber_switch_to(CTX *c, struct abruby_fiber *target, unsigned int argc, VALUE *argv)
{
    struct abruby_machine *vm = c->abm;
    struct abruby_fiber *cur = vm->current_fiber;

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

    bool first = (target->uctx == NULL);
    if (first) {
        // First resume — pass the full argv to fiber_entry via the
        // static handoff slots; allocate the target's stack and prime
        // its ucontext.
        fiber_starting_argc = argc;
        fiber_starting_argv = argv;
        fiber_first_resume_setup(target);
    } else {
        // Subsequent resume / yield — only one value is meaningful;
        // it'll come back from the swap as Fiber.yield's return.
        target->transfer_value = (argc >= 1) ? argv[0] : Qnil;
    }

    // The actual swap.  We need a ucontext_t to save current state
    // into.  For the root fiber the first time it switches *out*, we
    // also need to lazily allocate its uctx.
    if (cur->uctx == NULL) {
        cur->uctx = (struct abruby_fiber_uctx *)ruby_xcalloc(1, sizeof(struct abruby_fiber_uctx));
    }

    vm->current_fiber = target;
    if (swapcontext(&cur->uctx->uctx, &target->uctx->uctx) != 0) {
        // swapcontext failed — restore current_fiber and raise
        vm->current_fiber = cur;
        cur->state = ABRUBY_FIBER_RUNNING;
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "fiber: swapcontext failed"));
        return (RESULT){exc, RESULT_RAISE};
    }
    // Returned from the swap — we're running again.  cur is still
    // valid (it's on our C stack).  vm->current_fiber may have been
    // restored to us by the swap-back path.
    vm->current_fiber = cur;
    cur->state = ABRUBY_FIBER_RUNNING;

    VALUE got = cur->transfer_value;
    cur->transfer_value = Qnil;

    // If the target finished by raising, surface that on the caller.
    if (target->state == ABRUBY_FIBER_DONE && (target->done_state & RESULT_RAISE)) {
        return (RESULT){got, RESULT_RAISE};
    }
    return RESULT_OK(got);
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

    extern VALUE abruby_fiber_wrap(struct abruby_machine *vm, struct abruby_fiber *f);
    struct abruby_fiber *f = fiber_alloc(c->abm, proc_val);
    return RESULT_OK(abruby_fiber_wrap(c->abm, f));
}

// Fiber#resume(*args) — first call starts the fiber's body, subsequent
// calls return from the most recent Fiber.yield with the args.
RESULT ab_fiber_resume(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_fiber *f = (struct abruby_fiber *)RTYPEDDATA_GET_DATA(self);
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
    return fiber_switch_to(c, cur->resumer, argc, argv);
}

// Fiber#alive? — returns true unless the fiber finished.
RESULT ab_fiber_alive_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    const struct abruby_fiber *f = (const struct abruby_fiber *)RTYPEDDATA_GET_DATA(self);
    return RESULT_OK(f->state == ABRUBY_FIBER_DONE ? Qfalse : Qtrue);
}

// Mark / free for fibers — called from abruby.c via the obj_type
// dispatch.
void abruby_fiber_mark(struct abruby_fiber *f) {
    if (!RB_SPECIAL_CONST_P(f->proc_value))    rb_gc_mark(f->proc_value);
    if (!RB_SPECIAL_CONST_P(f->transfer_value)) rb_gc_mark(f->transfer_value);
    if (!RB_SPECIAL_CONST_P(f->ctx.self))       rb_gc_mark(f->ctx.self);
    // Mark the suspended VALUE stack so locals captured at yield-time
    // stay alive across the switch.
    VALUE *base = f->ctx.stack;
    VALUE *top = f->ctx.fp;
    if (top && top >= base) {
        for (VALUE *p = base; p < top; p++) {
            VALUE v = *p;
            if (!RB_SPECIAL_CONST_P(v)) rb_gc_mark(v);
        }
    }
}

void abruby_fiber_free(struct abruby_fiber *f) {
    fiber_free_cstack(f);
}
