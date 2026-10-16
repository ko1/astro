/* TracePoint events (#included into korb_runtime.c).  Nothing here runs until a TracePoint is enabled: the
 * parser's records (node.h, struct korb_trace_rec) sit unread, and no dispatch path tests a flag.  Enabling
 * one with a swapped event (KORB_TRE_SWAP) puts every node of user code back on its interpreter dispatcher —
 * a fused SD would run a recorded statement without asking it — and each recorded node on
 * korb_trace_dispatch, then drops the method caches (and AOT direct calls) that hold the old entry
 * dispatchers.  Disabling the last one restores what was there.  The other events fire from their own (cold)
 * places, gated on vm->trace_mask. */

static RESULT korb_binding_at_frame(CTX *c, korb_value *slots, korb_value *fbase, korb_value self, uint32_t bfile, uint32_t bline);   /* fwd */

struct korb_trace_rec *
korb_trace_rec_new(struct korb_vm *const vm, struct Node *const n, const uint8_t kind)
{
    if (vm->tr_n == vm->tr_nchunk * KORB_TR_CHUNK) {
        struct korb_trace_rec **const nc = realloc(vm->tr_chunk, sizeof(*nc) * (vm->tr_nchunk + 1));
        struct korb_trace_rec *const ch = nc ? calloc(KORB_TR_CHUNK, sizeof(*ch)) : NULL;
        if (!ch) { fprintf(stderr, "koruby_precise: out of memory (trace records)\n"); abort(); }
        nc[vm->tr_nchunk++] = ch;
        vm->tr_chunk = nc;
    }
    struct korb_trace_rec *const r = &vm->tr_chunk[vm->tr_n / KORB_TR_CHUNK][vm->tr_n % KORB_TR_CHUNK];   /* calloc'd, used once */
    r->n = n; r->kind = kind;
    n->head.line = (int32_t)++vm->tr_n;
    return r;
}

static const struct korb_trace_rec *
korb_trace_rec_of(const struct korb_vm *const vm, const struct Node *const n)
{
    if (n->head.line <= 0 || (uint32_t)n->head.line > vm->tr_n) return NULL;
    const uint32_t i = (uint32_t)n->head.line - 1;
    const struct korb_trace_rec *const r = &vm->tr_chunk[i / KORB_TR_CHUNK][i % KORB_TR_CHUNK];
    return r->n == n ? r : NULL;
}

/* ---- the swap ----------------------------------------------------------------------------------------- */

struct korb_trace_walk { struct korb_vm *vm; struct Node **seen; uint32_t cap, n; };

static bool
korb_trace_seen(struct korb_trace_walk *const w, struct Node *const n)
{
    if (w->n * 2 >= w->cap) {
        const uint32_t ncap = w->cap ? w->cap * 2 : 4096;
        struct Node **const nt = calloc(ncap, sizeof *nt);
        if (!nt) { fprintf(stderr, "koruby_precise: out of memory (trace walk)\n"); abort(); }
        for (uint32_t i = 0; i < w->cap; i++) {
            if (w->seen[i] == NULL) continue;
            uint32_t j = (uint32_t)((uintptr_t)w->seen[i] >> 4) * 2654435761u & (ncap - 1);
            while (nt[j] != NULL) j = (j + 1) & (ncap - 1);
            nt[j] = w->seen[i];
        }
        free(w->seen);
        w->seen = nt; w->cap = ncap;
    }
    uint32_t j = (uint32_t)((uintptr_t)n >> 4) * 2654435761u & (w->cap - 1);
    for (; w->seen[j] != NULL; j = (j + 1) & (w->cap - 1)) if (w->seen[j] == n) return true;
    w->seen[j] = n; w->n++;
    return false;
}

static void
korb_trace_save(struct korb_vm *const vm, struct Node *const n)
{
    if (vm->tr_saved_n == vm->tr_saved_cap) {
        vm->tr_saved_cap = vm->tr_saved_cap ? vm->tr_saved_cap * 2 : 1024;
        vm->tr_saved = realloc(vm->tr_saved, sizeof(*vm->tr_saved) * vm->tr_saved_cap);
        if (!vm->tr_saved) { fprintf(stderr, "koruby_precise: out of memory (trace saves)\n"); abort(); }
    }
    vm->tr_saved[vm->tr_saved_n].n = n;
    vm->tr_saved[vm->tr_saved_n].d = (void *)n->head.dispatcher;
    vm->tr_saved_n++;
}

