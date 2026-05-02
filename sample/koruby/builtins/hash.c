/* Hash — moved from builtins.c. */

/* ---------- Hash ---------- */
static VALUE hash_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Fast path: bucket lookup.  On miss, fall back to default_proc
     * (calling it with (self, key)) before returning default_value. */
    struct korb_hash *h = (struct korb_hash *)self;
    if (h->size > 0) {
        VALUE r = korb_hash_aref(self, argv[0]);
        if (!UNDEF_P(r)) {
            /* korb_hash_aref returns default_value on miss; distinguish
             * by re-checking presence. */
            uint64_t hh = h->compare_by_identity ? (uint64_t)argv[0] : korb_hash_value(argv[0]);
            uint32_t b = (uint32_t)(hh % h->bucket_cnt);
            for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
                if (e->hash == hh &&
                    (h->compare_by_identity ? e->key == argv[0] : korb_eql(e->key, argv[0])))
                    return e->value;
            }
        }
    }
    /* Miss path. */
    if (!NIL_P(h->default_proc)) {
        VALUE args[2] = {self, argv[0]};
        return korb_funcall(c, h->default_proc, korb_intern("call"), 2, args);
    }
    return h->default_value;
}
static VALUE hash_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    return korb_hash_aset(self, argv[0], argv[1]);
}
static VALUE hash_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(korb_hash_size(self));
}
static VALUE hash_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}


/* ---------- Hash methods (extended) ---------- */

static VALUE hash_compare_by_identity(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    if (h->compare_by_identity) return self;
    h->compare_by_identity = true;
    if (h->size == 0) return self;
    /* Rehash every entry under the new (identity) hash function and rebuild
     * bucket chains.  Insertion order (h->first chain) is preserved. */
    memset(h->buckets, 0, h->bucket_cnt * sizeof(*h->buckets));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        e->hash = (uint64_t)e->key;
        uint32_t b = (uint32_t)(e->hash % h->bucket_cnt);
        e->bucket_next = h->buckets[b];
        h->buckets[b] = e;
    }
    return self;
}

static VALUE hash_compare_by_identity_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    return KORB_BOOL(h->compare_by_identity);
}

static VALUE hash_keys(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) korb_ary_push(r, e->key);
    return r;
}

static VALUE hash_values(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) korb_ary_push(r, e->value);
    return r;
}

static VALUE hash_each_value(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_yield(c, 1, &e->value);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE hash_each_key(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_yield(c, 1, &e->key);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE hash_key_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    struct korb_hash *h = (struct korb_hash *)self;
    uint64_t hh = korb_hash_value(argv[0]);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(e->key, argv[0])) return Qtrue;
    }
    return Qfalse;
}

static VALUE hash_merge(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* shallow copy then merge args.  When a block is given, on key
     * conflict it's invoked as `block.call(key, old_val, new_val)`
     * and its return value becomes the merged value. */
    struct korb_hash *src = (struct korb_hash *)self;
    bool has_block = korb_block_given();
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = src->first; e; e = e->next) {
        korb_hash_aset(r, e->key, e->value);
    }
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_HASH) continue;
        struct korb_hash *o = (struct korb_hash *)argv[i];
        for (struct korb_hash_entry *e = o->first; e; e = e->next) {
            /* Detect "key already present in r" via the entry list, not
             * korb_hash_aref's value (which returns the default value on
             * miss and is indistinguishable from a stored nil/undef). */
            bool already = false;
            VALUE existing = Qnil;
            struct korb_hash *rh = (struct korb_hash *)r;
            for (struct korb_hash_entry *re = rh->first; re; re = re->next) {
                if (korb_eql(re->key, e->key)) { existing = re->value; already = true; break; }
            }
            if (has_block && already) {
                VALUE args[3] = { e->key, existing, e->value };
                VALUE merged = korb_yield(c, 3, args);
                if (c->state != KORB_NORMAL) return Qnil;
                korb_hash_aset(r, e->key, merged);
            } else {
                korb_hash_aset(r, e->key, e->value);
            }
        }
    }
    return r;
}

/* Hash#merge! / #update — destructive merge into self. */
static VALUE hash_merge_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_HASH) continue;
        struct korb_hash *o = (struct korb_hash *)argv[i];
        for (struct korb_hash_entry *e = o->first; e; e = e->next) {
            korb_hash_aset(self, e->key, e->value);
        }
    }
    return self;
}

static VALUE hash_invert(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_hash_aset(r, e->value, e->key);
    }
    return r;
}

static VALUE hash_to_a(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        korb_ary_push(r, pair);
    }
    return r;
}

