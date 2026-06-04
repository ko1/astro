/* Hash — moved from builtins.c. */

/* KORB_HASH_YIELD_FRAME — park a cross-yield moving root (an accumulator /
 * pair array) in a synthetic frame's last_line slot, made current for the
 * duration of a yield loop.  Identical idiom to array.c's
 * KORB_ARY_YIELD_FRAME.
 *
 * Rationale: korb_yield runs the block body at the block's own (lower) sp,
 * shrinking the GC scan range [stack_base, c->sp_top).  A moving array root
 * parked in an sp[] slot ABOVE that level falls outside the range and gets
 * collected under a moving GC (STRESS).  The frame chain, by contrast, is
 * ALWAYS walked by visit_roots (last_line / last_match are forwarded), so a
 * root stashed there survives regardless of how far the block lowers sp_top.
 *
 * The synthetic frame inherits self / fp / cref / current_class / file from
 * the live frame so the block's lvar / ivar / const / $~ lookups still
 * resolve against the surrounding receiver.  Caller MUST restore
 * c->current_frame = fr.prev on every exit path (including early returns from
 * a propagating block result).  Hash objects + entries are libc-malloc'd
 * (non-moving) so h / e / korb_hash_new results need no parking — only ARRAY
 * handles do.  `init_expr` may itself be a GC point; it is evaluated before
 * the frame is linked and its result is held only across the immediately-
 * following assignment (no GC in between). */
#define KORB_HASH_YIELD_FRAME(c, fr, init_expr)                      \
    struct korb_frame fr = {                                         \
        .prev          = (c)->current_frame,                         \
        .self          = (c)->current_frame->self,                   \
        .fp            = (c)->current_frame->fp,                     \
        .cref          = (c)->current_frame->cref,                   \
        .current_class = (c)->current_frame->current_class,          \
        .current_file  = (c)->current_frame->current_file,           \
        .block         = (c)->current_frame->block,                  \
        .last_line     = Qnil,                                       \
        .last_match    = (c)->current_frame->last_match,             \
    };                                                               \
    fr.last_line = (init_expr);                                      \
    (c)->current_frame = &fr

/* ---------- Hash ---------- */
static RESULT hash_aref(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Fast path: bucket lookup.  On miss, dispatch through #default so
     * subclass overrides (`class X < Hash; def default(k); ...; end`)
     * participate in the lookup. */
    struct korb_hash *h = (struct korb_hash *)self;
    korb_hash_rehash_identity_if_stale(h);
    if (h->size > 0) {
        uint64_t hh = h->compare_by_identity ? (uint64_t)argv[0] : korb_hash_value(c, argv[0]);
        uint32_t b = (uint32_t)(hh % h->bucket_cnt);
        for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
            if (e->hash == hh &&
                (h->compare_by_identity ? e->key == argv[0] : korb_eql(c, e->key, argv[0])))
                return RESULT_OK(e->value);
        }
    }
    /* Miss path. */
    /* For the canonical Hash class skip the funcall round-trip. */
    struct korb_class *kls = korb_class_of_class(self);
    if (kls == KORB_VM(c)->hash_class) {
        if (!NIL_P(h->default_proc)) {
            VALUE args[2] = {self, argv[0]};
            return korb_funcall(c, c->sp_top, h->default_proc, korb_intern("call"), 2, args);
        }
        return RESULT_OK(h->default_value);
    }
    /* Subclass: defer to #default so overrides apply. */
    return korb_funcall(c, c->sp_top, self, korb_intern("default"), 1, &argv[0]);
}
static RESULT hash_aset(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    VALUE key = argv[0];
    /* CRuby semantics: dup + freeze unfrozen String keys to prevent
     * later mutation from corrupting the hash table.  If the same
     * (string-equal) key already lives in the hash, reuse it. */
    if (!SPECIAL_CONST_P(key) && BUILTIN_TYPE(key) == T_STRING &&
        !korb_obj_frozen_p(key) && !((struct korb_hash *)self)->compare_by_identity) {
        /* Look up an existing equal key. */
        struct korb_hash *h = (struct korb_hash *)self;
        bool found = false;
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            if (e->key == key || korb_eql(c, e->key, key)) {
                key = e->key; found = true; break;
            }
        }
        if (!found) {
            VALUE dup_key = korb_str_dup(c, c->sp_top, key);
            ((struct RBasic *)dup_key)->head.flags |= FL_FROZEN;
            key = dup_key;
        }
    }
    return RESULT_OK(korb_hash_aset(c, self, key, argv[1]));
}
static RESULT hash_size(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(INT2FIX(korb_hash_size(self)));
}
static RESULT hash_each(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* No block: return Enumerator. */
    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("each"));
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    /* CRuby Hash#each yields a single 2-element Array per pair: a
     * 1-param block sees the [key, value] tuple; a 2-param block
     * auto-destructures into k, v.  Yielding 2 args directly would
     * give the 1-param block only the key. */
    struct korb_hash *h = (struct korb_hash *)self;
    /* Park the per-pair Array (a moving handle) in a synthetic frame so it
     * survives both the per-pair korb_ary_push (a GC point that would collect
     * a bare C-local) and the korb_yield (which lowers sp_top below any sp[]
     * slot).  The hash entries are libc-malloc'd (non-moving) so e stays
     * valid. */
    KORB_HASH_YIELD_FRAME(c, fr, Qnil);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        fr.last_line = korb_ary_new_capa(c, c->sp_top, 2);
        korb_ary_push(c, c->sp_top, fr.last_line, e->key);
        korb_ary_push(c, c->sp_top, fr.last_line, e->value);
        RESULT _y = korb_yield(c, 1, &fr.last_line);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
    }
    c->current_frame = fr.prev;
    return RESULT_OK(self);
}


/* ---------- Hash methods (extended) ---------- */

static RESULT hash_compare_by_identity(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_hash *h = (struct korb_hash *)self;
    if (h->compare_by_identity) return RESULT_OK(self);
    h->compare_by_identity = true;
    if (h->size == 0) return RESULT_OK(self);
    /* Rehash every entry under the new (identity) hash function and rebuild
     * bucket chains.  Insertion order (h->first chain) is preserved. */
    memset(h->buckets, 0, h->bucket_cnt * sizeof(*h->buckets));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        e->hash = (uint64_t)e->key;
        uint32_t b = (uint32_t)(e->hash % h->bucket_cnt);
        e->bucket_next = h->buckets[b];
        h->buckets[b] = e;
    }
    { extern uint64_t korb_g_gc_gen; h->identity_rehash_gen = korb_g_gc_gen; }
    return RESULT_OK(self);
}

static RESULT hash_compare_by_identity_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    return RESULT_OK(KORB_BOOL(h->compare_by_identity));
}

static RESULT hash_keys(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    sp[0] = 0;                       /* park slot; zero before it becomes scanned */
    c->sp_top = sp + 1;
    sp[0] = korb_ary_new(c, sp + 1);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) korb_ary_push(c, sp + 1, sp[0], e->key);
    return RESULT_OK(sp[0]);
}

static RESULT hash_values(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    sp[0] = 0;
    c->sp_top = sp + 1;
    sp[0] = korb_ary_new(c, sp + 1);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) korb_ary_push(c, sp + 1, sp[0], e->value);
    return RESULT_OK(sp[0]);
}

static RESULT hash_each_value(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("each_value"));
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        CHECK(korb_yield(c, 1, &e->value));
    }
    return RESULT_OK(self);
}

static RESULT hash_each_key(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("each_key"));
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        CHECK(korb_yield(c, 1, &e->key));
    }
    return RESULT_OK(self);
}

static RESULT hash_key_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    struct korb_hash *h = (struct korb_hash *)self;
    uint64_t hh = korb_hash_value(c, argv[0]);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(c, e->key, argv[0])) return RESULT_OK(Qtrue);
    }
    return RESULT_OK(Qfalse);
}

