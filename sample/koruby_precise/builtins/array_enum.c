/* koruby_precise — array_enum.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Array enumerable / aggregate methods -------------------------------- */
static RESULT korb_lazy_new(CTX *c, VALUE *slots, VALUE source, uint8_t mode);   /* enumerator.c */
static RESULT korb_arithseq_new(CTX *c, VALUE *slots, VALUE recv, VALUE a0, VALUE a1, uint8_t nargs, uint8_t is_pct);   /* arithseq.c */


static RESULT korb_m_ary_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block != NULL && VALUE_SLICE_LEN(a) == 0) {  /* block form: first truthy-yield index */
        for (uint32_t i = 0; ; i++) {
            const KorbArray *ary = SELF_ARY;
            if (i >= ary->len) break;
            slots[0] = ary->items->data[i];
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
        }
        return RESULT_OK(KORB_NIL);
    }
    const KorbArray *ary = SELF_ARY;
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < ary->len; i++)
        if (korb_value_eq(ary->items->data[i], needle)) return RESULT_OK(LONG2FIX(i));
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block != NULL && VALUE_SLICE_LEN(a) == 0) {  /* block form: count truthy yields */
        intptr_t n = 0;
        for (uint32_t i = 0; ; i++) {
            const KorbArray *ary = SELF_ARY;
            if (i >= ary->len) break;
            slots[0] = ary->items->data[i];
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) n++;
        }
        return RESULT_OK(LONG2FIX(n));
    }
    const KorbArray *ary = SELF_ARY;
    if (VALUE_SLICE_LEN(a) == 0) return RESULT_OK(LONG2FIX(ary->len));
    VALUE needle = VALUE_SLICE_GET(a, 0);
    intptr_t n = 0;
    for (uint32_t i = 0; i < ary->len; i++) if (korb_value_eq(ary->items->data[i], needle)) n++;
    return RESULT_OK(LONG2FIX(n));
}

