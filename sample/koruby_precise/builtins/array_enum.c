/* koruby_precise — array_enum.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Array enumerable / aggregate methods -------------------------------- */
static RESULT korb_lazy_new(CTX *c, VALUE *slots, VALUE source, uint8_t mode);   /* enumerator.c */
static RESULT korb_arithseq_new(CTX *c, VALUE *slots, VALUE recv, VALUE a0, VALUE a1, uint8_t nargs, uint8_t is_pct);   /* arithseq.c */


/* Array#index is a true alias of #find_index (registered to the same CFUNC). */

static RESULT korb_m_ary_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block != NULL && VALUE_SLICE_LEN(a) > 0) korb_warn(c, slots, "given block not used");   /* arg wins */
    if (block != NULL && VALUE_SLICE_LEN(a) == 0) {  /* block form: count truthy yields */
        korb_sword_t n = 0;
        for (uint32_t i = 0; ; i++) {
            const KorbArray *ary = SELF_ARY;
            if (i >= ary->len) break;
            slots[0] = korb_items_data(ary->items)[i];
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) n++;
        }
        return RESULT_OK(LONG2FIX(n));
    }
    if (VALUE_SLICE_LEN(a) == 0) return RESULT_OK(LONG2FIX(SELF_ARY->len));
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* needle (root across element == dispatch) */
    const uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    korb_sword_t cnt = 0;
    for (uint32_t i = 0; i < len; i++) {
        const VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        if (KORB_OBJECT_P(e) || KORB_OBJECT_P(slots[0])) {  /* user == → dispatch (element == needle) */
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

static RESULT korb_m_ary_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE init = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : LONG2FIX(0);
    const KorbArray *ary = SELF_ARY;
    bool any_float = KORB_FLOAT_P(init), all_num = FIXNUM_P(init) || KORB_FLOAT_P(init);
    for (uint32_t i = 0; all_num && i < ary->len; i++) {
        VALUE e = korb_items_data(ary->items)[i];
        if (KORB_FLOAT_P(e)) any_float = true; else if (!FIXNUM_P(e)) all_num = false;
    }
    if (all_num) {                                   /* numeric fast path */
        if (any_float) {                             /* Kahan-Babuska-Neumaier compensated sum (CRuby) */
            double sum; korb_num_to_d(init, &sum);
            double comp = 0.0;
            const KorbArray *ar = SELF_ARY;
            for (uint32_t i = 0; i < ar->len; i++) {
                double x; korb_num_to_d(korb_items_data(ar->items)[i], &x);
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
        for (uint32_t i = 0; i < ary->len; i++) acc += FIX2LONG(korb_items_data(ary->items)[i]);
        return RESULT_OK(LONG2FIX(acc));
    }
    slots[0] = init;                                 /* general fold: init + e0 + e1 + ... via + operator */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ar = SELF_ARY;
        if (i >= ar->len) break;
        slots[1] = korb_items_data(ar->items)[i];
        RESULT r = korb_plus_slow(c, slots + 2, VALUE_REF_AT(&slots[0]), slots[1], 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    return RESULT_OK(slots[0]);
}
/* Array#sum with a block: init + block(e0) + block(e1) + ... (+ via dispatch). */
static RESULT korb_m_ary_sum_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) return korb_m_ary_sum(c, slots, self, a);
    const uint32_t plus = korb_intern(c->vm, "+", 1);
    slots[0] = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : LONG2FIX(0);   /* acc (recv) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ar = SELF_ARY;
        if (i >= ar->len) break;
        slots[1] = korb_items_data(ar->items)[i];
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[1] = r.value;                          /* arg = block(e); recv = slots[0] */
        r = korb_send(c, slots + 2, plus, 0, 1);     /* acc = acc + block(e) */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    return RESULT_OK(slots[0]);
}

/* min (want=-1) / max (want=1) by <=> */
static RESULT korb_cmp_spaceship(CTX *c, VALUE *slots, VALUE a, VALUE b, int *out);   /* fwd */
static RESULT korb_ary_minmax(CTX *c, VALUE *slots, VALUE_REF self, int want) {
    const uint32_t len = SELF_ARY->len;
    if (len == 0) return RESULT_OK(KORB_NIL);
    slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[0];   /* best (rooted across any <=> dispatch GC) */
    for (uint32_t i = 1; i < len; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        int cmp;
        if (UNLIKELY(KORB_OBJECT_P(e) || KORB_OBJECT_P(slots[0]))) {   /* user/Comparable → dispatch <=> */
            slots[1] = e;                                             /* root e across the dispatch */
            CHECK(korb_cmp_spaceship(c, slots + 2, slots[1], slots[0], &cmp));
            e = slots[1];                                            /* re-read (may have moved) */
        } else {
            cmp = korb_cmp_full(c, e, slots[0]);
            if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(e), korb_type_name(slots[0]));
        }
        if (cmp == want) slots[0] = e;
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_cmp_block(CTX *c, VALUE *slots, VALUE lhs, VALUE rhs,
                             NODE *block, VALUE *def_env, VALUE *cself, int *out);
/* min (want=-1) / max (want=1) via a comparator block.  The running best lives
 * in slots[0] (rooted); each yield may GC, so the element is re-read from the
 * rooted array after the compare. */
static RESULT korb_ary_minmax_blk(CTX *c, VALUE *slots, VALUE_REF self, int want,
                                  NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if (len == 0) return RESULT_OK(KORB_NIL);
    slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[0];   /* best (rooted) */
    for (uint32_t i = 1; i < len; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        int cmp;
        CHECK(korb_cmp_block(c, slots + 1, e, slots[0], block, def_env, cself, &cmp));
        if (cmp == want) slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* re-read post-yield */
    }
    return RESULT_OK(slots[0]);
}
/* min(n)/max(n): the n smallest (want=-1) / largest (want=1), sorted accordingly. */
static RESULT korb_ary_minmax_n(CTX *c, VALUE *slots, VALUE_REF self, int want, korb_sword_t n, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
    if (n == 0) return korb_ary_new(c, slots, 0);          /* max(0)/min(0) → [] (no comparison) */
    uint32_t len = SELF_ARY->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, len));        /* sorted-ascending working copy */
    VALUE_REF tmp = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < len; i++) CHECK(korb_ary_push_val(c, slots + 1, tmp, korb_items_data(SELF_ARY->items)[i]));
    /* insertion sort ascending.  A user/Comparable element needs <=> dispatch
     * (may GC/move tmp) → re-fetch the items pointer each step and root the key
     * in slots[1]; native types stay on the GC-free korb_cmp_full path. */
    for (uint32_t i = 1; i < len; i++) {
        slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(tmp))->items)[i];   /* key (rooted) */
        uint32_t j = i;
        while (j > 0) {
            const VALUE left = korb_items_data(VAL2ARY(VALUE_REF_GET(tmp))->items)[j-1];
            int cmp;
            if (block != NULL) {                             /* min(n)/max(n) { |a,b| ... } — block is the comparator */
                VALUE cmpargs[2] = { left, slots[1] };
                RESULT cr = korb_block_yield(c, slots + 2, block, def_env, cmpargs, 2, cself);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                if (UNLIKELY(cr.value == KORB_NIL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(left), korb_type_name(slots[1]));
                const korb_sword_t cv = FIX2LONG(cr.value);
                cmp = cv < 0 ? -1 : cv > 0 ? 1 : 0;
            } else if (UNLIKELY(KORB_OBJECT_P(left) || KORB_OBJECT_P(slots[1]))) {
                CHECK(korb_cmp_spaceship(c, slots + 2, left, slots[1], &cmp));
            } else {
                cmp = korb_cmp_full(c, left, slots[1]);
                if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(left), korb_type_name(slots[1]));
            }
            if (cmp <= 0) break;
            KorbArrayItems *const it = VAL2ARY(VALUE_REF_GET(tmp))->items;   /* re-fetch (dispatch may have moved tmp) */
            ARO_STORE(c, it, &korb_items_data(it)[j], korb_items_data(it)[j-1]); j--;
        }
        KorbArrayItems *const it = VAL2ARY(VALUE_REF_GET(tmp))->items;
        ARO_STORE(c, it, &korb_items_data(it)[j], slots[1]);
    }
    uint32_t take = (uint32_t)n; if (take > len) take = len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, take));
    VALUE_REF dst = VALUE_REF_AT(slots + 1);              /* tmp(slots[0]) stays rooted below */
    for (uint32_t i = 0; i < take; i++) {
        uint32_t src = want < 0 ? i : len - 1 - i;        /* min: ascending; max: descending */
        CHECK(korb_ary_push_val(c, slots + 2, dst, korb_items_data(VAL2ARY(VALUE_REF_GET(tmp))->items)[src]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_min(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                             NODE *block, VALUE *def_env, VALUE *cself) {
    korb_sword_t n;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(VALUE_SLICE_GET(a, 0), &n)) return korb_ary_minmax_n(c, slots, self, -1, n, block, def_env, cself);
    if (block != NULL) return korb_ary_minmax_blk(c, slots, self, -1, block, def_env, cself);
    return korb_ary_minmax(c, slots, self, -1);
}
static RESULT korb_m_ary_max(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                             NODE *block, VALUE *def_env, VALUE *cself) {
    korb_sword_t n;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(VALUE_SLICE_GET(a, 0), &n)) return korb_ary_minmax_n(c, slots, self, 1, n, block, def_env, cself);
    if (block != NULL) return korb_ary_minmax_blk(c, slots, self, 1, block, def_env, cself);
    return korb_ary_minmax(c, slots, self,  1);
}
static RESULT korb_m_ary_transpose(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t rows = SELF_ARY->len;
    if (rows == 0) return korb_ary_new(c, slots, 0);
    /* coerce each row to an Array via #to_ary into a rooted src array. */
    VALUE_REF src = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, rows)));
    const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
    for (uint32_t i = 0; i < rows; i++) {
        slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        if (!KORB_ARRAY_P(slots[0])) {
            if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce_p(c, slots + 1, &slots[0], to_ary)) {
                RESULT r = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                slots[0] = r.value;
            }
            if (UNLIKELY(!KORB_ARRAY_P(slots[0]))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i]));
        }
        CHECK(korb_ary_push_val(c, slots + 1, src, slots[0]));
    }
    #define SRC_I(i) (VAL2ARY(korb_items_data(VAL2ARY(VALUE_REF_GET(src))->items)[(i)]))
    const uint32_t cols = SRC_I(0)->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, cols));           /* result rows */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    for (uint32_t j = 0; j < cols; j++) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 2, rows));       /* one output row */
        VALUE_REF row = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < rows; i++) {
            const KorbArray *e = SRC_I(i);
            if (UNLIKELY(e->len != cols)) return korb_raise(c, slots, KORB_E_INDEX, 0, "element size differs (%u should be %u)", e->len, cols);
            CHECK(korb_ary_push_val(c, slots + 2, row, korb_items_data(e->items)[j]));
        }
        CHECK(korb_ary_push_val(c, slots + 2, out, VALUE_REF_GET(row)));
    }
    #undef SRC_I
    return RESULT_OK(VALUE_REF_GET(out));
}
/* minmax via comparator block — replicates CRuby's pairwise scan: seed min/max
 * from the first pair, then for each subsequent pair route the smaller against
 * min and the larger against max.  This reproduces CRuby's exact behaviour even
 * for a non-antisymmetric comparator (e.g. the degenerate `{|x| x}`).  min/max
 * land in slots[0]/slots[1]; A(i) re-reads the (possibly moved) element. */
