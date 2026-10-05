/* koruby_precise — array_enum.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Array enumerable / aggregate methods -------------------------------- */
static RESULT korb_lazy_new(CTX *c, korb_value *slots, korb_value source, uint8_t mode);   /* enumerator.c */
static RESULT korb_arithseq_new(CTX *c, korb_value *slots, korb_value recv, korb_value a0, korb_value a1, uint8_t nargs, uint8_t is_pct);   /* arithseq.c */


/* Array#index is a true alias of #find_index (registered to the same CFUNC). */

static RESULT korb_m_ary_count(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (block != NULL && KORB_SLICE_LEN(a) > 0) korb_warn(c, slots, "given block not used");   /* arg wins */
    if (block != NULL && KORB_SLICE_LEN(a) == 0) {  /* block form: count truthy yields */
        korb_sword_t n = 0;
        for (uint32_t i = 0; ; i++) {
            const KorbArray *ary = SELF_ARY;
            if (i >= ary->len) break;
            slots[0] = korb_ary_ptr(ary)[i];
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) n++;
        }
        return RESULT_OK(LONG2FIX(n));
    }
    if (KORB_SLICE_LEN(a) == 0) return RESULT_OK(LONG2FIX(SELF_ARY->len));
    slots[0] = KORB_SLICE_GET(a, 0);                    /* needle (root across element == dispatch) */
    const uint32_t len = VAL2ARY(KORB_REF_GET(self))->len;
    korb_sword_t cnt = 0;
    const bool eqr = KORB_EQ_REDEF(c);   /* read once: the loop calls out of line */
    for (uint32_t i = 0; i < len; i++) {
        const korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];
        if (korb_eq_send_p_(eqr, e, slots[0])) {  /* user == → dispatch (element == needle) */
            slots[1] = e; slots[2] = slots[0];
            RESULT r = korb_send_impl(c, slots + 3, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) cnt++;
        } else if (korb_value_eq(e, slots[0])) {
            cnt++;
        }
    }
    return RESULT_OK(LONG2FIX(cnt));
}

static RESULT korb_m_ary_sum(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value init = KORB_SLICE_LEN(a) >= 1 ? KORB_SLICE_GET(a, 0) : LONG2FIX(0);
    const KorbArray *ary = SELF_ARY;
    bool any_float = KORB_FLOAT_P(init), all_num = FIXNUM_P(init) || KORB_FLOAT_P(init);
    for (uint32_t i = 0; all_num && i < ary->len; i++) {
        korb_value e = korb_ary_ptr(ary)[i];
        if (KORB_FLOAT_P(e)) any_float = true; else if (!FIXNUM_P(e)) all_num = false;
    }
    if (all_num) {                                   /* numeric fast path */
        if (any_float) {                             /* Kahan-Babuska-Neumaier compensated sum (CRuby) */
            double sum; korb_num_to_d(init, &sum);
            double comp = 0.0;
            const KorbArray *ar = SELF_ARY;
            for (uint32_t i = 0; i < ar->len; i++) {
                double x; korb_num_to_d(korb_ary_ptr(ar)[i], &x);
                const double t = sum + x;
                if (t - t == 0.0) {                  /* t finite → compensate; non-finite (Inf/NaN) → skip (Inf-Inf=NaN) */
                    const double as = sum < 0 ? -sum : sum, ax = x < 0 ? -x : x;
                    comp += (as >= ax) ? ((sum - t) + x) : ((x - t) + sum);
                }
                sum = t;
            }
            return korb_float_new(c, slots, sum + comp);
        }
        korb_sword_t acc = FIX2LONG(init);
        for (uint32_t i = 0; i < ary->len; i++) acc += FIX2LONG(korb_ary_ptr(ary)[i]);
        return RESULT_OK(LONG2FIX(acc));
    }
    slots[0] = init;                                 /* general fold: init + e0 + e1 + ... via + operator */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ar = SELF_ARY;
        if (i >= ar->len) break;
        slots[1] = korb_ary_ptr(ar)[i];
        RESULT r = korb_plus_slow(c, slots + 2, KORB_REF_AT(&slots[0]), slots[1], 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    return RESULT_OK(slots[0]);
}
/* Array#sum with a block: init + block(e0) + block(e1) + ... (+ via dispatch).  Once the
 * running sum and a block value are both Float/Integer, switch to the compensated Float
 * sum as the block-less form (CRuby does; hexapdf's line widths came out 49.99 not 50). */
static RESULT korb_m_ary_sum_b(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (block == NULL) return korb_m_ary_sum(c, slots, self, a);
    const uint32_t plus = korb_intern_lit(c->vm, "+", 1);
    slots[0] = KORB_SLICE_LEN(a) >= 1 ? KORB_SLICE_GET(a, 0) : LONG2FIX(0);   /* acc (recv) */
    bool fmode = false;
    double sum = 0.0, comp = 0.0;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ar = SELF_ARY;
        if (i >= ar->len) break;
        slots[1] = korb_ary_ptr(ar)[i];
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        const korb_value e = r.value;
        if (!fmode && KORB_FLOAT_P(e) && (FIXNUM_P(slots[0]) || KORB_FLOAT_P(slots[0]))) {
            korb_num_to_d(slots[0], &sum);
            comp = 0.0;
            fmode = true;
        }
        if (fmode) {
            if (KORB_FLOAT_P(e) || FIXNUM_P(e)) {
                double x; korb_num_to_d(e, &x);
                const double t = sum + x;
                if (t - t == 0.0) {                  /* as korb_m_ary_sum: compensate only while finite */
                    const double as = sum < 0 ? -sum : sum, ax = x < 0 ? -x : x;
                    comp += (as >= ax) ? ((sum - t) + x) : ((x - t) + sum);
                }
                sum = t;
                continue;
            }
            slots[1] = e;                            /* root across the Float alloc */
            slots[0] = UNWRAP(korb_float_new(c, slots + 2, sum + comp));
            fmode = false;
        } else {
            slots[1] = e;
        }
        r = korb_send(c, slots + 2, plus, 0, 1);     /* acc = acc + block(e) */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    if (fmode) return korb_float_new(c, slots, sum + comp);
    return RESULT_OK(slots[0]);
}

/* min (want=-1) / max (want=1) by <=> */
static RESULT korb_cmp_spaceship(CTX *c, korb_value *slots, korb_value a, korb_value b, int *out);   /* fwd */
struct korb_msort_blk { NODE *block; korb_value *def_env; korb_value *cself; };
typedef RESULT (*korb_msort_cmp_fn)(CTX *c, korb_value *slots, korb_value a, korb_value b, const void *ud, int *out);
static RESULT korb_ary_msort(CTX *c, korb_value *slots, korb_ref aref, korb_msort_cmp_fn cmp, const void *ud);   /* fwd */
static RESULT korb_msort_cmp_block(CTX *c, korb_value *slots, korb_value a, korb_value b, const void *ud, int *out);       /* fwd */
static RESULT korb_msort_cmp_spaceship(CTX *c, korb_value *slots, korb_value a, korb_value b, const void *ud, int *out);   /* fwd */
static RESULT korb_ary_minmax(CTX *c, korb_value *slots, korb_ref self, int want) {
    const uint32_t len = SELF_ARY->len;
    if (len == 0) return RESULT_OK(KORB_NIL);
    slots[0] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[0];   /* best (rooted across any <=> dispatch GC) */
    for (uint32_t i = 1; i < len; i++) {
        korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];
        int cmp;
        if (UNLIKELY(korb_eq_needs_send(e) || korb_eq_needs_send(slots[0]) || KORB_CLASS_P(e) || KORB_CLASS_P(slots[0]))) {   /* user/Comparable/class → dispatch <=> */
            slots[1] = e;                                             /* root e across the dispatch */
            CHECK(korb_cmp_spaceship(c, slots + 2, slots[1], slots[0], &cmp));
            e = slots[1];                                            /* re-read (may have moved) */
        } else {
            cmp = korb_cmp_full(c, e, slots[0]);
            if (UNLIKELY(cmp == 2)) return korb_raise_cmperr(c, slots, 0, e, slots[0], false);
        }
        if (cmp == want) slots[0] = e;
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_cmp_block(CTX *c, korb_value *slots, korb_value lhs, korb_value rhs,
                             NODE *block, korb_value *def_env, korb_value *cself, int *out);
/* min (want=-1) / max (want=1) via a comparator block.  The running best lives
 * in slots[0] (rooted); each yield may GC, so the element is re-read from the
 * rooted array after the compare. */
static RESULT korb_ary_minmax_blk(CTX *c, korb_value *slots, korb_ref self, int want,
                                  NODE *block, korb_value *def_env, korb_value *cself) {
    uint32_t len = VAL2ARY(KORB_REF_GET(self))->len;
    if (len == 0) return RESULT_OK(KORB_NIL);
    slots[0] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[0];   /* best (rooted) */
    for (uint32_t i = 1; i < len; i++) {
        korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];
        int cmp;
        CHECK(korb_cmp_block(c, slots + 1, e, slots[0], block, def_env, cself, &cmp));
        if (cmp == want) slots[0] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];   /* re-read post-yield */
    }
    return RESULT_OK(slots[0]);
}
/* min(n)/max(n): the n smallest (want=-1) / largest (want=1), sorted accordingly. */
static RESULT korb_ary_minmax_n(CTX *c, korb_value *slots, korb_ref self, int want, korb_sword_t n, NODE *block, korb_value *def_env, korb_value *cself) {
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
    if (n == 0) return korb_ary_new(c, slots, 0);          /* max(0)/min(0) → [] (no comparison) */
    uint32_t len = SELF_ARY->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, len));        /* sorted-ascending working copy */
    korb_ref tmp = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < len; i++) CHECK(korb_ary_push_val(c, slots + 1, tmp, korb_ary_ptr(SELF_ARY)[i]));
    if (block != NULL) {                                 /* min(n)/max(n) { |a,b| ... } — block is the comparator */
        const struct korb_msort_blk k = { block, def_env, cself };
        CHECK(korb_ary_msort(c, slots + 1, tmp, korb_msort_cmp_block, &k));
    } else {
        CHECK(korb_ary_msort(c, slots + 1, tmp, korb_msort_cmp_spaceship, NULL));
    }
    if (n > (korb_sword_t)len) n = (korb_sword_t)len;   /* clamp before narrowing */
    const uint32_t take = (uint32_t)n;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, take));
    korb_ref dst = KORB_REF_AT(slots + 1);              /* tmp(slots[0]) stays rooted below */
    for (uint32_t i = 0; i < take; i++) {
        uint32_t src = want < 0 ? i : len - 1 - i;        /* min: ascending; max: descending */
        CHECK(korb_ary_push_val(c, slots + 2, dst, korb_ary_ptr(VAL2ARY(KORB_REF_GET(tmp)))[src]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_min(CTX *c, korb_value *slots, korb_ref self, korb_slice a,
                             NODE *block, korb_value *def_env, korb_value *cself) {
    korb_sword_t n;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(KORB_SLICE_GET(a, 0), &n)) return korb_ary_minmax_n(c, slots, self, -1, n, block, def_env, cself);
    if (block != NULL) return korb_ary_minmax_blk(c, slots, self, -1, block, def_env, cself);
    return korb_ary_minmax(c, slots, self, -1);
}
static RESULT korb_m_ary_max(CTX *c, korb_value *slots, korb_ref self, korb_slice a,
                             NODE *block, korb_value *def_env, korb_value *cself) {
    korb_sword_t n;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(KORB_SLICE_GET(a, 0), &n)) return korb_ary_minmax_n(c, slots, self, 1, n, block, def_env, cself);
    if (block != NULL) return korb_ary_minmax_blk(c, slots, self, 1, block, def_env, cself);
    return korb_ary_minmax(c, slots, self,  1);
}
static RESULT korb_m_ary_transpose(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    uint32_t rows = SELF_ARY->len;
    if (rows == 0) return korb_ary_new(c, slots, 0);
    /* coerce each row to an Array via #to_ary into a rooted src array. */
    korb_ref src = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, rows)));
    const uint32_t to_ary = korb_intern_lit(c->vm, "to_ary", 6);
    for (uint32_t i = 0; i < rows; i++) {
        slots[0] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];
        if (!KORB_ARRAY_P(slots[0])) {
            if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce_p(c, slots + 1, &slots[0], to_ary)) {
                RESULT r = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                slots[0] = r.value;
            }
            if (UNLIKELY(!KORB_ARRAY_P(slots[0]))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_coerce_name(c, korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i]));
        }
        CHECK(korb_ary_push_val(c, slots + 1, src, slots[0]));
    }
    #define SRC_I(i) (VAL2ARY(korb_ary_ptr(VAL2ARY(KORB_REF_GET(src)))[(i)]))
    const uint32_t cols = SRC_I(0)->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, cols));           /* result rows */
    korb_ref out = KORB_REF_AT(&slots[0]);
    for (uint32_t j = 0; j < cols; j++) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 2, rows));       /* one output row */
        korb_ref row = KORB_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < rows; i++) {
            const KorbArray *e = SRC_I(i);
            if (UNLIKELY(e->len != cols)) return korb_raise(c, slots, KORB_E_INDEX, 0, "element size differs (%u should be %u)", e->len, cols);
            CHECK(korb_ary_push_val(c, slots + 2, row, korb_ary_ptr(e)[j]));
        }
        CHECK(korb_ary_push_val(c, slots + 2, out, KORB_REF_GET(row)));
    }
    #undef SRC_I
    return RESULT_OK(KORB_REF_GET(out));
}
/* minmax via comparator block — replicates CRuby's pairwise scan: seed min/max
 * from the first pair, then for each subsequent pair route the smaller against
 * min and the larger against max.  This reproduces CRuby's exact behaviour even
 * for a non-antisymmetric comparator (e.g. the degenerate `{|x| x}`).  min/max
 * land in slots[0]/slots[1]; A(i) re-reads the (possibly moved) element. */
