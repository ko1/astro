/* koruby_precise — array_ext.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- more Array methods -------------------------------------------------- */

/* Array#pack — template engine over a manually-managed byte buffer (so X can
 * truncate).  Supports C/c, x, X, a/A/Z (strings), B/b (bits), H/h (hex),
 * M (quoted-printable), m (base64), u (uuencode), w (BER), P/p (pointer stub:
 * 8 zero bytes — real addresses are meaningless under a moving GC).  No alloc
 * happens between fetching elements and emitting bytes, so the bare array
 * pointer stays valid for the whole loop. */
static RESULT korb_m_ary_pack(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE tv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(tv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(tv));
    const KorbString *t = VAL2STR(tv);
    const KorbArray *ary = SELF_ARY;
    uint8_t *ob = NULL; size_t olen = 0, ocap = 0;
    #define PK_RESERVE(n) do { if (olen + (size_t)(n) > ocap) { ocap = (olen + (size_t)(n)) * 2 + 64; ob = (uint8_t *)realloc(ob, ocap); if (!ob) { fprintf(stderr, "koruby_precise: pack OOM\n"); abort(); } } } while (0)
    #define PK_PUT(b)     do { PK_RESERVE(1); ob[olen++] = (uint8_t)(b); } while (0)
    #define PK_PUTS(p,n)  do { PK_RESERVE(n); memcpy(ob + olen, (p), (n)); olen += (size_t)(n); } while (0)
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t ti = 0, ai = 0;                             /* template / array cursors */
    unsigned errtype = 0; const char *errmsg = NULL; char bad = 0;
    while (ti < t->len) {
        const char d = t->buf->data[ti++];
        if (d == ' ' || d == '\t' || d == '\n' || d == '\r' || d == '\v' || d == '\f') continue;
        if (d == '#') { while (ti < t->len && t->buf->data[ti] != '\n') ti++; continue; }   /* comment to EOL */
        bool star = false, has_cnt = false; long cnt = 1;
        if (ti < t->len && t->buf->data[ti] == '*') { star = true; ti++; }
        else if (ti < t->len && t->buf->data[ti] >= '0' && t->buf->data[ti] <= '9') {
            has_cnt = true; cnt = 0; while (ti < t->len && t->buf->data[ti] >= '0' && t->buf->data[ti] <= '9') cnt = cnt * 10 + (t->buf->data[ti++] - '0');
        }
        if (d == 'C' || d == 'c') {
            uint32_t emit = star ? (ary->len - ai) : (uint32_t)cnt;
            for (uint32_t k = 0; k < emit; k++) {
                if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
                VALUE e = ary->items->data[ai++];
                intptr_t b = FIXNUM_P(e) ? FIX2LONG(e) : 0;
                PK_PUT(b & 0xFF);
            }
        } else if (d == 'U') {                            /* UTF-8: codepoints → bytes */
            uint32_t emit = star ? (ary->len - ai) : (uint32_t)cnt;
            for (uint32_t k = 0; k < emit; k++) {
                if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
                VALUE e = ary->items->data[ai++];
                uint32_t cp = FIXNUM_P(e) ? (uint32_t)FIX2LONG(e) : 0;
                if (cp < 0x80) PK_PUT(cp);
                else if (cp < 0x800) { PK_PUT(0xC0 | (cp >> 6)); PK_PUT(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { PK_PUT(0xE0 | (cp >> 12)); PK_PUT(0x80 | ((cp >> 6) & 0x3F)); PK_PUT(0x80 | (cp & 0x3F)); }
                else { PK_PUT(0xF0 | (cp >> 18)); PK_PUT(0x80 | ((cp >> 12) & 0x3F)); PK_PUT(0x80 | ((cp >> 6) & 0x3F)); PK_PUT(0x80 | (cp & 0x3F)); }
            }
        } else if (d == 'x') {
            uint32_t emit = star ? 0 : (uint32_t)cnt;
            for (uint32_t k = 0; k < emit; k++) PK_PUT(0);
        } else if (d == 'X') {                            /* back up cnt bytes (X* → 0) */
            size_t back = star ? 0 : (size_t)cnt;
            if (back > olen) { errtype = KORB_E_ARGUMENT; errmsg = "X outside of string"; break; }
            olen -= back;
        } else if (d == 'w') {                            /* BER-compressed integer */
            if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
            VALUE e = ary->items->data[ai++];
            uintptr_t v = FIXNUM_P(e) ? (uintptr_t)FIX2LONG(e) : 0;
            uint8_t tmp[12]; int n = 0;
            tmp[n++] = (uint8_t)(v & 0x7f); v >>= 7;
            while (v) { tmp[n++] = (uint8_t)((v & 0x7f) | 0x80); v >>= 7; }
            while (n) PK_PUT(tmp[--n]);                   /* big-endian, high bit on all but last */
        } else if (d == 'P' || d == 'p') {                /* pointer stub: 8 zero bytes */
            if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
            ai++;
            for (int k = 0; k < 8; k++) PK_PUT(0);
        } else if (d == 'a' || d == 'A' || d == 'Z' || d == 'B' || d == 'b' || d == 'H' || d == 'h' || d == 'M' || d == 'm' || d == 'u') {
            if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
            VALUE e = ary->items->data[ai++];
            const bool coerce = (d == 'M' || d == 'm' || d == 'u');   /* these to_s their operand */
            char cobuf[64]; const char *ed; uint32_t elen;
            if (KORB_STRING_P(e)) { const KorbString *es = VAL2STR(e); ed = es->buf->data; elen = es->len; }
            else if (e == KORB_NIL) { ed = ""; elen = 0; }
            else if (coerce && FIXNUM_P(e)) { elen = korb_fmt_int((intptr_t)FIX2LONG(e), 10, cobuf); ed = cobuf; }
            else if (coerce && SYMBOL_P(e)) { ed = korb_sym_name(c->vm, SYM2ID(e)); elen = (uint32_t)strlen(ed); }
            else if (coerce && KORB_FLOAT_P(e)) { elen = korb_float_to_s(korb_float_val(e), cobuf); ed = cobuf; }
            else if (coerce && e == KORB_TRUE) { ed = "true"; elen = 4; }
            else if (coerce && e == KORB_FALSE) { ed = "false"; elen = 5; }
            else { errtype = KORB_E_TYPE; errmsg = "no implicit conversion into String"; break; }
            if (d == 'a' || d == 'A') {
                uint32_t want = star ? elen : (uint32_t)cnt;
                const char pad = (d == 'A') ? ' ' : '\0';
                for (uint32_t k = 0; k < want; k++) PK_PUT(k < elen ? ed[k] : pad);
            } else if (d == 'Z') {                        /* null-terminated string */
                if (star) { PK_PUTS(ed, elen); PK_PUT(0); }
                else { uint32_t want = (uint32_t)cnt; for (uint32_t k = 0; k < want; k++) PK_PUT((k < elen && k + 1 < want) ? ed[k] : 0); }
            } else if (d == 'B' || d == 'b') {           /* bit string: bit = byte&1 */
                uint32_t nbits = star ? elen : (uint32_t)cnt;
                unsigned byte = 0, used = 0;
                for (uint32_t k = 0; k < nbits; k++) {
                    const unsigned bit = (k < elen) ? (unsigned)(ed[k] & 1) : 0u;
                    if (d == 'B') byte |= bit << (7 - used); else byte |= bit << used;
                    if (++used == 8) { PK_PUT(byte); byte = 0; used = 0; }
                }
                if (used) PK_PUT(byte);                   /* flush partial byte */
            } else if (d == 'H' || d == 'h') {           /* hex string */
                uint32_t ndig = star ? elen : (uint32_t)cnt;
                unsigned byte = 0, used = 0;
                for (uint32_t k = 0; k < ndig; k++) {
                    const char ch = (k < elen) ? ed[k] : '0';
                    unsigned v = (ch >= '0' && ch <= '9') ? (unsigned)(ch - '0')
                               : (ch >= 'a' && ch <= 'f') ? (unsigned)(ch - 'a' + 10)
                               : (ch >= 'A' && ch <= 'F') ? (unsigned)(ch - 'A' + 10) : 0u;
                    if (d == 'H') byte |= v << (used ? 0 : 4); else byte |= v << (used ? 4 : 0);
                    if (++used == 2) { PK_PUT(byte); byte = 0; used = 0; }
                }
                if (used) PK_PUT(byte);                   /* flush partial nibble */
            } else if (d == 'M') {                        /* quoted-printable */
                const long wrap = (has_cnt && cnt > 1) ? cnt : 72;
                long col = 0; char last = 0;
                static const char HX[] = "0123456789ABCDEF";
                for (uint32_t k = 0; k < elen; k++) {
                    const unsigned char ch = (unsigned char)ed[k];
                    if (ch == '\n') { if (last == ' ' || last == '\t') PK_PUTS("=\n", 2); PK_PUT('\n'); col = 0; last = '\n'; continue; }
                    char enc[3]; int en;
                    if (ch == '=' || ch > 126 || (ch < 32 && ch != '\t')) { en = 3; enc[0] = '='; enc[1] = HX[ch >> 4]; enc[2] = HX[ch & 15]; }
                    else { en = 1; enc[0] = (char)ch; }
                    if (col + en > wrap) { PK_PUTS("=\n", 2); col = 0; }
                    PK_PUTS(enc, en); col += en; last = enc[en - 1];
                }
                if (elen > 0 && last != '\n') PK_PUTS("=\n", 2);
            } else {                                      /* m (base64) / u (uuencode) */
                long per = has_cnt ? cnt : 45;
                per = (per / 3) * 3; if (per < 3) per = 3;
                if (elen == 0) { /* empty → no output (both m and u) */ }
                for (uint32_t off = 0; off < elen; off += (uint32_t)per) {
                    uint32_t end = off + (uint32_t)per; if (end > elen) end = elen;
                    if (d == 'u') PK_PUT((end - off) + 0x20);   /* uuencode line-length char */
                    for (uint32_t i = off; i < end; i += 3) {
                        uint32_t n = end - i;
                        unsigned b0 = (unsigned char)ed[i], b1 = n > 1 ? (unsigned char)ed[i + 1] : 0, b2 = n > 2 ? (unsigned char)ed[i + 2] : 0;
                        unsigned v0 = b0 >> 2, v1 = ((b0 & 3) << 4) | (b1 >> 4), v2 = ((b1 & 15) << 2) | (b2 >> 6), v3 = b2 & 0x3f;
                        if (d == 'm') {
                            PK_PUT(B64[v0]); PK_PUT(B64[v1]);
                            PK_PUT(n > 1 ? B64[v2] : '='); PK_PUT(n > 2 ? B64[v3] : '=');
                        } else {
                            PK_PUT(v0 ? v0 + 0x20 : '`'); PK_PUT(v1 ? v1 + 0x20 : '`');
                            PK_PUT(v2 ? v2 + 0x20 : '`'); PK_PUT(v3 ? v3 + 0x20 : '`');
                        }
                    }
                    PK_PUT('\n');
                }
            }
        } else {
            bad = d; break;
        }
        if (errmsg) break;
    }
    if (errmsg) { free(ob); return korb_raise(c, slots, errtype, 0, "%s", errmsg); }
    if (bad)    { free(ob); return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Array#pack: directive '%c' not supported", bad); }
    RESULT r = korb_str_new(c, slots, ob ? (const char *)ob : "", (uint32_t)olen);
    free(ob);
    if (LIKELY(r.state == KORB_NORMAL)) ((AroObjectHeader *)(uintptr_t)r.value)->flags |= KORB_FL_BINARY;   /* pack yields ASCII-8BIT */
    return r;
    #undef PK_RESERVE
    #undef PK_PUT
    #undef PK_PUTS
}

/* String#unpack — a/A/Z (strings), x (skip), C/c (byte), J/j/Q/q (8-byte LE int),
 * P/p (pointer deref → nil, meaningless under a moving GC).  Unknown directives
 * push nil rather than raise.  slots[0]=template, slots[1]=subject (both re-read
 * after each result push, which may move them). */
static RESULT korb_m_str_unpack(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE tv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(tv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(tv));
    slots[0] = tv;                                        /* template */
    slots[1] = VALUE_REF_GET(self);                      /* subject */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 3, 0));    /* result */
    VALUE_REF res = VALUE_REF_AT(&slots[2]);
    uint32_t ti = 0, si = 0;
    while (ti < VAL2STR(slots[0])->len) {
        const KorbString *t = VAL2STR(slots[0]);
        const char d = t->buf->data[ti++];
        if (d == ' ' || d == '\t' || d == '\n') continue;
        bool star = false; long cnt = 1;
        if (ti < t->len && t->buf->data[ti] == '*') { star = true; ti++; }
        else if (ti < t->len && t->buf->data[ti] >= '0' && t->buf->data[ti] <= '9') {
            cnt = 0; while (ti < t->len && t->buf->data[ti] >= '0' && t->buf->data[ti] <= '9') cnt = cnt * 10 + (t->buf->data[ti++] - '0');
        }
        const uint32_t slen = VAL2STR(slots[1])->len;
        if (d == 'a' || d == 'A' || d == 'Z') {          /* string slice (one value) */
            uint32_t avail = slen - si;
            uint32_t take = star ? avail : ((uint32_t)cnt < avail ? (uint32_t)cnt : avail);
            uint32_t vlen = take;
            if (d == 'Z') { for (uint32_t k = 0; k < take; k++) if (VAL2STR(slots[1])->buf->data[si + k] == '\0') { vlen = k; break; } }
            else if (d == 'A') { while (vlen > 0) { char ch = VAL2STR(slots[1])->buf->data[si + vlen - 1]; if (ch == ' ' || ch == '\0') vlen--; else break; } }
            KorbString *r = korb_str_alloc(c, slots + 3, vlen);   /* may move slots[0..2] */
            memcpy(r->buf->data, VAL2STR(slots[1])->buf->data + si, vlen);   /* re-read subject */
            slots[3] = (VALUE)r;
            CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
            si += take;
        } else if (d == 'x') {                            /* skip bytes (no value) */
            uint32_t skip = star ? (slen - si) : (uint32_t)cnt;
            if (UNLIKELY(si + skip > slen)) return korb_raise(c, slots + 3, KORB_E_ARGUMENT, 0, "x outside of string");
            si += skip;
        } else if (d == 'C' || d == 'c') {                /* unsigned / signed byte */
            const long reps = star ? (long)(slen - si) : cnt;
            for (long r = 0; r < reps; r++) {
                if (si >= VAL2STR(slots[1])->len) { CHECK(korb_ary_push_val(c, slots + 3, res, KORB_NIL)); continue; }
                int b = (unsigned char)VAL2STR(slots[1])->buf->data[si++];
                CHECK(korb_ary_push_val(c, slots + 3, res, LONG2FIX(d == 'c' ? (int8_t)b : b)));
            }
        } else if (d == 'J' || d == 'j' || d == 'Q' || d == 'q') {   /* 8-byte LE int */
            const long reps = star ? (long)((slen - si) / 8) : cnt;
            for (long r = 0; r < reps; r++) {
                uint64_t v = 0;
                for (int k = 0; k < 8; k++) { const KorbString *s = VAL2STR(slots[1]); if (si < s->len) v |= (uint64_t)(unsigned char)s->buf->data[si] << (8 * k); si++; }
                CHECK(korb_ary_push_val(c, slots + 3, res, LONG2FIX((intptr_t)v)));
            }
        } else if (d == 'U') {                            /* UTF-8 codepoints */
            for (long r = 0; (star || r < cnt); r++) {
                const KorbString *s = VAL2STR(slots[1]);
                if (si >= s->len) { if (star) break; CHECK(korb_ary_push_val(c, slots + 3, res, KORB_NIL)); continue; }
                const unsigned char b0 = (unsigned char)s->buf->data[si];
                uint32_t cp; int len;
                if (b0 < 0x80)            { cp = b0;        len = 1; }
                else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; len = 2; }
                else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; len = 3; }
                else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; len = 4; }
                else                      { cp = b0;        len = 1; }   /* invalid lead → one byte */
                for (int k = 1; k < len && si + (uint32_t)k < s->len; k++) cp = (cp << 6) | ((unsigned char)s->buf->data[si + k] & 0x3F);
                si += (uint32_t)len;
                CHECK(korb_ary_push_val(c, slots + 3, res, LONG2FIX((intptr_t)cp)));
            }
        } else {                                          /* P/p and anything else → nil */
            const long reps = star ? 0 : cnt;
            for (long r = 0; r < reps; r++) CHECK(korb_ary_push_val(c, slots + 3, res, KORB_NIL));
        }
    }
    return RESULT_OK(VALUE_REF_GET(res));
}
/* String#unpack1(template) — the first value of unpack (or nil). */
static RESULT korb_m_str_unpack1(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT r = korb_m_str_unpack(c, slots, self, a);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    const KorbArray *arr = VAL2ARY(r.value);
    return RESULT_OK(arr->len > 0 ? arr->items->data[0] : KORB_NIL);
}

