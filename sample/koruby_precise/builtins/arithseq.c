/* koruby_precise — arithseq.c: Enumerator::ArithmeticSequence (step / %).
 * #included into korb_runtime.c's TU.  A lazy numeric sequence created by
 * Numeric#step / Range#step / Range#% without a block. */

/* recv = begin (Numeric) or source Range; a0/a1 = literal call args. */
static RESULT korb_arithseq_new(CTX *c, korb_value *slots, korb_value recv, korb_value a0, korb_value a1, uint8_t nargs, uint8_t is_pct) {
    slots[0] = recv; slots[1] = a0; slots[2] = a1;            /* root across alloc */
    KorbArithSeq *as = korb_alloc(c, slots + 3, sizeof(KorbArithSeq), KORB_OBJ_ARITHSEQ);
    as->nargs = nargs; as->is_pct = is_pct;
    ARO_STORE(c, as, (korb_value *)(uintptr_t)&as->recv, slots[0]);
    ARO_STORE(c, as, (korb_value *)(uintptr_t)&as->a0,   slots[1]);
    ARO_STORE(c, as, (korb_value *)(uintptr_t)&as->a1,   slots[2]);
    return RESULT_OK((korb_value)as);
}

/* Resolve (begin, limit, step, exclusive) for the sequence.  limit == KORB_NIL
 * means endless.  Returns false on a non-numeric component (caller raises). */
static void korb_aseq_params(const KorbArithSeq *as, korb_value *beginv, korb_value *limv, korb_value *stepv, bool *excl) {
    if (KORB_RANGE_P(as->recv)) {
        const KorbRange *rg = VAL2RANGE(as->recv);
        *beginv = rg->rbegin; *limv = rg->rend; *excl = rg->exclude_end != 0;
        *stepv = as->nargs >= 1 ? as->a0 : LONG2FIX(1);
    } else {
        *beginv = as->recv; *excl = false;
        *limv  = as->nargs >= 1 ? as->a0 : KORB_NIL;
        *stepv = as->nargs >= 2 && as->a1 != KORB_NIL ? as->a1 : LONG2FIX(1);   /* step(0, nil): inspect keeps the nil */
    }
}

/* A component that is neither Integer nor Float: the prelude's generic path
 * (which raises the TypeError for a non-Numeric). */
/* Rational / BigDecimal / ...: a heap Numeric the C paths do not compute with. */
static bool korb_aseq_generic_p(const korb_value v) {
    return AROH_IS_GC_OBJECT(v) && !KORB_FLOAT_P(v) && !KORB_INTEGER_P(v) && !KORB_STRING_P(v);
}
static RESULT korb_aseq_generic(CTX *c, korb_value *slots, korb_ref self, const char *const meth, const uint32_t argc, const korb_value arg) {
    slots[0] = KORB_REF_GET(self);
    if (argc) slots[1] = arg;
    return korb_send(c, slots + 1 + argc, korb_intern(c->vm, meth, (uint32_t)strlen(meth)), 0, argc);
}
/* Materialize the sequence into a fresh Array (rooted via return).  Raises
 * RangeError for an endless sequence (limit nil), matching CRuby. */
