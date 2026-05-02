/* Hash — moved from builtins.c. */

/* ---------- Hash ---------- */
static VALUE hash_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_hash_aref(self, argv[0]);
}
static VALUE hash_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
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
    /* shallow copy then merge args */
    struct korb_hash *src = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = src->first; e; e = e->next) {
        korb_hash_aset(r, e->key, e->value);
    }
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_HASH) continue;
        struct korb_hash *o = (struct korb_hash *)argv[i];
        for (struct korb_hash_entry *e = o->first; e; e = e->next) {
            korb_hash_aset(r, e->key, e->value);
        }
    }
    return r;
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

