/* koruby_precise — enumerator.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* Streaming-each sink (see korb_vm::gen_sink): the yielder feeds each generated
 * value to `block` (evaluated in def_env/cself).  On a block-level break the
 * value is parked in *break_slot and `broke` is set so the driver returns it.
 * kind 0 = each (block for side effects); kind 1 = take_while (collect the
 * value into *collect while the block is truthy, stop at the first falsy). */
struct korb_gen_sink {
    NODE  *block;
    VALUE *def_env;
    VALUE *cself;
    VALUE *break_slot;   /* rooted slot on the driver's frame for the break value */
    VALUE *collect;      /* take_while: rooted slot holding the result Array */
    int    kind;
    bool   broke;
};
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
/* Shared body.  `want_value` distinguishes Yielder#yield (returns whatever the
 * consumer sent back — that becomes the source's `yield` value, which is what
 * Enumerator#feed sets) from Yielder#<< (returns self so `y << a << b` chains). */
static RESULT korb_m_yielder_push_impl(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool want_value);
static RESULT korb_m_yielder_push(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_m_yielder_push_impl(c, slots, self, a, false);
}
static RESULT korb_m_yielder_yield(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_m_yielder_push_impl(c, slots, self, a, true);
}
static RESULT korb_m_yielder_push_impl(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool want_value) {
    const uint32_t ac = VALUE_SLICE_LEN(a);
    const uint32_t csym = korb_intern(c->vm, "@__c", 4);   /* @__c = [collector, limit, ops, op_state] */
    slots[1] = korb_ivar_get(c, VALUE_REF_GET(self), csym);     /* the tuple (rooted) */
    slots[0] = korb_items_data(VAL2ARY(slots[1])->items)[0];              /* collector (rooted in slots[0]) */
    const VALUE limv = korb_items_data(VAL2ARY(slots[1])->items)[1];     /* limit (fixnum, immediate) */
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
    const VALUE ops = korb_items_data(VAL2ARY(slots[1])->items)[2];      /* nil for a raw generator */
    if (ops != KORB_NIL && VAL2ARY(ops)->len) {
        slots[3] = v;                                          /* value (transformed by map) */
        slots[4] = ops;                                        /* ops (rooted) */
        slots[5] = korb_items_data(VAL2ARY(slots[1])->items)[3];         /* op_state (rooted) */
        RESULT ar = korb_lazy_apply(c, slots, 3, &keep, &term);
        if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
        v = slots[3];                                         /* possibly mapped */
        slots[1] = korb_ivar_get(c, VALUE_REF_GET(self), csym);   /* re-read after op dispatch GC */
        slots[0] = korb_items_data(VAL2ARY(slots[1])->items)[0];
    }
    if (keep && FIXNUM_P(limv) && FIX2LONG(limv) == -2 && c->vm->gen_sink) {
        /* streaming-each sink: hand the value to the user block instead of
         * collecting.  A block break / StopIteration stops the whole drive. */
        struct korb_gen_sink *const sink = c->vm->gen_sink;
        slots[2] = v;                                         /* root across the yield */
        RESULT yr = korb_block_yield(c, slots + 3, sink->block, sink->def_env, &slots[2], 1, sink->cself);
        if (yr.state == KORB_BREAK && korb_break_owned(c, sink->block, sink->def_env)) { sink->broke = true; *sink->break_slot = yr.value;
            return korb_raise(c, slots + 1, KORB_E_STOP_ITERATION, 0, "iteration reached limit"); }
        if (UNLIKELY(yr.state != KORB_NORMAL)) return yr;     /* StopIteration / real error propagates */
        if (sink->kind == 1) {                                /* take_while: collect while truthy */
            if (!KORB_TRUTHY(yr.value))
                return korb_raise(c, slots + 1, KORB_E_STOP_ITERATION, 0, "iteration reached limit");
            slots[3] = slots[2];                              /* the value (block result was the predicate) */
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(sink->collect), slots[3]));
        }
        return RESULT_OK(want_value ? yr.value : VALUE_REF_GET(self));
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
static RESULT korb_enum_gen_new(CTX *c, VALUE *slots, VALUE proc, VALUE size) {
    slots[0] = proc; slots[1] = size;                    /* root both across alloc */
    KorbEnumerator *e = korb_alloc(c, slots + 2, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    slots[2] = (VALUE)e;                                 /* root the new enum across the respond_to? dispatch below */
    e->mode = 3;
    size = slots[1];                                     /* re-read after alloc */
    e->size = FIXNUM_P(size) ? size : KORB_NIL;          /* known Fixnum size → Enumerator#size directly */
    if (KORB_FLOAT_P(size) && isinf(korb_float_val(size)) && korb_float_val(size) > 0) e->size_inf = 1;   /* +Infinity size */
    else if (!FIXNUM_P(size) && size != KORB_NIL &&
             korb_responds_to_coerce(c, slots + 3, size, korb_intern(c->vm, "call", 4))) {   /* honors #respond_to?/mocks */
        e = VAL2ENUM(slots[2]); size = slots[1];         /* re-read enum + size after the (GC-capable) respond_to? dispatch */
        ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->size_proc, size);   /* a callable size → invoked lazily by #size */
    }
    e = VAL2ENUM(slots[2]);
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->source, slots[0]);
    return RESULT_OK((VALUE)e);
}
/* Run the generator proc, collecting up to `limit` values (limit < 0 = unbounded,
 * finite only).  Returns the collector Array. */