static RESULT korb_m_ary_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE init = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : LONG2FIX(0);
    const KorbArray *ary = SELF_ARY;
    bool any_float = KORB_FLOAT_P(init), all_num = FIXNUM_P(init) || KORB_FLOAT_P(init);
    for (uint32_t i = 0; all_num && i < ary->len; i++) {
        VALUE e = ary->items->data[i];
        if (KORB_FLOAT_P(e)) any_float = true; else if (!FIXNUM_P(e)) all_num = false;
    }
    if (all_num) {                                   /* numeric fast path */
        if (any_float) {
            double acc; korb_num_to_d(init, &acc);
            const KorbArray *ar = SELF_ARY;
            for (uint32_t i = 0; i < ar->len; i++) { double d; korb_num_to_d(ar->items->data[i], &d); acc += d; }
            return korb_float_new(c, slots, acc);
        }
        intptr_t acc = FIX2LONG(init);
        for (uint32_t i = 0; i < ary->len; i++) acc += FIX2LONG(ary->items->data[i]);
        return RESULT_OK(LONG2FIX(acc));
    }
    slots[0] = init;                                 /* general fold: init + e0 + e1 + ... via + operator */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ar = SELF_ARY;
        if (i >= ar->len) break;
        slots[1] = ar->items->data[i];
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
        slots[1] = ar->items->data[i];
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
    slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[0];   /* best (rooted across any <=> dispatch GC) */
    for (uint32_t i = 1; i < len; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
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
    slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[0];   /* best (rooted) */
    for (uint32_t i = 1; i < len; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
        int cmp;
        CHECK(korb_cmp_block(c, slots + 1, e, slots[0], block, def_env, cself, &cmp));
        if (cmp == want) slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[i];   /* re-read post-yield */
    }
    return RESULT_OK(slots[0]);
}
/* min(n)/max(n): the n smallest (want=-1) / largest (want=1), sorted accordingly. */
static RESULT korb_ary_minmax_n(CTX *c, VALUE *slots, VALUE_REF self, int want, intptr_t n) {
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
    if (n == 0) return korb_ary_new(c, slots, 0);          /* max(0)/min(0) → [] (no comparison) */
    uint32_t len = SELF_ARY->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, len));        /* sorted-ascending working copy */
    VALUE_REF tmp = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < len; i++) CHECK(korb_ary_push_val(c, slots + 1, tmp, SELF_ARY->items->data[i]));
    /* insertion sort ascending.  A user/Comparable element needs <=> dispatch
     * (may GC/move tmp) → re-fetch the items pointer each step and root the key
     * in slots[1]; native types stay on the GC-free korb_cmp_full path. */
    for (uint32_t i = 1; i < len; i++) {
        slots[1] = VAL2ARY(VALUE_REF_GET(tmp))->items->data[i];   /* key (rooted) */
        uint32_t j = i;
        while (j > 0) {
            const VALUE left = VAL2ARY(VALUE_REF_GET(tmp))->items->data[j-1];
            int cmp;
            if (UNLIKELY(KORB_OBJECT_P(left) || KORB_OBJECT_P(slots[1]))) {
                CHECK(korb_cmp_spaceship(c, slots + 2, left, slots[1], &cmp));
            } else {
                cmp = korb_cmp_full(c, left, slots[1]);
                if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(left), korb_type_name(slots[1]));
            }
            if (cmp <= 0) break;
            KorbArrayItems *const it = VAL2ARY(VALUE_REF_GET(tmp))->items;   /* re-fetch (dispatch may have moved tmp) */
            ARO_STORE(c, it, &it->data[j], it->data[j-1]); j--;
        }
        KorbArrayItems *const it = VAL2ARY(VALUE_REF_GET(tmp))->items;
        ARO_STORE(c, it, &it->data[j], slots[1]);
    }
    uint32_t take = (uint32_t)n; if (take > len) take = len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, take));
    VALUE_REF dst = VALUE_REF_AT(slots + 1);              /* tmp(slots[0]) stays rooted below */
    for (uint32_t i = 0; i < take; i++) {
        uint32_t src = want < 0 ? i : len - 1 - i;        /* min: ascending; max: descending */
        CHECK(korb_ary_push_val(c, slots + 2, dst, VAL2ARY(VALUE_REF_GET(tmp))->items->data[src]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_min(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                             NODE *block, VALUE *def_env, VALUE *cself) {
    intptr_t n;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(VALUE_SLICE_GET(a, 0), &n)) return korb_ary_minmax_n(c, slots, self, -1, n);
    if (block != NULL) return korb_ary_minmax_blk(c, slots, self, -1, block, def_env, cself);
    return korb_ary_minmax(c, slots, self, -1);
}
static RESULT korb_m_ary_max(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                             NODE *block, VALUE *def_env, VALUE *cself) {
    intptr_t n;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(VALUE_SLICE_GET(a, 0), &n)) return korb_ary_minmax_n(c, slots, self, 1, n);
    if (block != NULL) return korb_ary_minmax_blk(c, slots, self, 1, block, def_env, cself);
    return korb_ary_minmax(c, slots, self,  1);
}
static RESULT korb_m_ary_transpose(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t rows = SELF_ARY->len;
    if (rows == 0) return korb_ary_new(c, slots, 0);
    VALUE first = SELF_ARY->items->data[0];
    if (UNLIKELY(!KORB_ARRAY_P(first))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(first));
    uint32_t cols = VAL2ARY(first)->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, cols));               /* result rows */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    for (uint32_t j = 0; j < cols; j++) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, rows));       /* one output row */
        VALUE_REF row = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < rows; i++) {
            VALUE e = SELF_ARY->items->data[i];
            if (UNLIKELY(!KORB_ARRAY_P(e) || VAL2ARY(e)->len != cols)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "element size differs");
            CHECK(korb_ary_push_val(c, slots + 2, row, VAL2ARY(e)->items->data[j]));
        }
        CHECK(korb_ary_push_val(c, slots + 2, out, VALUE_REF_GET(row)));
    }
    return RESULT_OK(VALUE_REF_GET(out));
}
/* minmax via comparator block — replicates CRuby's pairwise scan: seed min/max
 * from the first pair, then for each subsequent pair route the smaller against
 * min and the larger against max.  This reproduces CRuby's exact behaviour even
 * for a non-antisymmetric comparator (e.g. the degenerate `{|x| x}`).  min/max
 * land in slots[0]/slots[1]; A(i) re-reads the (possibly moved) element. */
