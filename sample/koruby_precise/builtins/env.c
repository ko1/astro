/* ENV — a hash-like view of the process environment (getenv/setenv/environ).
 * Defined as a module (like Math) whose singleton methods back `ENV[...]`.
 * Included into korb_runtime.c after math.c, so it shares its static helpers
 * (korb_def_modfunc, korb_str_cstr_len, UNWRAP/CHECK, ...). */

extern char **environ;

/* arg → a NUL-terminated env name, coercing with #to_str.  The (possibly
 * coerced) String is parked in slots[park] so the borrowed pointer stays valid
 * across the rest of the caller. */
ARO_BORROW static const char *korb_env_name_at(CTX *c, VALUE *slots, uint32_t park, VALUE v, RESULT *err) {
    if (UNLIKELY(!KORB_STRING_P(v))) {
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (AROH_IS_GC_OBJECT(v) && korb_responds_to(c, v, to_str)) {
            slots[park] = v;
            const RESULT r = korb_send(c, slots + park + 1, to_str, 0, 0);
            if (UNLIKELY(r.state != KORB_NORMAL)) { *err = r; return NULL; }
            v = r.value;
        }
        if (UNLIKELY(!KORB_STRING_P(v))) {
            *err = korb_raise(c, slots + park, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(v));
            return NULL;
        }
    }
    slots[park] = v;                                  /* root: the pointer below borrows its bytes */
    err->state = KORB_NORMAL;
    uint32_t len; return korb_str_cstr_len(slots[park], &len);
}
ARO_BORROW static const char *korb_env_name(CTX *c, VALUE *slots, VALUE v, RESULT *err) {
    return korb_env_name_at(c, slots, 0, v, err);
}

/* ENV[name] → the value String, or nil. */
static RESULT korb_m_env_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *name = korb_env_name(c, slots, VALUE_SLICE_GET(a, 0), &err);
    if (!name) return err;
    const char *v = getenv(name);
    if (!v) return RESULT_OK(KORB_NIL);
    const RESULT r = korb_str_new(c, slots + 1, v, (uint32_t)strlen(v));
    if (LIKELY(r.state == KORB_NORMAL))
        ((AroObjectHeader *)(uintptr_t)r.value)->flags |= KORB_FL_FROZEN;   /* CRuby: ENV values are frozen */
    return r;
}

/* ENV[name] = value / ENV.store(name, value) — nil value deletes.  Returns value. */
static RESULT korb_m_env_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *name = korb_env_name(c, slots, VALUE_SLICE_GET(a, 0), &err);
    if (!name) return err;
    if (UNLIKELY(strchr(name, '=') != NULL))
        return korb_raise_errno(c, slots + 2, EINVAL, "setenv", name);   /* CRuby: Errno::EINVAL */
    const VALUE val = VALUE_SLICE_GET(a, 1);
    if (val == KORB_NIL) { unsetenv(name); return RESULT_OK(KORB_NIL); }
    RESULT verr; const char *const v = korb_env_name_at(c, slots, 1, val, &verr);
    if (!v) return verr;
    name = korb_str_cstr_len(slots[0], &(uint32_t){0});   /* re-borrow: the value coercion may have GC'd */
    setenv(name, v, 1);
    return RESULT_OK(slots[1]);
}

/* ENV.key?(name) and its aliases. */
static RESULT korb_m_env_key_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *name = korb_env_name(c, slots, VALUE_SLICE_GET(a, 0), &err);
    if (!name) return err;
    return RESULT_OK(getenv(name) ? KORB_TRUE : KORB_FALSE);
}

/* ENV.fetch(name[, default]) { |name| ... } — value, else block/default, else KeyError. */
static RESULT korb_m_env_fetch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                               struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)self;
    RESULT err; const char *name = korb_env_name(c, slots, VALUE_SLICE_GET(a, 0), &err);
    if (!name) return err;
    const char *v = getenv(name);
    if (v) return korb_str_new(c, slots, v, (uint32_t)strlen(v));
    if (block != NULL) { slots[0] = VALUE_SLICE_GET(a, 0); return korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self); }
    if (VALUE_SLICE_LEN(a) >= 2) return RESULT_OK(VALUE_SLICE_GET(a, 1));
    char msg[512]; snprintf(msg, sizeof msg, "key not found: \"%s\"", name);   /* KeyError w/ #receiver = ENV, #key */
    return korb_raise_key(c, slots, korb_const_get(c->vm, korb_intern(c->vm, "ENV", 3)), VALUE_SLICE_GET(a, 0), msg);
}

