/* koruby_precise — fiber.c: stackful coroutines (Fiber), #included into
 * korb_runtime.c's TU.  Each fiber gets its own value-stack (fixed mmap) and
 * native C-stack (malloc) and switches via ucontext.  The moving GC object is a
 * thin handle; all mutable state + roots live in a libc-stable KorbFiberRep,
 * scanned via the vm fiber list (see AROH_VISIT_ROOTS).  <ucontext.h> is
 * included at the top of korb_runtime.c (before koruby macros). */

#define KORB_FIBER_VSLOTS_BYTES ((size_t)2 << 20)   /* per-fiber value stack */
#define KORB_FIBER_CSTACK_BYTES ((size_t)1 << 20)   /* per-fiber native stack (1 MiB) */
#define KORB_FIBER_VSLOTS_MARGIN 1024               /* slots reserved below limit */
#define KORB_FIBER_CSTACK_MARGIN ((size_t)64 << 10) /* native-stack floor margin */

/* ---- FiberError (const-only class; etype stays RUNTIME, exc_class drives
 * rescue/#class — same pattern as ThreadError) ------------------------------ */
static RESULT
korb_raise_fiber_error(CTX *c, VALUE *slots, const char *msg)
{
    const VALUE cls = korb_const_get(c->vm, korb_intern(c->vm, "FiberError", 10));
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "%s", msg);
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value))
        ARO_STORE(c, VAL2EXC(r.value), (VALUE *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    return r;
}

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
    if (r.state == KORB_RAISE && r.value == KORB_UNDEF) r = RESULT_OK(KORB_NIL);   /* #kill sentinel */
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
    rep->transferred = 0; rep->killing = 0;
    rep->storage = KORB_NIL;                           /* Fiber#storage (lazily inherited on first read) */
    rep->fibobj = (VALUE)fb;                           /* Fiber.current (root; no GC between alloc and here) */
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

static VALUE korb_fiber_kill_sentinel(CTX *c);   /* fwd (Fiber#kill unwind payload) */

/* Switch into `rep` (fstate 0 or 2) carrying `xfer` — the shared engine behind
 * Fiber#resume and Fiber#raise.  With deliver_raise, the fiber's Fiber.yield
 * raises xfer instead of returning it.  Returns the value the fiber next
 * yields (or its block's result when it finishes; RAISE if it died raising). */
static RESULT
korb_fiber_switch_in(CTX *c, VALUE *slots, KorbFiberRep *const rep, VALUE xfer, int deliver_raise)
{
    rep->transfer = xfer;
    rep->pending_raise = deliver_raise ? 1u : 0u;

    KorbFiberRep *const prev = c->vm->running_fiber;
    VALUE *const s_slots = c->slots; VALUE *const s_top = c->slots_top;
    VALUE *const s_limit = c->slots_limit; VALUE *const s_hw = c->slots_high_water;
    const char *const s_cstack = c->cstack_limit;
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
                    (unsigned)((uintptr_t)c >> 32), (unsigned)((uintptr_t)c & 0xFFFFFFFFu));
    }
    /* activate the fiber's stacks */
    c->slots = rep->vslots; c->slots_top = rep->vslots_top;
    c->slots_limit = rep->vslots_limit; c->slots_high_water = rep->vslots_hw;
    c->cstack_limit = (const char *)rep->cstack + KORB_FIBER_CSTACK_MARGIN;
    korb_re_sync_floor(c);   /* astrogre \g<> guard must use the fiber's stack */
    c->vm->running_fiber = rep;
    /* `$!` is fiber-local: the fiber starts from ITS own errinfo depth, and the
     * resumer's stack is hidden while it runs (and restored on the way back). */
    const uint32_t s_errinfo_n = c->errinfo_n;
    c->errinfo_n = rep->errinfo_n;

    swapcontext(&here, (ucontext_t *)rep->uctx);       /* === into the fiber === */

    /* === back: fiber yielded or finished === */
    rep->errinfo_n = c->errinfo_n;                     /* keep the fiber's depth for its next resume */
    c->errinfo_n = s_errinfo_n;
    c->vm->running_fiber = prev;
    c->slots = s_slots; c->slots_top = s_top; c->slots_limit = s_limit;
    c->slots_high_water = s_hw; c->cstack_limit = s_cstack;
    korb_re_sync_floor(c);   /* restore the outer stack's floor */
    if (prev == NULL) c->vm->main_slots = NULL;

    /* While we were away, someone may have killed US (a child fiber calling
     * parent.kill).  We are the one at a switch point, so unwind here. */
    if (prev != NULL && prev->killing) { prev->killing = 0; return RESULT_RAISE_(korb_fiber_kill_sentinel(c)); }
    if (rep->raised) { rep->raised = 0; return RESULT_RAISE_(rep->transfer); }
    return RESULT_OK(rep->transfer);
}