static RESULT korb_enum_gen_run(CTX *c, VALUE *slots, VALUE_REF self, korb_sword_t limit) {
    struct korb_vm *const vm = c->vm;
    if (vm->yielder_class == KORB_NIL) {                        /* lazily build Enumerator::Yielder (a GC root) */
        slots[0] = UNWRAP(korb_class_new(c, slots, 0, korb_builtin_class_obj(vm, KORB_C_OBJECT)));
        korb_class_def_cfn(c, slots[0], "yield", korb_m_yielder_yield, -1);
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
            const KorbArray *pair = VAL2ARY(korb_items_data(VAL2ARY(slots[0])->items)[i]);
            const char *const opn = korb_sym_name(vm, SYM2ID(korb_items_data(pair->items)[0]));
            if (!strcmp(opn, "uniq")) { slots[4] = UNWRAP(korb_hash_new(c, slots + 5, 8)); CHECK(korb_ary_push_val(c, slots + 5, st, slots[4])); continue; }   /* uniq: seen-Hash state */
            korb_sword_t init = 0;
            if (!strcmp(opn, "take") || !strcmp(opn, "drop")) init = FIX2LONG(korb_items_data(pair->items)[1]);
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
    RESULT br = korb_send_impl(c, slots + 7, korb_intern(vm, "call", 4), 0, 1, NULL, NULL, NULL);
    if (br.state == KORB_RAISE && KORB_EXC_P(br.value) && VAL2EXC(br.value)->etype == KORB_E_STOP_ITERATION)
        return RESULT_OK(slots[2]);                           /* hit the bound (or natural end) → collector */
    if (UNLIKELY(br.state != KORB_NORMAL && br.state != KORB_BREAK)) return br;
    /* Streaming each (limit -2) wants the generator block's OWN return value —
     * for a to_enum generator that is the underlying method's return (CRuby
     * Enumerator#each semantics).  Materializing callers want the collector. */
    if (limit == -2 && br.state == KORB_NORMAL) return RESULT_OK(br.value);
    return RESULT_OK(slots[2]);                               /* finished naturally (finite) */
}
/* Stream a (possibly infinite) generator's values straight to `block`, stopping
 * on a block break or StopIteration.  Uses the limit=-2 sink protocol so the
 * yielder feeds the block per value without materializing — this is what lets
 * `Enumerator.produce{…}.each{ … break }` / take_while / detect terminate. */
static RESULT korb_enum_gen_drive_block(CTX *c, VALUE *slots, VALUE_REF self, int kind,
                                        NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = KORB_NIL;                                      /* [0] = rooted break-value slot */
    slots[1] = (kind == 1) ? UNWRAP(korb_ary_new(c, slots + 1, 8)) : KORB_NIL;   /* [1] = take_while collector */
    struct korb_gen_sink sink = { block, def_env, cself, &slots[0], &slots[1], kind, false };
    struct korb_gen_sink *const prev = c->vm->gen_sink;       /* save (nesting) */
    c->vm->gen_sink = &sink;
    RESULT r = korb_enum_gen_run(c, slots + 2, self, -2);
    c->vm->gen_sink = prev;                                   /* restore */
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (kind == 1) return RESULT_OK(slots[1]);               /* take_while → the collected Array */
    return sink.broke ? RESULT_OK(slots[0]) : RESULT_OK(r.value);   /* natural end → the generator block's return value */
}
static RESULT korb_enum_gen_each_stream(CTX *c, VALUE *slots, VALUE_REF self,
                                        NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_enum_gen_drive_block(c, slots, self, 0, block, def_env, cself);
}
/* ---- lazy / cycle enumerators (deferred, possibly-infinite source) -------- */
/* A lazy enumerator carries a `source` (Array/Range) + a chain of deferred `ops`
 * (Array of [op_sym, proc] pairs) + a mode (1 lazy, 2 cycle).  Terminal methods
 * (first/take/force/to_a/each) drive the source, applying ops, bounded by a
 * limit (required for an infinite source). */
static RESULT korb_lazy_new(CTX *c, VALUE *slots, VALUE source, uint8_t mode) {
    slots[0] = source;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));            /* empty ops */
    /* source size (Enumerator#size): Array → len; finite integer Range → count;
     * else unknown (nil).  Only immediate (Fixnum) sizes are stored — e->size is
     * not a GC-scanned field, so a heap-boxed Float (an endless range's Infinity)
     * would go stale; those report nil for now. */
    slots[2] = KORB_NIL;
    bool src_inf = false;
    if (KORB_ARRAY_P(slots[0])) slots[2] = LONG2FIX(VAL2ARY(slots[0])->len);
    else if (KORB_RANGE_P(slots[0])) {
        const KorbRange *r = VAL2RANGE(slots[0]);
        if (FIXNUM_P(r->rbegin) && FIXNUM_P(r->rend)) {
            korb_sword_t n = FIX2LONG(r->rend) - FIX2LONG(r->rbegin) + (r->exclude_end ? 0 : 1);
            slots[2] = LONG2FIX(n < 0 ? 0 : n);
        } else if (KORB_INTEGER_P(r->rbegin) &&                 /* endless / +Infinity integer range → #size is Infinity */
                   (r->rend == KORB_NIL || (KORB_FLOAT_P(r->rend) && isinf(korb_float_val(r->rend)) && korb_float_val(r->rend) > 0))) {
            src_inf = true;
        }
    }
    KorbEnumerator *e = korb_alloc(c, slots + 3, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    e->mode = mode;
    e->size_inf = src_inf;
    e->size = slots[2];                                         /* re-read after alloc */
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->source, slots[0]);
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->ops, slots[1]);
    return RESULT_OK((VALUE)e);
}
/* Size of a chained lazy enum: map/collect preserve it, take(n)=min(size,n),
 * drop(n)=max(0,size-n), everything else is unknown (nil).  blk_proc carries the
 * count for take/drop. */
static VALUE korb_lazy_size(const char *op, VALUE osz, bool src_inf, VALUE blk_proc, bool *dst_inf) {
    const bool inf = src_inf || (KORB_FLOAT_P(osz) && isinf(korb_float_val(osz)));
    *dst_inf = false;
    if (!strcmp(op, "take")) { if (inf) return blk_proc; if (!FIXNUM_P(osz)) return KORB_NIL; korb_sword_t n = FIX2LONG(blk_proc), s = FIX2LONG(osz); return LONG2FIX(n < s ? n : s); }
    if (!strcmp(op, "map") || !strcmp(op, "collect") || !strcmp(op, "with_index")) { *dst_inf = inf; return osz; }   /* size-preserving */
    if (!strcmp(op, "drop")) { if (inf) { *dst_inf = true; return KORB_NIL; } if (!FIXNUM_P(osz)) return KORB_NIL; korb_sword_t n = FIX2LONG(blk_proc), s = FIX2LONG(osz); return LONG2FIX(s > n ? s - n : 0); }
    return KORB_NIL;   /* filtering ops (select/reject/grep/…) make the size unknowable */
}
/* Return a new lazy enum = self with one more op appended (op_sym, blk_proc). */
static RESULT korb_lazy_chain(CTX *c, VALUE *slots, VALUE_REF self, const char *op, VALUE blk_proc) {
    const KorbEnumerator *e = SELF_ENUM;
    slots[0] = e->source; slots[1] = blk_proc;
    const uint32_t oldn = (SELF_ENUM->ops == KORB_NIL) ? 0 : VAL2ARY(SELF_ENUM->ops)->len;   /* raw generator → nil ops */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 3, oldn + 1));   /* clone ops */
    VALUE_REF nops = VALUE_REF_AT(&slots[2]);
    for (uint32_t i = 0; i < oldn; i++) CHECK(korb_ary_push_val(c, slots + 3, nops, korb_items_data(VAL2ARY(SELF_ENUM->ops)->items)[i]));
    /* new pair [op_sym, proc] */
    slots[3] = ID2SYM(korb_intern(c->vm, op, (uint32_t)strlen(op)));
    slots[4] = slots[1];                                          /* blk_proc */
    VALUE pair = UNWRAP(korb_ary_new(c, slots + 5, 2));
    slots[5] = pair;
    VALUE_REF pr = VALUE_REF_AT(&slots[5]);
    CHECK(korb_ary_push_val(c, slots + 6, pr, slots[3]));
    CHECK(korb_ary_push_val(c, slots + 6, pr, slots[4]));
    CHECK(korb_ary_push_val(c, slots + 6, nops, VALUE_REF_GET(pr)));
    bool dst_inf;
    const VALUE nsz = korb_lazy_size(op, SELF_ENUM->size, SELF_ENUM->size_inf, slots[4], &dst_inf);   /* slots[4] = blk_proc (count for take/drop) */
    KorbEnumerator *ne = korb_alloc(c, slots + 6, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    ne->mode = SELF_ENUM->mode;
    ne->size_inf = dst_inf;
    ne->size = nsz;
    ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->source, slots[0]);
    ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->ops, VALUE_REF_GET(nops));
    return RESULT_OK((VALUE)ne);
}
/* Like korb_lazy_chain but the op pair carries two payload values (e.g. grep's
 * [op_sym, pattern, block_proc]) instead of a single proc. */
