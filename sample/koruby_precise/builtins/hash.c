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
static uint64_t korb_deep_hash_d(CTX *c, korb_value v, uint32_t depth, bool *cycle) {
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
        if (a->head.flags & KORB_FL_JOIN_VISITING) { *cycle = true; return 0xC0FFEEULL; }   /* recursive → constant (no exponential blowup) */
        a->head.flags |= KORB_FL_JOIN_VISITING;
        uint64_t h = 0x345678ULL + a->len;
        for (uint32_t i = 0; i < a->len; i++) h = h * 31u + korb_deep_hash_d(c, korb_ary_ptr(a)[i], depth + 1, cycle);
        a->head.flags &= ~KORB_FL_JOIN_VISITING;         /* pure computation: no GC, `a` stays valid */
        return h;
    }
    if (KORB_HASH_P(v)) {                              /* order-independent (xor) */
        KorbHash *const hh = VAL2HASH(v);
        if (hh->head.flags & KORB_FL_JOIN_VISITING) { *cycle = true; return 0xBEEFULL; }   /* recursive hash */
        hh->head.flags |= KORB_FL_JOIN_VISITING;
        uint64_t h = 0x9e3779b9ULL + hh->len;
        for (uint32_t i = 0; i < hh->len; i++)
            h ^= korb_deep_hash_d(c, korb_items_data(hh->items)[2*i], depth + 1, cycle) * 31u + korb_deep_hash_d(c, korb_items_data(hh->items)[2*i+1], depth + 1, cycle);
        hh->head.flags &= ~KORB_FL_JOIN_VISITING;
        return h;
    }
    if (KORB_SET_P(v)) {                              /* order-independent (xor of element hashes) */
        const KorbArray *a = VAL2ARY(VAL2SET(v)->elems);
        uint64_t h = 0x517cc1b7ULL + a->len;
        for (uint32_t i = 0; i < a->len; i++) h ^= korb_deep_hash_d(c, korb_ary_ptr(a)[i], depth + 1, cycle);
        return h;
    }
    if (KORB_RATIONAL_P(v)) {                          /* reduced num/den → equal Rationals hash alike */
        const KorbRational *rt = VAL2RAT(v);
        return korb_deep_hash_d(c, rt->num, depth + 1, cycle) * 31u + korb_deep_hash_d(c, rt->den, depth + 1, cycle);
    }
    if (KORB_COMPLEX_P(v)) {
        const KorbComplex *cx = VAL2CPX(v);
        return korb_deep_hash_d(c, cx->re, depth + 1, cycle) * 31u + korb_deep_hash_d(c, cx->im, depth + 1, cycle);
    }
    if (KORB_RANGE_P(v)) {                             /* == begin/end + exclude_end */
        const KorbRange *r = VAL2RANGE(v);
        uint64_t h = 0x9e3779b1ULL + (r->exclude_end ? 1u : 0u);
        h = h * 31u + korb_deep_hash_d(c, r->rbegin, depth + 1, cycle);
        h = h * 31u + korb_deep_hash_d(c, r->rend, depth + 1, cycle);
        return h;
    }
    if (KORB_BIGNUM_P(v)) {
        korb_mp_t z; korb_to_mpz(v, z);
        uint64_t h = (korb_mp_sgn(z) < 0) ? 0xABCDEF01ULL : 0x12345678ULL;
        const size_t n = korb_mp_size(z);
        for (size_t i = 0; i < n; i++) h = h * 1099511628211ULL + (uint64_t)korb_mp_getlimbn(z, (korb_mp_size_t)i);
        korb_mp_clear(z);
        return h;
    }
    /* identity — the OBJECT_ID, not the address: a moving GC relocates the
     * object and `obj.hash` must not change under it. */
    { const korb_value oid = korb_oid_find(c->vm, v);
      const korb_value id = (oid != KORB_NIL) ? oid : korb_oid_assign(c->vm, v);   /* assign does not GC */
      return FIXNUM_P(id) ? (uint64_t)FIX2LONG(id) * 0x9e3779b97f4a7c15ULL : (uint64_t)(uintptr_t)v; }
}

/* #hash is salted per process (CVE-2011-4815): an attacker must not be able to
 * predict which keys collide.  Only the public method is salted — the internal
 * Hash index keeps using korb_value_hash, which never leaves the process. */
static uint64_t
korb_hash_salt(void)
{
    static uint64_t salt = 0;
    if (UNLIKELY(salt == 0)) {
        const int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            if (read(fd, &salt, sizeof salt) != (ssize_t)sizeof salt) salt = 0;
            close(fd);
        }
        if (salt == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            salt = ((uint64_t)getpid() << 32) ^ (uint64_t)ts.tv_nsec ^ 0x9e3779b97f4a7c15ULL;
        }
    }
    return salt;
}

static RESULT korb_m_obj_hash(CTX *c, korb_value *slots, korb_ref self, korb_slice a);   /* fwd */
/* True when `v`'s class answers #hash with anything but the generic structural
 * one below — Ruby code of its own, or another builtin (Struct#hash,
 * Regexp#hash, ...).  Only the generic one can be folded into this walk. */