static RESULT korb_ary_minmax_pair_blk(CTX *c, VALUE *slots, VALUE_REF self,
                                       NODE *block, VALUE *def_env, VALUE *cself) {
#define A(idx) (korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[(idx)])
    uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
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
static RESULT korb_m_ary_minmax(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block != NULL) {
        CHECK(korb_ary_minmax_pair_blk(c, slots, self, block, def_env, cself));  /* fills slots[0..1] */
    } else {
        slots[0] = UNWRAP(korb_ary_minmax(c, slots, self, -1));        /* min (nil if empty) */
        slots[1] = UNWRAP(korb_ary_minmax(c, slots + 1, self, 1));     /* max */
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(VALUE_REF_GET(VALUE_REF_AT(&slots[2])));
}

/* Comparator-block helper: yield (lhs, rhs) to `block`, reduce the result to a
 * sign in *out (-1/0/1) with <=> semantics.  A nil / non-numeric result raises
 * ArgumentError; a block exception propagates.  slots[0]/slots[1] hold the two
 * (rooted) operands across the yield, so the caller need not root them. */
static RESULT korb_cmp_block(CTX *c, VALUE *slots, VALUE lhs, VALUE rhs,
                             NODE *block, VALUE *def_env, VALUE *cself, int *out) {
    slots[0] = lhs; slots[1] = rhs;                       /* stage + root args */
    RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[0], 2, cself);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    const VALUE v = r.value;
    double d;
    if (UNLIKELY(v == KORB_NIL || !korb_num_to_d(v, &d)))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed",
                          korb_type_name(lhs), korb_type_name(rhs));
    *out = d < 0 ? -1 : d > 0 ? 1 : 0;
    return RESULT_OK(KORB_NIL);
}

/* qsort_r comparator for the default-<=> sort: korb_cmp_full doesn't allocate
 * for builtin-comparable types, so the array pointer stays put during qsort.
 * `cmp==2` (incomparable) is latched into err; we raise after the sort. */
struct korb_sortctx { CTX *c; int err; };
static int korb_sort_cmp(const void *pa, const void *pb, void *arg) {
    struct korb_sortctx *const sc = (struct korb_sortctx *)arg;
    int r = korb_cmp_full(sc->c, *(const VALUE *)pa, *(const VALUE *)pb);
    if (UNLIKELY(r == 2)) { sc->err = 1; return 0; }
    return r;
}

/* Typed in-place sort for an all-Fixnum array.  A Fixnum VALUE is `(n<<1)|1`,
 * a strictly monotonic map of n, so comparing the raw VALUEs as signed
 * korb_sword_t yields integer order — no per-compare callback (the qsort_r PLT
 * indirection + korb_cmp_full type dispatch that dominate the generic path).
 * Median-of-three quicksort with an insertion-sort cutoff; tail-recursion on
 * the larger side is looped to bound stack depth.  Reordering existing Fixnums
 * creates no heap edges → no write barrier needed (matches korb_m_ary_sort). */
static inline void korb_fix_insort(VALUE *const d, const korb_sword_t lo, const korb_sword_t hi) {
    for (korb_sword_t i = lo + 1; i <= hi; i++) {
        const VALUE k = d[i]; korb_sword_t j = i - 1;
        while (j >= lo && (korb_sword_t)d[j] > (korb_sword_t)k) { d[j + 1] = d[j]; j--; }
        d[j + 1] = k;
    }
}
static void korb_fix_qsort(VALUE *const d, korb_sword_t lo, korb_sword_t hi) {
    while (hi - lo > 16) {
        const korb_sword_t mid = lo + ((hi - lo) >> 1);   /* median-of-three pivot into d[hi-1] */
        if ((korb_sword_t)d[mid] < (korb_sword_t)d[lo])     { VALUE t = d[mid]; d[mid] = d[lo]; d[lo] = t; }
        if ((korb_sword_t)d[hi]  < (korb_sword_t)d[lo])     { VALUE t = d[hi];  d[hi]  = d[lo]; d[lo] = t; }
        if ((korb_sword_t)d[hi]  < (korb_sword_t)d[mid])    { VALUE t = d[hi];  d[hi]  = d[mid]; d[mid] = t; }
        const VALUE pivot = d[mid];
        { VALUE t = d[mid]; d[mid] = d[hi - 1]; d[hi - 1] = t; }   /* park pivot at hi-1 */
        korb_sword_t i = lo, j = hi - 1;
        for (;;) {
            do i++; while ((korb_sword_t)d[i] < (korb_sword_t)pivot);
            do j--; while ((korb_sword_t)d[j] > (korb_sword_t)pivot);
            if (i >= j) break;
            VALUE t = d[i]; d[i] = d[j]; d[j] = t;
        }
        { VALUE t = d[i]; d[i] = d[hi - 1]; d[hi - 1] = t; }       /* restore pivot to its seat */
        if (i - lo < hi - i) { korb_fix_qsort(d, lo, i - 1); lo = i + 1; }   /* recurse smaller side */
        else                 { korb_fix_qsort(d, i + 1, hi); hi = i - 1; }
    }
    korb_fix_insort(d, lo, hi);
}
/* Dispatch `a <=> b` (user / Comparable) → *out = sign(-1/0/1).  May GC (caller
 * re-fetches the array after the call).  nil result = incomparable → raise. */