static RESULT korb_lazy_chain2(CTX *c, VALUE *slots, VALUE_REF self, const char *op, VALUE v1, VALUE v2) {
    const KorbEnumerator *e = SELF_ENUM;
    slots[0] = e->source; slots[1] = v1; slots[2] = v2;
    const uint32_t oldn = (SELF_ENUM->ops == KORB_NIL) ? 0 : VAL2ARY(SELF_ENUM->ops)->len;
    slots[3] = UNWRAP(korb_ary_new(c, slots + 4, oldn + 1));   /* clone ops */
    VALUE_REF nops = VALUE_REF_AT(&slots[3]);
    for (uint32_t i = 0; i < oldn; i++) CHECK(korb_ary_push_val(c, slots + 4, nops, korb_items_data(VAL2ARY(SELF_ENUM->ops)->items)[i]));
    slots[4] = ID2SYM(korb_intern(c->vm, op, (uint32_t)strlen(op)));   /* op_sym */
    VALUE pair = UNWRAP(korb_ary_new(c, slots + 5, 3));
    slots[5] = pair;
    VALUE_REF pr = VALUE_REF_AT(&slots[5]);
    CHECK(korb_ary_push_val(c, slots + 6, pr, slots[4]));      /* op_sym */
    CHECK(korb_ary_push_val(c, slots + 6, pr, slots[1]));      /* pattern */
    CHECK(korb_ary_push_val(c, slots + 6, pr, slots[2]));      /* block proc (or nil) */
    CHECK(korb_ary_push_val(c, slots + 6, nops, VALUE_REF_GET(pr)));
    KorbEnumerator *ne = korb_alloc(c, slots + 6, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    ne->mode = SELF_ENUM->mode;
    ne->size = KORB_NIL;                                          /* grep/grep_v: size unknown */
    ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->source, slots[0]);
    ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->ops, VALUE_REF_GET(nops));
    return RESULT_OK((VALUE)ne);
}
/* pattern === val via a real dispatch (Regexp/Class/Range/Proc/user #===), so a
 * grep with a Proc or custom pattern behaves correctly (korb_case_eq can't call
 * a Proc).  pat/val staged + rooted; *m = truthiness of the result. */
static RESULT korb_grep_eqq(CTX *c, VALUE *slots, VALUE pat, VALUE val, bool *m) {
    slots[0] = pat; slots[1] = val;
    RESULT r = korb_send(c, slots + 2, c->vm->mid_eqq, 0, 1);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    *m = KORB_TRUTHY(r.value);
    return RESULT_OK(KORB_NIL);
}
/* call proc.call(v) — proc/v staged + rooted. */
static RESULT korb_call1(CTX *c, VALUE *slots, VALUE proc, VALUE v) {
    slots[0] = proc; slots[1] = v;
    return korb_send(c, slots + 2, korb_intern(c->vm, "call", 4), 0, 1);
}
/* call proc.call(v1, v2) — proc + 2 args staged + rooted. */
static RESULT korb_call2(CTX *c, VALUE *slots, VALUE proc, VALUE v1, VALUE v2) {
    slots[0] = proc; slots[1] = v1; slots[2] = v2;
    return korb_send(c, slots + 3, korb_intern(c->vm, "call", 4), 0, 2);
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
        const KorbArray *pair = VAL2ARY(korb_items_data(VAL2ARY(slots[voff + 1])->items)[oi]);
        const char *const opn = korb_sym_name(c->vm, SYM2ID(korb_items_data(pair->items)[0]));
        if (!strcmp(opn, "drop")) {                             /* skip the first N */
            const korb_sword_t s = FIX2LONG(korb_items_data(VAL2ARY(slots[voff + 2])->items)[oi]);
            if (s > 0) { korb_ary_store_at(c, slots[voff + 2], oi, LONG2FIX(s - 1)); *keep = false; return RESULT_OK(KORB_NIL); }
            continue;
        }
        if (!strcmp(opn, "take")) {                             /* keep the first N, then terminate */
            const korb_sword_t s = FIX2LONG(korb_items_data(VAL2ARY(slots[voff + 2])->items)[oi]);
            if (s <= 0) { *keep = false; *term = true; return RESULT_OK(KORB_NIL); }
            korb_ary_store_at(c, slots[voff + 2], oi, LONG2FIX(s - 1));
            continue;
        }
        if (!strcmp(opn, "compact")) { if (slots[voff] == KORB_NIL) { *keep = false; return RESULT_OK(KORB_NIL); } continue; }
        if (!strcmp(opn, "grep") || !strcmp(opn, "grep_v")) {   /* pattern === value (grep_v: NOT), optional block maps the match */
            const bool want = (opn[4] == '\0');                 /* "grep" wants a match; "grep_v" wants none */
            bool m; RESULT mr = korb_grep_eqq(c, slots + voff + 3, korb_items_data(pair->items)[1], slots[voff], &m);
            if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
            if (m != want) { *keep = false; return RESULT_OK(KORB_NIL); }
            const KorbArray *pair2 = VAL2ARY(korb_items_data(VAL2ARY(slots[voff + 1])->items)[oi]);   /* re-read: === may have GC'd */
            if (pair2->len >= 3 && korb_items_data(pair2->items)[2] != KORB_NIL) {   /* block: transform the kept value */
                slots[voff + 3] = korb_items_data(pair2->items)[2];        /* proc (rooted before the dispatch) */
                RESULT cr = korb_call1(c, slots + voff + 4, slots[voff + 3], slots[voff]);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                slots[voff] = cr.value;
            }
            continue;
        }
        if (!strcmp(opn, "uniq")) {                             /* dedup by value (or block result); op_state[oi] holds a seen-Hash */
            slots[voff + 3] = slots[voff];                      /* key = value */
            if (korb_items_data(pair->items)[1] != KORB_NIL) {             /* uniq(&block): key = block.call(value) */
                slots[voff + 4] = korb_items_data(pair->items)[1];
                RESULT ur = korb_call1(c, slots + voff + 5, slots[voff + 4], slots[voff]);
                if (UNLIKELY(ur.state != KORB_NORMAL)) return ur;
                slots[voff + 3] = ur.value;
            }
            slots[voff + 4] = korb_items_data(VAL2ARY(slots[voff + 2])->items)[oi];   /* this op's seen-Hash */
            if (korb_hash_find(VAL2HASH(slots[voff + 4]), slots[voff + 3]) >= 0) { *keep = false; return RESULT_OK(KORB_NIL); }
            CHECK(korb_hash_set(c, slots + voff + 5, VALUE_REF_AT(&slots[voff + 4]), VALUE_REF_AT(&slots[voff + 3]), KORB_TRUE));
            continue;
        }
        /* block ops: proc.call(value) — slots[voff] held across the dispatch (rooted). */
        slots[voff + 3] = korb_items_data(pair->items)[1];                /* proc */
        RESULT cr = korb_call1(c, slots + voff + 4, slots[voff + 3], slots[voff]);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        const VALUE rv = cr.value;
        if (!strcmp(opn, "select") || !strcmp(opn, "filter")) { if (!KORB_TRUTHY(rv)) { *keep = false; return RESULT_OK(KORB_NIL); } }
        else if (!strcmp(opn, "reject")) { if (KORB_TRUTHY(rv)) { *keep = false; return RESULT_OK(KORB_NIL); } }
        else if (!strcmp(opn, "map") || !strcmp(opn, "collect")) { slots[voff] = rv; }
        else if (!strcmp(opn, "filter_map")) { if (!KORB_TRUTHY(rv)) { *keep = false; return RESULT_OK(KORB_NIL); } slots[voff] = rv; }
        else if (!strcmp(opn, "take_while")) { if (!KORB_TRUTHY(rv)) { *keep = false; *term = true; return RESULT_OK(KORB_NIL); } }
        else if (!strcmp(opn, "drop_while")) {
            if (FIX2LONG(korb_items_data(VAL2ARY(slots[voff + 2])->items)[oi])) {   /* still dropping */
                if (KORB_TRUTHY(rv)) { *keep = false; return RESULT_OK(KORB_NIL); }
                korb_ary_store_at(c, slots[voff + 2], oi, LONG2FIX(0));
            }
        }
    }
    return RESULT_OK(KORB_NIL);
}
/* Feed one candidate `value` through the source enum's op chain starting at
 * `start_oi`, pushing surviving values to `res` (bounded by `limit`).  flat_map
 * fans out: each element of an Array block-result re-enters ops[oi+1..] via a
 * recursive call — the only op that produces 0..N outputs per input.  Shared
 * state: op_state[] (take/drop counters), `seen` (per-op uniq Hash array).
 * slots[0] holds the current value; slots[1+] is op scratch. */