static RESULT hash_merge(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* shallow copy then merge args.  When a block is given, on key
     * conflict it's invoked as `block.call(key, old_val, new_val)`
     * and its return value becomes the merged value. */
    struct korb_hash *src = (struct korb_hash *)self;
    bool has_block = korb_block_given(c);
    VALUE r = korb_hash_new(c, c->sp_top);
    struct korb_hash *rh = (struct korb_hash *)r;
    /* Preserve compare_by_identity / default_value / default_proc
     * across dup/merge (CRuby semantics). */
    rh->compare_by_identity = src->compare_by_identity;
    rh->default_value = src->default_value;
    rh->default_proc = src->default_proc;
    /* Honor subclass: result has self's class (CRuby semantics). */
    if (((struct RBasic *)self)->klass != (VALUE)KORB_VM(c)->hash_class) {
        ((struct RBasic *)r)->klass = ((struct RBasic *)self)->klass;
    }
    for (struct korb_hash_entry *e = src->first; e; e = e->next) {
        korb_hash_aset(c, r, e->key, e->value);
    }
    for (int i = 0; i < argc; i++) {
        VALUE arg = argv[i];
        if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_HASH) {
            /* Coerce via #to_hash — CRuby semantics.  Try the call
             * unconditionally (covers method_missing-based mocks); only
             * skip if it raises NoMethodError. */
            if (!SPECIAL_CONST_P(arg)) {
                RESULT _th = korb_funcall(c, c->sp_top, arg, korb_intern("to_hash"), 0, NULL);
                if (_th.state == KORB_RAISE) {
                    /* swallow NoMethodError; propagate other errors */
                    VALUE bang = _th.value;
                    VALUE eNo = korb_const_get(KORB_VM(c)->object_class, korb_intern("NoMethodError"));
                    if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) &&
                        BUILTIN_TYPE(eNo) == T_CLASS) {
                        struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                        bool is_nm = false;
                        for (struct korb_class *kk = bk; kk; kk = kk->super) {
                            if (kk == (struct korb_class *)eNo) { is_nm = true; break; }
                        }
                        if (is_nm) continue;
                    }
                    return _th;
                }
                if (_th.state != KORB_NORMAL) return _th;
                arg = _th.value;
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
                if (korb_eql(c, re->key, e->key)) { existing = re->value; already = true; break; }
            }
            if (has_block && already) {
                VALUE args[3] = { e->key, existing, e->value };
                VALUE merged = UNWRAP(korb_yield(c, 3, args));
                korb_hash_aset(c, r, e->key, merged);
            } else {
                korb_hash_aset(c, r, e->key, e->value);
            }
        }
    }
    return RESULT_OK(r);
}

/* Hash#merge! / #update — destructive merge into self. */
static RESULT hash_merge_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    bool has_block = (c->current_block != NULL);
    for (int i = 0; i < argc; i++) {
        VALUE arg = argv[i];
        /* Try to_hash coercion if not already a Hash.  Attempt the call
         * unconditionally (covers method_missing / mock-based to_hash, which
         * korb_class_find_method does NOT see); a missing to_hash surfaces as
         * NoMethodError, which we convert to the CRuby TypeError. */
        if (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_HASH) {
            if (SPECIAL_CONST_P(arg)) {
                return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                                  "no implicit conversion of (special) into Hash");
            }
            RESULT tr = korb_funcall_r(c, c->sp_top, arg, korb_intern("to_hash"), 0, NULL);
            if (tr.state == KORB_RAISE) {
                VALUE bang = tr.value;
                VALUE eNo = korb_const_get(KORB_VM(c)->object_class, korb_intern("NoMethodError"));
                if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) && BUILTIN_TYPE(eNo) == T_CLASS) {
                    struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                    for (struct korb_class *kk = bk; kk; kk = kk->super) {
                        if (kk == (struct korb_class *)eNo) {
                            /* argv[i] (orig arg) is value-stack-scanned, so it's
                             * forwarded after the funcall GC — safe to read. */
                            return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                                              "no implicit conversion of %s into Hash",
                                              korb_id_name(korb_class_of_class(argv[i])->name));
                        }
                    }
                }
                return tr;
            }
            if (tr.state != KORB_NORMAL) return tr;
            if (SPECIAL_CONST_P(tr.value) || BUILTIN_TYPE(tr.value) != T_HASH) {
                return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                                  "can't convert to Hash (to_hash returned non-Hash)");
            }
            arg = tr.value;
            self = sp[-argc - 1];   /* re-read self after the to_hash GC point */
        }
        struct korb_hash *o = (struct korb_hash *)arg;
        for (struct korb_hash_entry *e = o->first; e; e = e->next) {
            VALUE val = e->value;
            if (has_block) {
                /* Walk self's bucket chain to distinguish "key absent"
                 * from "key present with nil value" — korb_hash_aref
                 * returns the default value for both. */
                struct korb_hash *sh = (struct korb_hash *)self;
                bool found = false;
                VALUE existing = Qnil;
                for (struct korb_hash_entry *se = sh->first; se; se = se->next) {
                    if (se->key == e->key || korb_eql(c, se->key, e->key)) {
                        found = true;
                        existing = se->value;
                        break;
                    }
                }
                if (found) {
                    sp[0] = e->key;
                    sp[1] = existing;
                    sp[2] = e->value;
                    RESULT yr = korb_yield_r(c, 3, &sp[0]);
                    if (yr.state != KORB_NORMAL) return yr;
                    val = yr.value;
                }
            }
            korb_hash_aset(c, self, e->key, val);
        }
    }
    return RESULT_OK(self);
}

static RESULT hash_invert(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new(c, c->sp_top);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_hash_aset(c, r, e->value, e->key);
    }
    return RESULT_OK(r);
}

static RESULT hash_to_a(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    /* r=sp[0], pair=sp[1] parked; zero-fill + reserve so both are scanned. */
    sp[0] = 0; sp[1] = 0;
    c->sp_top = sp + 2;
    sp[0] = korb_ary_new(c, sp + 2);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        sp[1] = korb_ary_new_capa(c, sp + 2, 2);
        korb_ary_push(c, sp + 2, sp[1], e->key);
        korb_ary_push(c, sp + 2, sp[1], e->value);
        korb_ary_push(c, sp + 2, sp[0], sp[1]);
    }
    VALUE result = sp[0];
    c->sp_top = sp;
    return RESULT_OK(result);
}

/* Hash#__korb_kwargs_validate__(declared_keys) — internal: raise
 * ArgumentError("unknown keyword(s): :foo, :bar") if any key in the
 * receiver isn't in `declared_keys` (an Array of Symbols).  Used by
 * the def prologue when no **kwrest is declared. */
static RESULT hash_kwargs_validate(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_ARRAY) return RESULT_OK(Qnil);
    const struct korb_hash *h = (const struct korb_hash *)self;
    const struct korb_array *decl = (const struct korb_array *)argv[0];
    /* Collect unknowns. */
    uint32_t cap = 16;
    uint32_t cnt = 0;
    VALUE *unknown = korb_xmalloc(cap * sizeof(VALUE));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        bool found = false;
        for (size_t j = 0; j < decl->len; j++) {
            if (korb_eql(c, e->key, korb_ary_items(decl)[j])) { found = true; break; }
        }
        if (!found) {
            if (cnt >= cap) {
                cap *= 2;
                unknown = korb_xrealloc(unknown, cap * sizeof(VALUE));
            }
            unknown[cnt++] = e->key;
        }
    }
    if (cnt == 0) return RESULT_OK(Qnil);
    /* Build "unknown keyword: :foo" / "unknown keywords: :foo, :bar" */
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "unknown keyword%s:", cnt == 1 ? "" : "s");
    for (uint32_t i = 0; i < cnt && off < (int)sizeof(buf) - 4; i++) {
        VALUE v = korb_inspect(c, c->sp_top, unknown[i]);
        const char *vs = (BUILTIN_TYPE(v) == T_STRING)
                           ? ((struct korb_string *)v)->ptr : "?";
        off += snprintf(buf + off, sizeof(buf) - off, "%s %s", i == 0 ? "" : ",", vs);
    }
    VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
    return korb_raise(c, (struct korb_class *)eArg, "%s", buf);
}