static RESULT korb_ary_minmax_pair_blk(CTX *c, korb_value *slots, korb_ref self,
                                       NODE *block, korb_value *def_env, korb_value *cself) {
#define A(idx) (korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[(idx)])
    uint32_t n = VAL2ARY(KORB_REF_GET(self))->len;
    if (n == 0) { slots[0] = KORB_NIL; slots[1] = KORB_NIL; return RESULT_OK(KORB_NIL); }
    if (n == 1) { slots[0] = A(0); slots[1] = A(0); return RESULT_OK(KORB_NIL); }
    int cmp;
    CHECK(korb_cmp_block(c, slots + 2, A(0), A(1), block, def_env, cself, &cmp));
    if (cmp <= 0) { slots[0] = A(0); slots[1] = A(1); } else { slots[0] = A(1); slots[1] = A(0); }
    uint32_t i = 2;
    while (i + 1 < n) {
        CHECK(korb_cmp_block(c, slots + 2, A(i), A(i + 1), block, def_env, cself, &cmp));
        uint32_t lo = (cmp <= 0) ? i : i + 1, hi = (cmp <= 0) ? i + 1 : i;
        CHECK(korb_cmp_block(c, slots + 2, A(lo), slots[0], block, def_env, cself, &cmp));
        if (cmp < 0) slots[0] = A(lo);
        CHECK(korb_cmp_block(c, slots + 2, A(hi), slots[1], block, def_env, cself, &cmp));
        if (cmp > 0) slots[1] = A(hi);
        i += 2;
    }
    if (i < n) {                                              /* odd leftover element */
        CHECK(korb_cmp_block(c, slots + 2, A(i), slots[0], block, def_env, cself, &cmp));
        if (cmp < 0) slots[0] = A(i);
        CHECK(korb_cmp_block(c, slots + 2, A(i), slots[1], block, def_env, cself, &cmp));
        if (cmp > 0) slots[1] = A(i);
    }
    return RESULT_OK(KORB_NIL);
#undef A
}
static RESULT korb_m_ary_minmax(CTX *c, korb_value *slots, korb_ref self, korb_slice a,
                                NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    if (block != NULL) {
        CHECK(korb_ary_minmax_pair_blk(c, slots, self, block, def_env, cself));  /* fills slots[0..1] */
    } else {
        slots[0] = UNWRAP(korb_ary_minmax(c, slots, self, -1));        /* min (nil if empty) */
        slots[1] = UNWRAP(korb_ary_minmax(c, slots + 1, self, 1));     /* max */
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(KORB_REF_GET(KORB_REF_AT(&slots[2])));
}

/* Comparator-block helper: yield (lhs, rhs) to `block`, reduce the result to a
 * sign in *out (-1/0/1) with <=> semantics.  A nil / non-numeric result raises
 * ArgumentError; a block exception propagates.  slots[0]/slots[1] hold the two
 * (rooted) operands across the yield, so the caller need not root them. */
static RESULT korb_cmp_block(CTX *c, korb_value *slots, korb_value lhs, korb_value rhs,
                             NODE *block, korb_value *def_env, korb_value *cself, int *out) {
    slots[0] = lhs; slots[1] = rhs;                       /* stage + root args */
    RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[0], 2, cself);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    double d;
    if (LIKELY(korb_num_to_d(r.value, &d))) { *out = d < 0 ? -1 : d > 0 ? 1 : 0; return RESULT_OK(KORB_NIL); }
    if (UNLIKELY(r.value == KORB_NIL))
        return korb_raise_cmperr(c, slots, 0, slots[0], slots[1], false);
    /* CRuby's rb_cmpint: a non-numeric result is asked `> 0`, then `< 0`. */
    slots[2] = r.value; slots[3] = LONG2FIX(0);
    RESULT gr = korb_send(c, slots + 4, korb_intern_lit(c->vm, ">", 1), 0, 1);
    if (UNLIKELY(gr.state != KORB_NORMAL)) return gr;
    if (KORB_TRUTHY(gr.value)) { *out = 1; return RESULT_OK(KORB_NIL); }
    slots[3] = LONG2FIX(0);
    RESULT lr = korb_send(c, slots + 4, korb_intern_lit(c->vm, "<", 1), 0, 1);
    if (UNLIKELY(lr.state != KORB_NORMAL)) return lr;
    *out = KORB_TRUTHY(lr.value) ? -1 : 0;
    return RESULT_OK(KORB_NIL);
}

/* qsort_r comparator for the default-<=> sort: korb_cmp_full doesn't allocate
 * for builtin-comparable types, so the array pointer stays put during qsort.
 * `cmp==2` (incomparable) is latched into err; we raise after the sort. */
/* `bad_l`/`bad_r` keep the pair the comparator refused, so the error blames the
 * operands CRuby blames instead of whatever landed in slots 0 and 1. */
struct korb_sortctx { CTX *c; int err; korb_value bad_l, bad_r; };
static int korb_sort_cmp(const void *pa, const void *pb, void *arg) {
    struct korb_sortctx *const sc = (struct korb_sortctx *)arg;
    int r = korb_cmp_full(sc->c, *(const korb_value *)pa, *(const korb_value *)pb);
    if (UNLIKELY(r == 2)) {
        if (!sc->err) { sc->bad_l = *(const korb_value *)pa; sc->bad_r = *(const korb_value *)pb; }
        sc->err = 1; return 0;
    }
    return r;
}

/* Typed in-place sort for an all-Fixnum array.  A Fixnum korb_value is `(n<<1)|1`,
 * a strictly monotonic map of n, so comparing the raw korb_values as signed
 * korb_sword_t yields integer order — no per-compare callback (the qsort_r PLT
 * indirection + korb_cmp_full type dispatch that dominate the generic path).
 * Median-of-three quicksort with an insertion-sort cutoff; tail-recursion on
 * the larger side is looped to bound stack depth.  Reordering existing Fixnums
 * creates no heap edges → no write barrier needed (matches korb_m_ary_sort). */
static inline void korb_fix_insort(korb_value *const d, const korb_sword_t lo, const korb_sword_t hi) {
    for (korb_sword_t i = lo + 1; i <= hi; i++) {
        const korb_value k = d[i]; korb_sword_t j = i - 1;
        while (j >= lo && (korb_sword_t)d[j] > (korb_sword_t)k) { d[j + 1] = d[j]; j--; }
        d[j + 1] = k;
    }
}
static void korb_fix_qsort(korb_value *const d, korb_sword_t lo, korb_sword_t hi) {
    while (hi - lo > 16) {
        const korb_sword_t mid = lo + ((hi - lo) >> 1);   /* median-of-three pivot into d[hi-1] */
        if ((korb_sword_t)d[mid] < (korb_sword_t)d[lo])     { korb_value t = d[mid]; d[mid] = d[lo]; d[lo] = t; }
        if ((korb_sword_t)d[hi]  < (korb_sword_t)d[lo])     { korb_value t = d[hi];  d[hi]  = d[lo]; d[lo] = t; }
        if ((korb_sword_t)d[hi]  < (korb_sword_t)d[mid])    { korb_value t = d[hi];  d[hi]  = d[mid]; d[mid] = t; }
        const korb_value pivot = d[mid];
        { korb_value t = d[mid]; d[mid] = d[hi - 1]; d[hi - 1] = t; }   /* park pivot at hi-1 */
        korb_sword_t i = lo, j = hi - 1;
        for (;;) {
            do i++; while ((korb_sword_t)d[i] < (korb_sword_t)pivot);
            do j--; while ((korb_sword_t)d[j] > (korb_sword_t)pivot);
            if (i >= j) break;
            korb_value t = d[i]; d[i] = d[j]; d[j] = t;
        }
        { korb_value t = d[i]; d[i] = d[hi - 1]; d[hi - 1] = t; }       /* restore pivot to its seat */
        if (i - lo < hi - i) { korb_fix_qsort(d, lo, i - 1); lo = i + 1; }   /* recurse smaller side */
        else                 { korb_fix_qsort(d, i + 1, hi); hi = i - 1; }
    }
    korb_fix_insort(d, lo, hi);
}
/* Dispatch `a <=> b` (user / Comparable) → *out = sign(-1/0/1).  May GC (caller
 * re-fetches the array after the call).  nil result = incomparable → raise. */
static RESULT korb_cmp_spaceship(CTX *c, korb_value *slots, korb_value a, korb_value b, int *out) {
    slots[0] = a; slots[1] = b;
    RESULT r = korb_send(c, slots + 2, c->vm->mid_cmp, 0, 1);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (UNLIKELY(!FIXNUM_P(r.value)))
        return korb_raise_cmperr(c, slots, 0, slots[0], slots[1], false);
    const korb_sword_t v = FIX2LONG(r.value);
    *out = (v > 0) - (v < 0);
    return RESULT_OK(KORB_NIL);
}

/* ---- merge sort whose comparisons may run Ruby -------------------------------
 * Insertion sorts here were O(n^2) (sort_by { rand } on 20k elements: 3 s).  The
 * order lives in a libc index permutation, so a comparison that yields / GCs
 * moves nothing it depends on: both operands are re-read from the rooted array
 * before every call, and the array is rewritten once at the end. */
/* Stable (ties keep input order): the permutation of *aref's indices, or NULL
 * after a raise (*err).  The caller frees it.  Uses slots[0..1] + the callee's. */
static uint32_t *korb_ary_msort_perm(CTX *c, korb_value *slots, korb_ref aref, korb_msort_cmp_fn cmp,
                                     const void *ud, RESULT *err) {
    const uint32_t n = VAL2ARY(KORB_REF_GET(aref))->len;
    uint32_t *const perm = malloc(sizeof(uint32_t) * 2u * (n ? n : 1));
    if (!perm) { *err = korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory"); return NULL; }
    uint32_t *src = perm, *dst = perm + n;
    for (uint32_t i = 0; i < n; i++) src[i] = i;
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            const uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint32_t i = lo, j = mid, o = lo;
            while (i < mid && j < hi) {
                const KorbArray *const ar = VAL2ARY(KORB_REF_GET(aref));   /* re-read: the last compare may have GC'd */
                slots[0] = korb_ary_ptr(ar)[src[i]];
                slots[1] = korb_ary_ptr(ar)[src[j]];
                int r = 0;
                const RESULT cr = cmp(c, slots + 2, slots[0], slots[1], ud, &r);
                if (UNLIKELY(cr.state != KORB_NORMAL)) { free(perm); *err = cr; return NULL; }
                dst[o++] = (r <= 0) ? src[i++] : src[j++];
            }
            while (i < mid) dst[o++] = src[i++];
            while (j < hi) dst[o++] = src[j++];
        }
        uint32_t *const t = src; src = dst; dst = t;
    }
    if (src != perm) memcpy(perm, src, sizeof(uint32_t) * n);
    return perm;
}
/* Reorder *aref by perm (from korb_ary_msort_perm on an array of the same length).
 * Nothing here allocates on the GC heap. */
