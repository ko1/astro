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
    /* CRuby Hash#each yields a single 2-element Array per pair: a
     * 1-param block sees the [key, value] tuple; a 2-param block
     * auto-destructures into k, v.  Yielding 2 args directly would
     * give the 1-param block only the key. */
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        korb_yield(c, 1, &pair);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}


/* ---------- Hash methods (extended) ---------- */

static VALUE hash_compare_by_identity(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
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
    struct korb_hash *rh = (struct korb_hash *)r;
    /* Preserve compare_by_identity / default_value / default_proc
     * across dup/merge (CRuby semantics). */
    rh->compare_by_identity = src->compare_by_identity;
    rh->default_value = src->default_value;
    rh->default_proc = src->default_proc;
    /* Honor subclass: result has self's class (CRuby semantics). */
    if (((struct RBasic *)self)->klass != (VALUE)korb_vm->hash_class) {
        ((struct RBasic *)r)->klass = ((struct RBasic *)self)->klass;
    }
    for (struct korb_hash_entry *e = src->first; e; e = e->next) {
        korb_hash_aset(r, e->key, e->value);
    }
    for (int i = 0; i < argc; i++) {
        VALUE arg = argv[i];
        if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_HASH) {
            /* Coerce via #to_hash — CRuby semantics.  Try the call
             * unconditionally (covers method_missing-based mocks); only
             * skip if it raises NoMethodError. */
            if (!SPECIAL_CONST_P(arg)) {
                arg = korb_funcall(c, arg, korb_intern("to_hash"), 0, NULL);
                if (c->state == KORB_RAISE) {
                    /* swallow NoMethodError; propagate other errors */
                    VALUE bang = c->state_value;
                    VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
                    if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) &&
                        BUILTIN_TYPE(eNo) == T_CLASS) {
                        struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                        bool is_nm = false;
                        for (struct korb_class *kk = bk; kk; kk = kk->super) {
                            if (kk == (struct korb_class *)eNo) { is_nm = true; break; }
                        }
                        if (is_nm) {
                            c->state = KORB_NORMAL;
                            c->state_value = Qnil;
                            continue;
                        }
                    }
                    return Qnil;
                }
            }
            if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_HASH) continue;
        }
        struct korb_hash *o = (struct korb_hash *)arg;
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
    CHECK_FROZEN_RET(c, self, Qnil);
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

/* Hash#__korb_kwargs_validate__(declared_keys) — internal: raise
 * ArgumentError("unknown keyword(s): :foo, :bar") if any key in the
 * receiver isn't in `declared_keys` (an Array of Symbols).  Used by
 * the def prologue when no **kwrest is declared. */
static VALUE hash_kwargs_validate(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_ARRAY) return Qnil;
    const struct korb_hash *h = (const struct korb_hash *)self;
    const struct korb_array *decl = (const struct korb_array *)argv[0];
    /* Collect unknowns. */
    uint32_t cap = 16;
    uint32_t cnt = 0;
    VALUE *unknown = korb_xmalloc(cap * sizeof(VALUE));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        bool found = false;
        for (size_t j = 0; j < decl->len; j++) {
            if (korb_eql(e->key, decl->ptr[j])) { found = true; break; }
        }
        if (!found) {
            if (cnt >= cap) {
                cap *= 2;
                unknown = korb_xrealloc(unknown, cap * sizeof(VALUE));
            }
            unknown[cnt++] = e->key;
        }
    }
    if (cnt == 0) return Qnil;
    /* Build "unknown keyword: :foo" / "unknown keywords: :foo, :bar" */
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "unknown keyword%s:", cnt == 1 ? "" : "s");
    for (uint32_t i = 0; i < cnt && off < (int)sizeof(buf) - 4; i++) {
        VALUE v = korb_inspect(unknown[i]);
        const char *vs = (BUILTIN_TYPE(v) == T_STRING)
                           ? ((struct korb_string *)v)->ptr : "?";
        off += snprintf(buf + off, sizeof(buf) - off, "%s %s", i == 0 ? "" : ",", vs);
    }
    VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
    korb_raise(c, (struct korb_class *)eArg, "%s", buf);
    return Qnil;
}

/* Hash#__korb_required_kwarg__(name) — internal: fetch the key or raise
 * ArgumentError "missing keyword: name".  Used by parse.c when emitting
 * the prologue for `def f(name:)` so the missing-key path produces the
 * canonical ArgumentError instead of a leaked KeyError. */
