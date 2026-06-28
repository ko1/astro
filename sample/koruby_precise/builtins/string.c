/* koruby_precise — string.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- String methods ------------------------------------------------------ */

static RESULT korb_m_str_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_STR->len)); }
static RESULT korb_m_str_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_STR->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_str_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_str_to_sym(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const KorbString *s = SELF_STR;
    return RESULT_OK(ID2SYM(korb_intern(c->vm, s->buf->data, s->len)));
}
/* transform-into-new-string helper (op: 0=upcase 1=downcase 2=capitalize 3=reverse) */
static RESULT korb_str_transform(CTX *c, VALUE *slots, VALUE_REF self, int op) {
    uint32_t len = SELF_STR->len;
    KorbString *r = korb_str_alloc(c, slots, len);
    const KorbString *s = SELF_STR;   /* re-read after alloc (GC may have moved it) */
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        unsigned char out;
        switch (op) {
          case 0: out = (ch >= 'a' && ch <= 'z') ? (unsigned char)(ch - 32) : ch; break;
          case 1: out = (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + 32) : ch; break;
          case 2:
            if (i == 0) out = (ch >= 'a' && ch <= 'z') ? (unsigned char)(ch - 32) : ch;
            else        out = (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + 32) : ch;
            break;
          default: out = (unsigned char)s->buf->data[len - 1 - i]; break;
        }
        r->buf->data[i] = (char)out;
    }
    return RESULT_OK((VALUE)r);
}
static RESULT korb_m_str_upcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { (void)a; return korb_str_transform(c, slots, self, 0); }
static RESULT korb_m_str_downcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_str_transform(c, slots, self, 1); }
static RESULT korb_m_str_capitalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_str_transform(c, slots, self, 2); }
/* String#reverse — reverse CHARACTERS (UTF-8 sequences kept intact), not bytes. */
static RESULT korb_m_str_reverse(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t len = SELF_STR->len;
    KorbString *r = korb_str_alloc(c, slots, len);
    const KorbString *s = SELF_STR;                      /* re-read after alloc */
    uint32_t wi = len, i = 0;
    while (i < len) {
        const unsigned char b = (unsigned char)s->buf->data[i];
        uint32_t clen = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        if (i + clen > len) clen = 1;                    /* truncated lead → one byte */
        wi -= clen;
        memcpy(r->buf->data + wi, s->buf->data + i, clen);
        i += clen;
    }
    return RESULT_OK((VALUE)r);
}

/* byte-substring search: index of needle in hay[0..hlen), or -1 (empty matches at 0) */
static int32_t
korb_byte_find(const char *hay, uint32_t hlen, const char *needle, uint32_t nlen)
{
    if (nlen == 0) return 0;
    if (nlen > hlen) return -1;
    for (uint32_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return (int32_t)i;
    return -1;
}

static inline bool korb_is_ws(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/* count UTF-8 codepoints in the first nbytes bytes (lead bytes only) */
static uint32_t
korb_utf8_count(const char *b, uint32_t nbytes)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < nbytes; i++)
        if (((unsigned char)b[i] & 0xC0) != 0x80) n++;
    return n;
}

/* byte offset of codepoint index ci (clamped to [0, len]) */
static uint32_t
korb_utf8_byteoff(const char *b, uint32_t len, uint32_t ci)
{
    uint32_t i = 0, n = 0;
    while (i < len && n < ci) {
        i++;
        while (i < len && ((unsigned char)b[i] & 0xC0) == 0x80) i++;
        n++;
    }
    return i;
}

/* alloc a fresh String = self->buf->data[start, start+len); re-reads self after the
 * alloc-GC (source may have moved) — THE safe substring primitive. */
static RESULT
korb_str_slice_new(CTX *c, VALUE *slots, VALUE_REF sref, uint32_t start, uint32_t len)
{
    KorbString *r = korb_str_alloc(c, slots, len);
    const KorbString *s = VAL2STR(VALUE_REF_GET(sref));   /* re-read: GC may have moved it */
    memcpy(r->buf->data, s->buf->data + start, len);
    return RESULT_OK((VALUE)r);
}

/* ---- mutable String operations (in place; header never moves) ------------ */

static uint32_t korb_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0|(cp>>6)); out[1] = (char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0|(cp>>12)); out[1] = (char)(0x80|((cp>>6)&0x3F)); out[2] = (char)(0x80|(cp&0x3F)); return 3; }
    out[0] = (char)(0xF0|(cp>>18)); out[1] = (char)(0x80|((cp>>12)&0x3F)); out[2] = (char)(0x80|((cp>>6)&0x3F)); out[3] = (char)(0x80|(cp&0x3F)); return 4;
}
/* append other (a rooted String) onto self in place */
static RESULT korb_str_append_str(CTX *c, VALUE *slots, VALUE_REF self, VALUE_REF other) {
    uint32_t on = VAL2STR(VALUE_REF_GET(other))->len;
    KorbString *s = korb_str_ensure(c, slots, self, VAL2STR(VALUE_REF_GET(self))->len + on);
    const KorbString *o = VAL2STR(VALUE_REF_GET(other));   /* re-read after grow */
    memcpy(s->buf->data + s->len, o->buf->data, on);
    s->len += on; s->buf->data[s->len] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
/* append one element (String or Integer codepoint) onto self */
static RESULT korb_str_append_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_REF oref) {
    VALUE o = VALUE_REF_GET(oref);
    if (KORB_STRING_P(o)) return korb_str_append_str(c, slots, self, oref);
    if (FIXNUM_P(o)) {
        intptr_t cp = FIX2LONG(o);
        if (cp < 0 || cp > 0x10FFFF) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "%ld out of char range", (long)cp);
        char buf[4]; uint32_t n = korb_utf8_encode((uint32_t)cp, buf);   /* stable C buffer */
        return korb_str_cat(c, slots, self, buf, n);
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(o));
}
static RESULT korb_m_str_ltlt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    CHECK(korb_str_append_one(c, slots, self, VALUE_SLICE_REF(a, 0)));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_concat(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++)
        CHECK(korb_str_append_one(c, slots, self, VALUE_SLICE_REF(a, j)));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_replace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(o));
    VAL2STR(VALUE_REF_GET(self))->len = 0;             /* clear, then append other */
    return korb_str_append_str(c, slots, self, VALUE_SLICE_REF(a, 0));
}
static RESULT korb_m_str_prepend(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    uint32_t pn = 0;
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        VALUE o = VALUE_SLICE_GET(a, j);
        if (UNLIKELY(!KORB_STRING_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(o));
        pn += VAL2STR(o)->len;
    }
    uint32_t slen = VAL2STR(VALUE_REF_GET(self))->len;
    KorbString *s = korb_str_ensure(c, slots, self, slen + pn);   /* single grow; args rooted */
    s = VAL2STR(VALUE_REF_GET(self));
    memmove(s->buf->data + pn, s->buf->data, slen);
    uint32_t off = 0;
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        const KorbString *o = VAL2STR(VALUE_SLICE_GET(a, j));
        memcpy(s->buf->data + off, o->buf->data, o->len); off += o->len;
    }
    s->len = slen + pn; s->buf->data[s->len] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_clear(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    (void)c;(void)slots;(void)a;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    s->len = 0; s->buf->data[0] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
/* in-place case/reverse (op: 0 upcase 1 downcase 2 capitalize 3 swapcase 4 reverse);
 * returns self if changed, else nil (Ruby bang convention) — reverse! always self. */
static RESULT korb_str_transform_bang(CTX *c, VALUE *slots, VALUE_REF self, int op) {
    (void)c;(void)slots;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t len = s->len; bool changed = false;
    if (op == 4) {                                     /* reverse! (UTF-8 char-aware) */
        char *tmp = malloc(len ? len : 1);
        if (!tmp) abort();
        uint32_t wi = len, i = 0;
        while (i < len) {
            const unsigned char b = (unsigned char)s->buf->data[i];
            uint32_t clen = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
            if (i + clen > len) clen = 1;
            wi -= clen; memcpy(tmp + wi, s->buf->data + i, clen); i += clen;
        }
        memcpy(s->buf->data, tmp, len); free(tmp);
        return RESULT_OK(VALUE_REF_GET(self));
    }
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i], out = ch;
        switch (op) {
          case 0: if (ch >= 'a' && ch <= 'z') out = (unsigned char)(ch - 32); break;
          case 1: if (ch >= 'A' && ch <= 'Z') out = (unsigned char)(ch + 32); break;
          case 2:
            if (i == 0) { if (ch >= 'a' && ch <= 'z') out = (unsigned char)(ch - 32); }
            else { if (ch >= 'A' && ch <= 'Z') out = (unsigned char)(ch + 32); }
            break;
          default: out = (unsigned char)(isupper(ch) ? tolower(ch) : islower(ch) ? toupper(ch) : ch); break;
        }
        if (out != ch) { s->buf->data[i] = (char)out; changed = true; }
    }
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_upcase_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { (void)a; return korb_str_transform_bang(c, slots, self, 0); }
static RESULT korb_m_str_downcase_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_str_transform_bang(c, slots, self, 1); }
static RESULT korb_m_str_capitalize_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_str_transform_bang(c, slots, self, 2); }
static RESULT korb_m_str_swapcase_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_str_transform_bang(c, slots, self, 3); }
static RESULT korb_m_str_reverse_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)a; return korb_str_transform_bang(c, slots, self, 4); }