static bool korb_ary_apply_perm(CTX *c, korb_ref aref, const uint32_t *const perm) {
    KorbArray *const ar = VAL2ARY(KORB_REF_GET(aref));
    const uint32_t n = ar->len;
    korb_value *const snap = malloc(sizeof(korb_value) * (n ? n : 1));
    if (!snap) return false;
    memcpy(snap, korb_ary_ptr(ar), sizeof(korb_value) * n);
    for (uint32_t i = 0; i < n; i++) ARO_STORE(c, korb_ary_owner(ar), &korb_ary_ptr(ar)[i], snap[perm[i]]);
    free(snap);
    return true;
}
/* Sort the rooted *aref in place (stable). */
static RESULT korb_ary_msort(CTX *c, korb_value *slots, korb_ref aref, korb_msort_cmp_fn cmp, const void *ud) {
    RESULT err = RESULT_OK(KORB_NIL);
    uint32_t *const perm = korb_ary_msort_perm(c, slots, aref, cmp, ud, &err);
    if (!perm) return err;
    const bool ok = korb_ary_apply_perm(c, aref, perm);
    free(perm);
    return ok ? RESULT_OK(KORB_NIL) : korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory");
}
/* comparators */
static RESULT korb_msort_cmp_block(CTX *c, korb_value *slots, korb_value a, korb_value b, const void *ud, int *out) {
    const struct korb_msort_blk *const k = ud;
    return korb_cmp_block(c, slots, a, b, k->block, k->def_env, k->cself, out);
}
static RESULT korb_msort_cmp_spaceship(CTX *c, korb_value *slots, korb_value a, korb_value b, const void *ud, int *out) {
    (void)ud;
    if (UNLIKELY(korb_eq_needs_send(a) || korb_eq_needs_send(b) || KORB_CLASS_P(a) || KORB_CLASS_P(b)))
        return korb_cmp_spaceship(c, slots, a, b, out);
    const int r = korb_cmp_full(c, a, b);
    if (UNLIKELY(r == 2)) return korb_raise_cmperr(c, slots, 0, a, b, false);
    *out = r;
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_sort(CTX *c, korb_value *slots, korb_ref self, korb_slice a,
                              NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    korb_ref dst = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        korb_value e = korb_ary_ptr(SELF_ARY)[i];
        CHECK(korb_ary_push_val(c, slots + 2, dst, e));   /* scratch above key slot */
    }
    KorbArray *d = VAL2ARY(KORB_REF_GET(dst));
    if (block == NULL) {
        /* default <=> : O(n log n) qsort.  korb_cmp_full is GC-free for builtin
         * types so the items pointer stays valid; reordering existing elements
         * creates no new heap edges (all already tracked) → no write barrier. */
        korb_value *const dd0 = korb_ary_ptr(d);
        const uint32_t dn = d->len;
        bool all_fix = true, has_obj = false;
        for (uint32_t i = 0; i < dn; i++) {
            const korb_value e = dd0[i];
            if (!FIXNUM_P(e)) all_fix = false;
            if (KORB_OBJECT_P(e) || KORB_CLASS_P(e)) { has_obj = true; break; }   /* user object / class (Module#<=>, Rails::Railtie.<=>) → dispatch */
        }
        if (all_fix) {                      /* homogeneous Fixnum → callback-free typed sort */
            if (dn > 1) korb_fix_qsort(dd0, 0, (korb_sword_t)dn - 1);
            return RESULT_OK(KORB_REF_GET(dst));
        }
        if (has_obj) {                     /* user/Comparable objects: <=> per compare (may GC) */
            CHECK(korb_ary_msort(c, slots + 1, dst, korb_msort_cmp_spaceship, NULL));
            return RESULT_OK(KORB_REF_GET(dst));
        }
        struct korb_sortctx sc = { c, 0, KORB_NIL, KORB_NIL };
        qsort_r(korb_ary_ptr(d), d->len, sizeof(korb_value), korb_sort_cmp, &sc);
        /* not comparable by value (arrays holding objects with their own <=>, e.g. [Date, name]):
         * sort again dispatching <=>, which also raises CRuby's error for a truly incomparable pair */
        if (UNLIKELY(sc.err)) {
            if (!korb_eq_needs_send(sc.bad_l) && !korb_eq_needs_send(sc.bad_r))
                return korb_raise_cmperr(c, slots, 0, sc.bad_l, sc.bad_r, false);
            CHECK(korb_ary_msort(c, slots + 1, dst, korb_msort_cmp_spaceship, NULL));
        }
        return RESULT_OK(KORB_REF_GET(dst));
    }
    {   /* comparator block: each compare yields (may GC) */
        const struct korb_msort_blk k = { block, def_env, cself };
        CHECK(korb_ary_msort(c, slots + 1, dst, korb_msort_cmp_block, &k));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}

/* sort_by: build parallel value+key arrays via the block, insertion-sort by key. */
static RESULT korb_m_ary_sort_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#sort_by");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        korb_ref vals = KORB_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    korb_ref keys = KORB_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;                              /* root key */
        CHECK(korb_ary_push_val(c, slots + 4, vals, slots[2]));
        CHECK(korb_ary_push_val(c, slots + 4, keys, slots[3]));
    }
    {   /* scalar keys: stable merge sort of an index permutation — korb_cmp_full allocates
         * nothing, so no GC can move the arrays until the write-back below */
        KorbArray *const vd = VAL2ARY(KORB_REF_GET(vals)), *const kd = VAL2ARY(KORB_REF_GET(keys));
        const uint32_t n = vd->len;
        const korb_value *const kdat = korb_ary_ptr(kd);
        uint32_t *const perm = malloc(sizeof(uint32_t) * 2u * (n ? n : 1));
        if (!perm) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory");
        uint32_t *src = perm, *dst = perm + n;
        for (uint32_t i = 0; i < n; i++) src[i] = i;
        bool bail = false;
        for (uint32_t w = 1; w < n && !bail; w *= 2) {
            for (uint32_t lo = 0; lo < n && !bail; lo += 2 * w) {
                const uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
                uint32_t i = lo, j = mid, o = lo;
                while (i < mid && j < hi) {
                    const int cmp = korb_cmp_full(c, kdat[src[i]], kdat[src[j]]);
                    if (UNLIKELY(cmp == 2)) { bail = true; break; }   /* not comparable by value: <=> below */
                    dst[o++] = (cmp <= 0) ? src[i++] : src[j++];      /* ties keep input order */
                }
                if (bail) break;
                while (i < mid) dst[o++] = src[i++];
                while (j < hi) dst[o++] = src[j++];
            }
            uint32_t *const t = src; src = dst; dst = t;
        }
        if (bail) { free(perm); goto dispatch_sort; }
        korb_value *const old = malloc(sizeof(korb_value) * 2u * (n ? n : 1));   /* libc copy of both columns */
        if (!old) { free(perm); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory"); }
        memcpy(old, korb_ary_ptr(vd), sizeof(korb_value) * n);
        memcpy(old + n, kdat, sizeof(korb_value) * n);
        void *const vit = korb_ary_owner(vd), *const kit = korb_ary_owner(kd);
        korb_value *const vdat = korb_ary_ptr(vd), *const kdw = korb_ary_ptr(kd);
        for (uint32_t i = 0; i < n; i++) {
            ARO_STORE(c, vit, &vdat[i], old[src[i]]);
            ARO_STORE(c, kit, &kdw[i], old[n + src[i]]);
        }
        free(old);
        free(perm);
        return RESULT_OK(KORB_REF_GET(vals));
    }

  dispatch_sort:;   /* a key needs <=> dispatch (Comparable user object): sort the keys'
                     * indices, then reorder both columns the same way */
    {
        RESULT err = RESULT_OK(KORB_NIL);
        uint32_t *const perm = korb_ary_msort_perm(c, slots + 2, keys, korb_msort_cmp_spaceship, NULL, &err);
        if (!perm) return err;
        const bool ok = korb_ary_apply_perm(c, vals, perm) && korb_ary_apply_perm(c, keys, perm);
        free(perm);
        if (!ok) return korb_raise(c, slots + 2, KORB_E_RUNTIME, 0, "out of memory");
        return RESULT_OK(KORB_REF_GET(vals));
    }
}
/* self.to_enum(:meth) — sized twin below; used where CRuby has no size fn. */
static RESULT korb_ary_to_enum(CTX *c, korb_value *slots, korb_ref self, const char *meth);

/* sort_by!: sort in place by block key (sort_by then copy back into self). */
static RESULT korb_m_ary_sort_by_bang(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    /* No block → an Enumerator; the frozen check happens when it is driven. */
    if (UNLIKELY(block == NULL)) return korb_ary_to_enum(c, slots, self, "sort_by!");
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));   /* CRuby raises FrozenError upfront, even on an empty array */
    RESULT sr = korb_m_ary_sort_by(c, slots, self, a, block, def_env, cself);   /* sorted copy at slots[0] */
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[0] = sr.value;                                  /* root the sorted array */
    korb_ref sorted = KORB_REF_AT(&slots[0]);
    VAL2ARY(KORB_REF_GET(self))->len = 0;
    uint32_t n = VAL2ARY(KORB_REF_GET(sorted))->len;
    for (uint32_t i = 0; i < n; i++) {
        korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(sorted)))[i];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(KORB_REF_GET(self));
}
/* self.to_enum(:meth) — for no-block enumerator returns. */
static RESULT korb_ary_to_enum(CTX *c, korb_value *slots, korb_ref self, const char *meth) {
    slots[0] = KORB_REF_GET(self);
    slots[1] = ID2SYM(korb_intern(c->vm, meth, (uint32_t)strlen(meth)));
    return korb_send_impl(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1, NULL, NULL, NULL);
}
/* self.to_enum(:meth[, arg]) — the predicate-driven methods (find/rfind/rindex/
 * bsearch/…) have no size function in CRuby, and find's `ifnone` has to be
 * replayed when the enumerator is driven.  `arg` is KORB_NIL when absent. */
static RESULT korb_ary_to_enum_unsized(CTX *c, korb_value *slots, korb_ref self, const char *meth, korb_slice a) {
    const bool has_arg = KORB_SLICE_LEN(a) > 0;
    slots[0] = KORB_REF_GET(self);
    slots[1] = ID2SYM(korb_intern(c->vm, meth, (uint32_t)strlen(meth)));
    if (has_arg) slots[2] = KORB_SLICE_GET(a, 0);
    return korb_send_impl(c, slots + (has_arg ? 3 : 2), korb_intern_lit(c->vm, "to_enum", 7), 0, has_arg ? 2 : 1, NULL, NULL, NULL);
}
/* min_by(want=-1) / max_by(want=1): element with the extreme block key. */
static RESULT korb_ary_minmax_by(CTX *c, korb_value *slots, korb_ref self, NODE *block, korb_value *def_env, korb_value *cself, int want) {
    if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "min_by", 6)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    slots[0] = KORB_NIL;   /* best value */
    slots[1] = KORB_NIL;   /* best key */
    bool have = false;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;
        if (!have) { slots[0] = slots[2]; slots[1] = slots[3]; have = true; continue; }
        int cmp = korb_cmp_full(c, slots[3], slots[1]);
        if (UNLIKELY(cmp == 2)) CHECK(korb_cmp_spaceship(c, slots + 4, slots[3], slots[1], &cmp));   /* user object key → <=> */
        if ((want < 0 && cmp < 0) || (want > 0 && cmp > 0)) { slots[0] = slots[2]; slots[1] = slots[3]; }
    }
    return RESULT_OK(slots[0]);
}
/* min_by(n)/max_by(n): sort by the block key (ascending), then take the first n
 * (min) or the last n reversed (max).  `want_max` picks the end + reversal. */
static RESULT korb_ary_minmax_by_n(CTX *c, korb_value *slots, korb_ref self, korb_value nv, NODE *block, korb_value *def_env, korb_value *cself, bool want_max) {
    korb_sword_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise_no_int(c, slots, nv);
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative size (%ld)", (long)n);
    RESULT sr = korb_m_ary_sort_by(c, slots, self, KORB_SLICE_MAKE(NULL, 0), block, def_env, cself);   /* ascending by key */
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[0] = sr.value;                                 /* sorted copy (rooted) */
    const uint32_t len = VAL2ARY(slots[0])->len;
    uint32_t take = (n < (korb_sword_t)len) ? (uint32_t)n : len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, take));
    korb_ref dst = KORB_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < take; i++) {
        const KorbArray *const s = VAL2ARY(slots[0]);    /* re-read after push (GC) */
        const korb_value e = want_max ? korb_ary_ptr(s)[len - 1 - i]   /* last n, largest first */
                                 : korb_ary_ptr(s)[i];            /* first n, smallest first */
        CHECK(korb_ary_push_val(c, slots + 2, dst, e));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_min_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (block == NULL) return korb_ary_to_enum(c, slots, self, "min_by");
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL) return korb_ary_minmax_by_n(c, slots, self, KORB_SLICE_GET(a, 0), block, def_env, cself, false);
    return korb_ary_minmax_by(c, slots, self, block, def_env, cself, -1);
}
static RESULT korb_m_ary_max_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (block == NULL) return korb_ary_to_enum(c, slots, self, "max_by");
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL) return korb_ary_minmax_by_n(c, slots, self, KORB_SLICE_GET(a, 0), block, def_env, cself, true);
    return korb_ary_minmax_by(c, slots, self, block, def_env, cself,  1);
}
static RESULT korb_m_ary_minmax_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    if (block == NULL) return korb_ary_to_enum(c, slots, self, "minmax_by");
    slots[0] = UNWRAP(korb_ary_minmax_by(c, slots, self, block, def_env, cself, -1));
    slots[1] = UNWRAP(korb_ary_minmax_by(c, slots + 1, self, block, def_env, cself, 1));
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(KORB_REF_GET(KORB_REF_AT(&slots[2])));
}
/* filter_map: collect block results that are truthy. */
static RESULT korb_m_ary_filter_map(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#filter_map");
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* flat_map: map then flatten one level (Array results spliced). */
static RESULT korb_m_ary_flat_map(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) {                          /* no block → Enumerator tagged op=3 so .with_index/.each flat-map */
        slots[0] = UNWRAP(korb_enum_desc(c, slots, KORB_REF_GET(self), "flat_map"));
        RESULT er = korb_enum_new(c, slots + 1, KORB_REF_GET(self), slots[0]);
        if (er.state == KORB_NORMAL) VAL2ENUM(er.value)->op = 3;
        return er;
    }
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;                              /* root result */
        if (!KORB_ARRAY_P(slots[0]) && KORB_OBJECT_P(slots[0])) {   /* non-Array result → try #to_ary */
            const uint32_t to_ary = korb_intern_lit(c->vm, "to_ary", 6);
            if (korb_responds_to(c, slots[0], to_ary)) {
                RESULT tr = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);   /* recv at slots[0] */
                if (UNLIKELY(tr.state != KORB_NORMAL)) return tr;
                if (UNLIKELY(tr.value != KORB_NIL && !KORB_ARRAY_P(tr.value)))
                    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Array (%s#to_ary gives %s)",
                                      korb_coerce_name(c, slots[0]), korb_coerce_name(c, slots[0]), korb_coerce_name(c, tr.value));
                if (KORB_ARRAY_P(tr.value)) slots[0] = tr.value;   /* nil → keep the original element as-is */
            }
        }
        if (KORB_ARRAY_P(slots[0])) {
            uint32_t m = VAL2ARY(slots[0])->len;
            for (uint32_t j = 0; j < m; j++) {
                korb_value e = korb_ary_ptr(VAL2ARY(slots[0]))[j];
                CHECK(korb_ary_push_val(c, slots + 1, dst, e));
            }
        } else {
            CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        }
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* partition: [truthy_elems, falsy_elems]. */
static RESULT korb_m_ary_partition(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#partition");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        korb_ref yes = KORB_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    korb_ref no  = KORB_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 3, KORB_TRUTHY(r.value) ? yes : no, slots[2]));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));    korb_ref out = KORB_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[1]));
    return RESULT_OK(KORB_REF_GET(out));
}
/* group_by: Hash{ block_key => [elems...] }. */
static RESULT korb_m_ary_group_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#group_by");
    slots[0] = UNWRAP(korb_hash_new(c, slots, 4));       korb_ref h = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[1] = korb_ary_ptr(ary)[i];                  /* element */
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[2] = r.value;                              /* key */
        RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots + 4, h, slots[2], &ferr);   /* CTX-aware (custom eql?/hash key) */
        if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
        if (idx < 0) {                                   /* new bucket array */
            slots[3] = UNWRAP(korb_ary_new(c, slots + 4, 4));
            CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_hash_set(c, slots + 4, h, KORB_REF_AT(&slots[2]), slots[3]));
        } else {
            korb_value bucket = korb_items_data(VAL2HASH(KORB_REF_GET(h))->items)[2 * idx + 1];
            slots[3] = bucket;
            CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[1]));
        }
    }
    return RESULT_OK(KORB_REF_GET(h));
}

