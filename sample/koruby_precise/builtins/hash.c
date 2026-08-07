/* koruby_precise — hash.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Object#hash --------------------------------------------------------- *
 * Deterministic structural hash for the public #hash method (distinct from
 * korb_value_hash, which only the Hash open-addressing index uses and covers
 * indexable immediates only).  Immediates/String reuse it; Array/Hash combine
 * element hashes; Float/Bignum hash by value; everything else by identity. */
/* depth-capped to survive self-referential structures (`a=[]; a<<a; a.hash`):
 * past KORB_DEEP_HASH_MAX, fold in a constant instead of recursing.  A cycle
 * recurses the same path each call, so the result stays deterministic and two
 * structurally-equal recursive containers still hash alike. */
#define KORB_DEEP_HASH_MAX 96u
static uint64_t korb_deep_hash_d(VALUE v, uint32_t depth) {
    if (FIXNUM_P(v) || SYMBOL_P(v) || KORB_STRING_P(v) || v == KORB_NIL || v == KORB_TRUE || v == KORB_FALSE)
        return korb_value_hash(v);
    if (KORB_FLOAT_P(v)) {
        union { double d; uint64_t u; } t; t.d = korb_float_val(v);
        uint64_t x = (t.d == 0.0) ? 0 : t.u;           /* +0.0 / -0.0 hash alike */
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 29;
        return x;
    }
    if (depth >= KORB_DEEP_HASH_MAX) return 0xC0FFEEULL;   /* recursion guard */
    if (KORB_ARRAY_P(v)) {
        KorbArray *const a = VAL2ARY(v);
        if (a->head.flags & KORB_FL_JOIN_VISITING) return 0xC0FFEEULL;   /* recursive → constant (no exponential blowup) */
        a->head.flags |= KORB_FL_JOIN_VISITING;
        uint64_t h = 0x345678ULL + a->len;
        for (uint32_t i = 0; i < a->len; i++) h = h * 31u + korb_deep_hash_d(korb_items_data(a->items)[i], depth + 1);
        a->head.flags &= ~KORB_FL_JOIN_VISITING;         /* pure computation: no GC, `a` stays valid */
        return h;
    }
    if (KORB_HASH_P(v)) {                              /* order-independent (xor) */
        KorbHash *const hh = VAL2HASH(v);
        if (hh->head.flags & KORB_FL_JOIN_VISITING) return 0xBEEFULL;   /* recursive hash */
        hh->head.flags |= KORB_FL_JOIN_VISITING;
        uint64_t h = 0x9e3779b9ULL + hh->len;
        for (uint32_t i = 0; i < hh->len; i++)
            h ^= korb_deep_hash_d(korb_items_data(hh->items)[2*i], depth + 1) * 31u + korb_deep_hash_d(korb_items_data(hh->items)[2*i+1], depth + 1);
        hh->head.flags &= ~KORB_FL_JOIN_VISITING;
        return h;
    }
    if (KORB_SET_P(v)) {                              /* order-independent (xor of element hashes) */
        const KorbArray *a = VAL2ARY(VAL2SET(v)->elems);
        uint64_t h = 0x517cc1b7ULL + a->len;
        for (uint32_t i = 0; i < a->len; i++) h ^= korb_deep_hash_d(korb_items_data(a->items)[i], depth + 1);
        return h;
    }
    if (KORB_RATIONAL_P(v)) {                          /* reduced num/den → equal Rationals hash alike */
        const KorbRational *rt = VAL2RAT(v);
        return korb_deep_hash_d(rt->num, depth + 1) * 31u + korb_deep_hash_d(rt->den, depth + 1);
    }
    if (KORB_COMPLEX_P(v)) {
        const KorbComplex *cx = VAL2CPX(v);
        return korb_deep_hash_d(cx->re, depth + 1) * 31u + korb_deep_hash_d(cx->im, depth + 1);
    }
    if (KORB_RANGE_P(v)) {                             /* == begin/end + exclude_end */
        const KorbRange *r = VAL2RANGE(v);
        uint64_t h = 0x9e3779b1ULL + (r->exclude_end ? 1u : 0u);
        h = h * 31u + korb_deep_hash_d(r->rbegin, depth + 1);
        h = h * 31u + korb_deep_hash_d(r->rend, depth + 1);
        return h;
    }
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(v)) {
        mpz_t z; korb_to_mpz(v, z);
        uint64_t h = (mpz_sgn(z) < 0) ? 0xABCDEF01ULL : 0x12345678ULL;
        const size_t n = mpz_size(z);
        for (size_t i = 0; i < n; i++) h = h * 1099511628211ULL + (uint64_t)mpz_getlimbn(z, (mp_size_t)i);
        mpz_clear(z);
        return h;
    }