static RESULT korb_cmp_spaceship(CTX *c, VALUE *slots, VALUE a, VALUE b, int *out) {
    slots[0] = a; slots[1] = b;
    RESULT r = korb_send(c, slots + 2, c->vm->mid_cmp, 0, 1);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (UNLIKELY(!FIXNUM_P(r.value)))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed",
                          korb_type_name(slots[0]), korb_type_name(slots[1]));
    const korb_sword_t v = FIX2LONG(r.value);
    *out = (v > 0) - (v < 0);
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_sort(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                              NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = korb_items_data(SELF_ARY->items)[i];
        CHECK(korb_ary_push_val(c, slots + 2, dst, e));   /* scratch above key slot */
    }
    KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
    if (block == NULL) {
        /* default <=> : O(n log n) qsort.  korb_cmp_full is GC-free for builtin
         * types so the items pointer stays valid; reordering existing elements
         * creates no new heap edges (all already tracked) → no write barrier. */
        VALUE *const dd0 = korb_items_data(d->items);
        const uint32_t dn = d->len;
        bool all_fix = true, has_obj = false;
        for (uint32_t i = 0; i < dn; i++) {
            const VALUE e = dd0[i];
            if (!FIXNUM_P(e)) all_fix = false;
            if (KORB_OBJECT_P(e)) { has_obj = true; break; }   /* user object → needs <=> dispatch */
        }
        if (all_fix) {                      /* homogeneous Fixnum → callback-free typed sort */
            if (dn > 1) korb_fix_qsort(dd0, 0, (korb_sword_t)dn - 1);
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        if (has_obj) {
            /* user/Comparable objects: dispatch <=> per compare (may GC/move dst →
             * re-fetch the items pointer each time, key rooted in slots[1]).  Stable
             * binary-insertion sort: O(n log n) dispatches (the dispatch dominates;
             * the element shifts are cheap pointer writes).  `cmp<=0` keeps the key
             * after equal elements → stable. */
            const uint32_t len = d->len;
            for (uint32_t i = 1; i < len; i++) {
                slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(dst))->items)[i];   /* key (rooted) */
                uint32_t lo = 0, hi = i;
                while (lo < hi) {                                         /* find insertion point in [0,i) */
                    const uint32_t mid = (lo + hi) >> 1;
                    const VALUE mv = korb_items_data(VAL2ARY(VALUE_REF_GET(dst))->items)[mid];
                    int cmp = 0;
                    CHECK(korb_cmp_spaceship(c, slots + 2, mv, slots[1], &cmp));   /* dst[mid] <=> key */
                    if (cmp <= 0) lo = mid + 1; else hi = mid;
                }
                KorbArrayItems *dit = VAL2ARY(VALUE_REF_GET(dst))->items;  /* no dispatch below → stable ptr */
                for (uint32_t j = i; j > lo; j--)
                    ARO_STORE(c, dit, &korb_items_data(dit)[j], korb_items_data(dit)[j-1]);     /* shift [lo,i) right by 1 */
                ARO_STORE(c, dit, &korb_items_data(dit)[lo], slots[1]);
            }
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        struct korb_sortctx sc = { c, 0 };
        qsort_r(korb_items_data(d->items), d->len, sizeof(VALUE), korb_sort_cmp, &sc);
        if (UNLIKELY(sc.err)) {
            const VALUE *dd = korb_items_data(VAL2ARY(VALUE_REF_GET(dst))->items);
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed",
                              korb_type_name(dd[0]), korb_type_name(d->len > 1 ? dd[1] : KORB_NIL));
        }
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    /* comparator-block insertion sort: each compare yields → may GC/move dst.
     * Re-fetch the items pointer after every yield; the lifted key is rooted in
     * slots[1].  Matches CRuby's stable insertion order for these inputs. */
    uint32_t len = d->len;
    for (uint32_t i = 1; i < len; i++) {
        slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(dst))->items)[i];   /* key (rooted) */
        uint32_t j = i;
        while (j > 0) {
            VALUE left = korb_items_data(VAL2ARY(VALUE_REF_GET(dst))->items)[j-1];
            int cmp;
            CHECK(korb_cmp_block(c, slots + 2, left, slots[1], block, def_env, cself, &cmp));
            if (cmp <= 0) break;
            KorbArray *dd = VAL2ARY(VALUE_REF_GET(dst));           /* re-fetch post-yield */
            ARO_STORE(c, dd->items, &korb_items_data(dd->items)[j], korb_items_data(dd->items)[j-1]); j--;
        }
        KorbArrayItems *dit = VAL2ARY(VALUE_REF_GET(dst))->items;
        ARO_STORE(c, dit, &korb_items_data(dit)[j], slots[1]);
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* sort_by: build parallel value+key arrays via the block, insertion-sort by key. */
static RESULT korb_m_ary_sort_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#sort_by");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        VALUE_REF vals = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    VALUE_REF keys = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;                              /* root key */
        CHECK(korb_ary_push_val(c, slots + 4, vals, slots[2]));
        CHECK(korb_ary_push_val(c, slots + 4, keys, slots[3]));
    }
    KorbArray *vd = VAL2ARY(VALUE_REF_GET(vals)), *kd = VAL2ARY(VALUE_REF_GET(keys));
    KorbArrayItems *const vit = vd->items, *const kit = kd->items;
    const VALUE *vdat = korb_items_data(vit), *kdat = korb_items_data(kit);
    for (uint32_t i = 1; i < vd->len; i++) {             /* fast lockstep insertion sort by scalar key, no alloc/GC */
        VALUE vk = vdat[i], kk = kdat[i]; uint32_t j = i;
        while (j > 0) {
            int cmp = korb_cmp_full(c, kdat[j-1], kk);
            if (UNLIKELY(cmp == 2)) goto dispatch_sort;  /* key not comparable by value → redo via <=> (GC-safe) */
            if (cmp <= 0) break;
            ARO_STORE(c, vit, &vdat[j], vdat[j-1]); ARO_STORE(c, kit, &kdat[j], kdat[j-1]); j--;
        }
        ARO_STORE(c, vit, &vdat[j], vk); ARO_STORE(c, kit, &kdat[j], kk);
    }
    return RESULT_OK(VALUE_REF_GET(vals));

  dispatch_sort:;   /* a block key needs <=> dispatch (Comparable user object): index-based
                     * insertion sort, re-reading the (movable) arrays each step and rooting the
                     * in-flight val/key in slots.  Matches Array#sort's object path. */
    {
        const uint32_t len = VAL2ARY(VALUE_REF_GET(keys))->len;
        for (uint32_t i = 1; i < len; i++) {
            slots[2] = korb_items_data(VAL2ARY(VALUE_REF_GET(vals))->items)[i];   /* val_i (rooted) */
            slots[3] = korb_items_data(VAL2ARY(VALUE_REF_GET(keys))->items)[i];   /* key_i (rooted) */
            uint32_t j = i;
            while (j > 0) {
                const VALUE prevk = korb_items_data(VAL2ARY(VALUE_REF_GET(keys))->items)[j-1];
                int cmp = 0;
                CHECK(korb_cmp_spaceship(c, slots + 4, prevk, slots[3], &cmp));   /* keys[j-1] <=> key_i */
                if (cmp <= 0) break;
                KorbArray *const vv = VAL2ARY(VALUE_REF_GET(vals));
                ARO_STORE(c, vv->items, &korb_items_data(vv->items)[j], korb_items_data(vv->items)[j-1]);
                KorbArray *const kq = VAL2ARY(VALUE_REF_GET(keys));
                ARO_STORE(c, kq->items, &korb_items_data(kq->items)[j], korb_items_data(kq->items)[j-1]);
                j--;
            }
            KorbArray *const vv = VAL2ARY(VALUE_REF_GET(vals));
            ARO_STORE(c, vv->items, &korb_items_data(vv->items)[j], slots[2]);
            KorbArray *const kq = VAL2ARY(VALUE_REF_GET(keys));
            ARO_STORE(c, kq->items, &korb_items_data(kq->items)[j], slots[3]);
        }
        return RESULT_OK(VALUE_REF_GET(vals));
    }
}
/* sort_by!: sort in place by block key (sort_by then copy back into self). */
static RESULT korb_m_ary_sort_by_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));   /* CRuby raises FrozenError upfront, even on an empty array */
    RESULT sr = korb_m_ary_sort_by(c, slots, self, a, block, def_env, cself);   /* sorted copy at slots[0] */
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[0] = sr.value;                                  /* root the sorted array */
    VALUE_REF sorted = VALUE_REF_AT(&slots[0]);
    VAL2ARY(VALUE_REF_GET(self))->len = 0;
    uint32_t n = VAL2ARY(VALUE_REF_GET(sorted))->len;
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(sorted))->items)[i];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* self.to_enum(:meth) — for no-block enumerator returns. */
static RESULT korb_ary_to_enum(CTX *c, VALUE *slots, VALUE_REF self, const char *meth) {
    slots[0] = VALUE_REF_GET(self);
    slots[1] = ID2SYM(korb_intern(c->vm, meth, (uint32_t)strlen(meth)));
    return korb_send_impl(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1, NULL, NULL, NULL);
}
/* min_by(want=-1) / max_by(want=1): element with the extreme block key. */
static RESULT korb_ary_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *cself, int want) {
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "min_by", 6)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    slots[0] = KORB_NIL;   /* best value */
    slots[1] = KORB_NIL;   /* best key */
    bool have = false;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = korb_items_data(ary->items)[i];
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
static RESULT korb_ary_minmax_by_n(CTX *c, VALUE *slots, VALUE_REF self, VALUE nv, NODE *block, VALUE *def_env, VALUE *cself, bool want_max) {
    korb_sword_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(nv));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative size (%ld)", (long)n);
    RESULT sr = korb_m_ary_sort_by(c, slots, self, VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);   /* ascending by key */
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[0] = sr.value;                                 /* sorted copy (rooted) */
    const uint32_t len = VAL2ARY(slots[0])->len;
    uint32_t take = (n < (korb_sword_t)len) ? (uint32_t)n : len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, take));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < take; i++) {
        const KorbArray *const s = VAL2ARY(slots[0]);    /* re-read after push (GC) */
        const VALUE e = want_max ? korb_items_data(s->items)[len - 1 - i]   /* last n, largest first */
                                 : korb_items_data(s->items)[i];            /* first n, smallest first */
        CHECK(korb_ary_push_val(c, slots + 2, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_min_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) return korb_ary_to_enum(c, slots, self, "min_by");
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) return korb_ary_minmax_by_n(c, slots, self, VALUE_SLICE_GET(a, 0), block, def_env, cself, false);
    return korb_ary_minmax_by(c, slots, self, block, def_env, cself, -1);
}
static RESULT korb_m_ary_max_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) return korb_ary_to_enum(c, slots, self, "max_by");
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) return korb_ary_minmax_by_n(c, slots, self, VALUE_SLICE_GET(a, 0), block, def_env, cself, true);
    return korb_ary_minmax_by(c, slots, self, block, def_env, cself,  1);
}
static RESULT korb_m_ary_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) return korb_ary_to_enum(c, slots, self, "minmax_by");
    slots[0] = UNWRAP(korb_ary_minmax_by(c, slots, self, block, def_env, cself, -1));
    slots[1] = UNWRAP(korb_ary_minmax_by(c, slots + 1, self, block, def_env, cself, 1));
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(VALUE_REF_GET(VALUE_REF_AT(&slots[2])));
}
/* filter_map: collect block results that are truthy. */
static RESULT korb_m_ary_filter_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#filter_map");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* flat_map: map then flatten one level (Array results spliced). */
static RESULT korb_m_ary_flat_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) {                          /* no block → Enumerator tagged op=3 so .with_index/.each flat-map */
        slots[0] = UNWRAP(korb_enum_desc(c, slots, VALUE_REF_GET(self), "flat_map"));
        RESULT er = korb_enum_new(c, slots + 1, VALUE_REF_GET(self), slots[0]);
        if (er.state == KORB_NORMAL) VAL2ENUM(er.value)->op = 3;
        return er;
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;                              /* root result */
        if (!KORB_ARRAY_P(slots[0]) && KORB_OBJECT_P(slots[0])) {   /* non-Array result → try #to_ary */
            const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
            if (korb_responds_to(c, slots[0], to_ary)) {
                RESULT tr = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);   /* recv at slots[0] */
                if (UNLIKELY(tr.state != KORB_NORMAL)) return tr;
                if (UNLIKELY(tr.value != KORB_NIL && !KORB_ARRAY_P(tr.value)))
                    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Array (%s#to_ary gives %s)",
                                      korb_coerce_name(c, slots[0]), korb_coerce_name(c, slots[0]), korb_type_name(tr.value));
                if (KORB_ARRAY_P(tr.value)) slots[0] = tr.value;   /* nil → keep the original element as-is */
            }
        }
        if (KORB_ARRAY_P(slots[0])) {
            uint32_t m = VAL2ARY(slots[0])->len;
            for (uint32_t j = 0; j < m; j++) {
                VALUE e = korb_items_data(VAL2ARY(slots[0])->items)[j];
                CHECK(korb_ary_push_val(c, slots + 1, dst, e));
            }
        } else {
            CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* partition: [truthy_elems, falsy_elems]. */
static RESULT korb_m_ary_partition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#partition");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        VALUE_REF yes = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    VALUE_REF no  = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 3, KORB_TRUTHY(r.value) ? yes : no, slots[2]));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));    VALUE_REF out = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[1]));
    return RESULT_OK(VALUE_REF_GET(out));
}
/* group_by: Hash{ block_key => [elems...] }. */
static RESULT korb_m_ary_group_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#group_by");
    slots[0] = UNWRAP(korb_hash_new(c, slots, 4));       VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[1] = korb_items_data(ary->items)[i];                  /* element */
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[2] = r.value;                              /* key */
        RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots + 4, h, slots[2], &ferr);   /* CTX-aware (custom eql?/hash key) */
        if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
        if (idx < 0) {                                   /* new bucket array */
            slots[3] = UNWRAP(korb_ary_new(c, slots + 4, 4));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_hash_set(c, slots + 4, h, VALUE_REF_AT(&slots[2]), slots[3]));
        } else {
            VALUE bucket = korb_items_data(VAL2HASH(VALUE_REF_GET(h))->items)[2 * idx + 1];
            slots[3] = bucket;
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(h));
}