/* Hash#__korb_required_kwarg__(name) — internal: fetch the key or raise
 * ArgumentError "missing keyword: name".  Used by parse.c when emitting
 * the prologue for `def f(name:)` so the missing-key path produces the
 * canonical ArgumentError instead of a leaked KeyError. */
static RESULT hash_required_kwarg(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    const struct korb_hash *h = (const struct korb_hash *)self;
    uint64_t hh = korb_hash_value(c, argv[0]);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(c, e->key, argv[0])) return RESULT_OK(e->value);
    }
    VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
    const char *kn = SYMBOL_P(argv[0]) ? korb_id_name(korb_sym2id(argv[0])) : "?";
    return korb_raise(c, (struct korb_class *)eArg, "missing keyword: :%s", kn);
}

/* Hash#__korb_required_kwargs_check__(keys) — internal: verify all keys
 * present in self; if any are missing, raise ArgumentError listing them
 * all (CRuby's "missing keywords: :a, :b" format).  Called once at the
 * top of the kwargs prologue so the error mentions every missing key
 * instead of just the first encountered. */
static RESULT hash_required_kwargs_check(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_ARRAY) return RESULT_OK(Qnil);
    const struct korb_hash *h = (const struct korb_hash *)self;
    /* Collect missing keys preserving declared order.  `missing` is parked
     * at sp[0] (zero-filled + reserved); the input keys array is re-read from
     * argv[0] (value-stack tracked) each iteration since korb_ary_push is a
     * moving-GC point. */
    sp[0] = 0;
    c->sp_top = sp + 1;
    sp[0] = korb_ary_new(c, sp + 1);
    {
        const struct korb_array *keys0 = (const struct korb_array *)argv[0];
        for (long i = 0; i < (long)keys0->len; i++) {
            keys0 = (const struct korb_array *)argv[0];
            VALUE key = korb_ary_items(keys0)[i];
            uint64_t hh = korb_hash_value(c, key);
            bool found = false;
            for (struct korb_hash_entry *e = h->first; e; e = e->next) {
                if (e->hash == hh && korb_eql(c, e->key, key)) { found = true; break; }
            }
            if (!found) korb_ary_push(c, c->sp_top, sp[0], key);
        }
    }
    c->sp_top = sp;
    struct korb_array *miss_a = (struct korb_array *)sp[0];
    if (miss_a->len == 0) return RESULT_OK(Qnil);
    /* Build "missing keyword(s): :a, :b" message. */
    char buf[1024];
    int off = 0;
    const char *plural = miss_a->len > 1 ? "keywords" : "keyword";
    off += snprintf(buf + off, sizeof(buf) - off, "missing %s: ", plural);
    for (long i = 0; i < (long)miss_a->len && off < (int)sizeof(buf) - 16; i++) {
        const char *kn = SYMBOL_P(korb_ary_items(miss_a)[i]) ? korb_id_name(korb_sym2id(korb_ary_items(miss_a)[i])) : "?";
        off += snprintf(buf + off, sizeof(buf) - off, "%s:%s",
                        i == 0 ? "" : ", ", kn);
    }
    VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
    return korb_raise(c, (struct korb_class *)eArg, "%s", buf);
}

static RESULT hash_fetch(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || argc > 2) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1..2)", argc);
    }
    const struct korb_hash *h = (const struct korb_hash *)self;
    uint64_t hh = korb_hash_value(c, argv[0]);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(c, e->key, argv[0])) return RESULT_OK(e->value);
    }
    /* Not found: priority is block > default arg > KeyError. */
    if (korb_block_given(c)) {
        VALUE r = UNWRAP(korb_yield(c, 1, &argv[0]));
        return RESULT_OK(r);
    }
    if (argc >= 2) return RESULT_OK(argv[1]);
    /* korb_inspect dispatches user #inspect (GC); park the result string and
     * fetch the KeyError class AFTER it (a C-local fetched before would be
     * stale).  ks->ptr stays valid via the parked, forwarded handle. */
    sp[0] = korb_inspect(c, sp + 1, argv[0]);
    c->sp_top = sp + 1;
    VALUE eKey = korb_const_get(KORB_VM(c)->object_class, korb_intern("KeyError"));
    if (UNDEF_P(eKey) || !eKey) eKey = (VALUE)NULL;
    return korb_raise(c, (struct korb_class *)eKey, "key not found: %s", korb_str_cstr(sp[0]));
}

static RESULT hash_delete(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    if (argc < 1) return RESULT_OK(Qnil);
    struct korb_hash *h = (struct korb_hash *)self;
    korb_hash_rehash_identity_if_stale(h);
    VALUE key = argv[0];
    uint64_t hh = h->compare_by_identity ? (uint64_t)key : korb_hash_value(c, key);
    uint32_t b = (uint32_t)(hh % h->bucket_cnt);
    /* Unlink from bucket chain */
    struct korb_hash_entry **slot = &h->buckets[b];
    struct korb_hash_entry *target = NULL;
    while (*slot) {
        if ((*slot)->hash == hh &&
            (h->compare_by_identity ? ((*slot)->key == key) : korb_eql(c, (*slot)->key, key))) {
            target = *slot;
            *slot = target->bucket_next;
            break;
        }
        slot = &(*slot)->bucket_next;
    }
    if (!target) {
        /* Not found: if a block was given, yield key and return its result. */
        if (korb_block_given(c)) {
            VALUE r = UNWRAP(korb_yield(c, 1, &key));
            return RESULT_OK(r);
        }
        return RESULT_OK(Qnil);
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
    return RESULT_OK(target->value);
}

static RESULT hash_eqq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_HASH));
}

/* Hash#== — content comparison, using == on values and eql? on keys. */
static RESULT hash_eq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_HASH) {
        return RESULT_OK(Qfalse);
    }
    struct korb_hash *a = (struct korb_hash *)self;
    struct korb_hash *b = (struct korb_hash *)argv[0];
    if (a == b) return RESULT_OK(Qtrue);
    if (a->size != b->size) return RESULT_OK(Qfalse);
    /* Recursion guard for self-referential hashes. */
    static __thread VALUE eq_stk_a[64];
    static __thread VALUE eq_stk_b[64];
    static __thread int eq_top = 0;
    for (int j = 0; j < eq_top; j++) {
        if (eq_stk_a[j] == self && eq_stk_b[j] == argv[0]) return RESULT_OK(Qtrue);
    }
    if (eq_top < 64) { eq_stk_a[eq_top] = self; eq_stk_b[eq_top] = argv[0]; eq_top++; }
    bool result = true;
    for (struct korb_hash_entry *e = a->first; e; e = e->next) {
        VALUE bv = korb_hash_aref(c, argv[0], e->key);
        /* If bv == default_value, ambiguous — check key existence. */
        if (bv == ((struct korb_hash *)argv[0])->default_value) {
            bool found = false;
            for (struct korb_hash_entry *be = b->first; be; be = be->next) {
                if (be->key == e->key || korb_eql(c, be->key, e->key)) {
                    found = true; bv = be->value; break;
                }
            }
            if (!found) { result = false; break; }
        }
        if (e->value == bv) continue;
        /* Dispatch e->value.==(bv) for user-defined equality. */
        RESULT er = korb_funcall_r(c, c->sp_top, e->value, korb_intern("=="), 1, &bv);
        if (er.state != KORB_NORMAL) { if (eq_top > 0) eq_top--; return er; }
        if (!RTEST(er.value)) { result = false; break; }
    }
    if (eq_top > 0) eq_top--;
    return RESULT_OK(result ? Qtrue : Qfalse);
}

static RESULT hash_dup(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    sp[0] = self;
    return hash_merge(c, 0, sp + 1);
}

/* Hash#clone — like dup but preserves frozen flag (CRuby semantics). */
static RESULT hash_clone(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    sp[0] = self;
    VALUE r = UNWRAP(hash_merge(c, 0, sp + 1));
    if (korb_obj_frozen_p(self) && !SPECIAL_CONST_P(r)) {
        ((struct RBasic *)r)->head.flags |= FL_FROZEN;
    }
    return RESULT_OK(r);
}