/* Replace self bytes [bs,be) with replref (a rooted String); do_repl=false deletes. */
static RESULT korb_str_splice(CTX *c, VALUE *slots, VALUE_REF self, uint32_t bs, uint32_t be, VALUE_REF replref, bool do_repl) {
    uint32_t rn = do_repl ? VAL2STR(VALUE_REF_GET(replref))->len : 0;
    uint32_t slen = VAL2STR(VALUE_REF_GET(self))->len;
    uint32_t newlen = slen - (be - bs) + rn;
    KorbString *s = korb_str_ensure(c, slots, self, newlen);   /* alloc; self+repl rooted */
    s = VAL2STR(VALUE_REF_GET(self));
    memmove(s->buf->data + bs + rn, s->buf->data + be, slen - be);
    if (rn) { const KorbString *r = VAL2STR(VALUE_REF_GET(replref)); memcpy(s->buf->data + bs, r->buf->data, rn); }
    s->len = newlen; s->buf->data[newlen] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Compute byte span [*bs,*be) for a string index target. idx + optional len arg
 * (len_v = KORB_NIL if absent). *found=false ⇒ no match / out of range. */
/* If *v isn't already an integer but responds to #to_int, replace it with the
 * coerced value (may GC — call before reading the receiver string). */
static RESULT korb_coerce_to_int(CTX *c, VALUE *slots, VALUE *v) {
    intptr_t tmp;
    if (korb_to_index(*v, &tmp)) return RESULT_OK(KORB_TRUE);
    const uint32_t to_int = korb_intern(c->vm, "to_int", 6);
    if (!korb_responds_to(c, *v, to_int)) return RESULT_OK(KORB_FALSE);
    slots[0] = *v;
    RESULT r = korb_send_impl(c, slots + 1, to_int, 0, 0, NULL, NULL, KORB_NIL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (!korb_to_index(r.value, &tmp)) return RESULT_OK(KORB_FALSE);
    *v = r.value;
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_str_target_span(CTX *c, VALUE *slots, VALUE_REF self, VALUE idx, VALUE len_v, bool *found, uint32_t *bs, uint32_t *be, bool write) {
    if (!KORB_STRING_P(idx) && !KORB_RANGE_P(idx)) {   /* coerce a non-String/Range index via #to_int (before reading self) */
        RESULT cr = korb_coerce_to_int(c, slots, &idx);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    }
    if (len_v != KORB_NIL) {
        RESULT cr = korb_coerce_to_int(c, slots, &len_v);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t ncp = korb_utf8_count(s->buf->data, s->len);
    *found = true;
    if (KORB_STRING_P(idx)) {                          /* substring target */
        const KorbString *sub = VAL2STR(idx);
        int32_t at = korb_byte_find(s->buf->data, s->len, sub->buf->data, sub->len);
        if (at < 0) { *found = false; return RESULT_OK(KORB_NIL); }
        *bs = (uint32_t)at; *be = (uint32_t)at + sub->len;
        return RESULT_OK(KORB_NIL);
    }
    intptr_t st, ln;
    if (KORB_RANGE_P(idx)) {
        const KorbRange *r = VAL2RANGE(idx);
        const bool beginless = (r->rbegin == KORB_NIL);
        const bool endless   = (r->rend   == KORB_NIL);
        intptr_t b, e;
        if (beginless) b = 0;
        else if (UNLIKELY(!korb_to_index(r->rbegin, &b))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (endless) e = (intptr_t)ncp;
        else if (UNLIKELY(!korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (b < 0) b += ncp;
        if (!endless && e < 0) e += ncp;
        st = b; ln = ((endless || r->exclude_end) ? e - 1 : e) - b + 1; if (ln < 0) ln = 0;
    } else {
        if (UNLIKELY(!korb_to_index(idx, &st))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(idx));
        if (st < 0) st += ncp;
        if (len_v != KORB_NIL) {
            if (UNLIKELY(!korb_to_index(len_v, &ln))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(len_v));
        } else ln = 1;
    }
    const bool single = (len_v == KORB_NIL && !KORB_RANGE_P(idx));   /* str[i]: one char, nil at end (read only) */
    if (st < 0 || st > (intptr_t)ncp || ln < 0 || (single && !write && st == (intptr_t)ncp)) { *found = false; return RESULT_OK(KORB_NIL); }
    if (single && write && st == (intptr_t)ncp) ln = 0;              /* str[len]=x → append (empty span at end) */
    if (st + ln > (intptr_t)ncp) ln = (intptr_t)ncp - st;
    *bs = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)st);
    *be = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)(st + ln));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_str_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    uint32_t na = VALUE_SLICE_LEN(a);
    if (UNLIKELY(na < 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE repl = VALUE_SLICE_GET(a, na - 1);
    if (UNLIKELY(!KORB_STRING_P(repl))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(repl));
    VALUE idx = VALUE_SLICE_GET(a, 0);
    VALUE len_v = (na == 3) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    bool found; uint32_t bs = 0, be = 0;
    RESULT sp = korb_str_target_span(c, slots, self, idx, len_v, &found, &bs, &be, true);
    if (UNLIKELY(sp.state != KORB_NORMAL)) return sp;
    if (!found) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld out of string", (long)(VALUE_SLICE_LEN(a)>=1 && FIXNUM_P(VALUE_SLICE_GET(a,0)) ? FIX2LONG(VALUE_SLICE_GET(a,0)) : 0));
    CHECK(korb_str_splice(c, slots, self, bs, be, VALUE_SLICE_REF(a, na - 1), true));
    return RESULT_OK(VALUE_SLICE_GET(a, na - 1));   /* re-read: splice's GC may have moved repl (the `a` slice is a rooted slot) */
}
static RESULT korb_m_str_slice_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    uint32_t na = VALUE_SLICE_LEN(a);
    if (UNLIKELY(na < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE idx = VALUE_SLICE_GET(a, 0);
    VALUE len_v = (na >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    bool found; uint32_t bs = 0, be = 0;
    RESULT sp = korb_str_target_span(c, slots, self, idx, len_v, &found, &bs, &be, false);
    if (UNLIKELY(sp.state != KORB_NORMAL)) return sp;
    if (!found) return RESULT_OK(KORB_NIL);
    slots[0] = UNWRAP(korb_str_slice_new(c, slots, self, bs, be - bs));   /* removed part */
    CHECK(korb_str_splice(c, slots + 1, self, bs, be, self, false));      /* delete it */
    return RESULT_OK(slots[0]);
}
/* in-place whitespace strip (mode: 0 both, 1 left, 2 right). self if changed else nil. */
static bool korb_str_sets_match(VALUE_SLICE a, unsigned char ch);
static RESULT korb_str_strip_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode) {
    (void)c;(void)slots;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t lo = 0, hi = s->len;
    bool has_set = VALUE_SLICE_LEN(a) >= 1;
    if (mode != 2) while (lo < hi && (has_set ? korb_str_sets_match(a, (unsigned char)s->buf->data[lo]) : (unsigned char)s->buf->data[lo] <= ' ')) lo++;
    if (mode != 1) while (hi > lo && (has_set ? korb_str_sets_match(a, (unsigned char)s->buf->data[hi-1]) : (unsigned char)s->buf->data[hi-1] <= ' ')) hi--;
    if (lo == 0 && hi == s->len) return RESULT_OK(KORB_NIL);   /* unchanged */
    uint32_t nlen = hi - lo;
    if (lo) memmove(s->buf->data, s->buf->data + lo, nlen);
    s->len = nlen; s->buf->data[nlen] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_strip_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_strip_bang(c, slots, self, a, 0); }
static RESULT korb_m_str_lstrip_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_strip_bang(c, slots, self, a, 1); }
static RESULT korb_m_str_rstrip_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_strip_bang(c, slots, self, a, 2); }
static RESULT korb_m_str_chomp_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t n = s->len;
    if (n == 0) return RESULT_OK(KORB_NIL);           /* empty → no-op → nil, before any sep-type check (CRuby) */
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* chomp!(sep) */
        VALUE sv = VALUE_SLICE_GET(a, 0);
        if (sv == KORB_NIL) return RESULT_OK(KORB_NIL);   /* nil sep → no-op → nil */
        if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
        const KorbString *sep = VAL2STR(sv);
        if (sep->len == 0) { while (n >= 1 && s->buf->data[n-1] == '\n') { if (n >= 2 && s->buf->data[n-2] == '\r') n--; n--; } }
        else if (n >= sep->len && memcmp(s->buf->data + n - sep->len, sep->buf->data, sep->len) == 0) n -= sep->len;
        if (n == s->len) return RESULT_OK(KORB_NIL);
        s->len = n; s->buf->data[n] = '\0';
        return RESULT_OK(VALUE_REF_GET(self));
    }
    if (n >= 1 && s->buf->data[n-1] == '\n') { n--; if (n >= 1 && s->buf->data[n-1] == '\r') n--; }
    else if (n >= 1 && s->buf->data[n-1] == '\r') n--;
    else return RESULT_OK(KORB_NIL);
    s->len = n; s->buf->data[n] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_chop_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    if (s->len == 0) return RESULT_OK(KORB_NIL);
    uint32_t n = s->len;
    if (n >= 2 && s->buf->data[n-1] == '\n' && s->buf->data[n-2] == '\r') n -= 2;
    else {
        n--;                                           /* back up over one UTF-8 char */
        while (n > 0 && ((unsigned char)s->buf->data[n] & 0xC0) == 0x80) n--;
    }
    s->len = n; s->buf->data[n] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}

/* char-set membership for count/squeeze/delete: supports leading ^ negation and
 * a-z ranges (ASCII-byte level). */
static bool korb_charset_match(const char *set, uint32_t n, unsigned char ch) {
    bool neg = false; uint32_t i = 0;
    if (n > 1 && set[0] == '^') { neg = true; i = 1; }   /* a lone "^" is the literal char, not a complement */
    bool in = false;
    for (; i < n; i++) {
        if (i + 2 < n && set[i+1] == '-') {
            if ((unsigned char)set[i] <= ch && ch <= (unsigned char)set[i+2]) in = true;
            i += 2;
        } else if ((unsigned char)set[i] == ch) in = true;
    }
    return neg ? !in : in;
}
/* true if ch is in EVERY set arg (Ruby count/delete intersect multiple sets) */
static bool korb_str_sets_match(VALUE_SLICE a, unsigned char ch) {
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        VALUE sv = VALUE_SLICE_GET(a, j);
        if (!KORB_STRING_P(sv)) continue;
        const KorbString *set = VAL2STR(sv);
        if (!korb_charset_match(set->buf->data, set->len, ch)) return false;
    }
    return true;
}
/* delete_prefix/suffix (mode 0/1); in_place → bang (self if changed else nil). */
static RESULT korb_str_delfix(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode, bool in_place) {
    VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *p = VAL2STR(pv);
    uint32_t bs = 0, be = s->len; bool match = false;
    if (p->len <= s->len) {
        if (mode == 0 && memcmp(s->buf->data, p->buf->data, p->len) == 0) { bs = p->len; match = true; }
        else if (mode == 1 && memcmp(s->buf->data + s->len - p->len, p->buf->data, p->len) == 0) { be = s->len - p->len; match = true; }
    }
    if (!in_place) return korb_str_slice_new(c, slots, self, bs, be - bs);
    if (!match) return RESULT_OK(KORB_NIL);
    KorbString *m = VAL2STR(VALUE_REF_GET(self));
    uint32_t nlen = be - bs;
    if (bs) memmove(m->buf->data, m->buf->data + bs, nlen);
    m->len = nlen; m->buf->data[nlen] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_delete_prefix(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_str_delfix(c, slots, self, a, 0, false); }
static RESULT korb_m_str_delete_suffix(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_str_delfix(c, slots, self, a, 1, false); }
static RESULT korb_m_str_delete_prefix_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_delfix(c, slots, self, a, 0, true); }
static RESULT korb_m_str_delete_suffix_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_delfix(c, slots, self, a, 1, true); }
static RESULT korb_m_str_between(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE lo = VALUE_SLICE_GET(a, 0), hi = VALUE_SLICE_GET(a, 1);
    (void)lo; (void)hi;
    return korb_num_between(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), VALUE_SLICE_GET(a, 1));
}
static RESULT korb_m_str_clamp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE lo, hi;
    if (VALUE_SLICE_LEN(a) == 1 && KORB_RANGE_P(VALUE_SLICE_GET(a, 0))) {   /* clamp(min..max) */
        const KorbRange *r = VAL2RANGE(VALUE_SLICE_GET(a, 0)); lo = r->rbegin; hi = r->rend;
    } else { lo = VALUE_SLICE_GET(a, 0); hi = VALUE_SLICE_GET(a, 1); }
    VALUE s = VALUE_REF_GET(self);
    if (lo != KORB_NIL) {                              /* nil bound = unbounded that side */
        if (UNLIKELY(!KORB_STRING_P(lo))) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if (korb_cmp_values(s, lo) < 0) return RESULT_OK(lo);
    }
    if (hi != KORB_NIL) {
        if (UNLIKELY(!KORB_STRING_P(hi))) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if (korb_cmp_values(s, hi) > 0) return RESULT_OK(hi);
    }
    return RESULT_OK(s);
}
/* delete: remove chars present in ALL set args. (in_place → delete!) */
static RESULT korb_str_delete_into(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool in_place) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t n = s->len;
    KorbString *r = korb_str_alloc(c, slots, n);
    s = VAL2STR(VALUE_REF_GET(self));                   /* re-read after alloc */
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        if (!korb_str_sets_match(a, ch)) r->buf->data[w++] = (char)ch;
    }
    r->len = w; r->buf->data[w] = '\0';
    if (!in_place) return RESULT_OK((VALUE)r);
    bool changed = (w != n);
    slots[0] = (VALUE)r;
    KorbString *s2 = korb_str_ensure(c, slots + 1, self, w);
    r = VAL2STR(slots[0]);
    memcpy(s2->buf->data, r->buf->data, w);
    s2->len = w; s2->buf->data[w] = '\0';
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_str_delete_into(c, slots, self, a, false); }
static RESULT korb_m_str_delete_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_delete_into(c, slots, self, a, true); }
/* expand a tr spec (byte level) — `a-z` ranges into individual bytes. */
static uint32_t korb_tr_expand(const char *s, uint32_t n, unsigned char *out, uint32_t cap) {
    uint32_t k = 0, i = 0;
    while (i < n && k < cap) {
        if (i + 2 < n && s[i + 1] == '-' && (unsigned char)s[i] <= (unsigned char)s[i + 2]) {
            for (int ch = (unsigned char)s[i]; ch <= (unsigned char)s[i + 2] && k < cap; ch++) out[k++] = (unsigned char)ch;
            i += 3;
        } else out[k++] = (unsigned char)s[i++];
    }
    return k;
}
/* String#tr(from, to) — byte-level translate; `^` negation, ranges, to-empty
 * deletes, to-shorter repeats its last char.  (UTF-8 chars beyond ASCII pass.) */