/* grep(pat)(keep=1) / grep_v(pat)(keep=0): select by `pat === elem`, optional block map. */
static RESULT korb_ary_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool keep) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    const bool restore_md = (block == NULL);                        /* CRuby leaves $~ alone unless a block runs */
    VALUE_REF saved_md = SLOTS_PUSH(slots, restore_md ? korb_re_get_lastmatch(c) : KORB_NIL);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_items_data(ary->items)[i];                  /* root elem across yield */
        const VALUE _grep_pat = VALUE_SLICE_GET(a, 0);
        bool _m;
        if (KORB_REGEXP_P(_grep_pat)) {
            slots[1] = _grep_pat;                                   /* root the pattern (a Regexp is movable) across the build */
            _m = korb_re_caseeq_backref(c, slots + 2, VALUE_REF_GET(VALUE_REF_AT(&slots[1])), slots[0]);
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
    if (restore_md) korb_re_set_lastmatch(c, VALUE_REF_GET(saved_md));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)   { return korb_ary_grep(c, slots, self, a, block, def_env, cself, true); }
static RESULT korb_m_ary_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_ary_grep(c, slots, self, a, block, def_env, cself, false); }

static RESULT korb_m_ary_sort_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                   NODE *block, VALUE *def_env, VALUE *cself) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    (void)a;
    if (block == NULL) {
        KorbArray *d = VAL2ARY(VALUE_REF_GET(self));    /* in-place; cmp does not alloc */
        KorbArrayItems *const dit = d->items;
        const VALUE *data = korb_items_data(dit);
        for (uint32_t i = 1; i < d->len; i++) {
            VALUE key = data[i]; uint32_t j = i;
            while (j > 0) {
                int cmp = korb_cmp_full(c, data[j-1], key);
                if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(data[j-1]), korb_type_name(key));
                if (cmp <= 0) break;
                ARO_STORE(c, dit, &data[j], data[j-1]); j--;
            }
            ARO_STORE(c, dit, &data[j], key);
        }
        return RESULT_OK(VALUE_REF_GET(self));
    }
    /* comparator-block in-place sort: yields may GC/move self; re-fetch items
     * after each compare, lifted key rooted in slots[0]. */
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 1; i < len; i++) {
        slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* key (rooted) */
        uint32_t j = i;
        while (j > 0) {
            VALUE left = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[j-1];
            int cmp;
            CHECK(korb_cmp_block(c, slots + 1, left, slots[0], block, def_env, cself, &cmp));
            if (cmp <= 0) break;
            KorbArray *dd = VAL2ARY(VALUE_REF_GET(self));
            ARO_STORE(c, dd->items, &korb_items_data(dd->items)[j], korb_items_data(dd->items)[j-1]); j--;
        }
        KorbArrayItems *sit = VAL2ARY(VALUE_REF_GET(self))->items;
        ARO_STORE(c, sit, &korb_items_data(sit)[j], slots[0]);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_tally(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, 0)))
        slots[0] = VALUE_SLICE_GET(a, 0);              /* tally(hash): accumulate into it */
    else
        slots[0] = UNWRAP(korb_hash_new(c, slots, 4));
    VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[1] = korb_items_data(ary->items)[i];                 /* elem (root) */
        RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots + 2, h, slots[1], &ferr);   /* CTX-aware key */
        if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
        korb_sword_t cnt = idx < 0 ? 0 : FIX2LONG(korb_items_data(VAL2HASH(VALUE_REF_GET(h))->items)[2*idx+1]);
        CHECK(korb_hash_set(c, slots + 2, h, VALUE_REF_AT(&slots[1]), LONG2FIX(cnt + 1)));
    }
    return RESULT_OK(VALUE_REF_GET(h));
}
/* Join `aref`'s array into `ms`, recursing into nested arrays, with each non-array
 * element rendered via #to_s — dispatched for user objects (which can GC), so the
 * array is re-read each step and cycle detection uses the KORB_FL_JOIN_VISITING
 * header flag (survives GC moves) rather than raw pointers.  Returns RAISE on a
 * recursive-array cycle or a propagated #to_s error.  `sepref` roots the (String
 * or nil) separator; `slots` is a fresh rooted region. */
static RESULT korb_join_rec(CTX *c, VALUE *slots, FILE *ms, VALUE_REF aref, VALUE_REF sepref, bool *first) {
    if (((AroObjectHeader *)(uintptr_t)VALUE_REF_GET(aref))->flags & KORB_FL_JOIN_VISITING)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "recursive array join");
    ((AroObjectHeader *)(uintptr_t)VALUE_REF_GET(aref))->flags |= KORB_FL_JOIN_VISITING;
    RESULT rr = RESULT_OK(KORB_NIL);
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(aref))->len; i++) {
        slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(aref))->items)[i];   /* root the element (re-read: a prior #to_s may have moved the array) */
        /* Element order per CRuby: an Array (or an object with #to_ary but no
         * #to_str) recurses; otherwise #to_str, then #to_s.  The recursion happens
         * before the separator so a nested array isn't given a leading one.  The
         * #to_str/#to_ary probes honor #respond_to_missing? (proxies/mocks); the
         * element stays rooted in slots[0] across those dispatches. */
        const uint32_t to_str_id = korb_intern(c->vm, "to_str", 6), to_ary_id = korb_intern(c->vm, "to_ary", 6);
        /* Resolve the element to either an array (recurse, no separator) or a
         * scalar String (write with separator), following CRuby's #to_str →
         * #to_ary → #to_s chain where a nil conversion falls through to the next.
         * The receiver stays at slots[0]; `scalar` is captured last (no GC before
         * the fwrite).  korb_send_impl takes the element as receiver via slots+1. */
        VALUE scalar = KORB_NIL;
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
                RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) { rr = r; goto done; }
                if (KORB_STRING_P(r.value)) scalar = r.value;           /* non-String → fprint below */
            }
        }
        if (recurse) {
            rr = korb_join_rec(c, slots + 1, ms, VALUE_REF_AT(&slots[0]), sepref, first);
            if (UNLIKELY(rr.state != KORB_NORMAL)) goto done;
            continue;                                                  /* recursion wrote its own seps + *first */
        }
        if (!*first && KORB_STRING_P(VALUE_REF_GET(sepref))) {
            const KorbString *const sep = VAL2STR(VALUE_REF_GET(sepref));
            fwrite(korb_strbuf_data(sep->buf), 1, sep->len, ms);
        }
        if (KORB_STRING_P(scalar)) fwrite(korb_strbuf_data(VAL2STR(scalar)->buf), 1, VAL2STR(scalar)->len, ms);   /* resolved scalar String */
        else korb_fprint_to_s(c, ms, slots[0]);                        /* immediate / builtin / non-String #to_s */
        *first = false;
    }
  done:
    ((AroObjectHeader *)(uintptr_t)VALUE_REF_GET(aref))->flags &= ~(uint32_t)KORB_FL_JOIN_VISITING;   /* re-read (may have moved) */
    return rr;
}
static RESULT korb_m_ary_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (SELF_ARY->len == 0) {                                       /* [].join(anything) → "" (sep not validated) */
        RESULT e = korb_str_new(c, slots, "", 0);
        if (LIKELY(e.state == KORB_NORMAL)) KORB_STR_ENC_SET(e.value, KORB_ENC_USASCII);   /* CRuby starts at US-ASCII */
        return e;
    }
    /* coerced separator parked in slots[0] so it survives the per-element to_s allocs */
    slots[0] = KORB_NIL;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        VALUE sv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!KORB_STRING_P(sv))) {                         /* coerce a #to_str separator */
            const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
            if (KORB_OBJECT_P(sv) && korb_responds_to_coerce_p(c, slots, &sv, to_str)) {
                slots[0] = sv;
                const RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                sv = sr.value;
            }
            if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
        }
        slots[0] = sv;                                              /* the (possibly coerced) String sep */
    }
    if (slots[0] == KORB_NIL) {                                    /* no explicit separator → fall back to $, */
        const VALUE ofs = korb_const_get(c->vm, korb_intern(c->vm, "$,", 2));
        if (KORB_STRING_P(ofs)) { slots[0] = ofs; korb_warn(c, slots + 1, "$, is set to non-nil value"); }
    }
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    slots[1] = VALUE_REF_GET(self);                                 /* root the array across element #to_s GC */
    bool first = true;
    /* slots[0] = separator (String|nil), slots[1] = self; join drives from slots+2.
     * The memstream buffer is C heap, unaffected by GC during #to_s dispatch. */
    RESULT jr = korb_join_rec(c, slots + 2, ms, VALUE_REF_AT(&slots[1]), VALUE_REF_AT(&slots[0]), &first);
    fclose(ms);
    if (UNLIKELY(jr.state != KORB_NORMAL)) { free(buf); return jr; }
    RESULT r = korb_str_new(c, slots + 2, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}

/* recursive permutation builder: append each `want`-length position-permutation
 * of `self` (as a fresh array) into `out`.  `cur` is the shared work array; out/
 * cur are rooted by the caller; `scratch` is a fresh cursor above them. */