static bool korb_has_user_hash(CTX *c, korb_value v) {
    const korb_value cls = korb_dispatch_class(c, v);
    if (!KORB_CLASS_P(cls)) return false;
    korb_value def = KORB_NIL;   /* once per element of every hashed container: cached lookup */
    const struct korb_method *const m = korb_mcache_find(c->vm, cls, c->vm->mid_hash, &def);
    if (m == NULL) return false;
    return !(m->kind == KORB_METHOD_CFUNC && m->rfn == korb_m_obj_hash);
}

/* korb_deep_hash_d cannot call back into Ruby, so it hashes an element with a
 * user-defined #hash by identity.  CRuby dispatches, so containers of such
 * objects compare by value ([node, …].hash is how rubocop's duplicate-key cop
 * finds two identical AST nodes).  Walk the containers here and hand the leaves
 * to the alloc-free version.  depth 0 is the receiver itself: dispatching there
 * would re-enter the very #hash this implements. */
static RESULT korb_deep_hash_r(CTX *c, korb_value *slots, korb_value v, uint32_t depth, bool *cycle, uint64_t *out) {
    if (depth >= KORB_DEEP_HASH_MAX) { *out = 0xC0FFEEULL; return RESULT_OK(KORB_NIL); }
    if (depth > 0 && korb_has_user_hash(c, v)) {
        slots[0] = v;
        const RESULT r = korb_send(c, slots + 1, korb_intern_lit(c->vm, "hash", 4), 0, 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        *out = FIXNUM_P(r.value) ? (uint64_t)FIX2LONG(r.value) : korb_deep_hash_d(c, r.value, depth, cycle);
        return RESULT_OK(KORB_NIL);
    }
    if (KORB_ARRAY_P(v) || KORB_SET_P(v)) {
        const bool set = !KORB_ARRAY_P(v);
        slots[0] = set ? VAL2SET(v)->elems : v;            /* root: a dispatch below may move it */
        KorbArray *const a0 = VAL2ARY(slots[0]);
        if (a0->head.flags & KORB_FL_JOIN_VISITING) { *cycle = true; *out = 0xC0FFEEULL; return RESULT_OK(KORB_NIL); }
        a0->head.flags |= KORB_FL_JOIN_VISITING;
        uint64_t h = (set ? 0x517cc1b7ULL : 0x345678ULL) + a0->len;
        RESULT err = RESULT_OK(KORB_NIL);
        for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++) {
            uint64_t eh;
            err = korb_deep_hash_r(c, slots + 1, korb_ary_ptr(VAL2ARY(slots[0]))[i], depth + 1, cycle, &eh);
            if (UNLIKELY(err.state != KORB_NORMAL)) break;
            h = set ? (h ^ eh) : (h * 31u + eh);
        }
        VAL2ARY(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING;   /* re-read: a dispatch may have moved it */
        if (UNLIKELY(err.state != KORB_NORMAL)) return err;
        *out = h;
        return RESULT_OK(KORB_NIL);
    }
    if (KORB_HASH_P(v)) {
        slots[0] = v;
        KorbHash *const h0 = VAL2HASH(slots[0]);
        if (h0->head.flags & KORB_FL_JOIN_VISITING) { *cycle = true; *out = 0xBEEFULL; return RESULT_OK(KORB_NIL); }
        h0->head.flags |= KORB_FL_JOIN_VISITING;
        uint64_t h = 0x9e3779b9ULL + h0->len;
        RESULT err = RESULT_OK(KORB_NIL);
        for (uint32_t i = 0; i < VAL2HASH(slots[0])->len; i++) {
            uint64_t kh, vh;
            err = korb_deep_hash_r(c, slots + 1, korb_items_data(VAL2HASH(slots[0])->items)[2*i], depth + 1, cycle, &kh);
            if (UNLIKELY(err.state != KORB_NORMAL)) break;
            err = korb_deep_hash_r(c, slots + 1, korb_items_data(VAL2HASH(slots[0])->items)[2*i+1], depth + 1, cycle, &vh);
            if (UNLIKELY(err.state != KORB_NORMAL)) break;
            h ^= kh * 31u + vh;
        }
        VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING;
        if (UNLIKELY(err.state != KORB_NORMAL)) return err;
        *out = h;
        return RESULT_OK(KORB_NIL);
    }
    if (KORB_RANGE_P(v)) {                                /* same mix as korb_deep_hash_d, ends dispatched */
        slots[0] = v;
        uint64_t bh, eh;
        CHECK(korb_deep_hash_r(c, slots + 1, VAL2RANGE(slots[0])->rbegin, depth + 1, cycle, &bh));
        CHECK(korb_deep_hash_r(c, slots + 1, VAL2RANGE(slots[0])->rend, depth + 1, cycle, &eh));
        *out = ((0x9e3779b1ULL + (VAL2RANGE(slots[0])->exclude_end ? 1u : 0u)) * 31u + bh) * 31u + eh;
        return RESULT_OK(KORB_NIL);
    }
    *out = korb_deep_hash_d(c, v, depth, cycle);
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_obj_hash(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    bool cycle = false;
    uint64_t h;
    const RESULT r = korb_deep_hash_r(c, slots, KORB_REF_GET(self), 0, &cycle, &h);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    /* CRuby drives Array#hash through rb_exec_recursive_outer: a cycle anywhere
     * below collapses the WHOLE result to a function of the outermost length. */
    const korb_value sv = KORB_REF_GET(self);
    if (UNLIKELY(cycle && KORB_ARRAY_P(sv))) h = 0x9E3779B97F4A7C15ULL + VAL2ARY(sv)->len * 31u;
    else if (UNLIKELY(cycle && KORB_HASH_P(sv))) h = 0xBEEFULL + VAL2HASH(sv)->len * 31u;
    h ^= korb_hash_salt();
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 29;
    return RESULT_OK(LONG2FIX((korb_sword_t)(h >> 2)));   /* >>2 keeps it FIXABLE */
}

/* ---- Hash methods -------------------------------------------------------- */

/* Hash#__kwargs_marked? / #__kwargs_mark! — the persistent ruby2_keywords mark
 * that Hash.ruby2_keywords_hash?/ruby2_keywords_hash expose. */
static RESULT korb_m_hash_kwmarked(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c; (void)slots; (void)a;
    return RESULT_OK(korb_r2k_hash_p(KORB_REF_GET(self)) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_hash_kwmark(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c; (void)slots; (void)a;
    korb_r2k_set(KORB_REF_GET(self));
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_hash_size(CTX *c, korb_value *slots, korb_ref self, korb_slice a)  { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_HASH->len)); }
static RESULT korb_m_hash_empty(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_HASH->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_hash_self(CTX *c, korb_value *slots, korb_ref self, korb_slice a)  { (void)c;(void)slots;(void)a; return RESULT_OK(KORB_REF_GET(self)); }
/* default_proc.(hash, key) without a #call send: CRuby calls the proc itself (a redefined Proc#call is
 * not seen), and the generic send was 53k an iteration in railsbench.  slots needs >= 3 free cells. */
static RESULT
korb_hash_call_default_proc(CTX *c, korb_value *slots, const korb_value proc, const korb_value hash, const korb_value key)
{
    slots[0] = proc; slots[1] = hash; slots[2] = key;
    if (LIKELY(KORB_PROC_P(proc)))
        return korb_m_proc_call(c, slots + 3, KORB_REF_AT(&slots[0]), KORB_SLICE_MAKE(&slots[1], 2), NULL, NULL, NULL);
    return korb_send_impl(c, slots + 3, korb_intern_lit(c->vm, "call", 4), 0, 2, NULL, NULL, NULL);
}
static RESULT korb_m_hash_default(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (KORB_SLICE_LEN(a) >= 1 && SELF_HASH->default_proc != KORB_NIL)   /* default(key) + proc → proc.(self, key) */
        return korb_hash_call_default_proc(c, slots, SELF_HASH->default_proc, KORB_REF_GET(self), KORB_SLICE_GET(a, 0));
    return RESULT_OK(SELF_HASH->default_val);
}

static RESULT korb_m_hash_cmp_by_id(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    korb_value v = KORB_REF_GET(self);
    if (!(((AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_CMP_BY_ID))
        KORB_HASH_DROP_INDEX(VAL2HASH(v));                /* built by value: rebuilt by address on the next insert */
    ((AroObjectHeader *)(uintptr_t)v)->flags |= KORB_FL_CMP_BY_ID;
    return RESULT_OK(v);
}
static RESULT korb_m_hash_cmp_by_id_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a;
    return RESULT_OK((((AroObjectHeader *)(uintptr_t)KORB_REF_GET(self))->flags & KORB_FL_CMP_BY_ID) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_hash_aref(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    RESULT ferr; const int32_t idx = korb_hash_find_ctx(c, slots, self, KORB_SLICE_GET(a, 0), &ferr);
    if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
    const KorbHash *h = SELF_HASH;                        /* re-read after possible eql? dispatch */
    if (idx >= 0) return RESULT_OK(korb_items_data(h->items)[2 * idx + 1]);
    if (h->default_proc != KORB_NIL)                      /* Hash.new { |h,k| } → default_proc.(self, key) */
        return korb_hash_call_default_proc(c, slots, h->default_proc, KORB_REF_GET(self), KORB_SLICE_GET(a, 0));
    {   /* a miss goes through #default(key), which a subclass may override */
        const uint32_t dmid = korb_intern_lit(c->vm, "default", 7);
        const korb_value cls = korb_dispatch_class(c, KORB_REF_GET(self));
        const struct korb_method *const dm = KORB_CLASS_P(cls) ? korb_find_method_c(c->vm, cls, dmid, NULL) : NULL;
        if (UNLIKELY(dm != NULL && dm->kind == KORB_METHOD_ISEQ)) {   /* only a Ruby override is observable */
            slots[0] = KORB_REF_GET(self);
            slots[1] = KORB_SLICE_GET(a, 0);
            return korb_send(c, slots + 2, dmid, 0, 1);
        }
    }
    return RESULT_OK(SELF_HASH->default_val);
}

static RESULT korb_m_hash_aset(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    /* the frozen private copy of a mutable String key is korb_hash_set's job */
    CHECK(korb_hash_set(c, slots, self, KORB_SLICE_REF(a, 0), KORB_SLICE_GET(a, 1)));
    return RESULT_OK(KORB_SLICE_GET(a, 1));        /* []= yields the value (rooted re-read) */
}

static RESULT korb_m_hash_assoc(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    /* CRuby Hash#assoc is a linear scan comparing keys with #== (not the eql?/hash
     * lookup), so e.g. assoc(1) matches a 1.0 key. */
    slots[0] = KORB_SLICE_GET(a, 0);                    /* needle (rooted across == dispatch) */
    const uint32_t n = VAL2HASH(KORB_REF_GET(self))->len;
    const bool eqr = KORB_EQ_REDEF(c);   /* read once: the loop calls out of line */
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        const korb_value k = korb_items_data(h->items)[2 * i];
        bool match;
        if (korb_eq_send_p_(eqr, k, slots[0])) {  /* user == → dispatch (key == needle) */
            slots[1] = k; slots[2] = slots[0];
            RESULT r = korb_send_impl(c, slots + 3, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            match = KORB_TRUTHY(r.value);
        } else {
            match = korb_value_eq(k, slots[0]);
        }
        if (match) {
            const KorbHash *h2 = VAL2HASH(KORB_REF_GET(self));
            slots[1] = korb_items_data(h2->items)[2 * i]; slots[2] = korb_items_data(h2->items)[2 * i + 1];
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
            korb_ref arr = KORB_REF_AT(slots + 3);
            CHECK(korb_ary_push_val(c, slots + 4, arr, slots[1]));
            CHECK(korb_ary_push_val(c, slots + 4, arr, slots[2]));
            return RESULT_OK(KORB_REF_GET(arr));
        }
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_hash_key_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    RESULT ferr; const int32_t idx = korb_hash_find_ctx(c, slots, self, KORB_SLICE_GET(a, 0), &ferr);
    if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
    return RESULT_OK(idx >= 0 ? KORB_TRUE : KORB_FALSE);
}

/* Hash#== — same size, same keys (eql?/hash), values compared with #== (so a
 * user object value's == is honoured, which korb_value_eq cannot do without a CTX). */
static RESULT korb_m_hash_eq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const korb_value other = KORB_SLICE_GET(a, 0);
    if (KORB_REF_GET(self) == other) return RESULT_OK(KORB_TRUE);
    if (!KORB_HASH_P(other)) {                            /* Hash-like (responds to #to_hash) → other == self */
        korb_value o = other;
        if (korb_ary_respond_to_pub(c, slots, &o, korb_intern_lit(c->vm, "to_hash", 7))) {   /* any receiver (rb_respond_to) */
            slots[0] = o; slots[1] = KORB_REF_GET(self);
            const RESULT r = korb_send_impl(c, slots + 2, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            return RESULT_OK(KORB_TRUTHY(r.value) ? KORB_TRUE : KORB_FALSE);
        }
        return RESULT_OK(KORB_FALSE);
    }
    if (VAL2HASH(KORB_REF_GET(self))->len != VAL2HASH(other)->len) return RESULT_OK(KORB_FALSE);
    if (VAL2HASH(KORB_REF_GET(self))->len != 0 &&        /* non-empty: differ only by compare_by_identity → not equal (two empty hashes stay equal regardless) */
        ((VAL2HASH(KORB_REF_GET(self))->head.flags ^ VAL2HASH(other)->head.flags) & KORB_FL_CMP_BY_ID))
        return RESULT_OK(KORB_FALSE);
    if (VAL2HASH(KORB_REF_GET(self))->head.flags & KORB_FL_JOIN_VISITING) return RESULT_OK(KORB_TRUE);   /* recursive */
    slots[0] = KORB_REF_GET(self); slots[1] = other;     /* root both across value == dispatch */
    const uint32_t n = VAL2HASH(slots[0])->len;
    VAL2HASH(slots[0])->head.flags |= KORB_FL_JOIN_VISITING;
    korb_value result = KORB_TRUE;
    const bool eqr = KORB_EQ_REDEF(c);   /* read once: the loop calls out of line */
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *x = VAL2HASH(slots[0]);
        const korb_value k = korb_items_data(x->items)[2 * i], v = korb_items_data(x->items)[2 * i + 1];
        RESULT ferr; const int32_t j = korb_hash_find_ctx(c, slots + 2, KORB_REF_AT(&slots[1]), k, &ferr);   /* key match = #hash + #eql? (user keys dispatch) */
        if (UNLIKELY(ferr.state != KORB_NORMAL)) { VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING; return ferr; }
        if (j < 0) { result = KORB_FALSE; break; }
        const korb_value v2 = korb_items_data(VAL2HASH(slots[1])->items)[2 * j + 1];
        if (korb_eq_send_p_(eqr, v, v2)) {   /* value == via dispatch (recurses for nested) */
            slots[2] = v; slots[3] = v2;
            RESULT r = korb_send_impl(c, slots + 4, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) { VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING; return r; }
            if (!KORB_TRUTHY(r.value)) { result = KORB_FALSE; break; }
        } else if (v != v2 && !korb_value_eq(v, v2)) {   /* rb_equal: the same object is equal (a shared NaN) */
            result = KORB_FALSE; break;
        }
    }
    VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING;
    return RESULT_OK(result);
}
/* Hash#eql? — like Hash#== but values compared with #eql? (type-strict). */
static RESULT korb_m_hash_eql(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const korb_value other = KORB_SLICE_GET(a, 0);
    if (KORB_REF_GET(self) == other) return RESULT_OK(KORB_TRUE);
    if (!KORB_HASH_P(other)) return RESULT_OK(KORB_FALSE);
    if (VAL2HASH(KORB_REF_GET(self))->len != VAL2HASH(other)->len) return RESULT_OK(KORB_FALSE);
    if (VAL2HASH(KORB_REF_GET(self))->head.flags & KORB_FL_JOIN_VISITING) return RESULT_OK(KORB_TRUE);   /* recursive → assume equal */
    slots[0] = KORB_REF_GET(self); slots[1] = other;
    const uint32_t mid_eql = korb_intern_lit(c->vm, "eql?", 4);
    const uint32_t n = VAL2HASH(slots[0])->len;
    VAL2HASH(slots[0])->head.flags |= KORB_FL_JOIN_VISITING;
    korb_value result = KORB_TRUE;
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *x = VAL2HASH(slots[0]);
        const korb_value k = korb_items_data(x->items)[2 * i], v = korb_items_data(x->items)[2 * i + 1];
        RESULT ferr; const int32_t j = korb_hash_find_ctx(c, slots + 2, KORB_REF_AT(&slots[1]), k, &ferr);   /* key match = #hash + #eql? (user keys dispatch) */
        if (UNLIKELY(ferr.state != KORB_NORMAL)) { VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING; return ferr; }
        if (j < 0) { result = KORB_FALSE; break; }
        const korb_value v2 = korb_items_data(VAL2HASH(slots[1])->items)[2 * j + 1];
        if (v == v2) continue;                           /* rb_eql: the same object is eql? (a shared NaN) */
        if (korb_eq_needs_send(v) || korb_eq_needs_send(v2)) {
            slots[2] = v; slots[3] = v2;
            RESULT r = korb_send_impl(c, slots + 4, mid_eql, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) { VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING; return r; }
            if (!KORB_TRUTHY(r.value)) { result = KORB_FALSE; break; }
        } else if (!korb_value_eql(v, v2)) {
            result = KORB_FALSE; break;
        }
    }
    VAL2HASH(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING;
    return RESULT_OK(result);
}
static RESULT korb_m_hash_value_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    slots[0] = KORB_SLICE_GET(a, 0);                    /* needle (root across value == dispatch) */
    const uint32_t n = VAL2HASH(KORB_REF_GET(self))->len;
    const bool eqr = KORB_EQ_REDEF(c);   /* read once: the loop calls out of line */
    for (uint32_t i = 0; i < n; i++) {
        const korb_value v = korb_items_data(VAL2HASH(KORB_REF_GET(self))->items)[2 * i + 1];
        if (korb_eq_send_p_(eqr, v, slots[0])) {  /* user == → dispatch (value == needle) */
            slots[1] = v; slots[2] = slots[0];
            RESULT r = korb_send_impl(c, slots + 3, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) return RESULT_OK(KORB_TRUE);
        } else if (korb_value_eq(v, slots[0])) {
            return RESULT_OK(KORB_TRUE);
        }
    }
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_hash_fetch(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (UNLIKELY(KORB_SLICE_LEN(a) < 1 || KORB_SLICE_LEN(a) > 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 1..2)", (unsigned)KORB_SLICE_LEN(a));
    if (UNLIKELY(KORB_SLICE_LEN(a) == 2 && block != NULL)) korb_warn(c, slots, "block supersedes default value argument");
    RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots, self, KORB_SLICE_GET(a, 0), &ferr);
    if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
    const KorbHash *h = SELF_HASH;                        /* re-read after possible eql? dispatch */
    if (idx >= 0) return RESULT_OK(korb_items_data(h->items)[2 * idx + 1]);
    if (block != NULL) {                                  /* miss: block form yields the key (wins over a default) */
        korb_value k = KORB_SLICE_GET(a, 0);
        return korb_block_yield(c, slots, block, def_env, &k, 1, cself);
    }
    if (KORB_SLICE_LEN(a) >= 2) return RESULT_OK(KORB_SLICE_GET(a, 1));
    char *kb = NULL; size_t ksz = 0; FILE *km = open_memstream(&kb, &ksz);
    if (km) { korb_fprint_inspect(c, km, KORB_SLICE_GET(a, 0)); fclose(km); }
    char msg[512]; snprintf(msg, sizeof msg, "key not found: %s", kb ? kb : "");
    free(kb);
    return korb_raise_key(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), msg);   /* KeyError w/ #receiver + #key */
}

/* collect keys (sel 0) or values (sel 1) into a new array */
static RESULT korb_hash_collect(CTX *c, korb_value *slots, korb_ref self, int sel) {
    uint32_t n = SELF_HASH->len;
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        korb_value e = korb_items_data(SELF_HASH->items)[2 * i + sel];   /* push roots e first */
        CHECK(korb_ary_push_val(c, slots, dst, e));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_hash_keys(CTX *c, korb_value *slots, korb_ref self, korb_slice a)   { (void)a; return korb_hash_collect(c, slots, self, 0); }
static RESULT korb_m_hash_values(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)a; return korb_hash_collect(c, slots, self, 1); }

static RESULT korb_m_hash_delete(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots, self, KORB_SLICE_GET(a, 0), &ferr);
    if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
    KorbHash *h = SELF_HASH;                              /* re-read after possible eql? dispatch */
    if (idx < 0) {
        if (block != NULL) {                              /* miss + block: yield the key, return its result */
            korb_value k = KORB_SLICE_GET(a, 0);
            return korb_block_yield(c, slots, block, def_env, &k, 1, cself);
        }
        return RESULT_OK(KORB_NIL);
    }
    const korb_value removed = korb_items_data(h->items)[2 * idx + 1];   /* held; no GC before return */
    korb_hash_remove_at(c, h, (uint32_t)idx);
    return RESULT_OK(removed);
}

static RESULT korb_m_hash_each_named(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self, const char *ename);
static RESULT korb_m_hash_each(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    return korb_m_hash_each_named(c, slots, self, a, block, def_env, captured_self, "each");
}
static RESULT korb_m_hash_each_named(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self, const char *ename) {
    (void)a;
    if (block == NULL) {                                 /* no block → Enumerator over [k,v] pairs */
        slots[0] = UNWRAP(korb_ary_new(c, slots, SELF_HASH->len));
        korb_ref arr = KORB_REF_AT(&slots[0]);
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = SELF_HASH;
            if (i >= h->len) break;
            slots[1] = korb_items_data(h->items)[2 * i]; slots[2] = korb_items_data(h->items)[2 * i + 1];
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
            CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[2]));
            CHECK(korb_ary_push_val(c, slots + 4, arr, slots[3]));
        }
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, KORB_REF_GET(self), ename));
        const RESULT er = korb_enum_new(c, slots + 2, KORB_REF_GET(arr), slots[1]);
        return korb_enum_each_like(c, er, KORB_REF_GET(self));
    }
    /* a forwarded lambda (&lam / &obj.method(:m)) gets ONE [k, v] argument, so
     * its arity check sees 1 (CRuby yields the pair; only a proc block unpacks) */
    const bool lambda_blk = def_env == KORB_BLK_FWD && KORB_PROC_P(*captured_self) && VAL2PROC(*captured_self)->is_lambda;
    const uint32_t np = lambda_blk ? 1 : korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; ) {
        const KorbHash *h = SELF_HASH;
        if (i >= h->len) break;
        slots[0] = korb_items_data(h->items)[2 * i];    /* key — rooted so the post-yield advance check survives GC */
        korb_value v = korb_items_data(h->items)[2 * i + 1];
        RESULT r;
        if (np >= 2) {                       /* |k, v| — fast path, no pair alloc */
            korb_value argv[2] = { slots[0], v };
            r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        } else {                             /* |pair| — yield a [k, v] array */
            slots[1] = v;                                            /* root v (key already at slots[0]) */
            korb_value pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
            slots[2] = pair;                                         /* root pair */
            CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
            CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
            korb_value parg = slots[2];
            r = korb_block_yield(c, slots + 3, block, def_env, &parg, 1, captured_self);
        }
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        /* Delete-during-iteration safe: if the block deleted the current key, the
         * next key was compacted into slot i, so reprocess it; otherwise advance. */
        const KorbHash *h2 = SELF_HASH;
        if (i >= h2->len || korb_items_data(h2->items)[2 * i] != slots[0]) continue;
        i++;
    }
    return RESULT_OK(KORB_REF_GET(self));
}