/* grep(pat)(keep=1) / grep_v(pat)(keep=0): select by `pat === elem`, optional block map. */
static RESULT korb_ary_grep(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself, bool keep) {
    if (UNLIKELY(KORB_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    const bool restore_md = (block == NULL);                        /* CRuby leaves $~ alone unless a block runs */
    korb_ref saved_md = SLOTS_PUSH(slots, restore_md ? korb_re_get_lastmatch(c) : KORB_NIL);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_ary_ptr(ary)[i];                  /* root elem across yield */
        const korb_value _grep_pat = KORB_SLICE_GET(a, 0);
        bool _m;
        if (KORB_REGEXP_P(_grep_pat)) {
            slots[1] = _grep_pat;                                   /* root the pattern (a Regexp is movable) across the build */
            if (LIKELY(KORB_STRING_P(slots[0]) || SYMBOL_P(slots[0]))) {
                CHECK(korb_re_check_subject(c, slots + 2, slots[0]));   /* broken subject → ArgumentError, as Regexp#=== */
                _m = korb_re_caseeq_backref(c, slots + 2, KORB_REF_GET(KORB_REF_AT(&slots[1])), slots[0]);
            } else {                                                /* anything else: full Regexp#===, which coerces through #to_str */
                slots[2] = slots[0];
                const RESULT er = korb_send(c, slots + 3, c->vm->mid_eqq, 0, 1);
                if (UNLIKELY(er.state != KORB_NORMAL)) return er;
                _m = KORB_TRUTHY(er.value);
            }
        } else {
            CHECK(korb_pat_eq(c, slots + 1, _grep_pat, slots[0], &_m));
        }
        if (_m == keep) {
            if (block != NULL) {
                RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
            } else {
                CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
            }
        }
    }
    if (restore_md) korb_re_set_lastmatch(c, KORB_REF_GET(saved_md));
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_grep(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself)   { return korb_ary_grep(c, slots, self, a, block, def_env, cself, true); }
static RESULT korb_m_ary_grep_v(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) { return korb_ary_grep(c, slots, self, a, block, def_env, cself, false); }

static RESULT korb_m_ary_sort_bang(CTX *c, korb_value *slots, korb_ref self, korb_slice a,
                                   NODE *block, korb_value *def_env, korb_value *cself) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    /* sort a copy (O(n log n); dispatches <=> or yields as needed, which may GC) and adopt its order */
    RESULT sr = korb_m_ary_sort(c, slots, self, a, block, def_env, cself);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[0] = sr.value;
    KorbArray *const dst = VAL2ARY(KORB_REF_GET(self));
    void *const dstit = korb_ary_owner(dst); korb_value *const dstit_d = korb_ary_ptr(dst);
    const KorbArray *const src = VAL2ARY(slots[0]);
    for (uint32_t i = 0; i < dst->len && i < src->len; i++)
        ARO_STORE(c, dstit, &dstit_d[i], korb_ary_ptr(src)[i]);
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_ary_tally(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (KORB_SLICE_LEN(a) >= 1 && KORB_HASH_P(KORB_SLICE_GET(a, 0)))
        slots[0] = KORB_SLICE_GET(a, 0);              /* tally(hash): accumulate into it */
    else
        slots[0] = UNWRAP(korb_hash_new(c, slots, 4));
    korb_ref h = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[1] = korb_ary_ptr(ary)[i];                 /* elem (root) */
        RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots + 2, h, slots[1], &ferr);   /* CTX-aware key */
        if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
        korb_sword_t cnt = idx < 0 ? 0 : FIX2LONG(korb_items_data(VAL2HASH(KORB_REF_GET(h))->items)[2*idx+1]);
        CHECK(korb_hash_set(c, slots + 2, h, KORB_REF_AT(&slots[1]), LONG2FIX(cnt + 1)));
    }
    return RESULT_OK(KORB_REF_GET(h));
}
/* Join `aref`'s array into `ms`, recursing into nested arrays, with each non-array
 * element rendered via #to_s — dispatched for user objects (which can GC), so the
 * array is re-read each step and cycle detection uses the KORB_FL_JOIN_VISITING
 * header flag (survives GC moves) rather than raw pointers.  Returns RAISE on a
 * recursive-array cycle or a propagated #to_s error.  `sepref` roots the (String
 * or nil) separator; `slots` is a fresh rooted region. */
/* Merge one element's encoding into the join result's: an ASCII-only element
 * never forces one, two different real ones give up (UINT32_MAX = keep UTF-8). */
#define KORB_JOIN_ENC_NONE (UINT32_MAX - 1u)   /* no non-ASCII element seen yet */
/* Fold one element into the running result encoding, exactly as rb_str_append
 * would: the result starts US-ASCII and widens to the first non-7-bit
 * element's encoding.  UINT32_MAX marks an incompatible pair (CompatibilityError). */
static void korb_join_enc_merge(CTX *c, korb_value str, uint32_t *enc, bool *asc, uint32_t bad[2]) {
    if (*enc == UINT32_MAX) return;
    /* CRuby appends into a copy of the FIRST element, so that element's encoding
     * seeds the result and a later 7-bit piece never changes it. */
    if (*enc == KORB_JOIN_ENC_NONE) {
        *enc = KORB_STR_ENC(str);
        *asc = korb_str_ascii_only_p(c->vm, str);
        return;
    }
    if (korb_str_ascii_only_p(c->vm, str)) return;      /* 7-bit: fits any destination */
    const uint32_t e = KORB_STR_ENC(str);
    if (e == *enc) { *asc = false; return; }
    if (*asc) { *enc = e; *asc = false; return; }       /* nothing non-7-bit yet → adopt */
    bad[0] = *enc; bad[1] = e; *enc = UINT32_MAX;
}
/* `enc`: the negotiated result encoding (KORB_ENC_USASCII until a non-ASCII-only
 * element sets it; UINT32_MAX once two incompatible ones meet). */
static RESULT korb_join_rec(CTX *c, korb_value *slots, FILE *ms, korb_ref aref, korb_ref sepref, bool *first, uint32_t *enc, bool *asc, uint32_t bad[2]) {
    if (((AroObjectHeader *)(uintptr_t)KORB_REF_GET(aref))->flags & KORB_FL_JOIN_VISITING)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "recursive array join");
    ((AroObjectHeader *)(uintptr_t)KORB_REF_GET(aref))->flags |= KORB_FL_JOIN_VISITING;
    RESULT rr = RESULT_OK(KORB_NIL);
    for (uint32_t i = 0; i < VAL2ARY(KORB_REF_GET(aref))->len; i++) {
        slots[0] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(aref)))[i];   /* root the element (re-read: a prior #to_s may have moved the array) */
        /* Element order per CRuby: an Array (or an object with #to_ary but no
         * #to_str) recurses; otherwise #to_str, then #to_s.  The recursion happens
         * before the separator so a nested array isn't given a leading one.  The
         * #to_str/#to_ary probes honor #respond_to_missing? (proxies/mocks); the
         * element stays rooted in slots[0] across those dispatches. */
        const uint32_t to_str_id = korb_intern_lit(c->vm, "to_str", 6), to_ary_id = korb_intern_lit(c->vm, "to_ary", 6);
        /* Resolve the element to either an array (recurse, no separator) or a
         * scalar String (write with separator), following CRuby's #to_str →
         * #to_ary → #to_s chain where a nil conversion falls through to the next.
         * The receiver stays at slots[0]; `scalar` is captured last (no GC before
         * the fwrite).  korb_send_impl takes the element as receiver via slots+1. */
        korb_value scalar = KORB_NIL;
        bool recurse = KORB_ARRAY_P(slots[0]);
        if (!recurse && KORB_OBJECT_P(slots[0])) {
            if (korb_responds_to_coerce(c, slots + 1, slots[0], to_str_id)) {   /* #to_str: String, else fall through */
                RESULT r = korb_send_impl(c, slots + 1, to_str_id, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) { rr = r; goto done; }
                if (KORB_STRING_P(r.value)) scalar = r.value;
            }
            if (scalar == KORB_NIL && korb_responds_to_coerce(c, slots + 1, slots[0], to_ary_id)) {   /* #to_ary: Array → recurse */
                RESULT r = korb_send_impl(c, slots + 1, to_ary_id, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) { rr = r; goto done; }
                if (KORB_ARRAY_P(r.value)) { slots[0] = r.value; recurse = true; }
            }
            if (!recurse && scalar == KORB_NIL) {                       /* #to_s last */
                RESULT r = korb_send_impl(c, slots + 1, korb_intern_lit(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) { rr = r; goto done; }
                if (KORB_STRING_P(r.value)) scalar = r.value;           /* non-String → fprint below */
            }
        }
        if (recurse) {                                                 /* a nested array is ONE element: [[], []].join("-") is "-" */
            if (!*first && KORB_STRING_P(KORB_REF_GET(sepref))) {
                korb_join_enc_merge(c, KORB_REF_GET(sepref), enc, asc, bad);
                const KorbString *const sep = VAL2STR(KORB_REF_GET(sepref));
                fwrite(korb_strbuf_data(sep->buf), 1, sep->len, ms);
            }
            bool inner_first = true;
            rr = korb_join_rec(c, slots + 1, ms, KORB_REF_AT(&slots[0]), sepref, &inner_first, enc, asc, bad);
            if (UNLIKELY(rr.state != KORB_NORMAL)) goto done;
            *first = false;
            continue;
        }
        if (!*first && KORB_STRING_P(KORB_REF_GET(sepref))) {
            korb_join_enc_merge(c, KORB_REF_GET(sepref), enc, asc, bad);
            const KorbString *const sep = VAL2STR(KORB_REF_GET(sepref));
            fwrite(korb_strbuf_data(sep->buf), 1, sep->len, ms);
        }
        const korb_value sv = KORB_STRING_P(scalar) ? scalar : (KORB_STRING_P(slots[0]) ? slots[0] : KORB_NIL);
        if (sv != KORB_NIL) {
            korb_join_enc_merge(c, sv, enc, asc, bad);
            fwrite(korb_strbuf_data(VAL2STR(sv)->buf), 1, VAL2STR(sv)->len, ms);   /* String element / #to_str result */
        }
        else korb_fprint_to_s_s(c, slots + 1, ms, slots[0]);           /* immediate / builtin / non-String #to_s (slots: a Class may override it) */
        *first = false;
    }
  done:
    ((AroObjectHeader *)(uintptr_t)KORB_REF_GET(aref))->flags &= ~(uint32_t)KORB_FL_JOIN_VISITING;   /* re-read (may have moved) */
    return rr;
}
static RESULT korb_m_ary_join(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    /* CRuby warns about a non-nil $, before it looks at self, so `[].join` warns too */
    if ((KORB_SLICE_LEN(a) == 0 || KORB_SLICE_GET(a, 0) == KORB_NIL) &&
        KORB_STRING_P(korb_const_get(c->vm, korb_intern_lit(c->vm, "$,", 2))))
        korb_warn(c, slots, "$, is set to non-nil value");
    if (SELF_ARY->len == 0) {                                       /* [].join(anything) → "" (sep not validated) */
        RESULT e = korb_str_new(c, slots, "", 0);
        if (LIKELY(e.state == KORB_NORMAL)) KORB_STR_ENC_SET(e.value, KORB_ENC_USASCII);   /* CRuby starts at US-ASCII */
        return e;
    }
    /* coerced separator parked in slots[0] so it survives the per-element to_s allocs */
    slots[0] = KORB_NIL;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL) {
        korb_value sv = KORB_SLICE_GET(a, 0);
        if (UNLIKELY(!KORB_STRING_P(sv))) {                         /* coerce a #to_str separator */
            const uint32_t to_str = korb_intern_lit(c->vm, "to_str", 6);
            if (KORB_OBJECT_P(sv) && korb_responds_to_coerce_p(c, slots, &sv, to_str)) {
                slots[0] = sv;
                const RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                sv = sr.value;
            }
            if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, KORB_SLICE_GET(a, 0)));
        }
        slots[0] = sv;                                              /* the (possibly coerced) String sep */
    }
    if (slots[0] == KORB_NIL) {                                    /* no explicit separator → fall back to $, */
        const korb_value ofs = korb_const_get(c->vm, korb_intern_lit(c->vm, "$,", 2));
        if (KORB_STRING_P(ofs)) slots[0] = ofs;                    /* warned above */
    }
    {   /* all elements Strings (the usual case): size it, allocate once and copy — no stdio stream, no dispatch */
        const KorbArray *const ar = SELF_ARY;
        const korb_value *const it = korb_ary_ptr(ar);
        const uint32_t seplen = KORB_STRING_P(slots[0]) ? VAL2STR(slots[0])->len : 0;
        uint64_t total = (uint64_t)seplen * (ar->len - 1);
        uint32_t i = 0;
        for (; i < ar->len && KORB_STRING_P(it[i]); i++) total += VAL2STR(it[i])->len;
        if (i == ar->len && total < (1u << 31)) {
            uint32_t jenc = KORB_JOIN_ENC_NONE, jbad[2] = {0, 0};
            bool jasc = true;
            for (uint32_t k = 0; k < ar->len; k++) {             /* the order korb_join_rec merges them in */
                if (k != 0 && seplen != 0) korb_join_enc_merge(c, slots[0], &jenc, &jasc, jbad);
                korb_join_enc_merge(c, it[k], &jenc, &jasc, jbad);
            }
            if (UNLIKELY(jenc == UINT32_MAX)) return korb_raise_enc_compat(c, slots + 1, jbad[0], jbad[1]);
            slots[1] = KORB_REF_GET(self);
            KorbString *const out = korb_str_alloc(c, slots + 2, (uint32_t)total);
            const KorbArray *const a2 = VAL2ARY(slots[1]);       /* re-read after the alloc */
            const korb_value *const it2 = korb_ary_ptr(a2);
            char *dst = korb_strbuf_data(out->buf);
            for (uint32_t k = 0; k < a2->len; k++) {
                if (k != 0 && seplen != 0) { memcpy(dst, korb_strbuf_data(VAL2STR(slots[0])->buf), seplen); dst += seplen; }
                const KorbString *const e = VAL2STR(it2[k]);
                memcpy(dst, korb_strbuf_data(e->buf), e->len); dst += e->len;
            }
            if (jenc != KORB_JOIN_ENC_NONE) KORB_STR_ENC_SET((korb_value)out, jenc);
            return RESULT_OK((korb_value)out);
        }
    }
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    slots[1] = KORB_REF_GET(self);                                 /* root the array across element #to_s GC */
    bool first = true;
    /* slots[0] = separator (String|nil), slots[1] = self; join drives from slots+2.
     * The memstream buffer is C heap, unaffected by GC during #to_s dispatch. */
    uint32_t jenc = KORB_JOIN_ENC_NONE, jbad[2] = {0, 0};
    bool jasc = true;
    /* the separator participates, but only after the first element has seeded
       the encoding — it is appended between elements, never in front */
    RESULT jr = korb_join_rec(c, slots + 2, ms, KORB_REF_AT(&slots[1]), KORB_REF_AT(&slots[0]), &first, &jenc, &jasc, jbad);
    fclose(ms);
    if (UNLIKELY(jr.state != KORB_NORMAL)) { free(buf); return jr; }
    if (UNLIKELY(jenc == UINT32_MAX)) { free(buf); return korb_raise_enc_compat(c, slots + 2, jbad[0], jbad[1]); }
    RESULT r = korb_str_new(c, slots + 2, buf ? buf : "", (uint32_t)sz);
    free(buf);
    if (LIKELY(r.state == KORB_NORMAL) && jenc != KORB_JOIN_ENC_NONE)
        KORB_STR_ENC_SET(r.value, jenc);
    return r;
}

/* recursive permutation builder: append each `want`-length position-permutation
 * of `self` (as a fresh array) into `out`.  `cur` is the shared work array; out/
 * cur are rooted by the caller; `scratch` is a fresh cursor above them. */
