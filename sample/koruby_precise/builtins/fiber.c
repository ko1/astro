/* koruby_precise — fiber.c: stackful coroutines (Fiber), #included into
 * korb_runtime.c's TU.  Each fiber gets its own value-stack (fixed mmap) and
 * native C-stack (malloc) and switches via ucontext.  The moving GC object is a
 * thin handle; all mutable state + roots live in a libc-stable KorbFiberRep,
 * scanned via the vm fiber list (see AROH_VISIT_ROOTS).  <ucontext.h> is
 * included at the top of korb_runtime.c (before koruby macros). */

/* swapcontext brings back the signal mask saved when this context switched
 * out; a trap or a parked signal since then (gen moved) must survive it. */
static inline void korb_sigmask_resume(const struct korb_vm *const vm, const uint32_t gen_out) {
#ifndef KORB_WASI
    if (vm->sigmask_gen != gen_out) (void)pthread_sigmask(SIG_SETMASK, &vm->sigmask, NULL);
#else
    (void)vm; (void)gen_out;
#endif
}

#define KORB_FIBER_VSLOTS_BYTES ((size_t)2 << 20)   /* per-fiber value stack */
#define KORB_FIBER_CSTACK_BYTES ((size_t)1 << 20)   /* per-fiber native stack (1 MiB) */
#define KORB_FIBER_VSLOTS_MARGIN 1024               /* slots reserved below limit */
#define KORB_FIBER_CSTACK_MARGIN ((size_t)64 << 10) /* native-stack floor margin */

/* ---- FiberError (const-only class; etype stays RUNTIME, exc_class drives
 * rescue/#class — same pattern as ThreadError) ------------------------------ */