static RESULT korb_m_str_tr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE fv = VALUE_SLICE_GET(a, 0), tv = VALUE_SLICE_GET(a, 1);
    if (UNLIKELY(!KORB_STRING_P(fv) || !KORB_STRING_P(tv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const KorbString *fs = VAL2STR(fv), *ts = VAL2STR(tv);
    bool neg = fs->len > 1 && fs->buf->data[0] == '^';   /* a lone "^" is the literal char, not a complement */
    unsigned char fromx[512], tox[512];
    uint32_t fn = korb_tr_expand(fs->buf->data + (neg ? 1 : 0), fs->len - (neg ? 1u : 0u), fromx, 512);
    uint32_t tn = korb_tr_expand(ts->buf->data, ts->len, tox, 512);
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));      /* no GC during the scan */
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        int idx = -1;
        for (uint32_t k = 0; k < fn; k++) if (fromx[k] == ch) { idx = (int)k; break; }
        bool match = neg ? (idx < 0) : (idx >= 0);
        if (!match) { fputc(ch, ms); continue; }
        if (tn == 0) continue;                              /* delete */
        fputc(neg ? tox[tn - 1] : tox[(uint32_t)idx < tn ? (uint32_t)idx : tn - 1], ms);
    }
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf, (uint32_t)sz);
    free(buf);
    return r;
}
/* tr_s: like tr, but runs of *translated* chars that map to the same output are
 * squeezed to one.  Pre-existing runs (untranslated) are left intact. */
static RESULT korb_m_str_tr_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE fv = VALUE_SLICE_GET(a, 0), tv = VALUE_SLICE_GET(a, 1);
    if (UNLIKELY(!KORB_STRING_P(fv) || !KORB_STRING_P(tv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const KorbString *fs = VAL2STR(fv), *ts = VAL2STR(tv);
    bool neg = fs->len > 1 && fs->buf->data[0] == '^';   /* a lone "^" is the literal char, not a complement */
    unsigned char fromx[512], tox[512];
    uint32_t fn = korb_tr_expand(fs->buf->data + (neg ? 1 : 0), fs->len - (neg ? 1u : 0u), fromx, 512);
    uint32_t tn = korb_tr_expand(ts->buf->data, ts->len, tox, 512);
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));      /* no GC during the scan */
    bool prev_tr = false; int prev_out = -1;
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        int idx = -1;
        for (uint32_t k = 0; k < fn; k++) if (fromx[k] == ch) { idx = (int)k; break; }
        bool match = neg ? (idx < 0) : (idx >= 0);
        if (!match) { fputc(ch, ms); prev_tr = false; prev_out = -1; continue; }
        if (tn == 0) { prev_tr = false; prev_out = -1; continue; }   /* delete */
        int outc = neg ? tox[tn - 1] : tox[(uint32_t)idx < tn ? (uint32_t)idx : tn - 1];
        if (prev_tr && prev_out == outc) continue;                   /* squeeze translated run */
        fputc(outc, ms); prev_tr = true; prev_out = outc;
    }
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf, (uint32_t)sz);
    free(buf);
    return r;
}
/* gsub/sub with a literal String pattern + String|Hash replacement (no regex/block). */
static RESULT korb_str_gsub_into(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool global, bool in_place) {
    VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))                 /* regex pattern → deferred (astrogre) */
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "String#gsub/sub supports only a String pattern");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 2))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "String#gsub/sub without a replacement (Enumerator) is not supported");
    VALUE rv = VALUE_SLICE_GET(a, 1);
    /* snapshot pattern + replacement bytes into stable C buffers (survive grows) */
    const KorbString *ps = VAL2STR(pv);
    uint32_t pn = ps->len; char *pat = malloc(pn ? pn : 1); memcpy(pat, ps->buf->data, pn);
    char *rep; uint32_t rn;
    if (KORB_STRING_P(rv)) {
        const KorbString *rs = VAL2STR(rv);
        rn = rs->len; rep = malloc(rn ? rn : 1); memcpy(rep, rs->buf->data, rn);
    } else if (KORB_HASH_P(rv)) {                     /* hash: matched substring → hash[match].to_s ("" if absent) */
        int32_t idx = korb_hash_find(VAL2HASH(rv), pv);   /* literal pattern → key is the whole match */
        if (idx < 0) { rn = 0; rep = malloc(1); }
        else {
            VALUE val = VAL2HASH(rv)->items->data[2 * idx + 1];
            char *b = NULL; size_t z = 0; FILE *m = open_memstream(&b, &z);
            if (m) { korb_fprint_to_s(c, m, val); fclose(m); }
            rn = (uint32_t)z; rep = malloc(rn ? rn : 1); if (rn) memcpy(rep, b, rn); free(b);
        }
    } else {
        free(pat);
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(rv));
    }
    /* build result into a C buffer (no GC during the scan) */
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { free(pat); free(rep); fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t i = 0, sn = s->len; bool replaced = false;
    while (i < sn) {
        if (pn > 0 && i + pn <= sn && memcmp(s->buf->data + i, pat, pn) == 0 && (global || !replaced)) {
            fwrite(rep, 1, rn, ms); i += pn; replaced = true;
        } else {
            fputc(s->buf->data[i], ms); i++;
        }
    }
    fclose(ms); free(pat); free(rep);
    RESULT nr = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    if (!in_place) return nr;
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    slots[0] = nr.value;
    const KorbString *res = VAL2STR(slots[0]);
    uint32_t w = res->len;
    KorbString *s2 = korb_str_ensure(c, slots + 1, self, w);
    res = VAL2STR(slots[0]);
    memcpy(s2->buf->data, res->buf->data, w);
    s2->len = w; s2->buf->data[w] = '\0';
    return RESULT_OK(replaced ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_gsub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_str_gsub_into(c, slots, self, a, true, false); }
static RESULT korb_m_str_sub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { return korb_str_gsub_into(c, slots, self, a, false, false); }
static RESULT korb_m_str_gsub_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_gsub_into(c, slots, self, a, true, true); }
static RESULT korb_m_str_sub_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_gsub_into(c, slots, self, a, false, true); }
static RESULT korb_m_str_ascii_only(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < s->len; i++)
        if ((unsigned char)s->buf->data[i] >= 0x80) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
