/* koruby_precise — enumerator.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Enumerator (eager): values materialized at creation ----------------- */
/* Build from a values Array + a desc String (or nil).  vals/desc must be rooted
 * by the caller's slots region; we root-copy into the new object. */
static RESULT
korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc)
{
    slots[0] = vals; slots[1] = desc;                  /* root across alloc */
    KorbEnumerator *e = korb_alloc(c, slots + 2, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->values, slots[0]);
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->desc,   slots[1]);
    return RESULT_OK((VALUE)e);
}
/* Enumerator::Yielder#yield / #<< — append the yielded value(s) to the collector
 * array stashed in the yielder's @__c ivar.  Multiple args → an array element;
 * none → nil.  Returns self (`y << v << w` chains).  Eager model: the block runs
 * once at Enumerator.new, so an unbounded generator (`loop { y << ... }`) would
 * not terminate — only finite generators are supported. */
/* Apply the deferred lazy `ops` chain to a candidate value while driving a
 * generator.  slots[voff]=value (transformed in place by map/filter_map),
 * slots[voff+1]=ops Array, slots[voff+2]=op_state Array (per-op Fixnum counters,
 * mutated).  Sets *keep (value survives the chain) and *term (a take/take_while
 * bound was reached → stop the whole drive).  Uses slots[voff+3..] as scratch. */
static RESULT korb_lazy_apply(CTX *c, VALUE *slots, uint32_t voff, bool *keep, bool *term);
static RESULT korb_m_yielder_push(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t ac = VALUE_SLICE_LEN(a);
    const uint32_t csym = korb_intern(c->vm, "@__c", 4);   /* @__c = [collector, limit, ops, op_state] */
    slots[1] = korb_ivar_get(c, VALUE_REF_GET(self), csym);     /* the tuple (rooted) */
    slots[0] = VAL2ARY(slots[1])->items->data[0];              /* collector (rooted in slots[0]) */
    const VALUE limv = VAL2ARY(slots[1])->items->data[1];     /* limit (fixnum, immediate) */
    VALUE v;
    if (ac == 1) v = VALUE_SLICE_GET(a, 0);
    else if (ac == 0) v = KORB_NIL;
    else {                                                      /* y.yield(a, b) → [a, b] */
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, ac));
        for (uint32_t i = 0; i < ac; i++)
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), VALUE_SLICE_GET(a, i)));
        v = slots[2];
    }
    /* apply any deferred lazy ops (select/map/reject/filter_map/take_while/...) */
    bool keep = true, term = false;
    const VALUE ops = VAL2ARY(slots[1])->items->data[2];      /* nil for a raw generator */
    if (ops != KORB_NIL && VAL2ARY(ops)->len) {
        slots[3] = v;                                          /* value (transformed by map) */
        slots[4] = ops;                                        /* ops (rooted) */
        slots[5] = VAL2ARY(slots[1])->items->data[3];         /* op_state (rooted) */
        RESULT ar = korb_lazy_apply(c, slots, 3, &keep, &term);
        if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
        v = slots[3];                                         /* possibly mapped */
        slots[1] = korb_ivar_get(c, VALUE_REF_GET(self), csym);   /* re-read after op dispatch GC */
        slots[0] = VAL2ARY(slots[1])->items->data[0];
    }
    if (keep) {
        slots[2] = v;                                         /* root across the push alloc */
        CHECK(korb_ary_push_val(c, slots + 6, VALUE_REF_AT(&slots[0]), slots[2]));
    }
    /* bounded: stop on a take/take_while bound, or once `limit` values collected
     * (StopIteration — an enclosing `loop` swallows it, else gen_run catches it). */
    if (term) return korb_raise(c, slots + 1, KORB_E_STOP_ITERATION, 0, "iteration reached limit");
    if (FIXNUM_P(limv) && FIX2LONG(limv) >= 0 && VAL2ARY(slots[0])->len >= (uint32_t)FIX2LONG(limv))
        return korb_raise(c, slots + 1, KORB_E_STOP_ITERATION, 0, "iteration reached limit");
    return RESULT_OK(VALUE_REF_GET(self));
}
/* A deferred generator enumerator: stores the block as a proc in `source`, mode 3.
 * Terminals re-run the proc (bounded by a limit via the yielder) — so infinite
 * generators work for first/take/next without eager materialization. */
static RESULT korb_enum_gen_new(CTX *c, VALUE *slots, VALUE proc) {
    slots[0] = proc;
    KorbEnumerator *e = korb_alloc(c, slots + 1, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    e->mode = 3;
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->source, slots[0]);
    return RESULT_OK((VALUE)e);
}
/* Run the generator proc, collecting up to `limit` values (limit < 0 = unbounded,
 * finite only).  Returns the collector Array. */