static RESULT
korb_raise_fiber_error(CTX *c, korb_value *slots, const char *msg)
{
    const korb_value cls = korb_const_get(c->vm, korb_intern_lit(c->vm, "FiberError", 10));
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "%s", msg);
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value))
        ARO_STORE(c, VAL2EXC(r.value), (korb_value *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    return r;
}

/* runs on the fiber's native stack; CTX passed split across two int args. */
static void
korb_fiber_trampoline(unsigned hi, unsigned lo)
{
    CTX *const c = (CTX *)(uintptr_t)(((uint64_t)hi << 32) | (uint64_t)lo);
    KorbFiberRep *const rep = c->vm->starting_fiber;
    korb_sigmask_resume(c->vm, c->vm->sigmask_gen - 1);   /* the context was built with an older mask */
    rep->fstate = 1;                                  /* running */
    korb_value *const base = c->slots;                     /* proc.call(first resume's values) */
    base[0] = rep->proc;
    uint32_t n = 1;
    if (rep->start_splat == 2) n = 0;                 /* resume() with no values: no block arguments */
    else if (rep->start_splat && KORB_ARRAY_P(rep->transfer)) {
        const KorbArray *const aa = VAL2ARY(rep->transfer);
        n = aa->len;
        for (uint32_t i = 0; i < n; i++) base[1 + i] = korb_ary_ptr(aa)[i];   /* no alloc in this loop */
    } else {
        base[1] = rep->transfer;
    }
    RESULT r = korb_send(c, base + 1 + n, korb_intern_lit(c->vm, "call", 4), 0, n);
    rep->fstate = 3;                                  /* done */
    if (r.state == KORB_RETURN || r.state == KORB_BREAK)   /* `return` / `break` の逃げ場がない */
        r = korb_raise(c, c->slots, KORB_E_LOCALJUMP, 0, "%s",
                       r.state == KORB_RETURN ? "unexpected return" : "break from proc-closure");
    if (r.state == KORB_RAISE && r.value == KORB_UNDEF) r = RESULT_OK(KORB_NIL);   /* #kill sentinel */
    rep->raised = (r.state == KORB_RAISE) ? 1u : 0u;
    rep->transfer = (r.state == KORB_NORMAL || r.state == KORB_RAISE) ? r.value : KORB_NIL;
    if (rep->resumer_mask_gen != c->vm->sigmask_gen) korb_signal_park_pending(c->vm);
    swapcontext((ucontext_t *)rep->uctx, (ucontext_t *)rep->resume_uctx);  /* never returns */
}

static struct korb_thread *korb_thread_boot(CTX *c);   /* fwd (thread.c, included later) */

static RESULT
korb_fiber_new(CTX *c, korb_value *slots, NODE *block, korb_value *def_env, korb_value *captured_self)
{
    if (UNLIKELY(block == NULL))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to create a Fiber without a block");
    /* the block as a Proc: that closes the creating frame's env, which the fiber
     * may outlive (graphql's Dataloader resumes fibers after spawn_fiber returned) */
    if (def_env == KORB_BLK_FWD || block == KORB_BLK_CPROC) slots[1] = KORB_CSELF_VAL(captured_self);   /* &blk: already a Proc */
    else slots[1] = UNWRAP(korb_make_proc(c, slots + 1, block, def_env, KORB_CSELF_VAL(captured_self), 0));
    KorbFiber *fb = korb_alloc(c, slots + 2, sizeof(KorbFiber), KORB_OBJ_FIBER);   /* no GC below */
    KorbFiberRep *rep = calloc(1, sizeof(KorbFiberRep));
    if (!rep) { fprintf(stderr, "koruby_precise: oom (fiber rep)\n"); abort(); }
    fb->rep = rep;
    rep->body = block;
    rep->def_env = NULL;
    rep->proc = slots[1];
    rep->captured_self = KORB_CSELF_VAL(captured_self);
    rep->transfer = KORB_NIL; rep->tilde = KORB_NIL;
    rep->transferred = 0; rep->killing = 0;
    rep->storage = KORB_NIL;                           /* set below: a copy of the creator's */
    rep->fibobj = (korb_value)fb;                           /* Fiber.current (root; no GC between alloc and here) */
    rep->fstate = 0;
    rep->owner = korb_thread_boot(c);                  /* only this thread may resume/transfer it */
    void *vs = mmap(NULL, KORB_FIBER_VSLOTS_BYTES, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (vs == MAP_FAILED) { perror("koruby_precise: mmap fiber vslots"); abort(); }
    rep->vslots = (korb_value *)vs + 4;                     /* leading slack: the four bottom-header cells */
    rep->vslots[-1] = 0;                               /* fiber toplevel self cell (base[-1]; step 2) */
    rep->vslots[-2] = 0;                               /* fiber toplevel EP (base[-2]) */
    rep->vslots[-3] = 0;                               /* fiber toplevel link (base[-3]) */
    rep->vslots[-4] = 0;                               /* fiber toplevel identity (base[-4]) */
    rep->vslots_top = rep->vslots;
    rep->vslots_limit = (korb_value *)vs + KORB_FIBER_VSLOTS_BYTES / sizeof(korb_value) - KORB_FIBER_VSLOTS_MARGIN;
    rep->vslots_hw = rep->vslots;
    rep->cstack = malloc(KORB_FIBER_CSTACK_BYTES);
    if (!rep->cstack) abort();
    rep->uctx = calloc(1, sizeof(ucontext_t));
    if (!rep->uctx) abort();
    rep->link = c->vm->fiber_list;                    /* register for GC scanning */
    c->vm->fiber_list = rep;
    korb_scope_stacks_init(c, &rep->ss);
    /* a new fiber starts with a COPY of its creator's storage (CRuby), so writes
     * never leak back; Fiber.new(storage:) overwrites it at the call site */
    {
        const KorbFiberRep *const cur = c->vm->running_fiber ? c->vm->running_fiber : c->vm->root_fiber;
        if (cur != NULL && cur->storage != KORB_NIL && VAL2HASH(cur->storage)->len > 0) {
            slots[0] = (korb_value)fb;                      /* park the fiber across the copy's allocs */
            slots[2] = cur->storage;                   /* receiver for #dup (cursor slots+3) */
            const RESULT dr = korb_send(c, slots + 3, korb_intern_lit(c->vm, "dup", 3), 0, 0);   /* slots[2] = recv */
            if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
            VAL2FIBER(slots[0])->rep->storage = dr.value;
            return RESULT_OK(slots[0]);
        }
    }
    return RESULT_OK((korb_value)fb);
}

static korb_value korb_fiber_kill_sentinel(CTX *c);   /* fwd (Fiber#kill unwind payload) */

/* The green thread a Fiber belongs to: CRuby's "fiber called across threads". */
static bool korb_fiber_foreign(CTX *c, const KorbFiberRep *rep) {
    return rep->owner != NULL && c->vm->cur_thread != NULL && rep->owner != c->vm->cur_thread;
}

/* Switch into `rep` (fstate 0 or 2) carrying `xfer` — the shared engine behind
 * Fiber#resume and Fiber#raise.  With deliver_raise, the fiber's Fiber.yield
 * raises xfer instead of returning it.  Returns the value the fiber next
 * yields (or its block's result when it finishes; RAISE if it died raising). */
static RESULT
korb_fiber_switch_in(CTX *c, korb_value *slots, KorbFiberRep *const rep, korb_value xfer, int deliver_raise)
{
    rep->transfer = xfer;
    rep->pending_raise = deliver_raise ? 1u : 0u;

    KorbFiberRep *const prev = c->vm->running_fiber;
    korb_value *const s_slots = c->slots; korb_value *const s_top = c->slots_top;
    korb_value *const s_limit = c->slots_limit; korb_value *const s_hw = c->slots_high_water;
    const char *const s_cstack = c->cstack_limit;
    const korb_value s_cfunc_link = c->cfunc_link;          /* names a frame in the resumer's stack */
    const korb_value *const s_carry_base = c->carry_base;   /* same, for a staged (not yet consumed) link */
    const uint32_t s_carry_line = c->carry_line_bits;
    /* While this resumer is suspended, the GC scans its stack up to the recorded
     * top.  Use `slots` (the resume frame's true cursor), not the possibly-lagging
     * c->slots_top (== s_top) — same fix as Fiber.yield.  s_top is still used to
     * RESTORE c->slots_top after the fiber returns. */
    if (prev == NULL) { c->vm->main_slots = s_slots; c->vm->main_slots_top = slots; }
    else { prev->vslots_top = slots; prev->vslots_hw = s_hw; }

    ucontext_t here;
    rep->resume_uctx = &here;
    if (rep->fstate == 0) {                            /* first resume → build context */
        getcontext((ucontext_t *)rep->uctx);
        ((ucontext_t *)rep->uctx)->uc_stack.ss_sp = rep->cstack;
        ((ucontext_t *)rep->uctx)->uc_stack.ss_size = KORB_FIBER_CSTACK_BYTES;
        ((ucontext_t *)rep->uctx)->uc_link = NULL;
        c->vm->starting_fiber = rep;
        makecontext((ucontext_t *)rep->uctx, (void (*)(void))korb_fiber_trampoline, 2,
                    (unsigned)((uint64_t)(uintptr_t)c >> 32), (unsigned)((uintptr_t)c & 0xFFFFFFFFu));
    }
    /* activate the fiber's stacks */
    c->slots = rep->vslots; c->slots_top = rep->vslots_top;
    c->slots_limit = rep->vslots_limit; c->slots_high_water = rep->vslots_hw;
    c->cstack_limit = (const char *)rep->cstack + KORB_FIBER_CSTACK_MARGIN;
    c->cfunc_link = rep->vcfunc_link;                  /* 0 on the first resume: no frames yet */
    c->carry_base = rep->vcarry_base; c->carry_line_bits = rep->vcarry_line_bits;
    korb_re_sync_floor(c);   /* astrogre \g<> guard must use the fiber's stack */
    c->vm->running_fiber = rep;

    const uint32_t mask_gen = c->vm->sigmask_gen;
    rep->resumer_mask_gen = mask_gen;
    if (rep->mask_gen != mask_gen) korb_signal_park_pending(c->vm);   /* its saved mask predates a trap */
    korb_scope_stacks_swap(c, &rep->ss);               /* the fiber's own $!, save stacks and $~ while it runs */
    { korb_value *const tc = korb_re_tilde_cell(c); const korb_value t = *tc; *tc = rep->tilde; rep->tilde = t; }
    /* the generator sinks live on each fiber's C stack: an `each` driven inside
     * the fiber (external enumeration) must not chain onto the resumer's */
    { struct korb_gen_sink *const g = c->vm->gen_sink; c->vm->gen_sink = rep->gen_sink; rep->gen_sink = g; }
    swapcontext(&here, (ucontext_t *)rep->uctx);       /* === into the fiber === */
    korb_sigmask_resume(c->vm, mask_gen);
    korb_scope_stacks_swap(c, &rep->ss);
    { korb_value *const tc = korb_re_tilde_cell(c); const korb_value t = *tc; *tc = rep->tilde; rep->tilde = t; }
    { struct korb_gen_sink *const g = c->vm->gen_sink; c->vm->gen_sink = rep->gen_sink; rep->gen_sink = g; }

    /* === back: fiber yielded or finished === */
    c->vm->running_fiber = prev;
    c->slots = s_slots; c->slots_top = s_top; c->slots_limit = s_limit;
    c->slots_high_water = s_hw; c->cstack_limit = s_cstack;
    c->cfunc_link = s_cfunc_link;
    c->carry_base = s_carry_base; c->carry_line_bits = s_carry_line;
    korb_re_sync_floor(c);   /* restore the outer stack's floor */
    if (prev == NULL) c->vm->main_slots = NULL;

    /* While we were away, someone may have killed US (a child fiber calling
     * parent.kill).  We are the one at a switch point, so unwind here. */
    if (prev != NULL && prev->killing) { prev->killing = 0; return RESULT_RAISE_(korb_fiber_kill_sentinel(c)); }
    if (rep->raised) { rep->raised = 0; return RESULT_RAISE_(rep->transfer); }
    return RESULT_OK(rep->transfer);
}

/* The value several arguments travel as between resume and Fiber.yield:
 * none → nil, one → itself, more → an Array (CRuby). */
static RESULT korb_fiber_pack(CTX *c, korb_value *slots, korb_slice a) {
    const uint32_t n = KORB_SLICE_LEN(a);
    if (n == 0) return RESULT_OK(KORB_NIL);
    if (n == 1) return RESULT_OK(KORB_SLICE_GET(a, 0));
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, KORB_REF_AT(&slots[0]), KORB_SLICE_GET(a, i)));
    return RESULT_OK(slots[0]);
}