static RESULT korb_perm_rec(CTX *c, korb_value *scratch, korb_ref self, korb_ref out, korb_ref cur,
                            uint32_t want, bool *used, uint32_t depth) {
    if (depth == want) {
        korb_ref copy = SLOTS_PUSH(scratch, UNWRAP(korb_ary_new(c, scratch, want)));   /* copy cur */
        for (uint32_t k = 0; k < VAL2ARY(KORB_REF_GET(cur))->len; k++)
            CHECK(korb_ary_push_val(c, scratch + 1, copy, korb_ary_ptr(VAL2ARY(KORB_REF_GET(cur)))[k]));
        return korb_ary_push_val(c, scratch + 1, out, KORB_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(KORB_REF_GET(self))->len;
    for (uint32_t i = 0; i < len; i++) {
        if (used[i]) continue;
        used[i] = true;
        CHECK(korb_ary_push_val(c, scratch, cur, korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i]));
        CHECK(korb_perm_rec(c, scratch, self, out, cur, want, used, depth + 1));
        KorbArray *cv = VAL2ARY(KORB_REF_GET(cur)); cv->len--;     /* pop */
        used[i] = false;
    }
    return RESULT_OK(KORB_NIL);
}
/* CRuby's blockless permutation/combination Enumerator re-reads the array when
 * it RUNS, so a later mutation shows through; handing it the tuples built now
 * freezes them.  Only #size is settled here, from the count just computed. */
static RESULT korb_ary_enum_for(CTX *c, korb_value *slots, korb_ref self, korb_slice a,
                                const char *meth, korb_sword_t size) {
    const bool has_arg = KORB_SLICE_LEN(a) > 0;
    const korb_value arg = has_arg ? KORB_SLICE_GET(a, 0) : KORB_NIL;
    slots[0] = KORB_REF_GET(self);
    slots[1] = ID2SYM(korb_intern(c->vm, meth, (uint32_t)strlen(meth)));
    if (has_arg) slots[2] = arg;
    const RESULT er = korb_send(c, slots + (has_arg ? 3 : 2), korb_intern_lit(c->vm, "to_enum", 7), 0, has_arg ? 2 : 1);
    if (UNLIKELY(er.state != KORB_NORMAL)) return er;
    slots[0] = er.value;
    slots[1] = LONG2FIX(size);
    return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__set_size", 10), 0, 1);
}
static RESULT korb_m_ary_permutation(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    uint32_t len = SELF_ARY->len;
    korb_sword_t want = len;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL) {
        { korb_value _iv = KORB_SLICE_GET(a, 0); if (UNLIKELY(!korb_to_index(_iv, &want))) { RESULT _cr = korb_coerce_to_int(c, slots, &_iv); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; if (!korb_to_index(_iv, &want)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); len = SELF_ARY->len; } }
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                   /* out: array of permutations */
    korb_ref out = KORB_REF_AT(&slots[0]);
    if (want >= 0 && (uint32_t)want <= len) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)want));   /* cur work array */
        korb_ref cur = KORB_REF_AT(&slots[1]);
        bool *used = calloc(len ? len : 1, sizeof(bool));
        if (!used) { fprintf(stderr, "koruby_precise: oom (permutation)\n"); abort(); }
        RESULT rr = korb_perm_rec(c, slots + 2, self, out, cur, (uint32_t)want, used, 0);
        free(used);
        if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    }
    if (block == NULL)
        return korb_ary_enum_for(c, slots + 2, self, a, "permutation", VAL2ARY(KORB_REF_GET(out))->len);
    for (uint32_t i = 0; i < VAL2ARY(KORB_REF_GET(out))->len; i++) {
        korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(out)))[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &e, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(KORB_REF_GET(self));
}
/* recursive combination builder: append each `want`-length combination (indices
 * chosen in increasing order from `start`) of self into `out`. */
static RESULT korb_comb_rec(CTX *c, korb_value *scratch, korb_ref self, korb_ref out, korb_ref cur,
                            uint32_t want, uint32_t start, uint32_t depth) {
    if (depth == want) {
        korb_ref copy = SLOTS_PUSH(scratch, UNWRAP(korb_ary_new(c, scratch, want)));
        for (uint32_t k = 0; k < VAL2ARY(KORB_REF_GET(cur))->len; k++)
            CHECK(korb_ary_push_val(c, scratch + 1, copy, korb_ary_ptr(VAL2ARY(KORB_REF_GET(cur)))[k]));
        return korb_ary_push_val(c, scratch + 1, out, KORB_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(KORB_REF_GET(self))->len;
    for (uint32_t i = start; i < len; i++) {
        CHECK(korb_ary_push_val(c, scratch, cur, korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i]));
        CHECK(korb_comb_rec(c, scratch, self, out, cur, want, i + 1, depth + 1));
        KorbArray *cv = VAL2ARY(KORB_REF_GET(cur)); cv->len--;     /* pop */
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_combination(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    uint32_t len = SELF_ARY->len;
    korb_sword_t want = 0;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL) {
        { korb_value _iv = KORB_SLICE_GET(a, 0); if (UNLIKELY(!korb_to_index(_iv, &want))) { RESULT _cr = korb_coerce_to_int(c, slots, &_iv); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; if (!korb_to_index(_iv, &want)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); len = SELF_ARY->len; } }
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                   /* out: array of combinations */
    korb_ref out = KORB_REF_AT(&slots[0]);
    if (want >= 0 && (uint32_t)want <= len) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)want));   /* cur work array */
        korb_ref cur = KORB_REF_AT(&slots[1]);
        CHECK(korb_comb_rec(c, slots + 2, self, out, cur, (uint32_t)want, 0, 0));
    }
    if (block == NULL)
        return korb_ary_enum_for(c, slots + 2, self, a, "combination", VAL2ARY(KORB_REF_GET(out))->len);
    for (uint32_t i = 0; i < VAL2ARY(KORB_REF_GET(out))->len; i++) {
        korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(out)))[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &e, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(KORB_REF_GET(self));
}
/* repeated_combination: like combination but indices may repeat (i, not i+1). */
static RESULT korb_rcomb_rec(CTX *c, korb_value *scratch, korb_ref self, korb_ref out, korb_ref cur,
                             uint32_t want, uint32_t start, uint32_t depth) {
    if (depth == want) {
        korb_ref copy = SLOTS_PUSH(scratch, UNWRAP(korb_ary_new(c, scratch, want)));
        for (uint32_t k = 0; k < VAL2ARY(KORB_REF_GET(cur))->len; k++)
            CHECK(korb_ary_push_val(c, scratch + 1, copy, korb_ary_ptr(VAL2ARY(KORB_REF_GET(cur)))[k]));
        return korb_ary_push_val(c, scratch + 1, out, KORB_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(KORB_REF_GET(self))->len;
    for (uint32_t i = start; i < len; i++) {
        CHECK(korb_ary_push_val(c, scratch, cur, korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i]));
        CHECK(korb_rcomb_rec(c, scratch, self, out, cur, want, i, depth + 1));   /* i: allow reuse */
        VAL2ARY(KORB_REF_GET(cur))->len--;
    }
    return RESULT_OK(KORB_NIL);
}
/* repeated_permutation: all length-`want` tuples with repetition (len^want). */
static RESULT korb_rperm_rec(CTX *c, korb_value *scratch, korb_ref self, korb_ref out, korb_ref cur,
                             uint32_t want, uint32_t depth) {
    if (depth == want) {
        korb_ref copy = SLOTS_PUSH(scratch, UNWRAP(korb_ary_new(c, scratch, want)));
        for (uint32_t k = 0; k < VAL2ARY(KORB_REF_GET(cur))->len; k++)
            CHECK(korb_ary_push_val(c, scratch + 1, copy, korb_ary_ptr(VAL2ARY(KORB_REF_GET(cur)))[k]));
        return korb_ary_push_val(c, scratch + 1, out, KORB_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(KORB_REF_GET(self))->len;
    for (uint32_t i = 0; i < len; i++) {
        CHECK(korb_ary_push_val(c, scratch, cur, korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i]));
        CHECK(korb_rperm_rec(c, scratch, self, out, cur, want, depth + 1));
        VAL2ARY(KORB_REF_GET(cur))->len--;
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_repeated(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself, bool perm) {
    korb_sword_t want = 0;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL) {
        { korb_value _iv = KORB_SLICE_GET(a, 0); if (UNLIKELY(!korb_to_index(_iv, &want))) { RESULT _cr = korb_coerce_to_int(c, slots, &_iv); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; if (!korb_to_index(_iv, &want)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); } }
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                   /* out */
    korb_ref out = KORB_REF_AT(&slots[0]);
    if (want >= 0) {                                                /* want<0 → empty (size 0) */
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)want));   /* cur work array */
        korb_ref cur = KORB_REF_AT(&slots[1]);
        CHECK(perm ? korb_rperm_rec(c, slots + 2, self, out, cur, (uint32_t)want, 0)
                   : korb_rcomb_rec(c, slots + 2, self, out, cur, (uint32_t)want, 0, 0));
    }
    if (block == NULL)
        return korb_ary_enum_for(c, slots + 2, self, a,
                                 perm ? "repeated_permutation" : "repeated_combination",
                                 VAL2ARY(KORB_REF_GET(out))->len);
    for (uint32_t i = 0; i < VAL2ARY(KORB_REF_GET(out))->len; i++) {
        korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(out)))[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &e, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_ary_repeated_combination(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) { return korb_m_ary_repeated(c, slots, self, a, block, def_env, cself, false); }
static RESULT korb_m_ary_repeated_permutation(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) { return korb_m_ary_repeated(c, slots, self, a, block, def_env, cself, true); }
/* Array#to_h — elements (or block results) must be 2-element arrays → Hash. */
static RESULT korb_m_ary_to_h(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, SELF_ARY->len));
    korb_ref dst = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[1] = korb_ary_ptr(ary)[i];                        /* element / pair */
        if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[1] = r.value;
        }
        if (UNLIKELY(!KORB_ARRAY_P(slots[1]))) {               /* coerce a pair-like element via #to_ary */
            const uint32_t to_ary = korb_intern_lit(c->vm, "to_ary", 6);
            if (!korb_responds_to_coerce(c, slots + 2, slots[1], to_ary))
                return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "wrong element type %s at %u (expected array)", korb_coerce_name(c, slots[1]), i);
            RESULT ar = korb_send_impl(c, slots + 2, to_ary, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
            if (UNLIKELY(!KORB_ARRAY_P(ar.value)))
                return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "wrong element type %s at %u (expected array)", korb_coerce_name(c, slots[1]), i);
            slots[1] = ar.value;
        }
        if (UNLIKELY(VAL2ARY(slots[1])->len != 2))
            return korb_raise(c, slots + 2, KORB_E_ARGUMENT, 0, "wrong array length at %u (expected 2, was %u)", i, VAL2ARY(slots[1])->len);
        slots[2] = korb_ary_ptr(VAL2ARY(slots[1]))[0];          /* key */
        korb_value val = korb_ary_ptr(VAL2ARY(slots[1]))[1];
        CHECK(korb_hash_set(c, slots + 3, dst, KORB_REF_AT(&slots[2]), val));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* Array#cycle([n]) — yield elements n times (forever if n omitted); → nil. */