static RESULT korb_ary_minmax_pair_blk(CTX *c, VALUE *slots, VALUE_REF self,
                                       NODE *block, VALUE *def_env, VALUE *cself) {
#define A(idx) (VAL2ARY(VALUE_REF_GET(self))->items->data[(idx)])
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
 * intptr_t yields integer order — no per-compare callback (the qsort_r PLT
 * indirection + korb_cmp_full type dispatch that dominate the generic path).
 * Median-of-three quicksort with an insertion-sort cutoff; tail-recursion on
 * the larger side is looped to bound stack depth.  Reordering existing Fixnums
 * creates no heap edges → no write barrier needed (matches korb_m_ary_sort). */
static inline void korb_fix_insort(VALUE *const d, const intptr_t lo, const intptr_t hi) {
    for (intptr_t i = lo + 1; i <= hi; i++) {
        const VALUE k = d[i]; intptr_t j = i - 1;
        while (j >= lo && (intptr_t)d[j] > (intptr_t)k) { d[j + 1] = d[j]; j--; }
        d[j + 1] = k;
    }
}
static void korb_fix_qsort(VALUE *const d, intptr_t lo, intptr_t hi) {
    while (hi - lo > 16) {
        const intptr_t mid = lo + ((hi - lo) >> 1);   /* median-of-three pivot into d[hi-1] */
        if ((intptr_t)d[mid] < (intptr_t)d[lo])     { VALUE t = d[mid]; d[mid] = d[lo]; d[lo] = t; }
        if ((intptr_t)d[hi]  < (intptr_t)d[lo])     { VALUE t = d[hi];  d[hi]  = d[lo]; d[lo] = t; }
        if ((intptr_t)d[hi]  < (intptr_t)d[mid])    { VALUE t = d[hi];  d[hi]  = d[mid]; d[mid] = t; }
        const VALUE pivot = d[mid];
        { VALUE t = d[mid]; d[mid] = d[hi - 1]; d[hi - 1] = t; }   /* park pivot at hi-1 */
        intptr_t i = lo, j = hi - 1;
        for (;;) {
            do i++; while ((intptr_t)d[i] < (intptr_t)pivot);
            do j--; while ((intptr_t)d[j] > (intptr_t)pivot);
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
    const intptr_t v = FIX2LONG(r.value);
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
        VALUE e = SELF_ARY->items->data[i];
        CHECK(korb_ary_push_val(c, slots + 2, dst, e));   /* scratch above key slot */
    }
    KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
    if (block == NULL) {
        /* default <=> : O(n log n) qsort.  korb_cmp_full is GC-free for builtin
         * types so the items pointer stays valid; reordering existing elements
         * creates no new heap edges (all already tracked) → no write barrier. */
        VALUE *const dd0 = d->items->data;
        const uint32_t dn = d->len;
        bool all_fix = true, has_obj = false;
        for (uint32_t i = 0; i < dn; i++) {
            const VALUE e = dd0[i];
            if (!FIXNUM_P(e)) all_fix = false;
            if (KORB_OBJECT_P(e)) { has_obj = true; break; }   /* user object → needs <=> dispatch */
        }
        if (all_fix) {                      /* homogeneous Fixnum → callback-free typed sort */
            if (dn > 1) korb_fix_qsort(dd0, 0, (intptr_t)dn - 1);
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
                slots[1] = VAL2ARY(VALUE_REF_GET(dst))->items->data[i];   /* key (rooted) */
                uint32_t lo = 0, hi = i;
                while (lo < hi) {                                         /* find insertion point in [0,i) */
                    const uint32_t mid = (lo + hi) >> 1;
                    const VALUE mv = VAL2ARY(VALUE_REF_GET(dst))->items->data[mid];
                    int cmp = 0;
                    CHECK(korb_cmp_spaceship(c, slots + 2, mv, slots[1], &cmp));   /* dst[mid] <=> key */
                    if (cmp <= 0) lo = mid + 1; else hi = mid;
                }
                KorbArrayItems *dit = VAL2ARY(VALUE_REF_GET(dst))->items;  /* no dispatch below → stable ptr */
                for (uint32_t j = i; j > lo; j--)
                    ARO_STORE(c, dit, &dit->data[j], dit->data[j-1]);     /* shift [lo,i) right by 1 */
                ARO_STORE(c, dit, &dit->data[lo], slots[1]);
            }
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        struct korb_sortctx sc = { c, 0 };
        qsort_r(d->items->data, d->len, sizeof(VALUE), korb_sort_cmp, &sc);
        if (UNLIKELY(sc.err)) {
            const VALUE *dd = VAL2ARY(VALUE_REF_GET(dst))->items->data;
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
        slots[1] = VAL2ARY(VALUE_REF_GET(dst))->items->data[i];   /* key (rooted) */
        uint32_t j = i;
        while (j > 0) {
            VALUE left = VAL2ARY(VALUE_REF_GET(dst))->items->data[j-1];
            int cmp;
            CHECK(korb_cmp_block(c, slots + 2, left, slots[1], block, def_env, cself, &cmp));
            if (cmp <= 0) break;
            KorbArray *dd = VAL2ARY(VALUE_REF_GET(dst));           /* re-fetch post-yield */
            ARO_STORE(c, dd->items, &dd->items->data[j], dd->items->data[j-1]); j--;
        }
        KorbArrayItems *dit = VAL2ARY(VALUE_REF_GET(dst))->items;
        ARO_STORE(c, dit, &dit->data[j], slots[1]);
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
        slots[2] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;                              /* root key */
        CHECK(korb_ary_push_val(c, slots + 4, vals, slots[2]));
        CHECK(korb_ary_push_val(c, slots + 4, keys, slots[3]));
    }
    KorbArray *vd = VAL2ARY(VALUE_REF_GET(vals)), *kd = VAL2ARY(VALUE_REF_GET(keys));
    KorbArrayItems *const vit = vd->items, *const kit = kd->items;
    const VALUE *vdat = vit->data, *kdat = kit->data;
    for (uint32_t i = 1; i < vd->len; i++) {             /* lockstep insertion sort, no alloc */
        VALUE vk = vdat[i], kk = kdat[i]; uint32_t j = i;
        while (j > 0) {
            int cmp = korb_cmp_full(c, kdat[j-1], kk);
            if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(kdat[j-1]), korb_type_name(kk));
            if (cmp <= 0) break;
            ARO_STORE(c, vit, &vdat[j], vdat[j-1]); ARO_STORE(c, kit, &kdat[j], kdat[j-1]); j--;
        }
        ARO_STORE(c, vit, &vdat[j], vk); ARO_STORE(c, kit, &kdat[j], kk);
    }
    return RESULT_OK(VALUE_REF_GET(vals));
}
/* sort_by!: sort in place by block key (sort_by then copy back into self). */
static RESULT korb_m_ary_sort_by_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    RESULT sr = korb_m_ary_sort_by(c, slots, self, a, block, def_env, cself);   /* sorted copy at slots[0] */
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[0] = sr.value;                                  /* root the sorted array */
    VALUE_REF sorted = VALUE_REF_AT(&slots[0]);
    VAL2ARY(VALUE_REF_GET(self))->len = 0;
    uint32_t n = VAL2ARY(VALUE_REF_GET(sorted))->len;
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(sorted))->items->data[i];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* min_by(want=-1) / max_by(want=1): element with the extreme block key. */
static RESULT korb_ary_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *cself, int want) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Array#min_by/max_by without a block is not supported");
    slots[0] = KORB_NIL;   /* best value */
    slots[1] = KORB_NIL;   /* best key */
    bool have = false;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[2] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;
        if (!have) { slots[0] = slots[2]; slots[1] = slots[3]; have = true; continue; }
        int cmp = korb_cmp_full(c, slots[3], slots[1]);
        if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if ((want < 0 && cmp < 0) || (want > 0 && cmp > 0)) { slots[0] = slots[2]; slots[1] = slots[3]; }
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_ary_min_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_ary_minmax_by(c, slots, self, block, def_env, cself, -1); }
static RESULT korb_m_ary_max_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_ary_minmax_by(c, slots, self, block, def_env, cself,  1); }
static RESULT korb_m_ary_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
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
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* flat_map: map then flatten one level (Array results spliced). */
static RESULT korb_m_ary_flat_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; ARY_REQUIRE_BLOCK("Array#flat_map");           /* no-block enum would map, not flat_map → raise */
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;                              /* root result */
        if (KORB_ARRAY_P(slots[0])) {
            uint32_t m = VAL2ARY(slots[0])->len;
            for (uint32_t j = 0; j < m; j++) {
                VALUE e = VAL2ARY(slots[0])->items->data[j];
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
        slots[2] = ary->items->data[i];
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
        slots[1] = ary->items->data[i];                  /* element */
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[2] = r.value;                              /* key */
        int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(h)), slots[2]);
        if (idx < 0) {                                   /* new bucket array */
            slots[3] = UNWRAP(korb_ary_new(c, slots + 4, 4));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_hash_set(c, slots + 4, h, VALUE_REF_AT(&slots[2]), slots[3]));
        } else {
            VALUE bucket = VAL2HASH(VALUE_REF_GET(h))->items->data[2 * idx + 1];
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
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];                  /* root elem across yield */
        if (korb_case_eq(c, VALUE_SLICE_GET(a, 0), slots[0]) == keep) {
            if (block != NULL) {
                RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
            } else {
                CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
            }
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)   { return korb_ary_grep(c, slots, self, a, block, def_env, cself, true); }
static RESULT korb_m_ary_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_ary_grep(c, slots, self, a, block, def_env, cself, false); }