/* split environ entry "K=V" — returns key len via *klen, value pointer via *val. */
static const char *korb_env_split(const char *e, uint32_t *klen, const char **val) {
    const char *eq = strchr(e, '=');
    if (!eq) { *klen = (uint32_t)strlen(e); *val = e + strlen(e); return e; }
    *klen = (uint32_t)(eq - e); *val = eq + 1; return e;
}

/* ENV.keys / ENV.values (want_val selects). */
static RESULT korb_env_collect(CTX *c, VALUE *slots, bool want_val) {
    slots[0] = UNWRAP(korb_ary_new(c, slots, 16));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; const char *key = korb_env_split(*e, &klen, &val);
        slots[1] = want_val ? UNWRAP(korb_str_new(c, slots + 1, val, (uint32_t)strlen(val)))
                            : UNWRAP(korb_str_new(c, slots + 1, key, klen));
        CHECK(korb_ary_push_val(c, slots + 2, arr, slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(arr));
}
static RESULT korb_m_env_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)self; (void)a; return korb_env_collect(c, slots, false); }
static RESULT korb_m_env_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; (void)a; return korb_env_collect(c, slots, true); }

/* ENV.to_h → { "K" => "V", ... }. */
static RESULT korb_m_env_to_h(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, 16));
    VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; const char *key = korb_env_split(*e, &klen, &val);
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, key, klen));
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, val, (uint32_t)strlen(val)));
        CHECK(korb_hash_set(c, slots + 3, h, VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(h));
}
/* ENV.to_h { |k, v| [k2, v2] } → a Hash built from the block's returned pairs. */
static RESULT korb_m_env_to_h_blk(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, struct Node *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) return korb_m_env_to_h(c, slots, self, a);
    slots[0] = UNWRAP(korb_hash_new(c, slots, 16));
    VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; const char *key = korb_env_split(*e, &klen, &val);
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, key, klen));
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, val, (uint32_t)strlen(val)));
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[1], 2, cself);   /* yield(k, v) as separate args */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[1] = r.value;                              /* the returned pair (rooted) */
        if (!KORB_ARRAY_P(slots[1])) {                   /* coerce via #to_ary only (not #to_a) */
            VALUE pv = slots[1];
            if (KORB_OBJECT_P(pv) && korb_responds_to_coerce_p(c, slots + 3, &pv, korb_intern(c->vm, "to_ary", 6))) {
                slots[3] = pv;
                RESULT ar = korb_send_impl(c, slots + 4, korb_intern(c->vm, "to_ary", 6), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
                slots[1] = ar.value;
            }
            if (UNLIKELY(!KORB_ARRAY_P(slots[1]))) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong element type %s (expected array)", korb_type_name(r.value));
        }
        if (UNLIKELY(VAL2ARY(slots[1])->len != 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "element has wrong array length (expected 2, was %u)", VAL2ARY(slots[1])->len);
        slots[2] = korb_items_data(VAL2ARY(slots[1])->items)[0]; slots[3] = korb_items_data(VAL2ARY(slots[1])->items)[1];
        CHECK(korb_hash_set(c, slots + 4, h, VALUE_REF_AT(&slots[2]), slots[3]));
    }
    return RESULT_OK(VALUE_REF_GET(h));
}

/* ENV.each / ENV.each_pair { |k, v| ... } → ENV. */
static RESULT korb_m_env_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                              struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));   /* (no Enumerator yet) */
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; const char *key = korb_env_split(*e, &klen, &val);
        slots[0] = UNWRAP(korb_str_new(c, slots, key, klen));
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, val, (uint32_t)strlen(val)));
        VALUE argv[2] = { slots[0], slots[1] };
        CHECK(korb_block_yield(c, slots + 2, block, def_env, argv, 2, captured_self));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* ENV.delete(name) → the removed value String, or nil. */
