/* ENV — a hash-like view of the process environment (getenv/setenv/environ).
 * Defined as a module (like Math) whose singleton methods back `ENV[...]`.
 * Included into korb_runtime.c after math.c, so it shares its static helpers
 * (korb_def_modfunc, korb_str_cstr_len, UNWRAP/CHECK, ...). */

extern char **environ;

/* arg → a NUL-terminated env name; raises TypeError on a non-String. */
static const char *korb_env_name(CTX *c, VALUE *slots, VALUE v, RESULT *err) {
    if (UNLIKELY(!KORB_STRING_P(v))) {
        *err = korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(v));
        return NULL;
    }
    err->state = KORB_NORMAL;
    uint32_t len; return korb_str_cstr_len(v, &len);
}

/* ENV[name] → the value String, or nil. */
static RESULT korb_m_env_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *name = korb_env_name(c, slots, VALUE_SLICE_GET(a, 0), &err);
    if (!name) return err;
    const char *v = getenv(name);
    return v ? korb_str_new(c, slots, v, (uint32_t)strlen(v)) : RESULT_OK(KORB_NIL);
}

/* ENV[name] = value / ENV.store(name, value) — nil value deletes.  Returns value. */
static RESULT korb_m_env_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *name = korb_env_name(c, slots, VALUE_SLICE_GET(a, 0), &err);
    if (!name) return err;
    const VALUE val = VALUE_SLICE_GET(a, 1);
    if (val == KORB_NIL) { unsetenv(name); return RESULT_OK(KORB_NIL); }
    if (UNLIKELY(!KORB_STRING_P(val)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(val));
    uint32_t vlen; const char *v = korb_str_cstr_len(val, &vlen);
    setenv(name, v, 1);
    return RESULT_OK(val);
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
    return korb_raise(c, slots, KORB_E_KEY, 0, "key not found: \"%s\"", name);
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
    intptr_t n = 0; for (char **e = environ; *e; e++) n++;
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
    korb_const_define(c, korb_intern(c->vm, "$$", 2), LONG2FIX((intptr_t)getpid()));   /* process id */
    /* $LOAD_PATH / $: — the require search path (one Array shared by both names). */
    slots[1] = korb_ary_new(c, slots + 1, 0).value;
    korb_const_define(c, korb_intern(c->vm, "$LOAD_PATH", 10), slots[1]);
    korb_const_define(c, korb_intern(c->vm, "$:", 2), slots[1]);
    slots[1] = korb_ary_new(c, slots + 1, 0).value;                                    /* $" / $LOADED_FEATURES */
    korb_const_define(c, korb_intern(c->vm, "$\"", 2), slots[1]);
    korb_const_define(c, korb_intern(c->vm, "$LOADED_FEATURES", 16), slots[1]);
}

/* ENV.merge!/update(*hashes) [{ |key, old, new| }] → set each pair, block resolves
 * conflicts for existing keys; returns ENV. */
static RESULT korb_m_env_merge_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        slots[0] = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_HASH_P(slots[0]))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(slots[0]));
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(slots[0]);
            if (i >= h->len) break;
            slots[1] = h->items->data[2 * i];       /* key   (rooted) */
            slots[2] = h->items->data[2 * i + 1];   /* value (rooted) */
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
    ENVR("to_h", to_h, 0);       ENVR("to_hash", to_h, 0);
    ENVB("each", each, 0);       ENVB("each_pair", each, 0);
    ENVR("delete", delete, 1);
    ENVB("merge!", merge_bang, -1);   ENVB("update", merge_bang, -1);
    ENVR("size", size, 0);       ENVR("length", size, 0);
    ENVR("empty?", empty_p, 0);
    ENVR("value?", value_p, 1);  ENVR("has_value?", value_p, 1);
    ENVR("to_s", to_s, 0);       ENVR("inspect", to_s, 0);
    ENVR("class", class, 0);
#undef ENVR
#undef ENVB
}