static RESULT hash_empty_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(((struct korb_hash *)self)->size == 0));
}

static RESULT hash_map(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("map"));
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    struct korb_hash *h = (struct korb_hash *)self;
    /* Park the result Array (moving) in a synthetic frame across the per-pair
     * korb_yield — an sp[] slot would be collected when yield lowers sp_top
     * below it (see KORB_HASH_YIELD_FRAME). */
    KORB_HASH_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        korb_ary_push(c, c->sp_top, fr.last_line, yr.value);
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

static RESULT hash_select(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* No block: return Enumerator (.to_enum(:select)). */
    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("select"));
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    const struct korb_hash *h = (const struct korb_hash *)self;
    VALUE r = korb_hash_new(c, c->sp_top);
    ((struct korb_hash *)r)->compare_by_identity = h->compare_by_identity;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        VALUE m = UNWRAP(korb_yield(c, 2, args));
        if (RTEST(m)) korb_hash_aset(c, r, e->key, e->value);
    }
    return RESULT_OK(r);
}

/* Hash#partition — yield [k, v] pairs to block; return [[match],[no_match]]
 * each as Arrays of [k,v] pairs (not Hashes — matching CRuby's Enumerable
 * behavior on Hash). */
static RESULT hash_partition(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    const struct korb_hash *h = (const struct korb_hash *)self;
    /* yes / no / pair are moving Arrays held across the per-pair korb_yield.
     * Park them in two chained synthetic frames (the frame chain is always
     * scanned, unlike sp[] slots above the lowered yield sp_top):
     *   fr.last_line  = yes, fr.last_match = no, fr2.last_line = pair. */
    KORB_HASH_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));   /* yes */
    fr.last_match = korb_ary_new(c, c->sp_top);                 /* no */
    KORB_HASH_YIELD_FRAME(c, fr2, Qnil);                        /* pair */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        VALUE m = yr.value;
        fr2.last_line = korb_ary_new_capa(c, c->sp_top, 2);
        korb_ary_push(c, c->sp_top, fr2.last_line, e->key);
        korb_ary_push(c, c->sp_top, fr2.last_line, e->value);
        korb_ary_push(c, c->sp_top, RTEST(m) ? fr.last_line : fr.last_match, fr2.last_line);
    }
    fr2.last_line = korb_ary_new_capa(c, c->sp_top, 2);
    korb_ary_push(c, c->sp_top, fr2.last_line, fr.last_line);
    korb_ary_push(c, c->sp_top, fr2.last_line, fr.last_match);
    VALUE result = fr2.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* Hash#tally — Enumerable's tally returns counts of distinct values.
 * For a hash, counts pairs (which are unique by key already), so returns
 * each pair → 1.  CRuby behaves the same way. */
static RESULT hash_tally(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    const struct korb_hash *h = (const struct korb_hash *)self;
    VALUE r = korb_hash_new(c, c->sp_top);   /* libc hash — non-moving */
    /* pair parked at sp[0]; zero-fill + reserve so it is scanned. */
    sp[0] = 0;
    c->sp_top = sp + 1;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        sp[0] = korb_ary_new_capa(c, sp + 1, 2);
        korb_ary_push(c, sp + 1, sp[0], e->key);
        korb_ary_push(c, sp + 1, sp[0], e->value);
        korb_hash_aset(c, r, sp[0], INT2FIX(1));
    }
    return RESULT_OK(r);
}

/* Hash.new(default = nil) / Hash.new { |h, k| ... }. */
/* Hash[pairs] / Hash[k1,v1,k2,v2,...] / Hash[other_hash] — convert to Hash. */
/* Apply receiver-class to a freshly-created hash.  When self is a
 * Hash subclass, the result should be an instance of that subclass. */
static inline void hash_apply_self_class(CTX *c, VALUE r, VALUE self) {
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_CLASS &&
        self != (VALUE)KORB_VM(c)->hash_class) {
        ((struct RBasic *)r)->klass = self;
    }
}
static RESULT hash_class_aref(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc == 0) {
        VALUE r = korb_hash_new(c, c->sp_top);
        hash_apply_self_class(c, r, sp[-argc - 1]);  /* re-read self: korb_hash_new moved it */
        return RESULT_OK(r);
    }
    if (argc == 1) {
        VALUE arg = argv[0];
        /* Try #to_hash first (CRuby checks before #to_ary). */
        if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) != T_HASH &&
            BUILTIN_TYPE(arg) != T_ARRAY) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, arg, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_hash")) }));
            /* respond_to? is a GC point — re-read arg (still the original
             * argument) before dispatching the coercion on it. */
            arg = argv[0];
            if (RTEST(rt)) {
                VALUE coerced = UNWRAP(korb_funcall(c, c->sp_top, arg, korb_intern("to_hash"), 0, NULL));
                if (!SPECIAL_CONST_P(coerced) && BUILTIN_TYPE(coerced) == T_HASH) {
                    arg = coerced;
                }
            } else {
                VALUE rt2 = UNWRAP(korb_funcall(c, c->sp_top, arg, korb_intern("respond_to?"), 1,
                                         (VALUE[]){ korb_id2sym(korb_intern("to_ary")) }));
                arg = argv[0];
                if (RTEST(rt2)) {
                    VALUE coerced = UNWRAP(korb_funcall(c, c->sp_top, arg, korb_intern("to_ary"), 0, NULL));
                    if (!SPECIAL_CONST_P(coerced) && BUILTIN_TYPE(coerced) == T_ARRAY) {
                        arg = coerced;
                    }
                }
            }
        }
        if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_HASH) {
            VALUE r = korb_hash_new(c, c->sp_top);
            hash_apply_self_class(c, r, sp[-argc - 1]);  /* re-read self: korb_hash_new moved it */
            struct korb_hash *src = (struct korb_hash *)arg;
            for (struct korb_hash_entry *e = src->first; e; e = e->next) {
                korb_hash_aset(c, r, e->key, e->value);
            }
            return RESULT_OK(r);
        }
        if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_ARRAY) {
            /* Hash[ [[k,v], [k,v]] ] form. */
            VALUE r = korb_hash_new(c, c->sp_top);
            hash_apply_self_class(c, r, sp[-argc - 1]);  /* re-read self: korb_hash_new moved it */
            struct korb_array *a = (struct korb_array *)arg;
            for (long i = 0; i < a->len; i++) {
                VALUE pair = korb_ary_items(a)[i];
                if (SPECIAL_CONST_P(pair) || BUILTIN_TYPE(pair) != T_ARRAY) {
                    VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
                    struct korb_class *klass = korb_class_of_class(pair);
                    const char *cls = klass ? korb_id_name(klass->name) : "(unknown)";
                    return korb_raise(c, (struct korb_class *)eA,
                               "wrong element type %s at %ld (expected array)",
                               cls, i);
                }
                struct korb_array *p = (struct korb_array *)pair;
                if (p->len == 1) {
                    korb_hash_aset(c, r, korb_ary_items(p)[0], Qnil);
                } else if (p->len == 2) {
                    korb_hash_aset(c, r, korb_ary_items(p)[0], korb_ary_items(p)[1]);
                } else {
                    VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
                    return korb_raise(c, (struct korb_class *)eA,
                               "invalid number of elements (%ld for 1..2)", p->len);
                }
            }
            return RESULT_OK(r);
        }
    }
    /* Hash[k,v,k,v,...] flat form.  argc must be even. */
    if (argc % 2 != 0) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "odd number of arguments for Hash");
    }
    VALUE r = korb_hash_new(c, c->sp_top);
    hash_apply_self_class(c, r, sp[-argc - 1]);  /* re-read self: korb_hash_new moved it */
    for (int i = 0; i + 1 < argc; i += 2) {
        korb_hash_aset(c, r, argv[i], argv[i+1]);
    }
    return RESULT_OK(r);
}

/* Hash#initialize(default = nil) / Hash#initialize { |h, k| ... } —
 * default_value or default_proc; subclasses can override.  Called by
 * Hash.new after the empty allocation. */