static RESULT korb_m_env_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *name0 = korb_env_name(c, slots, VALUE_SLICE_GET(a, 0), &err);
    if (!name0) return err;
    /* name0 points into the (movable) arg String; copy it — korb_str_new below
     * GCs and may relocate that String, but unsetenv still needs the name. */
    char name[1024]; const size_t nl = strlen(name0);
    if (nl >= sizeof(name)) return RESULT_OK(KORB_NIL);   /* absurdly long → not a live env name */
    memcpy(name, name0, nl + 1);
    const char *v = getenv(name);
    if (!v) return RESULT_OK(KORB_NIL);
    slots[0] = UNWRAP(korb_str_new(c, slots, v, (uint32_t)strlen(v)));
    unsetenv(name);
    return RESULT_OK(slots[0]);
}

static RESULT korb_m_env_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)self; (void)a;
    korb_sword_t n = 0; for (char **e = environ; *e; e++) n++;
    return RESULT_OK(LONG2FIX(n));
}
static RESULT korb_m_env_empty_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)self; (void)a;
    return RESULT_OK((environ && *environ) ? KORB_FALSE : KORB_TRUE);
}

/* ENV.value?(v) / has_value?(v). */
static RESULT korb_m_env_value_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE vv = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(vv)) return RESULT_OK(KORB_FALSE);
    uint32_t wlen; const char *want = korb_str_cstr_len(vv, &wlen);
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; korb_env_split(*e, &klen, &val);
        if (strlen(val) == wlen && memcmp(val, want, wlen) == 0) return RESULT_OK(KORB_TRUE);
    }
    (void)slots; return RESULT_OK(KORB_FALSE);
}

/* ENV.to_s / ENV.inspect. */
static RESULT korb_m_env_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a; return korb_str_new(c, slots, "ENV", 3);
}
/* CRuby's ENV is a plain Object (with singleton methods), so ENV.class is Object,
 * not the internal module used here for method dispatch. */
static RESULT korb_m_env_class(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)self; (void)a;
    return RESULT_OK(korb_builtin_class_obj(c->vm, KORB_C_OBJECT));
}

/* Define the top-level ARGV array (the script's command-line arguments) and $0.
 * Called from main once the args are known. */
void korb_define_argv(CTX *c, int n, char *const *args, const char *prog) {
    VALUE *slots = c->slots;
    slots[0] = korb_ary_new(c, slots, (uint32_t)(n > 0 ? n : 1)).value;
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    for (int k = 0; k < n; k++) {
        slots[1] = korb_str_new(c, slots + 1, args[k], (uint32_t)strlen(args[k])).value;
        (void)korb_ary_push_val(c, slots + 2, arr, slots[1]);
    }
    korb_const_define(c, korb_intern(c->vm, "ARGV", 4), VALUE_REF_GET(arr));
    if (prog) {   /* $0 / $PROGRAM_NAME (globals share the const table with a '$' name) */
        slots[1] = korb_str_new(c, slots + 1, prog, (uint32_t)strlen(prog)).value;
        korb_const_define(c, korb_intern(c->vm, "$0", 2), slots[1]);
        korb_const_define(c, korb_intern(c->vm, "$PROGRAM_NAME", 13), slots[1]);
    }
    korb_const_define(c, korb_intern(c->vm, "$$", 2), LONG2FIX((korb_sword_t)getpid()));   /* process id */
    /* $LOAD_PATH / $: — the require search path (one Array shared by both names). */
    slots[1] = korb_ary_new(c, slots + 1, 0).value;
    /* bundled pure-Ruby stdlib: `require 'delegate'` etc. resolve to koruby's
     * own lib/ regardless of CWD (compiled-in absolute src dir). */
    {
        static const char lib_dir[] = KORUBY_SRC_DIR "/lib";
        slots[2] = korb_str_new(c, slots + 2, lib_dir, (uint32_t)(sizeof lib_dir - 1)).value;
        korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[1]), slots[2]);
    }
    korb_const_define(c, korb_intern(c->vm, "$LOAD_PATH", 10), slots[1]);
    korb_const_define(c, korb_intern(c->vm, "$:", 2), slots[1]);
    slots[1] = korb_ary_new(c, slots + 1, 0).value;                                    /* $" / $LOADED_FEATURES */
    korb_const_define(c, korb_intern(c->vm, "$\"", 2), slots[1]);
    korb_const_define(c, korb_intern(c->vm, "$LOADED_FEATURES", 16), slots[1]);
}

/* ENV.select/filter (sel=true) / reject (sel=false) { |k,v| } → a Hash of the
 * (non-)matching pairs.  Iterates a snapshot so the block may mutate ENV. */