/* Fiber#resume([v]) — switch into the fiber; returns the value it yields (or its
 * block's result when it finishes). */
static RESULT
korb_m_fiber_resume(CTX *c, korb_value *slots, korb_ref self, korb_slice a)
{
    KorbFiberRep *const rep = VAL2FIBER(KORB_REF_GET(self))->rep;
    if (UNLIKELY(korb_fiber_foreign(c, rep)))
        return korb_raise_fiber_error(c, slots, "fiber called across threads");
    if (UNLIKELY(rep->fstate == 3)) return korb_raise_fiber_error(c, slots, "attempt to resume a terminated fiber");
    if (UNLIKELY(rep->fstate == 1))   /* 自分自身か、resume 連鎖の祖先か */
        return korb_raise_fiber_error(c, slots,
            (rep == c->vm->running_fiber || (c->vm->running_fiber == NULL && rep == c->vm->root_fiber))
            ? "attempt to resume the current fiber" : "attempt to resume a resuming fiber");
    if (UNLIKELY(rep->transferred))   /* CRuby: a transferred fiber is not resumable */
        return korb_raise_fiber_error(c, slots, "attempt to yield on a not resumed fiber");
    if (rep->fstate == 0) rep->start_splat = KORB_SLICE_LEN(a) == 0 ? 2 : KORB_SLICE_LEN(a) > 1;   /* the block takes them as its arguments */
    const korb_value xfer = UNWRAP(korb_fiber_pack(c, slots + 1, a));
    return korb_fiber_switch_in(c, slots + 1, VAL2FIBER(KORB_REF_GET(self))->rep, xfer, 0);
}