#endif
    return (uint64_t)(uintptr_t)v;                     /* identity (user objects etc.) */
}
static uint64_t korb_deep_hash(VALUE v) { return korb_deep_hash_d(v, 0); }
static RESULT korb_m_obj_hash(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    return RESULT_OK(LONG2FIX((intptr_t)(korb_deep_hash(VALUE_REF_GET(self)) >> 2)));   /* >>2 keeps it FIXABLE */
}

/* ---- Hash methods -------------------------------------------------------- */

static RESULT korb_m_hash_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_HASH->len)); }
static RESULT korb_m_hash_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_HASH->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_hash_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_hash_default(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1 && SELF_HASH->default_proc != KORB_NIL) {   /* default(key) + proc → proc.call(self, key) */
        slots[0] = SELF_HASH->default_proc;
        slots[1] = VALUE_REF_GET(self);
        slots[2] = VALUE_SLICE_GET(a, 0);
        return korb_send_impl(c, slots + 3, korb_intern(c->vm, "call", 4), 0, 2, NULL, NULL, KORB_NIL);
    }
    return RESULT_OK(SELF_HASH->default_val);
}

static RESULT korb_m_hash_cmp_by_id(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    VALUE v = VALUE_REF_GET(self);
    ((AroObjectHeader *)(uintptr_t)v)->flags |= KORB_FL_CMP_BY_ID;
    return RESULT_OK(v);
}
static RESULT korb_m_hash_cmp_by_id_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    return RESULT_OK((((AroObjectHeader *)(uintptr_t)VALUE_REF_GET(self))->flags & KORB_FL_CMP_BY_ID) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_hash_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT ferr; const int32_t idx = korb_hash_find_ctx(c, slots, self, VALUE_SLICE_GET(a, 0), &ferr);
    if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
    const KorbHash *h = SELF_HASH;                        /* re-read after possible eql? dispatch */
    if (idx >= 0) return RESULT_OK(korb_items_data(h->items)[2 * idx + 1]);
    if (h->default_proc != KORB_NIL) {                    /* Hash.new { |h,k| } → default_proc.call(self, key) */
        slots[0] = h->default_proc;
        slots[1] = VALUE_REF_GET(self);
        slots[2] = VALUE_SLICE_GET(a, 0);
        return korb_send(c, slots + 3, korb_intern(c->vm, "call", 4), 0, 2);
    }
    return RESULT_OK(h->default_val);
}

static RESULT korb_m_hash_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    const VALUE k0 = VALUE_SLICE_GET(a, 0);
    /* CRuby dups + freezes a mutable String key, so later mutation of the caller's
     * string can't corrupt the stored key (symbols/other keys are stored as-is). */
    if (UNLIKELY(KORB_STRING_P(k0) && !(((const AroObjectHeader *)(uintptr_t)k0)->flags & KORB_FL_FROZEN))) {
        slots[0] = k0;
        RESULT dr = korb_send(c, slots + 1, korb_intern(c->vm, "dup", 3), 0, 0);   /* GC-safe copy via #dup */
        if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
        slots[0] = dr.value;
        if (AROH_IS_GC_OBJECT(slots[0])) ((AroObjectHeader *)(uintptr_t)slots[0])->flags |= KORB_FL_FROZEN;
        CHECK(korb_hash_set(c, slots + 1, self, VALUE_REF_AT(&slots[0]), VALUE_SLICE_GET(a, 1)));
        return RESULT_OK(VALUE_SLICE_GET(a, 1));
    }
    CHECK(korb_hash_set(c, slots, self, VALUE_SLICE_REF(a, 0), VALUE_SLICE_GET(a, 1)));
    return RESULT_OK(VALUE_SLICE_GET(a, 1));        /* []= yields the value (rooted re-read) */
}