static RESULT korb_m_ary_take(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(nv));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to take negative size");
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if ((uint32_t)n > len) n = len;
    return korb_ary_subseq(c, slots, self, 0, (uint32_t)n);
}
/* Array#sample([n][, random:]) — CRuby-exact via the MT19937 generator.
 * sample (no arg): one random element.  sample(n), n<=10: CRuby's rnds[] +
 * sorted-insertion distinct-index algorithm (bit-exact).  n>10: copy + partial
 * Fisher-Yates (random but not CRuby-bit-exact). */
static RESULT korb_m_ary_sample(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t argc = VALUE_SLICE_LEN(a);
    if (argc >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, argc - 1))) argc--;   /* random: kwargs */
    const uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if (argc == 0) {   /* one random element */
        if (len == 0) return RESULT_OK(KORB_NIL);
        KorbMT *const st = korb_rng_from_kwargs(c, a);
        return RESULT_OK(VAL2ARY(VALUE_REF_GET(self))->items->data[korb_mt_limited(st, len - 1)]);
    }
    intptr_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative sample number");
    if ((uint32_t)n > (intptr_t)len) n = len;
    if (n == 0) return korb_ary_new(c, slots, 0);

    if (n <= 10) {
        /* CRuby ary_sample n<=10: draw all randoms FIRST (alloc would move the
         * rng buffer), then build distinct indices, then materialize. */
        KorbMT *const st = korb_rng_from_kwargs(c, a);
        long rnds[10], idx[10], sorted[10];
        for (intptr_t i = 0; i < n; i++) rnds[i] = (long)korb_mt_limited(st, (uint32_t)(len - i - 1));   /* RAND_UPTO(len-i) */
        sorted[0] = idx[0] = rnds[0];
        for (intptr_t i = 1; i < n; i++) {
            long k = rnds[i], j;
            for (j = 0; j < i; ++j) { if (k < sorted[j]) break; ++k; }
            memmove(&sorted[j+1], &sorted[j], sizeof(sorted[0]) * (size_t)(i - j));
            sorted[j] = idx[i] = k;
        }
        const RESULT rr = korb_ary_new(c, slots, (uint32_t)n);   /* may GC; self re-read below */
        if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
        slots[0] = rr.value;
        VALUE_REF dst = VALUE_REF_AT(&slots[0]);
        const KorbArray *const src = VAL2ARY(VALUE_REF_GET(self));
        for (intptr_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, src->items->data[idx[i]]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    /* n>10: copy then partial Fisher-Yates (random, not CRuby-bit-exact). */
    const RESULT cp = korb_ary_subseq(c, slots, self, 0, len);
    if (UNLIKELY(cp.state != KORB_NORMAL)) return cp;
    slots[0] = cp.value;
    KorbMT *const st = korb_rng_from_kwargs(c, a);
    KorbArrayItems *const it = VAL2ARY(slots[0])->items;
    for (intptr_t i = 0; i < n; i++) {
        const uint32_t j = i + korb_mt_limited(st, (uint32_t)(len - i - 1));   /* [i, len-1] */
        const VALUE t = it->data[i]; it->data[i] = it->data[j]; it->data[j] = t;
    }
    VAL2ARY(slots[0])->len = (uint32_t)n;
    return RESULT_OK(slots[0]);
}
/* Array#shuffle([random:]) — CRuby-exact Fisher-Yates over a copy, driven by the
 * MT19937 generator (the `random:` kwarg's Random, else the default). */
static RESULT korb_m_ary_shuffle(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    const RESULT cp = korb_ary_subseq(c, slots, self, 0, len);   /* copy (may GC) */
    if (UNLIKELY(cp.state != KORB_NORMAL)) return cp;
    slots[0] = cp.value;                                          /* root the copy */
    KorbMT *const st = korb_rng_from_kwargs(c, a);                /* AFTER alloc: rng ptr is fresh */
    KorbArrayItems *const it = VAL2ARY(slots[0])->items;
    for (uint32_t i = len; i > 1; i--) {                          /* j in [0, i-1]; swap (i-1, j) */
        const uint32_t j = korb_mt_limited(st, i - 1);
        const VALUE t = it->data[i - 1]; it->data[i - 1] = it->data[j]; it->data[j] = t;
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_ary_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(nv));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to drop negative size");
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if ((uint32_t)n > len) n = len;
    return korb_ary_subseq(c, slots, self, (uint32_t)n, len - (uint32_t)n);
}
static RESULT korb_m_ary_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = VALUE_SLICE_GET(a, 0);             /* root the needle across a possible yield */
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    uint32_t w = 0; VALUE last = KORB_NIL; bool found = false;
    for (uint32_t r = 0; r < ary->len; r++) {
        if (korb_value_eq(it->data[r], slots[0])) { last = it->data[r]; found = true; }   /* return the deleted element */
        else { if (w != r) ARO_STORE(c, it, &it->data[w], it->data[r]); w++; }
    }
    for (uint32_t r = w; r < ary->len; r++) ARO_STORE(c, it, &it->data[r], KORB_NIL);
    ary->len = w;
    if (found) return RESULT_OK(last);
    if (block != NULL) return korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);   /* not found → block value */
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_delete_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t i;
    if (!korb_to_index(iv, &i)) return RESULT_OK(KORB_NIL);
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    if (i < 0) i += ary->len;
    if (i < 0 || (uint32_t)i >= ary->len) return RESULT_OK(KORB_NIL);
    KorbArrayItems *it = ary->items;
    VALUE removed = it->data[i];
    for (uint32_t r = (uint32_t)i; r + 1 < ary->len; r++) ARO_STORE(c, it, &it->data[r], it->data[r + 1]);
    ary->len--; ARO_STORE(c, it, &it->data[ary->len], KORB_NIL);
    return RESULT_OK(removed);
}
static RESULT korb_m_ary_rindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block != NULL && VALUE_SLICE_LEN(a) == 0) {   /* block form (arg, if given, wins per CRuby) */
        for (int32_t i = (int32_t)VAL2ARY(VALUE_REF_GET(self))->len - 1; i >= 0; i--) {
            slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_NIL && r.value != KORB_FALSE) return RESULT_OK(LONG2FIX(i));
        }
        return RESULT_OK(KORB_NIL);
    }
    const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (int32_t i = (int32_t)ary->len - 1; i >= 0; i--)
        if (korb_value_eq(ary->items->data[i], needle)) return RESULT_OK(LONG2FIX(i));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_rotate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    intptr_t sh = 1;
    if (VALUE_SLICE_LEN(a) >= 1 && UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &sh))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (len == 0) return korb_ary_subseq(c, slots, self, 0, 0);
    intptr_t s = ((sh % (intptr_t)len) + (intptr_t)len) % (intptr_t)len;   /* normalized left rotation */
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, len)));
    for (uint32_t i = 0; i < len; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[(s + i) % len];
        CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* zip rows: [ self[i], other0[i], other1[i], ... ]. With a block, yield each row
 * and return nil; otherwise collect rows into an array. dst lives at slots[1]
 * (block path leaves it nil/unused), rows built at slots[2]. */
static RESULT korb_m_ary_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t k = VALUE_SLICE_LEN(a);
    uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    slots[0] = (block == NULL) ? UNWRAP(korb_ary_new(c, slots, n)) : KORB_NIL;   /* dst */
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 2, k + 1));              /* row at slots[1] */
        VALUE_REF row = VALUE_REF_AT(&slots[1]);
        CHECK(korb_ary_push_val(c, slots + 2, row, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
        for (uint32_t j = 0; j < k; j++)
            CHECK(korb_ary_push_val(c, slots + 2, row, korb_zip_elem(VALUE_SLICE_GET(a, j), i)));
        if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        } else {
            CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
        }
    }
    return RESULT_OK(block != NULL ? KORB_NIL : VALUE_REF_GET(dst));
}