static VALUE hash_required_kwarg(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    const struct korb_hash *h = (const struct korb_hash *)self;
    uint64_t hh = korb_hash_value(argv[0]);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(e->key, argv[0])) return e->value;
    }
    VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
    const char *kn = SYMBOL_P(argv[0]) ? korb_id_name(korb_sym2id(argv[0])) : "?";
    korb_raise(c, (struct korb_class *)eArg, "missing keyword: :%s", kn);
    return Qnil;
}

/* Hash#__korb_required_kwargs_check__(keys) — internal: verify all keys
 * present in self; if any are missing, raise ArgumentError listing them
 * all (CRuby's "missing keywords: :a, :b" format).  Called once at the
 * top of the kwargs prologue so the error mentions every missing key
 * instead of just the first encountered. */
static VALUE hash_required_kwargs_check(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_ARRAY) return Qnil;
    const struct korb_hash *h = (const struct korb_hash *)self;
    const struct korb_array *keys = (const struct korb_array *)argv[0];
    /* Collect missing keys preserving declared order. */
    VALUE missing = korb_ary_new();
    for (long i = 0; i < (long)keys->len; i++) {
        VALUE key = keys->ptr[i];
        uint64_t hh = korb_hash_value(key);
        bool found = false;
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            if (e->hash == hh && korb_eql(e->key, key)) { found = true; break; }
        }
        if (!found) korb_ary_push(missing, key);
    }
    struct korb_array *miss_a = (struct korb_array *)missing;
    if (miss_a->len == 0) return Qnil;
    /* Build "missing keyword(s): :a, :b" message. */
    char buf[1024];
    int off = 0;
    const char *plural = miss_a->len > 1 ? "keywords" : "keyword";
    off += snprintf(buf + off, sizeof(buf) - off, "missing %s: ", plural);
    for (long i = 0; i < (long)miss_a->len && off < (int)sizeof(buf) - 16; i++) {
        const char *kn = SYMBOL_P(miss_a->ptr[i]) ? korb_id_name(korb_sym2id(miss_a->ptr[i])) : "?";
        off += snprintf(buf + off, sizeof(buf) - off, "%s:%s",
                        i == 0 ? "" : ", ", kn);
    }
    VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
    korb_raise(c, (struct korb_class *)eArg, "%s", buf);
    return Qnil;
}

static VALUE hash_fetch(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || argc > 2) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1..2)", argc);
        return Qnil;
    }
    const struct korb_hash *h = (const struct korb_hash *)self;
    uint64_t hh = korb_hash_value(argv[0]);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(e->key, argv[0])) return e->value;
    }
    /* Not found: priority is block > default arg > KeyError. */
    if (korb_block_given()) {
        VALUE r = korb_yield(c, 1, &argv[0]);
        if (c->state != KORB_NORMAL) return Qnil;
        return r;
    }
    if (argc >= 2) return argv[1];
    VALUE eKey = korb_const_get(korb_vm->object_class, korb_intern("KeyError"));
    if (UNDEF_P(eKey) || !eKey) eKey = (VALUE)NULL;
    VALUE ks = korb_inspect(argv[0]);
    korb_raise(c, (struct korb_class *)eKey, "key not found: %s", korb_str_cstr(ks));
    return Qnil;
}

static VALUE hash_delete(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
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
    if (!target) {
        /* Not found: if a block was given, yield key and return its result. */
        if (korb_block_given()) {
            VALUE r = korb_yield(c, 1, &key);
            if (c->state != KORB_NORMAL) return Qnil;
            return r;
        }
        return Qnil;
    }
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

/* Hash#clone — like dup but preserves frozen flag (CRuby semantics). */
static VALUE hash_clone(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = hash_merge(c, self, 0, NULL);
    if (korb_obj_frozen_p(self) && !SPECIAL_CONST_P(r)) {
        ((struct RBasic *)r)->head.flags |= FL_FROZEN;
    }
    return r;
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
    const struct korb_hash *h = (const struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        VALUE m = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) korb_hash_aset(r, e->key, e->value);
    }
    return r;
}

/* Hash#partition — yield [k, v] pairs to block; return [[match],[no_match]]
 * each as Arrays of [k,v] pairs (not Hashes — matching CRuby's Enumerable
 * behavior on Hash). */