/* Fiber#resume([v]) — switch into the fiber; returns the value it yields (or its
 * block's result when it finishes). */
static RESULT
korb_m_fiber_resume(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    KorbFiberRep *const rep = VAL2FIBER(VALUE_REF_GET(self))->rep;
    if (UNLIKELY(rep->fstate == 3)) return korb_raise_fiber_error(c, slots, "attempt to resume a terminated fiber");
    if (UNLIKELY(rep->fstate == 1)) return korb_raise_fiber_error(c, slots, "attempt to resume a resumed fiber (double resume)");
    if (UNLIKELY(rep->transferred))   /* CRuby: a transferred fiber is not resumable */
        return korb_raise_fiber_error(c, slots, "attempt to yield on a not resumed fiber");
    return korb_fiber_switch_in(c, slots, rep,
                                (VALUE_SLICE_LEN(a) >= 1) ? VALUE_SLICE_GET(a, 0) : KORB_NIL, 0);
}

/* The value a killed fiber unwinds with.  A plain (non-Exception) payload so no
 * `rescue => e` can intercept it; only `ensure` runs, as CRuby's kill does. */
static VALUE korb_fiber_kill_sentinel(CTX *c) {
    (void)c;
    return KORB_UNDEF;
}

/* Fiber#transfer([v]) — hand control over without becoming the target's resumer.
 * koruby keeps the C-stack discipline of resume (control comes back here when
 * the target yields or finishes); what transfer adds is the bookkeeping that
 * makes #resume on a transferred fiber (and #transfer to a yielding one) the
 * FiberErrors CRuby raises. */
static RESULT
korb_m_fiber_transfer(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    KorbFiberRep *const rep = VAL2FIBER(VALUE_REF_GET(self))->rep;
    if (UNLIKELY(rep->fstate == 3)) return korb_raise_fiber_error(c, slots, "attempt to resume a terminated fiber");
    if (UNLIKELY(rep == c->vm->running_fiber)) return korb_raise_fiber_error(c, slots, "attempt to transfer to self");
    if (UNLIKELY(rep->fstate == 2 && !rep->transferred))
        return korb_raise_fiber_error(c, slots, "attempt to transfer to a yielding fiber");
    rep->transferred = 1;
    return korb_fiber_switch_in(c, slots, rep,
                                (VALUE_SLICE_LEN(a) >= 1) ? VALUE_SLICE_GET(a, 0) : KORB_NIL, 0);
}

/* Fiber#kill — terminate the fiber at its suspension point (ensure blocks run).
 * An unborn or dead fiber is simply marked dead. */
static RESULT
korb_m_fiber_kill(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)a;
    KorbFiberRep *const rep = VAL2FIBER(VALUE_REF_GET(self))->rep;
    if (rep->fstate == 0 || rep->fstate == 3) { rep->fstate = 3; return RESULT_OK(VALUE_REF_GET(self)); }
    if (UNLIKELY(rep == c->vm->running_fiber))
        return RESULT_RAISE_(korb_fiber_kill_sentinel(c));   /* killing myself: unwind here */
    if (rep->fstate == 1) {
        /* Running, but not us: it is an ancestor in the resume chain (a child
         * killing its parent).  Flag it — it unwinds when control returns to it,
         * which is the point where it is safe to touch its stack. */
        rep->killing = 1;
        return RESULT_OK(VALUE_REF_GET(self));
    }
    rep->killing = 1;
    const RESULT r = korb_fiber_switch_in(c, slots, rep, KORB_NIL, 0);
    rep->killing = 0;
    if (r.state == KORB_RAISE && r.value == KORB_UNDEF) { /* the sentinel came back out: swallow it */
        rep->fstate = 3;
        return RESULT_OK(VALUE_REF_GET(self));
    }
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Fiber#raise(...) — build the exception in the calling context (CRuby 4.0
 * semantics: the automatic cause comes from the caller, not the fiber), then
 * deliver it at the fiber's suspension point.  Kernel#raise-style args. */