static RESULT korb_m_ary_cycle(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    const bool bounded = KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL;
    korb_sword_t n = 0;
    if (bounded) {
        korb_value nv = KORB_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(nv, &n))) {          /* coerce count via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &nv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(nv, &n)) return korb_raise_no_int(c, slots, nv);
        }
    }
    if (block == NULL) {
        if (bounded) {                                  /* finite → eager Enumerator of repeated elements */
            const uint32_t blen = VAL2ARY(KORB_REF_GET(self))->len;
            slots[0] = UNWRAP(korb_ary_new(c, slots, (n > 0 ? (uint32_t)n : 0) * blen));
            korb_ref out = KORB_REF_AT(&slots[0]);
            for (korb_sword_t pass = 0; pass < n; pass++)
                for (uint32_t i = 0; i < VAL2ARY(KORB_REF_GET(self))->len; i++)
                    CHECK(korb_ary_push_val(c, slots + 1, out, korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i]));
            slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, KORB_REF_GET(self), "cycle"));
            return korb_enum_new(c, slots + 2, KORB_REF_GET(out), slots[1]);
        }
        {   /* unbounded → infinite lazy enum; #size is Infinity unless self is empty */
            RESULT lr = korb_lazy_new(c, slots, KORB_REF_GET(self), 2);
            if (lr.state == KORB_NORMAL && VAL2ARY(KORB_REF_GET(self))->len > 0) {
                VAL2ENUM(lr.value)->size_inf = 1;
            } else if (lr.state == KORB_NORMAL) {
                VAL2ENUM(lr.value)->size = LONG2FIX(0);
            }
            return lr;
        }
    }
    if (bounded && n <= 0) return RESULT_OK(KORB_NIL);
    if (SELF_ARY->len == 0) return RESULT_OK(KORB_NIL);
    for (korb_sword_t pass = 0; !bounded || pass < n; pass++) {
        for (uint32_t i = 0; ; i++) {
            const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
            if (i >= ary->len) break;
            korb_value e = korb_ary_ptr(ary)[i];
            RESULT r = korb_block_yield(c, slots, block, def_env, &e, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_compact(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        korb_value e = korb_ary_ptr(SELF_ARY)[i];
        if (e != KORB_NIL) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}

static RESULT korb_m_ary_compact_bang(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    (void)slots;(void)a;
    KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
    void *const it = korb_ary_owner(ary); korb_value *const it_d = korb_ary_ptr(ary);
    uint32_t w = 0; bool changed = false;
    for (uint32_t r = 0; r < ary->len; r++) {
        if (it_d[r] == KORB_NIL) { changed = true; continue; }
        if (w != r) ARO_STORE(c, it, &it_d[w], it_d[r]);
        w++;
    }
    for (uint32_t r = w; r < ary->len; r++) ARO_STORE(c, it, &it_d[r], KORB_NIL);
    ary->len = w;
    return RESULT_OK(changed ? KORB_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_ary_each_index(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    (void)a;
    if (block == NULL) {                                  /* → Enumerator of indices */
        const uint32_t len = VAL2ARY(KORB_REF_GET(self))->len;
        slots[0] = UNWRAP(korb_ary_new(c, slots, len));
        korb_ref idx = KORB_REF_AT(&slots[0]);
        for (uint32_t i = 0; i < len; i++) CHECK(korb_ary_push_val(c, slots + 1, idx, LONG2FIX(i)));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, KORB_REF_GET(self), "each_index"));
        const RESULT er = korb_enum_new(c, slots + 2, KORB_REF_GET(idx), slots[1]);
        return korb_enum_each_like(c, er, KORB_REF_GET(self));
    }
    for (uint32_t i = 0; ; i++) {
        if (i >= VAL2ARY(KORB_REF_GET(self))->len) break;
        korb_value iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(KORB_REF_GET(self));
}

static RESULT korb_m_ary_uniq(CTX *c, korb_value *slots, korb_ref self, korb_slice a);   /* fwd */
/* A 64-bit hash for uniq/Set-style dedup: dispatch #hash for a user object (so
 * elements are compared by hash first, and #eql? only when hashes match, like
 * CRuby's st_table), else the intrinsic value hash. */
static RESULT korb_elem_hash(CTX *c, korb_value *slots, korb_value v, uint64_t *out) {
    if (!KORB_OBJECT_P(v)) { *out = korb_value_hash(v); return RESULT_OK(KORB_NIL); }
    slots[0] = v;
    const RESULT r = korb_send_impl(c, slots + 1, korb_intern_lit(c->vm, "hash", 4), 0, 0, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (FIXNUM_P(r.value)) *out = (uint64_t)FIX2LONG(r.value);
    else if (KORB_BIGNUM_P(r.value)) *out = (uint64_t)korb_mp_get_ui(VAL2BIG(r.value)->z);
    else *out = korb_value_hash(r.value);
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_uniq_b(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself);   /* fwd */
static RESULT korb_m_ary_uniq_bang(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    const uint32_t before = SELF_ARY->len;
    const RESULT ur = block ? korb_m_ary_uniq_b(c, slots, self, a, block, def_env, cself)
                            : korb_m_ary_uniq(c, slots, self, a);   /* CTX-aware (dispatches eql? for user objects) */
    if (UNLIKELY(ur.state != KORB_NORMAL)) return ur;
    slots[0] = ur.value;                                    /* the deduped array (rooted) */
    const uint32_t after = VAL2ARY(slots[0])->len;
    if (after == before) return RESULT_OK(KORB_NIL);        /* no dups removed */
    KorbArray *const ary = SELF_ARY;                        /* re-read after uniq's allocs */
    const KorbArray *const u = VAL2ARY(slots[0]);
    for (uint32_t i = 0; i < after; i++) ARO_STORE(c, korb_ary_owner(ary), &korb_ary_ptr(ary)[i], korb_ary_ptr(u)[i]);
    ary->len = after;
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_ary_uniq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const uint32_t n0 = SELF_ARY->len;
    uint64_t *const hashes = n0 ? (uint64_t *)malloc((size_t)n0 * sizeof(uint64_t)) : NULL;   /* stored elems' hashes (parallel to dst) */
    if (n0 && UNLIKELY(!hashes)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "out of memory");
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n0)));
    const uint32_t eqm = korb_intern_lit(c->vm, "eql?", 4);
    RESULT ret = RESULT_OK(KORB_NIL);
    for (uint32_t i = 0; i < n0; i++) {                   /* bound to the snapshot (hashes[] size); a mutating #eql? can't overflow */
        if (i >= VAL2ARY(KORB_REF_GET(self))->len) break;   /* array shrank during iteration */
        slots[0] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];   /* e, rooted across hash/eql? dispatch + push */
        uint64_t he;
        { RESULT hr = korb_elem_hash(c, slots + 1, slots[0], &he); if (UNLIKELY(hr.state != KORB_NORMAL)) { ret = hr; goto done; } }
        const KorbArray *d = VAL2ARY(KORB_REF_GET(dst));
        bool seen = false;
        for (uint32_t j = 0; j < d->len; j++) {
            if (hashes[j] != he) continue;               /* hash mismatch → never call #eql? */
            const korb_value existing = korb_ary_ptr(d)[j];
            if (existing == slots[0]) { seen = true; break; }   /* identity short-circuit (CRuby rb_any_cmp), even when #eql? is non-reflexive */
            if (korb_eq_needs_send(existing) || korb_eq_needs_send(slots[0])) {   /* same hash → e.eql?(existing) */
                slots[1] = slots[0]; slots[2] = slots[0]; slots[3] = existing;
                const RESULT r = korb_send_impl(c, slots + 4, eqm, 0, 1, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) { ret = r; goto done; }
                if (KORB_TRUTHY(r.value)) { seen = true; break; }
                d = VAL2ARY(KORB_REF_GET(dst));         /* re-read after dispatch GC */
            } else if (korb_value_eql(existing, slots[0])) { seen = true; break; }
        }
        if (!seen) {
            const uint32_t idx = VAL2ARY(KORB_REF_GET(dst))->len;
            RESULT pr = korb_ary_push_val(c, slots + 1, dst, slots[0]);
            if (UNLIKELY(pr.state != KORB_NORMAL)) { ret = pr; goto done; }
            hashes[idx] = he;
        }
    }
    ret = RESULT_OK(KORB_REF_GET(dst));
done:
    free(hashes);
    return ret;
}
/* Membership test for the set operations (|, &): CRuby collects values through an
 * intermediate hash, so equivalence is by #hash + #eql? for user objects.  Non-object
 * elements fall to type-strict value/identity equality (the fast path).  *out ← found. */
static RESULT korb_arr_member_eql(CTX *c, korb_value *slots, korb_ref ary, korb_value elem, bool *out) {
    *out = false;
    slots[0] = elem;                                     /* root elem across #hash / #eql? dispatch */
    const bool elem_obj = korb_eq_needs_send(elem);
    const uint32_t eqm = korb_intern_lit(c->vm, "eql?", 4);
    uint64_t eh = 0; bool have_eh = false;
    for (uint32_t j = 0; j < VAL2ARY(KORB_REF_GET(ary))->len; j++) {
        const korb_value cand = korb_ary_ptr(VAL2ARY(KORB_REF_GET(ary)))[j];
        if (cand == slots[0]) { *out = true; return RESULT_OK(KORB_NIL); }   /* identity short-circuit (CRuby rb_any_cmp), even when #eql? is non-reflexive */
        if (elem_obj || korb_eq_needs_send(cand)) {       /* dispatch: same #hash then elem.eql?(cand) */
            if (!have_eh) { const RESULT hr = korb_elem_hash(c, slots + 1, slots[0], &eh); if (UNLIKELY(hr.state != KORB_NORMAL)) return hr; have_eh = true; }
            slots[1] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(ary)))[j];   /* cand, rooted for the hash dispatch */
            uint64_t ch; { const RESULT hr = korb_elem_hash(c, slots + 2, slots[1], &ch); if (UNLIKELY(hr.state != KORB_NORMAL)) return hr; }
            if (ch != eh) continue;
            slots[2] = slots[0];                          /* receiver = elem (base[-2]) */
            slots[3] = slots[1];                          /* arg0     = cand (base[-1]) */
            const RESULT r = korb_send_impl(c, slots + 4, eqm, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) { *out = true; return RESULT_OK(KORB_NIL); }
        } else if (korb_value_eql(cand, slots[0])) { *out = true; return RESULT_OK(KORB_NIL); }
    }
    return RESULT_OK(KORB_NIL);
}
/* Array#uniq — block form dedups by yield(x); keeps the first element per key. */
static RESULT korb_m_ary_uniq_b(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (block == NULL) return korb_m_ary_uniq(c, slots, self, a);
    slots[0] = UNWRAP(korb_ary_new(c, slots, SELF_ARY->len));       /* result */
    korb_ref dst = KORB_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));              /* seen keys */
    korb_ref keys = KORB_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        if (i >= VAL2ARY(KORB_REF_GET(self))->len) break;
        slots[2] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];   /* element */
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;                                        /* key */
        bool seen = false;
        const KorbArray *ks = VAL2ARY(KORB_REF_GET(keys));
        for (uint32_t j = 0; j < ks->len; j++) if (korb_value_eql(korb_ary_ptr(ks)[j], slots[3])) { seen = true; break; }
        if (!seen) {
            CHECK(korb_ary_push_val(c, slots + 4, keys, slots[3]));
            CHECK(korb_ary_push_val(c, slots + 4, dst, slots[2]));
        }
    }
    return RESULT_OK(KORB_REF_GET(dst));
}

/* recursive flatten helper: append all leaves of `src` into dst */
/* depth-limited flatten: depth<0 = full, 0 = copy as-is, >0 = that many levels. */
/* guard = the arrays on the current recursion path (seeded with the top-level
 * self); a child already present means a cycle → ArgumentError, not a SEGV. */
static RESULT korb_ary_flatten_depth(CTX *c, korb_value *slots, korb_ref dst, korb_ref src, int depth, korb_ref guard) {
    uint32_t n = VAL2ARY(KORB_REF_GET(src))->len;
    for (uint32_t i = 0; i < n; i++) {
        korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(src)))[i];
        if (depth != 0 && !KORB_ARRAY_P(e) && KORB_OBJECT_P(e)) {        /* non-Array element with #to_ary → flatten its result */
            slots[0] = e;                                    /* park: the probe below runs Ruby, so `e` cannot stay a C local */
            korb_value cv = e;                                    /* rb_check_array_type: respond_to?- and method_missing-aware */
            const RESULT cr = korb_check_funcall(c, slots + 1, &cv, korb_intern_lit(c->vm, "to_ary", 6));
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            e = slots[0];                                    /* re-read the (possibly moved) element */
            if (cr.value == KORB_TRUE && cv != KORB_NIL) {   /* nil → not coercible: leave the element as a leaf */
                if (UNLIKELY(!KORB_ARRAY_P(cv)))
                    return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "can't convert %s into Array (%s#to_ary gives %s)", korb_coerce_name(c, e), korb_coerce_name(c, e), korb_coerce_name(c, cv));
                e = cv;
            }
        }
        if (KORB_ARRAY_P(e) && depth != 0) {
            if (depth < 0) {                              /* only unlimited flatten can loop; a finite depth bounds it (and CRuby doesn't raise) */
                const KorbArray *const g = VAL2ARY(KORB_REF_GET(guard));
                for (uint32_t j = 0; j < g->len; j++)
                    if (korb_ary_ptr(g)[j] == e) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to flatten recursive array");
            }
            slots[0] = e;
            CHECK(korb_ary_push_val(c, slots + 1, guard, e));            /* push e onto the path */
            CHECK(korb_ary_flatten_depth(c, slots + 1, dst, KORB_REF_AT(&slots[0]), depth < 0 ? depth : depth - 1, guard));
            VAL2ARY(KORB_REF_GET(guard))->len--;                        /* pop (e stays rooted in slots[0] until here) */
        } else {
            CHECK(korb_ary_push_val(c, slots, dst, e));
        }
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_flatten(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    int depth = -1;
    korb_sword_t d;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL) {   /* depth arg → #to_int, else TypeError */
        korb_value dv = KORB_SLICE_GET(a, 0);
        if (!korb_to_index(dv, &d)) {
            RESULT cr = korb_coerce_to_int(c, slots, &dv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(dv, &d)) return korb_raise_no_int(c, slots, KORB_SLICE_GET(a, 0));
        }
        depth = (int)d;
    }
    uint32_t n = SELF_ARY->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    korb_ref dst = KORB_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));                    /* guard (arrays on the path) */
    korb_ref guard = KORB_REF_AT(&slots[1]);
    CHECK(korb_ary_push_val(c, slots + 2, guard, KORB_REF_GET(self)));
    CHECK(korb_ary_flatten_depth(c, slots + 2, dst, self, depth, guard));
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_flatten_b(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    korb_sword_t depth = -1;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL) (void)korb_to_index(KORB_SLICE_GET(a, 0), &depth);
    const uint32_t len0 = VAL2ARY(KORB_REF_GET(self))->len;
    RESULT fr = korb_m_ary_flatten(c, slots, self, a);   /* flattened copy */
    if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
    slots[0] = fr.value;
    /* "changed" means the copy differs, which covers a #to_ary element as well
     * as a nested Array (a length change, or an element replaced in place) */
    bool changed = depth != 0 && VAL2ARY(slots[0])->len != len0;
    if (!changed && depth != 0) {
        const KorbArray *const src = VAL2ARY(KORB_REF_GET(self));
        const KorbArray *const cp  = VAL2ARY(slots[0]);
        for (uint32_t i = 0; i < len0 && !changed; i++)
            if (korb_ary_ptr(src)[i] != korb_ary_ptr(cp)[i]) changed = true;
    }
    korb_ref flat = KORB_REF_AT(&slots[0]);
    VAL2ARY(KORB_REF_GET(self))->len = 0;
    uint32_t fn = VAL2ARY(KORB_REF_GET(flat))->len;
    for (uint32_t i = 0; i < fn; i++) {
        korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(flat)))[i];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(changed ? KORB_REF_GET(self) : KORB_NIL);
}

static RESULT korb_m_ary_concat(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    const uint32_t self_len0 = VAL2ARY(KORB_REF_GET(self))->len;   /* initial length: concat(self, self) appends the original, not the growing, self */
    for (uint32_t k = 0; k < KORB_SLICE_LEN(a); k++) {   /* concat(*arrays) */
        slots[0] = KORB_SLICE_GET(a, k);                 /* arg (rooted; possibly coerced) */
        if (UNLIKELY(!KORB_ARRAY_P(slots[0]))) {          /* coerce via #to_ary (a wrong-typed one raises) */
            korb_value av = slots[0];
            const RESULT cr = korb_coerce_to_ary(c, slots + 1, &av);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            slots[0] = av;
            if (cr.value != KORB_TRUE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_coerce_name(c, av));
        }
        const uint32_t n = (slots[0] == KORB_REF_GET(self)) ? self_len0 : VAL2ARY(slots[0])->len;   /* self-arg → its original length */
        for (uint32_t i = 0; i < n; i++)
            CHECK(korb_ary_push_val(c, slots + 1, self, korb_ary_ptr(VAL2ARY(slots[0]))[i]));   /* re-read other (rooted) */
    }
    return RESULT_OK(KORB_REF_GET(self));
}