static VALUE hash_partition(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_hash *h = (const struct korb_hash *)self;
    VALUE yes = korb_ary_new();
    VALUE no = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        VALUE m = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        korb_ary_push(RTEST(m) ? yes : no, pair);
    }
    VALUE pair = korb_ary_new_capa(2);
    korb_ary_push(pair, yes);
    korb_ary_push(pair, no);
    return pair;
}

/* Hash#tally — Enumerable's tally returns counts of distinct values.
 * For a hash, counts pairs (which are unique by key already), so returns
 * each pair → 1.  CRuby behaves the same way. */
static VALUE hash_tally(CTX *c, VALUE self, int argc, VALUE *argv) {
    const struct korb_hash *h = (const struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        korb_hash_aset(r, pair, INT2FIX(1));
    }
    return r;
}

/* Hash.new(default = nil) / Hash.new { |h, k| ... }. */
/* Hash[pairs] / Hash[k1,v1,k2,v2,...] / Hash[other_hash] — convert to Hash. */
/* Apply receiver-class to a freshly-created hash.  When self is a
 * Hash subclass, the result should be an instance of that subclass. */
static inline void hash_apply_self_class(VALUE r, VALUE self) {
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_CLASS &&
        self != (VALUE)korb_vm->hash_class) {
        ((struct RBasic *)r)->klass = self;
    }
}
static VALUE hash_class_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 0) {
        VALUE r = korb_hash_new();
        hash_apply_self_class(r, self);
        return r;
    }
    if (argc == 1) {
        VALUE arg = argv[0];
        if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_HASH) {
            VALUE r = korb_hash_new();
            hash_apply_self_class(r, self);
            struct korb_hash *src = (struct korb_hash *)arg;
            for (struct korb_hash_entry *e = src->first; e; e = e->next) {
                korb_hash_aset(r, e->key, e->value);
            }
            return r;
        }
        if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_ARRAY) {
            /* Hash[ [[k,v], [k,v]] ] form. */
            VALUE r = korb_hash_new();
            hash_apply_self_class(r, self);
            struct korb_array *a = (struct korb_array *)arg;
            for (long i = 0; i < a->len; i++) {
                VALUE pair = a->ptr[i];
                if (!SPECIAL_CONST_P(pair) && BUILTIN_TYPE(pair) == T_ARRAY) {
                    struct korb_array *p = (struct korb_array *)pair;
                    if (p->len == 2) korb_hash_aset(r, p->ptr[0], p->ptr[1]);
                }
            }
            return r;
        }
    }
    /* Hash[k,v,k,v,...] flat form. */
    VALUE r = korb_hash_new();
    hash_apply_self_class(r, self);
    for (int i = 0; i + 1 < argc; i += 2) {
        korb_hash_aset(r, argv[i], argv[i+1]);
    }
    return r;
}

static VALUE hash_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    /* Ruby 3.2+: Hash.new accepts a `capacity:` keyword (and rejects
     * unknown keywords).  Peel off a trailing FL_KWARGS hash before the
     * arity check so `Hash.new(capacity: 100)` doesn't look like a
     * positional default. */
    int eff_argc = argc;
    if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH &&
        (RBASIC(argv[argc - 1])->head.flags & FL_KWARGS)) {
        struct korb_hash *kw = (struct korb_hash *)argv[argc - 1];
        VALUE cap_key = korb_id2sym(korb_intern("capacity"));
        for (struct korb_hash_entry *e = kw->first; e; e = e->next) {
            if (!korb_eql(e->key, cap_key)) {
                VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
                const char *kn = SYMBOL_P(e->key) ? korb_id_name(korb_sym2id(e->key)) : "?";
                korb_raise(c, (struct korb_class *)eA,
                           "unknown keyword: :%s", kn);
                return Qnil;
            }
        }
        /* capacity is informational only — we don't pre-grow buckets. */
        eff_argc = argc - 1;
    }
    if (eff_argc > 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0..1)", eff_argc);
        return Qnil;
    }
    if (current_block && eff_argc >= 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given 1, expected 0)");
        return Qnil;
    }
    VALUE h = korb_hash_new();
    struct korb_hash *hh = (struct korb_hash *)h;
    if (current_block) {
        hh->default_proc = (VALUE)current_block;
    } else if (eff_argc >= 1) {
        hh->default_value = argv[0];
    }
    return h;
}