static RESULT korb_aseq_to_array(CTX *c, korb_value *slots, korb_ref self) {
    const KorbArithSeq *as = VAL2ASEQ(KORB_REF_GET(self));
    korb_value beginv, limv, stepv; bool excl;
    korb_aseq_params(as, &beginv, &limv, &stepv, &excl);
    if (limv == KORB_NIL)
        return korb_raise(c, slots, KORB_E_RANGE, 0, "cannot convert endless arithmetic sequence to an array");
    /* Non-numeric (e.g. String) range stepped by a positive Integer: materialize
     * the whole range via #succ (Range#to_a), then stride by the step.  Numeric
     * ranges fall through to the scalar paths below. */
    if (KORB_RANGE_P(as->recv) && !FIXNUM_P(beginv) && !KORB_FLOAT_P(beginv)
        && FIXNUM_P(stepv) && FIX2LONG(stepv) > 0) {
        const uintptr_t st = (uintptr_t)FIX2LONG(stepv);
        slots[0] = VAL2ASEQ(KORB_REF_GET(self))->recv;            /* root the range across to_a's alloc */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots + 1, KORB_REF_AT(&slots[0]), KORB_SLICE_MAKE(NULL, 0)));
        korb_ref full = KORB_REF_AT(&slots[0]);
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
        korb_ref dst = KORB_REF_AT(&slots[1]);
        for (uintptr_t i = 0; i < VAL2ARY(KORB_REF_GET(full))->len; i += st) {
            korb_value ev = korb_ary_ptr(VAL2ARY(KORB_REF_GET(full)))[i];   /* push_val roots ev on its grow path */
            CHECK(korb_ary_push_val(c, slots + 2, dst, ev));
        }
        return RESULT_OK(KORB_REF_GET(dst));
    }
    const bool use_float = KORB_FLOAT_P(beginv) || KORB_FLOAT_P(limv) || KORB_FLOAT_P(stepv);
    /* Extract all scalars BEFORE allocating — under STRESS the array alloc GCs and
     * would move the Float operands (beginv/limv/stepv) out from under us. */
    double s = 0, lim = 0, st = 0; korb_sword_t is = 0, ilim = 0, ist = 0;
    if (use_float) {
        if (!korb_num_to_d(beginv, &s) || !korb_num_to_d(limv, &lim) || !korb_num_to_d(stepv, &st))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "step requires numeric arguments");
        if (st == 0.0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
    } else {
        if (UNLIKELY(!FIXNUM_P(beginv) || !FIXNUM_P(limv) || !FIXNUM_P(stepv) ||
                     (FIX2LONG(stepv) > 0 ? FIX2LONG(limv) > FIXNUM_MAX - FIX2LONG(stepv)
                                          : FIX2LONG(limv) < FIXNUM_MIN - FIX2LONG(stepv)))) {
            if (!KORB_INTEGER_P(beginv) || !KORB_INTEGER_P(limv) || !KORB_INTEGER_P(stepv))
                return korb_aseq_generic(c, slots, self, "__generic_to_a", 0, KORB_NIL);   /* Rational, BigDecimal, ... */
            slots[0] = beginv;                                        /* a value leaves the Fixnum range */
            return korb_int_step_wide_x(c, slots + 1, KORB_REF_AT(&slots[0]), limv, stepv, NULL, NULL, NULL, true, excl, true);
        }
        is = FIX2LONG(beginv); ilim = FIX2LONG(limv); ist = FIX2LONG(stepv);
        if (ist == 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
    korb_ref dst = KORB_REF_AT(&slots[0]);
    if (use_float) {
        const long cnt = korb_float_step_n(s, lim, st, excl);   /* CRuby's count formula (+ clamp) */
        for (long i = 0; i < cnt; i++) {
            slots[1] = UNWRAP(korb_float_new(c, slots + 1, korb_float_step_at(s, lim, st, i, excl)));
            CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
        }
    } else {
        for (korb_sword_t i = is; ist > 0 ? (excl ? i < ilim : i <= ilim) : (excl ? i > ilim : i >= ilim); i += ist)
            CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}

static RESULT korb_m_aseq_to_a(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a; return korb_aseq_to_array(c, slots, self);
}
/* ArithmeticSequence#size — analytic (never materializes): an endless or
 * step-toward-infinity sequence is Infinity; finite counts use the closed form
 * (integer exact; float with CRuby's epsilon fudge to avoid drift off-by-one). */
static RESULT korb_m_aseq_size(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const KorbArithSeq *as = VAL2ASEQ(KORB_REF_GET(self));
    korb_value beginv, limv, stepv; bool excl;
    korb_aseq_params(as, &beginv, &limv, &stepv, &excl);
    double bd, ld, sd;
    if (!korb_num_to_d(beginv, &bd) || !korb_num_to_d(stepv, &sd) || (limv != KORB_NIL && !korb_num_to_d(limv, &ld))) {
        if (korb_aseq_generic_p(beginv) || korb_aseq_generic_p(stepv) || korb_aseq_generic_p(limv))
            return korb_aseq_generic(c, slots, self, "__generic_size", 0, KORB_NIL);
        return RESULT_OK(KORB_NIL);                                            /* non-numeric (e.g. String range) */
    }
    if (limv == KORB_NIL) return korb_float_new(c, slots, INFINITY);          /* endless numeric */
    if (UNLIKELY(sd == 0.0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
    if (isinf(sd))                                                            /* infinite step: at most the first element */
        return RESULT_OK(LONG2FIX((sd > 0 ? bd <= ld : bd >= ld) ? 1 : 0));
    if (isinf(ld)) return ((sd > 0) == (ld > 0)) ? korb_float_new(c, slots, INFINITY) : RESULT_OK(LONG2FIX(0));
    if (FIXNUM_P(beginv) && FIXNUM_P(limv) && FIXNUM_P(stepv)) {              /* exact integer count */
        korb_sword_t span = FIX2LONG(limv) - FIX2LONG(beginv), st = FIX2LONG(stepv);
        korb_sword_t cnt;
        if (st > 0) cnt = span < 0 ? 0 : span / st + 1;
        else        cnt = span > 0 ? 0 : (-span) / (-st) + 1;
        if (excl && cnt > 0 && span % st == 0) cnt--;                        /* endpoint excluded when hit exactly */
        return RESULT_OK(LONG2FIX(cnt));
    }
    const long cnt = korb_float_step_n(bd, ld, sd, excl);                     /* CRuby's ruby_float_step_size */
    return RESULT_OK(LONG2FIX(cnt > 0 ? cnt : 0));
}
static RESULT korb_m_aseq_each(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    if (block == NULL) return RESULT_OK(KORB_REF_GET(self));
    {   /* an Integer sequence streams: a long (or endless) one must not materialize first */
        korb_value bv, lv, sv; bool excl;
        korb_aseq_params(VAL2ASEQ(KORB_REF_GET(self)), &bv, &lv, &sv, &excl);
        if (FIXNUM_P(bv) && FIXNUM_P(sv) && FIX2LONG(sv) != 0 && (lv == KORB_NIL || FIXNUM_P(lv))) {
            const korb_sword_t st = FIX2LONG(sv), lim = lv == KORB_NIL ? 0 : FIX2LONG(lv);
            const bool endless = (lv == KORB_NIL);
            for (korb_sword_t i = FIX2LONG(bv); FIXABLE(i); i += st) {   /* past the Fixnum range: stop (bounded ones end first) */
                if (!endless && (st > 0 ? (excl ? i >= lim : i > lim) : (excl ? i <= lim : i < lim))) break;
                korb_value iv = LONG2FIX(i);
                const RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            }
            return RESULT_OK(KORB_REF_GET(self));
        }
    }
    slots[0] = UNWRAP(korb_aseq_to_array(c, slots, self));     /* materialize (rooted) */
    korb_ref arr = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(KORB_REF_GET(arr));
        if (i >= v->len) break;
        slots[1] = korb_ary_ptr(v)[i];
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(KORB_REF_GET(self));
}
/* first / first(n): lazy — take from the front without materializing the whole seq. */
static RESULT korb_m_aseq_first(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const KorbArithSeq *as = VAL2ASEQ(KORB_REF_GET(self));
    korb_value beginv, limv, stepv; bool excl;
    korb_aseq_params(as, &beginv, &limv, &stepv, &excl);
    const bool want_n = KORB_SLICE_LEN(a) >= 1;
    korb_sword_t n = 1;
    if (want_n && UNLIKELY(!korb_to_index(KORB_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    const bool use_float = KORB_FLOAT_P(beginv) || KORB_FLOAT_P(limv) || KORB_FLOAT_P(stepv);
    double s = 0, lim = 0, st = 0; korb_sword_t is = 0, ilim = 0, ist = 0; bool endless = (limv == KORB_NIL);
    if (use_float) {
        if (!korb_num_to_d(beginv, &s) || !korb_num_to_d(stepv, &st) || (!endless && !korb_num_to_d(limv, &lim)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "step requires numeric arguments");
    } else {
        if (UNLIKELY(!FIXNUM_P(beginv) || !FIXNUM_P(stepv) || (!endless && !FIXNUM_P(limv)))) {
            if (korb_aseq_generic_p(beginv) || korb_aseq_generic_p(stepv) || korb_aseq_generic_p(limv))
                return korb_aseq_generic(c, slots, self, "__generic_first", want_n ? 1 : 0, want_n ? LONG2FIX(n) : KORB_NIL);
            return korb_raise(c, slots, KORB_E_TYPE, 0, "step requires numeric arguments");
        }
        is = FIX2LONG(beginv); ist = FIX2LONG(stepv); if (!endless) ilim = FIX2LONG(limv);
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, (uint32_t)(n > 0 ? n : 0)));
    korb_ref dst = KORB_REF_AT(&slots[0]);
    for (korb_sword_t i = 0; (want_n ? (korb_sword_t)VAL2ARY(KORB_REF_GET(dst))->len < n : i < 1); i++) {
        if (use_float) {
            if (!endless && i >= korb_float_step_n(s, lim, st, excl)) break;
            const double d = endless ? s + (double)i * st : korb_float_step_at(s, lim, st, i, excl);
            slots[1] = UNWRAP(korb_float_new(c, slots + 1, d));
            CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
        } else {
            korb_sword_t d = is + i * ist;
            if (!endless && (ist > 0 ? (excl ? d >= ilim : d > ilim) : (excl ? d <= ilim : d < ilim))) break;
            CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(d)));
        }
    }
    if (!want_n) {   /* first → element or nil */
        const KorbArray *d = VAL2ARY(KORB_REF_GET(dst));
        return RESULT_OK(d->len ? korb_ary_ptr(d)[0] : KORB_NIL);
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_aseq_last(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    slots[0] = UNWRAP(korb_aseq_to_array(c, slots, self));
    const KorbArray *d = VAL2ARY(slots[0]);
    return RESULT_OK(d->len ? korb_ary_ptr(d)[d->len - 1] : KORB_NIL);
}
static RESULT korb_m_aseq_begin(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a; korb_value b, l, s; bool e; korb_aseq_params(VAL2ASEQ(KORB_REF_GET(self)), &b, &l, &s, &e); return RESULT_OK(b);
}
static RESULT korb_m_aseq_end(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a; korb_value b, l, s; bool e; korb_aseq_params(VAL2ASEQ(KORB_REF_GET(self)), &b, &l, &s, &e); return RESULT_OK(l);
}
static RESULT korb_m_aseq_exclude_end(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a; korb_value b, l, s; bool e; korb_aseq_params(VAL2ASEQ(KORB_REF_GET(self)), &b, &l, &s, &e); return RESULT_OK(e ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_aseq_step_acc(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a; korb_value b, l, s; bool e; korb_aseq_params(VAL2ASEQ(KORB_REF_GET(self)), &b, &l, &s, &e); return RESULT_OK(s);
}

/* __arith_seq(recv, limit, step, nargs) — Numeric#step without a block (prelude) */
static RESULT korb_bi_arith_seq(CTX *c, korb_value *slots, korb_slice args) {
    const korb_value na = KORB_SLICE_GET(args, 3);
    return korb_arithseq_new(c, slots, KORB_SLICE_GET(args, 0), KORB_SLICE_GET(args, 1), KORB_SLICE_GET(args, 2),
                             (uint8_t)(FIXNUM_P(na) ? FIX2LONG(na) : 2), 0);
}