static RESULT korb_enum_gen_run(CTX *c, VALUE *slots, VALUE_REF self, intptr_t limit) {
    struct korb_vm *const vm = c->vm;
    if (vm->yielder_class == KORB_NIL) {                        /* lazily build Enumerator::Yielder (a GC root) */
        slots[0] = UNWRAP(korb_class_new(c, slots, 0, korb_builtin_class_obj(vm, KORB_C_OBJECT)));
        korb_class_def_cfn(c, slots[0], "yield", korb_m_yielder_push, -1);
        korb_class_def_cfn(c, slots[0], "<<",    korb_m_yielder_push, -1);
        vm->yielder_class = slots[0];
    }
    /* deferred lazy ops carried by the generator + a fresh per-run op_state
     * (Fixnum counters: take/drop init to the count, drop_while to 1). */
    slots[0] = SELF_ENUM->ops;                                /* ops (nil for a raw generator) */
    slots[1] = KORB_NIL;                                      /* op_state */
    if (slots[0] != KORB_NIL && VAL2ARY(slots[0])->len) {
        const uint32_t no = VAL2ARY(slots[0])->len;
        slots[1] = UNWRAP(korb_ary_new(c, slots + 2, no));
        VALUE_REF st = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < no; i++) {
            const KorbArray *pair = VAL2ARY(VAL2ARY(slots[0])->items->data[i]);
            const char *const opn = korb_sym_name(vm, SYM2ID(pair->items->data[0]));
            intptr_t init = 0;
            if (!strcmp(opn, "take") || !strcmp(opn, "drop")) init = FIX2LONG(pair->items->data[1]);
            else if (!strcmp(opn, "drop_while")) init = 1;
            CHECK(korb_ary_push_val(c, slots + 2, st, LONG2FIX(init)));
        }
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 8));         /* collector (returned) */
    slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 4));        /* @__c = [collector, limit, ops, op_state] */
    { VALUE_REF pr = VALUE_REF_AT(&slots[3]);
      CHECK(korb_ary_push_val(c, slots + 4, pr, slots[2]));
      CHECK(korb_ary_push_val(c, slots + 4, pr, LONG2FIX(limit)));
      CHECK(korb_ary_push_val(c, slots + 4, pr, slots[0]));
      CHECK(korb_ary_push_val(c, slots + 4, pr, slots[1])); }
    slots[4] = UNWRAP(korb_obj_new(c, slots + 4, vm->yielder_class));
    CHECK(korb_ivar_set(c, slots + 5, VALUE_REF_AT(&slots[4]), korb_intern(vm, "@__c", 4), slots[3]));
    if (limit == 0) return RESULT_OK(slots[2]);               /* take(0): no run */
    slots[5] = SELF_ENUM->source;                            /* the generator proc */
    slots[6] = slots[4];                                     /* arg0 = yielder */
    RESULT br = korb_send_impl(c, slots + 7, korb_intern(vm, "call", 4), 0, 1, NULL, NULL, KORB_NIL);
    if (br.state == KORB_RAISE && KORB_EXC_P(br.value) && VAL2EXC(br.value)->etype == KORB_E_STOP_ITERATION)
        return RESULT_OK(slots[2]);                           /* hit the bound (or natural end) → collector */
    if (UNLIKELY(br.state != KORB_NORMAL && br.state != KORB_BREAK)) return br;
    return RESULT_OK(slots[2]);                               /* finished naturally (finite) */
}
/* ---- lazy / cycle enumerators (deferred, possibly-infinite source) -------- */
/* A lazy enumerator carries a `source` (Array/Range) + a chain of deferred `ops`
 * (Array of [op_sym, proc] pairs) + a mode (1 lazy, 2 cycle).  Terminal methods
 * (first/take/force/to_a/each) drive the source, applying ops, bounded by a
 * limit (required for an infinite source). */