static RESULT korb_m_ary_sort_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                   NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) {
        KorbArray *d = VAL2ARY(VALUE_REF_GET(self));    /* in-place; cmp does not alloc */
        KorbArrayItems *const dit = d->items;
        const VALUE *data = dit->data;
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
        slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[i];   /* key (rooted) */
        uint32_t j = i;
        while (j > 0) {
            VALUE left = VAL2ARY(VALUE_REF_GET(self))->items->data[j-1];
            int cmp;
            CHECK(korb_cmp_block(c, slots + 1, left, slots[0], block, def_env, cself, &cmp));
            if (cmp <= 0) break;
            KorbArray *dd = VAL2ARY(VALUE_REF_GET(self));
            ARO_STORE(c, dd->items, &dd->items->data[j], dd->items->data[j-1]); j--;
        }
        KorbArrayItems *sit = VAL2ARY(VALUE_REF_GET(self))->items;
        ARO_STORE(c, sit, &sit->data[j], slots[0]);
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
        slots[1] = ary->items->data[i];                 /* elem (root) */
        int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(h)), slots[1]);
        intptr_t cnt = idx < 0 ? 0 : FIX2LONG(VAL2HASH(VALUE_REF_GET(h))->items->data[2*idx+1]);
        CHECK(korb_hash_set(c, slots + 2, h, VALUE_REF_AT(&slots[1]), LONG2FIX(cnt + 1)));
    }
    return RESULT_OK(VALUE_REF_GET(h));
}

