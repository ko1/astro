/* koruby_precise — range.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */

/* CRuby's ruby_float_step: how many values a float step yields, and the i-th one
 * (an inclusive range clamps an overshooting last value back to `end`). */
long korb_float_step_n(double beg, double end, double unit, bool excl) {
    double n = (end - beg) / unit;
    double err = (fabs(beg) + fabs(end) + fabs(end - beg)) / fabs(unit) * DBL_EPSILON;
    if (err > 0.5) err = 0.5;
    if (excl) {
        if (n <= 0) return 0;
        n = (n < 1) ? 0 : floor(n - err);
    } else {
        if (n < 0) return 0;
        n = floor(n + err);
    }
    /* one more step may still fit within the rounding error (CRuby) */
    const double d = (n + 1) * unit + beg;
    if (beg < end) { if (excl ? d < end : d <= end) n++; }
    else if (beg > end) { if (excl ? d > end : d >= end) n++; }
    return (long)n + 1;
}
double korb_float_step_at(double beg, double end, double unit, long i, bool excl) {
    const double d = beg + (double)i * unit;
    if (!excl && (unit >= 0 ? end < d : d < end)) return end;
    return d;
}
/* ---- Range methods ------------------------------------------------------- */


/* integer iteration bounds [lo, hi) ; false if endpoints aren't both Integer */
static bool korb_range_int_bounds(const KorbRange *r, korb_sword_t *lo, korb_sword_t *hi) {
    if (!FIXNUM_P(r->rbegin)) return false;
    if (FIXNUM_P(r->rend)) {
        const korb_sword_t e = FIX2LONG(r->rend);
        *lo = FIX2LONG(r->rbegin);
        *hi = r->exclude_end ? e : e + 1;
        return true;
    }
    /* An Integer begin with a Float end still iterates over integers
     * ((2..3.7).to_a == [2, 3]); only the stop point is derived from the float. */
    if (KORB_FLOAT_P(r->rend)) {
        const double e = korb_float_val(r->rend);
        if (isnan(e) || isinf(e) || e >= 9.2e18 || e <= -9.2e18) return false;
        const double fl = floor(e);
        *lo = FIX2LONG(r->rbegin);
        *hi = (r->exclude_end && fl == e) ? (korb_sword_t)e : (korb_sword_t)fl + 1;
        return true;
    }
    return false;
}

static RESULT korb_m_range_begin(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->rbegin); }
static RESULT korb_m_range_end(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->rend); }
static RESULT korb_m_range_exclude(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a){ (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->exclude_end ? KORB_TRUE : KORB_FALSE); }