static bool
korb_trace_visit(struct Node *const n, void *const a)
{
    struct korb_trace_walk *const w = a;
    if (korb_trace_seen(w, n)) return false;
    const node_dispatcher_func_t d = korb_trace_rec_of(w->vm, n) ? korb_trace_dispatch : n->head.kind->default_dispatcher;
    if (n->head.dispatcher != d) { korb_trace_save(w->vm, n); n->head.dispatcher = d; }
    return true;
}

/* The AOT direct calls bound to an entry's SD: unbound so the next call reads the entry again (they may bind
 * anew afterwards, to whatever the entry holds then). */
static void
korb_dc_unbind_all(struct korb_vm *const vm)
{
    for (uint32_t i = 0; i < vm->dc_cap; i++) {
        struct korb_dc_ent *const e = &vm->dc_tab[i];
        if (e->ic == NULL || e->ret == NULL) continue;
        astro_ld_call_reset(e->ret, (uintptr_t)korb_entry_call);
        e->ret = NULL;
        vm->dc_n--;
    }
}

static void
korb_trace_walk_from(struct korb_vm *const vm, const uint32_t from)
{
    struct korb_trace_walk w = { vm, NULL, 0, 0 };
    for (uint32_t i = from; i < vm->tr_n; i++) {
        struct Node *const n = vm->tr_chunk[i / KORB_TR_CHUNK][i % KORB_TR_CHUNK].n;
        korb_node_walk(n, korb_trace_visit, &w);
    }
    free(w.seen);
}

static void
korb_trace_swap(struct korb_vm *const vm, const bool on)
{
    if (on == vm->trace_swapped) return;
    if (on) {
        korb_trace_walk_from(vm, 0);
        vm->trace_swapped = true;
    } else {
        for (uint32_t i = vm->tr_saved_n; i-- > 0; ) vm->tr_saved[i].n->head.dispatcher = (node_dispatcher_func_t)vm->tr_saved[i].d;
        vm->tr_saved_n = 0;
        vm->trace_swapped = false;
    }
    vm->method_serial++;          /* the caches hold entry dispatchers */
    korb_dc_unbind_all(vm);
}

void
korb_trace_new_code(CTX *const c, struct Node *const root)
{
    struct korb_trace_walk w = { c->vm, NULL, 0, 0 };
    korb_node_walk(root, korb_trace_visit, &w);
    free(w.seen);
}

/* ---- firing --------------------------------------------------------------------------------------------- */

/* The keys `enable(target:)` matches a frame by: its own (a method's body, a block's entry), then those of the
 * blocks and the method it is written in (a target covers the blocks inside it).  The live chain only. */
static uint32_t
korb_trace_frame_keys(const struct korb_vm *const vm, const korb_value *const base, korb_value *const out, const uint32_t max)
{
    uint32_t n = 0;
    korb_value id = korb_id_get(base);
    korb_value h = korb_ep_get(base);
    if (h != 0 && (h & 7u) == 0 && !VAL2ENV(h)->closed && VAL2ENV(h)->loc == base) h = VAL2ENV(h)->prev;   /* its own env */
    while (n < max) {
        const uintptr_t p = (uintptr_t)(id & ~(korb_value)7u);
        if ((id & 7u) == KORB_FID_METHOD) {
            if (korb_bt_known_ptr(vm, p)) out[n++] = LONG2FIX((long)(uintptr_t)((const struct korb_method *)p)->body);
            break;
        }
        if ((id & 7u) != KORB_FID_BLOCK) break;
        out[n++] = LONG2FIX((long)p);
        if (h == 0) break;
        korb_value next;
        if (h & 1u) {
            const korb_value *const fb = (const korb_value *)(uintptr_t)(h & ~(uintptr_t)1u);
            id = korb_id_get(fb);
            next = korb_ep_get(fb);
            if (next != 0 && (next & 7u) == 0 && !VAL2ENV(next)->closed && VAL2ENV(next)->loc == fb) next = VAL2ENV(next)->prev;
        } else {
            id = VAL2ENV(h)->id;
            next = VAL2ENV(h)->prev;
        }
        h = next;
    }
    return n;
}