static RESULT korb_lazy_new(CTX *c, VALUE *slots, VALUE source, uint8_t mode) {
    slots[0] = source;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));            /* empty ops */
    KorbEnumerator *e = korb_alloc(c, slots + 2, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    e->mode = mode;
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->source, slots[0]);
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->ops, slots[1]);
    return RESULT_OK((VALUE)e);
}
/* Return a new lazy enum = self with one more op appended (op_sym, blk_proc). */
static RESULT korb_lazy_chain(CTX *c, VALUE *slots, VALUE_REF self, const char *op, VALUE blk_proc) {
    const KorbEnumerator *e = SELF_ENUM;
    slots[0] = e->source; slots[1] = blk_proc;
    const uint32_t oldn = (SELF_ENUM->ops == KORB_NIL) ? 0 : VAL2ARY(SELF_ENUM->ops)->len;   /* raw generator → nil ops */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 3, oldn + 1));   /* clone ops */
    VALUE_REF nops = VALUE_REF_AT(&slots[2]);
    for (uint32_t i = 0; i < oldn; i++) CHECK(korb_ary_push_val(c, slots + 3, nops, VAL2ARY(SELF_ENUM->ops)->items->data[i]));
    /* new pair [op_sym, proc] */
    slots[3] = ID2SYM(korb_intern(c->vm, op, (uint32_t)strlen(op)));
    slots[4] = slots[1];                                          /* blk_proc */
    VALUE pair = UNWRAP(korb_ary_new(c, slots + 5, 2));
    slots[5] = pair;
    VALUE_REF pr = VALUE_REF_AT(&slots[5]);
    CHECK(korb_ary_push_val(c, slots + 6, pr, slots[3]));
    CHECK(korb_ary_push_val(c, slots + 6, pr, slots[4]));
    CHECK(korb_ary_push_val(c, slots + 6, nops, VALUE_REF_GET(pr)));
    KorbEnumerator *ne = korb_alloc(c, slots + 6, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    ne->mode = SELF_ENUM->mode;
    ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->source, slots[0]);
    ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->ops, VALUE_REF_GET(nops));
    return RESULT_OK((VALUE)ne);
}
/* call proc.call(v) — proc/v staged + rooted. */
static RESULT korb_call1(CTX *c, VALUE *slots, VALUE proc, VALUE v) {
    slots[0] = proc; slots[1] = v;
    return korb_send(c, slots + 2, korb_intern(c->vm, "call", 4), 0, 1);
}
/* Apply the lazy `ops` chain to slots[voff] (value; transformed in place by
 * map/filter_map).  slots[voff+1]=ops, slots[voff+2]=op_state (per-op Fixnum
 * counters, mutated).  *keep = value survives; *term = a take/take_while bound
 * hit (stop the drive).  ops/op_state are re-read from their slots after every
 * proc dispatch (GC-safe).  Uses slots[voff+3..] as scratch. */
static RESULT korb_lazy_apply(CTX *c, VALUE *slots, uint32_t voff, bool *keep, bool *term) {
    *keep = true; *term = false;
    const uint32_t n = VAL2ARY(slots[voff + 1])->len;
    for (uint32_t oi = 0; oi < n; oi++) {
        const KorbArray *pair = VAL2ARY(VAL2ARY(slots[voff + 1])->items->data[oi]);
        const char *const opn = korb_sym_name(c->vm, SYM2ID(pair->items->data[0]));
        if (!strcmp(opn, "drop")) {                             /* skip the first N */
            const intptr_t s = FIX2LONG(VAL2ARY(slots[voff + 2])->items->data[oi]);
            if (s > 0) { korb_ary_store_at(c, slots[voff + 2], oi, LONG2FIX(s - 1)); *keep = false; return RESULT_OK(KORB_NIL); }
            continue;
        }
        if (!strcmp(opn, "take")) {                             /* keep the first N, then terminate */
            const intptr_t s = FIX2LONG(VAL2ARY(slots[voff + 2])->items->data[oi]);
            if (s <= 0) { *keep = false; *term = true; return RESULT_OK(KORB_NIL); }
            korb_ary_store_at(c, slots[voff + 2], oi, LONG2FIX(s - 1));
            continue;
        }
        if (!strcmp(opn, "compact")) { if (slots[voff] == KORB_NIL) { *keep = false; return RESULT_OK(KORB_NIL); } continue; }
        /* block ops: proc.call(value) — slots[voff] held across the dispatch (rooted). */
        slots[voff + 3] = pair->items->data[1];                /* proc */
        RESULT cr = korb_call1(c, slots + voff + 4, slots[voff + 3], slots[voff]);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        const VALUE rv = cr.value;
        if (!strcmp(opn, "select") || !strcmp(opn, "filter")) { if (!KORB_TRUTHY(rv)) { *keep = false; return RESULT_OK(KORB_NIL); } }
        else if (!strcmp(opn, "reject")) { if (KORB_TRUTHY(rv)) { *keep = false; return RESULT_OK(KORB_NIL); } }
        else if (!strcmp(opn, "map") || !strcmp(opn, "collect")) { slots[voff] = rv; }
        else if (!strcmp(opn, "filter_map")) { if (!KORB_TRUTHY(rv)) { *keep = false; return RESULT_OK(KORB_NIL); } slots[voff] = rv; }
        else if (!strcmp(opn, "take_while")) { if (!KORB_TRUTHY(rv)) { *keep = false; *term = true; return RESULT_OK(KORB_NIL); } }
        else if (!strcmp(opn, "drop_while")) {
            if (FIX2LONG(VAL2ARY(slots[voff + 2])->items->data[oi])) {   /* still dropping */
                if (KORB_TRUTHY(rv)) { *keep = false; return RESULT_OK(KORB_NIL); }
                korb_ary_store_at(c, slots[voff + 2], oi, LONG2FIX(0));
            }
        }
    }
    return RESULT_OK(KORB_NIL);
}
/* Drive a lazy/cycle enum: produce values, apply ops, push up to `limit` (or all
 * if limit<0) into a fresh Array.  `self` rooted.  `sink` (non-null) yields each
 * instead of collecting (for lazy#each). */