/* Hash#default — the default_value or nil. */
static VALUE hash_default_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    /* Hash#default(key) — when default_proc is set and a key is given,
     * call the proc with (self, key); otherwise return default_value. */
    if (!NIL_P(h->default_proc) && argc >= 1) {
        VALUE args[2] = { self, argv[0] };
        return korb_funcall(c, h->default_proc, korb_intern("call"), 2, args);
    }
    return h->default_value;
}

/* Hash#default= — set the default_value, clear default_proc. */
static VALUE hash_default_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_hash *h = (struct korb_hash *)self;
    h->default_value = argv[0];
    h->default_proc = Qnil;  /* setting default value clears default_proc */
    return argv[0];
}

/* Hash#default_proc — the default_proc or nil. */
static VALUE hash_default_proc_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE v = ((struct korb_hash *)self)->default_proc;
    return NIL_P(v) ? Qnil : v;
}

/* Hash#default_proc= — store a Proc as the miss-path resolver. */
static VALUE hash_default_proc_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    CHECK_FROZEN_RET(c, self, Qnil);
    VALUE blk = argv[0];
    struct korb_hash *h = (struct korb_hash *)self;
    if (NIL_P(blk)) {
        h->default_proc = Qnil;
        return Qnil;
    }
    /* Coerce non-Proc via #to_proc (mock objects, etc.). */
    if (SPECIAL_CONST_P(blk) || BUILTIN_TYPE(blk) != T_PROC) {
        if (!SPECIAL_CONST_P(blk)) {
            VALUE rt = korb_funcall(c, blk, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_proc")) });
            if (c->state == KORB_RAISE) return Qnil;
            if (RTEST(rt)) {
                blk = korb_funcall(c, blk, korb_intern("to_proc"), 0, NULL);
                if (c->state == KORB_RAISE) return Qnil;
            }
        }
        if (SPECIAL_CONST_P(blk) || BUILTIN_TYPE(blk) != T_PROC) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "wrong default_proc type %s (expected Proc)",
                       korb_id_name(korb_class_of_class(argv[0])->name));
            return Qnil;
        }
    }
    /* Lambda with wrong arity → TypeError ("default_proc takes two
     * arguments (2 for arity)"). */
    struct korb_proc *p = (struct korb_proc *)blk;
    if (p->is_lambda) {
        long ar = (long)p->params_cnt - (long)p->opt_cnt + (long)p->post_cnt;
        if (ar != 2) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "default_proc takes two arguments (2 for %ld)", ar);
            return Qnil;
        }
    }
    h->default_proc = blk;
    /* CRuby: setting default_proc clears default_value. */
    h->default_value = Qnil;
    return argv[0];
}

/* Hash#clear — empty the hash. */
static VALUE hash_clear(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_hash *h = (struct korb_hash *)self;
    /* Walk buckets and clear chains. */
    for (uint32_t i = 0; i < h->bucket_cnt; i++) h->buckets[i] = NULL;
    h->first = h->last = NULL;
    h->size = 0;
    return self;
}

/* Hash#delete_if { |k, v| ... } — destructive reject. */
static VALUE hash_delete_if(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
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

/* Hash#reject! — like delete_if but returns nil if no entries were
 * removed (CRuby bang semantics). */
static VALUE hash_reject_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE keys = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_ary_push(keys, e->key);
    }
    struct korb_array *ka = (struct korb_array *)keys;
    bool any_deleted = false;
    for (long i = 0; i < ka->len; i++) {
        VALUE k = ka->ptr[i];
        VALUE v = korb_hash_aref(self, k);
        VALUE args[2] = {k, v};
        VALUE drop = korb_yield(c, 2, args);
        if (c->state == KORB_RAISE) return Qnil;
        if (RTEST(drop)) {
            VALUE ad[1] = {k};
            hash_delete(c, self, 1, ad);
            any_deleted = true;
        }
    }
    return any_deleted ? self : Qnil;
}

/* Hash#keep_if { |k, v| ... } — opposite of delete_if. */
static VALUE hash_keep_if(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
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
    struct korb_hash *rh = (struct korb_hash *)r;
    /* Preserve default value/proc and compare_by_identity (CRuby
     * semantics: compact returns a new Hash with same settings). */
    rh->default_value = h->default_value;
    rh->default_proc = h->default_proc;
    rh->compare_by_identity = h->compare_by_identity;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (!NIL_P(e->value)) korb_hash_aset(r, e->key, e->value);
    }
    return r;
}

/* Hash#compact! — destructive compact.  Returns nil if no nil values
 * were removed (CRuby semantics: bang methods that didn't change the
 * receiver return nil). */