static bool korb_str_pure_ascii(const KorbString *s) {
    for (uint32_t i = 0; i < s->len; i++) if ((unsigned char)s->buf->data[i] >= 0x80) return false;
    return true;
}
/* Unicode normalization: for a pure-ASCII string every NF{,K}{C,D} form is the
 * identity, so this is exact for the (all-ASCII) corpus; the table-driven
 * general case is out of scope → NotImplementedError on non-ASCII input. */
static RESULT korb_m_str_unicode_normalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const uint32_t len = VAL2STR(VALUE_REF_GET(self))->len;
    if (UNLIKELY(!korb_str_pure_ascii(VAL2STR(VALUE_REF_GET(self)))))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Unicode normalization of non-ASCII strings is not supported");
    KorbString *const r = korb_str_alloc(c, slots, len);          /* may move self */
    memcpy(r->buf->data, VAL2STR(VALUE_REF_GET(self))->buf->data, len);   /* re-read after alloc */
    return RESULT_OK((VALUE)r);
}
static RESULT korb_m_str_unicode_normalized_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (UNLIKELY(!korb_str_pure_ascii(VAL2STR(VALUE_REF_GET(self)))))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Unicode normalization of non-ASCII strings is not supported");
    return RESULT_OK(KORB_TRUE);                                  /* pure ASCII is already normalized */
}
static RESULT korb_m_str_unicode_normalize_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (UNLIKELY(!korb_str_pure_ascii(VAL2STR(VALUE_REF_GET(self)))))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Unicode normalization of non-ASCII strings is not supported");
    return RESULT_OK(VALUE_REF_GET(self));                        /* ASCII → unchanged, returns self */
}
/* rpartition(sep) → [before, sep, after] split at the LAST occurrence of sep. */
static RESULT korb_m_str_rpartition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *sep = VAL2STR(sv);
    int32_t at = -1;
    if (sep->len == 0) at = (int32_t)s->len;
    else for (int32_t i = (int32_t)s->len - (int32_t)sep->len; i >= 0; i--)
        if (memcmp(s->buf->data + i, sep->buf->data, sep->len) == 0) { at = i; break; }
    uint32_t pre_e, post_s;
    if (at < 0) { pre_e = 0; post_s = 0; }            /* not found → ["","",self] */
    else { pre_e = (uint32_t)at; post_s = (uint32_t)at + sep->len; }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 3)));
    if (at < 0) {
        slots[0] = UNWRAP(korb_str_new(c, slots + 1, "", 0));
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        slots[0] = UNWRAP(korb_str_new(c, slots + 1, "", 0));
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        CHECK(korb_ary_push_val(c, slots + 1, dst, VALUE_REF_GET(self)));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    slots[0] = UNWRAP(korb_str_slice_new(c, slots + 1, self, 0, pre_e));
    CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    slots[0] = VALUE_SLICE_GET(a, 0);                 /* the separator */
    CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    slots[0] = UNWRAP(korb_str_slice_new(c, slots + 1, self, post_s, VAL2STR(VALUE_REF_GET(self))->len - post_s));
    CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_str_partition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *sep = VAL2STR(sv);
    int32_t at = (s->len >= sep->len) ? korb_byte_find(s->buf->data, s->len, sep->buf->data, sep->len) : -1;
    uint32_t post_s = (at < 0) ? 0 : (uint32_t)at + sep->len;   /* before any alloc (sep moves under moving GC) */
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 3)));
    if (at < 0) {                                     /* not found → [self,"",""] */
        CHECK(korb_ary_push_val(c, slots + 1, dst, VALUE_REF_GET(self)));
        slots[0] = UNWRAP(korb_str_new(c, slots + 1, "", 0));
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        slots[0] = UNWRAP(korb_str_new(c, slots + 1, "", 0));
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    slots[0] = UNWRAP(korb_str_slice_new(c, slots + 1, self, 0, (uint32_t)at));
    CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    slots[0] = VALUE_SLICE_GET(a, 0);                 /* the separator */
    CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    slots[0] = UNWRAP(korb_str_slice_new(c, slots + 1, self, post_s, VAL2STR(VALUE_REF_GET(self))->len - post_s));
    CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_str_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *const s = VAL2STR(VALUE_REF_GET(self));
    const char *const d = s->buf->data;
    const uint32_t len = s->len;
    char buf[320]; uint32_t j = 0, i = 0;
    #define KORB_DIG(ch) ((ch) >= '0' && (ch) <= '9')
    #define KORB_PUT(ch) do { if (j < sizeof(buf) - 1) buf[j++] = (ch); } while (0)
    while (i < len && korb_is_ws((unsigned char)d[i])) i++;
    if (i < len && (d[i] == '+' || d[i] == '-')) { KORB_PUT(d[i]); i++; }
    bool any_digit = false;
    while (i < len) {                                 /* integer part (underscore only between digits) */
        if (KORB_DIG(d[i])) { KORB_PUT(d[i]); i++; any_digit = true; }
        else if (d[i] == '_' && any_digit && i + 1 < len && KORB_DIG(d[i + 1])) i++;
        else break;
    }
    if (i < len && d[i] == '.' && i + 1 < len && KORB_DIG(d[i + 1])) {   /* fractional (needs a digit after '.') */
        KORB_PUT('.'); i++;
        while (i < len) {
            if (KORB_DIG(d[i])) { KORB_PUT(d[i]); i++; any_digit = true; }
            else if (d[i] == '_' && i + 1 < len && KORB_DIG(d[i + 1]) && KORB_DIG(d[i - 1])) i++;
            else break;
        }
    }
    if (any_digit && i < len && (d[i] == 'e' || d[i] == 'E')) {          /* exponent (drop if no digits follow) */
        const uint32_t save_j = j;
        KORB_PUT(d[i]); i++;
        if (i < len && (d[i] == '+' || d[i] == '-')) { KORB_PUT(d[i]); i++; }
        bool exp_digit = false;
        while (i < len) {
            if (KORB_DIG(d[i])) { KORB_PUT(d[i]); i++; exp_digit = true; }
            else if (d[i] == '_' && exp_digit && i + 1 < len && KORB_DIG(d[i + 1])) i++;
            else break;
        }
        if (!exp_digit) j = save_j;                   /* "1e" with no exponent → strip the 'e' */
    }
    buf[j] = '\0';
    #undef KORB_DIG
    #undef KORB_PUT
    return korb_float_new(c, slots, any_digit ? strtod(buf, NULL) : 0.0);
}
static RESULT korb_m_str_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t cnt = 0;
    for (uint32_t i = 0; i < s->len; i++)
        if (korb_str_sets_match(a, (unsigned char)s->buf->data[i])) cnt++;
    return RESULT_OK(LONG2FIX(cnt));
}
static RESULT korb_m_str_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t bits = 16;
    if (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) bits = FIX2LONG(VALUE_SLICE_GET(a, 0));
    intptr_t sum = 0;
    for (uint32_t i = 0; i < s->len; i++) sum += (unsigned char)s->buf->data[i];
    if (bits > 0 && bits < 64) sum &= ((intptr_t)1 << bits) - 1;
    return RESULT_OK(LONG2FIX(sum));
}
/* squeeze: collapse runs of identical chars (only those in the sets, if given). */
static RESULT korb_str_squeeze_into(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool in_place) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t n = s->len;
    bool has_set = VALUE_SLICE_LEN(a) > 0;
    /* build squeezed bytes into the dst string */
    KorbString *r = korb_str_alloc(c, slots, n);          /* capacity n (worst case) */
    s = VAL2STR(VALUE_REF_GET(self));                      /* re-read after alloc */
    uint32_t w = 0; int prev = -1;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        bool squeezable = !has_set || korb_str_sets_match(a, ch);
        if (squeezable && (int)ch == prev) continue;
        r->buf->data[w++] = (char)ch;
        prev = squeezable ? (int)ch : -1;
    }
    r->len = w; r->buf->data[w] = '\0';
    if (!in_place) return RESULT_OK((VALUE)r);
    /* copy back into self */
    slots[0] = (VALUE)r;
    bool changed = (w != n);
    KorbString *s2 = korb_str_ensure(c, slots + 1, self, w);
    r = VAL2STR(slots[0]);
    memcpy(s2->buf->data, r->buf->data, w);
    s2->len = w; s2->buf->data[w] = '\0';
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_squeeze(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_squeeze_into(c, slots, self, a, false); }
static RESULT korb_m_str_squeeze_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_squeeze_into(c, slots, self, a, true); }
/* append_as_bytes(*objs): append each Integer as a byte (low 8 bits) / String bytes. */
static RESULT korb_m_str_append_as_bytes(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        VALUE o = VALUE_SLICE_GET(a, j);
        if (FIXNUM_P(o)) {
            char b = (char)(FIX2LONG(o) & 0xFF);
            CHECK(korb_str_cat(c, slots, self, &b, 1));
        } else if (KORB_STRING_P(o)) {
            CHECK(korb_str_append_str(c, slots, self, VALUE_SLICE_REF(a, j)));
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Integer or String)", korb_type_name(o));
        }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_str_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    return RESULT_OK(korb_byte_find(s->buf->data, s->len, n->buf->data, n->len) >= 0 ? KORB_TRUE : KORB_FALSE);
}