static RESULT
korb_m_fiber_raise(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    KorbFiberRep *const rep = VAL2FIBER(VALUE_REF_GET(self))->rep;
    if (UNLIKELY(rep->fstate == 0))
        return korb_raise_fiber_error(c, slots, "cannot raise exception on unborn fiber");
    if (UNLIKELY(rep->fstate == 3))
        return korb_raise_fiber_error(c, slots, "attempt to resume a terminated fiber");
    const RESULT r = korb_exc_build_with_cause(c, slots, a);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;            /* argument error → caller */
    if (rep == c->vm->running_fiber ||
        (c->vm->running_fiber == NULL && rep == c->vm->root_fiber))
        return RESULT_RAISE_(r.value);                         /* raise on the current fiber */
    if (UNLIKELY(rep->fstate == 1))                            /* running deeper in the resume chain */
        return korb_raise_fiber_error(c, slots, "attempt to resume a resumed fiber (double resume)");
    return korb_fiber_switch_in(c, slots, rep, r.value, 1);
}

/* Fiber.current — the running fiber, or the implicit root fiber on the main
 * stack (CRuby returns a Fiber object there too; koruby has no object for the
 * root, so it reports nil rather than inventing one). */
static RESULT
korb_m_fiber_s_current(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)a;
    const KorbFiberRep *const rep = c->vm->running_fiber;
    if (rep) return RESULT_OK(rep->fibobj);
    /* The main stack has no rep of its own; hand out a stand-in Fiber (built
     * once, kept alive through fiber_list's fibobj edge) so callers still get a
     * Fiber.  It is permanently "running", so #resume reports a double resume
     * exactly as resuming the current fiber does. */
    if (c->vm->root_fiber != NULL) return RESULT_OK(c->vm->root_fiber->fibobj);
    KorbFiber *fb = korb_alloc(c, slots, sizeof(KorbFiber), KORB_OBJ_FIBER);   /* no GC below */
    KorbFiberRep *rp = calloc(1, sizeof(KorbFiberRep));
    if (!rp) { fprintf(stderr, "koruby_precise: oom (root fiber rep)\n"); abort(); }
    fb->rep = rp;
    rp->fibobj = (VALUE)fb;
    rp->transfer = KORB_NIL;
    rp->storage = KORB_NIL;
    rp->captured_self = KORB_NIL;
    rp->fstate = 1;                                    /* running: never resumable */
    rp->link = c->vm->fiber_list;                      /* rooted via the fibobj edge */
    c->vm->fiber_list = rp;
    c->vm->root_fiber = rp;
    (void)self;
    return RESULT_OK(rp->fibobj);
}

/* ---- Fiber storage (fiber-local variables) --------------------------------
 * Each fiber owns a Hash, created on first write.  A new fiber inherits a COPY
 * of its creator's storage the first time it is touched (CRuby semantics), so
 * writes never leak back to the parent. */