/* The value a killed fiber unwinds with.  A plain (non-Exception) payload so no
 * `rescue => e` can intercept it; only `ensure` runs, as CRuby's kill does. */
static korb_value korb_fiber_kill_sentinel(CTX *c) {
    (void)c;
    return KORB_UNDEF;
}

/* Fiber#transfer([v]) — hand control over without becoming the target's resumer.
 * koruby keeps the C-stack discipline of resume (control comes back here when
 * the target yields or finishes); what transfer adds is the bookkeeping that
 * makes #resume on a transferred fiber (and #transfer to a yielding one) the
 * FiberErrors CRuby raises. */
static RESULT
korb_m_fiber_transfer(CTX *c, korb_value *slots, korb_ref self, korb_slice a)
{
    KorbFiberRep *const rep = VAL2FIBER(KORB_REF_GET(self))->rep;
    if (UNLIKELY(korb_fiber_foreign(c, rep)))
        return korb_raise_fiber_error(c, slots, "fiber called across threads");
    if (UNLIKELY(rep->fstate == 3)) return korb_raise_fiber_error(c, slots, "attempt to resume a terminated fiber");
    if (UNLIKELY(rep == c->vm->running_fiber)) return korb_raise_fiber_error(c, slots, "attempt to transfer to self");
    if (UNLIKELY(rep->fstate == 2 && !rep->transferred))
        return korb_raise_fiber_error(c, slots, "attempt to transfer to a yielding fiber");
    rep->transferred = 1;
    if (rep->fstate == 0) rep->start_splat = KORB_SLICE_LEN(a) == 0 ? 2 : KORB_SLICE_LEN(a) > 1;
    const korb_value xfer = UNWRAP(korb_fiber_pack(c, slots + 1, a));
    return korb_fiber_switch_in(c, slots + 1, VAL2FIBER(KORB_REF_GET(self))->rep, xfer, 0);
}