static RESULT korb_enum_gen_run(CTX *c, VALUE *slots, VALUE_REF self, intptr_t limit);   /* fwd */
static RESULT korb_lazy_drive(CTX *c, VALUE *slots, VALUE_REF self, intptr_t limit) {
    if (SELF_ENUM->mode == 3 || SELF_ENUM->mode == 4) return korb_enum_gen_run(c, slots, self, limit);   /* (lazy) generator */
    slots[0] = UNWRAP(korb_ary_new(c, slots, limit > 0 ? (uint32_t)limit : 8));
    VALUE_REF res = VALUE_REF_AT(&slots[0]);                     /* result (rooted) */
    const uint8_t mode = SELF_ENUM->mode;
    /* per-op state for stateful ops: drop/take counter, drop_while "still dropping"
     * flag.  Indexed by op position; bounded so we can use a C-stack array. */
    intptr_t op_state[64];
    bool has_terminator = false;   /* a take/take_while op bounds an otherwise-infinite source */
    { const KorbArray *ops0 = VAL2ARY(SELF_ENUM->ops);
      uint32_t nop = ops0->len < 64 ? ops0->len : 64;
      for (uint32_t oi = 0; oi < nop; oi++) {
          const KorbArray *pair = VAL2ARY(ops0->items->data[oi]);
          const char *opn = korb_sym_name(c->vm, SYM2ID(pair->items->data[0]));
          if (!strcmp(opn, "drop") || !strcmp(opn, "take")) op_state[oi] = FIX2LONG(pair->items->data[1]);
          else if (!strcmp(opn, "drop_while")) op_state[oi] = 1;            /* 1 = still dropping */
          else op_state[oi] = 0;
          if (!strcmp(opn, "take") || !strcmp(opn, "take_while")) has_terminator = true;
      } }
    /* source enumeration index/value lives in slots[1] (the candidate value). */
    intptr_t produced = 0;
    /* helper: process one candidate value `cand` (in slots[1]); returns via
     * pushing to res; sets *stop on take_while termination or limit reached. */
    #define LAZY_FEED(cand_expr) do {                                                      \
        slots[1] = (cand_expr);                                                            \
        bool keep = true;                                                                  \
        for (uint32_t oi = 0; keep; oi++) {                                                \
            const KorbArray *ops = VAL2ARY(SELF_ENUM->ops);   /* re-read: op dispatch GCs */ \
            if (oi >= ops->len) break;                                                     \
            const KorbArray *pair = VAL2ARY(ops->items->data[oi]);                         \
            uint32_t opid = SYM2ID(pair->items->data[0]);                                  \
            const char *opn = korb_sym_name(c->vm, opid);                                  \
            if (!strcmp(opn, "drop")) { if (oi < 64 && op_state[oi] > 0) { op_state[oi]--; keep = false; } continue; } \
            if (!strcmp(opn, "take")) { if (oi >= 64 || op_state[oi] <= 0) { keep = false; goto lazy_done; } op_state[oi]--; continue; } \
            if (!strcmp(opn, "compact")) { if (slots[1] == KORB_NIL) keep = false; continue; }  /* no-block: drop nils */ \
            slots[2] = pair->items->data[1];                                               \
            RESULT cr = korb_call1(c, slots + 3, slots[2], slots[1]);                      \
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;                              \
            if (!strcmp(opn, "select") || !strcmp(opn, "filter")) { if (!KORB_TRUTHY(cr.value)) keep = false; } \
            else if (!strcmp(opn, "reject")) { if (KORB_TRUTHY(cr.value)) keep = false; }  \
            else if (!strcmp(opn, "map") || !strcmp(opn, "collect")) { slots[1] = cr.value; } \
            else if (!strcmp(opn, "filter_map")) { if (!KORB_TRUTHY(cr.value)) keep = false; else slots[1] = cr.value; } \
            else if (!strcmp(opn, "take_while")) { if (!KORB_TRUTHY(cr.value)) { keep = false; goto lazy_done; } } \
            else if (!strcmp(opn, "drop_while")) { if (oi < 64 && op_state[oi]) { if (KORB_TRUTHY(cr.value)) keep = false; else op_state[oi] = 0; } } \
        }                                                                                  \
        if (keep) { CHECK(korb_ary_push_val(c, slots + 3, res, slots[1])); produced++; if (limit >= 0 && produced >= limit) goto lazy_done; } \
    } while (0)

    if (mode == 2) {                                            /* cycle: repeat the array */
        const VALUE src = SELF_ENUM->source;
        if (!KORB_ARRAY_P(src) || VAL2ARY(src)->len == 0) goto lazy_done;
        if (limit < 0) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "cycle without a count");
        while (produced < limit) {
            uint32_t n = VAL2ARY(SELF_ENUM->source)->len;
            for (uint32_t i = 0; i < n && produced < limit; i++) LAZY_FEED(VAL2ARY(SELF_ENUM->source)->items->data[i]);
        }
    } else {                                                    /* lazy */
        const VALUE src = SELF_ENUM->source;
        if (KORB_RANGE_P(src)) {
            VALUE bv = VAL2RANGE(src)->rbegin, ev = VAL2RANGE(src)->rend;
            const bool excl = VAL2RANGE(src)->exclude_end != 0;
            if (!FIXNUM_P(bv)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "lazy over a non-integer range");
            intptr_t i = FIX2LONG(bv);
            const bool inf = (ev == KORB_NIL) || KORB_FLOAT_P(ev);   /* nil or Float::INFINITY → unbounded */
            intptr_t end = inf ? 0 : FIX2LONG(ev);
            for (;; i++) {
                if (!inf) { if (excl ? (i >= end) : (i > end)) break; }
                else if (limit < 0 && !has_terminator) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "lazy.force on an infinite range");
                LAZY_FEED(LONG2FIX(i));
            }
        } else if (KORB_ARRAY_P(src)) {
            uint32_t n = VAL2ARY(src)->len;
            for (uint32_t i = 0; i < n; i++) LAZY_FEED(VAL2ARY(SELF_ENUM->source)->items->data[i]);
        }
    }
  lazy_done:
    #undef LAZY_FEED
    return RESULT_OK(VALUE_REF_GET(res));
}