static RESULT korb_m_hash_assoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    /* CRuby Hash#assoc is a linear scan comparing keys with #== (not the eql?/hash
     * lookup), so e.g. assoc(1) matches a 1.0 key. */
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* needle (rooted across == dispatch) */
    const uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        const VALUE k = korb_items_data(h->items)[2 * i];
        bool match;
        if (KORB_OBJECT_P(k) || KORB_OBJECT_P(slots[0])) {  /* user == → dispatch (key == needle) */
            slots[1] = k; slots[2] = slots[0];
            RESULT r = korb_send_impl(c, slots + 3, c->vm->mid_eq, 0, 1, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            match = KORB_TRUTHY(r.value);
        } else {
            match = korb_value_eq(k, slots[0]);
        }
        if (match) {
            const KorbHash *h2 = VAL2HASH(VALUE_REF_GET(self));
            slots[1] = korb_items_data(h2->items)[2 * i]; slots[2] = korb_items_data(h2->items)[2 * i + 1];
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
            VALUE_REF arr = VALUE_REF_AT(slots + 3);
            CHECK(korb_ary_push_val(c, slots + 4, arr, slots[1]));
            CHECK(korb_ary_push_val(c, slots + 4, arr, slots[2]));
            return RESULT_OK(VALUE_REF_GET(arr));
        }
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_hash_key_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT ferr; const int32_t idx = korb_hash_find_ctx(c, slots, self, VALUE_SLICE_GET(a, 0), &ferr);
    if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
    return RESULT_OK(idx >= 0 ? KORB_TRUE : KORB_FALSE);
}

/* Hash#== — same size, same keys (eql?/hash), values compared with #== (so a
 * user object value's == is honoured, which korb_value_eq cannot do without a CTX). */