static RESULT korb_m_range_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        const KorbRange *const r = SELF_RANGE;
        if (KORB_FLOAT_P(r->rbegin))                     /* Float begin → not iterable (even endless) */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(r->rbegin));
        if ((r->rend == KORB_NIL || (KORB_FLOAT_P(r->rend) && isinf(korb_float_val(r->rend)))) && KORB_INTEGER_P(r->rbegin))   /* endless / +∞-end Integer → Infinity */
            return korb_float_new(c, slots, INFINITY);
        if (r->rbegin == KORB_NIL && KORB_INTEGER_P(r->rend))   /* beginless Integer → Infinity too */
            return korb_float_new(c, slots, INFINITY);
        if (KORB_INTEGER_P(r->rbegin))
            return RESULT_OK(KORB_NIL);                  /* Bignum-bounded integer range: exact count not computed (gap) */
        /* other begin: iterable-by-succ (String/Symbol) → nil; else (nil/Array/…) can't iterate → TypeError */
        slots[0] = r->rbegin;                            /* root across the respond_to check */
        if (korb_responds_to(c, slots[0], korb_intern(c->vm, "succ", 4)))
            return RESULT_OK(KORB_NIL);
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(slots[0]));
    }
    return RESULT_OK(LONG2FIX(hi > lo ? hi - lo : 0));
}
static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* defined below */
static RESULT korb_m_ary_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_last(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_range_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    korb_sword_t lo, hi;
    if (block == NULL && VALUE_SLICE_LEN(a) == 0 &&
        (SELF_RANGE->rend == KORB_NIL || SELF_RANGE->rbegin == KORB_NIL))   /* (n..) / (..n): infinite → Float::INFINITY */
        return korb_flo(c, slots, (double)INFINITY);
    if (block != NULL || !korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* block or non-int → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_count(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* count(obj): 1 if obj in the integer range else 0 */
        VALUE o = VALUE_SLICE_GET(a, 0);
        if (!FIXNUM_P(o)) return RESULT_OK(LONG2FIX(0));
        korb_sword_t v = FIX2LONG(o);
        return RESULT_OK(LONG2FIX(v >= lo && v < hi ? 1 : 0));
    }
    return RESULT_OK(LONG2FIX(hi > lo ? hi - lo : 0));
}

/* Ordered compare a <=> b for Range#cover?: the GC-free fast path (numeric/
 * string), falling back to dispatching #<=> for anything else (custom
 * Comparable, Bignum, ...).  *out = -1/0/1, or 2 when incomparable. */
static RESULT korb_range_cmp(CTX *c, VALUE *slots, VALUE a, VALUE b, int *out) {
    const int fast = korb_cmp_values(a, b);
    if (fast != 2) { *out = fast; return RESULT_OK(KORB_NIL); }
    return korb_comparable_cmp(c, slots, a, b, out);   /* dispatch a.<=>(b) */
}
/* Does the range [beg,end] (excl for right-open) cover the single value x?
 * nil begin/end = unbounded on that side.  slots[0..] are scratch; beg/end/x
 * must be caller-rooted (this only dispatches #<=>, which may GC).  *out set. */
static RESULT korb_range_cover_value(CTX *c, VALUE *slots, VALUE beg, VALUE end, bool excl, VALUE x, bool *out) {
    int lc = -1, uc = -1;
    if (beg != KORB_NIL) { RESULT cl = korb_range_cmp(c, slots, beg, x, &lc); if (UNLIKELY(cl.state != KORB_NORMAL)) return cl; }
    if (end != KORB_NIL) { RESULT cu = korb_range_cmp(c, slots, x, end, &uc); if (UNLIKELY(cu.state != KORB_NORMAL)) return cu; }
    if (lc == 2 || uc == 2) { *out = false; return RESULT_OK(KORB_NIL); }
    const bool lower = (lc <= 0);
    const bool upper = (end == KORB_NIL) ? true : (excl ? (uc < 0) : (uc <= 0));
    *out = (lower && upper);
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_range_cover(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbRange *r = SELF_RANGE;
    const bool excl = r->exclude_end;
    slots[0] = r->rbegin; slots[1] = r->rend; slots[2] = VALUE_SLICE_GET(a, 0);   /* [0]beg [1]end [2]arg — rooted */
    if (KORB_RANGE_P(slots[2])) {                /* cover?(other_range): self contains the whole range (CRuby r_cover_range_p) */
        const bool o_excl = VAL2RANGE(slots[2])->exclude_end;
        slots[3] = VAL2RANGE(slots[2])->rbegin; slots[4] = VAL2RANGE(slots[2])->rend;   /* [3]val_beg [4]val_end */
        const bool beg_nil = slots[0] == KORB_NIL, end_nil = slots[1] == KORB_NIL;
        const bool vb_nil  = slots[3] == KORB_NIL, ve_nil  = slots[4] == KORB_NIL;
        if (!end_nil && ve_nil) return RESULT_OK(KORB_FALSE);   /* self bounded above, other endless */
        if (!beg_nil && vb_nil) return RESULT_OK(KORB_FALSE);   /* self bounded below, other beginless */
        if (!vb_nil && !ve_nil) {                                /* other empty / backward → false */
            int cbe; RESULT cc = korb_range_cmp(c, slots + 5, slots[3], slots[4], &cbe);
            if (UNLIKELY(cc.state != KORB_NORMAL)) return cc;
            if (cbe == 2 || cbe > (o_excl ? -1 : 0)) return RESULT_OK(KORB_FALSE);
        }
        if (!vb_nil) {                                           /* other.begin must fall inside self */
            bool cov; RESULT cc = korb_range_cover_value(c, slots + 5, slots[0], slots[1], excl, slots[3], &cov);
            if (UNLIKELY(cc.state != KORB_NORMAL)) return cc;
            if (!cov) return RESULT_OK(KORB_FALSE);
        }
        int cmp_end;                                            /* self.end <=> other.end (nil handled per CRuby r_less) */
        if (!ve_nil && !end_nil) {
            RESULT cc = korb_range_cmp(c, slots + 5, slots[1], slots[4], &cmp_end);
            if (UNLIKELY(cc.state != KORB_NORMAL)) return cc;
            if (cmp_end == 2) return RESULT_OK(KORB_FALSE);     /* incomparable ends */
        } else if (end_nil && ve_nil) cmp_end = 0;              /* both endless */
        else cmp_end = 1;                                       /* endless self vs bounded other → +∞ */
        bool res;
        if (excl == o_excl)      res = (cmp_end >= 0);
        else if (excl)           res = (cmp_end > 0);
        else if (cmp_end >= 0)   res = true;
        else {                                                   /* incl self, excl other, self.end < other.end: cover other.max */
            slots[5] = slots[2];                                 /* root the other range */
            RESULT mr = korb_send_impl(c, slots + 6, korb_intern(c->vm, "max", 3), 0, 0, NULL, NULL, NULL);
            if (mr.state == KORB_RAISE && KORB_EXC_P(mr.value) && VAL2EXC(mr.value)->etype == KORB_E_TYPE) res = false;   /* max undeterminable → false */
            else if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
            else if (mr.value == KORB_NIL) res = false;
            else {
                slots[6] = mr.value; int ce;
                RESULT cc = korb_range_cmp(c, slots + 7, slots[1], slots[6], &ce);
                if (UNLIKELY(cc.state != KORB_NORMAL)) return cc;
                res = (ce != 2 && ce >= 0);
            }
        }
        return RESULT_OK(res ? KORB_TRUE : KORB_FALSE);
    }
    bool cov; RESULT cc = korb_range_cover_value(c, slots + 3, slots[0], slots[1], excl, slots[2], &cov);
    if (UNLIKELY(cc.state != KORB_NORMAL)) return cc;
    return RESULT_OK(cov ? KORB_TRUE : KORB_FALSE);
}

/* include?/member?: membership of a single value (a Range/other container is not an element). */
static RESULT korb_m_range_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE x = VALUE_SLICE_GET(a, 0);
    const KorbRange *const r = VAL2RANGE(VALUE_REF_GET(self));
    /* Fully-open (nil..nil): true for a Numeric/Time value, TypeError otherwise —
     * checked before the container-argument short-circuit so (nil..nil).include?([])
     * raises rather than returning false. */
    if (r->rbegin == KORB_NIL && r->rend == KORB_NIL) {
        bool linear = FIXNUM_P(x) || KORB_FLOAT_P(x) || KORB_BIGNUM_P(x) || KORB_RATIONAL_P(x) || KORB_COMPLEX_P(x);
        if (!linear && KORB_OBJECT_P(x)) {
            const VALUE dcls = korb_dispatch_class(c, x);
            const VALUE num = korb_const_get(c->vm, korb_intern(c->vm, "Numeric", 7));
            const VALUE tim = korb_const_get(c->vm, korb_intern(c->vm, "Time", 4));
            linear = (num != KORB_NIL && korb_class_has_ancestor(dcls, num)) ||
                     (tim != KORB_NIL && korb_class_has_ancestor(dcls, tim));
        }
        if (linear) return RESULT_OK(KORB_TRUE);
        return korb_raise(c, slots, KORB_E_TYPE, 0, "cannot determine inclusion in beginless/endless ranges");
    }
    if (KORB_RANGE_P(x) || KORB_ARRAY_P(x) || KORB_HASH_P(x)) return RESULT_OK(KORB_FALSE);
    /* String-element range: CRuby uses #succ-membership (a value is in the range
     * iff it appears in the begin, begin.succ, … , end sequence), with #to_str
     * coercion of the argument, and raises TypeError for an open (nil) bound. */
    if (KORB_STRING_P(r->rbegin) || KORB_STRING_P(r->rend)) {
        if (r->rbegin == KORB_NIL || r->rend == KORB_NIL)
            return korb_raise(c, slots, KORB_E_TYPE, 0, "cannot determine inclusion in beginless/endless ranges");
        slots[0] = VALUE_REF_GET(self);                  /* root the range (holds begin/end) */
        slots[1] = x;                                    /* the (maybe to_str-coerced) argument */
        if (!KORB_STRING_P(slots[1])) {                  /* coerce via #to_str; non-convertible → false */
            const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
            if (KORB_OBJECT_P(slots[1]) && korb_responds_to_coerce(c, slots + 2, slots[1], to_str)) {
                const char *onm = korb_type_name(slots[1]);
                RESULT sr = korb_send_impl(c, slots + 2, to_str, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                if (!KORB_STRING_P(sr.value))
                    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to String (%s#to_str gives %s)", onm, onm, korb_type_name(sr.value));
                slots[1] = sr.value;
            } else return RESULT_OK(KORB_FALSE);
        }
        const bool excl = VAL2RANGE(slots[0])->exclude_end;
        slots[2] = VAL2RANGE(slots[0])->rbegin;          /* current = begin (rooted) */
        const VALUE end = VAL2RANGE(slots[0])->rend;
        slots[3] = end;                                  /* end (rooted) */
        const uint32_t el = korb_utf8_count(korb_strbuf_data(VAL2STR(end)->buf), VAL2STR(end)->len);
        const uint32_t mid_succ = korb_intern(c->vm, "succ", 4);
        for (int guard = 0; guard < 10000000; guard++) {
            const KorbString *cur = VAL2STR(slots[2]), *xs = VAL2STR(slots[1]);
            const bool cur_eq_end = (cur->len == VAL2STR(slots[3])->len && memcmp(korb_strbuf_data(cur->buf), korb_strbuf_data(VAL2STR(slots[3])->buf), cur->len) == 0);
            if (!(excl && cur_eq_end)) {                  /* check membership (skip end itself when exclusive) */
                if (cur->len == xs->len && memcmp(korb_strbuf_data(cur->buf), korb_strbuf_data(xs->buf), cur->len) == 0) return RESULT_OK(KORB_TRUE);
            }
            if (cur_eq_end) return RESULT_OK(KORB_FALSE); /* reached end without a match */
            if (korb_utf8_count(korb_strbuf_data(cur->buf), cur->len) > el) return RESULT_OK(KORB_FALSE);   /* overshot end length */
            slots[4] = slots[2];                          /* stage current as the #succ receiver */
            RESULT sc = korb_send_impl(c, slots + 5, mid_succ, 0, 0, NULL, NULL, NULL);   /* current = current.succ */
            if (UNLIKELY(sc.state != KORB_NORMAL)) return sc;
            if (UNLIKELY(!KORB_STRING_P(sc.value))) return RESULT_OK(KORB_FALSE);
            slots[2] = sc.value;
        }
        return RESULT_OK(KORB_FALSE);
    }
    /* Non-string range: classify by the DEFINED bound (either may be nil for an
     * open range).  Numeric → cover (works for open ranges).  Custom Comparable
     * → succ-membership, but an open bound can't be walked → TypeError. */
    const VALUE defb = (r->rbegin != KORB_NIL) ? r->rbegin : r->rend;
    if (!KORB_OBJECT_P(defb)) {
        return korb_m_range_cover(c, slots, self, a);        /* numeric/other → cover-based (fast) */
    }
    /* CRuby treats Numeric (incl. subclasses) and Time bounds as "linear objects"
     * → #cover? logic (works for open ranges), never #succ iteration. */
    else if (({ const VALUE dcls = korb_dispatch_class(c, defb);
                const VALUE num = korb_const_get(c->vm, korb_intern(c->vm, "Numeric", 7));
                const VALUE tim = korb_const_get(c->vm, korb_intern(c->vm, "Time", 4));
                (num != KORB_NIL && korb_class_has_ancestor(dcls, num)) ||
                (tim != KORB_NIL && korb_class_has_ancestor(dcls, tim)); })) {
        return korb_m_range_cover(c, slots, self, a);
    }
    else if (korb_responds_to(c, defb, korb_intern(c->vm, "to_str", 6))) {
        /* a String-coercible Comparable bound (e.g. mspec's SpecVersion) is treated
         * like a String range by CRuby's #include?, which for such objects reduces
         * to #cover? (a <=> comparison) rather than #succ iteration. */
        return korb_m_range_cover(c, slots, self, a);
    }
    else if (r->rbegin == KORB_NIL || r->rend == KORB_NIL) {
        /* custom-object range with an open bound → can't succ-walk → TypeError. */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "cannot determine inclusion in beginless/endless ranges");
    }
    else {
        /* custom-object range → succ-membership: walk begin, begin.succ, ...
         * checking == x, until current passes end (CRuby: include? ≠ cover here). */
        slots[0] = r->rbegin;                                /* current (rooted) */
        slots[1] = x;                                        /* target */
        slots[2] = r->rend;                                  /* end (may be nil) */
        const bool excl = VAL2RANGE(VALUE_REF_GET(self))->exclude_end;
        const uint32_t mid_eq = korb_intern(c->vm, "==", 2);
        const uint32_t mid_cmp = korb_intern(c->vm, "<=>", 3);
        const uint32_t mid_succ = korb_intern(c->vm, "succ", 4);
        for (int guard = 0; guard < 10000000; guard++) {
            if (slots[2] != KORB_NIL) {                      /* current <=> end → stop once past the end */
                slots[3] = slots[0]; slots[4] = slots[2];
                RESULT cmp = korb_send_impl(c, slots + 5, mid_cmp, 0, 1, NULL, NULL, NULL);
                if (UNLIKELY(cmp.state != KORB_NORMAL)) return cmp;
                if (!FIXNUM_P(cmp.value)) return RESULT_OK(KORB_FALSE);
                const korb_sword_t cv = FIX2LONG(cmp.value);
                if (excl ? (cv >= 0) : (cv > 0)) return RESULT_OK(KORB_FALSE);
            }
            slots[3] = slots[0]; slots[4] = slots[1];        /* current == x */
            RESULT eq = korb_send_impl(c, slots + 5, mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(eq.state != KORB_NORMAL)) return eq;
            if (KORB_TRUTHY(eq.value)) return RESULT_OK(KORB_TRUE);
            slots[3] = slots[0];                             /* current = current.succ */
            RESULT sc = korb_send_impl(c, slots + 4, mid_succ, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sc.state != KORB_NORMAL)) return sc;
            slots[0] = sc.value;
        }
        return RESULT_OK(KORB_FALSE);
    }
}
/* Range#== — another Range with == begin / == end and the same exclude_end. */
/* Range#== / #eql?: equal iff exclude_end matches and both endpoints compare
 * equal — via `mid` (== or eql?) so a custom-object endpoint's own operator is
 * honoured; korb_value_eq handles nil/cross-numeric fast without a dispatch. */
static RESULT korb_range_eq_via(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, uint32_t mid, bool strict) {
    const VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_RANGE_P(o)) return RESULT_OK(KORB_FALSE);
    if (SELF_RANGE->exclude_end != VAL2RANGE(o)->exclude_end) return RESULT_OK(KORB_FALSE);
    slots[0] = SELF_RANGE->rbegin; slots[1] = VAL2RANGE(o)->rbegin;   /* stage endpoints below the send area (GC-rooted) */
    slots[2] = SELF_RANGE->rend;   slots[3] = VAL2RANGE(o)->rend;
    for (int k = 0; k < 2; k++) {                                     /* k=0 begins, k=1 ends */
        const VALUE x = slots[2 * k], y = slots[2 * k + 1];
        bool eq;
        if (KORB_OBJECT_P(x) || KORB_OBJECT_P(y)) {                   /* dispatch the endpoint's own operator */
            slots[4] = x; slots[5] = y;                              /* recv, arg */
            RESULT r = korb_send_impl(c, slots + 6, mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            eq = KORB_TRUTHY(r.value);
        } else {                                                     /* eql? is type-strict (1.eql?(1.0)=false); == is cross-numeric */
            eq = strict ? korb_value_eql(x, y) : korb_value_eq(x, y);
        }
        if (!eq) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_range_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_range_eq_via(c, slots, self, a, c->vm->mid_eq, false);
}
static RESULT korb_m_range_eql(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_range_eq_via(c, slots, self, a, korb_intern(c->vm, "eql?", 4), true);
}
/* build an array of `take` consecutive ints from `from`, step +1 (asc) or -1 (desc). */
static RESULT korb_range_seq(CTX *c, VALUE *slots, korb_sword_t from, uint32_t take, int step) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, take)));
    for (uint32_t i = 0; i < take; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(from + step * (korb_sword_t)i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_min(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbRange *r = SELF_RANGE;
    if (UNLIKELY(r->rbegin == KORB_NIL)) return korb_raise(c, slots, KORB_E_RANGE, 0, "cannot get the minimum of beginless range");
    korb_sword_t lo, hi;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {   /* min(n) → first n ascending */
        korb_sword_t n;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) {   /* coerce via #to_int */
            VALUE nv = VALUE_SLICE_GET(a, 0);
            RESULT cr = korb_coerce_to_int_pub(c, slots, &nv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(nv, &n))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer",
                                  korb_coerce_name(c, VALUE_SLICE_GET(a, 0)));
        }
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        if (!korb_range_int_bounds(r, &lo, &hi)) {
            /* Integer begin but endless / Bignum end: min(n) = first n ascending
             * from begin (capped by a Fixnum end); no materialization. */
            if (FIXNUM_P(r->rbegin)) {
                const korb_sword_t b = FIX2LONG(r->rbegin);
                uint32_t take = (uint32_t)n;
                if (r->rend != KORB_NIL && FIXNUM_P(r->rend)) {
                    const korb_sword_t e = r->exclude_end ? FIX2LONG(r->rend) : FIX2LONG(r->rend) + 1;
                    take = (korb_sword_t)take > e - b ? (uint32_t)(e > b ? e - b : 0) : take;
                }
                return korb_range_seq(c, slots, b, take, 1);
            }
            /* endless range with a non-Integer begin can't be iterated from begin */
            if (r->rend == KORB_NIL && !KORB_INTEGER_P(r->rbegin))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(r->rbegin));
            slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
            return korb_m_ary_min(c, slots + 1, VALUE_REF_AT(&slots[0]), a, NULL, NULL, NULL);
        }
        uint32_t take = (uint32_t)n; if ((korb_sword_t)take > hi - lo) take = (uint32_t)(hi > lo ? hi - lo : 0);
        return korb_range_seq(c, slots, lo, take, 1);
    }
    if (korb_range_int_bounds(r, &lo, &hi)) return RESULT_OK(hi > lo ? LONG2FIX(lo) : KORB_NIL);
    if (r->rend != KORB_NIL) {                            /* non-int: begin, unless begin > end → empty → nil */
        const int cmp = korb_cmp_values(r->rbegin, r->rend);
        if (cmp == 1 || (r->exclude_end && cmp == 0)) return RESULT_OK(KORB_NIL);
    }
    return RESULT_OK(r->rbegin);
}
static RESULT korb_m_ary_max(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_max(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbRange *r = SELF_RANGE;
    if (UNLIKELY(r->rend == KORB_NIL)) return korb_raise(c, slots, KORB_E_RANGE, 0, "cannot get the maximum of endless range");
    if (UNLIKELY(r->rbegin == KORB_NIL)) {                /* beginless: count down from the end */
        if (VALUE_SLICE_LEN(a) == 0 || VALUE_SLICE_GET(a, 0) == KORB_NIL) {   /* max → the end */
            if (!r->exclude_end) return RESULT_OK(r->rend);
            if (FIXNUM_P(r->rend)) return RESULT_OK(LONG2FIX(FIX2LONG(r->rend) - 1));
            return korb_raise(c, slots, KORB_E_TYPE, 0, "cannot exclude non Integer end value");
        }
        if (FIXNUM_P(r->rend)) {                          /* max(n) → [end, end-1, ...] */
            korb_sword_t n;
            if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) {   /* coerce via #to_int */
            VALUE nv = VALUE_SLICE_GET(a, 0);
            RESULT cr = korb_coerce_to_int_pub(c, slots, &nv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(nv, &n))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer",
                                  korb_coerce_name(c, VALUE_SLICE_GET(a, 0)));
        }
            if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
            const korb_sword_t top = FIX2LONG(r->rend) - (r->exclude_end ? 1 : 0);
            return korb_range_seq(c, slots, top, (uint32_t)n, -1);
        }
    }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(r, &lo, &hi)) {            /* non-fixnum bounds */
        /* Bignum-bounded Integer range (e.g. 0...2**64): max is end / end-1 with
         * no materialization (korb_range_int_bounds only recognises Fixnum). */
        if (KORB_INTEGER_P(r->rbegin) && KORB_INTEGER_P(r->rend) &&
            (VALUE_SLICE_LEN(a) == 0 || VALUE_SLICE_GET(a, 0) == KORB_NIL)) {
            const int cmp = korb_int_cmp(r->rbegin, r->rend);
            if (cmp > 0 || (r->exclude_end && cmp == 0)) return RESULT_OK(KORB_NIL);   /* empty */
            if (!r->exclude_end) return RESULT_OK(r->rend);
            return korb_int_arith(c, slots, r->rend, LONG2FIX(1), 1, 0);              /* exclusive: end - 1 */
        }
        /* Non-numeric INCLUSIVE range (Time..Time, 'f'..'l'): the max is simply
         * the end (when begin <= end) — no #succ / to_a materialization needed.
         * Exclusive non-numeric still needs the succ-strided to_a below. */
        if (!r->exclude_end && r->rend != KORB_NIL &&
            (VALUE_SLICE_LEN(a) == 0 || VALUE_SLICE_GET(a, 0) == KORB_NIL)) {
            int cmp; RESULT cr = korb_range_cmp(c, slots, r->rbegin, r->rend, &cmp);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cmp != 2) return RESULT_OK(cmp <= 0 ? r->rend : KORB_NIL);
        }
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));   /* non-integer (String …) → via to_a */
        return korb_m_ary_max(c, slots + 1, VALUE_REF_AT(&slots[0]), a, NULL, NULL, NULL);
    }
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {   /* max(n) → last n descending */
        korb_sword_t n;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) {   /* coerce via #to_int */
            VALUE nv = VALUE_SLICE_GET(a, 0);
            RESULT cr = korb_coerce_to_int_pub(c, slots, &nv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(nv, &n))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer",
                                  korb_coerce_name(c, VALUE_SLICE_GET(a, 0)));
        }
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        if (!korb_range_int_bounds(r, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
        uint32_t take = (uint32_t)n; if ((korb_sword_t)take > hi - lo) take = (uint32_t)(hi > lo ? hi - lo : 0);
        return korb_range_seq(c, slots, hi - 1, take, -1);
    }
    if (korb_range_int_bounds(r, &lo, &hi)) return RESULT_OK(hi > lo ? LONG2FIX(hi - 1) : KORB_NIL);
    if (r->exclude_end) return korb_raise(c, slots, KORB_E_TYPE, 0, "cannot exclude non Integer end value");
    return RESULT_OK(r->rend);
}
static RESULT korb_m_ary_take(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_range_seq(CTX *c, VALUE *slots, korb_sword_t from, uint32_t take, int step);   /* fwd */
static RESULT korb_m_range_take(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const bool inf_end = KORB_FLOAT_P(SELF_RANGE->rend) && isinf(korb_float_val(SELF_RANGE->rend)) && korb_float_val(SELF_RANGE->rend) > 0;
    if (SELF_RANGE->rend == KORB_NIL || inf_end) {      /* endless / +Infinity end: take n consecutive from begin */
        if (UNLIKELY(!FIXNUM_P(SELF_RANGE->rbegin))) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(SELF_RANGE->rbegin));
        korb_sword_t n;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to take negative size");
        return korb_range_seq(c, slots, FIX2LONG(SELF_RANGE->rbegin), (uint32_t)n, +1);
    }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_take(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    korb_sword_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to take negative size");
    korb_sword_t end = lo + n; if (end > hi) end = hi;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(end > lo ? end - lo : 0))));
    for (korb_sword_t i = lo; i < end; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(SELF_RANGE->rbegin == KORB_NIL))
        return korb_raise(c, slots, KORB_E_RANGE, 0, "cannot get the first element of beginless range");
    if (VALUE_SLICE_LEN(a) >= 1) return korb_m_range_take(c, slots, self, a);
    return RESULT_OK(SELF_RANGE->rbegin);
}
static RESULT korb_m_range_last(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  {
    if (UNLIKELY(SELF_RANGE->rend == KORB_NIL))
        return korb_raise(c, slots, KORB_E_RANGE, 0, "cannot get the last element of endless range");
    if (VALUE_SLICE_LEN(a) == 0) return RESULT_OK(SELF_RANGE->rend);
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_last(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    korb_sword_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
    korb_sword_t start = hi - n; if (start < lo) start = lo;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > start ? hi - start : 0))));
    for (korb_sword_t i = start; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_sum_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    korb_sword_t lo, hi;
    /* non-fixnum init (e.g. Float, Rational) must accumulate via "+" so the type
     * is preserved: (1..5).sum(0.5) => 15.5, not 15. */
    const bool nonint_init = VALUE_SLICE_LEN(a) >= 1 && !FIXNUM_P(VALUE_SLICE_GET(a, 0));
    if (block != NULL || nonint_init || !korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* block / non-int init / non-int range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_sum_b(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    korb_sword_t init = (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    korb_sword_t acc = init;
    for (korb_sword_t i = lo; i < hi; i++) acc += i;   /* small ranges; Bignum unneeded here */
    return RESULT_OK(LONG2FIX(acc));
}
static RESULT korb_m_range_frozen(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;                          /* frozen literals/Range.new carry the flag; a dup'd Range does not */
    return RESULT_OK((((const AroObjectHeader *)(uintptr_t)VALUE_REF_GET(self))->flags & KORB_FL_FROZEN) ? KORB_TRUE : KORB_FALSE);
}

static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc);
static RESULT korb_enum_desc(CTX *c, VALUE *slots, VALUE recv, const char *meth);
static RESULT korb_m_range_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                              /* → Enumerator over the range's elements */
        if (SELF_RANGE->rend == KORB_NIL && !FIXNUM_P(SELF_RANGE->rbegin)) {
            /* endless non-Integer range: a materializing enumerator can't work —
             * hand back a #succ-driven generator (prelude __each_endless_enum). */
            slots[0] = VALUE_REF_GET(self);
            return korb_send_impl(c, slots + 1, korb_intern(c->vm, "__each_endless_enum", 19), 0, 0, NULL, NULL, NULL);
        }
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each"));
        return korb_enum_new(c, slots + 2, slots[0], slots[1]);
    }
    if (SELF_RANGE->rend == KORB_NIL && !FIXNUM_P(SELF_RANGE->rbegin)) {
        /* endless String/etc. range with a block: drive by #succ, re-reading the
         * current value from a rooted slot each round (succ allocates). */
        slots[0] = SELF_RANGE->rbegin;
        for (;;) {
            slots[1] = slots[0];
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[1] = slots[0];                       /* receiver for #succ */
            RESULT sr = korb_send_impl(c, slots + 2, korb_intern(c->vm, "succ", 4), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            slots[0] = sr.value;
        }
    }
    /* endless (1..) or +∞-end (1..Float::INFINITY) integer range: iterate from
     * begin upward indefinitely — the block is expected to break. */
    if (FIXNUM_P(SELF_RANGE->rbegin) &&
        (SELF_RANGE->rend == KORB_NIL ||
         (KORB_FLOAT_P(SELF_RANGE->rend) && isinf(korb_float_val(SELF_RANGE->rend)) && korb_float_val(SELF_RANGE->rend) > 0))) {
        for (korb_sword_t i = FIX2LONG(SELF_RANGE->rbegin); ; i++) {   /* i is a plain C int → GC-safe */
            VALUE iv = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;        /* break/return/raise exits here */
        }
    }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* non-integer (e.g. String) range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
        RESULT r = korb_m_ary_each(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
        return (r.state == KORB_NORMAL) ? RESULT_OK(VALUE_REF_GET(self)) : r;
    }
    for (korb_sword_t i = lo; i < hi; i++) {           /* bounds are plain ints — GC-safe */
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd (defined below) */

static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (SELF_RANGE->rend == KORB_NIL && SELF_RANGE->rbegin != KORB_NIL)   /* endless range → can't materialize */
        return korb_raise(c, slots, KORB_E_RANGE, 0, "cannot convert endless range to an array");
    korb_sword_t lo, hi;
    if (korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))));
        for (korb_sword_t i = lo; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    if (KORB_STRING_P(SELF_RANGE->rbegin) && KORB_STRING_P(SELF_RANGE->rend)) {   /* String range via succ */
        if (VAL2STR(SELF_RANGE->rbegin)->len == 1 && VAL2STR(SELF_RANGE->rend)->len == 1) {   /* single byte → codepoint iterate (CRuby: "A".."z" spans 58, incl. punctuation) */
            const uint8_t b0 = (uint8_t)korb_strbuf_data(VAL2STR(SELF_RANGE->rbegin)->buf)[0];
            const uint8_t e0 = (uint8_t)korb_strbuf_data(VAL2STR(SELF_RANGE->rend)->buf)[0];
            const uint32_t end_ch = SELF_RANGE->exclude_end ? e0 : (uint32_t)e0 + 1;
            slots[0] = UNWRAP(korb_ary_new(c, slots + 1, b0 < end_ch ? end_ch - b0 : 0));
            VALUE_REF out = VALUE_REF_AT(&slots[0]);
            for (uint32_t ch = b0; ch < end_ch; ch++) {
                const char cc = (char)ch;
                slots[1] = UNWRAP(korb_str_new(c, slots + 1, &cc, 1));
                CHECK(korb_ary_push_val(c, slots + 2, out, slots[1]));
            }
            return RESULT_OK(VALUE_REF_GET(out));
        }
        slots[1] = SELF_RANGE->rbegin;                     /* cur  (rooted before any alloc) */
        slots[2] = SELF_RANGE->rend;                       /* end  (rooted) */
        const bool excl = SELF_RANGE->exclude_end != 0;
        slots[0] = UNWRAP(korb_ary_new(c, slots + 3, 8));
        VALUE_REF out = VALUE_REF_AT(&slots[0]);
        for (int guard = 0; guard < 100000000; guard++) {
            uint32_t curlen = VAL2STR(slots[1])->len, endlen = VAL2STR(slots[2])->len;
            if (curlen > endlen) break;                    /* succ grew past end length */
            int cmp = korb_cmp_values(slots[1], slots[2]);
            if (cmp > 0) break;
            if (excl && cmp == 0) break;
            CHECK(korb_ary_push_val(c, slots + 3, out, slots[1]));
            if (cmp == 0) break;                           /* inclusive end reached */
            slots[1] = UNWRAP(korb_m_str_succ(c, slots + 3, VALUE_REF_AT(&slots[1]), VALUE_SLICE_MAKE(NULL, 0)));
        }
        return RESULT_OK(VALUE_REF_GET(out));
    }
    if (SYMBOL_P(SELF_RANGE->rbegin) && SYMBOL_P(SELF_RANGE->rend)) {   /* Symbol range → succ over the names, collect Symbols */
        const char *const bn = korb_sym_name(c->vm, SYM2ID(SELF_RANGE->rbegin));
        const char *const en = korb_sym_name(c->vm, SYM2ID(SELF_RANGE->rend));
        const bool excl = SELF_RANGE->exclude_end != 0;
        if (strlen(bn) == 1 && strlen(en) == 1) {          /* single byte → codepoint iterate (:A..:z = 58) */
            const uint8_t b0 = (uint8_t)bn[0], e0 = (uint8_t)en[0];
            const uint32_t end_ch = excl ? e0 : (uint32_t)e0 + 1;
            slots[0] = UNWRAP(korb_ary_new(c, slots + 1, b0 < end_ch ? end_ch - b0 : 0));
            VALUE_REF out = VALUE_REF_AT(&slots[0]);
            for (uint32_t ch = b0; ch < end_ch; ch++) {
                const char cc = (char)ch;
                CHECK(korb_ary_push_val(c, slots + 1, out, ID2SYM(korb_intern(c->vm, &cc, 1))));
            }
            return RESULT_OK(VALUE_REF_GET(out));
        }
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, bn, (uint32_t)strlen(bn)));   /* cur (String) */
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, en, (uint32_t)strlen(en)));   /* end (String) */
        slots[0] = UNWRAP(korb_ary_new(c, slots + 3, 8));
        VALUE_REF out = VALUE_REF_AT(&slots[0]);
        for (int guard = 0; guard < 100000000; guard++) {
            const uint32_t curlen = VAL2STR(slots[1])->len, endlen = VAL2STR(slots[2])->len;
            if (curlen > endlen) break;
            const int cmp = korb_cmp_values(slots[1], slots[2]);
            if (cmp > 0) break;
            if (excl && cmp == 0) break;
            const KorbString *const cs = VAL2STR(slots[1]);
            CHECK(korb_ary_push_val(c, slots + 3, out, ID2SYM(korb_intern(c->vm, korb_strbuf_data(cs->buf), cs->len))));
            if (cmp == 0) break;
            slots[1] = UNWRAP(korb_m_str_succ(c, slots + 3, VALUE_REF_AT(&slots[1]), VALUE_SLICE_MAKE(NULL, 0)));
        }
        return RESULT_OK(VALUE_REF_GET(out));
    }
    /* a custom object with #succ (and #<=> for the bound test): iterate begin,
     * begin.succ, … until it passes end.  Everything lives in scanned slots so
     * the #succ / #<=> dispatches are GC-safe. */
    if (KORB_OBJECT_P(SELF_RANGE->rbegin) && korb_responds_to(c, SELF_RANGE->rbegin, korb_intern(c->vm, "succ", 4))) {
        const uint32_t mid_succ = korb_intern(c->vm, "succ", 4), mid_cmp = korb_intern(c->vm, "<=>", 3);
        const bool excl = SELF_RANGE->exclude_end;
        slots[0] = UNWRAP(korb_ary_new(c, slots + 1, 8));    /* out */
        slots[1] = SELF_RANGE->rbegin;                       /* cur */
        slots[2] = SELF_RANGE->rend;                         /* end */
        for (int guard = 0; guard < 100000000; guard++) {
            slots[3] = slots[1]; slots[4] = slots[2];        /* cur <=> end (recv slots[3], arg slots[4]) */
            RESULT cr = korb_send_impl(c, slots + 5, mid_cmp, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!FIXNUM_P(cr.value)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(slots[1]));
            const korb_sword_t cmp = FIX2LONG(cr.value);
            if (cmp > 0 || (excl && cmp == 0)) break;
            CHECK(korb_ary_push_val(c, slots + 5, VALUE_REF_AT(&slots[0]), slots[1]));
            if (cmp == 0) break;
            slots[3] = slots[1];                             /* cur = cur.succ */
            RESULT sr = korb_send_impl(c, slots + 4, mid_succ, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            slots[1] = sr.value;
        }
        return RESULT_OK(slots[0]);
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(SELF_RANGE->rbegin));
}

