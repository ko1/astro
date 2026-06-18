/* koruby_precise — hash.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Hash methods -------------------------------------------------------- */

static RESULT korb_m_hash_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_HASH->len)); }
static RESULT korb_m_hash_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_HASH->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_hash_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_hash_default(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_HASH->default_val); }

static RESULT korb_m_hash_cmp_by_id(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    VALUE v = VALUE_REF_GET(self);
    ((AroObjectHeader *)(uintptr_t)v)->flags |= KORB_FL_CMP_BY_ID;
    return RESULT_OK(v);
}
static RESULT korb_m_hash_cmp_by_id_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    return RESULT_OK((((AroObjectHeader *)(uintptr_t)VALUE_REF_GET(self))->flags & KORB_FL_CMP_BY_ID) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_hash_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbHash *h = SELF_HASH;
    int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, 0));
    return RESULT_OK(idx < 0 ? h->default_val : h->items->data[2 * idx + 1]);
}

static RESULT korb_m_hash_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    CHECK(korb_hash_set(c, slots, self, VALUE_SLICE_REF(a, 0), VALUE_SLICE_GET(a, 1)));
    return RESULT_OK(VALUE_SLICE_GET(a, 1));        /* []= yields the value (rooted re-read) */
}

static RESULT korb_m_hash_assoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int32_t idx = korb_hash_find(SELF_HASH, VALUE_SLICE_GET(a, 0));
    if (idx < 0) return RESULT_OK(KORB_NIL);
    slots[0] = SELF_HASH->items->data[2 * idx];        /* root k,v across the array alloc */
    slots[1] = SELF_HASH->items->data[2 * idx + 1];
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF arr = VALUE_REF_AT(slots + 2);
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[1]));
    return RESULT_OK(VALUE_REF_GET(arr));
}

static RESULT korb_m_hash_key_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    return RESULT_OK(korb_hash_find(SELF_HASH, VALUE_SLICE_GET(a, 0)) >= 0 ? KORB_TRUE : KORB_FALSE);
}

static RESULT korb_m_hash_value_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbHash *h = SELF_HASH;
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < h->len; i++)
        if (korb_value_eq(h->items->data[2 * i + 1], needle)) return RESULT_OK(KORB_TRUE);
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_hash_fetch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    const KorbHash *h = SELF_HASH;
    int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, 0));
    if (idx >= 0) return RESULT_OK(h->items->data[2 * idx + 1]);
    if (block != NULL) {                                  /* miss: block form yields the key (wins over a default) */
        VALUE k = VALUE_SLICE_GET(a, 0);
        return korb_block_yield(c, slots, block, def_env, &k, 1, cself);
    }
    if (VALUE_SLICE_LEN(a) >= 2) return RESULT_OK(VALUE_SLICE_GET(a, 1));
    char *kb = NULL; size_t ksz = 0; FILE *km = open_memstream(&kb, &ksz);
    if (km) { korb_fprint_inspect(c, km, VALUE_SLICE_GET(a, 0)); fclose(km); }
    RESULT r = korb_raise(c, slots, KORB_E_KEY, 0, "key not found: %s", kb ? kb : "");
    free(kb);
    return r;
}