/* build the inspect desc "#<Enumerator: RECV:meth>" (no koruby alloc during print). */
static RESULT korb_enum_desc(CTX *c, VALUE *slots, VALUE recv, const char *meth) {
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (ms) { fputs("#<Enumerator: ", ms); korb_fprint_inspect(c, ms, recv); fprintf(ms, ":%s>", meth); fclose(ms); }
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_enum_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (SELF_ENUM->mode != 0) return korb_lazy_drive(c, slots, self, -1);   /* lazy: force (finite only) */
    return RESULT_OK(SELF_ENUM->values);
}
static RESULT korb_m_enum_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const KorbEnumerator *const e = SELF_ENUM;
    /* generator (mode 3) / lazy / cycle have no materialized `values`; CRuby
     * returns nil when the size is unknown (no size given to Enumerator.new). */
    if (e->mode != 0 || !KORB_ARRAY_P(e->values)) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX(VAL2ARY(e->values)->len));
}
static RESULT korb_m_enum_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    VALUE d = SELF_ENUM->desc;
    if (KORB_STRING_P(d)) return RESULT_OK(d);
    return korb_str_new(c, slots, "#<Enumerator>", 13);
}
/* each: yield every materialized value; with no block, return self. */
static RESULT korb_m_enum_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    if (SELF_ENUM->mode != 0) {                       /* lazy/cycle: force (finite), then yield each — no materialized `values` to read */
        RESULT vr = korb_lazy_drive(c, slots, self, -1);
        if (UNLIKELY(vr.state != KORB_NORMAL)) return vr;
        slots[0] = vr.value;
        VALUE_REF vals = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; ; i++) {
            const KorbArray *v = VAL2ARY(VALUE_REF_GET(vals));
            if (i >= v->len) break;
            slots[1] = v->items->data[i];
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
        return RESULT_OK(VALUE_REF_GET(self));
    }
    const uint8_t op = SELF_ENUM->op;
    if (op != 0) {                                    /* select/reject/flat_map enum: re-drive the op, collect results */
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
        for (uint32_t i = 0; ; i++) {
            const KorbArray *v = VAL2ARY(SELF_ENUM->values);
            if (i >= v->len) break;
            slots[0] = v->items->data[i];            /* slots advanced by SLOTS_PUSH; dst is below */
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (op == 3) {                            /* flat_map: flatten the block result one level */
                slots[1] = r.value;
                if (KORB_ARRAY_P(slots[1])) {
                    VALUE_REF fr = VALUE_REF_AT(&slots[1]);
                    for (uint32_t k = 0; k < VAL2ARY(VALUE_REF_GET(fr))->len; k++)
                        CHECK(korb_ary_push_val(c, slots + 2, dst, VAL2ARY(VALUE_REF_GET(fr))->items->data[k]));
                } else {
                    CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
                }
            }
            else if (op == 4) { if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[0]); }   /* find/detect: first match, early-stop */
            else if (KORB_TRUTHY(r.value) == (op == 1)) CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        }
        return (op == 4) ? RESULT_OK(KORB_NIL) : RESULT_OK(VALUE_REF_GET(dst));   /* find: nil if no match */
    }
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        slots[0] = v->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* map: collect block results over the materialized values; no block → self. */
static RESULT korb_lazy_gen_new(CTX *c, VALUE *slots, VALUE proc);   /* fwd */
/* x.lazy — a lazy enumerator over x (Array/Range), or self if already lazy. */
static RESULT korb_m_to_lazy(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE v = VALUE_REF_GET(self);
    if (KORB_ENUM_P(v)) {
        const uint8_t m = VAL2ENUM(v)->mode;
        if (m == 1 || m == 2 || m == 4) return RESULT_OK(v);    /* already lazy / cycle / lazy-generator */
        if (m == 3) { slots[0] = VAL2ENUM(v)->source; return korb_lazy_gen_new(c, slots + 1, slots[0]); }   /* generator → lazy generator */
    }
    if (KORB_HASH_P(v)) {                                        /* lazy over the [k,v] pairs (the driver iterates Arrays) */
        slots[0] = v;
        RESULT ta = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_a", 4), 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(ta.state != KORB_NORMAL)) return ta;
        slots[0] = ta.value;
        return korb_lazy_new(c, slots + 1, slots[0], 1);
    }
    slots[0] = (KORB_ENUM_P(v)) ? VAL2ENUM(v)->values : v;       /* eager enum → its values */
    return korb_lazy_new(c, slots + 1, slots[0], 1);
}
/* gen.lazy → a lazy generator (mode 4): the block proc as `source` + an empty
 * `ops` chain, driven incrementally (bounded) by the ops-aware yielder — so
 * `select`/`map`/... on an infinite generator stay deferred. */