static RESULT korb_m_hash_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE other = VALUE_SLICE_GET(a, 0);
    if (VALUE_REF_GET(self) == other) return RESULT_OK(KORB_TRUE);
    if (!KORB_HASH_P(other)) return RESULT_OK(KORB_FALSE);
    if (VAL2HASH(VALUE_REF_GET(self))->len != VAL2HASH(other)->len) return RESULT_OK(KORB_FALSE);
    if (VAL2HASH(VALUE_REF_GET(self))->len != 0 &&        /* non-empty: differ only by compare_by_identity → not equal (two empty hashes stay equal regardless) */
        ((VAL2HASH(VALUE_REF_GET(self))->head.flags ^ VAL2HASH(other)->head.flags) & KORB_FL_CMP_BY_ID))
        return RESULT_OK(KORB_FALSE);
    if (VAL2HASH(VALUE_REF_GET(self))->head.flags & KORB_FL_JOIN_VISITING) return RESULT_OK(KORB_TRUE);   /* recursive */
    slots[0] = VALUE_REF_GET(self); slots[1] = other;     /* root both across value == dispatch */
    const uint32_t n = VAL2HASH(slots[0])->len;
    VAL2HASH(slots[0])->head.flags |= KORB_FL_JOIN_VISITING;
    VALUE result = KORB_TRUE;
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *x = VAL2HASH(slots[0]);
        const VALUE k = korb_items_data(x->items)[2 * i], v = korb_items_data(x->items)[2 * i + 1];
        const int32_t j = korb_hash_find(VAL2HASH(slots[1]), k);   /* key match = eql?/hash */
        if (j < 0) { result = KORB_FALSE; break; }
        const VALUE v2 = korb_items_data(VAL2HASH(slots[1])->items)[2 * j + 1];
        if (KORB_OBJECT_P(v) || KORB_ARRAY_P(v) || KORB_HASH_P(v) ||
            KORB_OBJECT_P(v2) || KORB_ARRAY_P(v2) || KORB_HASH_P(v2)) {   /* value == via dispatch (recurses for nested) */
            slots[2] = v; slots[3] = v2;
            RESULT r = korb_send_impl(c, slots + 4, c->vm->mid_eq, 0, 1, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) { VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING; return r; }
            if (!KORB_TRUTHY(r.value)) { result = KORB_FALSE; break; }
        } else if (!korb_value_eq(v, v2)) {
            result = KORB_FALSE; break;
        }
    }
    VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING;
    return RESULT_OK(result);
}
/* Hash#eql? — like Hash#== but values compared with #eql? (type-strict). */
static RESULT korb_m_hash_eql(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE other = VALUE_SLICE_GET(a, 0);
    if (VALUE_REF_GET(self) == other) return RESULT_OK(KORB_TRUE);
    if (!KORB_HASH_P(other)) return RESULT_OK(KORB_FALSE);
    if (VAL2HASH(VALUE_REF_GET(self))->len != VAL2HASH(other)->len) return RESULT_OK(KORB_FALSE);
    if (VAL2HASH(VALUE_REF_GET(self))->head.flags & KORB_FL_JOIN_VISITING) return RESULT_OK(KORB_TRUE);   /* recursive → assume equal */
    slots[0] = VALUE_REF_GET(self); slots[1] = other;
    const uint32_t mid_eql = korb_intern(c->vm, "eql?", 4);
    const uint32_t n = VAL2HASH(slots[0])->len;
    VAL2HASH(slots[0])->head.flags |= KORB_FL_JOIN_VISITING;
    VALUE result = KORB_TRUE;
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *x = VAL2HASH(slots[0]);
        const VALUE k = korb_items_data(x->items)[2 * i], v = korb_items_data(x->items)[2 * i + 1];
        const int32_t j = korb_hash_find(VAL2HASH(slots[1]), k);
        if (j < 0) { result = KORB_FALSE; break; }
        const VALUE v2 = korb_items_data(VAL2HASH(slots[1])->items)[2 * j + 1];
        if (KORB_OBJECT_P(v) || KORB_ARRAY_P(v) || KORB_HASH_P(v) ||
            KORB_OBJECT_P(v2) || KORB_ARRAY_P(v2) || KORB_HASH_P(v2)) {
            slots[2] = v; slots[3] = v2;
            RESULT r = korb_send_impl(c, slots + 4, mid_eql, 0, 1, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) { VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING; return r; }
            if (!KORB_TRUTHY(r.value)) { result = KORB_FALSE; break; }
        } else if (!korb_value_eql(v, v2)) {
            result = KORB_FALSE; break;
        }
    }
    VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING;
    return RESULT_OK(result);
}
static RESULT korb_m_hash_value_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* needle (root across value == dispatch) */
    const uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < n; i++) {
        const VALUE v = korb_items_data(VAL2HASH(VALUE_REF_GET(self))->items)[2 * i + 1];
        if (KORB_OBJECT_P(v) || KORB_OBJECT_P(slots[0])) {  /* user == → dispatch (value == needle) */
            slots[1] = v; slots[2] = slots[0];
            RESULT r = korb_send_impl(c, slots + 3, c->vm->mid_eq, 0, 1, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) return RESULT_OK(KORB_TRUE);
        } else if (korb_value_eq(v, slots[0])) {
            return RESULT_OK(KORB_TRUE);
        }
    }
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_hash_fetch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1 || VALUE_SLICE_LEN(a) > 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 1..2)", (unsigned)VALUE_SLICE_LEN(a));
    RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots, self, VALUE_SLICE_GET(a, 0), &ferr);
    if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
    const KorbHash *h = SELF_HASH;                        /* re-read after possible eql? dispatch */
    if (idx >= 0) return RESULT_OK(korb_items_data(h->items)[2 * idx + 1]);
    if (block != NULL) {                                  /* miss: block form yields the key (wins over a default) */
        VALUE k = VALUE_SLICE_GET(a, 0);
        return korb_block_yield(c, slots, block, def_env, &k, 1, cself);
    }
    if (VALUE_SLICE_LEN(a) >= 2) return RESULT_OK(VALUE_SLICE_GET(a, 1));
    char *kb = NULL; size_t ksz = 0; FILE *km = open_memstream(&kb, &ksz);
    if (km) { korb_fprint_inspect(c, km, VALUE_SLICE_GET(a, 0)); fclose(km); }
    char msg[512]; snprintf(msg, sizeof msg, "key not found: %s", kb ? kb : "");
    free(kb);
    return korb_raise_key(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), msg);   /* KeyError w/ #receiver + #key */
}

