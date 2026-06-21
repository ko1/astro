/* koruby_precise — fiber.c: stackful coroutines (Fiber), #included into
 * korb_runtime.c's TU.  Each fiber gets its own value-stack (fixed mmap) and
 * native C-stack (malloc) and switches via ucontext.  The moving GC object is a
 * thin handle; all mutable state + roots live in a libc-stable KorbFiberRep,
 * scanned via the vm fiber list (see AROH_VISIT_ROOTS).  <ucontext.h> is
 * included at the top of korb_runtime.c (before koruby macros). */

#define KORB_FIBER_VSLOTS_BYTES ((size_t)2 << 20)   /* per-fiber value stack */
#define KORB_FIBER_CSTACK_BYTES ((size_t)512 << 10) /* per-fiber native stack */
#define KORB_FIBER_VSLOTS_MARGIN 1024               /* slots reserved below limit */
#define KORB_FIBER_CSTACK_MARGIN ((size_t)64 << 10) /* native-stack floor margin */

/* runs on the fiber's native stack; CTX passed split across two int args. */
static void
korb_fiber_trampoline(unsigned hi, unsigned lo)
{
    CTX *const c = (CTX *)(((uintptr_t)hi << 32) | (uintptr_t)lo);
    KorbFiberRep *const rep = c->vm->starting_fiber;
    rep->fstate = 1;                                  /* running */
    VALUE arg = rep->transfer;                        /* value from the first resume */
    RESULT r = korb_block_yield(c, c->slots, rep->body, rep->def_env, &arg, 1, &rep->captured_self);
    rep->fstate = 3;                                  /* done */
    rep->raised = (r.state == KORB_RAISE) ? 1u : 0u;
    rep->transfer = (r.state == KORB_NORMAL || r.state == KORB_RAISE) ? r.value : KORB_NIL;
    swapcontext((ucontext_t *)rep->uctx, (ucontext_t *)rep->resume_uctx);  /* never returns */
}