/* When a lazy chain is iterated with a block (Enumerator::Lazy#each) the values
 * are streamed to that block instead of being collected — that is what lets an
 * infinite source terminate on `break`. */
struct korb_lazy_sink { NODE *block; VALUE *def_env; VALUE *cself; RESULT out; bool broke; };

static RESULT korb_lazy_run(CTX *c, VALUE *slots, VALUE_REF self, VALUE value, uint32_t start_oi,
                            korb_sword_t *op_state, VALUE_REF seen, VALUE_REF res, korb_sword_t limit,
                            korb_sword_t *produced, bool *term, struct korb_lazy_sink *sink) {
    char cstack_probe;
    if (UNLIKELY(&cstack_probe < c->cstack_limit)) return korb_raise(c, slots, KORB_E_SYSSTACK, 0, "stack level too deep");
    slots[0] = value;                                          /* candidate (rooted) */
    for (uint32_t oi = start_oi; ; oi++) {
        const KorbArray *ops = VAL2ARY(SELF_ENUM->ops);        /* re-read: op dispatch GCs */
        if (oi >= ops->len) break;
        const KorbArray *pair = VAL2ARY(korb_items_data(ops->items)[oi]);
        const char *const opn = korb_sym_name(c->vm, SYM2ID(korb_items_data(pair->items)[0]));
        if (!strcmp(opn, "drop")) { if (oi < 64 && op_state[oi] > 0) { op_state[oi]--; return RESULT_OK(KORB_NIL); } continue; }
        if (!strcmp(opn, "take")) { if (oi >= 64 || op_state[oi] <= 0) { *term = true; return RESULT_OK(KORB_NIL); } op_state[oi]--; continue; }
        if (!strcmp(opn, "compact")) { if (slots[0] == KORB_NIL) return RESULT_OK(KORB_NIL); continue; }
        if (!strcmp(opn, "grep") || !strcmp(opn, "grep_v")) {
            const bool gwant = (opn[4] == '\0');
            bool gm; RESULT gmr = korb_grep_eqq(c, slots + 1, korb_items_data(pair->items)[1], slots[0], &gm);
            if (UNLIKELY(gmr.state != KORB_NORMAL)) return gmr;
            if (gm != gwant) return RESULT_OK(KORB_NIL);
            const KorbArray *gpair = VAL2ARY(korb_items_data(VAL2ARY(SELF_ENUM->ops)->items)[oi]);   /* re-read after GC */
            if (gpair->len >= 3 && korb_items_data(gpair->items)[2] != KORB_NIL) {
                slots[1] = korb_items_data(gpair->items)[2];
                RESULT gr = korb_call1(c, slots + 2, slots[1], slots[0]);
                if (UNLIKELY(gr.state != KORB_NORMAL)) return gr;
                slots[0] = gr.value;
            }
            continue;
        }
        if (!strcmp(opn, "uniq")) {
            slots[1] = slots[0];                               /* dedup key */
            if (korb_items_data(pair->items)[1] != KORB_NIL) {
                slots[2] = korb_items_data(pair->items)[1];
                RESULT ur = korb_call1(c, slots + 3, slots[2], slots[0]);
                if (UNLIKELY(ur.state != KORB_NORMAL)) return ur;
                slots[1] = ur.value;
            }
            slots[2] = korb_items_data(VAL2ARY(VALUE_REF_GET(seen))->items)[oi];
            if (korb_hash_find(VAL2HASH(slots[2]), slots[1]) >= 0) return RESULT_OK(KORB_NIL);
            CHECK(korb_hash_set(c, slots + 3, VALUE_REF_AT(&slots[2]), VALUE_REF_AT(&slots[1]), KORB_TRUE));
            continue;
        }
        if (!strcmp(opn, "with_index")) {                     /* value → [value, idx] (no block) or block(value, idx) */
            const korb_sword_t idx = (oi < 64) ? op_state[oi]++ : 0;
            const VALUE blk = (pair->len >= 3) ? korb_items_data(pair->items)[2] : KORB_NIL;
            if (blk != KORB_NIL) {
                slots[1] = blk;
                RESULT wr = korb_call2(c, slots + 2, slots[1], slots[0], LONG2FIX(idx));
                if (UNLIKELY(wr.state != KORB_NORMAL)) return wr;
                slots[0] = wr.value;
            } else {
                slots[1] = LONG2FIX(idx);
                VALUE p = UNWRAP(korb_ary_new(c, slots + 2, 2));   /* [value, idx] */
                slots[2] = p;
                CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
                CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
                slots[0] = slots[2];
            }
            continue;
        }
        if (!strcmp(opn, "flat_map") || !strcmp(opn, "collect_concat")) {
            slots[1] = korb_items_data(pair->items)[1];                  /* proc */
            RESULT fr = korb_call1(c, slots + 2, slots[1], slots[0]);
            if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
            slots[1] = fr.value;                              /* block result (rooted) */
            if (KORB_ARRAY_P(slots[1])) {                     /* Array → flatten one level: each elem re-enters ops[oi+1..] */
                for (uint32_t k = 0; !*term && k < VAL2ARY(slots[1])->len; k++) {   /* re-read len/elem each iter (recursion GCs) */
                    RESULT r = korb_lazy_run(c, slots + 2, self, korb_items_data(VAL2ARY(slots[1])->items)[k], oi + 1, op_state, seen, res, limit, produced, term, sink);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                }
                return RESULT_OK(KORB_NIL);                   /* fanout handled here; don't push slots[0] */
            }
            slots[0] = slots[1];                              /* non-Array → a single value; continue downstream */
            continue;
        }
        /* block ops (select/reject/map/filter_map/take_while/drop_while) */
        slots[1] = korb_items_data(pair->items)[1];
        RESULT cr = korb_call1(c, slots + 2, slots[1], slots[0]);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        const VALUE rv = cr.value;
        if (!strcmp(opn, "select") || !strcmp(opn, "filter")) { if (!KORB_TRUTHY(rv)) return RESULT_OK(KORB_NIL); }
        else if (!strcmp(opn, "reject")) { if (KORB_TRUTHY(rv)) return RESULT_OK(KORB_NIL); }
        else if (!strcmp(opn, "map") || !strcmp(opn, "collect")) { slots[0] = rv; }
        else if (!strcmp(opn, "filter_map")) { if (!KORB_TRUTHY(rv)) return RESULT_OK(KORB_NIL); slots[0] = rv; }
        else if (!strcmp(opn, "take_while")) { if (!KORB_TRUTHY(rv)) { *term = true; return RESULT_OK(KORB_NIL); } }
        else if (!strcmp(opn, "drop_while")) { if (oi < 64 && op_state[oi]) { if (KORB_TRUTHY(rv)) return RESULT_OK(KORB_NIL); op_state[oi] = 0; } }
    }
    if (sink) {                                              /* stream to the block */
        slots[1] = slots[0];
        RESULT br = korb_block_yield(c, slots + 2, sink->block, sink->def_env, &slots[1], 1, sink->cself);
        if (UNLIKELY(br.state != KORB_NORMAL)) { sink->out = br; sink->broke = true; *term = true; return RESULT_OK(KORB_NIL); }
    } else {
        CHECK(korb_ary_push_val(c, slots + 1, res, slots[0]));    /* survived all ops → keep */
    }
    (*produced)++;
    if (limit >= 0 && *produced >= limit) *term = true;
    return RESULT_OK(KORB_NIL);
}
/* Drive a lazy/cycle enum: produce values, apply ops, push up to `limit` (or all
 * if limit<0) into a fresh Array.  `self` rooted.  `sink` (non-null) yields each
 * instead of collecting (for lazy#each). */