/* collect keys (sel 0) or values (sel 1) into a new array */
static RESULT korb_hash_collect(CTX *c, VALUE *slots, VALUE_REF self, int sel) {
    uint32_t n = SELF_HASH->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = korb_items_data(SELF_HASH->items)[2 * i + sel];   /* push roots e first */
        CHECK(korb_ary_push_val(c, slots, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_hash_collect(c, slots, self, 0); }
static RESULT korb_m_hash_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_hash_collect(c, slots, self, 1); }

static RESULT korb_m_hash_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots, self, VALUE_SLICE_GET(a, 0), &ferr);
    if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
    KorbHash *h = SELF_HASH;                              /* re-read after possible eql? dispatch */
    if (idx < 0) {
        if (block != NULL) {                              /* miss + block: yield the key, return its result */
            VALUE k = VALUE_SLICE_GET(a, 0);
            return korb_block_yield(c, slots, block, def_env, &k, 1, cself);
        }
        return RESULT_OK(KORB_NIL);
    }
    KorbArrayItems *it = h->items;
    VALUE removed = korb_items_data(it)[2 * idx + 1];               /* held; no GC before return */
    for (uint32_t i = (uint32_t)idx; i + 1 < h->len; i++) {  /* shift to keep order */
        ARO_STORE(c, it, &korb_items_data(it)[2 * i],     korb_items_data(it)[2 * (i + 1)]);
        ARO_STORE(c, it, &korb_items_data(it)[2 * i + 1], korb_items_data(it)[2 * (i + 1) + 1]);
    }
    h->len--;
    ARO_STORE(c, it, &korb_items_data(it)[2 * h->len], KORB_NIL);   /* drop tail refs (nil = no WB) */
    ARO_STORE(c, it, &korb_items_data(it)[2 * h->len + 1], KORB_NIL);
    KORB_HASH_DROP_INDEX(h);                             /* pairs shifted → index stale */
    return RESULT_OK(removed);
}

