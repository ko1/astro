/* koruby_precise — marshal_fast.c: Marshal.load in C for plain data.
 * #included into korb_runtime.c's TU.
 *
 * prelude/marshal.rb is the full loader (user classes, _load / marshal_load,
 * extended modules, procs, freeze:, IO sources, every error message).  Data
 * files that gems ship (phonelib's 2.8 MB of Hashes of Strings, …) only use
 * nil / true / false / Integer / Float / String (+ encoding) / Symbol / Array /
 * Hash and back-references, and interpreting the Ruby loader over them was 26×
 * CRuby.  __marshal_fast_load decodes exactly that subset; on anything else —
 * including malformed input — it reports "not mine" and the Ruby loader starts
 * over from the first byte, so its errors and semantics stay the reference.
 *
 * GC: the input bytes are copied out once (the String may move).  Every object
 * the stream registers goes into the links Array at *links (rooted by the
 * caller) the moment it exists — that is Marshal's own `@` table — so a
 * container under construction is re-read from there after each allocation. */

struct korb_mload {
    const unsigned char *p;
    size_t len, i;
    uint32_t *syms;
    uint32_t nsym, symcap;
};

#define KORB_MLOAD_MAX_DEPTH 512u

/* Not handled here: the caller falls back to the Ruby loader. */
#define KORB_MLOAD_NO() RESULT_OK(KORB_UNDEF)

static bool korb_mload_byte(struct korb_mload *const st, unsigned char *const out) {
    if (st->i >= st->len) return false;
    *out = st->p[st->i++];
    return true;
}

/* Marshal's packed long (w_long / r_long). */
static bool korb_mload_long(struct korb_mload *const st, int64_t *const out) {
    unsigned char b;
    if (!korb_mload_byte(st, &b)) return false;
    const int c = (signed char)b;
    if (c == 0) { *out = 0; return true; }
    if (c > 0) {
        if (c > 4) { *out = c - 5; return true; }
        int64_t x = 0;
        for (int k = 0; k < c; k++) {
            if (!korb_mload_byte(st, &b)) return false;
            x |= (int64_t)b << (8 * k);
        }
        *out = x;
        return true;
    }
    if (c < -4) { *out = c + 5; return true; }
    int64_t x = -1;
    for (int k = 0; k < -c; k++) {
        if (!korb_mload_byte(st, &b)) return false;
        x &= ~((int64_t)0xff << (8 * k));
        x |= (int64_t)b << (8 * k);
    }
    *out = x;
    return true;
}

/* A length-prefixed byte run, left in place. */
static bool korb_mload_bytes(struct korb_mload *const st, const unsigned char **const p, uint32_t *const n) {
    int64_t len;
    if (!korb_mload_long(st, &len) || len < 0 || (uint64_t)len > st->len - st->i) return false;
    *p = st->p + st->i;
    *n = (uint32_t)len;
    st->i += (size_t)len;
    return true;
}

static bool korb_mload_sym_push(struct korb_mload *const st, const uint32_t id) {
    if (st->nsym == st->symcap) {
        const uint32_t nc = st->symcap ? st->symcap * 2 : 64;
        uint32_t *const ns = realloc(st->syms, nc * sizeof *ns);
        if (!ns) return false;
        st->syms = ns; st->symcap = nc;
    }
    st->syms[st->nsym++] = id;
    return true;
}

/* ':' (plain) or ';' (link) — the symbol id, or false. */
static bool korb_mload_symbol(CTX *c, struct korb_mload *const st, uint32_t *const id) {
    unsigned char t;
    if (!korb_mload_byte(st, &t)) return false;
    if (t == ';') {
        int64_t k;
        if (!korb_mload_long(st, &k) || k < 0 || (uint64_t)k >= st->nsym) return false;
        *id = st->syms[k];
        return true;
    }
    if (t != ':') return false;
    const unsigned char *p; uint32_t n;
    if (!korb_mload_bytes(st, &p, &n)) return false;
    for (uint32_t k = 0; k < n; k++) if (p[k] >= 0x80) return false;   /* a non-ASCII name comes wrapped in 'I' */
    *id = korb_intern(c->vm, (const char *)p, n);
    return korb_mload_sym_push(st, *id);
}

/* Register v in the links table (Marshal's object numbering). */
static RESULT korb_mload_entry(CTX *c, VALUE *slots, VALUE *const links, const VALUE v) {
    slots[0] = v;
    CHECK(korb_ary_push_val(c, slots + 1, VALUE_REF_AT(links), slots[0]));
    return RESULT_OK(slots[0]);
}