static RESULT hash_initialize(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    int eff_argc = argc;
    /* Peel off trailing FL_KWARGS hash (capacity:). */
    if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH &&
        (RBASIC(argv[argc - 1])->head.flags & FL_KWARGS)) {
        struct korb_hash *kw = (struct korb_hash *)argv[argc - 1];
        VALUE cap_key = korb_id2sym(korb_intern("capacity"));
        for (struct korb_hash_entry *e = kw->first; e; e = e->next) {
            if (!korb_eql(c, e->key, cap_key)) {
                VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
                const char *kn = SYMBOL_P(e->key) ? korb_id_name(korb_sym2id(e->key)) : "?";
                return korb_raise(c, (struct korb_class *)eA,
                           "unknown keyword: :%s", kn);
            }
        }
        eff_argc = argc - 1;
    }
    if (eff_argc > 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0..1)", eff_argc);
    }
    if (c->current_block && eff_argc >= 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given 1, expected 0)");
    }
    struct korb_hash *hh = (struct korb_hash *)self;
    /* Reset both first so calling #initialize again clears them; the
     * appropriate one is then set based on args. */
    hh->default_value = Qnil;
    hh->default_proc = Qnil;
    if (c->current_block) {
        hh->default_proc = (VALUE)c->current_block;
    } else if (eff_argc >= 1) {
        hh->default_value = argv[0];
    }
    return RESULT_OK(self);
}

static RESULT hash_class_new(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;

    /* Allocate an empty hash, retag for subclass, then dispatch
     * initialize (which may be subclass-overridden — CRuby semantics).
     * Re-read self from sp[-argc-1] AFTER alloc since T_CLASS is
     * arena-allocated and can move. */
    VALUE h = korb_hash_new(c, c->sp_top);
    VALUE self = sp[-argc - 1];
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_CLASS) {
        ((struct korb_hash *)h)->basic.klass = self;
    }
    /* See ary_class_new comment: stage [h, argv...] on sp, bump c->sp_top
     * past the staging so the AST dispatcher's zero-fill on return
     * doesn't clobber h, and read back from sp[0] (GC may have moved h). */
    sp[0] = h;
    for (int i = 0; i < argc; i++) sp[1 + i] = argv[i];
    VALUE *prev_sp = c->sp_top;
    c->sp_top = sp + 1 + argc;
    UNWRAP(korb_funcall_r(c, c->sp_top, h, korb_intern("initialize"), argc, sp + 1));
    h = sp[0];
    c->sp_top = prev_sp;
    return RESULT_OK(h);
}

/* Hash#default — the default_value or nil. */
static RESULT hash_default_get(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    /* Hash#default(key) — when default_proc is set and a key is given,
     * call the proc with (self, key); otherwise return default_value. */
    if (!NIL_P(h->default_proc) && argc >= 1) {
        VALUE args[2] = { self, argv[0] };
        return korb_funcall(c, c->sp_top, h->default_proc, korb_intern("call"), 2, args);
    }
    return RESULT_OK(h->default_value);
}

/* Hash#default= — set the default_value, clear default_proc. */
static RESULT hash_default_set(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_hash *h = (struct korb_hash *)self;
    h->default_value = argv[0];
    h->default_proc = Qnil;  /* setting default value clears default_proc */
    return RESULT_OK(argv[0]);
}

/* Hash#default_proc — the default_proc or nil. */
static RESULT hash_default_proc_get(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE v = ((struct korb_hash *)self)->default_proc;
    return RESULT_OK(NIL_P(v) ? Qnil : v);
}

/* Hash#default_proc= — store a Proc as the miss-path resolver. */
static RESULT hash_default_proc_set(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    CHECK_FROZEN_R(c, self);
    VALUE blk = argv[0];
    struct korb_hash *h = (struct korb_hash *)self;
    if (NIL_P(blk)) {
        h->default_proc = Qnil;
        return RESULT_OK(Qnil);
    }
    /* Coerce non-Proc via #to_proc (mock objects, etc.). */
    if (SPECIAL_CONST_P(blk) || BUILTIN_TYPE(blk) != T_PROC) {
        if (!SPECIAL_CONST_P(blk)) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, blk, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_proc")) }));
            if (RTEST(rt)) {
                blk = UNWRAP(korb_funcall(c, c->sp_top, blk, korb_intern("to_proc"), 0, NULL));
            }
        }
        if (SPECIAL_CONST_P(blk) || BUILTIN_TYPE(blk) != T_PROC) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "wrong default_proc type %s (expected Proc)",
                       korb_id_name(korb_class_of_class(argv[0])->name));
        }
    }
    /* Lambda with wrong arity → TypeError ("default_proc takes two
     * arguments (2 for arity)"). */
    struct korb_proc *p = (struct korb_proc *)blk;
    if (p->is_lambda) {
        long ar = (long)p->params_cnt - (long)p->opt_cnt + (long)p->post_cnt;
        if (ar != 2) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "default_proc takes two arguments (2 for %ld)", ar);
        }
    }
    h->default_proc = blk;
    /* CRuby: setting default_proc clears default_value. */
    h->default_value = Qnil;
    return RESULT_OK(argv[0]);
}

/* Hash#clear — empty the hash. */
static RESULT hash_clear(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_hash *h = (struct korb_hash *)self;
    /* Walk buckets and clear chains. */
    for (uint32_t i = 0; i < h->bucket_cnt; i++) h->buckets[i] = NULL;
    h->first = h->last = NULL;
    h->size = 0;
    return RESULT_OK(self);
}

/* Hash#delete_if { |k, v| ... } — destructive reject. */
static RESULT hash_delete_if(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("delete_if"));
        return korb_funcall_r(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    CHECK_FROZEN_R(c, self);
    struct korb_hash *h = (struct korb_hash *)self;
    /* Snapshot keys into a moving Array parked in a synthetic frame
     * (fr.last_line) so it survives the per-key korb_yield (which lowers
     * sp_top below any sp[] slot).  sp[0]/sp[1] stage the hash_delete call
     * (self/key) — reserved only around that call, not across the yield. */
    KORB_HASH_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_ary_push(c, c->sp_top, fr.last_line, e->key);
    }
    long klen = ((struct korb_array *)fr.last_line)->len;
    for (long i = 0; i < klen; i++) {
        VALUE k = korb_ary_items((struct korb_array *)fr.last_line)[i];
        VALUE v = korb_hash_aref(c, self, k);
        VALUE args[2] = {k, v};
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        if (RTEST(yr.value)) {
            sp[0] = self;
            sp[1] = korb_ary_items((struct korb_array *)fr.last_line)[i];
            c->sp_top = sp + 2;
            DROP_RESULT(hash_delete(c, 1, sp + 2));
            c->sp_top = sp;
        }
    }
    c->current_frame = fr.prev;
    return RESULT_OK(self);
}

/* Hash#reject! — like delete_if but returns nil if no entries were
 * removed (CRuby bang semantics). */
static RESULT hash_reject_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* No block: return Enumerator.  Frozen check happens later (CRuby
     * order: enumerator first, frozen check on iteration). */
    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("reject!"));
        return korb_funcall_r(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    CHECK_FROZEN_R(c, self);
    struct korb_hash *h = (struct korb_hash *)self;
    /* keys snapshot parked in fr.last_line (survives korb_yield); sp[0]/sp[1]
     * stage the hash_delete call (self/key) reserved only around that call. */
    KORB_HASH_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_ary_push(c, c->sp_top, fr.last_line, e->key);
    }
    long klen = ((struct korb_array *)fr.last_line)->len;
    bool any_deleted = false;
    for (long i = 0; i < klen; i++) {
        VALUE k = korb_ary_items((struct korb_array *)fr.last_line)[i];
        VALUE v = korb_hash_aref(c, self, k);
        VALUE args[2] = {k, v};
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        if (RTEST(yr.value)) {
            sp[0] = self;
            sp[1] = korb_ary_items((struct korb_array *)fr.last_line)[i];
            c->sp_top = sp + 2;
            DROP_RESULT(hash_delete(c, 1, sp + 2));
            c->sp_top = sp;
            any_deleted = true;
        }
    }
    c->current_frame = fr.prev;
    return RESULT_OK(any_deleted ? self : Qnil);
}