static bool korb_ary_has(const KorbArray *ar, VALUE v) {
    for (uint32_t i = 0; i < ar->len; i++) if (korb_value_eql(ar->items->data[i], v)) return true;
    return false;
}
/* `|` union (in self then other, deduped) / `&` intersection (in both, self order, deduped) */
static RESULT korb_m_ary_union(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t sn = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < sn; i++) { VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i]; if (!korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e)); }
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {  /* union(*others) */
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
        uint32_t on = VAL2ARY(ov)->len;
        for (uint32_t i = 0; i < on; i++) { VALUE e = VAL2ARY(VALUE_SLICE_GET(a, k))->items->data[i]; if (!korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e)); }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_intersect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++)
        if (UNLIKELY(!KORB_ARRAY_P(VALUE_SLICE_GET(a, k)))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, k)));
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    bool no_args = VALUE_SLICE_LEN(a) == 0;             /* intersection() → plain copy of self, no dedup */
    uint32_t sn = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < sn; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
        if (no_args) { CHECK(korb_ary_push_val(c, slots + 1, dst, e)); continue; }
        bool in_all = true;                              /* element must be in every other array */
        for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) if (!korb_arr_has(VAL2ARY(VALUE_SLICE_GET(a, k)), e)) { in_all = false; break; }
        if (in_all && !korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