/* One object into *out.  slots[0..3] are this level's; a child uses slots+4. */
static RESULT
korb_mload_obj(CTX *c, VALUE *slots, VALUE *const links, struct korb_mload *const st, const uint32_t depth, VALUE *const out)
{
    if (depth > KORB_MLOAD_MAX_DEPTH) return KORB_MLOAD_NO();
    unsigned char t;
    if (!korb_mload_byte(st, &t)) return KORB_MLOAD_NO();
    switch (t) {
      case '0': *out = KORB_NIL; return RESULT_OK(KORB_NIL);
      case 'T': *out = KORB_TRUE; return RESULT_OK(KORB_NIL);
      case 'F': *out = KORB_FALSE; return RESULT_OK(KORB_NIL);
      case 'i': {
        int64_t x;
        if (!korb_mload_long(st, &x)) return KORB_MLOAD_NO();
        *out = LONG2FIX((korb_sword_t)x);
        return RESULT_OK(KORB_NIL);
      }
      case ':': case ';': {
        st->i--;
        uint32_t id;
        if (!korb_mload_symbol(c, st, &id)) return KORB_MLOAD_NO();
        *out = ID2SYM(id);
        return RESULT_OK(KORB_NIL);
      }
      case '@': {
        int64_t k;
        if (!korb_mload_long(st, &k) || k < 0 || (uint64_t)k >= VAL2ARY(*links)->len) return KORB_MLOAD_NO();
        *out = korb_items_data(VAL2ARY(*links)->items)[k];
        return RESULT_OK(KORB_NIL);
      }
      case '"': {
        const unsigned char *p; uint32_t n;
        if (!korb_mload_bytes(st, &p, &n)) return KORB_MLOAD_NO();
        const VALUE s = UNWRAP(korb_str_new(c, slots, (const char *)p, n));   /* st->p is libc memory: no GC hazard */
        KORB_STR_ENC_SET(s, KORB_ENC_BINARY);
        *out = UNWRAP(korb_mload_entry(c, slots, links, s));
        return RESULT_OK(KORB_NIL);
      }
      case 'f': {
        const unsigned char *p; uint32_t n;
        if (!korb_mload_bytes(st, &p, &n) || n == 0 || n >= 64) return KORB_MLOAD_NO();
        char buf[64];
        memcpy(buf, p, n); buf[n] = '\0';            /* old dumps append mantissa bytes after a NUL: strtod stops there */
        double d;
        if (strcmp(buf, "inf") == 0) d = HUGE_VAL;
        else if (strcmp(buf, "-inf") == 0) d = -HUGE_VAL;
        else if (strcmp(buf, "nan") == 0) d = NAN;
        else {
            char *end;
            d = strtod(buf, &end);
            if (end == buf) return KORB_MLOAD_NO();
        }
        const VALUE f = UNWRAP(korb_float_new(c, slots, d));
        *out = UNWRAP(korb_mload_entry(c, slots, links, f));
        return RESULT_OK(KORB_NIL);
      }
      case 'I': {                                    /* a String / Symbol followed by its encoding ivar */
        unsigned char inner;
        if (!korb_mload_byte(st, &inner)) return KORB_MLOAD_NO();
        const unsigned char *p; uint32_t n;
        if ((inner != '"' && inner != ':') || !korb_mload_bytes(st, &p, &n)) return KORB_MLOAD_NO();
        VALUE s = KORB_NIL;
        uint32_t li = 0, si = 0;
        if (inner == ':') {                          /* its symbol slot comes before the ivar's `:E` (r_symreal) */
            si = st->nsym;
            if (!korb_mload_sym_push(st, 0)) return KORB_MLOAD_NO();
        }
        if (inner == '"') {
            s = UNWRAP(korb_str_new(c, slots, (const char *)p, n));
            li = VAL2ARY(*links)->len;
            UNWRAP(korb_mload_entry(c, slots, links, s));
        }
        int64_t nivar;
        if (!korb_mload_long(st, &nivar) || nivar != 1) return KORB_MLOAD_NO();   /* only the encoding, nothing else */
        uint32_t key;
        if (!korb_mload_symbol(c, st, &key)) return KORB_MLOAD_NO();
        uint32_t enc;
        unsigned char vt;
        if (!korb_mload_byte(st, &vt)) return KORB_MLOAD_NO();
        const char *const kname = korb_sym_name(c->vm, key);
        if (kname != NULL && strcmp(kname, "E") == 0 && (vt == 'T' || vt == 'F')) {
            enc = (vt == 'T') ? KORB_ENC_UTF8 : KORB_ENC_USASCII;
        } else if (kname != NULL && strcmp(kname, "encoding") == 0 && vt == '"') {
            const unsigned char *ep; uint32_t en;
            if (!korb_mload_bytes(st, &ep, &en) || en == 0 || en >= 64) return KORB_MLOAD_NO();
            char ename[64];
            memcpy(ename, ep, en); ename[en] = '\0';
            enc = korb_enc_index_pub(c->vm, ename);
            /* the name String is numbered in the `@` table too (a repeat is a link) */
            const VALUE nm = UNWRAP(korb_str_new(c, slots, ename, en));
            UNWRAP(korb_mload_entry(c, slots, links, nm));
        } else {
            return KORB_MLOAD_NO();
        }
        if (inner == ':') {
            if (enc != KORB_ENC_UTF8) return KORB_MLOAD_NO();
            const uint32_t id = korb_intern_enc(c->vm, (const char *)p, n, KORB_ENC_UTF8);
            st->syms[si] = id;
            *out = ID2SYM(id);
            return RESULT_OK(KORB_NIL);
        }
        s = korb_items_data(VAL2ARY(*links)->items)[li];   /* re-read: the allocations above may have moved it */
        KORB_STR_ENC_SET(s, enc);
        *out = s;
        return RESULT_OK(KORB_NIL);
      }
      case '[': {
        int64_t n;
        if (!korb_mload_long(st, &n) || n < 0 || (uint64_t)n > st->len - st->i) return KORB_MLOAD_NO();
        const uint32_t li = VAL2ARY(*links)->len;
        const VALUE a = UNWRAP(korb_ary_new(c, slots, (uint32_t)n));
        UNWRAP(korb_mload_entry(c, slots, links, a));
        for (int64_t k = 0; k < n; k++) {
            VALUE e;
            const RESULT r = korb_mload_obj(c, slots + 4, links, st, depth + 1, &e);
            if (r.state != KORB_NORMAL || r.value == KORB_UNDEF) return r;
            slots[1] = e;
            slots[2] = korb_items_data(VAL2ARY(*links)->items)[li];
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
        }
        *out = korb_items_data(VAL2ARY(*links)->items)[li];
        return RESULT_OK(KORB_NIL);
      }
      case '{': case '}': {
        int64_t n;
        if (!korb_mload_long(st, &n) || n < 0 || (uint64_t)n > st->len - st->i) return KORB_MLOAD_NO();
        const uint32_t li = VAL2ARY(*links)->len;
        const VALUE h = UNWRAP(korb_hash_new(c, slots, (uint32_t)n));
        UNWRAP(korb_mload_entry(c, slots, links, h));
        for (int64_t k = 0; k < n; k++) {
            VALUE kv, vv;
            RESULT r = korb_mload_obj(c, slots + 4, links, st, depth + 1, &kv);
            if (r.state != KORB_NORMAL || r.value == KORB_UNDEF) return r;
            slots[1] = kv;
            r = korb_mload_obj(c, slots + 4, links, st, depth + 1, &vv);
            if (r.state != KORB_NORMAL || r.value == KORB_UNDEF) return r;
            if (!korb_key_indexable(slots[1]) && !FLONUM_P(slots[1]) && !KORB_FLOAT_P(slots[1]))
                return KORB_MLOAD_NO();                  /* a key whose #hash Ruby would run: leave it to the Ruby loader */
            slots[2] = vv;
            slots[3] = korb_items_data(VAL2ARY(*links)->items)[li];
            CHECK(korb_hash_set(c, slots + 4, VALUE_REF_AT(&slots[3]), VALUE_REF_AT(&slots[1]), slots[2]));
        }
        if (t == '}') {
            VALUE dv;
            const RESULT r = korb_mload_obj(c, slots + 4, links, st, depth + 1, &dv);
            if (r.state != KORB_NORMAL || r.value == KORB_UNDEF) return r;
            KorbHash *const hh = VAL2HASH(korb_items_data(VAL2ARY(*links)->items)[li]);
            ARO_STORE(c, hh, (VALUE *)(uintptr_t)&hh->default_val, dv);
        }
        *out = korb_items_data(VAL2ARY(*links)->items)[li];
        return RESULT_OK(KORB_NIL);
      }
      default:
        return KORB_MLOAD_NO();
    }
}