/* Hash#keep_if { |k, v| ... } — opposite of delete_if. */
static RESULT hash_keep_if(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("keep_if"));
        return korb_funcall_r(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    CHECK_FROZEN_R(c, self);
    struct korb_hash *h = (struct korb_hash *)self;
    /* keys snapshot parked in fr.last_line (survives korb_yield); sp[0]/sp[1]
     * stage the hash_delete call (self/key) reserved only around that call. */
    KORB_HASH_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_ary_push(c, c->sp_top, fr.last_line, e->key);
    }
    long klen = ((struct korb_array *)fr.last_line)->len;
    for (long i = 0; i < klen; i++) {
        VALUE k = korb_ary_items((struct korb_array *)fr.last_line)[i];
        VALUE v = korb_hash_aref(c, self, k);
        VALUE args[2] = {k, v};
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        if (!RTEST(yr.value)) {
            sp[0] = self;
            sp[1] = korb_ary_items((struct korb_array *)fr.last_line)[i];
            c->sp_top = sp + 2;
            DROP_RESULT(hash_delete(c, 1, sp + 2));
            c->sp_top = sp;
        }
    }
    c->current_frame = fr.prev;
    return RESULT_OK(self);
}

/* Hash#compact — return a copy with nil values removed. */
static RESULT hash_compact(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new(c, c->sp_top);
    struct korb_hash *rh = (struct korb_hash *)r;
    /* Preserve default value/proc and compare_by_identity (CRuby
     * semantics: compact returns a new Hash with same settings). */
    rh->default_value = h->default_value;
    rh->default_proc = h->default_proc;
    rh->compare_by_identity = h->compare_by_identity;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (!NIL_P(e->value)) korb_hash_aset(c, r, e->key, e->value);
    }
    return RESULT_OK(r);
}

/* Hash#compact! — destructive compact.  Returns nil if no nil values
 * were removed (CRuby semantics: bang methods that didn't change the
 * receiver return nil). */
static RESULT hash_compact_bang(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_hash *h = (struct korb_hash *)self;
    sp[0] = 0; sp[1] = 0; sp[2] = 0;
    c->sp_top = sp + 3;
    sp[0] = korb_ary_new(c, sp + 3);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (NIL_P(e->value)) korb_ary_push(c, sp + 3, sp[0], e->key);
    }
    long klen = ((struct korb_array *)sp[0])->len;
    if (klen == 0) { c->sp_top = sp; return RESULT_OK(Qnil); }
    for (long i = 0; i < klen; i++) {
        sp[1] = self;
        sp[2] = korb_ary_items((struct korb_array *)sp[0])[i];
        DROP_RESULT(hash_delete(c, 1, sp + 3));
    }
    c->sp_top = sp;
    return RESULT_OK(self);
}

/* Hash#values_at(*keys) — array of corresponding values. */
static RESULT hash_values_at(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    sp[0] = 0;
    c->sp_top = sp + 1;
    sp[0] = korb_ary_new(c, sp + 1);
    for (int i = 0; i < argc; i++) korb_ary_push(c, sp + 1, sp[0], korb_hash_aref(c, self, argv[i]));
    return RESULT_OK(sp[0]);
}

/* Hash#fetch_values(*keys) — array of values; raises if any key missing. */
static RESULT hash_fetch_values(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    korb_hash_rehash_identity_if_stale(h);
    /* Result (moving Array) parked in fr.last_line so it survives the block
     * fallback's korb_yield; k staged at sp[0] for that yield (argv). */
    KORB_HASH_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    for (int i = 0; i < argc; i++) {
        VALUE k = argv[i];
        bool found = false;
        VALUE v = Qnil;
        uint64_t hh = h->compare_by_identity ? (uint64_t)k : korb_hash_value(c, k);
        uint32_t b = (uint32_t)(hh % h->bucket_cnt);
        for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
            if (e->hash == hh &&
                (h->compare_by_identity ? e->key == k : korb_eql(c, e->key, k))) {
                found = true; v = e->value; break;
            }
        }
        if (!found) {
            /* Block fallback: yield key for the missing entry, push the
             * block's result.  Otherwise raise KeyError. */
            if (korb_block_given(c)) {
                sp[0] = k;
                c->sp_top = sp + 1;
                RESULT yr = korb_yield(c, 1, &sp[0]);
                if (yr.state != KORB_NORMAL) { c->sp_top = sp; c->current_frame = fr.prev; return yr; }
                c->sp_top = sp;
                korb_ary_push(c, c->sp_top, fr.last_line, yr.value);
                continue;
            }
            VALUE eK = korb_const_get(KORB_VM(c)->object_class, korb_intern("KeyError"));
            c->current_frame = fr.prev;
            return korb_raise(c, eK ? (struct korb_class *)eK : NULL,
                       "key not found");
        }
        korb_ary_push(c, c->sp_top, fr.last_line, v);
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* Hash#reject — non-destructive. */
static RESULT hash_reject(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        VALUE method_sym = korb_id2sym(korb_intern("reject"));
        return korb_funcall(c, c->sp_top, self, korb_intern("to_enum"), 1, &method_sym);
    }
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new(c, c->sp_top);
    struct korb_hash *rh = (struct korb_hash *)r;
    /* Retain compare_by_identity (CRuby semantics). */
    rh->compare_by_identity = h->compare_by_identity;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = {e->key, e->value};
        VALUE drop = UNWRAP(korb_yield(c, 2, args));
        if (!RTEST(drop)) korb_hash_aset(c, r, e->key, e->value);
    }
    return RESULT_OK(r);
}

/* Hash#replace(other) — destructive replace. */
static RESULT hash_replace(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Frozen check fires unconditionally — even if argv would have led
     * to a no-op, CRuby still raises FrozenError on a frozen receiver. */
    CHECK_FROZEN_R(c, self);
    if (argc < 1) return RESULT_OK(self);
    if (self == argv[0]) return RESULT_OK(self);
    /* Coerce non-Hash via #to_hash. */
    VALUE other = argv[0];
    if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_HASH) {
        if (!SPECIAL_CONST_P(other)) {
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, other, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_hash")) }));
            if (RTEST(rt)) {
                other = UNWRAP(korb_funcall(c, c->sp_top, other, korb_intern("to_hash"), 0, NULL));
            }
        }
        if (SPECIAL_CONST_P(other) || BUILTIN_TYPE(other) != T_HASH) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into Hash",
                       SPECIAL_CONST_P(argv[0]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[0])->name));
        }
    }
    CHECK_FROZEN_R(c, self);
    c->sp_top[0] = self;

    DROP_RESULT(hash_clear(c, 0, c->sp_top + 1));
    struct korb_hash *src = (struct korb_hash *)other;
    struct korb_hash *dst = (struct korb_hash *)self;
    /* Transfer compare_by_identity, default_value, default_proc. */
    dst->compare_by_identity = src->compare_by_identity;
    dst->default_value = src->default_value;
    dst->default_proc = src->default_proc;
    for (struct korb_hash_entry *e = src->first; e; e = e->next) {
        korb_hash_aset(c, self, e->key, e->value);
    }
    return RESULT_OK(self);
}

/* Hash#shift — remove and return the first [k, v] pair. */
static RESULT hash_shift(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK_FROZEN_R(c, self);
    struct korb_hash *h = (struct korb_hash *)self;
    if (!h->first) return RESULT_OK(Qnil);
    /* Park k/v at sp[0]/sp[1] across hash_delete and the pair allocation
     * (korb_ary_new_capa/push are moving-GC points); pair at sp[2]. */
    sp[0] = h->first->key;
    sp[1] = h->first->value;
    sp[2] = self;
    sp[3] = sp[0];
    DROP_RESULT(hash_delete(c, 1, sp + 4));
    sp[2] = korb_ary_new_capa(c, sp + 3, 2);
    korb_ary_push(c, sp + 3, sp[2], sp[0]);
    korb_ary_push(c, sp + 3, sp[2], sp[1]);
    return RESULT_OK(sp[2]);
}