static RESULT korb_lazy_gen_new(CTX *c, VALUE *slots, VALUE proc) {
    slots[0] = proc;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));           /* empty ops */
    KorbEnumerator *e = korb_alloc(c, slots + 2, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    e->mode = 4;
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->source, slots[0]);
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->ops, slots[1]);
    return RESULT_OK((VALUE)e);
}
/* For a plain (mode-3) generator: materialize it (finite only) into a mode-0
 * values enum in place, so the eager (mode-0) method paths work.  No-op for other
 * modes — a lazy generator (mode 4) defers instead. */
static RESULT korb_enum_force_gen(CTX *c, VALUE *slots, VALUE_REF self) {
    if (SELF_ENUM->mode != 3) return RESULT_OK(KORB_NIL);
    RESULT vr = korb_enum_gen_run(c, slots, self, -1);   /* finite materialize (infinite → hang, as expected) */
    if (UNLIKELY(vr.state != KORB_NORMAL)) return vr;
    slots[0] = vr.value;
    ARO_STORE(c, SELF_ENUM, (VALUE *)(uintptr_t)&SELF_ENUM->values, slots[0]);
    SELF_ENUM->mode = 0; SELF_ENUM->cursor = 0;
    return RESULT_OK(KORB_NIL);
}
/* A lazy chain op (select/map/reject/filter_map/take_while): on a lazy enum (1),
 * cycle (2) or lazy generator (4), reify the block to a Proc and append the op;
 * on a plain generator (3) materialize eagerly first, then collect like an eager
 * enum (0). */
static RESULT korb_lazy_op(CTX *c, VALUE *slots, VALUE_REF self, const char *op, bool is_map,
                           NODE *block, VALUE *def_env, VALUE *cself) {
    if (SELF_ENUM->mode == 3) { RESULT fr = korb_enum_force_gen(c, slots, self); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr; }
    if (SELF_ENUM->mode != 0) {                                  /* lazy(1)/cycle(2)/lazy-generator(4): defer (chain) */
        slots[0] = UNWRAP(korb_make_proc(c, slots, block, def_env, KORB_CSELF_VAL(cself), 0));
        return korb_lazy_chain(c, slots + 1, self, op, slots[0]);
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2ARY(SELF_ENUM->values)->len));   /* eager */
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        slots[1] = v->items->data[i];
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        const bool sel = (op[0] == 's' || op[0] == 'f');   /* select / filter / filter_map */
        if (is_map) CHECK(korb_ary_push_val(c, slots + 2, dst, r.value));
        else if (!strcmp(op, "reject")) { if (!KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1])); }
        else if (!strcmp(op, "filter_map")) { if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 2, dst, r.value)); }
        else if (sel) { if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1])); }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_enum_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_lazy_op(c, slots, self, "select", false, block, def_env, cself);
}
static RESULT korb_m_enum_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_lazy_op(c, slots, self, "reject", false, block, def_env, cself);
}
static RESULT korb_m_enum_filter_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_lazy_op(c, slots, self, "filter_map", false, block, def_env, cself);
}
static RESULT korb_m_enum_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_lazy_op(c, slots, self, "take_while", false, block, def_env, cself);
}
static RESULT korb_m_enum_compact(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (SELF_ENUM->mode == 0)                     /* only Enumerator::Lazy has #compact */
        return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method 'compact' for an instance of Enumerator");
    return korb_lazy_chain(c, slots, self, "compact", KORB_NIL);   /* no-block: drop nils */
}
static RESULT korb_m_enum_drop_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_lazy_op(c, slots, self, "drop_while", false, block, def_env, cself);
}
/* lazy drop(n) / take(n): chain a counted op (lazy); eager → slice the values. */
static RESULT korb_lazy_count_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, const char *op) {
    intptr_t n; if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (n < 0) n = 0;
    if (SELF_ENUM->mode == 3) {                          /* plain generator: take = bounded, drop = materialize */
        if (!strcmp(op, "take")) return korb_enum_gen_run(c, slots, self, n);
        RESULT fr = korb_enum_force_gen(c, slots, self); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
    }
    if (SELF_ENUM->mode == 2 && !strcmp(op, "take"))     /* cycle#take(n) is eager (only Lazy#take is deferred) */
        return korb_lazy_drive(c, slots, self, n);
    if (SELF_ENUM->mode != 0) { slots[0] = LONG2FIX(n); return korb_lazy_chain(c, slots + 1, self, op, slots[0]); }
    const bool is_take = !strcmp(op, "take");                    /* eager: slice values into a plain Array */
    const KorbArray *v = VAL2ARY(SELF_ENUM->values);
    const uint32_t len = v->len, lo = is_take ? 0 : (uint32_t)(n < (intptr_t)len ? n : len);
    const uint32_t hi = is_take ? (uint32_t)(n < (intptr_t)len ? n : len) : len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, hi - lo));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = lo; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, VAL2ARY(SELF_ENUM->values)->items->data[i]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_enum_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_lazy_count_op(c, slots, self, a, "drop"); }