static RESULT korb_enum_gen_run(CTX *c, VALUE *slots, VALUE_REF self, korb_sword_t limit);   /* fwd */
static RESULT korb_lazy_drive_sink(CTX *c, VALUE *slots, VALUE_REF self, korb_sword_t limit, struct korb_lazy_sink *sink) {
    if (SELF_ENUM->mode == 3 || SELF_ENUM->mode == 4) return korb_enum_gen_run(c, slots, self, limit);   /* (lazy) generator */
    slots[0] = UNWRAP(korb_ary_new(c, slots, limit > 0 ? (uint32_t)limit : 8));
    VALUE_REF res = VALUE_REF_AT(&slots[0]);                     /* result (rooted) */
    const uint8_t mode = SELF_ENUM->mode;
    /* per-op state for stateful ops: drop/take counter, drop_while "still dropping"
     * flag.  Indexed by op position; bounded so we can use a C-stack array. */
    korb_sword_t op_state[64];
    bool has_terminator = false;   /* a take/take_while op bounds an otherwise-infinite source */
    /* slots[1] = a per-op seen-hash array for `uniq` (a Hash at each uniq op's
     * index, nil elsewhere) — rooted below the working cursor.  The candidate
     * value lives in slots[2]; op scratch is slots[3+]. */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 2, 0));
    VALUE_REF seen = VALUE_REF_AT(&slots[1]);
    { const KorbArray *ops0 = VAL2ARY(SELF_ENUM->ops);
      uint32_t nop = ops0->len < 64 ? ops0->len : 64;
      for (uint32_t oi = 0; oi < nop; oi++) {
          const KorbArray *pair = VAL2ARY(korb_items_data(VAL2ARY(SELF_ENUM->ops)->items)[oi]);   /* re-read: hash_new GCs */
          const char *opn = korb_sym_name(c->vm, SYM2ID(korb_items_data(pair->items)[0]));
          if (!strcmp(opn, "drop") || !strcmp(opn, "take")) op_state[oi] = FIX2LONG(korb_items_data(pair->items)[1]);
          else if (!strcmp(opn, "with_index")) op_state[oi] = FIX2LONG(korb_items_data(pair->items)[1]);   /* running index (starts at the offset) */
          else if (!strcmp(opn, "drop_while")) op_state[oi] = 1;            /* 1 = still dropping */
          else op_state[oi] = 0;
          if (!strcmp(opn, "take") || !strcmp(opn, "take_while")) has_terminator = true;
          if (!strcmp(opn, "uniq")) { slots[2] = UNWRAP(korb_hash_new(c, slots + 3, 8)); CHECK(korb_ary_push_val(c, slots + 3, seen, slots[2])); }
          else CHECK(korb_ary_push_val(c, slots + 3, seen, KORB_NIL));
      } }
    korb_sword_t produced = 0;
    bool term = false;   /* a take/take_while bound (or the limit) was hit → stop the source */
    /* Feed one source value through the op chain; flat_map fanout + all state is
     * handled by korb_lazy_run.  Candidate + op scratch live at slots[2+]. */
    #define LAZY_FEED(cand_expr) do {                                                      \
        RESULT _r = korb_lazy_run(c, slots + 2, self, (cand_expr), 0, op_state, seen, res, limit, &produced, &term, sink); \
        if (UNLIKELY(_r.state != KORB_NORMAL)) return _r;                                  \
        if (term) goto lazy_done;                                                          \
    } while (0)

    if (mode == 2) {                                            /* cycle: repeat the array */
        const VALUE src = SELF_ENUM->source;
        if (!KORB_ARRAY_P(src) || VAL2ARY(src)->len == 0) goto lazy_done;
        if (limit < 0) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "cycle without a count");
        while (produced < limit) {
            uint32_t n = VAL2ARY(SELF_ENUM->source)->len;
            for (uint32_t i = 0; i < n && produced < limit; i++) LAZY_FEED(korb_items_data(VAL2ARY(SELF_ENUM->source)->items)[i]);
        }
    } else {                                                    /* lazy */
        const VALUE src = SELF_ENUM->source;
        if (KORB_RANGE_P(src)) {
            VALUE bv = VAL2RANGE(src)->rbegin, ev = VAL2RANGE(src)->rend;
            const bool excl = VAL2RANGE(src)->exclude_end != 0;
            if (!FIXNUM_P(bv)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "lazy over a non-integer range");
            korb_sword_t i = FIX2LONG(bv);
            const bool inf = (ev == KORB_NIL) || KORB_FLOAT_P(ev);   /* nil or Float::INFINITY → unbounded */
            korb_sword_t end = inf ? 0 : FIX2LONG(ev);
            for (;; i++) {
                if (!inf) { if (excl ? (i >= end) : (i > end)) break; }
                else if (limit < 0 && !has_terminator && sink == NULL) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "lazy.force on an infinite range");
                LAZY_FEED(LONG2FIX(i));
            }
        } else if (KORB_ARRAY_P(src)) {
            uint32_t n = VAL2ARY(src)->len;
            for (uint32_t i = 0; i < n; i++) LAZY_FEED(korb_items_data(VAL2ARY(SELF_ENUM->source)->items)[i]);
        }
    }
  lazy_done:
    #undef LAZY_FEED
    if (sink && sink->broke) return sink->out;               /* the block broke / raised */
    return RESULT_OK(VALUE_REF_GET(res));
}
static RESULT korb_lazy_drive(CTX *c, VALUE *slots, VALUE_REF self, korb_sword_t limit) {
    return korb_lazy_drive_sink(c, slots, self, limit, NULL);
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
    if (e->size_inf) return korb_float_new(c, slots, (double)INFINITY);          /* infinite source (endless/Float::INFINITY range) */
    if (e->size_proc != KORB_NIL) {                                             /* a callable size — call it fresh each time */
        slots[0] = e->size_proc;
        return korb_send(c, slots + 1, korb_intern(c->vm, "call", 4), 0, 0);
    }
    if (FIXNUM_P(e->size) || KORB_FLOAT_P(e->size)) return RESULT_OK(e->size);   /* explicit size (Enumerator.new(size)/to_enum) or Infinity */
    /* generator (mode 3) / lazy / cycle have no materialized `values`; CRuby
     * returns nil when the size is unknown (no size given to Enumerator.new). */
    if (e->mode != 0 || e->size_unknown || !KORB_ARRAY_P(e->values)) return RESULT_OK(KORB_NIL);
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
    if (SELF_ENUM->mode == 3 || SELF_ENUM->mode == 4) {  /* generator: stream (break/StopIteration safe on infinite sources) */
        RESULT r = korb_enum_gen_each_stream(c, slots, self, block, def_env, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (SELF_ENUM->mode == 4) return RESULT_OK(VALUE_REF_GET(self));   /* Enumerator::Lazy#each returns the lazy enum */
        return r;                                         /* plain generator: the source method's return value */
    }
    if (SELF_ENUM->mode != 0) {                       /* lazy/cycle: stream each value to the block */
        struct korb_lazy_sink sink = { block, def_env, cself, RESULT_OK(KORB_NIL), false };
        RESULT vr = korb_lazy_drive_sink(c, slots, self, -1, &sink);
        if (UNLIKELY(vr.state != KORB_NORMAL)) return vr;
        return RESULT_OK(VALUE_REF_GET(self));
    }
    const uint8_t op = SELF_ENUM->op;
    if (op != 0) {                                    /* select/reject/flat_map enum: re-drive the op, collect results */
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
        for (uint32_t i = 0; ; i++) {
            const KorbArray *v = VAL2ARY(SELF_ENUM->values);
            if (i >= v->len) break;
            slots[0] = korb_items_data(v->items)[i];            /* slots advanced by SLOTS_PUSH; dst is below */
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (op == 3) {                            /* flat_map: flatten the block result one level */
                slots[1] = r.value;
                if (KORB_ARRAY_P(slots[1])) {
                    VALUE_REF fr = VALUE_REF_AT(&slots[1]);
                    for (uint32_t k = 0; k < VAL2ARY(VALUE_REF_GET(fr))->len; k++)
                        CHECK(korb_ary_push_val(c, slots + 2, dst, korb_items_data(VAL2ARY(VALUE_REF_GET(fr))->items)[k]));
                } else {
                    CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
                }
            }
            else if (op == 4) { if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[0]); }   /* find/detect: first match, early-stop */
            else if (KORB_TRUTHY(r.value) == (op == 1)) CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        }
        return (op == 4) ? RESULT_OK(KORB_NIL) : RESULT_OK(VALUE_REF_GET(dst));   /* find: nil if no match */
    }
    if (!KORB_ARRAY_P(SELF_ENUM->values)) return RESULT_OK(VALUE_REF_GET(self));   /* allocate'd but uninitialized → empty */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        slots[0] = korb_items_data(v->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Enumerator#initialize([size]) { |yielder| ... } — set up a deferred generator
 * on an allocate'd enumerator.  koruby's Enumerator.new is special-cased in
 * korb_send_impl, so this is reached via `allocate` + send(:initialize) or a
 * subclass's generic .new.  (The size argument is accepted but not stored.) */
static RESULT korb_m_enum_initialize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    const VALUE ev = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_ENUM_P(ev))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not an enumerator");
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to create Enumerator object without a block");
    slots[0] = ev;                                          /* root across make_proc alloc */
    const VALUE proc = UNWRAP(korb_block_to_proc(c, slots + 1, block, def_env, cself));
    KorbEnumerator *const e = VAL2ENUM(slots[0]);           /* re-derive: make_proc may have moved it */
    e->mode = 3; e->cursor = 0;
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->source, proc);
    return RESULT_OK(slots[0]);
}
/* map: collect block results over the materialized values; no block → self. */
static RESULT korb_lazy_gen_new(CTX *c, VALUE *slots, VALUE proc, bool src_inf);   /* fwd */
/* x.lazy — a lazy enumerator over x (Array/Range), or self if already lazy. */
static RESULT korb_m_to_lazy(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE v = VALUE_REF_GET(self);
    if (KORB_ENUM_P(v)) {
        const uint8_t m = VAL2ENUM(v)->mode;
        if (m == 1 || m == 2 || m == 4) return RESULT_OK(v);    /* already lazy / cycle / lazy-generator */
        if (m == 3) { const bool si = VAL2ENUM(v)->size_inf; slots[0] = VAL2ENUM(v)->source; return korb_lazy_gen_new(c, slots + 1, slots[0], si); }   /* generator → lazy generator (carry infinite size) */
    }
    if (KORB_HASH_P(v)) {                                        /* lazy over the [k,v] pairs (the driver iterates Arrays) */
        slots[0] = v;
        RESULT ta = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_a", 4), 0, 0, NULL, NULL, NULL);
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
static RESULT korb_lazy_gen_new(CTX *c, VALUE *slots, VALUE proc, bool src_inf) {
    slots[0] = proc;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));           /* empty ops */
    KorbEnumerator *e = korb_alloc(c, slots + 2, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    e->mode = 4;
    e->size_inf = src_inf;                                       /* infinite source (e.g. loop.lazy) */
    e->size = KORB_NIL;                                          /* generator: size unknown unless set by Lazy.new(obj, size) */
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
    if (UNLIKELY(block == NULL))                                 /* CRuby: Lazy#select/map/... require a block */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to call lazy %s without a block", op);
    if (SELF_ENUM->mode == 3) { RESULT fr = korb_enum_force_gen(c, slots, self); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr; }
    if (SELF_ENUM->mode != 0) {                                  /* lazy(1)/cycle(2)/lazy-generator(4): defer (chain) */
        slots[0] = UNWRAP(korb_block_to_proc(c, slots, block, def_env, cself));
        return korb_lazy_chain(c, slots + 1, self, op, slots[0]);
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2ARY(SELF_ENUM->values)->len));   /* eager */
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    const bool is_take_while = !strcmp(op, "take_while");
    const bool is_drop_while = !strcmp(op, "drop_while");
    bool dropping = is_drop_while;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        slots[1] = korb_items_data(v->items)[i];
        if (is_drop_while && !dropping) {                 /* past the prefix: no more block calls */
            CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
            continue;
        }
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        const bool sel = (op[0] == 's' || op[0] == 'f');   /* select / filter / filter_map */
        if (is_map) CHECK(korb_ary_push_val(c, slots + 2, dst, r.value));
        else if (is_take_while) { if (!KORB_TRUTHY(r.value)) break; CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1])); }
        else if (is_drop_while) { if (KORB_TRUTHY(r.value)) continue; dropping = false; CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1])); }
        else if (!strcmp(op, "reject")) { if (!KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1])); }
        else if (!strcmp(op, "filter_map")) { if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 2, dst, r.value)); }
        else if (sel) { if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1])); }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_enum_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_lazy_op(c, slots, self, "select", false, block, def_env, cself);
}
/* Enumerator::Lazy#grep / #grep_v — on a lazy enum, chain the op (stays lazy, so
 * an infinite source + .first(n) never over-iterates); on an eager enum,
 * materialize and delegate to Array#grep.  `op` is "grep" or "grep_v". */