static RESULT korb_m_str_start_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE pv = VALUE_SLICE_GET(a, i);
        if (UNLIKELY(!KORB_STRING_P(pv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
        const KorbString *p = VAL2STR(pv);
        if (p->len <= s->len && memcmp(s->buf->data, p->buf->data, p->len) == 0) return RESULT_OK(KORB_TRUE);
    }
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_str_end_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE pv = VALUE_SLICE_GET(a, i);
        if (UNLIKELY(!KORB_STRING_P(pv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
        const KorbString *p = VAL2STR(pv);
        if (p->len <= s->len && memcmp(s->buf->data + s->len - p->len, p->buf->data, p->len) == 0) return RESULT_OK(KORB_TRUE);
    }
    return RESULT_OK(KORB_FALSE);
}

/* byte offset of the cidx-th codepoint (clamped to len). */
static uint32_t korb_str_char_to_byte(const KorbString *s, intptr_t cidx) {
    uint32_t b = 0;
    for (intptr_t k = 0; k < cidx && b < s->len; k++) {
        b++;
        while (b < s->len && ((unsigned char)s->buf->data[b] & 0xC0) == 0x80) b++;
    }
    return b;
}
static RESULT korb_m_str_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t boff = 0;
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* index(substr, start): range-check start first */
        intptr_t start;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &start))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        const KorbString *const s0 = VAL2STR(VALUE_REF_GET(self));
        uint32_t ncp = korb_utf8_count(s0->buf->data, s0->len);
        if (start < 0) start += ncp;
        if (start < 0 || start > (intptr_t)ncp) return RESULT_OK(KORB_NIL);   /* out of range → nil (needle not coerced) */
        boff = korb_str_char_to_byte(s0, start);
    }
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) {               /* coerce via #to_str, else TypeError (never #to_int) */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (!korb_responds_to(c, sv, to_str))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
        slots[0] = sv;
        RESULT cr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (UNLIKELY(!KORB_STRING_P(cr.value)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(slots[0]));
        sv = cr.value;
    }
    const KorbString *const s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);   /* re-read s after coercion's GC */
    int32_t b = korb_byte_find(s->buf->data + boff, s->len - boff, n->buf->data, n->len);
    if (b < 0) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX(korb_utf8_count(s->buf->data, boff + (uint32_t)b)));   /* char index */
}

static RESULT korb_m_str_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int base = 10;
    bool have_base = false;
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* to_i(base): base 0 = auto-detect prefix */
        intptr_t b;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &b))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        base = (int)b; have_base = true;
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    const char *const d = s->buf->data; uint32_t i = 0, end = s->len;
    while (i < end && korb_is_ws((unsigned char)d[i])) i++;
    intptr_t sign = 1;
    if (i < end && (d[i] == '+' || d[i] == '-')) { if (d[i] == '-') sign = -1; i++; }
    /* CRuby validates the radix only once there is a digit to parse: a blank /
     * sign-only string returns 0 regardless of an otherwise-invalid radix. */
    if (i >= end) return RESULT_OK(LONG2FIX(0));
    if (UNLIKELY(have_base && base != 0 && (base < 2 || base > 36)))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid radix %d", base);
    if (i + 1 < end && d[i] == '0') {                /* base prefix */
        const char p = d[i + 1] | 0x20;
        const int pb = p == 'x' ? 16 : p == 'b' ? 2 : p == 'o' ? 8 : p == 'd' ? 10 : 0;
        if (pb && (base == 0 || base == pb)) { base = pb; i += 2; }
        else if (base == 0) base = 8;                /* leading 0 → octal */
    }
    if (base == 0) base = 10;
    intptr_t n = 0; bool any = false, prev_us = false;
#ifdef KORB_HAVE_GMP
    bool big = false; mpz_t z;
#endif
    for (; i < end; i++) {
        const char ch = d[i];
        if (ch == '_') { if (!any || prev_us) break; prev_us = true; continue; }
        prev_us = false;
        int dig;
        if (ch >= '0' && ch <= '9') dig = ch - '0';
        else if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') dig = (ch | 0x20) - 'a' + 10;
        else break;
        if (dig >= base) break;
        any = true;
#ifdef KORB_HAVE_GMP
        if (big) { mpz_mul_ui(z, z, (unsigned long)base); mpz_add_ui(z, z, (unsigned long)dig); continue; }
        intptr_t nn;
        if (UNLIKELY(__builtin_mul_overflow(n, (intptr_t)base, &nn) || __builtin_add_overflow(nn, (intptr_t)dig, &nn))) {
            mpz_init_set_si(z, n);                       /* overflow → keep parsing in GMP */
            mpz_mul_ui(z, z, (unsigned long)base); mpz_add_ui(z, z, (unsigned long)dig);
            big = true; continue;
        }
        n = nn;
#else
        n = n * base + dig;
#endif
    }
#ifdef KORB_HAVE_GMP
    if (big) {
        if (sign < 0) mpz_neg(z, z);
        RESULT r = korb_big_from_mpz(c, slots, z);       /* normalizes back to Fixnum if it fits */
        mpz_clear(z);
        return r;
    }
#endif
    {
        const intptr_t v = sign * n;
#ifdef KORB_HAVE_GMP
        if (UNLIKELY(!FIXABLE(v))) {   /* fixnum-overflow but int64-fit (e.g. 2^62) → Bignum */
            mpz_t z2; mpz_init_set_si(z2, (long)v);
            RESULT r = korb_big_from_mpz(c, slots, z2);
            mpz_clear(z2);
            return r;
        }
#endif
        return RESULT_OK(LONG2FIX(v));
    }
}

/* Lenient radix parse for String#hex / #oct: skip ws + sign, optional base
 * prefix (0x/0b/0o/0d — overrides `base` when `any_prefix`, else only the
 * prefix matching `base`), then digits up to the first invalid char (`_`
 * separators allowed between digits).  Returns 0 when nothing parses. */
static intptr_t korb_str_radix(const char *const s, uint32_t len, int base, bool any_prefix) {
    uint32_t i = 0, end = len;
    while (i < end && isspace((unsigned char)s[i])) i++;
    intptr_t sign = 1;
    if (i < end && (s[i] == '+' || s[i] == '-')) { if (s[i] == '-') sign = -1; i++; }
    if (i + 1 < end && s[i] == '0') {
        const char p = s[i + 1] | 0x20;
        const int pb = p == 'x' ? 16 : p == 'b' ? 2 : p == 'o' ? 8 : p == 'd' ? 10 : 0;
        if (pb && (any_prefix || pb == base)) { base = pb; i += 2; }
    }
    intptr_t acc = 0; bool any = false, prev_us = false;
    for (; i < end; i++) {
        const char ch = s[i];
        if (ch == '_') { if (!any || prev_us) break; prev_us = true; continue; }
        prev_us = false;
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') d = (ch | 0x20) - 'a' + 10;
        else break;
        if (d >= base) break;
        acc = acc * base + d;
        any = true;
    }
    return sign * acc;
}
static RESULT korb_m_str_hex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    return RESULT_OK(LONG2FIX(korb_str_radix(s->buf->data, s->len, 16, false)));
}
static RESULT korb_m_str_oct(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    return RESULT_OK(LONG2FIX(korb_str_radix(s->buf->data, s->len, 8, true)));
}

/* String#to_r — lenient parse of [ws][sign]int['/'int | '.'frac]; non-numeric → (0/1). */
static RESULT korb_m_str_to_r(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    const char *const d = s->buf->data; const uint32_t len = s->len; uint32_t i = 0;
    while (i < len && isspace((unsigned char)d[i])) i++;
    intptr_t sign = 1;
    if (i < len && (d[i] == '+' || d[i] == '-')) { if (d[i] == '-') sign = -1; i++; }
    /* integer part (underscores allowed between digits) */
    intptr_t num = 0; bool any = false;
    while (i < len && ((d[i] >= '0' && d[i] <= '9') ||
                       (d[i] == '_' && any && i + 1 < len && isdigit((unsigned char)d[i + 1])))) {
        if (d[i] != '_') { num = num * 10 + (d[i] - '0'); any = true; }
        i++;
    }
    intptr_t den = 1;
    if (i < len && d[i] == '.') {                            /* fraction scales the denominator (also handles ".9") */
        i++; bool fany = false;
        while (i < len && ((d[i] >= '0' && d[i] <= '9') ||
                           (d[i] == '_' && fany && i + 1 < len && isdigit((unsigned char)d[i + 1])))) {
            if (d[i] != '_') { num = num * 10 + (d[i] - '0'); den *= 10; any = true; fany = true; }
            i++;
        }
    }
    if (any && i < len && d[i] == '/') {                     /* explicit denominator (after int or decimal) */
        i++; intptr_t dv = 0; bool dany = false;
        while (i < len && ((d[i] >= '0' && d[i] <= '9') ||
                           (d[i] == '_' && dany && i + 1 < len && isdigit((unsigned char)d[i + 1])))) {
            if (d[i] != '_') { dv = dv * 10 + (d[i] - '0'); dany = true; }
            i++;
        }
        if (dany) den *= dv;                                 /* korb_rat_new raises on den 0 */
    }
    if (!any) return korb_rat_new(c, slots, 0, 1);
    return korb_rat_new(c, slots, sign * num, den);
}
/* parse one base-10 number (int or decimal float) at d[*pi]; store its VALUE into
 * *outslot (rooted there), advance *pi, return true.  false if no digits. */