static VALUE hash_fetch(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    struct korb_hash *h = (struct korb_hash *)self;
    uint64_t hh = korb_hash_value(argv[0]);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(e->key, argv[0])) return e->value;
    }
    /* not found: default arg or block or raise */
    if (argc >= 2) return argv[1];
    /* try yielding to block */
    VALUE r = korb_yield(c, 1, &argv[0]);
    if (c->state != KORB_NORMAL) return Qnil;
    return r;
}

static VALUE hash_delete(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE key = argv[0];
    uint64_t hh = h->compare_by_identity ? (uint64_t)key : korb_hash_value(key);
    uint32_t b = (uint32_t)(hh % h->bucket_cnt);
    /* Unlink from bucket chain */
    struct korb_hash_entry **slot = &h->buckets[b];
    struct korb_hash_entry *target = NULL;
    while (*slot) {
        if ((*slot)->hash == hh &&
            (h->compare_by_identity ? ((*slot)->key == key) : korb_eql((*slot)->key, key))) {
            target = *slot;
            *slot = target->bucket_next;
            break;
        }
        slot = &(*slot)->bucket_next;
    }
    if (!target) return Qnil;
    /* Unlink from insertion-order chain */
    struct korb_hash_entry *prev = NULL;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e == target) {
            if (prev) prev->next = e->next;
            else h->first = e->next;
            if (h->last == e) h->last = prev;
            break;
        }
        prev = e;
    }
    h->size--;
    return target->value;
}

static VALUE hash_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_HASH);
}

static VALUE hash_dup(CTX *c, VALUE self, int argc, VALUE *argv) {
    return hash_merge(c, self, 0, NULL);
}

static VALUE hash_empty_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(((struct korb_hash *)self)->size == 0);
}

static VALUE hash_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        VALUE m = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        korb_ary_push(r, m);
    }
    return r;
}

static VALUE hash_select(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        VALUE m = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) korb_hash_aset(r, e->key, e->value);
    }
    return r;
}

/* Hash.new(default = nil) / Hash.new { |h, k| ... }. */
static VALUE hash_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE h = korb_hash_new();
    struct korb_hash *hh = (struct korb_hash *)h;
    extern struct korb_proc *current_block;
    if (current_block) {
        hh->default_proc = (VALUE)current_block;
    } else if (argc >= 1) {
        hh->default_value = argv[0];
    }
    return h;
}

/* Hash#default — the default_value or nil. */
static VALUE hash_default_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ((struct korb_hash *)self)->default_value;
}

/* Hash#default= — set the default_value. */
static VALUE hash_default_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    ((struct korb_hash *)self)->default_value = argv[0];
    return argv[0];
}

/* Hash#default_proc — the default_proc or nil. */
static VALUE hash_default_proc_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ((struct korb_hash *)self)->default_proc;
}

/* Hash#clear — empty the hash. */
static VALUE hash_clear(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    /* Walk buckets and clear chains. */
    for (uint32_t i = 0; i < h->bucket_cnt; i++) h->buckets[i] = NULL;
    h->first = h->last = NULL;
    h->size = 0;
    return self;
}

/* Hash#delete_if { |k, v| ... } — destructive reject. */
static VALUE hash_delete_if(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    /* Snapshot keys so we can iterate without mutation issues. */
    VALUE keys = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_ary_push(keys, e->key);
    }
    struct korb_array *ka = (struct korb_array *)keys;
    for (long i = 0; i < ka->len; i++) {
        VALUE k = ka->ptr[i];
        VALUE v = korb_hash_aref(self, k);
        VALUE args[2] = {k, v};
        VALUE drop = korb_yield(c, 2, args);
        if (c->state == KORB_RAISE) return Qnil;
        if (RTEST(drop)) {
            VALUE ad[1] = {k};
            hash_delete(c, self, 1, ad);
        }
    }
    return self;
}

/* Hash#keep_if { |k, v| ... } — opposite of delete_if. */
static VALUE hash_keep_if(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE keys = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_ary_push(keys, e->key);
    }
    struct korb_array *ka = (struct korb_array *)keys;
    for (long i = 0; i < ka->len; i++) {
        VALUE k = ka->ptr[i];
        VALUE v = korb_hash_aref(self, k);
        VALUE args[2] = {k, v};
        VALUE keep = korb_yield(c, 2, args);
        if (c->state == KORB_RAISE) return Qnil;
        if (!RTEST(keep)) {
            VALUE ad[1] = {k};
            hash_delete(c, self, 1, ad);
        }
    }
    return self;
}

/* Hash#compact — return a copy with nil values removed. */
static VALUE hash_compact(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (!NIL_P(e->value)) korb_hash_aset(r, e->key, e->value);
    }
    return r;
}