static RESULT korb_m_env_selrej(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool sel) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    slots[0] = UNWRAP(korb_hash_new(c, slots, 16));           /* result */
    slots[1] = UNWRAP(korb_m_env_to_h(c, slots + 1, self, VALUE_SLICE_MAKE(NULL, 0)));   /* snapshot */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *snap = VAL2HASH(slots[1]);
        if (i >= snap->len) break;
        slots[2] = korb_items_data(snap->items)[2 * i];
        slots[3] = korb_items_data(snap->items)[2 * i + 1];
        VALUE argv[2] = { slots[2], slots[3] };
        RESULT yr = korb_block_yield(c, slots + 4, block, def_env, argv, 2, cself);
        if (UNLIKELY(yr.state != KORB_NORMAL)) return yr;
        if (KORB_TRUTHY(yr.value) == sel) CHECK(korb_hash_set(c, slots + 4, VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[2]), slots[3]));
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_env_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_m_env_selrej(c, slots, self, a, block, def_env, cself, true); }
static RESULT korb_m_env_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_m_env_selrej(c, slots, self, a, block, def_env, cself, false); }
/* ENV.keep_if/select! (keep=true) / delete_if/reject! (keep=false) — mutating.
 * bang variants return nil when nothing changed. */
static RESULT korb_m_env_keepdel(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool keep, bool bang) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    slots[0] = UNWRAP(korb_m_env_to_h(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));       /* snapshot */
    bool changed = false;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *snap = VAL2HASH(slots[0]);
        if (i >= snap->len) break;
        slots[1] = korb_items_data(snap->items)[2 * i];
        slots[2] = korb_items_data(snap->items)[2 * i + 1];
        VALUE argv[2] = { slots[1], slots[2] };
        RESULT yr = korb_block_yield(c, slots + 3, block, def_env, argv, 2, cself);
        if (UNLIKELY(yr.state != KORB_NORMAL)) return yr;
        const bool remove = keep ? !KORB_TRUTHY(yr.value) : KORB_TRUTHY(yr.value);
        if (remove) {
            RESULT er; const char *name = korb_env_name(c, slots + 3, slots[1], &er);
            if (!name) return er;
            char nm[1024]; size_t nl = strlen(name); if (nl >= sizeof nm) nl = sizeof nm - 1;
            memcpy(nm, name, nl); nm[nl] = '\0';
            unsetenv(nm); changed = true;
        }
    }
    if (bang) return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_env_keep_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)   { return korb_m_env_keepdel(c, slots, self, a, block, def_env, cself, true, false); }
static RESULT korb_m_env_delete_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_m_env_keepdel(c, slots, self, a, block, def_env, cself, false, false); }
static RESULT korb_m_env_select_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_m_env_keepdel(c, slots, self, a, block, def_env, cself, true, true); }
static RESULT korb_m_env_reject_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_m_env_keepdel(c, slots, self, a, block, def_env, cself, false, true); }
/* ENV.merge!/update(*hashes) [{ |key, old, new| }] → set each pair, block resolves
 * conflicts for existing keys; returns ENV. */
static RESULT korb_m_env_merge_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        slots[0] = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_HASH_P(slots[0]))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(slots[0]));
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(slots[0]);
            if (i >= h->len) break;
            slots[1] = korb_items_data(h->items)[2 * i];       /* key   (rooted) */
            slots[2] = korb_items_data(h->items)[2 * i + 1];   /* value (rooted) */
            if (block != NULL) {                    /* conflict → yield(key, old, new) when the key already exists */
                RESULT er; const char *name = korb_env_name(c, slots + 3, slots[1], &er);
                if (!name) return er;
                const char *old = getenv(name);
                if (old != NULL) {
                    slots[3] = UNWRAP(korb_str_new(c, slots + 4, old, (uint32_t)strlen(old)));
                    VALUE argv[3] = { slots[1], slots[3], slots[2] };
                    RESULT yr = korb_block_yield(c, slots + 4, block, def_env, argv, 3, cself);
                    if (UNLIKELY(yr.state != KORB_NORMAL)) return yr;
                    slots[2] = yr.value;
                }
            }
            VALUE pair[2] = { slots[1], slots[2] };
            CHECK(korb_m_env_aset(c, slots + 3, self, VALUE_SLICE_MAKE(pair, 2)));
        }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* ENV.each_key { |k| } / each_value { |v| } → ENV (want_key selects which). */