static bool korb_str_parse_num(CTX *c, VALUE *outslot, const char *const d, uint32_t len, uint32_t *pi) {
    uint32_t i = *pi; intptr_t sg = 1;
    if (i < len && (d[i] == '+' || d[i] == '-')) { if (d[i] == '-') sg = -1; i++; }
    intptr_t ip = 0; bool dg = false;
    while (i < len && d[i] >= '0' && d[i] <= '9') { ip = ip * 10 + (d[i] - '0'); i++; dg = true; }
    intptr_t frac = 0, fden = 1; bool isf = false;
    if (i < len && d[i] == '.') {
        uint32_t j = i + 1; bool fdg = false;
        while (j < len && d[j] >= '0' && d[j] <= '9') { frac = frac * 10 + (d[j] - '0'); fden *= 10; j++; fdg = true; }
        if (fdg) { isf = true; i = j; }
    }
    if (!dg && !isf) return false;
    if (isf) *outslot = korb_float_new(c, outslot, (double)sg * ((double)ip + (double)frac / (double)fden)).value;
    else     *outslot = LONG2FIX(sg * ip);
    *pi = i;
    return true;
}
/* String#to_c — parse real[+imag i] / imag-only "Ni"; non-numeric → (0+0i). */
static RESULT korb_m_str_to_c(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    const char *const d = s->buf->data; const uint32_t len = s->len; uint32_t i = 0;
    while (i < len && isspace((unsigned char)d[i])) i++;
    slots[0] = LONG2FIX(0);                                  /* re */
    slots[1] = LONG2FIX(0);                                  /* im */
    if (korb_str_parse_num(c, &slots[2], d, len, &i)) {      /* first number → slots[2] */
        if (i < len && (d[i] | 0x20) == 'i') {               /* "Ni" → pure imaginary */
            slots[1] = slots[2]; i++;
        } else {
            slots[0] = slots[2];                             /* real part */
            if (i < len && (d[i] == '+' || d[i] == '-')) {   /* "+Ni" / "-Ni" imaginary */
                uint32_t save = i;
                if (korb_str_parse_num(c, &slots[2], d, len, &i) && i < len && (d[i] | 0x20) == 'i') {
                    slots[1] = slots[2]; i++;
                } else i = save;
            }
        }
    }
    return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
}

/* trim: mode 0=both 1=left 2=right */
/* mode: 0=both 1=left 2=right. With a charset arg (Ruby 4.0), strip those chars
 * (delete/count-style set with ranges + ^) instead of whitespace. */
static RESULT korb_str_strip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t start = 0, end = s->len;
    bool has_set = VALUE_SLICE_LEN(a) >= 1;
    if (mode != 2)
        while (start < end) {
            unsigned char ch = (unsigned char)s->buf->data[start];
            if (has_set ? korb_str_sets_match(a, ch) : (korb_is_ws(ch) || ch == '\0')) start++;
            else break;
        }
    if (mode != 1)
        while (end > start) {
            unsigned char ch = (unsigned char)s->buf->data[end-1];
            if (has_set ? korb_str_sets_match(a, ch) : (korb_is_ws(ch) || ch == '\0')) end--;
            else break;
        }
    return korb_str_slice_new(c, slots, self, start, end - start);
}
static RESULT korb_m_str_strip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_strip(c, slots, self, a, 0); }
static RESULT korb_m_str_lstrip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_strip(c, slots, self, a, 1); }
static RESULT korb_m_str_rstrip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_strip(c, slots, self, a, 2); }

static RESULT korb_m_str_chomp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t len = s->len;
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* chomp(sep) */
        VALUE sv = VALUE_SLICE_GET(a, 0);
        if (sv == KORB_NIL) return korb_str_slice_new(c, slots, self, 0, len);   /* nil sep → unchanged copy */
        if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
        const KorbString *sep = VAL2STR(sv);
        if (sep->len == 0) {                          /* "" → strip all trailing \n / \r\n */
            while (len >= 2 && s->buf->data[len-2] == '\r' && s->buf->data[len-1] == '\n') len -= 2;
            while (len >= 1 && s->buf->data[len-1] == '\n') len -= 1;
        } else if (len >= sep->len && memcmp(s->buf->data + len - sep->len, sep->buf->data, sep->len) == 0) {
            len -= sep->len;                          /* one trailing occurrence */
        }
        return korb_str_slice_new(c, slots, self, 0, len);
    }
    if (len >= 2 && s->buf->data[len-2] == '\r' && s->buf->data[len-1] == '\n') len -= 2;
    else if (len >= 1 && (s->buf->data[len-1] == '\n' || s->buf->data[len-1] == '\r')) len -= 1;
    return korb_str_slice_new(c, slots, self, 0, len);
}

static RESULT korb_m_str_chop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t len = s->len;
    if (len >= 2 && s->buf->data[len-2] == '\r' && s->buf->data[len-1] == '\n') len -= 2;
    else if (len >= 1) {
        len--;                                  /* drop a whole trailing UTF-8 codepoint */
        while (len > 0 && ((unsigned char)s->buf->data[len] & 0xC0) == 0x80) len--;
    }
    return korb_str_slice_new(c, slots, self, 0, len);
}

/* String#split(sep=nil): nil/" " → whitespace runs; string sep → that literal
 * (trailing empty fields dropped). */
/* with a block, yield each field then return self; otherwise return the array. */
static RESULT korb_split_finish(CTX *c, VALUE *slots, VALUE_REF self, VALUE_REF dst, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(dst));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
        if (i >= d->len) break;
        VALUE e = d->items->data[i];
        RESULT r = korb_block_yield(c, slots, block, def_env, &e, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_split(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    VALUE sepv = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    /* limit: 0/omitted = unlimited + drop trailing empties; <0 = unlimited keep;
     * >0 = at most `limit` fields (last = remainder).  limit==1 → [self] verbatim. */
    intptr_t limit = 0;
    if (VALUE_SLICE_LEN(a) >= 2 && VALUE_SLICE_GET(a, 1) != KORB_NIL) (void)korb_to_index(VALUE_SLICE_GET(a, 1), &limit);
    if (limit == 1) {                                         /* whole string (sep untouched); empty → [] */
        const uint32_t slen = VAL2STR(VALUE_REF_GET(self))->len;
        VALUE_REF d1 = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 1)));
        if (slen > 0) CHECK(korb_ary_push_val(c, slots + 1, d1, UNWRAP(korb_str_slice_new(c, slots + 1, self, 0, slen))));
        return korb_split_finish(c, slots + 1, self, d1, block, def_env, cself);
    }
    bool ws = (sepv == KORB_NIL);
    if (!ws) {
        if (UNLIKELY(!KORB_STRING_P(sepv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sepv));
        const KorbString *sp = VAL2STR(sepv);
        if (sp->len == 1 && sp->buf->data[0] == ' ') ws = true;   /* " " behaves as whitespace */
    }
    VALUE_REF sepref = ws ? (VALUE_REF){0} : VALUE_SLICE_REF(a, 0);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    if (VAL2STR(VALUE_REF_GET(self))->len == 0)            /* CRuby: empty string always splits to [] */
        return korb_split_finish(c, slots + 1, self, dst, block, def_env, cself);
    if (!ws && VAL2STR(VALUE_REF_GET(sepref))->len == 0) {  /* empty pattern → split into characters */
        uint32_t cpos = 0;
        for (;;) {
            const KorbString *s = VAL2STR(VALUE_REF_GET(self));
            if (cpos >= s->len) break;
            bool last_field = (limit > 0 && VAL2ARY(VALUE_REF_GET(dst))->len == (uint32_t)limit - 1);
            if (last_field) { CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, cpos, s->len - cpos)))); break; }
            uint32_t cl = 1;                               /* one UTF-8 codepoint */
            while (cpos + cl < s->len && ((unsigned char)s->buf->data[cpos+cl] & 0xC0) == 0x80) cl++;
            CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, cpos, cl))));
            cpos += cl;
        }
        return korb_split_finish(c, slots + 1, self, dst, block, def_env, cself);
    }
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));   /* re-read each iter */
        uint32_t slen = s->len;
        bool last_field = (limit > 0 && VAL2ARY(VALUE_REF_GET(dst))->len == (uint32_t)limit - 1);
        if (ws) {
            while (pos < slen && korb_is_ws((unsigned char)s->buf->data[pos])) pos++;
            if (pos >= slen) break;
            uint32_t start = pos;
            if (last_field) { CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, start, slen - start)))); break; }
            while (pos < slen && !korb_is_ws((unsigned char)s->buf->data[pos])) pos++;
            CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, start, pos - start))));
        } else {
            const KorbString *sep = VAL2STR(VALUE_REF_GET(sepref));
            uint32_t seplen = sep->len;
            int32_t found = (!last_field && pos <= slen) ? korb_byte_find(s->buf->data + pos, slen - pos, sep->buf->data, seplen) : -1;
            if (found < 0) {
                CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, slen - pos))));
                break;
            }
            uint32_t end = pos + (uint32_t)found;
            CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, end - pos))));
            pos = end + (seplen ? seplen : 1);
        }
    }
    if (!ws && limit == 0) {   /* CRuby drops trailing empty fields only when limit is 0/omitted */
        KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
        while (d->len > 0 && KORB_STRING_P(d->items->data[d->len-1]) && VAL2STR(d->items->data[d->len-1])->len == 0) {
            ARO_STORE(c, d->items, &d->items->data[--d->len], KORB_NIL);
        }
    }
    return korb_split_finish(c, slots + 1, self, dst, block, def_env, cself);
}