static RESULT korb_m_enum_take_l(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_lazy_count_op(c, slots, self, a, "take"); }
/* Enumerator#first([n]) — lazy: drive up to n; eager: head of values. */
static RESULT korb_m_enum_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const bool has_n = VALUE_SLICE_LEN(a) >= 1;
    intptr_t n = 1;
    if (has_n && UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (SELF_ENUM->mode != 0) {
        RESULT d = korb_lazy_drive(c, slots, self, n < 0 ? 0 : n);
        if (UNLIKELY(d.state != KORB_NORMAL)) return d;
        if (!has_n) { const KorbArray *r = VAL2ARY(d.value); return RESULT_OK(r->len ? r->items->data[0] : KORB_NIL); }
        return d;
    }
    const KorbArray *vals = VAL2ARY(SELF_ENUM->values);
    if (!has_n) return RESULT_OK(vals->len ? vals->items->data[0] : KORB_NIL);
    uint32_t take = (n < 0) ? 0 : ((uint32_t)n < vals->len ? (uint32_t)n : vals->len);
    slots[0] = UNWRAP(korb_ary_new(c, slots, take));
    VALUE_REF res = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < take; i++) CHECK(korb_ary_push_val(c, slots + 1, res, VAL2ARY(SELF_ENUM->values)->items->data[i]));
    return RESULT_OK(VALUE_REF_GET(res));
}

static RESULT korb_m_enum_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (SELF_ENUM->mode != 0) return korb_lazy_op(c, slots, self, "map", true, block, def_env, cself);
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2ARY(SELF_ENUM->values)->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        slots[1] = v->items->data[i];
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 2, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* with_index(off=0): yield (value, off+i).  With a block → mapped array;
 * without → a new Enumerator of [value, off+i] pairs. */