/* Hash#shift — remove & return the first [k,v] pair (insertion order); nil if empty. */
static RESULT korb_m_hash_shift(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    if (VAL2HASH(KORB_REF_GET(self))->len == 0) return RESULT_OK(KORB_NIL);
    slots[0] = korb_items_data(VAL2HASH(KORB_REF_GET(self))->items)[0];   /* key */
    slots[1] = korb_items_data(VAL2HASH(KORB_REF_GET(self))->items)[1];   /* val */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));           /* [k,v] (may GC) */
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
    korb_hash_remove_at(c, VAL2HASH(KORB_REF_GET(self)), 0);   /* re-read after alloc; keeps the key hints */
    return RESULT_OK(slots[2]);
}
/* Hash#each_value / each_key — yield each value (resp. key); return self. */
static RESULT korb_hash_each_kv(CTX *c, korb_value *slots, korb_ref self, NODE *block, korb_value *def_env, korb_value *cself, int want_key) {
    if (block == NULL) {                                  /* no block → Enumerator over the keys/values */
        slots[0] = UNWRAP(korb_ary_new(c, slots, SELF_HASH->len));   /* capacity >= len → pushes below never grow/GC */
        korb_ref arr = KORB_REF_AT(&slots[0]);
        for (uint32_t i = 0; i < SELF_HASH->len; i++)     /* SELF_HASH stable: no GC in this loop */
            UNWRAP(korb_ary_push_val(c, slots + 1, arr, korb_items_data(SELF_HASH->items)[2 * i + (want_key ? 0 : 1)]));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, KORB_REF_GET(self), want_key ? "each_key" : "each_value"));
        const RESULT er = korb_enum_new(c, slots + 2, slots[0], slots[1]);
        return korb_enum_each_like(c, er, KORB_REF_GET(self));
    }
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = SELF_HASH;
        if (i >= h->len) break;
        korb_value e = korb_items_data(h->items)[2 * i + (want_key ? 0 : 1)];
        RESULT r = korb_block_yield(c, slots, block, def_env, &e, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_hash_each_value(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; return korb_hash_each_kv(c, slots, self, block, def_env, cself, 0);
}
static RESULT korb_m_hash_each_key(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; return korb_hash_each_kv(c, slots, self, block, def_env, cself, 1);
}

/* Hash#merge(*others) [{ |key, old, new| }] — non-mutating; 0+ hash args merged
 * left-to-right into a copy of self; a block resolves key conflicts. */
static RESULT korb_m_hash_merge(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    slots[0] = UNWRAP(korb_hash_new(c, slots, SELF_HASH->len));   /* dst = copy of self */
    korb_ref dst = KORB_REF_AT(&slots[0]);
    if (VAL2HASH(KORB_REF_GET(self))->head.flags & KORB_FL_CMP_BY_ID)   /* merge retains compare_by_identity */
        ((AroObjectHeader *)(uintptr_t)KORB_REF_GET(dst))->flags |= KORB_FL_CMP_BY_ID;
    if (((const AroObjectHeader *)(uintptr_t)KORB_REF_GET(self))->flags & KORB_FL_HAS_KLASS) {
        const korb_value ov = korb_copy_klass(c, korb_klass_override_get(c->vm, KORB_REF_GET(self)),   /* merge returns a receiver-subclass instance (CRuby) */
                                         KORB_REF_GET(self));
        if (ov != KORB_NIL) korb_klass_override_set(c, KORB_REF_GET(dst), ov);
    }
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = korb_items_data(h->items)[2 * i];                        /* key (root) */
        korb_value val = korb_items_data(VAL2HASH(KORB_REF_GET(self))->items)[2 * i + 1];
        CHECK(korb_hash_set(c, slots + 2, dst, KORB_REF_AT(&slots[1]), val));
    }
    for (uint32_t k = 0; k < KORB_SLICE_LEN(a); k++) {
        korb_value ov = KORB_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_HASH_P(ov))) {                        /* coerce via #to_hash (a wrong-typed one raises) */
            const RESULT cr = korb_coerce_to_hash(c, slots + 1, &ov);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (UNLIKELY(cr.value != KORB_TRUE))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_coerce_name(c, ov));
        }
        slots[1] = ov;                                           /* root the arg hash */
        for (uint32_t i = 0; ; i++) {
            const KorbHash *oh = VAL2HASH(slots[1]);
            if (i >= oh->len) break;
            slots[2] = korb_items_data(oh->items)[2 * i];                   /* key */
            slots[3] = korb_items_data(VAL2HASH(slots[1])->items)[2 * i + 1];   /* new value */
            if (block != NULL) {
                int32_t idx = korb_hash_find(VAL2HASH(KORB_REF_GET(dst)), slots[2]);
                if (idx >= 0) {                                  /* conflict → yield(key, old, new) */
                    slots[4] = korb_items_data(VAL2HASH(KORB_REF_GET(dst))->items)[2 * idx + 1];
                    korb_value argv[3] = { slots[2], slots[4], slots[3] };
                    RESULT r = korb_block_yield(c, slots + 5, block, def_env, argv, 3, cself);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                    slots[3] = r.value;
                }
            }
            CHECK(korb_hash_set(c, slots + 5, dst, KORB_REF_AT(&slots[2]), slots[3]));
        }
    }
    {   /* retain the receiver's default value / default_proc (CRuby semantics) */
        KorbHash *const dh = VAL2HASH(KORB_REF_GET(dst));
        const KorbHash *const sh = VAL2HASH(KORB_REF_GET(self));
        ARO_STORE(c, dh, &dh->default_val,  sh->default_val);
        ARO_STORE(c, dh, &dh->default_proc, sh->default_proc);
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* Hash#update / merge! — like merge but mutates self in place, returns self. */
static RESULT korb_m_hash_update(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    for (uint32_t k = 0; k < KORB_SLICE_LEN(a); k++) {
        korb_value ov = KORB_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_HASH_P(ov))) {                        /* coerce via #to_hash (a wrong-typed one raises) */
            const RESULT cr = korb_coerce_to_hash(c, slots + 1, &ov);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (UNLIKELY(cr.value != KORB_TRUE))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_coerce_name(c, ov));
        }
        slots[0] = ov;                                           /* root the arg hash */
        for (uint32_t i = 0; ; i++) {
            const KorbHash *oh = VAL2HASH(slots[0]);
            if (i >= oh->len) break;
            slots[1] = korb_items_data(oh->items)[2 * i];                   /* key */
            slots[2] = korb_items_data(VAL2HASH(slots[0])->items)[2 * i + 1];   /* new value */
            if (block != NULL) {
                int32_t idx = korb_hash_find(VAL2HASH(KORB_REF_GET(self)), slots[1]);
                if (idx >= 0) {                                  /* conflict → yield(key, old, new) */
                    slots[3] = korb_items_data(VAL2HASH(KORB_REF_GET(self))->items)[2 * idx + 1];
                    korb_value argv[3] = { slots[1], slots[3], slots[2] };
                    RESULT r = korb_block_yield(c, slots + 4, block, def_env, argv, 3, cself);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                    slots[2] = r.value;
                }
            }
            CHECK(korb_hash_set(c, slots + 4, self, KORB_REF_AT(&slots[1]), slots[2]));
        }
    }
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_hash_make_pair(CTX *c, korb_value *cursor, korb_value *kslot, korb_value *vslot, korb_value *out);
static RESULT korb_m_hash_key(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    slots[0] = KORB_SLICE_GET(a, 0);                    /* needle (root across value == dispatch) */
    const uint32_t n = VAL2HASH(KORB_REF_GET(self))->len;
    const bool eqr = KORB_EQ_REDEF(c);   /* read once: the loop calls out of line */
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *const h = VAL2HASH(KORB_REF_GET(self));   /* re-read each iter */
        const korb_value v = korb_items_data(h->items)[2 * i + 1], k = korb_items_data(h->items)[2 * i];
        if (korb_eq_send_p_(eqr, v, slots[0])) {  /* user == → dispatch (value == needle) */
            slots[1] = k; slots[2] = v; slots[3] = slots[0];
            RESULT r = korb_send_impl(c, slots + 4, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[1]);
        } else if (korb_value_eq(v, slots[0])) {
            return RESULT_OK(k);
        }
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_hash_rassoc(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value needle = KORB_SLICE_GET(a, 0);
    const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
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
static RESULT korb_hash_cmp_op(CTX *c, korb_value *slots, korb_ref self, korb_slice a, int op) {
    korb_value ov = KORB_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_HASH_P(ov))) {                            /* coerce the operand via #to_hash (Hash subclasses are already KORB_HASH_P) */
        const uint32_t to_hash = korb_intern_lit(c->vm, "to_hash", 7);
        if (KORB_OBJECT_P(ov) && korb_responds_to_coerce_p(c, slots, &ov, to_hash)) {
            slots[1] = ov;
            RESULT hr = korb_send_impl(c, slots + 2, to_hash, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
            ov = hr.value;
        }
        if (UNLIKELY(!KORB_HASH_P(ov)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_coerce_name(c, KORB_SLICE_GET(a, 0)));
    }
    slots[0] = ov;                                              /* root the (possibly coerced) operand across the compare */
    const KorbHash *me = VAL2HASH(KORB_REF_GET(self)), *other = VAL2HASH(slots[0]);
    bool res;
    switch (op) {
      case 0: res = me->len <  other->len && korb_hash_is_subset(me, other); break;
      case 1: res = me->len <= other->len && korb_hash_is_subset(me, other); break;
      case 2: res = me->len >  other->len && korb_hash_is_subset(other, me); break;
      default: res = me->len >= other->len && korb_hash_is_subset(other, me); break;
    }
    return RESULT_OK(res ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_hash_lt(CTX *c, korb_value *slots, korb_ref self, korb_slice a)  { return korb_hash_cmp_op(c, slots, self, a, 0); }
static RESULT korb_m_hash_le(CTX *c, korb_value *slots, korb_ref self, korb_slice a)  { return korb_hash_cmp_op(c, slots, self, a, 1); }
static RESULT korb_m_hash_gt(CTX *c, korb_value *slots, korb_ref self, korb_slice a)  { return korb_hash_cmp_op(c, slots, self, a, 2); }
static RESULT korb_m_hash_ge(CTX *c, korb_value *slots, korb_ref self, korb_slice a)  { return korb_hash_cmp_op(c, slots, self, a, 3); }