static RESULT
korb_fiber_new(CTX *c, VALUE *slots, NODE *block, VALUE *def_env, VALUE *captured_self)
{
    if (UNLIKELY(block == NULL))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to create a Fiber without a block");
    KorbFiber *fb = korb_alloc(c, slots, sizeof(KorbFiber), KORB_OBJ_FIBER);   /* no GC below */
    KorbFiberRep *rep = calloc(1, sizeof(KorbFiberRep));
    if (!rep) { fprintf(stderr, "koruby_precise: oom (fiber rep)\n"); abort(); }
    fb->rep = rep;
    rep->body = block;
    rep->def_env = def_env;
    rep->captured_self = KORB_CSELF_VAL(captured_self);
    rep->transfer = KORB_NIL;
    rep->fstate = 0;
    void *vs = mmap(NULL, KORB_FIBER_VSLOTS_BYTES, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (vs == MAP_FAILED) { perror("koruby_precise: mmap fiber vslots"); abort(); }
    rep->vslots = (VALUE *)vs + 2;                     /* leading slack: bottom-header EP at base[-2] (fiber toplevel → vslots[-2]) */
    rep->vslots[-1] = 0;                               /* fiber toplevel self cell (base[-1]; step 2) */
    rep->vslots[-2] = 0;                               /* fiber toplevel EP (base[-2]) */
    rep->vslots_top = rep->vslots;
    rep->vslots_limit = (VALUE *)vs + KORB_FIBER_VSLOTS_BYTES / sizeof(VALUE) - KORB_FIBER_VSLOTS_MARGIN;
    rep->vslots_hw = rep->vslots;
    rep->cstack = malloc(KORB_FIBER_CSTACK_BYTES);
    if (!rep->cstack) abort();
    rep->uctx = calloc(1, sizeof(ucontext_t));
    if (!rep->uctx) abort();
    rep->link = c->vm->fiber_list;                    /* register for GC scanning */
    c->vm->fiber_list = rep;
    return RESULT_OK((VALUE)fb);
}

/* Fiber#resume([v]) — switch into the fiber; returns the value it yields (or its
 * block's result when it finishes). */
static RESULT
korb_m_fiber_resume(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    KorbFiberRep *const rep = VAL2FIBER(VALUE_REF_GET(self))->rep;
    if (UNLIKELY(rep->fstate == 3)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "dead fiber called");
    if (UNLIKELY(rep->fstate == 1)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "double resume");

    rep->transfer = (VALUE_SLICE_LEN(a) >= 1) ? VALUE_SLICE_GET(a, 0) : KORB_NIL;

    KorbFiberRep *const prev = c->vm->running_fiber;
    VALUE *const s_slots = c->slots; VALUE *const s_top = c->slots_top;
    VALUE *const s_limit = c->slots_limit; VALUE *const s_hw = c->slots_high_water;
    const char *const s_cstack = c->cstack_limit;
    if (prev == NULL) { c->vm->main_slots = s_slots; c->vm->main_slots_top = s_top; }
    else { prev->vslots_top = s_top; prev->vslots_hw = s_hw; }

    ucontext_t here;
    rep->resume_uctx = &here;
    if (rep->fstate == 0) {                            /* first resume → build context */
        getcontext((ucontext_t *)rep->uctx);
        ((ucontext_t *)rep->uctx)->uc_stack.ss_sp = rep->cstack;
        ((ucontext_t *)rep->uctx)->uc_stack.ss_size = KORB_FIBER_CSTACK_BYTES;
        ((ucontext_t *)rep->uctx)->uc_link = NULL;
        c->vm->starting_fiber = rep;
        makecontext((ucontext_t *)rep->uctx, (void (*)(void))korb_fiber_trampoline, 2,
                    (unsigned)((uintptr_t)c >> 32), (unsigned)((uintptr_t)c & 0xFFFFFFFFu));
    }
    /* activate the fiber's stacks */
    c->slots = rep->vslots; c->slots_top = rep->vslots_top;
    c->slots_limit = rep->vslots_limit; c->slots_high_water = rep->vslots_hw;
    c->cstack_limit = (const char *)rep->cstack + KORB_FIBER_CSTACK_MARGIN;
    c->vm->running_fiber = rep;

    swapcontext(&here, (ucontext_t *)rep->uctx);       /* === into the fiber === */

    /* === back: fiber yielded or finished === */
    c->vm->running_fiber = prev;
    c->slots = s_slots; c->slots_top = s_top; c->slots_limit = s_limit;
    c->slots_high_water = s_hw; c->cstack_limit = s_cstack;
    if (prev == NULL) c->vm->main_slots = NULL;

    if (rep->raised) { rep->raised = 0; return RESULT_RAISE_(rep->transfer); }
    return RESULT_OK(rep->transfer);
}

static RESULT
korb_m_fiber_alive(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    return RESULT_OK(VAL2FIBER(VALUE_REF_GET(self))->rep->fstate != 3 ? KORB_TRUE : KORB_FALSE);
}

/* Fiber.yield([v]) — suspend the running fiber, hand `v` to its resumer. */
static RESULT
korb_m_fiber_yield(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self;
    KorbFiberRep *const rep = c->vm->running_fiber;
    if (UNLIKELY(rep == NULL))
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "can't yield from root fiber");
    rep->transfer = (VALUE_SLICE_LEN(a) >= 1) ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    rep->fstate = 2;                                   /* suspended */
    rep->vslots_top = c->slots_top; rep->vslots_hw = c->slots_high_water;
    swapcontext((ucontext_t *)rep->uctx, (ucontext_t *)rep->resume_uctx);  /* === out === */
    /* === resumed: resume() restored c->slots to ours === */
    rep->fstate = 1;
    return RESULT_OK(rep->transfer);                   /* value from the next resume */
}