/* select (keep==true) / reject (keep==false) */
static RESULT korb_ary_filter(CTX *c, korb_value *slots, korb_ref self, NODE *block, korb_value *def_env, korb_value *captured_self, bool keep) {
    /* No block → an Enumerator tagged with the filter op (e->op), so a later
     * .each / .with_index block filters rather than maps. */
    if (UNLIKELY(block == NULL)) {
        slots[0] = UNWRAP(korb_enum_desc(c, slots, KORB_REF_GET(self), keep ? "select" : "reject"));
        RESULT er = korb_enum_new(c, slots + 1, KORB_REF_GET(self), slots[0]);
        if (er.state == KORB_NORMAL) VAL2ENUM(er.value)->op = keep ? 1 : 2;
        return er;
    }
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        korb_value e = korb_ary_ptr(ary)[i];
        slots[0] = e;                                       /* root e across the yield */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_select(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) { (void)a; return korb_ary_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_ary_reject(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) { (void)a; return korb_ary_filter(c, slots, self, block, def_env, captured_self, false); }

/* Enumerable#find / #detect take an optional `ifnone` callable, invoked when no
 * element matched (nil → nil). */
static RESULT korb_find_ifnone(CTX *c, korb_value *slots, korb_slice a) {
    if (KORB_SLICE_LEN(a) < 1 || KORB_SLICE_GET(a, 0) == KORB_NIL) return RESULT_OK(KORB_NIL);
    slots[0] = KORB_SLICE_GET(a, 0);
    return korb_send(c, slots + 1, korb_intern_lit(c->vm, "call", 4), 0, 0);
}
static RESULT korb_m_ary_find(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    /* no block → to_enum(:find, ifnone): re-driving replays the ifnone proc */
    if (UNLIKELY(block == NULL)) return korb_ary_to_enum_unsized(c, slots, self, "find", a);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        slots[0] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[0]);
    }
    return korb_find_ifnone(c, slots, a);   /* find(ifnone): nothing matched → ifnone.call */
}

static RESULT korb_m_ary_rfind(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    if (UNLIKELY(block == NULL)) return korb_ary_to_enum_unsized(c, slots, self, "rfind", a);
    for (int64_t i = (int64_t)VAL2ARY(KORB_REF_GET(self))->len - 1; i >= 0; i--) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if ((uint64_t)i >= ary->len) continue;
        slots[0] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[0]);
    }
    return korb_find_ifnone(c, slots, a);   /* rfind(ifnone) — same contract as #find */
}
static RESULT korb_m_ary_find_index(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    if (KORB_SLICE_LEN(a) >= 1) {                    /* find_index(obj): first index == obj */
        if (block != NULL) korb_warn(c, slots, "given block not used");   /* arg wins */
        slots[0] = KORB_SLICE_GET(a, 0);            /* needle (root across element == dispatch) */
        const uint32_t n = VAL2ARY(KORB_REF_GET(self))->len;
        const bool eqr = KORB_EQ_REDEF(c);   /* read once: the loop calls out of line */
        for (uint32_t i = 0; i < n; i++) {
            const korb_value e = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];   /* re-read each iter (dispatch may GC) */
            if (korb_eq_send_p_(eqr, e, slots[0])) {   /* user #== → dispatch element == needle */
                slots[1] = e; slots[2] = slots[0];
                RESULT r = korb_send_impl(c, slots + 3, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
            } else if (korb_value_eq(e, slots[0])) {
                return RESULT_OK(LONG2FIX(i));
            }
        }
        return RESULT_OK(KORB_NIL);
    }
    ARY_REQUIRE_BLOCK("Array#find_index");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        slots[0] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_take_while(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#take_while");
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!KORB_TRUTHY(r.value)) break;
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_drop_while(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#drop_while");
    uint32_t start = 0; bool dropping = true;
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_ary_ptr(ary)[i];
        if (dropping) {
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) { start = i + 1; continue; }
            dropping = false;
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    (void)start;
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_clear(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    (void)c;(void)slots;(void)a;
    KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
    for (uint32_t i = 0; i < ary->len; i++) ARO_STORE(c, korb_ary_owner(ary), &korb_ary_ptr(ary)[i], KORB_NIL);
    ary->len = 0;
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_ary_intersect_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value ov = KORB_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) {                /* coerce via #to_ary */
        const uint32_t to_ary = korb_intern_lit(c->vm, "to_ary", 6);
        if (KORB_OBJECT_P(ov) && korb_responds_to_coerce_p(c, slots, &ov, to_ary)) {
            slots[0] = ov;
            RESULT ar = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
            ov = ar.value;
        }
        if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_coerce_name(c, KORB_SLICE_GET(a, 0)));
    }
    slots[0] = ov;                                   /* root the (possibly coerced) other across #eql? dispatch */
    for (uint32_t i = 0; i < VAL2ARY(KORB_REF_GET(self))->len; i++) {
        slots[1] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];
        bool has; CHECK(korb_arr_member_eql(c, slots + 2, KORB_REF_AT(&slots[0]), slots[1], &has));
        if (has) return RESULT_OK(KORB_TRUE);
    }
    return RESULT_OK(KORB_FALSE);
}
/* find-any mode: only the sign of the block's numeric result matters, so a
 * Bignum (whose magnitude no double can hold) is as good as a Fixnum. */
static bool korb_bsearch_num_sign(korb_value rv, int *const sign) {
    if (KORB_BIGNUM_P(rv)) { *sign = korb_mp_sgn(VAL2BIG(rv)->z); return true; }
    double d;
    if (!(FIXNUM_P(rv) || KORB_FLOAT_P(rv)) || !korb_num_to_d(rv, &d)) return false;
    *sign = (d < 0) ? -1 : (d > 0) ? 1 : 0;
    return true;
}

/* bsearch: find-minimum (boolean block) or find-any (Integer block). Returns the
 * matching element, or nil. Array must be sorted for meaningful results. */
static RESULT korb_m_ary_bsearch(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    if (UNLIKELY(block == NULL)) return korb_ary_to_enum_unsized(c, slots, self, "bsearch", a);   /* CRuby: no size fn */
    uint32_t lo = 0, hi = VAL2ARY(KORB_REF_GET(self))->len;
    korb_value found = KORB_NIL; bool have = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        slots[0] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[mid];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        korb_value rv = r.value;
        int cmp;
        if (rv == KORB_TRUE || rv == KORB_FALSE || rv == KORB_NIL) {   /* find-minimum */
            if (KORB_TRUTHY(rv)) { found = slots[0]; have = true; hi = mid; }
            else lo = mid + 1;
        } else if (korb_bsearch_num_sign(rv, &cmp)) {                  /* find-any (numeric) */
            if (cmp == 0) return RESULT_OK(slots[0]);
            else if (cmp < 0) hi = mid;
            else lo = mid + 1;
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong element type %s (must be numeric, true, false or nil)", korb_type_name(rv));
        }
    }
    return RESULT_OK(have ? found : KORB_NIL);
}

static RESULT korb_m_ary_bsearch_index(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    if (UNLIKELY(block == NULL)) return korb_ary_to_enum_unsized(c, slots, self, "bsearch_index", a);   /* CRuby: no size fn */
    uint32_t lo = 0, hi = VAL2ARY(KORB_REF_GET(self))->len;
    uint32_t found = 0; bool have = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        slots[0] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[mid];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        korb_value rv = r.value;
        int cmp;
        if (rv == KORB_TRUE || rv == KORB_FALSE || rv == KORB_NIL) {
            if (KORB_TRUTHY(rv)) { found = mid; have = true; hi = mid; } else lo = mid + 1;
        } else if (korb_bsearch_num_sign(rv, &cmp)) {
            if (cmp == 0) return RESULT_OK(LONG2FIX(mid));
            else if (cmp < 0) hi = mid; else lo = mid + 1;
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong element type %s (must be numeric, true, false or nil)", korb_type_name(rv));
        }
    }
    return RESULT_OK(have ? LONG2FIX(found) : KORB_NIL);
}
static RESULT korb_m_ary_map_bang(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#map!");   /* no block → Enumerator; frozen only bites when driven */
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_ary_ptr(ary)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        KorbArray *a2 = VAL2ARY(KORB_REF_GET(self));
        ARO_STORE(c, korb_ary_owner(a2), &korb_ary_ptr(a2)[i], r.value);
    }
    return RESULT_OK(KORB_REF_GET(self));
}
/* Integer#step / Float#step (generic over numeric self) */
static RESULT korb_enum_new(CTX *c, korb_value *slots, korb_value vals, korb_value desc);
static RESULT korb_enum_desc(CTX *c, korb_value *slots, korb_value recv, const char *meth);
/* Integer#step where a value leaves the Fixnum range (a Bignum end point or
 * step, or a limit within one step of the edge): Integer#+ and #> / #< widen. */
static RESULT korb_int_step_wide_x(CTX *c, korb_value *slots, korb_ref self, korb_value limv, korb_value stepv,
                                   NODE *block, korb_value *def_env, korb_value *cself, bool collect, bool excl, bool want_ary) {
    struct korb_vm *const vm = c->vm;
    slots[0] = KORB_REF_GET(self);                   /* i */
    slots[1] = limv;
    slots[2] = stepv;
    slots[3] = collect ? UNWRAP(korb_ary_new(c, slots + 3, 8)) : KORB_NIL;
    slots[4] = stepv; slots[5] = LONG2FIX(0);
    if (KORB_TRUTHY(UNWRAP(korb_send(c, slots + 6, korb_intern_lit(vm, "==", 2), 0, 1))))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
    slots[4] = stepv; slots[5] = LONG2FIX(0);
    const bool down = KORB_TRUTHY(UNWRAP(korb_send(c, slots + 6, korb_intern_lit(vm, "<", 1), 0, 1)));
    const uint32_t past = excl ? korb_intern(vm, down ? "<=" : ">=", 2) : korb_intern(vm, down ? "<" : ">", 1);
    const uint32_t plus = korb_intern_lit(vm, "+", 1);
    for (;;) {
        if (slots[1] != KORB_NIL) {
            slots[4] = slots[0]; slots[5] = slots[1];
            if (KORB_TRUTHY(UNWRAP(korb_send(c, slots + 6, past, 0, 1)))) break;
        }
        if (collect) CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[0]));
        else CHECK(korb_block_yield(c, slots + 4, block, def_env, &slots[0], 1, cself));
        slots[4] = slots[0]; slots[5] = slots[2];
        slots[0] = UNWRAP(korb_send(c, slots + 6, plus, 0, 1));
    }
    if (!collect) return RESULT_OK(KORB_REF_GET(self));
    if (want_ary) return RESULT_OK(slots[3]);
    slots[4] = UNWRAP(korb_enum_desc(c, slots + 4, KORB_REF_GET(self), "step"));
    return korb_enum_new(c, slots + 5, slots[3], slots[4]);
}
static RESULT korb_int_step_wide(CTX *c, korb_value *slots, korb_ref self, korb_value limv, korb_value stepv,
                                 NODE *block, korb_value *def_env, korb_value *cself, bool collect) {
    return korb_int_step_wide_x(c, slots, self, limv, stepv, block, def_env, cself, collect, false, false);
}
static RESULT korb_m_num_step(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    /* keyword form `step(to:, by:)`: a trailing Hash with :to / :by maps to the
     * limit / step (limit nil ⇒ endless). */
    uint32_t na = KORB_SLICE_LEN(a);
    korb_value kwlim = KORB_NIL, kwstep = KORB_NIL; bool kw = false;
    if (na >= 1 && KORB_HASH_P(KORB_SLICE_GET(a, na - 1))) {
        const korb_value h = KORB_SLICE_GET(a, na - 1);
        const int32_t ti = korb_hash_find(VAL2HASH(h), ID2SYM(korb_intern_lit(c->vm, "to", 2)));
        const int32_t bi = korb_hash_find(VAL2HASH(h), ID2SYM(korb_intern_lit(c->vm, "by", 2)));
        if (ti >= 0 || bi >= 0) {
            kw = true;
            if (ti >= 0) kwlim  = korb_items_data(VAL2HASH(h)->items)[2 * ti + 1];
            if (bi >= 0) kwstep = korb_items_data(VAL2HASH(h)->items)[2 * bi + 1];
            na--;
            if (na >= 1 && ti >= 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "to is given twice");   /* positional limit + to: */
            if (na >= 2 && bi >= 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step is given twice"); /* positional step + by: */
        }
    }
    const korb_value limv0 = na >= 1 ? KORB_SLICE_GET(a, 0) : kwlim;                       /* limit (nil ⇒ endless) */
    const korb_value stepv0 = na >= 2 ? KORB_SLICE_GET(a, 1) : (kwstep != KORB_NIL ? kwstep : LONG2FIX(1));
    /* a non-nil non-Numeric step — a String (even a numeric-looking "1") — is an
     * ArgumentError in CRuby.  nil is allowed and means a default step of 1 (but
     * is preserved verbatim for the ArithmeticSequence's inspect). */
    #define KORB_NUM_P(v) (FIXNUM_P(v) || KORB_FLOAT_P(v) || KORB_BIGNUM_P(v) || KORB_RATIONAL_P(v) || KORB_COMPLEX_P(v))
    const bool numeric_step = (stepv0 == KORB_NIL) || KORB_NUM_P(stepv0);
    #undef KORB_NUM_P
    if (block == NULL) {
        /* a non-numeric step defers the error to iteration time, so the result is
         * a plain Enumerator rather than an ArithmeticSequence (CRuby) */
        if (!numeric_step) {
            slots[0] = KORB_REF_GET(self);
            slots[1] = limv0;
            slots[2] = stepv0;
            return korb_send(c, slots + 3, korb_intern_lit(c->vm, "__step_bad_enum", 15), 0, 2);
        }
        return korb_arithseq_new(c, slots, KORB_REF_GET(self), limv0, stepv0, (uint8_t)((kw || na >= 2) ? 2 : na), 0);
    }
    if (!numeric_step)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step requires numeric arguments");
    korb_value selfv = KORB_REF_GET(self);
    korb_value limv = limv0;
    korb_value stepv = (stepv0 == KORB_NIL) ? LONG2FIX(1) : stepv0;   /* nil step ⇒ 1 for iteration */
    bool use_float = KORB_FLOAT_P(selfv) || KORB_FLOAT_P(limv) || KORB_FLOAT_P(stepv);
    const bool collect = (block == NULL);             /* no block → materialize into an Enumerator */
    korb_ref dst = {0};
    if (use_float) {
        double s, lim, st;
        if (!korb_num_to_d(selfv, &s) || !korb_num_to_d(limv, &lim) || !korb_num_to_d(stepv, &st))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "step requires numeric arguments");
        if (st == 0.0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        if (collect) { slots[0] = UNWRAP(korb_ary_new(c, slots, 8)); dst = KORB_REF_AT(&slots[0]); }  /* after reading the doubles */
        /* An infinite/NaN step (`s + 0*Inf = NaN`) can make no progress, so yield
         * the start at most once.  An infinite/NaN *start* yields nothing at all
         * (CRuby: `Float::INFINITY.step(Float::INFINITY, 1) { }` never yields). */
        const bool once_only = isinf(st) || isnan(st);
        const bool no_yield  = (isinf(s) || isnan(s)) && !once_only;   /* Inf start + Inf step still yields once */
        /* CRuby ruby_float_step: fixed count with an epsilon fudge, so a limit
         * that is "one step away up to fp error" is still yielded (1.0.step(12.7,
         * 1.3) ends on 12.7). */
        long n = -1;
        if (!no_yield && !once_only && !isnan(lim)) {
            if (isinf(lim)) n = (st > 0) == (lim > 0) ? -2 : -1;   /* -2 = endless */
            else {
                double err = (fabs(s) + fabs(lim) + fabs(lim - s)) / fabs(st) * DBL_EPSILON;
                if (err > 0.5) err = 0.5;
                const double nf = floor((lim - s) / st + err);
                n = (nf < 0) ? -1 : (nf > 9e15 ? (long)9e15 : (long)nf);
            }
        } else if (once_only && !no_yield) {
            n = (isnan(lim) || (st > 0 ? s > lim : s < lim)) ? -1 : 0;   /* the start element only (if within limit) */
        }
        for (long i = 0; n == -2 || i <= n; i++) {
            double d = (i == 0) ? s : s + (double)i * st;
            if (n >= 0 && !isinf(lim) && (st > 0 ? d > lim : d < lim)) d = lim;   /* clamp fp overshoot to the limit */
            if (collect) { slots[1] = UNWRAP(korb_float_new(c, slots + 1, d)); CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1])); continue; }
            slots[0] = UNWRAP(korb_float_new(c, slots, d));
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    } else {
        /* A nil limit means ENDLESS (`1.step { }`), and a non-numeric one is
         * CRuby's comparison ArgumentError — FIX2LONG on a String pointer used
         * to make `1.step("a") { }` spin instead of raising. */
        const bool endless = (limv == KORB_NIL);
        if (!endless && !FIXNUM_P(limv) && !KORB_BIGNUM_P(limv))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0,
                              "comparison of %s with %s failed: coercion was not possible",
                              korb_coerce_name(c, selfv), korb_coerce_name(c, limv));
        if (UNLIKELY(!FIXNUM_P(selfv) || !FIXNUM_P(stepv) || (!endless && !FIXNUM_P(limv)) ||
                     (!endless && (FIX2LONG(stepv) > 0 ? FIX2LONG(limv) > FIXNUM_MAX - FIX2LONG(stepv)
                                                       : FIX2LONG(limv) < FIXNUM_MIN - FIX2LONG(stepv)))))
            return korb_int_step_wide(c, slots, self, limv, stepv, block, def_env, cself, collect);
        korb_sword_t s = FIX2LONG(selfv), lim = endless ? 0 : FIX2LONG(limv), st = FIX2LONG(stepv);
        if (st == 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        if (collect) { slots[0] = UNWRAP(korb_ary_new(c, slots, 8)); dst = KORB_REF_AT(&slots[0]); }
        for (korb_sword_t i = s; endless || (st > 0 ? i <= lim : i >= lim); i += st) {
            if (collect) { CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i))); continue; }
            slots[0] = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    }
    if (collect) {
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, KORB_REF_GET(self), "step"));   /* dst at slots[0] still rooted */
        return korb_enum_new(c, slots + 2, KORB_REF_GET(dst), slots[1]);
    }
    return RESULT_OK(KORB_REF_GET(self));
}