/* collect keys (sel 0) or values (sel 1) into a new array */
static RESULT korb_hash_collect(CTX *c, VALUE *slots, VALUE_REF self, int sel) {
    uint32_t n = SELF_HASH->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = SELF_HASH->items->data[2 * i + sel];   /* push roots e first */
        CHECK(korb_ary_push_val(c, slots, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_hash_collect(c, slots, self, 0); }
static RESULT korb_m_hash_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_hash_collect(c, slots, self, 1); }

static RESULT korb_m_hash_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    KorbHash *h = SELF_HASH;
    int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, 0));
    if (idx < 0) return RESULT_OK(KORB_NIL);
    KorbArrayItems *it = h->items;
    VALUE removed = it->data[2 * idx + 1];               /* held; no GC before return */
    for (uint32_t i = (uint32_t)idx; i + 1 < h->len; i++) {  /* shift to keep order */
        ARO_STORE(c, it, &it->data[2 * i],     it->data[2 * (i + 1)]);
        ARO_STORE(c, it, &it->data[2 * i + 1], it->data[2 * (i + 1) + 1]);
    }
    h->len--;
    ARO_STORE(c, it, &it->data[2 * h->len], KORB_NIL);   /* drop tail refs (nil = no WB) */
    ARO_STORE(c, it, &it->data[2 * h->len + 1], KORB_NIL);
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
            slots[1] = h->items->data[2 * i]; slots[2] = h->items->data[2 * i + 1];
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
            CHECK(korb_ary_push_val(c, slots + 4, arr, slots[3]));
        }
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(arr), slots[1]);
    }
    const uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = SELF_HASH;
        if (i >= h->len) break;
        VALUE k = h->items->data[2 * i];
        VALUE v = h->items->data[2 * i + 1];
        RESULT r;
        if (np >= 2) {                       /* |k, v| — fast path, no pair alloc */
            VALUE argv[2] = { k, v };
            r = korb_block_yield(c, slots, block, def_env, argv, 2, captured_self);
        } else {                             /* |pair| — yield a [k, v] array */
            slots[0] = k; slots[1] = v;                              /* root k,v in scratch */
            VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));      /* slots[0,1] rooted */
            slots[2] = pair;                                         /* root pair */
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
            VALUE parg = slots[2];
            r = korb_block_yield(c, slots + 3, block, def_env, &parg, 1, captured_self);
        }
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Hash#shift — remove & return the first [k,v] pair (insertion order); nil if empty. */
static RESULT korb_m_hash_shift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (VAL2HASH(VALUE_REF_GET(self))->len == 0) return RESULT_OK(KORB_NIL);
    slots[0] = VAL2HASH(VALUE_REF_GET(self))->items->data[0];   /* key */
    slots[1] = VAL2HASH(VALUE_REF_GET(self))->items->data[1];   /* val */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));           /* [k,v] (may GC) */
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    KorbHash *h = VAL2HASH(VALUE_REF_GET(self));               /* re-read after alloc */
    KorbArrayItems *it = h->items;
    for (uint32_t i = 1; i < h->len; i++) {                    /* shift pairs down by one */
        ARO_STORE(c, it, &it->data[2 * (i - 1)],     it->data[2 * i]);
        ARO_STORE(c, it, &it->data[2 * (i - 1) + 1], it->data[2 * i + 1]);
    }
    h->len--;
    ARO_STORE(c, it, &it->data[2 * h->len],     KORB_NIL);
    ARO_STORE(c, it, &it->data[2 * h->len + 1], KORB_NIL);
    KORB_HASH_DROP_INDEX(h);                             /* pairs shifted → index stale */
    return RESULT_OK(slots[2]);
}
/* Hash#each_value / each_key — yield each value (resp. key); return self. */
static RESULT korb_hash_each_kv(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *cself, int want_key) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#each_value/each_key without a block (Enumerator) is not supported");
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = SELF_HASH;
        if (i >= h->len) break;
        VALUE e = h->items->data[2 * i + (want_key ? 0 : 1)];
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
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = h->items->data[2 * i];                        /* key (root) */
        VALUE val = VAL2HASH(VALUE_REF_GET(self))->items->data[2 * i + 1];
        CHECK(korb_hash_set(c, slots + 2, dst, VALUE_REF_AT(&slots[1]), val));
    }
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_HASH_P(ov)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(ov));
        slots[1] = ov;                                           /* root the arg hash */
        for (uint32_t i = 0; ; i++) {
            const KorbHash *oh = VAL2HASH(slots[1]);
            if (i >= oh->len) break;
            slots[2] = oh->items->data[2 * i];                   /* key */
            slots[3] = VAL2HASH(slots[1])->items->data[2 * i + 1];   /* new value */
            if (block != NULL) {
                int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(dst)), slots[2]);
                if (idx >= 0) {                                  /* conflict → yield(key, old, new) */
                    slots[4] = VAL2HASH(VALUE_REF_GET(dst))->items->data[2 * idx + 1];
                    VALUE argv[3] = { slots[2], slots[4], slots[3] };
                    RESULT r = korb_block_yield(c, slots + 5, block, def_env, argv, 3, cself);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                    slots[3] = r.value;
                }
            }
            CHECK(korb_hash_set(c, slots + 5, dst, VALUE_REF_AT(&slots[2]), slots[3]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Hash#update / merge! — like merge but mutates self in place, returns self. */
static RESULT korb_m_hash_update(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_HASH_P(ov)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(ov));
        slots[0] = ov;                                           /* root the arg hash */
        for (uint32_t i = 0; ; i++) {
            const KorbHash *oh = VAL2HASH(slots[0]);
            if (i >= oh->len) break;
            slots[1] = oh->items->data[2 * i];                   /* key */
            slots[2] = VAL2HASH(slots[0])->items->data[2 * i + 1];   /* new value */
            if (block != NULL) {
                int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(self)), slots[1]);
                if (idx >= 0) {                                  /* conflict → yield(key, old, new) */
                    slots[3] = VAL2HASH(VALUE_REF_GET(self))->items->data[2 * idx + 1];
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
    (void)c;(void)slots;
    const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < h->len; i++)
        if (korb_value_eq(h->items->data[2*i+1], needle)) return RESULT_OK(h->items->data[2*i]);
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_hash_rassoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE needle = VALUE_SLICE_GET(a, 0);
    const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < h->len; i++) {
        if (korb_value_eq(h->items->data[2*i+1], needle)) {
            slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
            CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
            return RESULT_OK(slots[2]);
        }
    }
    return RESULT_OK(KORB_NIL);
}
/* true if every pair of `sub` appears in `sup` with an equal value */
static bool korb_hash_is_subset(const KorbHash *sub, const KorbHash *sup) {
    for (uint32_t i = 0; i < sub->len; i++) {
        int32_t idx = korb_hash_find(sup, sub->items->data[2*i]);
        if (idx < 0) return false;
        if (!korb_value_eq(sub->items->data[2*i+1], sup->items->data[2*idx+1])) return false;
    }
    return true;
}
/* op: 0 `<`  1 `<=`  2 `>`  3 `>=` (subset/superset comparison) */
static RESULT korb_hash_cmp_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int op) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_HASH_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(ov));
    const KorbHash *me = VAL2HASH(VALUE_REF_GET(self)), *other = VAL2HASH(ov);
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