static RESULT korb_m_range_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                              /* → Enumerator over the range's elements */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "map"));
        return korb_enum_new(c, slots + 2, slots[0], slots[1]);
    }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* non-integer range → map over to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
        return korb_m_ary_map(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))));
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_tally(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_range_tally(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* tally(hash) or String range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_tally(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_tally(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    slots[0] = UNWRAP(korb_hash_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0)));
    VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (korb_sword_t i = lo; i < hi; i++) {              /* int range elements are unique → count 1 each */
        slots[1] = LONG2FIX(i);
        CHECK(korb_hash_set(c, slots + 2, h, VALUE_REF_AT(&slots[1]), LONG2FIX(1)));
    }
    return RESULT_OK(VALUE_REF_GET(h));
}
static RESULT korb_m_range_each_with_object(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    if (UNLIKELY(block == NULL)) {                        /* no block → self.to_enum(:each_with_object, memo) */
        slots[0] = VALUE_REF_GET(self);
        slots[1] = ID2SYM(korb_intern(c->vm, "each_with_object", 16));
        slots[2] = VALUE_SLICE_GET(a, 0);
        return korb_send_impl(c, slots + 3, korb_intern(c->vm, "__to_enum_sized", 15), 0, 2, NULL, NULL, NULL);
    }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_each_with_object(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    slots[0] = VALUE_SLICE_GET(a, 0);                 /* memo (rooted) */
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE argv[2] = { LONG2FIX(i), slots[0] };
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_range_overlap(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_RANGE_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Range)", korb_type_name(o));
    korb_sword_t lo1, hi1, lo2, hi2;
    if (!korb_range_int_bounds(SELF_RANGE, &lo1, &hi1) || !korb_range_int_bounds(VAL2RANGE(o), &lo2, &hi2)) {
        const KorbRange *r1 = SELF_RANGE, *r2 = VAL2RANGE(o);     /* non-integer: compare endpoints */
        /* A nil begin is -inf, a nil end is +inf: begin-vs-end with either side
         * infinite is always "<" (the begin precedes the end), and a range with an
         * infinite endpoint is never empty. */
        int c1 = (r1->rbegin == KORB_NIL || r2->rend == KORB_NIL) ? -1 : korb_cmp_values(r1->rbegin, r2->rend);
        int c2 = (r2->rbegin == KORB_NIL || r1->rend == KORB_NIL) ? -1 : korb_cmp_values(r2->rbegin, r1->rend);
        int e1 = (r1->rbegin == KORB_NIL || r1->rend == KORB_NIL) ? -1 : korb_cmp_values(r1->rbegin, r1->rend);
        int e2 = (r2->rbegin == KORB_NIL || r2->rend == KORB_NIL) ? -1 : korb_cmp_values(r2->rbegin, r2->rend);
        if (c1 == 2 || c2 == 2 || e1 == 2 || e2 == 2) return RESULT_OK(KORB_FALSE);   /* incomparable */
        bool a_ok = r2->exclude_end ? (c1 < 0) : (c1 <= 0);
        bool b_ok = r1->exclude_end ? (c2 < 0) : (c2 <= 0);
        bool ne1  = r1->exclude_end ? (e1 < 0) : (e1 <= 0);
        bool ne2  = r2->exclude_end ? (e2 < 0) : (e2 <= 0);
        return RESULT_OK((a_ok && b_ok && ne1 && ne2) ? KORB_TRUE : KORB_FALSE);
    }
    bool ov = lo1 < hi2 && lo2 < hi1 && hi1 > lo1 && hi2 > lo2;   /* half-open [lo,hi) overlap, non-empty */
    return RESULT_OK(ov ? KORB_TRUE : KORB_FALSE);
}
/* min_by(want=-1)/max_by(want=1): element whose block key is the extreme. */
static RESULT korb_m_ary_min_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_max_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_range_by(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *cself, int want) {
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "min_by", 6)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {     /* non-integer range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return (want < 0 ? korb_m_ary_min_by : korb_m_ary_max_by)(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
    }
    if (hi <= lo) return RESULT_OK(KORB_NIL);
    bool have = false;
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!have) { slots[0] = iv; slots[1] = r.value; have = true; continue; }
        int cmp = korb_cmp_full(c, r.value, slots[1]);
        if (cmp == want) { slots[0] = iv; slots[1] = r.value; }
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_range_min_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_range_by(c, slots, self, block, def_env, cself, -1); }
static RESULT korb_m_range_max_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_range_by(c, slots, self, block, def_env, cself, 1); }
static RESULT korb_m_ary_sort_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_sort_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "sort_by", 7)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));      /* materialize, then Array#sort_by */
    return korb_m_ary_sort_by(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_range_reverse_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) {                        /* no block → self.to_enum(:reverse_each) (finite range) */
        slots[0] = VALUE_REF_GET(self);
        slots[1] = ID2SYM(korb_intern(c->vm, "reverse_each", 12));
        return korb_send_impl(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1, NULL, NULL, NULL);
    }
    if (UNLIKELY(SELF_RANGE->rend == KORB_NIL))           /* endless range → can't reverse-iterate from infinity */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(SELF_RANGE->rend));
    if (UNLIKELY(SELF_RANGE->rbegin == KORB_NIL && FIXNUM_P(SELF_RANGE->rend))) {
        /* beginless: count down from the end forever (the block is expected to
         * break; CRuby iterates the same way). */
        for (korb_sword_t i = FIX2LONG(SELF_RANGE->rend) - (SELF_RANGE->exclude_end ? 1 : 0); ; i--) {
            VALUE iv = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* non-int (e.g. String) range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        RESULT r = korb_m_ary_reverse_each(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
        return (r.state == KORB_NORMAL) ? RESULT_OK(VALUE_REF_GET(self)) : r;
    }
    for (korb_sword_t i = hi - 1; i >= lo; i--) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Range#each_slice(n) — consecutive n-element slices over the integer range.
 * block → yield each slice (returns self); no block → Enumerator over the slices.
 * Slices are pre-built into a rooted array so yielding is GC-safe. */
static RESULT korb_m_range_each_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    korb_sword_t n;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1 || !korb_to_index(VALUE_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (UNLIKELY(n <= 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid slice size");
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_each_slice(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                 /* array of slices */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    for (korb_sword_t i = lo; i < hi; i += n) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)n));
        VALUE_REF slice = VALUE_REF_AT(&slots[1]);
        for (korb_sword_t j = 0; j < n && i + j < hi; j++)
            CHECK(korb_ary_push_val(c, slots + 2, slice, LONG2FIX(i + j)));
        CHECK(korb_ary_push_val(c, slots + 2, out, VALUE_REF_GET(slice)));
    }
    if (block == NULL) {
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each_slice"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(out), slots[1]);
    }
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(out))->len; i++) {
        VALUE sl = korb_items_data(VAL2ARY(VALUE_REF_GET(out))->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &sl, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* bsearch (find-minimum mode): smallest i in [lo,hi) where block(i) is truthy. */
/* monotonic uint64 ordering of an IEEE double (so integer bisection of the bit
 * pattern == value bisection): flip sign bit for positives, invert for negatives. */
static uint64_t korb_d2u(double d) { uint64_t b; memcpy(&b, &d, 8); return (b & 0x8000000000000000ULL) ? ~b : (b | 0x8000000000000000ULL); }
static double   korb_u2d(uint64_t u) { uint64_t b = (u & 0x8000000000000000ULL) ? (u & ~0x8000000000000000ULL) : ~u; double d; memcpy(&d, &b, 8); return d; }
/* classify a bsearch block result: 0=exact hit, -1=go left (smaller), +1=go right
 * (bigger), with *cand set when this index is a find-minimum candidate. */
static int korb_bsearch_dir(VALUE v, bool *cand, bool *valid) {
    *cand = false; *valid = true;
    if (FIXNUM_P(v)) { korb_sword_t n = FIX2LONG(v); return n == 0 ? 0 : (n < 0 ? -1 : 1); }
    if (KORB_FLOAT_P(v)) { double d = korb_float_val(v); return d == 0 ? 0 : (d < 0 ? -1 : 1); }
    if (v == KORB_TRUE) { *cand = true; return -1; }   /* find-minimum: true → record + go left */
    if (v == KORB_FALSE || v == KORB_NIL) return 1;    /* go right */
    *valid = false; return 1;                          /* Object/String/etc. → invalid (caller raises) */
}
static RESULT korb_m_range_bsearch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    const KorbRange *rg = SELF_RANGE;
    /* bsearch is only defined for Integer/Float ranges (nil bound = ±∞); any
     * other endpoint (String/Array/Rational/…) → TypeError, reported against the
     * begin's class (CRuby quirk), and before the no-block Enumerator is built. */
    {
        const VALUE bb = rg->rbegin, ee = rg->rend;
        const bool bok = bb == KORB_NIL || KORB_INTEGER_P(bb) || KORB_FLOAT_P(bb);
        const bool eok = ee == KORB_NIL || KORB_INTEGER_P(ee) || KORB_FLOAT_P(ee);
        if (UNLIKELY(!bok || !eok))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't do binary search for %s", korb_type_name(bb));
    }
    if (UNLIKELY(block == NULL)) {                        /* no block → Enumerator */
        slots[0] = UNWRAP(korb_ary_new(c, slots, 0));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "bsearch"));
        return korb_enum_new(c, slots + 2, slots[0], slots[1]);
    }
    const VALUE bv = rg->rbegin, ev = rg->rend;
    const bool excl = rg->exclude_end != 0;
    /* ---- Float range (begin or end is a Float; nil bound → ±Infinity) ---- */
    if (KORB_FLOAT_P(bv) || KORB_FLOAT_P(ev)) {
        double bd = (bv == KORB_NIL) ? -INFINITY : (KORB_FLOAT_P(bv) ? korb_float_val(bv) : (double)FIX2LONG(bv));
        double ed = (ev == KORB_NIL) ?  INFINITY : (KORB_FLOAT_P(ev) ? korb_float_val(ev) : (double)FIX2LONG(ev));
        uint64_t lo = korb_d2u(bd), hi = korb_d2u(ed);
        if (!excl && hi != ~0ULL) hi++;                  /* inclusive end: search up to and incl ed */
        bool found = false; double ans = 0;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            VALUE fv = UNWRAP(korb_float_new(c, slots, korb_u2d(mid)));
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &fv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            bool cand, valid; int dir = korb_bsearch_dir(r.value, &cand, &valid); if (UNLIKELY(!valid)) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (must be numeric, true, false or nil)", korb_type_name(r.value));
            if (dir == 0) return korb_float_new(c, slots, korb_u2d(mid));
            if (dir < 0) { hi = mid; if (cand) { found = true; ans = korb_u2d(mid); } }
            else lo = mid + 1;
        }
        return found ? korb_float_new(c, slots, ans) : RESULT_OK(KORB_NIL);
    }
    /* ---- Integer range ---- */
    if (!(FIXNUM_P(bv) || bv == KORB_NIL) || !(FIXNUM_P(ev) || ev == KORB_NIL))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(bv));
    korb_sword_t lo, hi; bool have_lo = FIXNUM_P(bv), have_hi = FIXNUM_P(ev);
    lo = have_lo ? FIX2LONG(bv) : 0;
    if (have_hi) { hi = excl ? FIX2LONG(ev) : FIX2LONG(ev) + 1; }
    else {
        /* endless: exponentially probe upward until the predicate would go left. */
        korb_sword_t diff = 1; hi = lo + 1;
        for (;;) {
            VALUE iv = LONG2FIX(have_lo ? lo + diff : diff);
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            bool cand, valid; int dir = korb_bsearch_dir(r.value, &cand, &valid); if (UNLIKELY(!valid)) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (must be numeric, true, false or nil)", korb_type_name(r.value));
            if (dir <= 0) { hi = (have_lo ? lo + diff : diff) + 1; break; }
            if (diff > (INTPTR_MAX / 2)) { hi = INTPTR_MAX; break; }
            diff *= 2;
        }
    }
    if (!have_lo) {
        /* beginless: exponentially probe downward for a lower bound. */
        korb_sword_t diff = 1; lo = hi - 1;
        for (;;) {
            const korb_sword_t probe = hi - diff;
            VALUE iv = LONG2FIX(probe);
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            bool cand, valid; int dir = korb_bsearch_dir(r.value, &cand, &valid); if (UNLIKELY(!valid)) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (must be numeric, true, false or nil)", korb_type_name(r.value));
            if (dir > 0) { lo = probe + 1; break; }       /* predicate goes right here → bound below is lo */
            if (diff > (INTPTR_MAX / 2)) { lo = INTPTR_MIN; break; }
            diff *= 2;
        }
    }
    bool found = false; korb_sword_t ans = 0;
    while (lo < hi) {
        korb_sword_t mid = lo + (hi - lo) / 2;
        VALUE iv = LONG2FIX(mid);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        bool cand, valid; int dir = korb_bsearch_dir(r.value, &cand, &valid); if (UNLIKELY(!valid)) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (must be numeric, true, false or nil)", korb_type_name(r.value));
        if (dir == 0) return RESULT_OK(LONG2FIX(mid));
        if (dir < 0) { hi = mid; if (cand) { found = true; ans = mid; } }
        else lo = mid + 1;
    }
    return RESULT_OK(found ? LONG2FIX(ans) : KORB_NIL);
}
static RESULT korb_m_ary_minmax(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_minmax(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    /* Bounded range: decide via a single begin<=>end, no iteration.
     *   inclusive  → begin>end ? [nil,nil] : [begin, end]
     *   exclusive  → begin>=end ? [nil,nil] : (non-empty → min/max path: the max
     *                is the predecessor of end, needs Integer step / iteration)   */
    {
        const KorbRange *r = VAL2RANGE(VALUE_REF_GET(self));
        if (r->rbegin != KORB_NIL && r->rend != KORB_NIL) {
            slots[0] = VALUE_REF_GET(self);
            int cmp = 0;
            RESULT cr = korb_cmp_spaceship(c, slots + 1, VAL2RANGE(slots[0])->rbegin, VAL2RANGE(slots[0])->rend, &cmp);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            const KorbRange *rr = VAL2RANGE(slots[0]);
            const bool empty = rr->exclude_end ? (cmp >= 0) : (cmp > 0);
            if (empty || !rr->exclude_end) {                 /* inclusive fully resolved; exclusive only when empty */
                slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
                VALUE_REF dst = VALUE_REF_AT(&slots[1]);
                rr = VAL2RANGE(slots[0]);
                const VALUE lo = empty ? KORB_NIL : rr->rbegin, hi = empty ? KORB_NIL : rr->rend;
                slots[2] = lo; CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
                slots[2] = hi; CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
                return RESULT_OK(VALUE_REF_GET(dst));
            }
            /* non-empty exclusive → fall through to the min/max path */
        }
    }
    /* [min, max] via #min/#max so endless/beginless raise RangeError (not iterate). */
    RESULT mn = korb_m_range_min(c, slots, self, VALUE_SLICE_MAKE(NULL, 0));
    if (UNLIKELY(mn.state != KORB_NORMAL)) return mn;
    slots[0] = mn.value;
    RESULT mx = korb_m_range_max(c, slots + 1, self, VALUE_SLICE_MAKE(NULL, 0));
    if (UNLIKELY(mx.state != KORB_NORMAL)) return mx;
    slots[1] = mx.value;
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Range#sort/min/max/minmax with a comparator block: materialize to_a (ascending)
 * then delegate to the block-aware Array method.  No block → the integer-range
 * fast paths.  A count arg on min/max (e.g. min(2)) also keeps the fast path. */
static RESULT korb_m_range_sort_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                    NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
    if (block == NULL) return RESULT_OK(slots[0]);
    return korb_m_ary_sort(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
}
static RESULT korb_m_range_min_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                   NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL || (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL))
        return korb_m_range_min(c, slots, self, a);
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_min(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
}
static RESULT korb_m_range_max_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                   NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL || (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL))
        return korb_m_range_max(c, slots, self, a);
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_max(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
}
static RESULT korb_m_range_minmax_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                      NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) return korb_m_range_minmax(c, slots, self, a);
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_minmax(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
}
static RESULT korb_m_ary_chunk_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self);
static RESULT korb_m_ary_slice_when(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self);
static RESULT korb_m_range_chunk_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_chunk_while(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_ary_chunk(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_chunk(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_chunk(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_range_slice_when(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_slice_when(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_range_each_cons(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    RESULT r = korb_m_ary_each_cons(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    if (block != NULL && r.state == KORB_NORMAL) return RESULT_OK(VALUE_REF_GET(self));   /* block form → self (the range) */
    return r;
}
static RESULT korb_m_range_uniq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_uniq(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
}
static RESULT korb_m_range_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    slots[0] = UNWRAP(korb_range_by(c, slots, self, block, def_env, cself, -1));   /* min_by */
    slots[1] = UNWRAP(korb_range_by(c, slots + 1, self, block, def_env, cself, 1)); /* max_by */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(VALUE_REF_GET(VALUE_REF_AT(&slots[2])));
}
static RESULT korb_m_range_partition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "partition", 9)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* non-int (e.g. String) range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_partition(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));         /* matching */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));     /* non-matching */
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(KORB_TRUTHY(r.value) ? &slots[0] : &slots[1]), iv));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(VALUE_REF_GET(VALUE_REF_AT(&slots[2])));
}
static RESULT korb_m_range_flat_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#flat_map without a block (Enumerator) is not supported");
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_flat_map(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))));
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_ARRAY_P(r.value)) {                 /* one level of flattening */
            slots[1] = r.value;
            uint32_t sublen = VAL2ARY(slots[1])->len;
            for (uint32_t j = 0; j < sublen; j++)    /* re-read sub each push (GC may move it) */
                CHECK(korb_ary_push_val(c, slots + 2, dst, korb_items_data(VAL2ARY(slots[1])->items)[j]));
        } else {
            CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_range_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_drop(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    korb_sword_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to drop negative size");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (korb_sword_t i = lo + n; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_drop_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "drop_while", 10)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_drop_while(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    bool dropping = true;
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        if (dropping) {
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) continue;
            dropping = false;
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "take_while", 10)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* non-int (e.g. String) range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_take_while(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!KORB_TRUTHY(r.value)) break;
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_range_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool keep) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return (keep ? korb_m_ary_grep : korb_m_ary_grep_v)(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (korb_sword_t i = lo; i < hi; i++) {
        slots[0] = LONG2FIX(i);
        if (korb_case_eq(c, VALUE_SLICE_GET(a, 0), slots[0]) == keep) {
            if (block != NULL) {
                RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
            } else CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)   { return korb_range_grep(c, slots, self, a, block, def_env, cself, true); }
static RESULT korb_m_range_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_range_grep(c, slots, self, a, block, def_env, cself, false); }
static RESULT korb_m_range_group_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "group_by", 8)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_group_by(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    slots[0] = UNWRAP(korb_hash_new(c, slots, 4));     VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (korb_sword_t i = lo; i < hi; i++) {
        slots[1] = LONG2FIX(i);                        /* element */
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[2] = r.value;                            /* key */
        int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(h)), slots[2]);
        if (idx < 0) {
            slots[3] = UNWRAP(korb_ary_new(c, slots + 4, 4));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_hash_set(c, slots + 4, h, VALUE_REF_AT(&slots[2]), slots[3]));
        } else {
            slots[3] = korb_items_data(VAL2HASH(VALUE_REF_GET(h))->items)[2 * idx + 1];
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(h));
}
static RESULT korb_m_range_filter_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "filter_map", 10)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_filter_map(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_each_wi(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_each_wi(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        RESULT r = korb_m_ary_each_wi(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
        return (r.state == KORB_NORMAL && block != NULL) ? RESULT_OK(VALUE_REF_GET(self)) : r;
    }
    if (block == NULL) {                              /* → Enumerator of [elem, index] pairs */
        slots[0] = UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0)));
        VALUE_REF pairs = VALUE_REF_AT(&slots[0]);
        for (korb_sword_t i = lo; i < hi; i++) {
            slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
            CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), LONG2FIX(i)));
            CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), LONG2FIX(i - lo)));
            CHECK(korb_ary_push_val(c, slots + 2, pairs, slots[1]));
        }
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each_with_index"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(pairs), slots[1]);
    }
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE argv[2] = { LONG2FIX(i), LONG2FIX(i - lo) };
        RESULT r = korb_block_yield(c, slots, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static VALUE korb_zip_elem(VALUE arg, uint32_t i);    /* array_int_ext.c */
static RESULT korb_m_ary_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t k = VALUE_SLICE_LEN(a);
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {     /* non-integer range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_zip(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    slots[0] = (block == NULL) ? UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))) : KORB_NIL;
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (korb_sword_t i = lo; i < hi; i++) {
        uint32_t idx = (uint32_t)(i - lo);
        slots[1] = UNWRAP(korb_ary_new(c, slots + 2, k + 1));
        VALUE_REF row = VALUE_REF_AT(&slots[1]);
        CHECK(korb_ary_push_val(c, slots + 2, row, LONG2FIX(i)));
        for (uint32_t j = 0; j < k; j++)
            CHECK(korb_ary_push_val(c, slots + 2, row, korb_zip_elem(VALUE_SLICE_GET(a, j), idx)));   /* Array/Range arg */
        if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        } else CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
    }
    return RESULT_OK(block != NULL ? KORB_NIL : VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) > 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", VALUE_SLICE_LEN(a));
    bool has_pat = VALUE_SLICE_LEN(a) >= 1;
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_one(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    uint32_t cnt = 0;
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        bool t;
        if (has_pat) {
            CHECK(korb_pat_eq(c, slots, VALUE_SLICE_GET(a, 0), iv, &t));
        } else if (block != NULL) {
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else t = true;                              /* every Integer is truthy */
        if (t && ++cnt > 1) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(cnt == 1 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_ary_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_find_index(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    bool has_arg = VALUE_SLICE_LEN(a) >= 1;
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        bool hit;
        if (has_arg) hit = korb_value_eq(iv, VALUE_SLICE_GET(a, 0));
        else {
            if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#find_index without arg or block is not supported");
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            hit = KORB_TRUTHY(r.value);
        }
        if (hit) return RESULT_OK(LONG2FIX(i - lo));
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self);
static RESULT korb_m_range_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {     /* non-integer range → reduce over to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_reduce(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    if (block == NULL) {                                   /* symbol form */
        uint32_t na = VALUE_SLICE_LEN(a), op_mid;
        if (UNLIKELY(na < 1 || !korb_reduce_op(c, VALUE_SLICE_GET(a, na - 1), &op_mid)))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block or operator symbol given");
        korb_sword_t i = lo;
        if (na >= 2) slots[0] = VALUE_SLICE_GET(a, 0);
        else { if (lo >= hi) return RESULT_OK(KORB_NIL); slots[0] = LONG2FIX(lo); i = lo + 1; }
        for (; i < hi; i++) {
            slots[1] = slots[0]; slots[2] = LONG2FIX(i);
            RESULT r = korb_send_impl(c, slots + 3, op_mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;
        }
        return RESULT_OK(slots[0]);
    }
    korb_sword_t i = lo;
    if (VALUE_SLICE_LEN(a) >= 1) {
        slots[0] = VALUE_SLICE_GET(a, 0);
    } else {
        if (lo >= hi) return RESULT_OK(KORB_NIL);
        slots[0] = LONG2FIX(lo); i = lo + 1;
    }
    for (; i < hi; i++) {
        VALUE argv[2] = { slots[0], LONG2FIX(i) };
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    return RESULT_OK(slots[0]);
}

/* select (keep==true) / reject (keep==false) over an integer range */
static RESULT korb_m_ary_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_range_filter(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *captured_self, bool keep) {
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "select", 6)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return (keep ? korb_m_ary_select : korb_m_ary_reject)(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, captured_self);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_range_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_range_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_range_filter(c, slots, self, block, def_env, captured_self, false); }

static RESULT korb_m_range_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "find", 4)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_find(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}

/* any? (0) / all? (1) / none? (2) over an integer range, with block */
static RESULT korb_m_ary_any(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_none(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_range_quant(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self, int mode) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) > 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", VALUE_SLICE_LEN(a));
    bool has_pat = VALUE_SLICE_LEN(a) >= 1;
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {     /* non-integer range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return (mode == 0 ? korb_m_ary_any : mode == 1 ? korb_m_ary_all : korb_m_ary_none)(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    for (korb_sword_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        bool t;
        if (has_pat) {
            CHECK(korb_pat_eq(c, slots, VALUE_SLICE_GET(a, 0), iv, &t));     /* pattern === element */
        } else if (block != NULL) {
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else {
            t = KORB_TRUTHY(iv);                                /* int elements are always truthy */
        }
        if (mode == 0 && t) return RESULT_OK(KORB_TRUE);
        if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);
        if (mode == 2 && t) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_range_any(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)  { return korb_range_quant(c, slots, self, a, block, def_env, captured_self, 0); }
static RESULT korb_m_range_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)  { return korb_range_quant(c, slots, self, a, block, def_env, captured_self, 1); }
static RESULT korb_m_range_none(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { return korb_range_quant(c, slots, self, a, block, def_env, captured_self, 2); }

static RESULT korb_range_step_impl(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self, uint8_t is_pct) {
    uint8_t na = VALUE_SLICE_LEN(a) >= 1 ? 1 : 0;
    VALUE sv = na ? VALUE_SLICE_GET(a, 0) : LONG2FIX(1);
    if (na && !FIXNUM_P(sv) && !KORB_FLOAT_P(sv) && !KORB_BIGNUM_P(sv) && !KORB_RATIONAL_P(sv)) {   /* coerce a non-numeric step via #to_int */
        const uint32_t to_int = korb_intern(c->vm, "to_int", 6);
        bool got = false;
        if (KORB_OBJECT_P(sv) && korb_responds_to_coerce_p(c, slots, &sv, to_int)) {
            slots[0] = sv;
            RESULT sr = korb_send_impl(c, slots + 1, to_int, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (KORB_INTEGER_P(sr.value)) { sv = sr.value; got = true; }
        }
        /* CRuby also accepts a step that only knows #coerce: the pair's 2nd
         * element is the step in the range's own numeric domain. */
        const uint32_t co = korb_intern(c->vm, "coerce", 6);
        if (!got && KORB_OBJECT_P(sv) && korb_responds_to_coerce_p(c, slots, &sv, co)) {
            slots[0] = sv;
            slots[1] = SELF_RANGE->rbegin != KORB_NIL ? SELF_RANGE->rbegin : LONG2FIX(1);
            RESULT cr = korb_send_impl(c, slots + 2, co, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (KORB_ARRAY_P(cr.value) && VAL2ARY(cr.value)->len == 2)
                sv = korb_items_data(VAL2ARY(cr.value)->items)[1];
        }
    }
    /* An ArithmeticSequence describes a *numeric* progression; a String (or any
     * other Comparable) range steps through #succ / #+ and is a plain
     * Enumerator.  Which one applies is decided by the range's own endpoints. */
    const VALUE probe_ = SELF_RANGE->rbegin != KORB_NIL ? SELF_RANGE->rbegin : SELF_RANGE->rend;
    const bool numeric_range = FIXNUM_P(probe_) || KORB_FLOAT_P(probe_) ||
                               KORB_BIGNUM_P(probe_) || KORB_RATIONAL_P(probe_);
    if (block == NULL) {
        if (!numeric_range) {                         /* → Enumerator over the stepped values (lazy) */
            slots[0] = VALUE_REF_GET(self);
            slots[1] = ID2SYM(korb_intern(c->vm, is_pct ? "%" : "step", is_pct ? 1 : 4));
            if (na) slots[2] = sv;
            return korb_send(c, slots + 2 + na, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1 + na);
        }
        if (UNLIKELY((FIXNUM_P(sv) && sv == LONG2FIX(0)) || (KORB_FLOAT_P(sv) && korb_float_val(sv) == 0.0)))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        return korb_arithseq_new(c, slots, VALUE_REF_GET(self), sv, KORB_NIL, na, is_pct);
    }
    if (UNLIKELY(SELF_RANGE->rbegin == KORB_NIL))     /* iterating a beginless range is undefined */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "#step iteration for beginless ranges is meaningless");
    {   /* Endless numeric range (no end): iterate from begin upward by a positive
         * step forever — the block is expected to break.  Yields Float when either
         * the begin or the step is a Float, else Integer (CRuby semantics). */
        const KorbRange *const rng = SELF_RANGE;
        if (rng->rend == KORB_NIL && rng->rbegin != KORB_NIL
            && (FIXNUM_P(rng->rbegin) || KORB_FLOAT_P(rng->rbegin))) {
            const bool flo = KORB_FLOAT_P(sv) || KORB_FLOAT_P(rng->rbegin);
            if (flo) {
                double dbeg, dstep;
                if (!korb_num_to_d(rng->rbegin, &dbeg) || !korb_num_to_d(sv, &dstep))
                    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(sv));
                if (UNLIKELY(dstep == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
                if (dstep > 0) {
                    for (long i = 0; ; i++) {         /* count-based: no float-error accumulation */
                        slots[0] = UNWRAP(korb_float_new(c, slots + 1, dbeg + (double)i * dstep));
                        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
                        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                    }
                }
            }
            else if (FIXNUM_P(sv) && FIX2LONG(sv) > 0) {
                const korb_sword_t st = FIX2LONG(sv);
                for (korb_sword_t i = FIX2LONG(rng->rbegin); ; i += st) {
                    VALUE iv = LONG2FIX(i);
                    RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                }
            }
            else if (UNLIKELY(sv == LONG2FIX(0))) {
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
            }
            /* non-positive / non-numeric step → fall through to the bounded paths below */
        }
    }
    {   /* Float step OR Float range bounds → iterate over doubles (count-based, like CRuby). */
        const KorbRange *const rng = SELF_RANGE;
        if ((KORB_FLOAT_P(sv) || KORB_FLOAT_P(rng->rbegin) || KORB_FLOAT_P(rng->rend))
            && rng->rbegin != KORB_NIL && rng->rend != KORB_NIL) {
            double dbeg, dend, dstep;
            if (korb_num_to_d(rng->rbegin, &dbeg) && korb_num_to_d(rng->rend, &dend) && korb_num_to_d(sv, &dstep)) {
                if (UNLIKELY(dstep == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");   /* negative → count formula gives empty/backward */
                const bool excl = rng->exclude_end;
                const long lim = korb_float_step_n(dbeg, dend, dstep, excl);
                for (long i = 0; i < lim; i++) {
                    slots[0] = UNWRAP(korb_float_new(c, slots + 1, korb_float_step_at(dbeg, dend, dstep, i, excl)));
                    RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                }
                return RESULT_OK(VALUE_REF_GET(self));
            }
        }
    }
    /* Generic non-numeric range with a block (String range, or any element that
     * defines #+ and #<=>): iterate v = begin; yield; v = v + step — while
     * v <=> end stays in range (CRuby range_step for comparable+addable values).
     * A step that doesn't advance toward end (e.g. 0) yields nothing. */
    {
        const KorbRange *const rng = SELF_RANGE;
        const VALUE bg = rng->rbegin;
        const bool numeric_begin = FIXNUM_P(bg) || KORB_FLOAT_P(bg) || KORB_BIGNUM_P(bg) || KORB_RATIONAL_P(bg);
        const bool int_step = FIXNUM_P(sv) || KORB_BIGNUM_P(sv);
        /* An Integer step over a String range means "#succ that many times" and
         * falls through; anything else goes through #+, which is also how a Float
         * step gets the element type's own TypeError ("Float into String"). */
        if (block != NULL && bg != KORB_NIL && !numeric_begin && !int_step) {
            const bool excl = rng->exclude_end;       /* value copy — safe across GC */
            const uint32_t mid_plus = korb_intern(c->vm, "+", 1);
            slots[0] = bg;                            /* v    — all rooted (moving GC): the range,   */
            slots[1] = sv;                            /* step   its end, v, step, next may all move   */
            slots[2] = rng->rend;                     /* endv   under the #+/#<=>/yield dispatches.   */
            /* CRuby decides the direction once, from begin <=> end, and only
             * checks that the step advances the right way on the FIRST step. */
            int forward = 1;
            if (slots[2] != KORB_NIL) {
                int dcmp; RESULT dr = korb_range_cmp(c, slots + 3, slots[0], slots[2], &dcmp);
                if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
                if (dcmp == 2) return RESULT_OK(VALUE_REF_GET(self));   /* incomparable ends → nothing */
                forward = dcmp <= 0;
            }
            bool first = true;
            for (;;) {
                if (slots[2] != KORB_NIL) {           /* bounded: stop once v passes end */
                    int cmp; RESULT cr = korb_range_cmp(c, slots + 3, slots[0], slots[2], &cmp);
                    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                    if (cmp == 2) break;
                    if (forward ? (excl ? cmp >= 0 : cmp > 0) : (excl ? cmp <= 0 : cmp < 0)) break;
                    if (cmp == 0) {                   /* the last element: yield it without a further #+ */
                        if (!excl) { RESULT yr = korb_block_yield(c, slots + 3, block, def_env, &slots[0], 1, captured_self);
                                     if (UNLIKELY(yr.state != KORB_NORMAL)) return yr; }
                        break;
                    }
                }
                slots[3] = slots[0];                  /* recv for v+step */
                slots[4] = slots[1];
                RESULT nr = korb_send(c, slots + 5, mid_plus, 0, 1);   /* next = v + step */
                if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
                slots[3] = nr.value;                  /* park next (rooted) before the <=> dispatch may GC */
                if (first) {                          /* does the step move toward end at all? */
                    int pc; RESULT pr = korb_range_cmp(c, slots + 4, slots[0], slots[3], &pc);   /* v <=> next */
                    if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
                    if (pc == 2 || (forward ? pc >= 0 : pc <= 0)) break;   /* no progress → stop, no yield */
                    first = false;
                }
                RESULT yr = korb_block_yield(c, slots + 5, block, def_env, &slots[0], 1, captured_self);
                if (UNLIKELY(yr.state != KORB_NORMAL)) return yr;
                slots[0] = slots[3];                  /* v = next */
            }
            return RESULT_OK(VALUE_REF_GET(self));
        }
    }
    if (UNLIKELY(!FIXNUM_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(sv));
    korb_sword_t st = FIX2LONG(sv);
    if (UNLIKELY(st == 0)) {
        if (!numeric_range) return RESULT_OK(VALUE_REF_GET(self));   /* CRuby: no iteration, no error */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
    }
    korb_sword_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* non-int (e.g. String) range → stride over to_a */
        if (st > 0) {                                    /* (backward string stepping not supported here) */
            slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
            for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i += (uint32_t)st) {
                VALUE ev = korb_items_data(VAL2ARY(slots[0])->items)[i];
                RESULT r = korb_block_yield(c, slots + 1, block, def_env, &ev, 1, captured_self);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            }
        }
        return RESULT_OK(VALUE_REF_GET(self));
    }
    if (st > 0) {
        for (korb_sword_t i = lo; i < hi; i += st) {
            VALUE iv = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
    } else {                                              /* negative step → iterate downward from begin */
        const KorbRange *const rng = SELF_RANGE;
        if (FIXNUM_P(rng->rbegin) && FIXNUM_P(rng->rend)) {
            const korb_sword_t b = FIX2LONG(rng->rbegin), e = FIX2LONG(rng->rend);
            const korb_sword_t limit = rng->exclude_end ? e : e - 1;   /* loop while i > limit */
            for (korb_sword_t i = b; i > limit; i += st) {
                VALUE iv = LONG2FIX(i);
                RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            }
        }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_range_step(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_range_step_impl(c, slots, self, a, block, def_env, cself, 0);
}
static RESULT korb_m_range_pct(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_range_step_impl(c, slots, self, a, block, def_env, cself, 1);
}