/* Fiber#kill — terminate the fiber at its suspension point (ensure blocks run).
 * An unborn or dead fiber is simply marked dead. */
static RESULT
korb_m_fiber_kill(CTX *c, korb_value *slots, korb_ref self, korb_slice a)
{
    (void)a;
    KorbFiberRep *const rep = VAL2FIBER(KORB_REF_GET(self))->rep;
    if (rep->fstate == 0 || rep->fstate == 3) { rep->fstate = 3; return RESULT_OK(KORB_REF_GET(self)); }
    if (UNLIKELY(rep == c->vm->running_fiber))
        return RESULT_RAISE_(korb_fiber_kill_sentinel(c));   /* killing myself: unwind here */
    if (rep->fstate == 1) {
        /* Running, but not us: it is an ancestor in the resume chain (a child
         * killing its parent).  Flag it — it unwinds when control returns to it,
         * which is the point where it is safe to touch its stack. */
        rep->killing = 1;
        return RESULT_OK(KORB_REF_GET(self));
    }
    rep->killing = 1;
    const RESULT r = korb_fiber_switch_in(c, slots, rep, KORB_NIL, 0);
    rep->killing = 0;
    if (r.state == KORB_RAISE && r.value == KORB_UNDEF) { /* the sentinel came back out: swallow it */
        rep->fstate = 3;
        return RESULT_OK(KORB_REF_GET(self));
    }
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(KORB_REF_GET(self));
}

/* Fiber#raise(...) — build the exception in the calling context (CRuby 4.0
 * semantics: the automatic cause comes from the caller, not the fiber), then
 * deliver it at the fiber's suspension point.  Kernel#raise-style args. */
static RESULT
korb_m_fiber_raise(CTX *c, korb_value *slots, korb_ref self, korb_slice a)
{
    KorbFiberRep *const rep = VAL2FIBER(KORB_REF_GET(self))->rep;
    if (UNLIKELY(korb_fiber_foreign(c, rep)))
        return korb_raise_fiber_error(c, slots, "fiber called across threads");
    if (UNLIKELY(rep->fstate == 0))
        return korb_raise_fiber_error(c, slots, "cannot raise exception on unborn fiber");
    if (UNLIKELY(rep->fstate == 3))
        return korb_raise_fiber_error(c, slots, "attempt to resume a terminated fiber");
    const RESULT r = korb_exc_build_with_cause(c, slots, a);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;            /* argument error → caller */
    /* fstate 1 is the current fiber or, since koruby keeps resume on the C stack,
     * one of its resume-chain ancestors.  Raising into an ancestor is what
     * Kernel#raise here already does: the exception unwinds out to it. */
    if (rep->fstate == 1 || (c->vm->running_fiber == NULL && rep == c->vm->root_fiber))
        return RESULT_RAISE_(r.value);
    return korb_fiber_switch_in(c, slots, rep, r.value, 1);
}

/* A stand-in rep for a thread's root stack (no body, permanently running).
 * Registered in fiber_list so its fibobj/storage edges are GC roots. */
static KorbFiberRep *
korb_fiber_root_rep_alloc(CTX *c, korb_value *slots)
{
    KorbFiber *fb = korb_alloc(c, slots, sizeof(KorbFiber), KORB_OBJ_FIBER);   /* no GC below */
    KorbFiberRep *rp = calloc(1, sizeof(KorbFiberRep));
    if (!rp) { fprintf(stderr, "koruby_precise: oom (root fiber rep)\n"); abort(); }
    fb->rep = rp;
    rp->fibobj = (korb_value)fb;
    rp->transfer = KORB_NIL; rp->tilde = KORB_NIL;
    rp->storage = KORB_NIL;
    rp->tls = KORB_NIL;
    rp->captured_self = KORB_NIL;
    rp->fstate = 1;                                    /* running: never resumable */
    rp->link = c->vm->fiber_list;                      /* rooted via the fibobj edge */
    c->vm->fiber_list = rp;
    return rp;
}