static RESULT korb_m_hash_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                                 /* no block → Enumerator over [k,v] pairs */
        slots[0] = UNWRAP(korb_ary_new(c, slots, SELF_HASH->len));
        VALUE_REF arr = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = SELF_HASH;
            if (i >= h->len) break;
            slots[1] = korb_items_data(h->items)[2 * i]; slots[2] = korb_items_data(h->items)[2 * i + 1];
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
            CHECK(korb_ary_push_val(c, slots + 4, arr, slots[3]));
        }
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(arr), slots[1]);
    }
    const uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; ) {
        const KorbHash *h = SELF_HASH;
        if (i >= h->len) break;
        slots[0] = korb_items_data(h->items)[2 * i];    /* key — rooted so the post-yield advance check survives GC */
        VALUE v = korb_items_data(h->items)[2 * i + 1];
        RESULT r;
        if (np >= 2) {                       /* |k, v| — fast path, no pair alloc */
            VALUE argv[2] = { slots[0], v };
            r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        } else {                             /* |pair| — yield a [k, v] array */
            slots[1] = v;                                            /* root v (key already at slots[0]) */
            VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
            slots[2] = pair;                                         /* root pair */
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
            VALUE parg = slots[2];
            r = korb_block_yield(c, slots + 3, block, def_env, &parg, 1, captured_self);
        }
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        /* Delete-during-iteration safe: if the block deleted the current key, the
         * next key was compacted into slot i, so reprocess it; otherwise advance. */
        const KorbHash *h2 = SELF_HASH;
        if (i >= h2->len || korb_items_data(h2->items)[2 * i] != slots[0]) continue;
        i++;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Hash#shift — remove & return the first [k,v] pair (insertion order); nil if empty. */
static RESULT korb_m_hash_shift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    if (VAL2HASH(VALUE_REF_GET(self))->len == 0) return RESULT_OK(KORB_NIL);
    slots[0] = korb_items_data(VAL2HASH(VALUE_REF_GET(self))->items)[0];   /* key */
    slots[1] = korb_items_data(VAL2HASH(VALUE_REF_GET(self))->items)[1];   /* val */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));           /* [k,v] (may GC) */
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    KorbHash *h = VAL2HASH(VALUE_REF_GET(self));               /* re-read after alloc */
    KorbArrayItems *it = h->items;
    for (uint32_t i = 1; i < h->len; i++) {                    /* shift pairs down by one */
        ARO_STORE(c, it, &korb_items_data(it)[2 * (i - 1)],     korb_items_data(it)[2 * i]);
        ARO_STORE(c, it, &korb_items_data(it)[2 * (i - 1) + 1], korb_items_data(it)[2 * i + 1]);
    }
    h->len--;
    ARO_STORE(c, it, &korb_items_data(it)[2 * h->len],     KORB_NIL);
    ARO_STORE(c, it, &korb_items_data(it)[2 * h->len + 1], KORB_NIL);
    KORB_HASH_DROP_INDEX(h);                             /* pairs shifted → index stale */
    return RESULT_OK(slots[2]);
}
/* Hash#each_value / each_key — yield each value (resp. key); return self. */
static RESULT korb_hash_each_kv(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *cself, int want_key) {
    if (block == NULL) {                                  /* no block → Enumerator over the keys/values */
        slots[0] = UNWRAP(korb_ary_new(c, slots, SELF_HASH->len));   /* capacity >= len → pushes below never grow/GC */
        VALUE_REF arr = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; i < SELF_HASH->len; i++)     /* SELF_HASH stable: no GC in this loop */
            UNWRAP(korb_ary_push_val(c, slots + 1, arr, korb_items_data(SELF_HASH->items)[2 * i + (want_key ? 0 : 1)]));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), want_key ? "each_key" : "each_value"));
        return korb_enum_new(c, slots + 2, slots[0], slots[1]);
    }
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = SELF_HASH;
        if (i >= h->len) break;
        VALUE e = korb_items_data(h->items)[2 * i + (want_key ? 0 : 1)];
        RESULT r = korb_block_yield(c, slots, block, def_env, &e, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_hash_each_value(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_hash_each_kv(c, slots, self, block, def_env, cself, 0);
}
static RESULT korb_m_hash_each_key(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; return korb_hash_each_kv(c, slots, self, block, def_env, cself, 1);
}

/* Hash#merge(*others) [{ |key, old, new| }] — non-mutating; 0+ hash args merged
 * left-to-right into a copy of self; a block resolves key conflicts. */
static RESULT korb_m_hash_merge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_hash_new(c, slots, SELF_HASH->len));   /* dst = copy of self */
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    if (VAL2HASH(VALUE_REF_GET(self))->head.flags & KORB_FL_CMP_BY_ID)   /* merge retains compare_by_identity */
        ((AroObjectHeader *)(uintptr_t)VALUE_REF_GET(dst))->flags |= KORB_FL_CMP_BY_ID;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = korb_items_data(h->items)[2 * i];                        /* key (root) */
        VALUE val = korb_items_data(VAL2HASH(VALUE_REF_GET(self))->items)[2 * i + 1];
        CHECK(korb_hash_set(c, slots + 2, dst, VALUE_REF_AT(&slots[1]), val));
    }
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_HASH_P(ov))) {                        /* coerce via #to_hash (Hash subclasses are already KORB_HASH_P) */
            const uint32_t to_hash = korb_intern(c->vm, "to_hash", 7);
            if (KORB_OBJECT_P(ov) && korb_responds_to_coerce_p(c, slots, &ov, to_hash)) {
                slots[1] = ov;
                RESULT hr = korb_send_impl(c, slots + 2, to_hash, 0, 0, NULL, NULL, KORB_NIL);
                if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
                ov = hr.value;
            }
            if (UNLIKELY(!KORB_HASH_P(ov)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(VALUE_SLICE_GET(a, k)));
        }
        slots[1] = ov;                                           /* root the arg hash */
        for (uint32_t i = 0; ; i++) {
            const KorbHash *oh = VAL2HASH(slots[1]);
            if (i >= oh->len) break;
            slots[2] = korb_items_data(oh->items)[2 * i];                   /* key */
            slots[3] = korb_items_data(VAL2HASH(slots[1])->items)[2 * i + 1];   /* new value */
            if (block != NULL) {
                int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(dst)), slots[2]);
                if (idx >= 0) {                                  /* conflict → yield(key, old, new) */
                    slots[4] = korb_items_data(VAL2HASH(VALUE_REF_GET(dst))->items)[2 * idx + 1];
                    VALUE argv[3] = { slots[2], slots[4], slots[3] };
                    RESULT r = korb_block_yield(c, slots + 5, block, def_env, argv, 3, cself);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                    slots[3] = r.value;
                }
            }
            CHECK(korb_hash_set(c, slots + 5, dst, VALUE_REF_AT(&slots[2]), slots[3]));
        }
    }
    {   /* retain the receiver's default value / default_proc (CRuby semantics) */
        KorbHash *const dh = VAL2HASH(VALUE_REF_GET(dst));
        const KorbHash *const sh = VAL2HASH(VALUE_REF_GET(self));
        ARO_STORE(c, dh, &dh->default_val,  sh->default_val);
        ARO_STORE(c, dh, &dh->default_proc, sh->default_proc);
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Hash#update / merge! — like merge but mutates self in place, returns self. */
static RESULT korb_m_hash_update(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_HASH_P(ov))) {                        /* coerce via #to_hash (Hash subclasses are already KORB_HASH_P) */
            const uint32_t to_hash = korb_intern(c->vm, "to_hash", 7);
            if (KORB_OBJECT_P(ov) && korb_responds_to_coerce_p(c, slots, &ov, to_hash)) {
                slots[1] = ov;
                RESULT hr = korb_send_impl(c, slots + 2, to_hash, 0, 0, NULL, NULL, KORB_NIL);
                if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
                ov = hr.value;
            }
            if (UNLIKELY(!KORB_HASH_P(ov)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(VALUE_SLICE_GET(a, k)));
        }
        slots[0] = ov;                                           /* root the arg hash */
        for (uint32_t i = 0; ; i++) {
            const KorbHash *oh = VAL2HASH(slots[0]);
            if (i >= oh->len) break;
            slots[1] = korb_items_data(oh->items)[2 * i];                   /* key */
            slots[2] = korb_items_data(VAL2HASH(slots[0])->items)[2 * i + 1];   /* new value */
            if (block != NULL) {
                int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(self)), slots[1]);
                if (idx >= 0) {                                  /* conflict → yield(key, old, new) */
                    slots[3] = korb_items_data(VAL2HASH(VALUE_REF_GET(self))->items)[2 * idx + 1];
                    VALUE argv[3] = { slots[1], slots[3], slots[2] };
                    RESULT r = korb_block_yield(c, slots + 4, block, def_env, argv, 3, cself);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                    slots[2] = r.value;
                }
            }
            CHECK(korb_hash_set(c, slots + 4, self, VALUE_REF_AT(&slots[1]), slots[2]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_hash_make_pair(CTX *c, VALUE *cursor, VALUE *kslot, VALUE *vslot, VALUE *out);
static RESULT korb_m_hash_key(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* needle (root across value == dispatch) */
    const uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *const h = VAL2HASH(VALUE_REF_GET(self));   /* re-read each iter */
        const VALUE v = korb_items_data(h->items)[2 * i + 1], k = korb_items_data(h->items)[2 * i];
        if (KORB_OBJECT_P(v) || KORB_OBJECT_P(slots[0])) {  /* user == → dispatch (value == needle) */
            slots[1] = k; slots[2] = v; slots[3] = slots[0];
            RESULT r = korb_send_impl(c, slots + 4, c->vm->mid_eq, 0, 1, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[1]);
        } else if (korb_value_eq(v, slots[0])) {
            return RESULT_OK(k);
        }
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_hash_rassoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE needle = VALUE_SLICE_GET(a, 0);
    const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < h->len; i++) {
        if (korb_value_eq(korb_items_data(h->items)[2*i+1], needle)) {
            slots[0] = korb_items_data(h->items)[2*i]; slots[1] = korb_items_data(h->items)[2*i+1];
            CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
            return RESULT_OK(slots[2]);
        }
    }
    return RESULT_OK(KORB_NIL);
}
/* true if every pair of `sub` appears in `sup` with an equal value */
static bool korb_hash_is_subset(const KorbHash *sub, const KorbHash *sup) {
    for (uint32_t i = 0; i < sub->len; i++) {
        int32_t idx = korb_hash_find(sup, korb_items_data(sub->items)[2*i]);
        if (idx < 0) return false;
        if (!korb_value_eq(korb_items_data(sub->items)[2*i+1], korb_items_data(sup->items)[2*idx+1])) return false;
    }
    return true;
}
/* op: 0 `<`  1 `<=`  2 `>`  3 `>=` (subset/superset comparison) */
static RESULT korb_hash_cmp_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int op) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_HASH_P(ov))) {                            /* coerce the operand via #to_hash (Hash subclasses are already KORB_HASH_P) */
        const uint32_t to_hash = korb_intern(c->vm, "to_hash", 7);
        if (KORB_OBJECT_P(ov) && korb_responds_to_coerce_p(c, slots, &ov, to_hash)) {
            slots[1] = ov;
            RESULT hr = korb_send_impl(c, slots + 2, to_hash, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
            ov = hr.value;
        }
        if (UNLIKELY(!KORB_HASH_P(ov)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    slots[0] = ov;                                              /* root the (possibly coerced) operand across the compare */
    const KorbHash *me = VAL2HASH(VALUE_REF_GET(self)), *other = VAL2HASH(slots[0]);
    bool res;
    switch (op) {
      case 0: res = me->len <  other->len && korb_hash_is_subset(me, other); break;
      case 1: res = me->len <= other->len && korb_hash_is_subset(me, other); break;
      case 2: res = me->len >  other->len && korb_hash_is_subset(other, me); break;
      default: res = me->len >= other->len && korb_hash_is_subset(other, me); break;
    }
    return RESULT_OK(res ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_hash_lt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_cmp_op(c, slots, self, a, 0); }
static RESULT korb_m_hash_le(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_cmp_op(c, slots, self, a, 1); }
static RESULT korb_m_hash_gt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_cmp_op(c, slots, self, a, 2); }
static RESULT korb_m_hash_ge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_cmp_op(c, slots, self, a, 3); }