static RESULT korb_perm_rec(CTX *c, VALUE *scratch, VALUE_REF self, VALUE_REF out, VALUE_REF cur,
                            uint32_t want, bool *used, uint32_t depth) {
    if (depth == want) {
        VALUE_REF copy = SLOTS_PUSH(scratch, UNWRAP(korb_ary_new(c, scratch, want)));   /* copy cur */
        for (uint32_t k = 0; k < VAL2ARY(VALUE_REF_GET(cur))->len; k++)
            CHECK(korb_ary_push_val(c, scratch + 1, copy, korb_items_data(VAL2ARY(VALUE_REF_GET(cur))->items)[k]));
        return korb_ary_push_val(c, scratch + 1, out, VALUE_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < len; i++) {
        if (used[i]) continue;
        used[i] = true;
        CHECK(korb_ary_push_val(c, scratch, cur, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i]));
        CHECK(korb_perm_rec(c, scratch, self, out, cur, want, used, depth + 1));
        KorbArray *cv = VAL2ARY(VALUE_REF_GET(cur)); cv->len--;     /* pop */
        used[i] = false;
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_permutation(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t len = SELF_ARY->len;
    korb_sword_t want = len;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        { VALUE _iv = VALUE_SLICE_GET(a, 0); if (UNLIKELY(!korb_to_index(_iv, &want))) { RESULT _cr = korb_coerce_to_int(c, slots, &_iv); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; if (!korb_to_index(_iv, &want)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); len = SELF_ARY->len; } }
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                   /* out: array of permutations */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    if (want >= 0 && (uint32_t)want <= len) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)want));   /* cur work array */
        VALUE_REF cur = VALUE_REF_AT(&slots[1]);
        bool *used = calloc(len ? len : 1, sizeof(bool));
        if (!used) { fprintf(stderr, "koruby_precise: oom (permutation)\n"); abort(); }
        RESULT rr = korb_perm_rec(c, slots + 2, self, out, cur, (uint32_t)want, used, 0);
        free(used);
        if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    }
    if (block == NULL) {
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "permutation"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(out), slots[1]);
    }
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(out))->len; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(out))->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &e, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* recursive combination builder: append each `want`-length combination (indices
 * chosen in increasing order from `start`) of self into `out`. */
static RESULT korb_comb_rec(CTX *c, VALUE *scratch, VALUE_REF self, VALUE_REF out, VALUE_REF cur,
                            uint32_t want, uint32_t start, uint32_t depth) {
    if (depth == want) {
        VALUE_REF copy = SLOTS_PUSH(scratch, UNWRAP(korb_ary_new(c, scratch, want)));
        for (uint32_t k = 0; k < VAL2ARY(VALUE_REF_GET(cur))->len; k++)
            CHECK(korb_ary_push_val(c, scratch + 1, copy, korb_items_data(VAL2ARY(VALUE_REF_GET(cur))->items)[k]));
        return korb_ary_push_val(c, scratch + 1, out, VALUE_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = start; i < len; i++) {
        CHECK(korb_ary_push_val(c, scratch, cur, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i]));
        CHECK(korb_comb_rec(c, scratch, self, out, cur, want, i + 1, depth + 1));
        KorbArray *cv = VAL2ARY(VALUE_REF_GET(cur)); cv->len--;     /* pop */
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_combination(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t len = SELF_ARY->len;
    korb_sword_t want = 0;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        { VALUE _iv = VALUE_SLICE_GET(a, 0); if (UNLIKELY(!korb_to_index(_iv, &want))) { RESULT _cr = korb_coerce_to_int(c, slots, &_iv); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; if (!korb_to_index(_iv, &want)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); len = SELF_ARY->len; } }
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                   /* out: array of combinations */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    if (want >= 0 && (uint32_t)want <= len) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)want));   /* cur work array */
        VALUE_REF cur = VALUE_REF_AT(&slots[1]);
        CHECK(korb_comb_rec(c, slots + 2, self, out, cur, (uint32_t)want, 0, 0));
    }
    if (block == NULL) {
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "combination"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(out), slots[1]);
    }
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(out))->len; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(out))->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &e, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* repeated_combination: like combination but indices may repeat (i, not i+1). */
static RESULT korb_rcomb_rec(CTX *c, VALUE *scratch, VALUE_REF self, VALUE_REF out, VALUE_REF cur,
                             uint32_t want, uint32_t start, uint32_t depth) {
    if (depth == want) {
        VALUE_REF copy = SLOTS_PUSH(scratch, UNWRAP(korb_ary_new(c, scratch, want)));
        for (uint32_t k = 0; k < VAL2ARY(VALUE_REF_GET(cur))->len; k++)
            CHECK(korb_ary_push_val(c, scratch + 1, copy, korb_items_data(VAL2ARY(VALUE_REF_GET(cur))->items)[k]));
        return korb_ary_push_val(c, scratch + 1, out, VALUE_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = start; i < len; i++) {
        CHECK(korb_ary_push_val(c, scratch, cur, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i]));
        CHECK(korb_rcomb_rec(c, scratch, self, out, cur, want, i, depth + 1));   /* i: allow reuse */
        VAL2ARY(VALUE_REF_GET(cur))->len--;
    }
    return RESULT_OK(KORB_NIL);
}
/* repeated_permutation: all length-`want` tuples with repetition (len^want). */
static RESULT korb_rperm_rec(CTX *c, VALUE *scratch, VALUE_REF self, VALUE_REF out, VALUE_REF cur,
                             uint32_t want, uint32_t depth) {
    if (depth == want) {
        VALUE_REF copy = SLOTS_PUSH(scratch, UNWRAP(korb_ary_new(c, scratch, want)));
        for (uint32_t k = 0; k < VAL2ARY(VALUE_REF_GET(cur))->len; k++)
            CHECK(korb_ary_push_val(c, scratch + 1, copy, korb_items_data(VAL2ARY(VALUE_REF_GET(cur))->items)[k]));
        return korb_ary_push_val(c, scratch + 1, out, VALUE_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < len; i++) {
        CHECK(korb_ary_push_val(c, scratch, cur, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i]));
        CHECK(korb_rperm_rec(c, scratch, self, out, cur, want, depth + 1));
        VAL2ARY(VALUE_REF_GET(cur))->len--;
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_repeated(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool perm) {
    korb_sword_t want = 0;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        { VALUE _iv = VALUE_SLICE_GET(a, 0); if (UNLIKELY(!korb_to_index(_iv, &want))) { RESULT _cr = korb_coerce_to_int(c, slots, &_iv); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; if (!korb_to_index(_iv, &want)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); } }
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                   /* out */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    if (want >= 0) {                                                /* want<0 → empty (size 0) */
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)want));   /* cur work array */
        VALUE_REF cur = VALUE_REF_AT(&slots[1]);
        CHECK(perm ? korb_rperm_rec(c, slots + 2, self, out, cur, (uint32_t)want, 0)
                   : korb_rcomb_rec(c, slots + 2, self, out, cur, (uint32_t)want, 0, 0));
    }
    if (block == NULL) {
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), perm ? "repeated_permutation" : "repeated_combination"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(out), slots[1]);
    }
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(out))->len; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(out))->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &e, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_repeated_combination(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_m_ary_repeated(c, slots, self, a, block, def_env, cself, false); }
static RESULT korb_m_ary_repeated_permutation(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_m_ary_repeated(c, slots, self, a, block, def_env, cself, true); }
/* Array#to_h — elements (or block results) must be 2-element arrays → Hash. */
static RESULT korb_m_ary_to_h(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, SELF_ARY->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[1] = korb_items_data(ary->items)[i];                        /* element / pair */
        if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[1] = r.value;
        }
        if (UNLIKELY(!KORB_ARRAY_P(slots[1]))) {               /* coerce a pair-like element via #to_ary */
            const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
            if (!korb_responds_to_coerce(c, slots + 2, slots[1], to_ary))
                return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "wrong element type %s at %u (expected array)", korb_type_name(slots[1]), i);
            RESULT ar = korb_send_impl(c, slots + 2, to_ary, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
            if (UNLIKELY(!KORB_ARRAY_P(ar.value)))
                return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "wrong element type %s at %u (expected array)", korb_type_name(slots[1]), i);
            slots[1] = ar.value;
        }
        if (UNLIKELY(VAL2ARY(slots[1])->len != 2))
            return korb_raise(c, slots + 2, KORB_E_ARGUMENT, 0, "wrong array length at %u (expected 2, was %u)", i, VAL2ARY(slots[1])->len);
        slots[2] = korb_items_data(VAL2ARY(slots[1])->items)[0];          /* key */
        VALUE val = korb_items_data(VAL2ARY(slots[1])->items)[1];
        CHECK(korb_hash_set(c, slots + 3, dst, VALUE_REF_AT(&slots[2]), val));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Array#cycle([n]) — yield elements n times (forever if n omitted); → nil. */