static RESULT korb_m_enum_with_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    intptr_t off = 0;
    if (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) off = FIX2LONG(VALUE_SLICE_GET(a, 0));
    /* lazy/cycle enums carry no materialized `values`; force them first (finite). */
    if (SELF_ENUM->mode != 0) { RESULT vr = korb_lazy_drive(c, slots, self, -1); if (UNLIKELY(vr.state != KORB_NORMAL)) return vr; slots[0] = vr.value; }
    else slots[0] = SELF_ENUM->values;
    VALUE_REF vals = VALUE_REF_AT(&slots[0]);          /* materialized values (rooted) */
    const uint8_t opc = SELF_ENUM->op;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, VAL2ARY(VALUE_REF_GET(vals))->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(VALUE_REF_GET(vals));
        if (i >= v->len) break;
        slots[2] = v->items->data[i];
        if (block != NULL) {
            VALUE argv[2] = { slots[2], LONG2FIX(off + (intptr_t)i) };
            RESULT r = korb_block_yield(c, slots + 3, block, def_env, argv, 2, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (opc == 0) CHECK(korb_ary_push_val(c, slots + 3, dst, r.value));   /* map/each: collect block result */
            else if (opc == 3) {                                                 /* flat_map: flatten the block result one level */
                slots[3] = r.value;
                if (KORB_ARRAY_P(slots[3])) {
                    VALUE_REF fr = VALUE_REF_AT(&slots[3]);
                    for (uint32_t k = 0; k < VAL2ARY(VALUE_REF_GET(fr))->len; k++)
                        CHECK(korb_ary_push_val(c, slots + 4, dst, VAL2ARY(VALUE_REF_GET(fr))->items->data[k]));
                } else {
                    CHECK(korb_ary_push_val(c, slots + 4, dst, slots[3]));
                }
            }
            else if (opc == 4) { if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[2]); }   /* find/detect: return the first match, early-stop */
            else if (KORB_TRUTHY(r.value) == (opc == 1)) CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));   /* select/reject: keep value */
        } else {                                       /* build [value, idx] pair */
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), LONG2FIX(off + (intptr_t)i)));
            CHECK(korb_ary_push_val(c, slots + 4, dst, slots[3]));
        }
    }
    if (block != NULL && opc == 4) return RESULT_OK(KORB_NIL);   /* find with no match */
    if (block == NULL) return korb_enum_new(c, slots + 2, VALUE_REF_GET(dst), KORB_NIL);
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* with_object(o): yield (value, o) for each; return o. */
static RESULT korb_m_enum_with_object(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    if (UNLIKELY(block == NULL)) {                     /* no block → Enumerator over [elem, obj] pairs */
        slots[0] = VALUE_SLICE_GET(a, 0);             /* obj (rooted) */
        const uint32_t n = VAL2ARY(SELF_ENUM->values)->len;
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n));   /* pairs (rooted) */
        VALUE_REF dst = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < n; i++) {
            slots[2] = VAL2ARY(SELF_ENUM->values)->items->data[i];   /* elem (rooted) */
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));        /* [elem, obj] */
            VALUE_REF pr = VALUE_REF_AT(&slots[3]);
            CHECK(korb_ary_push_val(c, slots + 4, pr, slots[2]));
            CHECK(korb_ary_push_val(c, slots + 4, pr, slots[0]));
            CHECK(korb_ary_push_val(c, slots + 4, dst, VALUE_REF_GET(pr)));
        }
        slots[2] = UNWRAP(korb_enum_desc(c, slots + 2, VALUE_REF_GET(self), "with_object"));
        return korb_enum_new(c, slots + 3, VALUE_REF_GET(dst), slots[2]);
    }
    slots[0] = VALUE_SLICE_GET(a, 0);                  /* the memo object (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        VALUE argv[2] = { v->items->data[i], slots[0] };
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}
/* mode-3 generator next/peek: re-run bounded to cursor+1, return the value there. */
static RESULT korb_enum_gen_at_cursor(CTX *c, VALUE *slots, VALUE_REF self, bool advance) {
    const uint32_t cur = SELF_ENUM->cursor;
    RESULT vr = korb_enum_gen_run(c, slots, self, (intptr_t)cur + 1);
    if (UNLIKELY(vr.state != KORB_NORMAL)) return vr;
    slots[0] = vr.value;
    if (cur >= VAL2ARY(slots[0])->len) return korb_raise(c, slots, KORB_E_STOP_ITERATION, 0, "iteration reached an end");
    const VALUE v = VAL2ARY(slots[0])->items->data[cur];
    if (advance) SELF_ENUM->cursor = cur + 1;
    return RESULT_OK(v);
}
static RESULT korb_m_enum_next(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (SELF_ENUM->mode == 3 || SELF_ENUM->mode == 4) return korb_enum_gen_at_cursor(c, slots, self, true);
    KorbEnumerator *e = SELF_ENUM;
    const KorbArray *v = VAL2ARY(e->values);
    if (e->cursor >= v->len) return korb_raise(c, slots, KORB_E_STOP_ITERATION, 0, "iteration reached an end");
    return RESULT_OK(v->items->data[e->cursor++]);
}
static RESULT korb_m_enum_peek(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (SELF_ENUM->mode == 3 || SELF_ENUM->mode == 4) return korb_enum_gen_at_cursor(c, slots, self, false);
    const KorbEnumerator *e = SELF_ENUM;
    const KorbArray *v = VAL2ARY(e->values);
    if (e->cursor >= v->len) return korb_raise(c, slots, KORB_E_STOP_ITERATION, 0, "iteration reached an end");
    return RESULT_OK(v->items->data[e->cursor]);
}
static RESULT korb_m_enum_rewind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    SELF_ENUM->cursor = 0;
    return RESULT_OK(VALUE_REF_GET(self));
}
/* next_values / peek_values: the yielded value(s) as an Array.  A multi-value
 * yield is stored as an Array already (return it); a single value is wrapped. */
static RESULT korb_enum_values_at_cursor(CTX *c, VALUE *slots, VALUE_REF self, bool advance) {
    KorbEnumerator *const e = SELF_ENUM;
    const KorbArray *const v = VAL2ARY(e->values);
    if (e->cursor >= v->len) return korb_raise(c, slots, KORB_E_STOP_ITERATION, 0, "iteration reached an end");
    slots[0] = v->items->data[e->cursor];             /* the single stored value (park before alloc) */
    if (advance) e->cursor++;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));
    CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), slots[0]));
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_enum_next_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_enum_values_at_cursor(c, slots, self, true); }
static RESULT korb_m_enum_peek_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_enum_values_at_cursor(c, slots, self, false); }