/* __marshal_fast_load(str) → [obj], or nil when the stream needs the Ruby
 * loader (anything beyond plain data, or malformed). */
static RESULT
korb_bi_marshal_fast_load(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    const VALUE src = VALUE_SLICE_GET(args, 0);
    if (!KORB_STRING_P(src)) return RESULT_OK(KORB_NIL);
    const KorbString *const s = VAL2STR(src);
    if (s->len < 3) return RESULT_OK(KORB_NIL);
    const unsigned char *const d0 = (const unsigned char *)korb_strbuf_data(s->buf);
    if (d0[0] != 4 || d0[1] != 8) return RESULT_OK(KORB_NIL);   /* version errors are the Ruby loader's */
    unsigned char *const copy = malloc(s->len);
    if (!copy) return RESULT_OK(KORB_NIL);
    memcpy(copy, d0, s->len);
    struct korb_mload st = { .p = copy, .len = s->len, .i = 2, .syms = NULL, .nsym = 0, .symcap = 0 };
    RESULT r = korb_ary_new(c, slots, 64);
    if (r.state == KORB_NORMAL) {
        slots[0] = r.value;
        VALUE top = KORB_NIL;
        r = korb_mload_obj(c, slots + 1, &slots[0], &st, 0, &top);
        if (r.state == KORB_NORMAL && r.value != KORB_UNDEF) {
            slots[1] = top;
            r = korb_ary_new(c, slots + 2, 1);
            if (r.state == KORB_NORMAL) {
                slots[2] = r.value;
                r = korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]);
                if (r.state == KORB_NORMAL) r = RESULT_OK(slots[2]);
            }
        } else if (r.state == KORB_NORMAL) {
            r = RESULT_OK(KORB_NIL);
        }
    }
    free(st.syms);
    free(copy);
    return r;
}