static RESULT korb_m_ary_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);      /* fwd (array_enum.c) */
static RESULT korb_m_ary_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);    /* fwd */
static RESULT korb_enum_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, const char *op) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    if (SELF_ENUM->mode == 1 || SELF_ENUM->mode == 4) {          /* lazy (1) / lazy-generator (4) → defer (chain) */
        slots[0] = VALUE_SLICE_GET(a, 0);                        /* pattern */
        slots[1] = KORB_NIL;
        if (block != NULL) {                                     /* optional block transforms each match */
            slots[1] = UNWRAP(korb_block_to_proc(c, slots + 1, block, def_env, cself));
        }
        return korb_lazy_chain2(c, slots + 2, self, op, slots[0], slots[1]);
    }
    RESULT av = korb_m_enum_to_a(c, slots, self, a);             /* eager (0/2/3): materialize + Array#grep */
    if (UNLIKELY(av.state != KORB_NORMAL)) return av;
    slots[0] = av.value;
    return (op[4] == '\0' ? korb_m_ary_grep : korb_m_ary_grep_v)(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_enum_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)   { return korb_enum_grep(c, slots, self, a, block, def_env, cself, "grep"); }
static RESULT korb_m_enum_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_enum_grep(c, slots, self, a, block, def_env, cself, "grep_v"); }
/* Enumerator::Lazy#uniq — lazy: chain (a per-drive seen-Hash dedups, so an
 * infinite source stays productive); eager: delegate to Array#uniq. */