/* Hash#slice(*keys) — sub-hash with only the given keys (those that exist). */
static RESULT hash_slice(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    korb_hash_rehash_identity_if_stale(h);
    VALUE r = korb_hash_new(c, c->sp_top);
    struct korb_hash *rh = (struct korb_hash *)r;
    /* CRuby: slice retains the compare_by_identity flag. */
    rh->compare_by_identity = h->compare_by_identity;
    for (int i = 0; i < argc; i++) {
        VALUE k = argv[i];
        uint64_t hh = h->compare_by_identity ? (uint64_t)k : korb_hash_value(c, k);
        uint32_t b = (uint32_t)(hh % h->bucket_cnt);
        for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
            if (e->hash == hh &&
                (h->compare_by_identity ? e->key == k : korb_eql(c, e->key, k))) {
                korb_hash_aset(c, r, e->key, e->value);
                break;
            }
        }
    }
    return RESULT_OK(r);
}

/* Hash#except(*keys) — copy without the given keys. */
static RESULT hash_except(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new(c, c->sp_top);
    struct korb_hash *rh = (struct korb_hash *)r;
    /* CRuby: except retains the compare_by_identity flag. */
    rh->compare_by_identity = h->compare_by_identity;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        bool skip = false;
        for (int i = 0; i < argc; i++) {
            if (korb_eql(c, e->key, argv[i])) { skip = true; break; }
        }
        if (!skip) korb_hash_aset(c, r, e->key, e->value);
    }
    return RESULT_OK(r);
}

/* Hash#count — h.size if no block, else count where block returns truthy. */
static RESULT hash_count(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    
    if (!c->current_block) return RESULT_OK(INT2FIX((long)h->size));
    long n = 0;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = {e->key, e->value};
        VALUE r = UNWRAP(korb_yield(c, 2, args));
        if (RTEST(r)) n++;
    }
    return RESULT_OK(INT2FIX(n));
}

/* Hash#min_by, Hash#max_by — yields [k, v]; finds min/max by block. */
static RESULT hash_min_or_max_by(CTX *c, VALUE self, int argc, VALUE *argv, int max) {
    struct korb_hash *h = (struct korb_hash *)self;
    if (!h->first) return RESULT_OK(Qnil);
    /* best_pair / best_key / current pair / bk are moving handles held across
     * korb_yield + korb_funcall (which lower sp_top below any sp[] slot).
     * Park them in two chained synthetic frames (always scanned):
     *   fr.last_line  = best_pair, fr.last_match = best_key,
     *   fr2.last_line = pair,      fr2.last_match = bk. */
    KORB_HASH_YIELD_FRAME(c, fr, Qnil);   /* best_pair / best_key */
    KORB_HASH_YIELD_FRAME(c, fr2, Qnil);  /* pair / bk */
    bool first = true;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        fr2.last_line = korb_ary_new_capa(c, c->sp_top, 2);
        korb_ary_push(c, c->sp_top, fr2.last_line, e->key);
        korb_ary_push(c, c->sp_top, fr2.last_line, e->value);
        RESULT yr = korb_yield(c, 1, &fr2.last_line);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        fr2.last_match = yr.value;  /* bk */
        if (first) {
            fr.last_line = fr2.last_line;
            fr.last_match = fr2.last_match;
            first = false;
        } else {
            RESULT cr = korb_funcall(c, c->sp_top, fr2.last_match, korb_intern("<=>"), 1, &fr.last_match);
            if (cr.state != KORB_NORMAL) { c->current_frame = fr.prev; return cr; }
            VALUE cmp = cr.value;
            if (FIXNUM_P(cmp)) {
                long cv = FIX2LONG(cmp);
                if ((max && cv > 0) || (!max && cv < 0)) {
                    fr.last_line = fr2.last_line;
                    fr.last_match = fr2.last_match;
                }
            }
        }
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}
static RESULT hash_min_by(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return hash_min_or_max_by(c, self, argc, argv, 0);
}
static RESULT hash_max_by(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return hash_min_or_max_by(c, self, argc, argv, 1);
}

/* Hash#sort — array of [k, v] sorted by [k, v] <=>. With a block,
 * forwards the block to Array#sort so the user comparator participates. */
static RESULT hash_sort(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    /* result=sp[0], pair=sp[1] parked; zero-fill + reserve. */
    sp[0] = 0; sp[1] = 0;
    c->sp_top = sp + 2;
    sp[0] = korb_ary_new(c, sp + 2);          /* result */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        sp[1] = korb_ary_new_capa(c, sp + 2, 2);   /* pair */
        korb_ary_push(c, sp + 2, sp[1], e->key);
        korb_ary_push(c, sp + 2, sp[1], e->value);
        korb_ary_push(c, sp + 2, sp[0], sp[1]);
    }
    if (korb_block_given(c)) {
        return korb_funcall_with_block(c, c->sp_top, sp[0], korb_intern("sort"), 0, NULL,
                                        (VALUE)c->current_block);
    }
    return korb_funcall(c, c->sp_top, sp[0], korb_intern("sort"), 0, NULL);
}

/* Hash#deconstruct_keys — pattern-match support hook.  Spec: takes one
 * argument (an Array of keys, or nil to mean "all keys"); returns self.
 * koruby ignores the keys arg and just returns self.  Validates argc. */
static RESULT hash_deconstruct_keys(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc != 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 1)", argc);
    }
    return RESULT_OK(self);
}

static RESULT hash_reduce(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    /* acc + pair are moving handles held across korb_yield.  Park acc in
     * fr.last_line and pair in fr.last_match (frame chain always scanned);
     * the yield argv is a fresh contiguous snapshot built each iteration. */
    KORB_HASH_YIELD_FRAME(c, fr, argc > 0 ? argv[0] : Qnil);   /* acc */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        fr.last_match = korb_ary_new_capa(c, c->sp_top, 2);   /* pair */
        korb_ary_push(c, c->sp_top, fr.last_match, e->key);
        korb_ary_push(c, c->sp_top, fr.last_match, e->value);
        VALUE args[2] = { fr.last_line, fr.last_match };
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        fr.last_line = yr.value;   /* acc */
    }
    VALUE acc = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(acc);
}

/* ---------- Hash#dig ----------
 * h.dig(k1, k2, ...) — equivalent to h[k1][k2]..., short-circuiting on
 * nil and dispatching #dig on intermediates so Hash/Array chains compose. */
static RESULT hash_dig(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) {
        VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eArg, "wrong number of arguments to dig (0 for 1+)");
    }
    VALUE first = korb_hash_aref(c, self, argv[0]);
    if (UNDEF_P(first)) first = Qnil;
    if (argc == 1) return RESULT_OK(first);
    if (NIL_P(first)) return RESULT_OK(Qnil);
    /* Intermediate must respond to #dig — else TypeError. */
    VALUE next_klass = (VALUE)korb_class_of_class(first);
    if (!next_klass || !korb_class_find_method((struct korb_class *)next_klass,
                                                 korb_intern("dig"))) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "%s does not have #dig method",
                   korb_id_name(korb_class_of_class(first)->name));
    }
    return korb_funcall(c, c->sp_top, first, korb_intern("dig"), argc - 1, argv + 1);
}

/* ---------- Hash#has_value? / value? ---------- */
static RESULT hash_has_value_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (korb_eq(c, e->value, argv[0])) return RESULT_OK(Qtrue);
    }
    return RESULT_OK(Qfalse);
}

/* ---------- Hash#group_by ----------
 * Bins [k, v] pairs under whatever the block returns. */