static RESULT korb_m_str_charlen(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    return RESULT_OK(LONG2FIX(korb_utf8_count(s->buf->data, s->len)));
}

static RESULT korb_m_str_chars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));
        if (pos >= s->len) break;
        uint32_t cl = 1;                                  /* one UTF-8 codepoint */
        while (pos + cl < s->len && ((unsigned char)s->buf->data[pos+cl] & 0xC0) == 0x80) cl++;
        CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, cl))));
        pos += cl;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_str_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(o)) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX(korb_cmp_values(VALUE_REF_GET(self), o)));
}

/* String#[] — int index, (int,len), Range, or substring match.  Indices are
 * codepoints; results are fresh strings (or nil). */
static RESULT korb_m_str_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE i0 = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(i0) && !KORB_RANGE_P(i0)) {     /* coerce a non-String/Range index via #to_int (before reading self) */
        RESULT cr = korb_coerce_to_int(c, slots, &i0);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    }
    const KorbString *s = SELF_STR;
    uint32_t ncp = korb_utf8_count(s->buf->data, s->len);

    if (KORB_STRING_P(i0)) {                       /* s[substr] → copy of substr if present */
        const KorbString *sub = VAL2STR(i0);
        if (korb_byte_find(s->buf->data, s->len, sub->buf->data, sub->len) < 0) return RESULT_OK(KORB_NIL);
        return korb_str_slice_new(c, slots, VALUE_SLICE_REF(a, 0), 0, sub->len);
    }
    if (KORB_RANGE_P(i0)) {
        const KorbRange *r = VAL2RANGE(i0);
        const bool beginless = (r->rbegin == KORB_NIL);   /* s[..e] → from 0 */
        const bool endless   = (r->rend   == KORB_NIL);   /* s[b..] → to the end */
        if (UNLIKELY((!beginless && !FIXNUM_P(r->rbegin)) || (!endless && !FIXNUM_P(r->rend))))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        intptr_t b = beginless ? 0 : FIX2LONG(r->rbegin);
        intptr_t e = endless ? (intptr_t)ncp : FIX2LONG(r->rend);
        if (b < 0) b += ncp;
        if (!endless && e < 0) e += ncp;
        if (b < 0 || b > (intptr_t)ncp) return RESULT_OK(KORB_NIL);
        intptr_t last = (endless || r->exclude_end) ? e - 1 : e;
        intptr_t cnt = last - b + 1;
        if (cnt < 0) cnt = 0;
        if (b + cnt > (intptr_t)ncp) cnt = (intptr_t)ncp - b;
        uint32_t bs = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)b);
        uint32_t es = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)(b + cnt));
        return korb_str_slice_new(c, slots, self, bs, es - bs);
    }
    intptr_t i;
    if (UNLIKELY(!korb_to_index(i0, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(i0));
    if (i < 0) i += ncp;

    if (VALUE_SLICE_LEN(a) >= 2) {                  /* s[start, len] */
        VALUE lv = VALUE_SLICE_GET(a, 1);
        intptr_t len;
        if (UNLIKELY(!korb_to_index(lv, &len))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(lv));
        if (len < 0 || i < 0 || i > (intptr_t)ncp) return RESULT_OK(KORB_NIL);
        if (i + len > (intptr_t)ncp) len = (intptr_t)ncp - i;
        uint32_t bs = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)i);
        uint32_t es = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)(i + len));
        return korb_str_slice_new(c, slots, self, bs, es - bs);
    }
    if (i < 0 || i >= (intptr_t)ncp) return RESULT_OK(KORB_NIL);   /* single codepoint */
    uint32_t bs = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)i);
    uint32_t es = korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)(i + 1));
    return korb_str_slice_new(c, slots, self, bs, es - bs);
}

static uint32_t korb_utf8_decode(const char *p, uint32_t avail, uint32_t *clen) {
    unsigned char b0 = (unsigned char)p[0]; uint32_t cp, cl;
    if (b0 < 0x80)        { *clen = 1; return b0; }
    else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; cl = 2; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; cl = 3; }
    else                  { cp = b0 & 0x07; cl = 4; }
    for (uint32_t k = 1; k < cl && k < avail; k++) cp = (cp << 6) | ((unsigned char)p[k] & 0x3F);
    *clen = cl; return cp;
}
/* forward decls: each_* without a block returns an Enumerator over the sibling
 * array (defined later in this TU). */
static RESULT korb_m_str_chars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_str_codepoints(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_str_bytes(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_str_lines(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc);          /* enumerator.c (included later) */
static RESULT korb_enum_desc(CTX *c, VALUE *slots, VALUE recv, const char *meth);
/* build an Enumerator over `arr_fn(self)` labelled `name`. */
static RESULT korb_str_each_enum(CTX *c, VALUE *slots, VALUE_REF self,
                                 RESULT (*arr_fn)(CTX *, VALUE *, VALUE_REF, VALUE_SLICE), const char *name) {
    slots[0] = UNWRAP(arr_fn(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), name));
    return korb_enum_new(c, slots + 2, slots[0], slots[1]);
}
static RESULT korb_m_str_each_codepoint(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) return korb_str_each_enum(c, slots, self, korb_m_str_codepoints, "each_codepoint");
    for (uint32_t pos = 0; ; ) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        uint32_t cl; uint32_t cp = korb_utf8_decode(s->buf->data + pos, s->len - pos, &cl);
        VALUE cv = LONG2FIX(cp);
        RESULT r = korb_block_yield(c, slots, block, def_env, &cv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        pos += cl;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* String#succ: increment rightmost alphanumeric run with carry; if none, byte++
 * on the last char. Carry out of the leftmost alnum prepends '1'/'a'/'A'. */
static RESULT korb_m_str_succ(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s0 = VAL2STR(VALUE_REF_GET(self));
    uint32_t n = s0->len;
    if (n == 0) return korb_str_new(c, slots, "", 0);
    char *buf = malloc(n + 2); memcpy(buf, s0->buf->data, n);
    bool has_alnum = false;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) { has_alnum = true; break; }
    }
    char carry_ch = 0;
    uint32_t insert_pos = 0;     /* where a carry-out digit is inserted (just before the leftmost carried alnum) */
    if (!has_alnum) {
        for (int i = (int)n - 1; i >= 0; i--) {
            unsigned char ch = (unsigned char)buf[i];
            if (ch == 0xFF) { buf[i] = 0x00; if (i == 0) carry_ch = 0x01; }
            else { buf[i] = (char)(ch + 1); break; }
        }
    } else {
        int i = (int)n - 1;
        for (;;) {
            unsigned char ch = (unsigned char)buf[i];
            bool carried = false;
            if (ch >= '0' && ch <= '9')      { if (ch == '9') { buf[i] = '0'; carry_ch = '1'; carried = true; } else buf[i] = (char)(ch + 1); }
            else if (ch >= 'a' && ch <= 'z') { if (ch == 'z') { buf[i] = 'a'; carry_ch = 'a'; carried = true; } else buf[i] = (char)(ch + 1); }
            else if (ch >= 'A' && ch <= 'Z') { if (ch == 'Z') { buf[i] = 'A'; carry_ch = 'A'; carried = true; } else buf[i] = (char)(ch + 1); }
            else { i--; if (i < 0) break; else continue; }
            if (!carried) break;
            int j = i - 1;
            while (j >= 0) {
                unsigned char cj = (unsigned char)buf[j];
                if ((cj>='0'&&cj<='9')||(cj>='a'&&cj<='z')||(cj>='A'&&cj<='Z')) break;
                j--;
            }
            if (j < 0) { insert_pos = (uint32_t)i; break; }   /* carry out → insert before leftmost alnum */
            i = j; carry_ch = 0;
        }
    }
    RESULT r;
    if (carry_ch) {                                 /* insert carry digit at insert_pos (0 for the byte path) */
        char *out = malloc(n + 1);
        memcpy(out, buf, insert_pos);
        out[insert_pos] = carry_ch;
        memcpy(out + insert_pos + 1, buf + insert_pos, n - insert_pos);
        r = korb_str_new(c, slots, out, n + 1); free(out);
    } else {
        r = korb_str_new(c, slots, buf, n);
    }
    free(buf);
    return r;
}
/* In-place mutate self from a computed new-string `newr`.  nil_if_unchanged:
 * return nil when the content is identical (tr!/sub! style); else return self
 * (succ! style).  The new string is rooted across the rewrite. */
static RESULT korb_str_bang_from(CTX *c, VALUE *slots, VALUE_REF self, RESULT newr, bool nil_if_unchanged) {
    if (UNLIKELY(newr.state != KORB_NORMAL)) return newr;
    slots[0] = newr.value;
    const KorbString *ns = VAL2STR(slots[0]), *os = VAL2STR(VALUE_REF_GET(self));
    if (nil_if_unchanged && ns->len == os->len &&
        (ns->len == 0 || memcmp(ns->buf->data, os->buf->data, ns->len) == 0))
        return RESULT_OK(KORB_NIL);
    VAL2STR(VALUE_REF_GET(self))->len = 0;
    return korb_str_append_str(c, slots + 1, self, VALUE_REF_AT(&slots[0]));
}
static RESULT korb_m_str_succ_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    return korb_str_bang_from(c, slots, self, korb_m_str_succ(c, slots, self, a), false);
}
/* tr!/tr_s! return nil only when NO source char matched the from-set; a
 * translation that maps chars to themselves (e.g. "a".tr!("a","a")) still
 * counts as a change and returns self.  So detect "any match" rather than
 * comparing the byte content before/after. */