static RESULT korb_m_ary_uniq_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);   /* fwd (array_ext.c) */
static RESULT korb_m_enum_uniq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (SELF_ENUM->mode == 1 || SELF_ENUM->mode == 4) {          /* lazy → chain "uniq" (proc = block or nil) */
        slots[0] = KORB_NIL;
        if (block != NULL) {
            slots[0] = UNWRAP(korb_block_to_proc(c, slots, block, def_env, cself));
        }
        return korb_lazy_chain(c, slots + 1, self, "uniq", slots[0]);
    }
    RESULT av = korb_m_enum_to_a(c, slots, self, a);             /* eager */
    if (UNLIKELY(av.state != KORB_NORMAL)) return av;
    slots[0] = av.value;
    return korb_m_ary_uniq_b(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
/* Enumerator::Lazy#flat_map / #collect_concat — lazy: chain (korb_lazy_run fans
 * out an Array block result one level); eager: delegate to Array#flat_map. */
static RESULT korb_m_ary_flat_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);   /* fwd */
static RESULT korb_m_enum_flat_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to call lazy flat_map without a block");
    if (SELF_ENUM->mode == 1 || SELF_ENUM->mode == 4) {          /* lazy → chain */
        slots[0] = UNWRAP(korb_block_to_proc(c, slots, block, def_env, cself));
        return korb_lazy_chain(c, slots + 1, self, "flat_map", slots[0]);
    }
    RESULT av = korb_m_enum_to_a(c, slots, self, a);             /* eager (0/2/3): materialize + Array#flat_map */
    if (UNLIKELY(av.state != KORB_NORMAL)) return av;
    slots[0] = av.value;
    return korb_m_ary_flat_map(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_enum_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_lazy_op(c, slots, self, "reject", false, block, def_env, cself);
}
static RESULT korb_m_enum_filter_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_lazy_op(c, slots, self, "filter_map", false, block, def_env, cself);
}
static RESULT korb_m_enum_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to call lazy take_while without a block");
    if (SELF_ENUM->mode == 3)                                    /* plain generator: eager, but self-bounded — stream + collect (infinite-safe) */
        return korb_enum_gen_drive_block(c, slots, self, 1, block, def_env, cself);
    return korb_lazy_op(c, slots, self, "take_while", false, block, def_env, cself);
}
static RESULT korb_m_enum_compact(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (SELF_ENUM->mode != 0)
        return korb_lazy_chain(c, slots, self, "compact", KORB_NIL);   /* lazy: chain the op (no-block: drop nils) */
    /* eager Enumerator (Enumerable#compact): materialize the non-nil values into a new Array. */
    slots[0] = SELF_ENUM->values;                                      /* root source */
    RESULT ar = korb_ary_new(c, slots + 1, KORB_ARRAY_P(slots[0]) ? VAL2ARY(slots[0])->len : 0);
    if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
    slots[1] = ar.value;
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    if (KORB_ARRAY_P(slots[0]))
        for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++) {
            const VALUE v = korb_items_data(VAL2ARY(slots[0])->items)[i];
            if (v != KORB_NIL) CHECK(korb_ary_push_val(c, slots + 2, dst, v));
        }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_enum_drop_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_lazy_op(c, slots, self, "drop_while", false, block, def_env, cself);
}
/* lazy drop(n) / take(n): chain a counted op (lazy); eager → slice the values. */
static RESULT korb_lazy_count_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, const char *op) {
    korb_sword_t n; if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
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
    const uint32_t len = v->len, lo = is_take ? 0 : (uint32_t)(n < (korb_sword_t)len ? n : len);
    const uint32_t hi = is_take ? (uint32_t)(n < (korb_sword_t)len ? n : len) : len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, hi - lo));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = lo; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(VAL2ARY(SELF_ENUM->values)->items)[i]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_enum_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_lazy_count_op(c, slots, self, a, "drop"); }
static RESULT korb_m_enum_take_l(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_lazy_count_op(c, slots, self, a, "take"); }
/* Enumerator#first([n]) — lazy: drive up to n; eager: head of values. */
static RESULT korb_m_enum_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const bool has_n = VALUE_SLICE_LEN(a) >= 1;
    korb_sword_t n = 1;
    if (has_n && UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (SELF_ENUM->mode != 0) {
        RESULT d = korb_lazy_drive(c, slots, self, n < 0 ? 0 : n);
        if (UNLIKELY(d.state != KORB_NORMAL)) return d;
        if (!has_n) { const KorbArray *r = VAL2ARY(d.value); return RESULT_OK(r->len ? korb_items_data(r->items)[0] : KORB_NIL); }
        return d;
    }
    const KorbArray *vals = VAL2ARY(SELF_ENUM->values);
    if (!has_n) return RESULT_OK(vals->len ? korb_items_data(vals->items)[0] : KORB_NIL);
    uint32_t take = (n < 0) ? 0 : ((uint32_t)n < vals->len ? (uint32_t)n : vals->len);
    slots[0] = UNWRAP(korb_ary_new(c, slots, take));
    VALUE_REF res = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < take; i++) CHECK(korb_ary_push_val(c, slots + 1, res, korb_items_data(VAL2ARY(SELF_ENUM->values)->items)[i]));
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
        slots[1] = korb_items_data(v->items)[i];
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 2, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* with_index(off=0): yield (value, off+i).  With a block → mapped array;
 * without → a new Enumerator of [value, off+i] pairs. */