static RESULT korb_m_env_each_kv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool want_key) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; const char *key = korb_env_split(*e, &klen, &val);
        slots[0] = want_key ? UNWRAP(korb_str_new(c, slots, key, klen))
                            : UNWRAP(korb_str_new(c, slots, val, (uint32_t)strlen(val)));
        CHECK(korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_env_each_key(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)   { return korb_m_env_each_kv(c, slots, self, a, block, def_env, cself, true); }
static RESULT korb_m_env_each_value(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_m_env_each_kv(c, slots, self, a, block, def_env, cself, false); }
/* ENV.key(value) → the first key whose value == value, or nil. */
static RESULT korb_m_env_key(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE want = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(want))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(want));
    const KorbString *ws = VAL2STR(want);
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; const char *key = korb_env_split(*e, &klen, &val);
        if (strlen(val) == ws->len && memcmp(val, korb_strbuf_data(ws->buf), ws->len) == 0)
            return korb_str_new(c, slots, key, klen);
    }
    return RESULT_OK(KORB_NIL);
}
/* ENV.assoc(key) → [key, value] or nil. */
static RESULT korb_m_env_assoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT er; const char *name0 = korb_env_name(c, slots, VALUE_SLICE_GET(a, 0), &er);
    if (!name0) return er;
    char nm[1024]; size_t nl = strlen(name0); if (nl >= sizeof nm) nl = sizeof nm - 1; memcpy(nm, name0, nl); nm[nl] = '\0';
    const char *v = getenv(nm);
    if (!v) return RESULT_OK(KORB_NIL);
    slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)nl));
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, v, (uint32_t)strlen(v)));
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(slots[2]);
}
/* ENV.rassoc(value) → [key, value] of the first entry with that value, or nil. */
static RESULT korb_m_env_rassoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE want = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(want)) return RESULT_OK(KORB_NIL);
    const KorbString *ws = VAL2STR(want);
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; const char *key = korb_env_split(*e, &klen, &val);
        if (strlen(val) == ws->len && memcmp(val, korb_strbuf_data(ws->buf), ws->len) == 0) {
            slots[0] = UNWRAP(korb_str_new(c, slots, key, klen));
            slots[1] = UNWRAP(korb_str_new(c, slots + 1, val, (uint32_t)strlen(val)));
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
            return RESULT_OK(slots[2]);
        }
    }
    return RESULT_OK(KORB_NIL);
}
/* ENV.invert → { value => key } Hash. */
static RESULT korb_m_env_invert(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, 16));
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; const char *key = korb_env_split(*e, &klen, &val);
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, val, (uint32_t)strlen(val)));   /* new key = value */
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, key, klen));                    /* new value = key */
        CHECK(korb_hash_set(c, slots + 3, VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[0]);
}
/* ENV.values_at(*keys) → [ENV[k], ...] (nil for missing). */
static RESULT korb_m_env_values_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = UNWRAP(korb_ary_new(c, slots, VALUE_SLICE_LEN(a)));
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE one = VALUE_SLICE_GET(a, i);
        RESULT r = korb_m_env_aref(c, slots + 1, self, VALUE_SLICE_MAKE(&one, 1));
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[1] = r.value;
        CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[0]), slots[1]));
    }
    return RESULT_OK(slots[0]);
}
/* ENV.slice(*keys) → { k => ENV[k] } for present keys. */
static RESULT korb_m_env_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = UNWRAP(korb_hash_new(c, slots, VALUE_SLICE_LEN(a)));
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE one = VALUE_SLICE_GET(a, i);
        RESULT r = korb_m_env_aref(c, slots + 1, self, VALUE_SLICE_MAKE(&one, 1));
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (r.value == KORB_NIL) continue;
        slots[1] = one; slots[2] = r.value;
        CHECK(korb_hash_set(c, slots + 3, VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[0]);
}
/* ENV.except(*keys) → to_h minus the given keys. */
static RESULT korb_m_env_except(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    slots[0] = UNWRAP(korb_hash_new(c, slots, 16));
    for (char **e = environ; *e; e++) {
        uint32_t klen; const char *val; const char *key = korb_env_split(*e, &klen, &val);
        bool excluded = false;                              /* skip keys named in the args */
        for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
            const VALUE one = VALUE_SLICE_GET(a, i);
            if (KORB_STRING_P(one) && VAL2STR(one)->len == klen && memcmp(korb_strbuf_data(VAL2STR(one)->buf), key, klen) == 0) { excluded = true; break; }
        }
        if (excluded) continue;
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, key, klen));
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, val, (uint32_t)strlen(val)));
        CHECK(korb_hash_set(c, slots + 3, VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[0]);
}
/* ENV.clear → remove every variable, returns ENV. */
static RESULT korb_m_env_clear(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = UNWRAP(korb_m_env_to_h(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));   /* snapshot the keys */
    const KorbHash *snap = VAL2HASH(slots[0]);
    for (uint32_t i = 0; i < snap->len; i++) {
        const VALUE key = korb_items_data(snap->items)[2 * i];
        if (!KORB_STRING_P(key)) continue;
        char nm[1024]; uint32_t kl; const char *kc = korb_str_cstr_len(key, &kl); if (kl >= sizeof nm) kl = sizeof nm - 1;
        memcpy(nm, kc, kl); nm[kl] = '\0'; unsetenv(nm);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* ENV.replace(hash) → clear ENV, then set every pair from hash; returns ENV. */
static RESULT korb_m_env_replace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE hv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_HASH_P(hv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(hv));
    { RESULT cr = korb_m_env_clear(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
    slots[0] = hv;                                           /* root the source hash */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(slots[0]);
        if (i >= h->len) break;
        slots[1] = korb_items_data(h->items)[2 * i];
        slots[2] = korb_items_data(h->items)[2 * i + 1];
        VALUE pair[2] = { slots[1], slots[2] };
        CHECK(korb_m_env_aset(c, slots + 3, self, VALUE_SLICE_MAKE(pair, 2)));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
void korb_init_env(CTX *c, VALUE *slots) {
    struct korb_vm *const vm = c->vm;
    slots[0] = (korb_class_new(c, slots, korb_intern(vm, "ENV", 3), KORB_NIL)).value;
    VAL2CLASS(slots[0])->is_module = 1;
    korb_const_define(c, korb_intern(vm, "ENV", 3), slots[0]);
    const VALUE env_sing = korb_obj_singleton(c, slots + 1, slots[0]).value;
#define ENVR(nm, fn, ar)  korb_class_def_cfn(c, env_sing, nm, korb_m_env_##fn, ar)
#define ENVB(nm, fn, ar)  korb_class_def_cfn_blk(c, env_sing, nm, korb_m_env_##fn, ar)
    ENVR("[]", aref, 1);         ENVR("aref", aref, 1);
    ENVR("[]=", aset, 2);        ENVR("store", aset, 2);
    ENVR("key?", key_p, 1);      ENVR("has_key?", key_p, 1);
    ENVR("include?", key_p, 1);  ENVR("member?", key_p, 1);
    ENVB("fetch", fetch, -1);
    ENVR("keys", keys, 0);       ENVR("values", values, 0);
    ENVB("to_h", to_h_blk, 0);   ENVR("to_hash", to_h, 0);
    ENVB("each", each, 0);       ENVB("each_pair", each, 0);
    ENVR("delete", delete, 1);
    ENVB("merge!", merge_bang, -1);   ENVB("update", merge_bang, -1);
    ENVB("select", select, 0);        ENVB("filter", select, 0);
    ENVB("reject", reject, 0);
    ENVB("keep_if", keep_if, 0);      ENVB("delete_if", delete_if, 0);
    ENVB("select!", select_bang, 0);  ENVB("filter!", select_bang, 0);
    ENVB("reject!", reject_bang, 0);
    ENVR("assoc", assoc, 1);          ENVR("rassoc", rassoc, 1);
    ENVR("invert", invert, 0);        ENVR("values_at", values_at, -1);
    ENVR("slice", slice, -1);         ENVR("except", except, -1);
    ENVR("clear", clear, 0);      ENVR("replace", replace, 1);
    ENVB("each_key", each_key, 0);    ENVB("each_value", each_value, 0);
    ENVR("key", key, 1);
    ENVR("size", size, 0);       ENVR("length", size, 0);
    ENVR("empty?", empty_p, 0);
    ENVR("value?", value_p, 1);  ENVR("has_value?", value_p, 1);
    ENVR("to_s", to_s, 0);       ENVR("inspect", to_s, 0);
    ENVR("class", class, 0);
#undef ENVR
#undef ENVB
}