/* any? (mode 0) / all? (1) / none? (2). A pattern arg (case ===) wins over a block. */
static RESULT korb_ary_quant(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self, int mode) {
    if (UNLIKELY(KORB_SLICE_LEN(a) > 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", (unsigned)KORB_SLICE_LEN(a));
    const bool has_pat = KORB_SLICE_LEN(a) >= 1;
    if (has_pat && block != NULL) korb_warn(c, slots, "given block not used");   /* pattern arg wins */
    slots[0] = has_pat ? KORB_SLICE_GET(a, 0) : KORB_NIL;           /* pattern (rooted across dispatch) */
    for (uint32_t i = 0; ; i++) {
        if (i >= VAL2ARY(KORB_REF_GET(self))->len) break;           /* re-read len each iter (dispatch may GC) */
        slots[1] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];
        bool t;
        if (has_pat) {
            CHECK(korb_pat_eq(c, slots + 2, slots[0], slots[1], &t));   /* pattern === element */
        } else if (block != NULL) {                                 /* truthiness of block result */
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else {
            t = KORB_TRUTHY(slots[1]);                              /* no block → element truthiness */
        }
        if (mode == 0 && t) return RESULT_OK(KORB_TRUE);     /* any? */
        if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);   /* all? */
        if (mode == 2 && t) return RESULT_OK(KORB_FALSE);    /* none? */
    }
    return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_ary_any(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self)  { return korb_ary_quant(c, slots, self, a, block, def_env, captured_self, 0); }
static RESULT korb_m_ary_all(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self)  { return korb_ary_quant(c, slots, self, a, block, def_env, captured_self, 1); }
static RESULT korb_m_ary_none(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) { return korb_ary_quant(c, slots, self, a, block, def_env, captured_self, 2); }

/* Numeric binop for send/symbol dispatch (op: 0+ 1- 2* 3/ 4%). Int op int → Int
 * (overflow→error), Float involved → Float; matches the node_plus/minus/... paths. */
static RESULT korb_num_binop(CTX *c, korb_value *slots, korb_value l, korb_value r, int op) {
    if (FIXNUM_P(l) && FIXNUM_P(r)) {
        korb_sword_t a = FIX2LONG(l), b = FIX2LONG(r), res;
        switch (op) {
          case 0: if (LIKELY(!__builtin_add_overflow(a, b, &res) && FIXABLE(res))) return RESULT_OK(LONG2FIX(res)); break;
          case 1: if (LIKELY(!__builtin_sub_overflow(a, b, &res) && FIXABLE(res))) return RESULT_OK(LONG2FIX(res)); break;
          case 2: if (LIKELY(!__builtin_mul_overflow(a, b, &res) && FIXABLE(res))) return RESULT_OK(LONG2FIX(res)); break;
          case 3: if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0"); return RESULT_OK(LONG2FIX(korb_int_fdiv(a, b)));
          default: if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0"); return RESULT_OK(LONG2FIX(korb_int_fmod(a, b)));
        }
        return korb_int_arith(c, slots, l, r, op, 0);    /* +,-,* overflow → promote to Bignum */
    }
    if (KORB_INTEGER_P(l) && KORB_INTEGER_P(r))          /* Bignum operand(s) → exact integer arith */
        return korb_int_arith(c, slots, l, r, op, 0);
    /* exact Complex/Rational — the operator slow-paths do this; the method-dispatch
     * path (Integer#+(rat) via send/sum/reduce(:+)) must match, not coerce to Float. */
    if (KORB_COMPLEX_P(l) || KORB_COMPLEX_P(r)) return korb_cpx_arith(c, slots, l, r, op);
    if (KORB_RATIONAL_P(l) || KORB_RATIONAL_P(r)) return korb_rat_arith(c, slots, l, r, op);
    return korb_num_arith(c, slots, l, r, op, 0);
}
static RESULT korb_m_num_add(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_num_binop(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), 0); }
static RESULT korb_m_num_sub(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_num_binop(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), 1); }
static RESULT korb_m_num_mul(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_num_binop(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), 2); }
static RESULT korb_m_num_div(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_num_binop(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), 3); }
static RESULT korb_m_num_mod(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_num_binop(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), 4); }
static RESULT korb_m_num_lt(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_cmp_slow(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), 0, 0); }
static RESULT korb_m_num_le(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_cmp_slow(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), 1, 0); }
static RESULT korb_m_num_gt(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_cmp_slow(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), 2, 0); }
static RESULT korb_m_num_ge(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_cmp_slow(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), 3, 0); }
/* extract the operator mid from a sym-form reduce arg (Symbol or String). */
static bool korb_reduce_op(CTX *c, korb_value v, uint32_t *op_mid) {
    if (SYMBOL_P(v))       { *op_mid = SYM2ID(v); return true; }
    if (KORB_STRING_P(v))  { *op_mid = korb_intern_enc(c->vm, korb_strbuf_data(VAL2STR(v)->buf), VAL2STR(v)->len, KORB_STR_ENC(v)); return true; }
    return false;
}
/* Resolve a reduce/inject operator arg → mid: Symbol/String, or #to_str-coercible
 * (else TypeError).  *ok set false only on the raise path. */
static RESULT korb_reduce_resolve_op(CTX *c, korb_value *slots, korb_value v, uint32_t *op_mid, bool *ok) {
    *ok = true;
    if (korb_reduce_op(c, v, op_mid)) return RESULT_OK(KORB_NIL);
    const uint32_t to_str = korb_intern_lit(c->vm, "to_str", 6);
    if (KORB_OBJECT_P(v) && korb_responds_to_coerce_p(c, slots, &v, to_str)) {
        slots[0] = v;
        RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(sr.state != KORB_NORMAL)) { *ok = false; return sr; }
        if (KORB_STRING_P(sr.value)) { *op_mid = korb_intern_enc(c->vm, korb_strbuf_data(VAL2STR(sr.value)->buf), VAL2STR(sr.value)->len, KORB_STR_ENC(sr.value)); return RESULT_OK(KORB_NIL); }
    }
    *ok = false;
    return korb_raise_not_sym(c, slots, v);
}
static RESULT korb_m_ary_reduce(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    uint32_t op_mid;
    /* argc 2 → sym form (op=a[1], init=a[0]), block IGNORED; argc 1 + no block →
     * sym form (op=a[0]); argc 1 + block → block form with init; argc 0 → block. */
    const uint32_t na0 = KORB_SLICE_LEN(a);
    bool sym_form = false;
    if (na0 >= 2 || (na0 == 1 && block == NULL)) {
        bool ok; RESULT rr = korb_reduce_resolve_op(c, slots, KORB_SLICE_GET(a, na0 - 1), &op_mid, &ok);
        if (UNLIKELY(!ok)) return rr;
        sym_form = true;
    }
    if (block == NULL && !sym_form)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block or operator symbol given");
    if (sym_form) {                                        /* reduce(:+) / reduce(init, :+) [block ignored] */
        uint32_t na = na0;
        uint32_t i = 0;
        if (na >= 2) slots[0] = KORB_SLICE_GET(a, 0);     /* explicit init */
        else { const KorbArray *ary = SELF_ARY; if (ary->len == 0) return RESULT_OK(KORB_NIL); slots[0] = korb_ary_ptr(ary)[0]; i = 1; }
        for (; ; i++) {
            const KorbArray *ary = SELF_ARY;
            if (i >= ary->len) break;
            slots[1] = slots[0]; slots[2] = korb_ary_ptr(ary)[i];   /* acc, elem (recv+arg) */
            RESULT r = korb_send_impl(c, slots + 3, op_mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;
        }
        return RESULT_OK(slots[0]);
    }
    uint32_t i = 0;
    if (KORB_SLICE_LEN(a) >= 1) {
        slots[0] = KORB_SLICE_GET(a, 0);                  /* acc = initial */
    } else {
        const KorbArray *ary = SELF_ARY;
        if (ary->len == 0) return RESULT_OK(KORB_NIL);
        slots[0] = korb_ary_ptr(ary)[0];
        i = 1;
    }
    for (; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        korb_value argv[2] = { slots[0], korb_ary_ptr(ary)[i] };  /* acc, elem */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;                                /* root new acc */
    }
    return RESULT_OK(slots[0]);
}

static RESULT korb_m_ary_each_with_object(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    if (UNLIKELY(block == NULL)) {                        /* no block → self.to_enum(:each_with_object, memo) */
        slots[0] = KORB_REF_GET(self);
        slots[1] = ID2SYM(korb_intern_lit(c->vm, "each_with_object", 16));
        slots[2] = KORB_SLICE_GET(a, 0);
        return korb_send_impl(c, slots + 3, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 2, NULL, NULL, NULL);
    }
    slots[0] = KORB_SLICE_GET(a, 0);                      /* the memo object (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        korb_value argv[2] = { korb_ary_ptr(ary)[i], slots[0] };  /* elem, memo */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}


/* An Enumerator built by delegating to the Array version describes itself with
 * the forced value array; name the enumerator we were called on instead. */
static RESULT korb_enum_redesc(CTX *c, korb_value *slots, korb_ref self, RESULT r, const char *meth, korb_slice a) {
    if (r.state != KORB_NORMAL || !KORB_ENUM_P(r.value)) return r;
    slots[0] = r.value;
    slots[1] = UNWRAP(korb_enum_desc_args(c, slots + 1, KORB_REF_GET(self), meth, a));
    KorbEnumerator *const e = VAL2ENUM(slots[0]);
    ARO_STORE(c, e, (korb_value *)(uintptr_t)&e->desc, slots[1]);
    return RESULT_OK(slots[0]);
}

/* Enumerator#each_slice / each_cons — delegate to the Array versions on the
 * enumerator's (eager-forced) value array, so Enumerable chains like
 * `str.each_char.each_slice(2)` work.  No block → an Enumerator of the
 * slices/windows (Array#each_slice handles that). */
static RESULT korb_m_enum_each_slice(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    RESULT av = korb_m_enum_to_a(c, slots, self, a);     /* eager array (forces a finite lazy enum) */
    if (UNLIKELY(av.state != KORB_NORMAL)) return av;
    slots[0] = av.value;
    RESULT r = korb_m_ary_each_slice(c, slots + 1, KORB_REF_AT(&slots[0]), a, block, def_env, cself);
    if (block != NULL && r.state == KORB_NORMAL) return RESULT_OK(KORB_REF_GET(self));   /* block form returns the enumerator */
    return korb_enum_redesc(c, slots, self, r, "each_slice", a);
}
static RESULT korb_m_enum_each_cons(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    RESULT av = korb_m_enum_to_a(c, slots, self, a);
    if (UNLIKELY(av.state != KORB_NORMAL)) return av;
    slots[0] = av.value;
    RESULT r = korb_m_ary_each_cons(c, slots + 1, KORB_REF_AT(&slots[0]), a, block, def_env, cself);
    if (block != NULL && r.state == KORB_NORMAL) return RESULT_OK(KORB_REF_GET(self));   /* block form returns the enumerator */
    return korb_enum_redesc(c, slots, self, r, "each_cons", a);
}