static KorbFiberRep *korb_fiber_cur_rep(CTX *c, VALUE *slots) {
    KorbFiberRep *rep = c->vm->running_fiber;
    if (rep == NULL) {                                  /* main stack: materialize the root fiber */
        (void)korb_m_fiber_s_current(c, slots, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0));
        rep = c->vm->root_fiber;
    }
    return rep;
}
/* The storage Hash of `rep`, or nil when it has none and none is requested. */
static RESULT korb_fiber_storage_of(CTX *c, VALUE *slots, KorbFiberRep *rep, bool create) {
    if (rep == NULL) return RESULT_OK(KORB_NIL);
    if (rep->storage == KORB_NIL && create) {
        const RESULT hr = korb_hash_new(c, slots, 4);
        if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
        rep->storage = hr.value;                        /* rep is libc-stable; the field is a GC root */
    }
    return RESULT_OK(rep->storage);
}
/* Fiber#storage / #storage= */
static RESULT korb_m_fiber_storage(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    return korb_fiber_storage_of(c, slots, VAL2FIBER(VALUE_REF_GET(self))->rep, false);
}
static RESULT korb_m_fiber_storage_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE h = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(h != KORB_NIL && !KORB_HASH_P(h)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "storage must be a hash");
    VAL2FIBER(VALUE_REF_GET(self))->rep->storage = h;
    return RESULT_OK(h);
}
/* Fiber[key] / Fiber[key] = value — the RUNNING fiber's storage. */
static RESULT korb_fiber_storage_key(CTX *c, VALUE *slots, VALUE k, VALUE *out) {
    if (SYMBOL_P(k)) { *out = k; return RESULT_OK(KORB_TRUE); }
    if (KORB_STRING_P(k)) {
        *out = ID2SYM(korb_intern(c->vm, korb_strbuf_data(VAL2STR(k)->buf), VAL2STR(k)->len));
        return RESULT_OK(KORB_TRUE);
    }
    char ib[128]; char *b = NULL; size_t bl = 0;
    FILE *ms = open_memstream(&b, &bl);
    if (ms) { korb_fprint_inspect(c, ms, k); fclose(ms); }
    snprintf(ib, sizeof ib, "%s", b ? b : "?"); free(b);
    return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", ib);
}
static RESULT korb_m_fiber_s_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE key;
    CHECK(korb_fiber_storage_key(c, slots, VALUE_SLICE_GET(a, 0), &key));
    slots[0] = key;
    KorbFiberRep *const rep = korb_fiber_cur_rep(c, slots + 1);
    if (rep == NULL || rep->storage == KORB_NIL) return RESULT_OK(KORB_NIL);
    const int32_t i = korb_hash_find(VAL2HASH(rep->storage), slots[0]);
    return RESULT_OK(i >= 0 ? korb_items_data(VAL2HASH(rep->storage)->items)[2 * i + 1] : KORB_NIL);
}
static RESULT korb_m_fiber_s_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE key;
    CHECK(korb_fiber_storage_key(c, slots, VALUE_SLICE_GET(a, 0), &key));
    slots[0] = key; slots[1] = VALUE_SLICE_GET(a, 1);
    KorbFiberRep *const rep = korb_fiber_cur_rep(c, slots + 2);
    const RESULT sr = korb_fiber_storage_of(c, slots + 2, rep, true);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[2] = sr.value;
    CHECK(korb_hash_set(c, slots + 3, VALUE_REF_AT(&slots[2]), VALUE_REF_AT(&slots[0]), slots[1]));
    rep->storage = slots[2];                            /* re-read: the set may have moved it */
    return RESULT_OK(slots[1]);
}
/* Fiber.blocking? — koruby's fibers never park the scheduler, so the main
 * (blocking) fiber reports 1 and any other fiber false, as CRuby does. */
static RESULT korb_m_fiber_s_blocking_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)self; (void)a;
    return RESULT_OK(c->vm->running_fiber == NULL ? LONG2FIX(1) : KORB_FALSE);
}
static RESULT korb_m_fiber_blocking_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)self; (void)a;
    return RESULT_OK(KORB_FALSE);                       /* per-fiber flag; koruby has no scheduler */
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
        return korb_raise_fiber_error(c, slots, "can't yield from root fiber");
    rep->transfer = (VALUE_SLICE_LEN(a) >= 1) ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    rep->fstate = 2;                                   /* suspended */
    /* Capture the TRUE live stack top, not c->slots_top: the latter is only
     * republished by korb_alloc, so between the fiber's last allocation and this
     * yield it can lag the real cursor (under-scan → live objects collected) or,
     * after a deeper call returned, sit stale-high (over-scan dead slots).  `slots`
     * is this yield frame's cursor — above all of the fiber's live data and below
     * its (unscanned) scratch — the same role c->slots_top plays for the active
     * stack at a korb_alloc-triggered GC. */
    rep->vslots_top = slots; rep->vslots_hw = c->slots_high_water;
    swapcontext((ucontext_t *)rep->uctx, (ucontext_t *)rep->resume_uctx);  /* === out === */
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