static const char *const korb_trace_ev_names[] = {   /* prelude TracePoint::EVENTS */
    "line", "class", "end", "call", "return", "c_call", "c_return", "b_call", "b_return", "a_call", "a_return",
    "thread_begin", "thread_end", "fiber_switch", "raise", "rescue", "script_compiled",
};
/* A block's parameters as Proc#parameters gives them (`lam`: a lambda's are required) */
static RESULT
korb_trace_block_params(CTX *c, korb_value *const sc, const struct Node *const entry, const bool lam)
{
    static const char *const knames[] = { "req", "opt", "rest", "keyreq", "key", "keyrest", "block", "nokey", "noblock" };
    const struct korb_param_info *const pi = entry->head.kind == &kind_node_entry ? (const struct korb_param_info *)entry->u.node_entry.param_info : NULL;
    const uint32_t n = pi ? pi->n : 0;
    sc[0] = UNWRAP(korb_ary_new(c, sc + 1, n ? n : 1));
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t kind = pi->e[i].kind;
        const char *const kn = (kind == 0 && !lam) ? "opt" : knames[kind];
        sc[1] = UNWRAP(korb_ary_new(c, sc + 2, 2));
        CHECK(korb_ary_push_val(c, sc + 2, KORB_REF_AT(&sc[1]), ID2SYM(korb_intern(c->vm, kn, (uint32_t)strlen(kn)))));
        if (pi->e[i].name) CHECK(korb_ary_push_val(c, sc + 2, KORB_REF_AT(&sc[1]), ID2SYM(pi->e[i].name)));
        CHECK(korb_ary_push_val(c, sc + 2, KORB_REF_AT(&sc[0]), sc[1]));
    }
    return RESULT_OK(sc[0]);
}

/* TracePoint.__fire(event, path, lineno, self, method_id, callee_id, defined_class, binding, value, keys, params)
 * for the frame at `base` (NULL: none — a thread's begin / end).  `sc` is free cells to stage in; `self` and `val`
 * are read off the cells they are passed in (rooted). */
static RESULT
korb_trace_fire_frame(CTX *c, korb_value *const sc, const uint32_t ev, korb_value *const base,
                      const korb_value *const self_cell, const uint32_t file, const uint32_t line, const korb_value *const val_cell)
{
    struct korb_vm *const vm = c->vm;
    if (c->trace_in_hook) return RESULT_OK(KORB_NIL);
    const struct korb_method *const hm = base ? korb_frame_home_method(vm, base) : NULL;
    const uint32_t evi = (uint32_t)__builtin_ctz(ev);
    sc[0] = korb_const_get(vm, korb_intern_lit(vm, "TracePoint", 10));
    sc[1] = ID2SYM(korb_intern(vm, korb_trace_ev_names[evi], strlen(korb_trace_ev_names[evi])));
    sc[2] = KORB_NIL;
    sc[3] = LONG2FIX((long)line);
    sc[4] = *self_cell;
    sc[5] = hm ? ID2SYM(hm->orig_mid ? hm->orig_mid : hm->mid) : KORB_NIL;
    sc[6] = hm ? ID2SYM(hm->mid) : KORB_NIL;
    sc[7] = hm ? (KORB_CLASS_P(hm->owner) ? hm->owner : korb_object_class(vm)) : KORB_NIL;
    sc[8] = KORB_NIL;
    sc[9] = val_cell ? *val_cell : KORB_NIL;
    sc[10] = KORB_NIL;
    sc[11] = KORB_NIL;
    if (base) {
        korb_value keys[16];
        const uint32_t nk = korb_trace_frame_keys(vm, base, keys, 16);
        sc[10] = UNWRAP(korb_ary_new(c, sc + 12, nk));
        for (uint32_t i = 0; i < nk; i++) UNWRAP(korb_ary_push_val(c, sc + 12, KORB_REF_AT(&sc[10]), keys[i]));
        const korb_value id = korb_id_get(base);
        if ((ev & (KORB_TRE_B_CALL | KORB_TRE_B_RETURN)) && (id & 7u) == KORB_FID_BLOCK) {
            const struct Node *const e = (const struct Node *)(uintptr_t)(id & ~(korb_value)7u);
            sc[11] = UNWRAP(korb_trace_block_params(c, sc + 12, e, e->head.flags.lambda_lit));
        }
    }
    if (file) {
        const char *const path = korb_sym_name(vm, file);
        sc[2] = UNWRAP(korb_str_new(c, sc + 12, path, (uint32_t)strlen(path)));
    }
    if (base) sc[8] = UNWRAP(korb_binding_at_frame(c, sc + 12, base, sc[4], 0, 0));
    c->trace_in_hook = true;
    const RESULT fr = korb_send(c, sc + 12, korb_intern_lit(vm, "__fire", 6), 0, 11);
    c->trace_in_hook = false;
    return fr;
}