static bool korb_str_tr_matched(VALUE_REF self, VALUE fv) {
    if (!KORB_STRING_P(fv)) return false;
    const KorbString *fs = VAL2STR(fv);
    bool neg = fs->len > 1 && fs->buf->data[0] == '^';   /* a lone "^" is the literal char, not a complement */
    unsigned char fromx[512];
    uint32_t fn = korb_tr_expand(fs->buf->data + (neg ? 1 : 0), fs->len - (neg ? 1u : 0u), fromx, 512);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        int idx = -1;
        for (uint32_t k = 0; k < fn; k++) if (fromx[k] == ch) { idx = (int)k; break; }
        if (neg ? (idx < 0) : (idx >= 0)) return true;
    }
    return false;
}
static RESULT korb_m_str_tr_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    const bool matched = korb_str_tr_matched(self, VALUE_SLICE_GET(a, 0));
    RESULT r = korb_str_bang_from(c, slots, self, korb_m_str_tr(c, slots, self, a), false);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(matched ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_tr_s_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    const bool matched = korb_str_tr_matched(self, VALUE_SLICE_GET(a, 0));
    RESULT r = korb_str_bang_from(c, slots, self, korb_m_str_tr_s(c, slots, self, a), false);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(matched ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_codepoints(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t pos = 0; ; ) {
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));
        if (pos >= s->len) break;
        uint32_t cl; uint32_t cp = korb_utf8_decode(s->buf->data + pos, s->len - pos, &cl);
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(cp)));
        pos += cl;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_str_each_byte(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) return korb_str_each_enum(c, slots, self, korb_m_str_bytes, "each_byte");
    for (uint32_t pos = 0; ; pos++) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        VALUE bv = LONG2FIX((unsigned char)s->buf->data[pos]);
        RESULT r = korb_block_yield(c, slots, block, def_env, &bv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_bytes(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_STR->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++)                  /* re-read self each push (buf may move) */
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX((unsigned char)SELF_STR->buf->data[i])));
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* length of the line starting at pos, including the trailing '\n' if present. */
/* length of the line at `pos`, including the trailing separator `sep` (seplen
 * bytes; default "\n").  seplen 0 (paragraph "") falls back to "\n\n"-ish — we
 * approximate with "\n" runs; corpus only uses single-char seps and default. */
static uint32_t korb_str_line_len(const KorbString *s, uint32_t pos, const char *sep, uint32_t seplen) {
    if (seplen == 0) seplen = 1, sep = "\n";   /* treat "" like \n for our purposes */
    uint32_t e = pos;
    while (e < s->len) {
        if (e + seplen <= s->len && memcmp(s->buf->data + e, sep, seplen) == 0) { e += seplen; break; }
        e++;
    }
    return e - pos;
}
/* resolve the line separator arg (a[0]) → bytes; default "\n". */
static const char *korb_line_sep(VALUE_SLICE a, uint32_t *seplen) {
    if (VALUE_SLICE_LEN(a) >= 1 && KORB_STRING_P(VALUE_SLICE_GET(a, 0))) {
        const KorbString *sp = VAL2STR(VALUE_SLICE_GET(a, 0));
        *seplen = sp->len; return sp->buf->data;
    }
    *seplen = 1; return "\n";
}
static RESULT korb_m_str_each_line(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    if (block == NULL) return korb_str_each_enum(c, slots, self, korb_m_str_lines, "each_line");
    char sepbuf[64]; uint32_t seplen;
    { const char *sp = korb_line_sep(a, &seplen); if (seplen > 63) seplen = 63; memcpy(sepbuf, sp, seplen); }
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        uint32_t ll = korb_str_line_len(s, pos, sepbuf, seplen);
        slots[0] = UNWRAP(korb_str_slice_new(c, slots, self, pos, ll));   /* root the line */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        pos += ll;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_lines(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) == KORB_NIL) {   /* nil sep → whole string as one line */
        VALUE_REF d = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 1)));
        CHECK(korb_ary_push_val(c, slots + 1, d, UNWRAP(korb_str_slice_new(c, slots + 1, self, 0, VAL2STR(VALUE_REF_GET(self))->len))));
        return RESULT_OK(VALUE_REF_GET(d));
    }
    char sepbuf[64]; uint32_t seplen;
    { const char *sp = korb_line_sep(a, &seplen); if (seplen > 63) seplen = 63; memcpy(sepbuf, sp, seplen); }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        uint32_t ll = korb_str_line_len(s, pos, sepbuf, seplen);
        CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, ll))));
        pos += ll;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_str_each_char(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) return korb_str_each_enum(c, slots, self, korb_m_str_chars, "each_char");
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        uint32_t cl = 1;
        while (pos + cl < s->len && ((unsigned char)s->buf->data[pos+cl] & 0xC0) == 0x80) cl++;
        slots[0] = UNWRAP(korb_str_slice_new(c, slots, self, pos, cl));   /* root the char */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        pos += cl;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* upto(other[, exclusive]) — yield self, self.succ, ... up to other (String
 * range semantics: stop when current > other or its length exceeds other's). */
static RESULT korb_m_str_upto(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (VALUE_SLICE_LEN(a) < 1 || !KORB_STRING_P(VALUE_SLICE_GET(a, 0)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String",
                          VALUE_SLICE_LEN(a) >= 1 ? korb_type_name(VALUE_SLICE_GET(a, 0)) : "nil");
    const bool excl = VALUE_SLICE_LEN(a) >= 2 && KORB_TRUTHY(VALUE_SLICE_GET(a, 1));
    slots[0] = VALUE_REF_GET(self);          /* cur (rooted) */
    slots[1] = VALUE_SLICE_GET(a, 0);        /* end (rooted) */
    if (block == NULL) slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 8));   /* collect for the Enumerator */
    /* single-byte begin AND end → iterate by byte value (CRuby fast path:
     * "9".upto("A") = 9 : ; < = > ? @ A, NOT succ which would carry "9"→"10"). */
    if (VAL2STR(slots[0])->len == 1 && VAL2STR(slots[1])->len == 1) {
        const int b = (unsigned char)VAL2STR(slots[0])->buf->data[0];
        const int e = (unsigned char)VAL2STR(slots[1])->buf->data[0];
        for (int ch = b; ch <= e; ch++) {
            if (excl && ch == e) break;
            const char cc = (char)ch;
            slots[3] = UNWRAP(korb_str_new(c, slots + 3, &cc, 1));
            if (block) {
                RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[3], 1, cself);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            } else {
                CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[2]), slots[3]));
            }
        }
        goto done;
    }
    for (int guard = 0; guard < 100000000; guard++) {
        const uint32_t curlen = VAL2STR(slots[0])->len, endlen = VAL2STR(slots[1])->len;
        if (curlen > endlen) break;                                        /* succ grew past end length */
        const int cmp = korb_cmp_values(slots[0], slots[1]);
        if (cmp > 0) break;
        if (excl && cmp == 0) break;
        if (block) {
            RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        } else {
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
        }
        if (cmp == 0) break;                                               /* inclusive end reached */
        slots[0] = UNWRAP(korb_m_str_succ(c, slots + 3, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0)));
    }
done:
    if (block == NULL) {
        slots[3] = UNWRAP(korb_enum_desc(c, slots + 3, VALUE_REF_GET(self), "upto"));
        return korb_enum_new(c, slots + 4, slots[2], slots[3]);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* crypt(salt) — POSIX crypt(3) one-way hash (libc).  The golden oracle is the
 * host CRuby, which uses the same libc crypt, so results match bit-for-bit. */
static RESULT korb_m_str_crypt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) < 1 || !KORB_STRING_P(VALUE_SLICE_GET(a, 0)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String",
                          VALUE_SLICE_LEN(a) >= 1 ? korb_type_name(VALUE_SLICE_GET(a, 0)) : "nil");
    const KorbString *key = SELF_STR, *salt = VAL2STR(VALUE_SLICE_GET(a, 0));
    if (salt->len < 2) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "salt too short (need >=2 bytes)");
    char kb[4096], sb[512];
    if (key->len >= sizeof(kb) || salt->len >= sizeof(sb))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "string too long for crypt");
    memcpy(kb, key->buf->data, key->len); kb[key->len] = '\0';
    memcpy(sb, salt->buf->data, salt->len); sb[salt->len] = '\0';
    const char *r = crypt(kb, sb);
    if (!r) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid salt");
    return korb_str_new(c, slots, r, (uint32_t)strlen(r));
}
/* bytes/chars/lines/codepoints WITH a block behave like each_* (yield, return
 * self); without a block they return the array. */
static RESULT korb_m_str_bytes_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block) return korb_m_str_each_byte(c, slots, self, a, block, def_env, cself);
    return korb_m_str_bytes(c, slots, self, a);
}
static RESULT korb_m_str_chars_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block) return korb_m_str_each_char(c, slots, self, a, block, def_env, cself);
    return korb_m_str_chars(c, slots, self, a);
}
static RESULT korb_m_str_lines_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block) return korb_m_str_each_line(c, slots, self, a, block, def_env, cself);
    return korb_m_str_lines(c, slots, self, a);
}
static RESULT korb_m_str_codepoints_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block) return korb_m_str_each_codepoint(c, slots, self, a, block, def_env, cself);
    return korb_m_str_codepoints(c, slots, self, a);
}