static RESULT hash_group_by(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new(c, c->sp_top);   /* libc hash — non-moving */
    /* pair / key / bucket are moving handles held across korb_yield.  Park
     * them in two chained synthetic frames (always scanned):
     *   fr.last_line = pair, fr.last_match = key, fr2.last_line = bucket. */
    KORB_HASH_YIELD_FRAME(c, fr, Qnil);    /* pair / key */
    KORB_HASH_YIELD_FRAME(c, fr2, Qnil);   /* bucket */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        fr.last_line = korb_ary_new_capa(c, c->sp_top, 2);   /* pair */
        korb_ary_push(c, c->sp_top, fr.last_line, e->key);
        korb_ary_push(c, c->sp_top, fr.last_line, e->value);
        VALUE args[2] = { e->key, e->value };
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        fr.last_match = yr.value;   /* key */
        fr2.last_line = korb_hash_aref(c, r, fr.last_match);   /* bucket */
        if (UNDEF_P(fr2.last_line) || NIL_P(fr2.last_line)) {
            fr2.last_line = korb_ary_new(c, c->sp_top);
            korb_hash_aset(c, r, fr.last_match, fr2.last_line);
        }
        korb_ary_push(c, c->sp_top, fr2.last_line, fr.last_line);
    }
    c->current_frame = fr.prev;
    return RESULT_OK(r);
}

/* ---------- Hash#sort_by ----------
 * Materialize [k, v] pairs + sort-keys, insertion-sort, return ordered
 * pair list.  Hash sizes encountered here are small enough that O(n^2)
 * is fine. */
static RESULT hash_sort_by(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    /* pairs / keys / pair / k are moving handles held across korb_yield +
     * korb_funcall.  Park them in two chained synthetic frames (always
     * scanned): fr.last_line = pairs, fr.last_match = keys,
     * fr2.last_line = pair, fr2.last_match = k.  pa/ka are re-derived from
     * fr.last_line / fr.last_match each access since korb_funcall may move
     * them. */
    KORB_HASH_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));   /* pairs */
    fr.last_match = korb_ary_new(c, c->sp_top);                 /* keys */
    KORB_HASH_YIELD_FRAME(c, fr2, Qnil);                        /* pair / k */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        fr2.last_line = korb_ary_new_capa(c, c->sp_top, 2);   /* pair */
        korb_ary_push(c, c->sp_top, fr2.last_line, e->key);
        korb_ary_push(c, c->sp_top, fr2.last_line, e->value);
        VALUE args[2] = { e->key, e->value };
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        fr2.last_match = yr.value;   /* k */
        korb_ary_push(c, c->sp_top, fr.last_line, fr2.last_line);
        korb_ary_push(c, c->sp_top, fr.last_match, fr2.last_match);
    }
    long klen = ((struct korb_array *)fr.last_match)->len;
    for (long i = 1; i < klen; i++) {
        long j = i;
        while (j > 0) {
            struct korb_array *ka = (struct korb_array *)fr.last_match;
            RESULT cr = korb_funcall(c, c->sp_top, korb_ary_items(ka)[j], korb_intern("<=>"), 1, &korb_ary_items(ka)[j-1]);
            if (cr.state != KORB_NORMAL) { c->current_frame = fr.prev; return cr; }
            if (!FIXNUM_P(cr.value) || FIX2LONG(cr.value) >= 0) break;
            ka = (struct korb_array *)fr.last_match;
            struct korb_array *pa = (struct korb_array *)fr.last_line;
            VALUE tk = korb_ary_items(ka)[j]; korb_ary_items(ka)[j] = korb_ary_items(ka)[j-1]; korb_ary_items(ka)[j-1] = tk;
            VALUE tp = korb_ary_items(pa)[j]; korb_ary_items(pa)[j] = korb_ary_items(pa)[j-1]; korb_ary_items(pa)[j-1] = tp;
            j--;
        }
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* ---------- Hash#filter_map ---------- */
static RESULT hash_filter_map(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    /* Result (moving Array) parked in fr.last_line across the per-pair
     * korb_yield (frame chain always scanned). */
    KORB_HASH_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        if (RTEST(yr.value)) korb_ary_push(c, c->sp_top, fr.last_line, yr.value);
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* ---------- Hash#sum ----------
 * Yields (k, v) and sums the block's return values onto an
 * accumulator (default 0).  Without a block, attempts +-aggregation
 * over [k, v] pairs (CRuby's behavior, may raise on Symbol+Integer). */
static RESULT hash_sum(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    /* acc + addend are moving handles held across korb_yield + korb_funcall.
     * Park acc in fr.last_line, addend in fr.last_match (always scanned). */
    KORB_HASH_YIELD_FRAME(c, fr, argc >= 1 ? argv[0] : INT2FIX(0));   /* acc */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (korb_block_given(c)) {
            VALUE args[2] = { e->key, e->value };
            RESULT yr = korb_yield(c, 2, args);
            if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
            fr.last_match = yr.value;   /* addend */
        } else {
            fr.last_match = korb_ary_new_capa(c, c->sp_top, 2);   /* addend */
            korb_ary_push(c, c->sp_top, fr.last_match, e->key);
            korb_ary_push(c, c->sp_top, fr.last_match, e->value);
        }
        RESULT ar = korb_funcall(c, c->sp_top, fr.last_line, korb_intern("+"), 1, &fr.last_match);
        if (ar.state != KORB_NORMAL) { c->current_frame = fr.prev; return ar; }
        fr.last_line = ar.value;   /* acc */
    }
    VALUE acc = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(acc);
}

/* ---------- Hash#each_with_object ----------
 * Yields ([k, v], memo) and returns memo at the end. */
static RESULT hash_each_with_object(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    struct korb_hash *h = (struct korb_hash *)self;
    /* memo + pair are moving handles held across korb_yield.  Park memo in
     * fr.last_line and pair in fr.last_match (always scanned); the yield argv
     * (pair, memo) is a fresh contiguous snapshot built each iteration. */
    KORB_HASH_YIELD_FRAME(c, fr, argv[0]);   /* memo */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        fr.last_match = korb_ary_new_capa(c, c->sp_top, 2);   /* pair */
        korb_ary_push(c, c->sp_top, fr.last_match, e->key);
        korb_ary_push(c, c->sp_top, fr.last_match, e->value);
        VALUE args[2] = { fr.last_match, fr.last_line };   /* pair, memo */
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
    }
    VALUE memo = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(memo);
}

/* ---------- Hash#take(n) ---------- */
static RESULT hash_take(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(korb_ary_new(c, c->sp_top));
    long n = FIX2LONG(argv[0]);
    struct korb_hash *h = (struct korb_hash *)self;
    /* out=sp[0], pair=sp[1] parked; zero-fill + reserve. */
    sp[0] = 0; sp[1] = 0;
    c->sp_top = sp + 2;
    sp[0] = korb_ary_new(c, sp + 2);   /* out */
    long taken = 0;
    for (struct korb_hash_entry *e = h->first; e && taken < n; e = e->next, taken++) {
        sp[1] = korb_ary_new_capa(c, sp + 2, 2);   /* pair, parked */
        korb_ary_push(c, sp + 2, sp[1], e->key);
        korb_ary_push(c, sp + 2, sp[1], e->value);
        korb_ary_push(c, sp + 2, sp[0], sp[1]);
    }
    return RESULT_OK(sp[0]);
}

/* ---------- Hash#flat_map ----------
 * Yields (k, v); flattens one level into the result. */
static RESULT hash_flat_map(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_hash *h = (struct korb_hash *)self;
    /* r + m are moving handles held across korb_yield.  Park r in
     * fr.last_line, m in fr.last_match (always scanned).  m's items are
     * re-derived from fr.last_match each inner iteration since the inner
     * korb_ary_push is a moving-GC point. */
    KORB_HASH_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        RESULT yr = korb_yield(c, 2, args);
        if (yr.state != KORB_NORMAL) { c->current_frame = fr.prev; return yr; }
        fr.last_match = yr.value;   /* m */
        if (!SPECIAL_CONST_P(fr.last_match) && BUILTIN_TYPE(fr.last_match) == T_ARRAY) {
            long mlen = ((struct korb_array *)fr.last_match)->len;
            for (long i = 0; i < mlen; i++) {
                korb_ary_push(c, c->sp_top, fr.last_line, korb_ary_items((struct korb_array *)fr.last_match)[i]);
            }
        } else {
            korb_ary_push(c, c->sp_top, fr.last_line, fr.last_match);
        }
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