static RESULT korb_m_ary_cycle(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    const bool bounded = VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL;
    korb_sword_t n = 0;
    if (bounded) {
        VALUE nv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(nv, &n))) {          /* coerce count via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &nv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(nv, &n)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        }
    }
    if (block == NULL) {
        if (bounded) {                                  /* finite → eager Enumerator of repeated elements */
            const uint32_t blen = VAL2ARY(VALUE_REF_GET(self))->len;
            slots[0] = UNWRAP(korb_ary_new(c, slots, (n > 0 ? (uint32_t)n : 0) * blen));
            VALUE_REF out = VALUE_REF_AT(&slots[0]);
            for (korb_sword_t pass = 0; pass < n; pass++)
                for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(self))->len; i++)
                    CHECK(korb_ary_push_val(c, slots + 1, out, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i]));
            slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "cycle"));
            return korb_enum_new(c, slots + 2, VALUE_REF_GET(out), slots[1]);
        }
        return korb_lazy_new(c, slots, VALUE_REF_GET(self), 2);   /* unbounded → infinite lazy enum */
    }
    if (bounded && n <= 0) return RESULT_OK(KORB_NIL);
    if (SELF_ARY->len == 0) return RESULT_OK(KORB_NIL);
    for (korb_sword_t pass = 0; !bounded || pass < n; pass++) {
        for (uint32_t i = 0; ; i++) {
            const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
            if (i >= ary->len) break;
            VALUE e = korb_items_data(ary->items)[i];
            RESULT r = korb_block_yield(c, slots, block, def_env, &e, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_compact(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = korb_items_data(SELF_ARY->items)[i];
        if (e != KORB_NIL) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_compact_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    (void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    uint32_t w = 0; bool changed = false;
    for (uint32_t r = 0; r < ary->len; r++) {
        if (korb_items_data(it)[r] == KORB_NIL) { changed = true; continue; }
        if (w != r) ARO_STORE(c, it, &korb_items_data(it)[w], korb_items_data(it)[r]);
        w++;
    }
    for (uint32_t r = w; r < ary->len; r++) ARO_STORE(c, it, &korb_items_data(it)[r], KORB_NIL);
    ary->len = w;
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_ary_each_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                                  /* → Enumerator of indices */
        const uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
        slots[0] = UNWRAP(korb_ary_new(c, slots, len));
        VALUE_REF idx = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; i < len; i++) CHECK(korb_ary_push_val(c, slots + 1, idx, LONG2FIX(i)));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each_index"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(idx), slots[1]);
    }
    for (uint32_t i = 0; ; i++) {
        if (i >= VAL2ARY(VALUE_REF_GET(self))->len) break;
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_uniq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd */
/* A 64-bit hash for uniq/Set-style dedup: dispatch #hash for a user object (so
 * elements are compared by hash first, and #eql? only when hashes match, like
 * CRuby's st_table), else the intrinsic value hash. */
static RESULT korb_elem_hash(CTX *c, VALUE *slots, VALUE v, uint64_t *out) {
    if (!KORB_OBJECT_P(v)) { *out = korb_value_hash(v); return RESULT_OK(KORB_NIL); }
    slots[0] = v;
    const RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "hash", 4), 0, 0, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (FIXNUM_P(r.value)) *out = (uint64_t)FIX2LONG(r.value);
    else if (KORB_BIGNUM_P(r.value)) *out = (uint64_t)korb_mp_get_ui(VAL2BIG(r.value)->z);
    else *out = korb_value_hash(r.value);
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_uniq_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);   /* fwd */
static RESULT korb_m_ary_uniq_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    const uint32_t before = SELF_ARY->len;
    const RESULT ur = block ? korb_m_ary_uniq_b(c, slots, self, a, block, def_env, cself)
                            : korb_m_ary_uniq(c, slots, self, a);   /* CTX-aware (dispatches eql? for user objects) */
    if (UNLIKELY(ur.state != KORB_NORMAL)) return ur;
    slots[0] = ur.value;                                    /* the deduped array (rooted) */
    const uint32_t after = VAL2ARY(slots[0])->len;
    if (after == before) return RESULT_OK(KORB_NIL);        /* no dups removed */
    KorbArray *const ary = SELF_ARY;                        /* re-read after uniq's allocs */
    const KorbArray *const u = VAL2ARY(slots[0]);
    for (uint32_t i = 0; i < after; i++) ARO_STORE(c, ary->items, &korb_items_data(ary->items)[i], korb_items_data(u->items)[i]);
    ary->len = after;
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_uniq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const uint32_t n0 = SELF_ARY->len;
    uint64_t *const hashes = n0 ? (uint64_t *)malloc((size_t)n0 * sizeof(uint64_t)) : NULL;   /* stored elems' hashes (parallel to dst) */
    if (n0 && UNLIKELY(!hashes)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "out of memory");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n0)));
    const uint32_t eqm = korb_intern(c->vm, "eql?", 4);
    RESULT ret = RESULT_OK(KORB_NIL);
    for (uint32_t i = 0; i < n0; i++) {                   /* bound to the snapshot (hashes[] size); a mutating #eql? can't overflow */
        if (i >= VAL2ARY(VALUE_REF_GET(self))->len) break;   /* array shrank during iteration */
        slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* e, rooted across hash/eql? dispatch + push */
        uint64_t he;
        { RESULT hr = korb_elem_hash(c, slots + 1, slots[0], &he); if (UNLIKELY(hr.state != KORB_NORMAL)) { ret = hr; goto done; } }
        const KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
        bool seen = false;
        for (uint32_t j = 0; j < d->len; j++) {
            if (hashes[j] != he) continue;               /* hash mismatch → never call #eql? */
            const VALUE existing = korb_items_data(d->items)[j];
            if (existing == slots[0]) { seen = true; break; }   /* identity short-circuit (CRuby rb_any_cmp), even when #eql? is non-reflexive */
            if (KORB_OBJECT_P(existing) || KORB_OBJECT_P(slots[0])) {   /* same hash → e.eql?(existing) */
                slots[1] = slots[0]; slots[2] = slots[0]; slots[3] = existing;
                const RESULT r = korb_send_impl(c, slots + 4, eqm, 0, 1, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) { ret = r; goto done; }
                if (KORB_TRUTHY(r.value)) { seen = true; break; }
                d = VAL2ARY(VALUE_REF_GET(dst));         /* re-read after dispatch GC */
            } else if (korb_value_eql(existing, slots[0])) { seen = true; break; }
        }
        if (!seen) {
            const uint32_t idx = VAL2ARY(VALUE_REF_GET(dst))->len;
            RESULT pr = korb_ary_push_val(c, slots + 1, dst, slots[0]);
            if (UNLIKELY(pr.state != KORB_NORMAL)) { ret = pr; goto done; }
            hashes[idx] = he;
        }
    }
    ret = RESULT_OK(VALUE_REF_GET(dst));
done:
    free(hashes);
    return ret;
}
/* Membership test for the set operations (|, &): CRuby collects values through an
 * intermediate hash, so equivalence is by #hash + #eql? for user objects.  Non-object
 * elements fall to type-strict value/identity equality (the fast path).  *out ← found. */
static RESULT korb_arr_member_eql(CTX *c, VALUE *slots, VALUE_REF ary, VALUE elem, bool *out) {
    *out = false;
    slots[0] = elem;                                     /* root elem across #hash / #eql? dispatch */
    const bool elem_obj = KORB_OBJECT_P(elem);
    const uint32_t eqm = korb_intern(c->vm, "eql?", 4);
    uint64_t eh = 0; bool have_eh = false;
    for (uint32_t j = 0; j < VAL2ARY(VALUE_REF_GET(ary))->len; j++) {
        const VALUE cand = korb_items_data(VAL2ARY(VALUE_REF_GET(ary))->items)[j];
        if (cand == slots[0]) { *out = true; return RESULT_OK(KORB_NIL); }   /* identity short-circuit (CRuby rb_any_cmp), even when #eql? is non-reflexive */
        if (elem_obj || KORB_OBJECT_P(cand)) {           /* dispatch: same #hash then elem.eql?(cand) */
            if (!have_eh) { const RESULT hr = korb_elem_hash(c, slots + 1, slots[0], &eh); if (UNLIKELY(hr.state != KORB_NORMAL)) return hr; have_eh = true; }
            slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(ary))->items)[j];   /* cand, rooted for the hash dispatch */
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
static RESULT korb_m_ary_uniq_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) return korb_m_ary_uniq(c, slots, self, a);
    slots[0] = UNWRAP(korb_ary_new(c, slots, SELF_ARY->len));       /* result */
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));              /* seen keys */
    VALUE_REF keys = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        if (i >= VAL2ARY(VALUE_REF_GET(self))->len) break;
        slots[2] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* element */
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;                                        /* key */
        bool seen = false;
        const KorbArray *ks = VAL2ARY(VALUE_REF_GET(keys));
        for (uint32_t j = 0; j < ks->len; j++) if (korb_value_eql(korb_items_data(ks->items)[j], slots[3])) { seen = true; break; }
        if (!seen) {
            CHECK(korb_ary_push_val(c, slots + 4, keys, slots[3]));
            CHECK(korb_ary_push_val(c, slots + 4, dst, slots[2]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* recursive flatten helper: append all leaves of `src` into dst */
/* depth-limited flatten: depth<0 = full, 0 = copy as-is, >0 = that many levels. */
/* guard = the arrays on the current recursion path (seeded with the top-level
 * self); a child already present means a cycle → ArgumentError, not a SEGV. */
static RESULT korb_ary_flatten_depth(CTX *c, VALUE *slots, VALUE_REF dst, VALUE_REF src, int depth, VALUE_REF guard) {
    uint32_t n = VAL2ARY(VALUE_REF_GET(src))->len;
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(src))->items)[i];
        if (depth != 0 && !KORB_ARRAY_P(e) && KORB_OBJECT_P(e)) {        /* non-Array element with #to_ary → flatten its result */
            const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
            if (korb_responds_to_coerce_p(c, slots, &e, to_ary)) {
                slots[0] = e;
                RESULT ar = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
                if (ar.value != KORB_NIL) {                  /* nil → not coercible: leave the element as a leaf (rb_check_array_type) */
                    if (UNLIKELY(!KORB_ARRAY_P(ar.value)))
                        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Array (%s#to_ary gives %s)", korb_coerce_name(c, slots[0]), korb_coerce_name(c, slots[0]), korb_type_name(ar.value));
                    e = ar.value;
                }
            }
        }
        if (KORB_ARRAY_P(e) && depth != 0) {
            if (depth < 0) {                              /* only unlimited flatten can loop; a finite depth bounds it (and CRuby doesn't raise) */
                const KorbArray *const g = VAL2ARY(VALUE_REF_GET(guard));
                for (uint32_t j = 0; j < g->len; j++)
                    if (korb_items_data(g->items)[j] == e) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to flatten recursive array");
            }
            slots[0] = e;
            CHECK(korb_ary_push_val(c, slots + 1, guard, e));            /* push e onto the path */
            CHECK(korb_ary_flatten_depth(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), depth < 0 ? depth : depth - 1, guard));
            VAL2ARY(VALUE_REF_GET(guard))->len--;                        /* pop (e stays rooted in slots[0] until here) */
        } else {
            CHECK(korb_ary_push_val(c, slots, dst, e));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_flatten(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int depth = -1;
    korb_sword_t d;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {   /* depth arg → #to_int, else TypeError */
        VALUE dv = VALUE_SLICE_GET(a, 0);
        if (!korb_to_index(dv, &d)) {
            RESULT cr = korb_coerce_to_int(c, slots, &dv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(dv, &d)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
        }
        depth = (int)d;
    }
    uint32_t n = SELF_ARY->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));                    /* guard (arrays on the path) */
    VALUE_REF guard = VALUE_REF_AT(&slots[1]);
    CHECK(korb_ary_push_val(c, slots + 2, guard, VALUE_REF_GET(self)));
    CHECK(korb_ary_flatten_depth(c, slots + 2, dst, self, depth, guard));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_flatten_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    korb_sword_t depth = -1;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) (void)korb_to_index(VALUE_SLICE_GET(a, 0), &depth);
    bool nested = false;
    const KorbArray *a0 = VAL2ARY(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < a0->len; i++) if (KORB_ARRAY_P(korb_items_data(a0->items)[i])) { nested = true; break; }
    bool changed = nested && depth != 0;                 /* depth 0 → no flattening → nil */
    RESULT fr = korb_m_ary_flatten(c, slots, self, a);   /* flattened copy */
    if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
    slots[0] = fr.value;
    VALUE_REF flat = VALUE_REF_AT(&slots[0]);
    VAL2ARY(VALUE_REF_GET(self))->len = 0;
    uint32_t fn = VAL2ARY(VALUE_REF_GET(flat))->len;
    for (uint32_t i = 0; i < fn; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(flat))->items)[i];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}