/* Hash#compact! — destructive compact. */
static VALUE hash_compact_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE keys = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (NIL_P(e->value)) korb_ary_push(keys, e->key);
    }
    struct korb_array *ka = (struct korb_array *)keys;
    for (long i = 0; i < ka->len; i++) {
        VALUE ad[1] = {ka->ptr[i]};
        hash_delete(c, self, 1, ad);
    }
    return self;
}

/* Hash#values_at(*keys) — array of corresponding values. */
static VALUE hash_values_at(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = korb_ary_new();
    for (int i = 0; i < argc; i++) korb_ary_push(r, korb_hash_aref(self, argv[i]));
    return r;
}

/* Hash#fetch_values(*keys) — array of values; raises if any key missing. */
static VALUE hash_fetch_values(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (int i = 0; i < argc; i++) {
        VALUE k = argv[i];
        bool found = false;
        VALUE v = Qnil;
        uint64_t hh = h->compare_by_identity ? (uint64_t)k : korb_hash_value(k);
        uint32_t b = (uint32_t)(hh % h->bucket_cnt);
        for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
            if (e->hash == hh &&
                (h->compare_by_identity ? e->key == k : korb_eql(e->key, k))) {
                found = true; v = e->value; break;
            }
        }
        if (!found) {
            korb_raise(c, NULL, "key not found");
            return Qnil;
        }
        korb_ary_push(r, v);
    }
    return r;
}

/* Hash#reject — non-destructive. */
static VALUE hash_reject(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = {e->key, e->value};
        VALUE drop = korb_yield(c, 2, args);
        if (c->state == KORB_RAISE) return Qnil;
        if (!RTEST(drop)) korb_hash_aset(r, e->key, e->value);
    }
    return r;
}

/* Hash#replace(other) — destructive replace. */
static VALUE hash_replace(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_HASH) return self;
    hash_clear(c, self, 0, NULL);
    struct korb_hash *src = (struct korb_hash *)argv[0];
    for (struct korb_hash_entry *e = src->first; e; e = e->next) {
        korb_hash_aset(self, e->key, e->value);
    }
    return self;
}

/* Hash#shift — remove and return the first [k, v] pair. */
static VALUE hash_shift(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    if (!h->first) return Qnil;
    VALUE k = h->first->key;
    VALUE v = h->first->value;
    VALUE ad[1] = {k};
    hash_delete(c, self, 1, ad);
    VALUE pair = korb_ary_new_capa(2);
    korb_ary_push(pair, k);
    korb_ary_push(pair, v);
    return pair;
}

/* Hash#slice(*keys) — sub-hash with only the given keys (those that exist). */
static VALUE hash_slice(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (int i = 0; i < argc; i++) {
        VALUE k = argv[i];
        uint64_t hh = h->compare_by_identity ? (uint64_t)k : korb_hash_value(k);
        uint32_t b = (uint32_t)(hh % h->bucket_cnt);
        for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
            if (e->hash == hh &&
                (h->compare_by_identity ? e->key == k : korb_eql(e->key, k))) {
                korb_hash_aset(r, e->key, e->value);
                break;
            }
        }
    }
    return r;
}

/* Hash#except(*keys) — copy without the given keys. */
static VALUE hash_except(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        bool skip = false;
        for (int i = 0; i < argc; i++) {
            if (korb_eql(e->key, argv[i])) { skip = true; break; }
        }
        if (!skip) korb_hash_aset(r, e->key, e->value);
    }
    return r;
}

/* Hash#count — h.size if no block, else count where block returns truthy. */
static VALUE hash_count(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    extern struct korb_proc *current_block;
    if (!current_block) return INT2FIX((long)h->size);
    long n = 0;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = {e->key, e->value};
        VALUE r = korb_yield(c, 2, args);
        if (c->state == KORB_RAISE) return Qnil;
        if (RTEST(r)) n++;
    }
    return INT2FIX(n);
}

/* Hash#min_by, Hash#max_by — yields [k, v]; finds min/max by block. */
static VALUE hash_min_or_max_by(CTX *c, VALUE self, int argc, VALUE *argv, int max) {
    struct korb_hash *h = (struct korb_hash *)self;
    if (!h->first) return Qnil;
    VALUE best_pair = Qnil;
    VALUE best_key = Qnil;
    bool first = true;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        VALUE bk = korb_yield(c, 1, &pair);
        if (c->state == KORB_RAISE) return Qnil;
        if (first) {
            best_pair = pair;
            best_key  = bk;
            first = false;
        } else {
            VALUE cmp = korb_funcall(c, bk, korb_intern("<=>"), 1, &best_key);
            if (FIXNUM_P(cmp)) {
                long cv = FIX2LONG(cmp);
                if ((max && cv > 0) || (!max && cv < 0)) {
                    best_pair = pair;
                    best_key = bk;
                }
            }
        }
    }
    return best_pair;
}
static VALUE hash_min_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    return hash_min_or_max_by(c, self, argc, argv, 0);
}
static VALUE hash_max_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    return hash_min_or_max_by(c, self, argc, argv, 1);
}