/* Fiber.current — the running fiber, or the current thread's root stand-in
 * (vm->root_fiber follows the running thread: thread ctx_save / ctx_load). */
static RESULT
korb_m_fiber_s_current(CTX *c, korb_value *slots, korb_ref self, korb_slice a)
{
    (void)a;
    const KorbFiberRep *const rep = c->vm->running_fiber;
    if (rep) return RESULT_OK(rep->fibobj);
    /* The main stack has no rep of its own; hand out a stand-in Fiber (built
     * once, kept alive through fiber_list's fibobj edge) so callers still get a
     * Fiber.  It is permanently "running", so #resume reports a double resume
     * exactly as resuming the current fiber does. */
    if (c->vm->root_fiber != NULL) return RESULT_OK(c->vm->root_fiber->fibobj);
    c->vm->root_fiber = korb_fiber_root_rep_alloc(c, slots);
    (void)self;
    return RESULT_OK(c->vm->root_fiber->fibobj);
}

static KorbFiberRep *korb_fiber_cur_rep(CTX *c, korb_value *slots);   /* fwd */
/* Fiber.blocking { |fiber| … } — run the block with the current fiber marked
 * blocking (Fiber.blocking? → 1, current_scheduler → nil), then restore. */
static RESULT
korb_m_fiber_s_blocking_blk(CTX *c, korb_value *slots, korb_ref self, korb_slice a,
                            NODE *block, korb_value *def_env, korb_value *cself)
{
    (void)self; (void)a;
    if (UNLIKELY(block == NULL))
        return korb_raise(c, slots, KORB_E_LOCALJUMP, 0, "no block given (yield)");
    KorbFiberRep *const rep = korb_fiber_cur_rep(c, slots);   /* libc-stable */
    slots[0] = rep->fibobj;
    const uint8_t was = rep->blocking;
    rep->blocking = 1;
    RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
    rep->blocking = was;
    return r;
}

/* ---- Fiber storage (fiber-local variables) --------------------------------
 * Each fiber owns a Hash, created on first write.  A new fiber inherits a COPY
 * of its creator's storage the first time it is touched (CRuby semantics), so
 * writes never leak back to the parent. */