/* … for a recorded node: `cur` is its cursor (the frame cells are read off it) */
static RESULT
korb_trace_fire(CTX *c, const korb_value *const cur, korb_value *const sc, const uint32_t ev,
                const struct korb_trace_rec *const r, const uint32_t line, const korb_value *const val_cell)
{
    korb_value *const base = (korb_value *)(uintptr_t)&cur[r->id_off] - KORB_ID_OFF;
    return korb_trace_fire_frame(c, sc, ev, base, &base[-1], r->file, line, val_cell);
}

/* The events with no node of their own (raise / rescue / thread begin and end / script_compiled), from their
 * cold paths: `base` the frame they happen in (or NULL), `line` 0 = that frame's current position unknown */
RESULT
korb_trace_event(CTX *c, korb_value *const sc, const uint32_t ev, korb_value *const base, const korb_value self,
                 const korb_value val)
{
    if (!(c->vm->trace_mask & ev) || c->trace_in_hook) return RESULT_OK(KORB_NIL);
    sc[0] = self; sc[1] = val;
    uint32_t file = 0;
    const uint32_t line = 0;
    if (base && korb_frames_snapshot_at(c, c->slots, c->slots_limit, base, 0, NULL) != 0) {
        const struct korb_frame_rec *const fr = &c->vm->fscratch[0];
        const char *const f = fr->file ? fr->file : korb_bt_cur_file(c->vm);
        file = korb_intern(c->vm, f, (uint32_t)strlen(f));
    }
    return korb_trace_fire_frame(c, sc + 2, ev, base, &sc[0], file, line, &sc[1]);
}

RESULT
korb_trace_dispatch(CTX *c, struct Node *n, korb_value *slots)
{
    struct korb_vm *const vm = c->vm;
    const node_dispatcher_func_t d = n->head.kind->default_dispatcher;
    const struct korb_trace_rec *const r = korb_trace_rec_of(vm, n);
    if (UNLIKELY(r == NULL || c->trace_in_hook || !vm->trace_swapped)) return d(c, n, slots);
    const uint32_t mask = vm->trace_mask;
    uint32_t open = 0, close = 0;
    if (r->kind & KORB_TR_CALL) { open = KORB_TRE_CALL; close = KORB_TRE_RETURN; }
    else if (r->kind & KORB_TR_BCALL) { open = KORB_TRE_B_CALL; close = KORB_TRE_B_RETURN; }
    else if (r->kind & KORB_TR_CLASS) { open = KORB_TRE_CLASS; close = KORB_TRE_END; }
    if (r->kind & KORB_TR_LINE) c->trace_ret_line = 0;
    if (open & mask) CHECK(korb_trace_fire(c, slots, slots, open, r, r->body_line, NULL));
    if ((r->kind & KORB_TR_LINE) && (vm->trace_mask & KORB_TRE_LINE)) CHECK(korb_trace_fire(c, slots, slots, KORB_TRE_LINE, r, r->line, NULL));
    const struct korb_trace_rec *const prev_rec = c->trace_cur_rec;
    korb_value *const prev_base = c->trace_cur_base;
    c->trace_cur_rec = r; c->trace_cur_base = (korb_value *)(uintptr_t)&slots[r->id_off] - KORB_ID_OFF;   /* a body too (`def m = expr`) */
    RESULT res = d(c, n, slots);
    c->trace_cur_rec = prev_rec; c->trace_cur_base = prev_base;
    if ((r->kind & KORB_TR_LINE) && res.state == KORB_RETURN && c->trace_ret_line == 0) c->trace_ret_line = r->line;   /* the `return`'s line */
    if (close & vm->trace_mask && vm->trace_swapped) {
        const uint32_t line = (res.state == KORB_RETURN && c->trace_ret_line) ? c->trace_ret_line : r->end_line;
        c->trace_ret_line = 0;
        slots[0] = res.value;                         /* rooted across the hook */
        slots[1] = res.state == KORB_RAISE ? KORB_NIL : res.value;
        const RESULT fr = korb_trace_fire(c, slots, slots + 2, close, r, line, &slots[1]);
        if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
        res.value = slots[0];
    }
    return res;
}