static VALUE hash_compact_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE keys = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (NIL_P(e->value)) korb_ary_push(keys, e->key);
    }
    struct korb_array *ka = (struct korb_array *)keys;
    if (ka->len == 0) return Qnil;
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
            /* Block fallback: yield key for the missing entry, push the
             * block's result.  Otherwise raise KeyError. */
            if (korb_block_given()) {
                VALUE blk_r = korb_yield(c, 1, &k);
                if (c->state == KORB_RAISE) return Qnil;
                korb_ary_push(r, blk_r);
                continue;
            }
            VALUE eK = korb_const_get(korb_vm->object_class, korb_intern("KeyError"));
            korb_raise(c, eK ? (struct korb_class *)eK : NULL,
                       "key not found");
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
    /* Frozen check fires unconditionally — even if argv would have led
     * to a no-op, CRuby still raises FrozenError on a frozen receiver. */
    CHECK_FROZEN_RET(c, self, Qnil);
    if (argc < 1) return self;
    if (self == argv[0]) return self;
    /* Coerce non-Hash via #to_hash. */
    VALUE other = argv[0];
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_HASH) {
        if (!SPECIAL_CONST_P(other)) {
            VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_hash")) });
            if (c->state == KORB_RAISE) return Qnil;
            if (RTEST(rt)) {
                other = korb_funcall(c, other, korb_intern("to_hash"), 0, NULL);
                if (c->state == KORB_RAISE) return Qnil;
            }
        }
        if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_HASH) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into Hash",
                       SPECIAL_CONST_P(argv[0]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[0])->name));
            return Qnil;
        }
    }
    CHECK_FROZEN_RET(c, self, Qnil);
    hash_clear(c, self, 0, NULL);
    struct korb_hash *src = (struct korb_hash *)other;
    struct korb_hash *dst = (struct korb_hash *)self;
    /* Transfer compare_by_identity, default_value, default_proc. */
    dst->compare_by_identity = src->compare_by_identity;
    dst->default_value = src->default_value;
    dst->default_proc = src->default_proc;
    for (struct korb_hash_entry *e = src->first; e; e = e->next) {
        korb_hash_aset(self, e->key, e->value);
    }
    return self;
}

/* Hash#shift — remove and return the first [k, v] pair. */
static VALUE hash_shift(CTX *c, VALUE self, int argc, VALUE *argv) {
    CHECK_FROZEN_RET(c, self, Qnil);
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

/* Hash#sort — array of [k, v] sorted by [k, v] <=>. With a block,
 * forwards the block to Array#sort so the user comparator participates. */
static VALUE hash_sort(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        korb_ary_push(r, pair);
    }
    if (korb_block_given()) {
        return korb_funcall_with_block(c, r, korb_intern("sort"), 0, NULL,
                                        (VALUE)current_block);
    }
    return korb_funcall(c, r, korb_intern("sort"), 0, NULL);
}

/* Hash#deconstruct_keys — pattern-match support hook.  Spec: takes one
 * argument (an Array of keys, or nil to mean "all keys"); returns self.
 * koruby ignores the keys arg and just returns self.  Validates argc. */
static VALUE hash_deconstruct_keys(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc != 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1)", argc);
        return Qnil;
    }
    return self;
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
    /* Intermediate must respond to #dig — else TypeError. */
    VALUE next_klass = (VALUE)korb_class_of_class(first);
    if (!next_klass || !korb_class_find_method((struct korb_class *)next_klass,
                                                 korb_intern("dig"))) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "%s does not have #dig method",
                   korb_id_name(korb_class_of_class(first)->name));
        return Qnil;
    }
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

/* ---------- Hash#sum ----------
 * Yields (k, v) and sums the block's return values onto an
 * accumulator (default 0).  Without a block, attempts +-aggregation
 * over [k, v] pairs (CRuby's behavior, may raise on Symbol+Integer). */
static VALUE hash_sum(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE acc = argc >= 1 ? argv[0] : INT2FIX(0);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE addend;
        if (korb_block_given()) {
            VALUE args[2] = { e->key, e->value };
            addend = korb_yield(c, 2, args);
            if (c->state != KORB_NORMAL) return Qnil;
        } else {
            addend = korb_ary_new_capa(2);
            korb_ary_push(addend, e->key);
            korb_ary_push(addend, e->value);
        }
        acc = korb_funcall(c, acc, korb_intern("+"), 1, &addend);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return acc;
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