/* recursively join leaves of nested arrays with `sep` (no koruby alloc; all reads). */
static void korb_join_rec(CTX *c, FILE *ms, const KorbArray *ary, const KorbString *sep, bool *first) {
    for (uint32_t i = 0; i < ary->len; i++) {
        VALUE e = ary->items->data[i];
        if (KORB_ARRAY_P(e)) { korb_join_rec(c, ms, VAL2ARY(e), sep, first); continue; }
        if (!*first && sep) fwrite(sep->buf->data, 1, sep->len, ms);
        korb_fprint_to_s(c, ms, e);
        *first = false;
    }
}
static RESULT korb_m_ary_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (SELF_ARY->len == 0) return korb_str_new(c, slots, "", 0);   /* [].join(anything) → "" (sep not validated) */
    /* sep at slots scratch so it survives the per-element to_s allocs */
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        VALUE sv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    }
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    const KorbArray *ary = SELF_ARY;
    const KorbString *sep = (VALUE_SLICE_LEN(a) >= 1 && KORB_STRING_P(VALUE_SLICE_GET(a, 0))) ? VAL2STR(VALUE_SLICE_GET(a, 0)) : NULL;
    bool first = true;
    korb_join_rec(c, ms, ary, sep, &first);             /* recurse into nested arrays (no GC) */
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
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
            CHECK(korb_ary_push_val(c, scratch + 1, copy, VAL2ARY(VALUE_REF_GET(cur))->items->data[k]));
        return korb_ary_push_val(c, scratch + 1, out, VALUE_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < len; i++) {
        if (used[i]) continue;
        used[i] = true;
        CHECK(korb_ary_push_val(c, scratch, cur, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
        CHECK(korb_perm_rec(c, scratch, self, out, cur, want, used, depth + 1));
        KorbArray *cv = VAL2ARY(VALUE_REF_GET(cur)); cv->len--;     /* pop */
        used[i] = false;
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_permutation(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t len = SELF_ARY->len;
    intptr_t want = len;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &want))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
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
        VALUE e = VAL2ARY(VALUE_REF_GET(out))->items->data[i];
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
            CHECK(korb_ary_push_val(c, scratch + 1, copy, VAL2ARY(VALUE_REF_GET(cur))->items->data[k]));
        return korb_ary_push_val(c, scratch + 1, out, VALUE_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = start; i < len; i++) {
        CHECK(korb_ary_push_val(c, scratch, cur, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
        CHECK(korb_comb_rec(c, scratch, self, out, cur, want, i + 1, depth + 1));
        KorbArray *cv = VAL2ARY(VALUE_REF_GET(cur)); cv->len--;     /* pop */
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_combination(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t len = SELF_ARY->len;
    intptr_t want = 0;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &want))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
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
        VALUE e = VAL2ARY(VALUE_REF_GET(out))->items->data[i];
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
            CHECK(korb_ary_push_val(c, scratch + 1, copy, VAL2ARY(VALUE_REF_GET(cur))->items->data[k]));
        return korb_ary_push_val(c, scratch + 1, out, VALUE_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = start; i < len; i++) {
        CHECK(korb_ary_push_val(c, scratch, cur, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
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
            CHECK(korb_ary_push_val(c, scratch + 1, copy, VAL2ARY(VALUE_REF_GET(cur))->items->data[k]));
        return korb_ary_push_val(c, scratch + 1, out, VALUE_REF_GET(copy));
    }
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < len; i++) {
        CHECK(korb_ary_push_val(c, scratch, cur, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
        CHECK(korb_rperm_rec(c, scratch, self, out, cur, want, depth + 1));
        VAL2ARY(VALUE_REF_GET(cur))->len--;
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_repeated(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool perm) {
    intptr_t want = 0;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &want))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
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
        VALUE e = VAL2ARY(VALUE_REF_GET(out))->items->data[i];
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
        slots[1] = ary->items->data[i];                        /* element / pair */
        if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[1] = r.value;
        }
        if (UNLIKELY(!KORB_ARRAY_P(slots[1])))
            return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "wrong element type %s at %u (expected array)", korb_type_name(slots[1]), i);
        if (UNLIKELY(VAL2ARY(slots[1])->len != 2))
            return korb_raise(c, slots + 2, KORB_E_ARGUMENT, 0, "wrong array length at %u (expected 2, was %u)", i, VAL2ARY(slots[1])->len);
        slots[2] = VAL2ARY(slots[1])->items->data[0];          /* key */
        VALUE val = VAL2ARY(slots[1])->items->data[1];
        CHECK(korb_hash_set(c, slots + 3, dst, VALUE_REF_AT(&slots[2]), val));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Array#cycle([n]) — yield elements n times (forever if n omitted); → nil. */
static RESULT korb_m_ary_cycle(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    const bool bounded = VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL;
    intptr_t n = 0;
    if (bounded && UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (block == NULL) {
        if (bounded) {                                  /* finite → eager Enumerator of repeated elements */
            const uint32_t blen = VAL2ARY(VALUE_REF_GET(self))->len;
            slots[0] = UNWRAP(korb_ary_new(c, slots, (n > 0 ? (uint32_t)n : 0) * blen));
            VALUE_REF out = VALUE_REF_AT(&slots[0]);
            for (intptr_t pass = 0; pass < n; pass++)
                for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(self))->len; i++)
                    CHECK(korb_ary_push_val(c, slots + 1, out, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
            slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "cycle"));
            return korb_enum_new(c, slots + 2, VALUE_REF_GET(out), slots[1]);
        }
        return korb_lazy_new(c, slots, VALUE_REF_GET(self), 2);   /* unbounded → infinite lazy enum */
    }
    if (bounded && n <= 0) return RESULT_OK(KORB_NIL);
    if (SELF_ARY->len == 0) return RESULT_OK(KORB_NIL);
    for (intptr_t pass = 0; !bounded || pass < n; pass++) {
        for (uint32_t i = 0; ; i++) {
            const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
            if (i >= ary->len) break;
            VALUE e = ary->items->data[i];
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
        VALUE e = SELF_ARY->items->data[i];
        if (e != KORB_NIL) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_compact_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    uint32_t w = 0; bool changed = false;
    for (uint32_t r = 0; r < ary->len; r++) {
        if (it->data[r] == KORB_NIL) { changed = true; continue; }
        if (w != r) ARO_STORE(c, it, &it->data[w], it->data[r]);
        w++;
    }
    for (uint32_t r = w; r < ary->len; r++) ARO_STORE(c, it, &it->data[r], KORB_NIL);
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

static RESULT korb_m_ary_uniq_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    uint32_t w = 0;
    for (uint32_t i = 0; i < ary->len; i++) {
        bool seen = false;
        for (uint32_t j = 0; j < w; j++) if (korb_value_eq(it->data[j], it->data[i])) { seen = true; break; }
        if (!seen) { if (w != i) ARO_STORE(c, it, &it->data[w], it->data[i]); w++; }
    }
    if (w == ary->len) return RESULT_OK(KORB_NIL);   /* unchanged */
    ary->len = w;
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_uniq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = SELF_ARY->items->data[i];
        const KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
        bool seen = false;
        for (uint32_t j = 0; j < d->len; j++) if (korb_value_eql(d->items->data[j], e)) { seen = true; break; }
        if (!seen) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
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
        slots[2] = VAL2ARY(VALUE_REF_GET(self))->items->data[i];   /* element */
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;                                        /* key */
        bool seen = false;
        const KorbArray *ks = VAL2ARY(VALUE_REF_GET(keys));
        for (uint32_t j = 0; j < ks->len; j++) if (korb_value_eql(ks->items->data[j], slots[3])) { seen = true; break; }
        if (!seen) {
            CHECK(korb_ary_push_val(c, slots + 4, keys, slots[3]));
            CHECK(korb_ary_push_val(c, slots + 4, dst, slots[2]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* recursive flatten helper: append all leaves of `src` into dst */
/* depth-limited flatten: depth<0 = full, 0 = copy as-is, >0 = that many levels. */
static RESULT korb_ary_flatten_depth(CTX *c, VALUE *slots, VALUE_REF dst, VALUE_REF src, int depth) {
    uint32_t n = VAL2ARY(VALUE_REF_GET(src))->len;
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(src))->items->data[i];
        if (KORB_ARRAY_P(e) && depth != 0) {
            slots[0] = e;
            CHECK(korb_ary_flatten_depth(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), depth < 0 ? depth : depth - 1));
        } else {
            CHECK(korb_ary_push_val(c, slots, dst, e));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_flatten(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int depth = -1;
    intptr_t d;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(VALUE_SLICE_GET(a, 0), &d)) depth = (int)d;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    CHECK(korb_ary_flatten_depth(c, slots + 1, dst, self, depth));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_flatten_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t depth = -1;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) (void)korb_to_index(VALUE_SLICE_GET(a, 0), &depth);
    bool nested = false;
    const KorbArray *a0 = VAL2ARY(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < a0->len; i++) if (KORB_ARRAY_P(a0->items->data[i])) { nested = true; break; }
    bool changed = nested && depth != 0;                 /* depth 0 → no flattening → nil */
    RESULT fr = korb_m_ary_flatten(c, slots, self, a);   /* flattened copy */
    if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
    slots[0] = fr.value;
    VALUE_REF flat = VALUE_REF_AT(&slots[0]);
    VAL2ARY(VALUE_REF_GET(self))->len = 0;
    uint32_t fn = VAL2ARY(VALUE_REF_GET(flat))->len;
    for (uint32_t i = 0; i < fn; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(flat))->items->data[i];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}

static RESULT korb_m_ary_concat(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {   /* concat(*arrays) */
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
        uint32_t n = VAL2ARY(ov)->len;
        for (uint32_t i = 0; i < n; i++)
            CHECK(korb_ary_push_val(c, slots, self, VAL2ARY(VALUE_SLICE_GET(a, k))->items->data[i]));   /* re-read other (rooted) */
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* select (keep==true) / reject (keep==false) */
static RESULT korb_ary_filter(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *captured_self, bool keep) {
    /* No block → would need an Enumerator that remembers the filter op (so a later
     * .with_index block filters, not maps); returning a plain elements-Enumerator
     * gives a silent wrong answer for select.with_index{...}, so raise instead. */
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Array#%s without a block (Enumerator) is not supported", keep ? "select" : "reject");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        VALUE e = ary->items->data[i];
        slots[0] = e;                                       /* root e across the yield */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_ary_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_ary_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_ary_filter(c, slots, self, block, def_env, captured_self, false); }

static RESULT korb_m_ary_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#find");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[0]);
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_ary_rfind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#rfind");
    for (int64_t i = (int64_t)VAL2ARY(VALUE_REF_GET(self))->len - 1; i >= 0; i--) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if ((uint64_t)i >= ary->len) continue;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[0]);
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* find_index(obj): first index == obj */
        VALUE needle = VALUE_SLICE_GET(a, 0);
        const KorbArray *ary = SELF_ARY;
        for (uint32_t i = 0; i < ary->len; i++)
            if (korb_value_eq(ary->items->data[i], needle)) return RESULT_OK(LONG2FIX(i));
        return RESULT_OK(KORB_NIL);
    }
    ARY_REQUIRE_BLOCK("Array#find_index");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}

static bool korb_ary_has(const KorbArray *ar, VALUE v);
static RESULT korb_m_ary_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; ARY_REQUIRE_BLOCK("Array#take_while");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
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
        slots[0] = ary->items->data[i];
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
    (void)c;(void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < ary->len; i++) ARO_STORE(c, ary->items, &ary->items->data[i], KORB_NIL);
    ary->len = 0;
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_intersect_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
    const KorbArray *me = VAL2ARY(VALUE_REF_GET(self)), *other = VAL2ARY(ov);
    for (uint32_t i = 0; i < me->len; i++)
        if (korb_ary_has(other, me->items->data[i])) return RESULT_OK(KORB_TRUE);
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
        slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[mid];
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
        slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[mid];
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
    (void)a; ARY_REQUIRE_BLOCK("Array#map!");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        KorbArray *a2 = VAL2ARY(VALUE_REF_GET(self));
        ARO_STORE(c, a2->items, &a2->items->data[i], r.value);
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
            if (ti >= 0) kwlim  = VAL2HASH(h)->items->data[2 * ti + 1];
            if (bi >= 0) kwstep = VAL2HASH(h)->items->data[2 * bi + 1];
            na--;
        }
    }
    const VALUE limv0 = na >= 1 ? VALUE_SLICE_GET(a, 0) : kwlim;                       /* limit (nil ⇒ endless) */
    const VALUE stepv0 = na >= 2 ? VALUE_SLICE_GET(a, 1) : (kwstep != KORB_NIL ? kwstep : LONG2FIX(1));
    if (block == NULL) {                                  /* no block → lazy ArithmeticSequence */
        return korb_arithseq_new(c, slots, VALUE_REF_GET(self), limv0, stepv0, (uint8_t)((kw || na >= 2) ? 2 : na), 0);
    }
    VALUE selfv = VALUE_REF_GET(self);
    VALUE limv = limv0;
    VALUE stepv = stepv0;
    bool use_float = KORB_FLOAT_P(selfv) || KORB_FLOAT_P(limv) || KORB_FLOAT_P(stepv);
    const bool collect = (block == NULL);             /* no block → materialize into an Enumerator */
    VALUE_REF dst = {0};
    if (use_float) {
        double s, lim, st;
        if (!korb_num_to_d(selfv, &s) || !korb_num_to_d(limv, &lim) || !korb_num_to_d(stepv, &st))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "step requires numeric arguments");
        if (st == 0.0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        if (collect) { slots[0] = UNWRAP(korb_ary_new(c, slots, 8)); dst = VALUE_REF_AT(&slots[0]); }  /* after reading the doubles */
        for (long i = 0; ; i++) {
            double d = s + (double)i * st;
            if (st > 0 ? d > lim : d < lim) break;
            if (collect) { slots[1] = UNWRAP(korb_float_new(c, slots + 1, d)); CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1])); continue; }
            slots[0] = UNWRAP(korb_float_new(c, slots, d));
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    } else {
        intptr_t s = FIX2LONG(selfv), lim = FIX2LONG(limv), st = FIX2LONG(stepv);
        if (st == 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        if (collect) { slots[0] = UNWRAP(korb_ary_new(c, slots, 8)); dst = VALUE_REF_AT(&slots[0]); }
        for (intptr_t i = s; st > 0 ? i <= lim : i >= lim; i += st) {
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
    bool has_pat = VALUE_SLICE_LEN(a) >= 1;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        bool t;
        if (has_pat) {
            t = korb_case_eq(c, VALUE_SLICE_GET(a, 0), slots[0]);    /* pattern === element */
        } else if (block != NULL) {                                 /* truthiness of block result */
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else {
            t = KORB_TRUTHY(slots[0]);                        /* no block → element truthiness */
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
        intptr_t a = FIX2LONG(l), b = FIX2LONG(r), res;
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
    if (KORB_STRING_P(v))  { *op_mid = korb_intern(c->vm, VAL2STR(v)->buf->data, VAL2STR(v)->len); return true; }
    return false;
}
static RESULT korb_m_ary_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    uint32_t op_mid;
    /* A trailing operator Symbol/String selects the symbol form and takes
     * precedence over any block (CRuby ignores the block in that case). */
    const uint32_t na0 = VALUE_SLICE_LEN(a);
    const bool sym_form = na0 >= 1 && korb_reduce_op(c, VALUE_SLICE_GET(a, na0 - 1), &op_mid);
    if (block == NULL && !sym_form)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block or operator symbol given");
    if (sym_form) {                                        /* reduce(:+) / reduce(init, :+) [block ignored] */
        uint32_t na = na0;
        uint32_t i = 0;
        if (na >= 2) slots[0] = VALUE_SLICE_GET(a, 0);     /* explicit init */
        else { const KorbArray *ary = SELF_ARY; if (ary->len == 0) return RESULT_OK(KORB_NIL); slots[0] = ary->items->data[0]; i = 1; }
        for (; ; i++) {
            const KorbArray *ary = SELF_ARY;
            if (i >= ary->len) break;
            slots[1] = slots[0]; slots[2] = ary->items->data[i];   /* acc, elem (recv+arg) */
            RESULT r = korb_send_impl(c, slots + 3, op_mid, 0, 1, NULL, NULL, KORB_NIL);
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
        slots[0] = ary->items->data[0];
        i = 1;
    }
    for (; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        VALUE argv[2] = { slots[0], ary->items->data[i] };  /* acc, elem */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;                                /* root new acc */
    }
    return RESULT_OK(slots[0]);
}

static RESULT korb_m_ary_each_with_object(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    ARY_REQUIRE_BLOCK("Array#each_with_object");
    slots[0] = VALUE_SLICE_GET(a, 0);                      /* the memo object (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = SELF_ARY;
        if (i >= ary->len) break;
        VALUE argv[2] = { ary->items->data[i], slots[0] };  /* elem, memo */
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