static KorbFiberRep *korb_fiber_cur_rep(CTX *c, korb_value *slots) {
    KorbFiberRep *rep = c->vm->running_fiber;
    if (rep == NULL) {                                  /* main stack: materialize the root fiber */
        (void)korb_m_fiber_s_current(c, slots, KORB_REF_AT(&slots[0]), KORB_SLICE_MAKE(NULL, 0));
        rep = c->vm->root_fiber;
    }
    return rep;
}
/* The storage Hash of `rep`, or nil when it has none and none is requested. */
static RESULT korb_fiber_storage_of(CTX *c, korb_value *slots, KorbFiberRep *rep, bool create) {
    if (rep == NULL) return RESULT_OK(KORB_NIL);
    if (rep->storage == KORB_NIL && create) {
        const RESULT hr = korb_hash_new(c, slots, 4);
        if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
        rep->storage = hr.value;                        /* rep is libc-stable; the field is a GC root */
    }
    return RESULT_OK(rep->storage);
}
/* A storage Hash must be a non-frozen Hash with Symbol keys (CRuby). */
RESULT korb_fiber_check_storage(CTX *c, korb_value *slots, korb_value h) {
    if (h == KORB_NIL) return RESULT_OK(KORB_NIL);
    if (UNLIKELY(!KORB_HASH_P(h)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "storage must be a hash");
    if (UNLIKELY(((const AroObjectHeader *)(uintptr_t)h)->flags & KORB_FL_FROZEN))
        return korb_raise_frozen(c, slots, h);
    const KorbHash *const hh = VAL2HASH(h);
    for (uint32_t i = 0; i < hh->len; i++)
        if (UNLIKELY(!SYMBOL_P(korb_items_data(hh->items)[2 * i])))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "storage keys must be symbols");
    return RESULT_OK(KORB_NIL);
}
/* Fiber#storage / #storage= */
static RESULT korb_m_fiber_storage(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    KorbFiberRep *const rep = VAL2FIBER(KORB_REF_GET(self))->rep;
    if (UNLIKELY(rep != korb_fiber_cur_rep(c, slots)))      /* only its own fiber may read it (CRuby) */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0,
                          "Fiber storage can only be accessed from the Fiber it belongs to");
    return korb_fiber_storage_of(c, slots, rep, false);
}
static RESULT korb_m_fiber_storage_set(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const korb_value h = KORB_SLICE_GET(a, 0);
    CHECK(korb_fiber_check_storage(c, slots, h));
    VAL2FIBER(KORB_REF_GET(self))->rep->storage = h;
    return RESULT_OK(h);
}
/* Fiber[key] / Fiber[key] = value — the RUNNING fiber's storage. */
static RESULT korb_fiber_storage_key(CTX *c, korb_value *slots, korb_value k, korb_value *out) {
    if (SYMBOL_P(k)) { *out = k; return RESULT_OK(KORB_TRUE); }
    if (!KORB_STRING_P(k) && korb_responds_to(c, k, korb_intern_lit(c->vm, "to_str", 6))) {
        slots[0] = k;                                   /* #to_str-able key (CRuby coerces) */
        const RESULT sr = korb_send(c, slots + 1, korb_intern_lit(c->vm, "to_str", 6), 0, 0);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        if (KORB_STRING_P(sr.value)) k = sr.value;
    }
    if (KORB_STRING_P(k)) {
        *out = ID2SYM(korb_intern_enc(c->vm, korb_str_ptr(VAL2STR(k)), VAL2STR(k)->len, KORB_STR_ENC(k)));
        return RESULT_OK(KORB_TRUE);
    }
    char ib[128]; char *b = NULL; size_t bl = 0;
    FILE *ms = open_memstream(&b, &bl);
    if (ms) { korb_fprint_inspect(c, ms, k); fclose(ms); }
    snprintf(ib, sizeof ib, "%s", b ? b : "?"); free(b);
    return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", ib);
}
static RESULT korb_m_fiber_s_aref(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)self;
    korb_value key;
    CHECK(korb_fiber_storage_key(c, slots, KORB_SLICE_GET(a, 0), &key));
    slots[0] = key;
    KorbFiberRep *const rep = korb_fiber_cur_rep(c, slots + 1);
    if (rep == NULL || rep->storage == KORB_NIL) return RESULT_OK(KORB_NIL);
    const int32_t i = korb_hash_find(VAL2HASH(rep->storage), slots[0]);
    return RESULT_OK(i >= 0 ? korb_items_data(VAL2HASH(rep->storage)->items)[2 * i + 1] : KORB_NIL);
}
static RESULT korb_m_fiber_s_aset(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)self;
    korb_value key;
    CHECK(korb_fiber_storage_key(c, slots, KORB_SLICE_GET(a, 0), &key));
    slots[0] = key; slots[1] = KORB_SLICE_GET(a, 1);
    KorbFiberRep *const rep = korb_fiber_cur_rep(c, slots + 2);
    const RESULT sr = korb_fiber_storage_of(c, slots + 2, rep, true);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[2] = sr.value;
    if (slots[1] == KORB_NIL) {                         /* assigning nil deletes the key (CRuby) */
        KorbHash *const h = VAL2HASH(slots[2]);
        const int32_t di = korb_hash_find(h, slots[0]);
        if (di >= 0) {                                  /* shift the pairs down (no alloc → no GC) */
            KorbArrayItems *const it = h->items;
            for (uint32_t i = (uint32_t)di; i + 1 < h->len; i++) {
                ARO_STORE(c, it, &korb_items_data(it)[2 * i],     korb_items_data(it)[2 * (i + 1)]);
                ARO_STORE(c, it, &korb_items_data(it)[2 * i + 1], korb_items_data(it)[2 * (i + 1) + 1]);
            }
            h->len--;
            ARO_STORE(c, it, &korb_items_data(it)[2 * h->len], KORB_NIL);
            ARO_STORE(c, it, &korb_items_data(it)[2 * h->len + 1], KORB_NIL);
            KORB_HASH_DROP_INDEX(h);
        }
        rep->storage = slots[2];
        return RESULT_OK(KORB_NIL);
    }
    CHECK(korb_hash_set(c, slots + 3, KORB_REF_AT(&slots[2]), KORB_REF_AT(&slots[0]), slots[1]));
    rep->storage = slots[2];                            /* re-read: the set may have moved it */
    return RESULT_OK(slots[1]);
}
/* Fiber.blocking? — 1 in a blocking fiber (the root, or one created with
 * blocking: true), false otherwise.  koruby has no scheduler, so the flag is
 * purely what the creator asked for. */