static RESULT korb_m_ary_concat(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    const uint32_t self_len0 = VAL2ARY(VALUE_REF_GET(self))->len;   /* initial length: concat(self, self) appends the original, not the growing, self */
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {   /* concat(*arrays) */
        slots[0] = VALUE_SLICE_GET(a, k);                 /* arg (rooted; possibly coerced) */
        if (UNLIKELY(!KORB_ARRAY_P(slots[0]))) {          /* coerce via #to_ary */
            const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
            if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce(c, slots + 1, slots[0], to_ary)) {
                RESULT r = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                slots[0] = r.value;
            }
            if (!KORB_ARRAY_P(slots[0])) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, k)));
        }
        const uint32_t n = (slots[0] == VALUE_REF_GET(self)) ? self_len0 : VAL2ARY(slots[0])->len;   /* self-arg → its original length */
        for (uint32_t i = 0; i < n; i++)
            CHECK(korb_ary_push_val(c, slots + 1, self, korb_items_data(VAL2ARY(slots[0])->items)[i]));   /* re-read other (rooted) */
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* select (keep==true) / reject (keep==false) */
static RESULT korb_ary_filter(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *captured_self, bool keep) {
    /* No block → an Enumerator tagged with the filter op (e->op), so a later
     * .each / .with_index block filters rather than maps. */
    if (UNLIKELY(block == NULL)) {
        slots[0] = UNWRAP(korb_enum_desc(c, slots, VALUE_REF_GET(self), keep ? "select" : "reject"));
        RESULT er = korb_enum_new(c, slots + 1, VALUE_REF_GET(self), slots[0]);
        if (er.state == KORB_NORMAL) VAL2ENUM(er.value)->op = keep ? 1 : 2;
        return er;
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        VALUE e = korb_items_data(ary->items)[i];
        slots[0] = e;                                       /* root e across the yield */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_ary_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_ary_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_ary_filter(c, slots, self, block, def_env, captured_self, false); }

/* Enumerable#find / #detect take an optional `ifnone` callable, invoked when no
 * element matched (nil → nil). */
static RESULT korb_find_ifnone(CTX *c, VALUE *slots, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) < 1 || VALUE_SLICE_GET(a, 0) == KORB_NIL) return RESULT_OK(KORB_NIL);
    slots[0] = VALUE_SLICE_GET(a, 0);
    return korb_send(c, slots + 1, korb_intern(c->vm, "call", 4), 0, 0);
}
static RESULT korb_m_ary_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    if (UNLIKELY(block == NULL)) {   /* no block → op-4 (find: early-stop) Enumerator; .each/.with_index drives it */
        slots[0] = UNWRAP(korb_enum_desc(c, slots, VALUE_REF_GET(self), "find"));
        RESULT er = korb_enum_new(c, slots + 1, VALUE_REF_GET(self), slots[0]);
        if (er.state == KORB_NORMAL) { VAL2ENUM(er.value)->op = 4; VAL2ENUM(er.value)->size_unknown = 1; }   /* CRuby: #find has no size fn */
        return er;
    }
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        slots[0] = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[0]);
    }
    return korb_find_ifnone(c, slots, a);   /* find(ifnone): nothing matched → ifnone.call */
}

static RESULT korb_m_ary_rfind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    ARY_REQUIRE_BLOCK("Array#rfind");
    for (int64_t i = (int64_t)VAL2ARY(VALUE_REF_GET(self))->len - 1; i >= 0; i--) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if ((uint64_t)i >= ary->len) continue;
        slots[0] = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[0]);
    }
    return korb_find_ifnone(c, slots, a);   /* rfind(ifnone) — same contract as #find */
}
static RESULT korb_m_ary_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* find_index(obj): first index == obj */
        if (block != NULL) korb_warn(c, slots, "given block not used");   /* arg wins */
        slots[0] = VALUE_SLICE_GET(a, 0);            /* needle (root across element == dispatch) */
        const uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
        for (uint32_t i = 0; i < n; i++) {
            const VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* re-read each iter (dispatch may GC) */
            if (KORB_OBJECT_P(e) || KORB_OBJECT_P(slots[0])) {   /* user #== → dispatch element == needle */
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
        slots[0] = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#take_while");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!KORB_TRUTHY(r.value)) break;
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_drop_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#drop_while");
    uint32_t start = 0; bool dropping = true;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_items_data(ary->items)[i];
        if (dropping) {
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) { start = i + 1; continue; }
            dropping = false;
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    (void)start;
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_clear(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    (void)c;(void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < ary->len; i++) ARO_STORE(c, ary->items, &korb_items_data(ary->items)[i], KORB_NIL);
    ary->len = 0;
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_intersect_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) {                /* coerce via #to_ary */
        const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
        if (KORB_OBJECT_P(ov) && korb_responds_to_coerce_p(c, slots, &ov, to_ary)) {
            slots[0] = ov;
            RESULT ar = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
            ov = ar.value;
        }
        if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    slots[0] = ov;                                   /* root the (possibly coerced) other across #eql? dispatch */
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(self))->len; i++) {
        slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        bool has; CHECK(korb_arr_member_eql(c, slots + 2, VALUE_REF_AT(&slots[0]), slots[1], &has));
        if (has) return RESULT_OK(KORB_TRUE);
    }
    return RESULT_OK(KORB_FALSE);
}
/* bsearch: find-minimum (boolean block) or find-any (Integer block). Returns the
 * matching element, or nil. Array must be sorted for meaningful results. */
static RESULT korb_m_ary_bsearch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#bsearch");
    uint32_t lo = 0, hi = VAL2ARY(VALUE_REF_GET(self))->len;
    VALUE found = KORB_NIL; bool have = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[mid];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        VALUE rv = r.value;
        if (rv == KORB_TRUE || rv == KORB_FALSE || rv == KORB_NIL) {   /* find-minimum */
            if (KORB_TRUTHY(rv)) { found = slots[0]; have = true; hi = mid; }
            else lo = mid + 1;
        } else if (FIXNUM_P(rv) || KORB_FLOAT_P(rv)) {                 /* find-any (numeric) */
            double cmp; (void)korb_num_to_d(rv, &cmp);
            if (cmp == 0) return RESULT_OK(slots[0]);
            else if (cmp < 0) hi = mid;
            else lo = mid + 1;
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong element type %s (must be numeric, true, false or nil)", korb_type_name(rv));
        }
    }
    return RESULT_OK(have ? found : KORB_NIL);
}

static RESULT korb_m_ary_bsearch_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#bsearch_index");
    uint32_t lo = 0, hi = VAL2ARY(VALUE_REF_GET(self))->len;
    uint32_t found = 0; bool have = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[mid];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        VALUE rv = r.value;
        if (rv == KORB_TRUE || rv == KORB_FALSE || rv == KORB_NIL) {
            if (KORB_TRUTHY(rv)) { found = mid; have = true; hi = mid; } else lo = mid + 1;
        } else if (FIXNUM_P(rv) || KORB_FLOAT_P(rv)) {
            double cmp; (void)korb_num_to_d(rv, &cmp);
            if (cmp == 0) return RESULT_OK(LONG2FIX(mid));
            else if (cmp < 0) hi = mid; else lo = mid + 1;
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong element type %s (must be numeric, true, false or nil)", korb_type_name(rv));
        }
    }
    return RESULT_OK(have ? LONG2FIX(found) : KORB_NIL);
}
static RESULT korb_m_ary_map_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    (void)a; ARY_REQUIRE_BLOCK("Array#map!");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        KorbArray *a2 = VAL2ARY(VALUE_REF_GET(self));
        ARO_STORE(c, a2->items, &korb_items_data(a2->items)[i], r.value);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Integer#step / Float#step (generic over numeric self) */
