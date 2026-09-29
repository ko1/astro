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
static RESULT korb_mload_entry(CTX *c, korb_value *slots, korb_value *const links, const korb_value v) {
    slots[0] = v;
    CHECK(korb_ary_push_val(c, slots + 1, KORB_REF_AT(links), slots[0]));
    return RESULT_OK(slots[0]);
}

/* One object into *out.  slots[0..3] are this level's; a child uses slots+4. */
static RESULT
korb_mload_obj(CTX *c, korb_value *slots, korb_value *const links, struct korb_mload *const st, const uint32_t depth, korb_value *const out)
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
        const korb_value s = UNWRAP(korb_str_new(c, slots, (const char *)p, n));   /* st->p is libc memory: no GC hazard */
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
        const korb_value f = UNWRAP(korb_float_new(c, slots, d));
        *out = UNWRAP(korb_mload_entry(c, slots, links, f));
        return RESULT_OK(KORB_NIL);
      }
      case 'I': {                                    /* a String / Symbol followed by its encoding ivar */
        unsigned char inner;
        if (!korb_mload_byte(st, &inner)) return KORB_MLOAD_NO();
        const unsigned char *p; uint32_t n;
        if ((inner != '"' && inner != ':') || !korb_mload_bytes(st, &p, &n)) return KORB_MLOAD_NO();
        korb_value s = KORB_NIL;
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
            const korb_value nm = UNWRAP(korb_str_new(c, slots, ename, en));
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
        const korb_value a = UNWRAP(korb_ary_new(c, slots, (uint32_t)n));
        UNWRAP(korb_mload_entry(c, slots, links, a));
        for (int64_t k = 0; k < n; k++) {
            korb_value e;
            const RESULT r = korb_mload_obj(c, slots + 4, links, st, depth + 1, &e);
            if (r.state != KORB_NORMAL || r.value == KORB_UNDEF) return r;
            slots[1] = e;
            slots[2] = korb_items_data(VAL2ARY(*links)->items)[li];
            CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
        }
        *out = korb_items_data(VAL2ARY(*links)->items)[li];
        return RESULT_OK(KORB_NIL);
      }
      case '{': case '}': {
        int64_t n;
        if (!korb_mload_long(st, &n) || n < 0 || (uint64_t)n > st->len - st->i) return KORB_MLOAD_NO();
        const uint32_t li = VAL2ARY(*links)->len;
        const korb_value h = UNWRAP(korb_hash_new(c, slots, (uint32_t)n));
        UNWRAP(korb_mload_entry(c, slots, links, h));
        for (int64_t k = 0; k < n; k++) {
            korb_value kv, vv;
            RESULT r = korb_mload_obj(c, slots + 4, links, st, depth + 1, &kv);
            if (r.state != KORB_NORMAL || r.value == KORB_UNDEF) return r;
            slots[1] = kv;
            r = korb_mload_obj(c, slots + 4, links, st, depth + 1, &vv);
            if (r.state != KORB_NORMAL || r.value == KORB_UNDEF) return r;
            if (!korb_key_indexable(slots[1]) && !FLONUM_P(slots[1]) && !KORB_FLOAT_P(slots[1]))
                return KORB_MLOAD_NO();                  /* a key whose #hash Ruby would run: leave it to the Ruby loader */
            slots[2] = vv;
            slots[3] = korb_items_data(VAL2ARY(*links)->items)[li];
            CHECK(korb_hash_set(c, slots + 4, KORB_REF_AT(&slots[3]), KORB_REF_AT(&slots[1]), slots[2]));
        }
        if (t == '}') {
            korb_value dv;
            const RESULT r = korb_mload_obj(c, slots + 4, links, st, depth + 1, &dv);
            if (r.state != KORB_NORMAL || r.value == KORB_UNDEF) return r;
            KorbHash *const hh = VAL2HASH(korb_items_data(VAL2ARY(*links)->items)[li]);
            ARO_STORE(c, hh, (korb_value *)(uintptr_t)&hh->default_val, dv);
        }
        *out = korb_items_data(VAL2ARY(*links)->items)[li];
        return RESULT_OK(KORB_NIL);
      }
      case 'o': {                                        /* a plain object of a class the dumper / loader vetted */
        uint32_t cid;
        if (!korb_mload_symbol(c, st, &cid)) return KORB_MLOAD_NO();
        const korb_value k = korb_const_get_path(c->vm, cid);
        if (!KORB_CLASS_P(k) || !VAL2CLASS(k)->marshal_plain || VAL2CLASS(k)->is_singleton) return KORB_MLOAD_NO();
        int64_t n;
        if (!korb_mload_long(st, &n) || n < 0 || (uint64_t)n > st->len - st->i) return KORB_MLOAD_NO();
        const uint32_t li = VAL2ARY(*links)->len;
        const korb_value o = UNWRAP(korb_obj_new(c, slots, k));
        UNWRAP(korb_mload_entry(c, slots, links, o));
        for (int64_t j = 0; j < n; j++) {
            uint32_t iv;
            if (!korb_mload_symbol(c, st, &iv)) return KORB_MLOAD_NO();
            const char *const nm = korb_sym_name(c->vm, iv);
            korb_value v;
            const RESULT r = korb_mload_obj(c, slots + 4, links, st, depth + 1, &v);
            if (r.state != KORB_NORMAL || r.value == KORB_UNDEF) return r;
            if (nm == NULL || nm[0] != '@') continue;         /* CRuby keeps it as a hidden ivar */
            slots[1] = v;
            slots[2] = korb_items_data(VAL2ARY(*links)->items)[li];
            CHECK(korb_ivar_set(c, slots + 3, KORB_REF_AT(&slots[2]), ID2SYM(iv), slots[1]));
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
korb_bi_marshal_fast_load(CTX *c, korb_value *slots, korb_slice args)
{
    const korb_value src = KORB_SLICE_GET(args, 0);
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
        korb_value top = KORB_NIL;
        r = korb_mload_obj(c, slots + 1, &slots[0], &st, 0, &top);
        if (r.state == KORB_NORMAL && r.value != KORB_UNDEF) {
            slots[1] = top;
            r = korb_ary_new(c, slots + 2, 1);
            if (r.state == KORB_NORMAL) {
                slots[2] = r.value;
                r = korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]);
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

/* ---- Marshal.dump in C for the same plain subset ----------------------------
 * Mirrors prelude/marshal.rb's dumper byte for byte (link ids pre-order, the
 * symbol table, `:E` for UTF-8 / US-ASCII, the float digits).  Anything else —
 * a subclass, extended / singleton'd or ivar-carrying object, a Hash by identity
 * or with a default proc, a heap Bignum, a non-ASCII Symbol, another encoding —
 * makes it answer nil and the Ruby dumper runs from scratch.  Nothing is
 * allocated on the GC heap while walking, so object addresses (the link keys)
 * cannot move under it; the result String is made at the very end. */

struct korb_mdump {
    unsigned char *b;
    size_t len, cap;
    korb_value *okey; uint32_t *oval; uint32_t ocap, ocnt;   /* object → link id (identity) */
    uint32_t *skey, *sval; uint32_t scap, scnt;         /* symbol id + 1 → symlink id */
    korb_value ckey[16]; uint32_t cval[16]; uint32_t ccnt;   /* class → qualified-name symbol (nothing moves mid-dump) */
    uint32_t mid_mdump, mid_udump;                      /* :marshal_dump / :_dump */
};

#define KORB_MDUMP_MAX_DEPTH 4096u

static bool korb_mdump_put(struct korb_mdump *const st, const void *const p, const size_t n) {
    if (st->len + n > st->cap) {
        size_t nc = st->cap ? st->cap * 2 : 256;
        while (nc < st->len + n) nc *= 2;
        unsigned char *const nb = realloc(st->b, nc);
        if (!nb) return false;
        st->b = nb; st->cap = nc;
    }
    memcpy(st->b + st->len, p, n);
    st->len += n;
    return true;
}
static bool korb_mdump_byte(struct korb_mdump *const st, const unsigned char c) { return korb_mdump_put(st, &c, 1); }

/* Marshal's packed long (w_long). */
static bool korb_mdump_long(struct korb_mdump *const st, const int64_t n) {
    if (n == 0) return korb_mdump_byte(st, 0);
    if (n > 0 && n < 123) return korb_mdump_byte(st, (unsigned char)(n + 5));
    if (n < 0 && n > -124) return korb_mdump_byte(st, (unsigned char)((n - 5) & 0xff));
    unsigned char buf[9]; int k = 0;
    int64_t v = n;
    if (n > 0) { while (v > 0) { buf[1 + k++] = (unsigned char)(v & 0xff); v >>= 8; } buf[0] = (unsigned char)k; }
    else       { while (v < -1) { buf[1 + k++] = (unsigned char)(v & 0xff); v >>= 8; } buf[0] = (unsigned char)((-k) & 0xff); }
    return korb_mdump_put(st, buf, (size_t)k + 1);
}

/* Object addresses are aligned: take the multiply's HIGH bits, the low ones barely mix. */
static inline uint32_t korb_mdump_hash(const korb_value v) { return (uint32_t)(((uint64_t)v * 0x9E3779B97F4A7C15ULL) >> 32); }

/* Link id of `v`, or UINT32_MAX after registering it (the next id). */
static uint32_t korb_mdump_link(struct korb_mdump *const st, const korb_value v, bool *const oom) {
    if (st->ocnt * 2 >= st->ocap) {
        const uint32_t nc = st->ocap ? st->ocap * 2 : 64;
        korb_value *const nk = calloc(nc, sizeof(korb_value));
        uint32_t *const nv = malloc(sizeof(uint32_t) * nc);
        if (!nk || !nv) { free(nk); free(nv); *oom = true; return 0; }
        for (uint32_t i = 0; i < st->ocap; i++) if (st->okey[i]) {
            uint32_t h = korb_mdump_hash(st->okey[i]) & (nc - 1);
            while (nk[h]) h = (h + 1) & (nc - 1);
            nk[h] = st->okey[i]; nv[h] = st->oval[i];
        }
        free(st->okey); free(st->oval);
        st->okey = nk; st->oval = nv; st->ocap = nc;
    }
    uint32_t h = korb_mdump_hash(v) & (st->ocap - 1);
    while (st->okey[h]) {
        if (st->okey[h] == v) return st->oval[h];
        h = (h + 1) & (st->ocap - 1);
    }
    st->okey[h] = v; st->oval[h] = st->ocnt++;
    return UINT32_MAX;
}

/* A Symbol (ASCII names only): `:` + bytes the first time, `;` + id after. */
static bool korb_mdump_sym(const struct korb_vm *const vm, struct korb_mdump *const st, const uint32_t id) {
    for (uint32_t i = 0; i < st->scnt; i++)
        if (st->skey[i] == id) return korb_mdump_byte(st, ';') && korb_mdump_long(st, st->sval[i]);
    const char *const nm = korb_sym_name(vm, id);
    const uint32_t n = vm->sym_lens[id];
    for (uint32_t i = 0; i < n; i++) if ((unsigned char)nm[i] >= 0x80) return false;
    if (st->scnt == st->scap) {
        st->scap = st->scap ? st->scap * 2 : 16;
        uint32_t *const nk = realloc(st->skey, sizeof(uint32_t) * st->scap);
        if (!nk) return false;
        st->skey = nk;
        uint32_t *const nv = realloc(st->sval, sizeof(uint32_t) * st->scap);
        if (!nv) return false;
        st->sval = nv;
    }
    st->skey[st->scnt] = id; st->sval[st->scnt] = st->scnt; st->scnt++;
    return korb_mdump_byte(st, ':') && korb_mdump_long(st, n) && korb_mdump_put(st, nm, n);
}

/* CRuby's w_float digits: the shortest round-tripping digit string placed by its
 * decimal point (prelude Marshal._float_str). */
static uint32_t korb_mdump_float_str(const double d, char *const out) {
    if (isinf(d)) { strcpy(out, d < 0 ? "-inf" : "inf"); return (uint32_t)strlen(out); }
    if (isnan(d)) { strcpy(out, "nan"); return 3; }
    if (d == 0.0) { strcpy(out, signbit(d) ? "-0" : "0"); return (uint32_t)strlen(out); }
    char tmp[48];
    int sig = 1;
    for (; sig < 17; sig++) {
        snprintf(tmp, sizeof tmp, "%.*e", sig - 1, d);
        if (strtod(tmp, NULL) == d) break;
    }
    if (sig == 17) snprintf(tmp, sizeof tmp, "%.*e", 16, d);
    const char *p = tmp;
    const bool neg = (*p == '-');
    if (neg) p++;
    char digits[24]; int nd = 0;
    for (; *p && *p != 'e'; p++) if (*p != '.') digits[nd++] = *p;
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    const int decpt = atoi(p + 1) + 1;
    char *o = out;
    if (neg) *o++ = '-';
    if (decpt < -3 || decpt > nd) {
        *o++ = digits[0];
        if (nd > 1) { *o++ = '.'; memcpy(o, digits + 1, (size_t)nd - 1); o += nd - 1; }
        o += sprintf(o, "e%d", decpt - 1);
    } else if (decpt > 0) {
        memcpy(o, digits, (size_t)decpt); o += decpt;
        if (nd > decpt) { *o++ = '.'; memcpy(o, digits + decpt, (size_t)(nd - decpt)); o += nd - decpt; }
    } else {
        *o++ = '0'; *o++ = '.';
        for (int i = 0; i < -decpt; i++) *o++ = '0';
        memcpy(o, digits, (size_t)nd); o += nd;
    }
    *o = '\0';
    return (uint32_t)(o - out);
}

/* false = not plain data (or out of memory): the Ruby dumper takes over. */
/* The qualified name of `k` as a symbol id (0: too many classes in one dump). */
static uint32_t korb_mdump_class_sym(CTX *const c, struct korb_mdump *const st, const korb_value k) {
    for (uint32_t i = 0; i < st->ccnt; i++) if (st->ckey[i] == k) return st->cval[i];
    if (st->ccnt == sizeof st->ckey / sizeof st->ckey[0]) return 0;
    char buf[256];
    korb_class_qname_into(c, k, buf, sizeof buf);
    const uint32_t id = korb_intern(c->vm, buf, strlen(buf));
    st->ckey[st->ccnt] = k; st->cval[st->ccnt] = id; st->ccnt++;
    return id;
}

/* An instance of a class the Ruby dumper already wrote as a plain 'o' record:
 * `o`, the class name, then the visible ivars as #instance_variables lists them. */
static bool korb_mdump_obj(CTX *const c, struct korb_mdump *const st, const korb_value v, const uint32_t depth);   /* fwd */
static bool korb_mdump_plain_object(CTX *const c, struct korb_mdump *const st, const korb_value v, const uint32_t depth) {
    const uint32_t csym = korb_mdump_class_sym(c, st, VAL2OBJ(v)->klass);
    if (csym == 0 || !korb_mdump_byte(st, 'o') || !korb_mdump_sym(c->vm, st, csym)) return false;
    const struct korb_vm *const vm = c->vm;
    const uint32_t sid0 = VAL2OBJ(v)->shape_id, n = vm->shapes[sid0].ivar_count;
    uint32_t sbuf[32];
    uint32_t *const syms = n <= 32 ? sbuf : malloc((size_t)n * sizeof(uint32_t));
    if (syms == NULL) return false;
    for (uint32_t sid = sid0; sid; sid = vm->shapes[sid].parent) {      /* leaf→root: each ivar at its index */
        const struct korb_shape *const s = &vm->shapes[sid];
        if (s->ivar_count >= 1 && s->ivar_count <= n) syms[s->ivar_count - 1] = s->edge_sym;
    }
    uint32_t shown = 0;                                   /* the filter korb_m_obj_ivars applies */
    for (uint32_t i = 0; i < n; i++) {
        const char *const nm = korb_sym_name(vm, syms[i]);
        const bool hide = nm == NULL || nm[0] != '@' || (nm[1] == '_' && nm[2] == '_' && syms[i] < vm->boot_sym_cnt);
        if (hide) syms[i] = UINT32_MAX; else shown++;
    }
    bool ok = korb_mdump_long(st, shown);
    for (uint32_t i = 0; ok && i < n; i++) {
        if (syms[i] == UINT32_MAX) continue;
        ok = korb_mdump_sym(vm, st, syms[i]) &&
             korb_mdump_obj(c, st, korb_items_data(VAL2OBJ(v)->ivars)[i], depth + 1);
    }
    if (syms != sbuf) free(syms);
    return ok;
}

static bool korb_mdump_obj(CTX *const c, struct korb_mdump *const st, const korb_value v, const uint32_t depth) {
    if (depth > KORB_MDUMP_MAX_DEPTH) return false;
    if (v == KORB_NIL) return korb_mdump_byte(st, '0');
    if (v == KORB_TRUE) return korb_mdump_byte(st, 'T');
    if (v == KORB_FALSE) return korb_mdump_byte(st, 'F');
    if (SYMBOL_P(v)) return korb_mdump_sym(c->vm, st, SYM2ID(v));
    if (FIXNUM_P(v)) {
        const int64_t n = FIX2LONG(v);
        if (n >= -0x40000000LL && n < 0x40000000LL) return korb_mdump_byte(st, 'i') && korb_mdump_long(st, n);
    } else if (!KORB_FLOAT_P(v)) {
        if (!AROH_IS_GC_OBJECT(v)) return false;
        if (KORB_OBJECT_P(v)) {                          /* a plain object: only a class the Ruby dumper vetted */
            const korb_value k = VAL2OBJ(v)->klass;
            if (!KORB_CLASS_P(k) || !VAL2CLASS(k)->marshal_plain || VAL2CLASS(k)->is_singleton ||
                VAL2CLASS(k)->name_sym == 0) return false;        /* a dup'd class may carry the mark: anonymous → Ruby's TypeError */
            if (KORB_CLASS_P(korb_own_singleton_class(c->vm, v))) return false;   /* extended / singleton: 'e' or TypeError */
            if (korb_class_find_method(k, st->mid_mdump, NULL) || korb_class_find_method(k, st->mid_udump, NULL)) return false;
            bool oom = false;
            const uint32_t id = korb_mdump_link(st, v, &oom);
            if (oom) return false;
            if (id != UINT32_MAX) return korb_mdump_byte(st, '@') && korb_mdump_long(st, id);
            return korb_mdump_plain_object(c, st, v, depth);
        }
        const uint32_t fl = ((const AroObjectHeader *)(uintptr_t)v)->flags;
        if (fl & (KORB_FL_HAS_KLASS | KORB_FL_HAS_IVARS)) return false;   /* subclass / extended / singleton / ivars */
        if (KORB_HASH_P(v) && ((fl & (KORB_FL_CMP_BY_ID | KORB_FL_R2K)) || VAL2HASH(v)->default_proc != KORB_NIL)) return false;
        if (KORB_STRING_P(v)) { const uint32_t e = KORB_STR_ENC(v); if (e != KORB_ENC_UTF8 && e != KORB_ENC_USASCII && e != KORB_ENC_BINARY) return false; }
        else if (!KORB_ARRAY_P(v) && !KORB_HASH_P(v)) return false;
    }
    bool oom = false;
    const uint32_t id = korb_mdump_link(st, v, &oom);
    if (oom) return false;
    if (id != UINT32_MAX) return korb_mdump_byte(st, '@') && korb_mdump_long(st, id);
    if (FIXNUM_P(v)) {                                   /* beyond 31 bits: the Bignum form */
        const int64_t n = FIX2LONG(v);
        uint64_t mag = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
        unsigned char w[16]; int nw = 0;
        while (mag) { w[2 * nw] = (unsigned char)(mag & 0xff); w[2 * nw + 1] = (unsigned char)((mag >> 8) & 0xff); mag >>= 16; nw++; }
        return korb_mdump_byte(st, 'l') && korb_mdump_byte(st, n < 0 ? '-' : '+') &&
               korb_mdump_long(st, nw) && korb_mdump_put(st, w, (size_t)nw * 2);
    }
    if (KORB_FLOAT_P(v)) {
        char fs[64];
        const uint32_t n = korb_mdump_float_str(korb_float_val(v), fs);
        return korb_mdump_byte(st, 'f') && korb_mdump_long(st, n) && korb_mdump_put(st, fs, n);
    }
    if (KORB_STRING_P(v)) {
        const uint32_t e = KORB_STR_ENC(v);
        const KorbString *const s = VAL2STR(v);
        if (e != KORB_ENC_BINARY && !korb_mdump_byte(st, 'I')) return false;
        if (!korb_mdump_byte(st, '"') || !korb_mdump_long(st, s->len) || !korb_mdump_put(st, korb_strbuf_data(s->buf), s->len)) return false;
        if (e == KORB_ENC_BINARY) return true;
        return korb_mdump_long(st, 1) && korb_mdump_sym(c->vm, st, korb_intern(c->vm, "E", 1)) &&
               korb_mdump_byte(st, e == KORB_ENC_UTF8 ? 'T' : 'F');
    }
    if (KORB_ARRAY_P(v)) {
        const KorbArray *const a = VAL2ARY(v);
        if (!korb_mdump_byte(st, '[') || !korb_mdump_long(st, a->len)) return false;
        for (uint32_t i = 0; i < VAL2ARY(v)->len; i++)
            if (!korb_mdump_obj(c, st, korb_items_data(VAL2ARY(v)->items)[i], depth + 1)) return false;
        return true;
    }
    const KorbHash *const h = VAL2HASH(v);                /* the checks above leave only a plain Hash */
    const bool dflt = h->default_val != KORB_NIL;
    if (!korb_mdump_byte(st, dflt ? '}' : '{') || !korb_mdump_long(st, h->len)) return false;
    for (uint32_t i = 0; i < VAL2HASH(v)->len; i++) {
        if (!korb_mdump_obj(c, st, korb_items_data(VAL2HASH(v)->items)[2 * i], depth + 1)) return false;
        if (!korb_mdump_obj(c, st, korb_items_data(VAL2HASH(v)->items)[2 * i + 1], depth + 1)) return false;
    }
    return !dflt || korb_mdump_obj(c, st, VAL2HASH(v)->default_val, depth + 1);
}

/* __marshal_mark_plain(obj): the Ruby dumper / loader handled obj as a plain 'o'
 * record, so the C paths may take the next instances of its class. */
static RESULT
korb_bi_marshal_mark_plain(CTX *c, korb_value *slots, korb_slice args)
{
    (void)c; (void)slots;
    const korb_value o = KORB_SLICE_GET(args, 0);            /* an instance: its class is marked only if it is a KorbObject */
    if (!KORB_OBJECT_P(o)) return RESULT_OK(KORB_NIL);
    const korb_value k = VAL2OBJ(o)->klass;
    if (KORB_CLASS_P(k) && !VAL2CLASS(k)->is_singleton && VAL2CLASS(k)->name_sym != 0) VAL2CLASS(k)->marshal_plain = 1;
    return RESULT_OK(KORB_NIL);
}

/* __marshal_fast_dump(obj) → the dump String, or nil (the Ruby dumper's turn). */
static RESULT
korb_bi_marshal_fast_dump(CTX *c, korb_value *slots, korb_slice args)
{
    struct korb_mdump st = { 0 };
    st.mid_mdump = korb_intern(c->vm, "marshal_dump", 12);
    st.mid_udump = korb_intern(c->vm, "_dump", 5);
    bool ok = korb_mdump_byte(&st, 4) && korb_mdump_byte(&st, 8) &&
              korb_mdump_obj(c, &st, KORB_SLICE_GET(args, 0), 0);
    free(st.okey); free(st.oval); free(st.skey); free(st.sval);
    RESULT r = RESULT_OK(KORB_NIL);
    if (ok) {
        r = korb_str_new(c, slots, (const char *)st.b, (uint32_t)st.len);
        if (r.state == KORB_NORMAL) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
    }
    free(st.b);
    return r;
}