static RESULT korb_m_fiber_s_blocking_p(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots; (void)self; (void)a;
    const KorbFiberRep *const rep = c->vm->running_fiber;
    return RESULT_OK((rep == NULL || rep->blocking) ? LONG2FIX(1) : KORB_FALSE);
}
static RESULT korb_m_fiber_blocking_p(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c; (void)slots; (void)a;
    const KorbFiberRep *const rep = VAL2FIBER(KORB_REF_GET(self))->rep;
    /* the root fiber has no body: it is always blocking */
    return RESULT_OK((rep->body == NULL || rep->blocking) ? KORB_TRUE : KORB_FALSE);
}

static RESULT
korb_m_fiber_alive(CTX *c, korb_value *slots, korb_ref self, korb_slice a)
{
    (void)c; (void)slots; (void)a;
    return RESULT_OK(VAL2FIBER(KORB_REF_GET(self))->rep->fstate != 3 ? KORB_TRUE : KORB_FALSE);
}

/* Fiber#inspect / #to_s — "#<Fiber:0xADDR file:line (status)>" (CRuby's shape).
 * The location is the block's, which is what CRuby shows. */
static RESULT
korb_m_fiber_inspect(CTX *c, korb_value *slots, korb_ref self, korb_slice a)
{
    (void)a;
    const korb_value fv = KORB_REF_GET(self);
    const KorbFiberRep *const rep = VAL2FIBER(fv)->rep;
    static const char *const st[4] = { "created", "resumed", "suspended", "terminated" };
    char loc[256] = "";
    uint32_t fsym = 0, line = 0;
    if (rep->body != NULL && korb_get_srcloc(c->vm, rep->body, &fsym, &line))   /* a root fiber has none: no location */
        snprintf(loc, sizeof loc, " %s:%u", korb_sym_name(c->vm, fsym), line);
    char buf[320];
    const int n = snprintf(buf, sizeof buf, "#<Fiber:0x%016zx%s (%s)>",
                           (size_t)(uintptr_t)rep, loc, st[rep->fstate & 3]);   /* the rep does not move (the object may) */
    return korb_str_new(c, slots, buf, (uint32_t)(n < 0 ? 0 : (n < (int)sizeof buf ? n : (int)sizeof buf - 1)));
}

/* Fiber.yield([v]) — suspend the running fiber, hand `v` to its resumer. */
static RESULT
korb_m_fiber_yield(CTX *c, korb_value *slots, korb_ref self, korb_slice a)
{
    (void)self;
    KorbFiberRep *const rep = c->vm->running_fiber;
    if (UNLIKELY(rep == NULL))
        return korb_raise_fiber_error(c, slots, "can't yield from root fiber");
    rep->transfer = UNWRAP(korb_fiber_pack(c, slots, a));   /* several values → an Array */
    rep->fstate = 2;                                   /* suspended */
    /* Capture the TRUE live stack top, not c->slots_top: the latter is only
     * republished by korb_alloc, so between the fiber's last allocation and this
     * yield it can lag the real cursor (under-scan → live objects collected) or,
     * after a deeper call returned, sit stale-high (over-scan dead slots).  `slots`
     * is this yield frame's cursor — above all of the fiber's live data and below
     * its (unscanned) scratch — the same role c->slots_top plays for the active
     * stack at a korb_alloc-triggered GC. */
    rep->vslots_top = slots; rep->vslots_hw = c->slots_high_water;
    rep->vcfunc_link = c->cfunc_link;                  /* travels with this fiber's stack */
    rep->vcarry_base = c->carry_base; rep->vcarry_line_bits = c->carry_line_bits;
    const uint32_t mask_gen = c->vm->sigmask_gen;
    rep->mask_gen = mask_gen;
    if (rep->resumer_mask_gen != mask_gen) korb_signal_park_pending(c->vm);
    swapcontext((ucontext_t *)rep->uctx, (ucontext_t *)rep->resume_uctx);  /* === out === */
    korb_sigmask_resume(c->vm, mask_gen);
    /* === resumed: resume() restored c->slots to ours === */
    rep->fstate = 1;
    if (UNLIKELY(rep->killing)) {                      /* Fiber#kill: unwind (ensure blocks run) */
        rep->pending_raise = 0;
        return RESULT_RAISE_(korb_fiber_kill_sentinel(c));
    }
    if (UNLIKELY(rep->pending_raise)) {                /* Fiber#raise delivery point */
        rep->pending_raise = 0;
        return RESULT_RAISE_(rep->transfer);
    }
    return RESULT_OK(rep->transfer);                   /* value from the next resume */
}