/* Hash#sort — array of [k, v] sorted by [k, v] <=>. */
static VALUE hash_sort(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        korb_ary_push(r, pair);
    }
    /* Sort by [k, v] <=> */
    return korb_funcall(c, r, korb_intern("sort"), 0, NULL);
}

static VALUE hash_reduce(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE acc = argc > 0 ? argv[0] : Qnil;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        VALUE args[2] = { acc, pair };
        acc = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return acc;
}

/* ---------- Hash#dig ----------
 * h.dig(k1, k2, ...) — equivalent to h[k1][k2]..., short-circuiting on
 * nil and dispatching #dig on intermediates so Hash/Array chains compose. */
static VALUE hash_dig(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) {
        VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eArg, "wrong number of arguments to dig (0 for 1+)");
        return Qnil;
    }
    VALUE first = korb_hash_aref(self, argv[0]);
    if (UNDEF_P(first)) first = Qnil;
    if (argc == 1) return first;
    if (NIL_P(first)) return Qnil;
    return korb_funcall(c, first, korb_intern("dig"), argc - 1, argv + 1);
}

/* ---------- Hash#has_value? / value? ---------- */
static VALUE hash_has_value_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (korb_eq(e->value, argv[0])) return Qtrue;
    }
    return Qfalse;
}

/* ---------- Hash#group_by ----------
 * Bins [k, v] pairs under whatever the block returns. */
static VALUE hash_group_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        VALUE args[2] = { e->key, e->value };
        VALUE key = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE bucket = korb_hash_aref(r, key);
        if (UNDEF_P(bucket) || NIL_P(bucket)) {
            bucket = korb_ary_new();
            korb_hash_aset(r, key, bucket);
        }
        korb_ary_push(bucket, pair);
    }
    return r;
}

/* ---------- Hash#sort_by ----------
 * Materialize [k, v] pairs + sort-keys, insertion-sort, return ordered
 * pair list.  Hash sizes encountered here are small enough that O(n^2)
 * is fine. */
static VALUE hash_sort_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE pairs = korb_ary_new();
    VALUE keys  = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        VALUE args[2] = { e->key, e->value };
        VALUE k = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        korb_ary_push(pairs, pair);
        korb_ary_push(keys, k);
    }
    struct korb_array *pa = (struct korb_array *)pairs;
    struct korb_array *ka = (struct korb_array *)keys;
    for (long i = 1; i < ka->len; i++) {
        long j = i;
        while (j > 0) {
            VALUE cmp = korb_funcall(c, ka->ptr[j], korb_intern("<=>"), 1, &ka->ptr[j-1]);
            if (!FIXNUM_P(cmp) || FIX2LONG(cmp) >= 0) break;
            VALUE tk = ka->ptr[j]; ka->ptr[j] = ka->ptr[j-1]; ka->ptr[j-1] = tk;
            VALUE tp = pa->ptr[j]; pa->ptr[j] = pa->ptr[j-1]; pa->ptr[j-1] = tp;
            j--;
        }
    }
    return pairs;
}

/* ---------- Hash#filter_map ---------- */
static VALUE hash_filter_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        VALUE m = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) korb_ary_push(r, m);
    }
    return r;
}

/* ---------- Hash#each_with_object ----------
 * Yields ([k, v], memo) and returns memo at the end. */
static VALUE hash_each_with_object(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    VALUE memo = argv[0];
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        VALUE args[2] = { pair, memo };
        korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return memo;
}

/* ---------- Hash#take(n) ---------- */
static VALUE hash_take(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return korb_ary_new();
    long n = FIX2LONG(argv[0]);
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE out = korb_ary_new();
    long taken = 0;
    for (struct korb_hash_entry *e = h->first; e && taken < n; e = e->next, taken++) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        korb_ary_push(out, pair);
    }
    return out;
}

/* ---------- Hash#flat_map ----------
 * Yields (k, v); flattens one level into the result. */
static VALUE hash_flat_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        VALUE m = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        if (!SPECIAL_CONST_P(m) && BUILTIN_TYPE(m) == T_ARRAY) {
            struct korb_array *ma = (struct korb_array *)m;
            for (long i = 0; i < ma->len; i++) korb_ary_push(r, ma->ptr[i]);
        } else {
            korb_ary_push(r, m);
        }
    }
    return r;
}