/* __trace_hooks(mask) (prelude TracePoint): the union of the enabled TracePoints' events */
static RESULT
korb_bi_trace_hooks(CTX *c, korb_value *slots, korb_slice a)
{
    (void)slots;
    const uint32_t mask = (uint32_t)FIX2LONG(KORB_SLICE_GET(a, 0));
    c->vm->trace_mask = mask;
    korb_trace_swap(c->vm, (mask & KORB_TRE_SWAP) != 0);
    return RESULT_OK(KORB_NIL);
}

/* Method / UnboundMethod / Proc #__trace_key: what korb_trace_frame_key gives the frames it runs */
static RESULT
korb_m_tp_target_key(CTX *c, korb_value *slots, korb_ref self, korb_slice a)
{
    (void)slots; (void)a;
    const korb_value v = KORB_REF_GET(self);
    if (KORB_PROC_P(v)) {
        const KorbProc *const p = VAL2PROC(v);
        return RESULT_OK(p->iseq != NULL && p->iseq != KORB_BLK_CPROC ? LONG2FIX((long)(uintptr_t)p->iseq) : KORB_NIL);
    }
    const KorbMethod *const mo = VAL2METH(v);
    const struct korb_method *m = mo->entry;
    if (m == NULL) {
        const korb_value k = KORB_CLASS_P(mo->owner) ? mo->owner : korb_dispatch_class(c, mo->recv);
        m = KORB_CLASS_P(k) ? korb_class_find_method(k, mo->mid, NULL) : NULL;
        if (m == NULL) m = korb_method_lookup(c->vm, mo->mid);
    }
    return RESULT_OK(m != NULL && m->kind == KORB_METHOD_ISEQ ? LONG2FIX((long)(uintptr_t)m->body) : KORB_NIL);
}

/* __trace_reentry(flag): TracePoint.allow_reentry lets events through inside a hook; answers the old state */
static RESULT
korb_bi_trace_reentry(CTX *c, korb_value *slots, korb_slice a)
{
    (void)slots;
    const bool prev = c->trace_in_hook;
    c->trace_in_hook = KORB_TRUTHY(KORB_SLICE_GET(a, 0));
    return RESULT_OK(prev ? KORB_TRUE : KORB_FALSE);
}

/* ---- ObjectSpace.trace_object_allocations ----------------------------------------------------------------- */

/* aro_gc_alloc_raw's hook (every allocation goes there while tracing: see korb_alloc_trace_set).  Inside the
 * allocator: no allocation here (the table is libc). */
static void
korb_alloc_trace_hook(void *const ctx, void *const payload)
{
    CTX *const c = ctx;
    struct korb_vm *const vm = c->vm;
    const struct korb_trace_rec *const r = c->trace_cur_rec;
    if (r == NULL) return;                                /* not under user code */
    const struct korb_method *m = NULL;
    const korb_value *const cf = (c->cfunc_link & 1u) ? korb_flink_ptr(c->cfunc_link) : NULL;
    if (cf != NULL && cf > c->trace_cur_base && (korb_id_get(cf) & 7u) == KORB_FID_CFUNC)
        m = (const struct korb_method *)(uintptr_t)(korb_id_get(cf) & ~(korb_value)7u);   /* a C method running for the statement */
    else if (c->trace_cur_base != NULL)
        m = korb_frame_home_method(vm, c->trace_cur_base);
    if (vm->alloc_n == vm->alloc_cap) {
        vm->alloc_cap = vm->alloc_cap ? vm->alloc_cap * 2 : 1024;
        vm->alloc_recs = realloc(vm->alloc_recs, sizeof(*vm->alloc_recs) * vm->alloc_cap);
        if (!vm->alloc_recs) { fprintf(stderr, "koruby_precise: out of memory (allocation records)\n"); abort(); }
    }
    vm->alloc_recs[vm->alloc_n] = (struct korb_alloc_rec){ r->file, r->line ? r->line : r->body_line, m, ARO_GC_COMMON(c)->stats.gc_count };
    korb_wtab_put(&vm->alloc_tab, (korb_value)(uintptr_t)payload, LONG2FIX((long)vm->alloc_n));
    vm->alloc_n++;
}