static RESULT korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc);
static RESULT korb_enum_desc(CTX *c, VALUE *slots, VALUE recv, const char *meth);
static RESULT korb_m_num_step(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    /* keyword form `step(to:, by:)`: a trailing Hash with :to / :by maps to the
     * limit / step (limit nil ⇒ endless). */
    uint32_t na = VALUE_SLICE_LEN(a);
    VALUE kwlim = KORB_NIL, kwstep = KORB_NIL; bool kw = false;
    if (na >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, na - 1))) {
        const VALUE h = VALUE_SLICE_GET(a, na - 1);
        const int32_t ti = korb_hash_find(VAL2HASH(h), ID2SYM(korb_intern(c->vm, "to", 2)));
        const int32_t bi = korb_hash_find(VAL2HASH(h), ID2SYM(korb_intern(c->vm, "by", 2)));
        if (ti >= 0 || bi >= 0) {
            kw = true;
            if (ti >= 0) kwlim  = korb_items_data(VAL2HASH(h)->items)[2 * ti + 1];
            if (bi >= 0) kwstep = korb_items_data(VAL2HASH(h)->items)[2 * bi + 1];
            na--;
            if (na >= 1 && ti >= 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "to is given twice");   /* positional limit + to: */
            if (na >= 2 && bi >= 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step is given twice"); /* positional step + by: */
        }
    }
    const VALUE limv0 = na >= 1 ? VALUE_SLICE_GET(a, 0) : kwlim;                       /* limit (nil ⇒ endless) */
    const VALUE stepv0 = na >= 2 ? VALUE_SLICE_GET(a, 1) : (kwstep != KORB_NIL ? kwstep : LONG2FIX(1));
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
            slots[0] = VALUE_REF_GET(self);
            slots[1] = limv0;
            slots[2] = stepv0;
            return korb_send(c, slots + 3, korb_intern(c->vm, "__step_bad_enum", 15), 0, 2);
        }
        return korb_arithseq_new(c, slots, VALUE_REF_GET(self), limv0, stepv0, (uint8_t)((kw || na >= 2) ? 2 : na), 0);
    }
    if (!numeric_step)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step requires numeric arguments");
    VALUE selfv = VALUE_REF_GET(self);
    VALUE limv = limv0;
    VALUE stepv = (stepv0 == KORB_NIL) ? LONG2FIX(1) : stepv0;   /* nil step ⇒ 1 for iteration */
    bool use_float = KORB_FLOAT_P(selfv) || KORB_FLOAT_P(limv) || KORB_FLOAT_P(stepv);
    const bool collect = (block == NULL);             /* no block → materialize into an Enumerator */
    VALUE_REF dst = {0};
    if (use_float) {
        double s, lim, st;
        if (!korb_num_to_d(selfv, &s) || !korb_num_to_d(limv, &lim) || !korb_num_to_d(stepv, &st))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "step requires numeric arguments");
        if (st == 0.0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        if (collect) { slots[0] = UNWRAP(korb_ary_new(c, slots, 8)); dst = VALUE_REF_AT(&slots[0]); }  /* after reading the doubles */
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
        korb_sword_t s = FIX2LONG(selfv), lim = FIX2LONG(limv), st = FIX2LONG(stepv);
        if (st == 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        if (collect) { slots[0] = UNWRAP(korb_ary_new(c, slots, 8)); dst = VALUE_REF_AT(&slots[0]); }
        for (korb_sword_t i = s; st > 0 ? i <= lim : i >= lim; i += st) {
            if (collect) { CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i))); continue; }
            slots[0] = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    }
    if (collect) {
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "step"));   /* dst at slots[0] still rooted */
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(dst), slots[1]);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* any? (mode 0) / all? (1) / none? (2). A pattern arg (case ===) wins over a block. */
static RESULT korb_ary_quant(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self, int mode) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) > 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", (unsigned)VALUE_SLICE_LEN(a));
    const bool has_pat = VALUE_SLICE_LEN(a) >= 1;
    if (has_pat && block != NULL) korb_warn(c, slots, "given block not used");   /* pattern arg wins */
    slots[0] = has_pat ? VALUE_SLICE_GET(a, 0) : KORB_NIL;           /* pattern (rooted across dispatch) */
    const bool pat_obj = has_pat && KORB_OBJECT_P(slots[0]);         /* user object → dispatch #=== (korb_case_eq can't) */
    const uint32_t ceq = pat_obj ? korb_intern(c->vm, "===", 3) : 0;
    for (uint32_t i = 0; ; i++) {
        if (i >= VAL2ARY(VALUE_REF_GET(self))->len) break;           /* re-read len each iter (dispatch may GC) */
        slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        bool t;
        if (has_pat) {
            if (pat_obj) {                                          /* pattern.===(element) */
                slots[2] = slots[0]; slots[3] = slots[1];
                RESULT r = korb_send_impl(c, slots + 4, ceq, 0, 1, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                t = KORB_TRUTHY(r.value);
            } else {
                t = korb_case_eq(c, slots[0], slots[1]);            /* Range/Class/Regexp/== fast path */
            }
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
static RESULT korb_m_ary_any(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)  { return korb_ary_quant(c, slots, self, a, block, def_env, captured_self, 0); }
static RESULT korb_m_ary_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)  { return korb_ary_quant(c, slots, self, a, block, def_env, captured_self, 1); }
static RESULT korb_m_ary_none(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { return korb_ary_quant(c, slots, self, a, block, def_env, captured_self, 2); }

/* Numeric binop for send/symbol dispatch (op: 0+ 1- 2* 3/ 4%). Int op int → Int
 * (overflow→error), Float involved → Float; matches the node_plus/minus/... paths. */
static RESULT korb_num_binop(CTX *c, VALUE *slots, VALUE l, VALUE r, int op) {
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
static RESULT korb_m_num_add(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_num_binop(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 0); }
static RESULT korb_m_num_sub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_num_binop(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 1); }
static RESULT korb_m_num_mul(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_num_binop(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 2); }
static RESULT korb_m_num_div(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_num_binop(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 3); }
static RESULT korb_m_num_mod(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_num_binop(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 4); }
static RESULT korb_m_num_lt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cmp_slow(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 0, 0); }
static RESULT korb_m_num_le(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cmp_slow(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 1, 0); }
static RESULT korb_m_num_gt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cmp_slow(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 2, 0); }
static RESULT korb_m_num_ge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cmp_slow(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 3, 0); }
/* extract the operator mid from a sym-form reduce arg (Symbol or String). */
static bool korb_reduce_op(CTX *c, VALUE v, uint32_t *op_mid) {
    if (SYMBOL_P(v))       { *op_mid = SYM2ID(v); return true; }
    if (KORB_STRING_P(v))  { *op_mid = korb_intern(c->vm, korb_strbuf_data(VAL2STR(v)->buf), VAL2STR(v)->len); return true; }
    return false;
}
/* Resolve a reduce/inject operator arg → mid: Symbol/String, or #to_str-coercible
 * (else TypeError).  *ok set false only on the raise path. */
static RESULT korb_reduce_resolve_op(CTX *c, VALUE *slots, VALUE v, uint32_t *op_mid, bool *ok) {
    *ok = true;
    if (korb_reduce_op(c, v, op_mid)) return RESULT_OK(KORB_NIL);
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    if (KORB_OBJECT_P(v) && korb_responds_to_coerce_p(c, slots, &v, to_str)) {
        slots[0] = v;
        RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(sr.state != KORB_NORMAL)) { *ok = false; return sr; }
        if (KORB_STRING_P(sr.value)) { *op_mid = korb_intern(c->vm, korb_strbuf_data(VAL2STR(sr.value)->buf), VAL2STR(sr.value)->len); return RESULT_OK(KORB_NIL); }
    }
    *ok = false;
    return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(v));
}
static RESULT korb_m_ary_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    uint32_t op_mid;
    /* argc 2 → sym form (op=a[1], init=a[0]), block IGNORED; argc 1 + no block →
     * sym form (op=a[0]); argc 1 + block → block form with init; argc 0 → block. */
    const uint32_t na0 = VALUE_SLICE_LEN(a);
    bool sym_form = false;
    if (na0 >= 2 || (na0 == 1 && block == NULL)) {
        bool ok; RESULT rr = korb_reduce_resolve_op(c, slots, VALUE_SLICE_GET(a, na0 - 1), &op_mid, &ok);
        if (UNLIKELY(!ok)) return rr;
        sym_form = true;
    }
    if (block == NULL && !sym_form)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block or operator symbol given");
    if (sym_form) {                                        /* reduce(:+) / reduce(init, :+) [block ignored] */
        uint32_t na = na0;
        uint32_t i = 0;
        if (na >= 2) slots[0] = VALUE_SLICE_GET(a, 0);     /* explicit init */
        else { const KorbArray *ary = SELF_ARY; if (ary->len == 0) return RESULT_OK(KORB_NIL); slots[0] = korb_items_data(ary->items)[0]; i = 1; }
        for (; ; i++) {
            const KorbArray *ary = SELF_ARY;
            if (i >= ary->len) break;
            slots[1] = slots[0]; slots[2] = korb_items_data(ary->items)[i];   /* acc, elem (recv+arg) */
            RESULT r = korb_send_impl(c, slots + 3, op_mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;
        }
        return RESULT_OK(slots[0]);
    }
    uint32_t i = 0;
    if (VALUE_SLICE_LEN(a) >= 1) {
        slots[0] = VALUE_SLICE_GET(a, 0);                  /* acc = initial */
    } else {
        const KorbArray *ary = SELF_ARY;
        if (ary->len == 0) return RESULT_OK(KORB_NIL);
        slots[0] = korb_items_data(ary->items)[0];
        i = 1;
    }
    for (; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        VALUE argv[2] = { slots[0], korb_items_data(ary->items)[i] };  /* acc, elem */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;                                /* root new acc */
    }
    return RESULT_OK(slots[0]);
}

static RESULT korb_m_ary_each_with_object(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    if (UNLIKELY(block == NULL)) {                        /* no block → self.to_enum(:each_with_object, memo) */
        slots[0] = VALUE_REF_GET(self);
        slots[1] = ID2SYM(korb_intern(c->vm, "each_with_object", 16));
        slots[2] = VALUE_SLICE_GET(a, 0);
        return korb_send_impl(c, slots + 3, korb_intern(c->vm, "__to_enum_sized", 15), 0, 2, NULL, NULL, NULL);
    }
    slots[0] = VALUE_SLICE_GET(a, 0);                      /* the memo object (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        VALUE argv[2] = { korb_items_data(ary->items)[i], slots[0] };  /* elem, memo */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}


/* Enumerator#each_slice / each_cons — delegate to the Array versions on the
 * enumerator's (eager-forced) value array, so Enumerable chains like
 * `str.each_char.each_slice(2)` work.  No block → an Enumerator of the
 * slices/windows (Array#each_slice handles that). */
static RESULT korb_m_enum_each_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    RESULT av = korb_m_enum_to_a(c, slots, self, a);     /* eager array (forces a finite lazy enum) */
    if (UNLIKELY(av.state != KORB_NORMAL)) return av;
    slots[0] = av.value;
    RESULT r = korb_m_ary_each_slice(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    if (block != NULL && r.state == KORB_NORMAL) return RESULT_OK(VALUE_REF_GET(self));   /* block form returns the enumerator */
    return r;
}
static RESULT korb_m_enum_each_cons(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    RESULT av = korb_m_enum_to_a(c, slots, self, a);
    if (UNLIKELY(av.state != KORB_NORMAL)) return av;
    slots[0] = av.value;
    RESULT r = korb_m_ary_each_cons(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    if (block != NULL && r.state == KORB_NORMAL) return RESULT_OK(VALUE_REF_GET(self));   /* block form returns the enumerator */
    return r;
}