static RESULT korb_m_enum_with_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    korb_sword_t off = 0;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {   /* nil ≡ no argument (offset 0) */
        VALUE ov = VALUE_SLICE_GET(a, 0);
        if (FIXNUM_P(ov)) off = FIX2LONG(ov);
        else {                                                            /* Float/#to_int coercion; String etc → TypeError */
            RESULT cr = korb_coerce_to_int(c, slots, &ov);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value != KORB_TRUE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
            korb_to_index(ov, &off);   /* ov is now Integer/Float — extract (truncating Float) */
        }
    }
    if (SELF_ENUM->mode == 1) {                                  /* lazy (source): chain "with_index" [offset, block] */
        slots[0] = LONG2FIX(off);
        slots[1] = KORB_NIL;
        if (block != NULL) { slots[1] = UNWRAP(korb_block_to_proc(c, slots + 1, block, def_env, cself)); }
        return korb_lazy_chain2(c, slots + 2, self, "with_index", slots[0], slots[1]);
    }
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
        slots[2] = korb_items_data(v->items)[i];
        if (block != NULL) {
            VALUE argv[2] = { slots[2], LONG2FIX(off + (korb_sword_t)i) };
            RESULT r = korb_block_yield(c, slots + 3, block, def_env, argv, 2, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (opc == 0) CHECK(korb_ary_push_val(c, slots + 3, dst, r.value));   /* map/each: collect block result */
            else if (opc == 3) {                                                 /* flat_map: flatten the block result one level */
                slots[3] = r.value;
                if (KORB_ARRAY_P(slots[3])) {
                    VALUE_REF fr = VALUE_REF_AT(&slots[3]);
                    for (uint32_t k = 0; k < VAL2ARY(VALUE_REF_GET(fr))->len; k++)
                        CHECK(korb_ary_push_val(c, slots + 4, dst, korb_items_data(VAL2ARY(VALUE_REF_GET(fr))->items)[k]));
                } else {
                    CHECK(korb_ary_push_val(c, slots + 4, dst, slots[3]));
                }
            }
            else if (opc == 4) { if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[2]); }   /* find/detect: return the first match, early-stop */
            else if (KORB_TRUTHY(r.value) == (opc == 1)) CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));   /* select/reject: keep value */
        } else {                                       /* build [value, idx] pair */
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), LONG2FIX(off + (korb_sword_t)i)));
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
    /* Materialize the receiver into a rooted array we can iterate over:
       - eager (0): `values` already holds the array
       - generator (3): force in place, then read `values`
       - lazy/cycle/lazy-gen (1/2/4): drive to a finite array (infinite → NotImplementedError).
       A lazy enum has no materialized `values` field, so reading it would deref garbage. */
    if (SELF_ENUM->mode == 3) CHECK(korb_enum_force_gen(c, slots, self));
    if (SELF_ENUM->mode != 0) {
        RESULT dv = korb_lazy_drive(c, slots, self, -1);
        if (UNLIKELY(dv.state != KORB_NORMAL)) return dv;
        slots[0] = dv.value;
    } else {
        slots[0] = SELF_ENUM->values;
    }
    VALUE_REF vals = VALUE_REF_AT(&slots[0]);       /* rooted: re-read after every may-GC call */
    if (UNLIKELY(block == NULL)) {                     /* no block → Enumerator over [elem, obj] pairs */
        slots[1] = VALUE_SLICE_GET(a, 0);             /* obj (rooted) */
        const uint32_t n = VAL2ARY(VALUE_REF_GET(vals))->len;
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, n));   /* pairs (rooted) */
        VALUE_REF dst = VALUE_REF_AT(&slots[2]);
        for (uint32_t i = 0; i < n; i++) {
            slots[3] = korb_items_data(VAL2ARY(VALUE_REF_GET(vals))->items)[i];   /* elem (rooted) */
            slots[4] = UNWRAP(korb_ary_new(c, slots + 4, 2));        /* [elem, obj] */
            VALUE_REF pr = VALUE_REF_AT(&slots[4]);
            CHECK(korb_ary_push_val(c, slots + 5, pr, slots[3]));
            CHECK(korb_ary_push_val(c, slots + 5, pr, slots[1]));
            CHECK(korb_ary_push_val(c, slots + 5, dst, VALUE_REF_GET(pr)));
        }
        slots[3] = UNWRAP(korb_enum_desc(c, slots + 3, VALUE_REF_GET(self), "with_object"));
        return korb_enum_new(c, slots + 4, VALUE_REF_GET(dst), slots[3]);
    }
    slots[1] = VALUE_SLICE_GET(a, 0);                  /* the memo object (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(VALUE_REF_GET(vals));
        if (i >= v->len) break;
        VALUE argv[2] = { korb_items_data(v->items)[i], slots[1] };
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[1]);
}
/* non-eager next/peek: re-drive bounded to cursor+1, return the value there.
 * Generators (mode 3/4) run their proc; lazy chains / cycle (mode 1/2) drive the
 * lazy pipeline (finite only). */
static RESULT korb_enum_gen_at_cursor(CTX *c, VALUE *slots, VALUE_REF self, bool advance) {
    const uint32_t cur = SELF_ENUM->cursor;
    RESULT vr = (SELF_ENUM->mode == 3 || SELF_ENUM->mode == 4)
                    ? korb_enum_gen_run(c, slots, self, (korb_sword_t)cur + 1)
                    : korb_lazy_drive(c, slots, self, (korb_sword_t)cur + 1);
    if (UNLIKELY(vr.state != KORB_NORMAL)) return vr;
    slots[0] = vr.value;
    if (cur >= VAL2ARY(slots[0])->len) return korb_raise(c, slots, KORB_E_STOP_ITERATION, 0, "iteration reached an end");
    const VALUE v = korb_items_data(VAL2ARY(slots[0])->items)[cur];
    if (advance) SELF_ENUM->cursor = cur + 1;
    return RESULT_OK(v);
}
/* __enum_mode: the internal mode (0 eager, 1 lazy, 2 cycle, 3/4 generator).
 * Exposed to the prelude, which drives external iteration (next/peek) of a
 * non-eager enumerator through a Fiber: those `each` bodies are not restartable
 * (a generator may have side effects), so re-driving to cursor+1 loses values. */
static RESULT korb_m_enum_mode(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    return RESULT_OK(LONG2FIX(SELF_ENUM->mode));
}
static RESULT korb_m_enum_next(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (SELF_ENUM->mode != 0) return korb_enum_gen_at_cursor(c, slots, self, true);
    KorbEnumerator *e = SELF_ENUM;
    const KorbArray *v = VAL2ARY(e->values);
    if (e->cursor >= v->len) return korb_raise(c, slots, KORB_E_STOP_ITERATION, 0, "iteration reached an end");
    return RESULT_OK(korb_items_data(v->items)[e->cursor++]);
}
static RESULT korb_m_enum_peek(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (SELF_ENUM->mode != 0) return korb_enum_gen_at_cursor(c, slots, self, false);
    const KorbEnumerator *e = SELF_ENUM;
    const KorbArray *v = VAL2ARY(e->values);
    if (e->cursor >= v->len) return korb_raise(c, slots, KORB_E_STOP_ITERATION, 0, "iteration reached an end");
    return RESULT_OK(korb_items_data(v->items)[e->cursor]);
}
static RESULT korb_m_enum_rewind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    SELF_ENUM->cursor = 0;
    return RESULT_OK(VALUE_REF_GET(self));
}
/* next_values / peek_values: the yielded value(s) as an Array.  A multi-value
 * yield is stored as an Array already (return it); a single value is wrapped. */
static RESULT korb_enum_values_at_cursor(CTX *c, VALUE *slots, VALUE_REF self, bool advance) {
    if (SELF_ENUM->mode != 0) {   /* generator / lazy / cycle: drive to the cursor, wrap the value */
        slots[0] = UNWRAP(korb_enum_gen_at_cursor(c, slots, self, advance));
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));
        CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), slots[0]));
        return RESULT_OK(slots[1]);
    }
    KorbEnumerator *const e = SELF_ENUM;
    const KorbArray *const v = VAL2ARY(e->values);
    if (e->cursor >= v->len) return korb_raise(c, slots, KORB_E_STOP_ITERATION, 0, "iteration reached an end");
    slots[0] = korb_items_data(v->items)[e->cursor];             /* the single stored value (park before alloc) */
    if (advance) e->cursor++;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));
    CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), slots[0]));
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_enum_next_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_enum_values_at_cursor(c, slots, self, true); }
static RESULT korb_m_enum_peek_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_enum_values_at_cursor(c, slots, self, false); }