static void
korb_alloc_trace_set(CTX *const c, const bool on)
{
    struct korb_vm *const vm = c->vm;
    AroGcCommonState *const g = ARO_GC_COMMON(c);
    if (on) {
        vm->alloc_saved_stress = g->stress; vm->alloc_saved_iv = g->stress_interval;
        if (!g->stress) { g->stress = true; g->stress_interval = UINT64_MAX; }   /* every allocation to the hook, none collects */
        g->alloc_hook = korb_alloc_trace_hook;
        vm->trace_mask |= KORB_TRE_ALLOC;
    } else {
        g->alloc_hook = NULL;
        g->stress = vm->alloc_saved_stress; g->stress_interval = vm->alloc_saved_iv;
        vm->trace_mask &= ~KORB_TRE_ALLOC;
    }
    korb_trace_swap(vm, (vm->trace_mask & KORB_TRE_SWAP) != 0);
}

/* __alloc_trace(op[, obj]): 0 start / 1 stop (nested; answers the depth left) / 2 clear /
 * 3 obj's [file, line, class path, method id, generation] or nil */
static RESULT
korb_bi_alloc_trace(CTX *c, korb_value *slots, korb_slice a)
{
    struct korb_vm *const vm = c->vm;
    switch (FIX2LONG(KORB_SLICE_GET(a, 0))) {
      case 0:
        if (vm->alloc_depth++ == 0) korb_alloc_trace_set(c, true);
        return RESULT_OK(LONG2FIX((long)vm->alloc_depth));
      case 1:
        if (vm->alloc_depth > 0 && --vm->alloc_depth == 0) korb_alloc_trace_set(c, false);
        return RESULT_OK(LONG2FIX((long)vm->alloc_depth));
      case 2:
        vm->alloc_tab.cnt = 0;
        if (vm->alloc_tab.idx) memset(vm->alloc_tab.idx, 0, sizeof(uint32_t) * ((size_t)vm->alloc_tab.idx_mask + 1));
        vm->alloc_n = 0;
        return RESULT_OK(KORB_NIL);
      default: {
        const korb_value obj = KORB_SLICE_GET(a, 1);
        if (!AROH_IS_GC_OBJECT(obj)) return RESULT_OK(KORB_NIL);
        const int32_t i = korb_wtab_find(&vm->alloc_tab, obj);
        if (i < 0) return RESULT_OK(KORB_NIL);
        const struct korb_alloc_rec ar = vm->alloc_recs[FIX2LONG(vm->alloc_tab.val[i])];
        slots[0] = UNWRAP(korb_ary_new(c, slots + 1, 5));
        const char *const f = korb_sym_name(vm, ar.file);
        slots[1] = UNWRAP(korb_str_new(c, slots + 2, f, (uint32_t)strlen(f)));
        CHECK(korb_ary_push_val(c, slots + 2, KORB_REF_AT(&slots[0]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 2, KORB_REF_AT(&slots[0]), LONG2FIX((long)ar.line)));
        const korb_value owner = ar.m && KORB_CLASS_P(ar.m->owner) && !VAL2CLASS(ar.m->owner)->is_singleton ? ar.m->owner : KORB_NIL;
        CHECK(korb_ary_push_val(c, slots + 2, KORB_REF_AT(&slots[0]), owner));   /* the prelude names it (Module#name) */
        CHECK(korb_ary_push_val(c, slots + 2, KORB_REF_AT(&slots[0]), ar.m ? ID2SYM(ar.m->orig_mid ? ar.m->orig_mid : ar.m->mid) : KORB_NIL));
        CHECK(korb_ary_push_val(c, slots + 2, KORB_REF_AT(&slots[0]), LONG2FIX((long)ar.gen)));
        return RESULT_OK(slots[0]);
      }
    }
}
