/* koruby_precise — string.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- String methods ------------------------------------------------------ */

static RESULT korb_m_str_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_STR->len)); }
static RESULT korb_m_str_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_STR->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_str_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static inline bool korb_str_is_frozen(VALUE v) {
    return AROH_IS_GC_OBJECT(v) && (((const AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_FROZEN);
}
static RESULT korb_str_enc_notimpl(CTX *c, VALUE *slots, VALUE v);                               /* fwd */
/* All bytes 7-bit?  A multi-byte "other" encoding still indexes byte-wise while
 * the content is pure ASCII, so such a string needs no per-encoding hook. */
static bool korb_str_bytes_ascii(VALUE v) {
    const KorbString *const s = VAL2STR(v);
    const char *const d = korb_strbuf_data(s->buf);
    for (uint32_t i = 0; i < s->len; i++) if ((unsigned char)d[i] >= 0x80) return false;
    return true;
}
static inline uint32_t korb_str_char_bytes(uint32_t enc, const unsigned char *p, uint32_t i, uint32_t n);   /* fwd */
/* Is this encoding one byte per character?  The multi-byte families are the
 * short list, so they are the ones named here; everything else (ISO-8859-x,
 * KOI8-x, Windows-125x, IBM/CP 8-bit code pages, TIS-620, macRoman, …) is a
 * single-byte, ASCII-compatible encoding as far as character indexing goes. */
static bool korb_enc_name_single_byte(const char *name) {
    static const char *const multi[] = {
        "UTF-16", "UTF16", "UTF-32", "UTF32", "UTF-7", "UTF7", "EUC", "euc",
        "Shift_JIS", "SHIFT_JIS", "SJIS", "Windows-31J", "CP932", "CP51932",
        "Big5", "BIG5", "GB", "ISO-2022", "stateless-ISO-2022", "Emacs-Mule",
    };
    for (size_t i = 0; i < sizeof multi / sizeof multi[0]; i++)
        if (strncmp(name, multi[i], strlen(multi[i])) == 0) return false;
    return true;
}
/* Map an encoding name to a header index: 0 UTF-8 / 1 US-ASCII / 2 ASCII-8BIT
 * directly; any other name is registered in vm->str_enc_names[3..7] and its
 * slot index returned (character ops on it will raise until hooks exist). */
static uint32_t korb_enc_index_for_name(struct korb_vm *vm, const char *name) {
    if (strcmp(name, "ASCII-8BIT") == 0 || strcmp(name, "BINARY") == 0) return KORB_ENC_BINARY;
    if (strcmp(name, "US-ASCII") == 0 || strcmp(name, "ASCII") == 0 || strcmp(name, "ANSI_X3.4-1968") == 0) return KORB_ENC_USASCII;
    if (strcmp(name, "UTF-8") == 0 || strcmp(name, "UTF8") == 0) return KORB_ENC_UTF8;
    const uint32_t sym = korb_intern(vm, name, (uint32_t)strlen(name));
    for (uint32_t i = KORB_ENC_OTHER_MIN; i < 8; i++) if (vm->str_enc_names[i] == sym) return i;
    /* single-byte names take a 3..5 slot (character ops just work); multi-byte
     * ones take 6..7, where character ops still raise. */
    const bool sb = korb_enc_name_single_byte(name);
    const uint32_t lo = sb ? KORB_ENC_OTHER_MIN : KORB_ENC_SB_MAX + 1;
    const uint32_t hi = sb ? KORB_ENC_SB_MAX : 7u;
    for (uint32_t i = lo; i <= hi; i++) if (vm->str_enc_names[i] == 0) { vm->str_enc_names[i] = sym; return i; }
    return hi;   /* that half of the registry is full: reuse its last slot */
}
/* String#__encoding_tag → the header encoding index (0..7). */
static RESULT korb_m_str_enc_tag(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)a;
    return RESULT_OK(LONG2FIX((intptr_t)KORB_STR_ENC(VALUE_REF_GET(self))));
}
/* String#__encoding_name → the encoding-name String for an "other" index (nil
 * for the three core encodings, which the prelude maps to constants). */
static RESULT korb_m_str_enc_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const uint32_t idx = KORB_STR_ENC(VALUE_REF_GET(self));
    if (idx < KORB_ENC_OTHER_MIN || idx >= 8 || c->vm->str_enc_names[idx] == 0) return RESULT_OK(KORB_NIL);
    const char *nm = korb_sym_name(c->vm, c->vm->str_enc_names[idx]);
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
}
/* String#__set_encoding_tag(n) → set the encoding index in place, return self. */
static RESULT korb_m_str_set_enc_tag(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE s = VALUE_REF_GET(self);
    if (UNLIKELY(korb_str_is_frozen(s)))
        return korb_raise(c, slots, KORB_E_FROZEN, 0, "can't modify frozen String: %s", "");
    const uint32_t n = FIXNUM_P(VALUE_SLICE_GET(a, 0)) ? (uint32_t)FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    KORB_STR_ENC_SET(s, n);
    return RESULT_OK(s);
}
/* String#force_encoding(enc) — set the encoding in place (enc is a name String or
 * an Encoding), return self.  Frozen-checked. */
static RESULT korb_m_str_force_encoding(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE s = VALUE_REF_GET(self);
    if (UNLIKELY(korb_str_is_frozen(s)))
        return korb_raise(c, slots, KORB_E_FROZEN, 0, "can't modify frozen String: %s", "");
    VALUE enc = VALUE_SLICE_GET(a, 0);
    char nbuf[64] = {0};
    if (!KORB_STRING_P(enc)) {                 /* a #to_str-coercible object, or an Encoding (read @name) */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(enc) && korb_responds_to_coerce_p(c, slots, &enc, to_str)) {   /* Encoding has no #to_str → skipped */
            slots[0] = enc;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (!KORB_STRING_P(sr.value)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
            enc = sr.value;
        } else {
            const VALUE nm = KORB_OBJECT_P(enc) ? korb_ivar_get(c, enc, ID2SYM(korb_intern(c->vm, "@name", 5))) : KORB_NIL;
            if (!KORB_STRING_P(nm)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(enc));
            enc = nm;
        }
    }
    { const KorbString *es = VAL2STR(enc);
      uint32_t l = es->len < sizeof nbuf - 1 ? es->len : (uint32_t)sizeof nbuf - 1;
      memcpy(nbuf, korb_strbuf_data(es->buf), l); }
    KORB_STR_ENC_SET(VALUE_REF_GET(self), korb_enc_index_for_name(c->vm, nbuf));
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Length of the valid UTF-8 sequence starting at p[i] (i<n), or 0 if the bytes
 * there are not a valid sequence (incomplete / bad continuation / overlong /
 * out-of-range / surrogate). */
static uint32_t korb_utf8_seq_len(const unsigned char *p, uint32_t i, uint32_t n) {
    const unsigned char b = p[i];
    if (b < 0x80) return 1;
    uint32_t need; uint32_t cp; unsigned char lo = 0x80, hi = 0xBF;
    if ((b & 0xE0) == 0xC0)      { need = 1; cp = b & 0x1F; if (b < 0xC2) return 0; }   /* reject overlong */
    else if ((b & 0xF0) == 0xE0) { need = 2; cp = b & 0x0F; if (b == 0xE0) lo = 0xA0; if (b == 0xED) hi = 0x9F; }
    else if ((b & 0xF8) == 0xF0) { need = 3; cp = b & 0x07; if (b == 0xF0) lo = 0x90; if (b == 0xF4) hi = 0x8F; if (b > 0xF4) return 0; }
    else return 0;
    if (i + 1 + need > n) return 0;
    for (uint32_t k = 1; k <= need; k++) {
        const unsigned char cb = p[i + k];
        const unsigned char lok = (k == 1) ? lo : 0x80, hik = (k == 1) ? hi : 0xBF;
        if (cb < lok || cb > hik) return 0;
        cp = (cp << 6) | (cb & 0x3F);
    }
    (void)cp;
    return need + 1;
}
static bool korb_str_utf8_valid(const KorbString *s) {
    const unsigned char *const p = (const unsigned char *)korb_strbuf_data(s->buf);
    for (uint32_t i = 0; i < s->len; ) { const uint32_t l = korb_utf8_seq_len(p, i, s->len); if (!l) return false; i += l; }
    return true;
}
/* String#valid_encoding? — true for ASCII-8BIT/US-ASCII-tagged (every byte legal)
 * and for well-formed UTF-8; false for malformed UTF-8. */
static RESULT korb_m_str_valid_encoding(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)a;
    const VALUE v = VALUE_REF_GET(self);
    const uint32_t enc = KORB_STR_ENC(v);
    if (enc == KORB_ENC_BINARY) return RESULT_OK(KORB_TRUE);   /* every byte legal */
    if (enc == KORB_ENC_USASCII) { const KorbString *s = VAL2STR(v);
        for (uint32_t i = 0; i < s->len; i++) if ((unsigned char)korb_strbuf_data(s->buf)[i] >= 0x80) return RESULT_OK(KORB_FALSE);
        return RESULT_OK(KORB_TRUE); }
    if (enc >= KORB_ENC_OTHER_MIN) return RESULT_OK(KORB_TRUE);   /* "other": assume valid until hooked */
    return RESULT_OK(korb_str_utf8_valid(VAL2STR(v)) ? KORB_TRUE : KORB_FALSE);
}
/* String#scrub([repl]) — replace each maximal invalid UTF-8 sub-sequence with
 * `repl` (default U+FFFD).  A binary/US-ASCII string is returned as-is. */
static RESULT korb_m_str_scrub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE v = VALUE_REF_GET(self);
    const uint32_t enc = KORB_STR_ENC(v);
    if (enc == KORB_ENC_BINARY || enc >= KORB_ENC_OTHER_MIN) return RESULT_OK(v);   /* every byte legal / not hooked */
    const bool us_ascii = (enc == KORB_ENC_USASCII);   /* US-ASCII: high bytes are invalid */
    const KorbString *const s = VAL2STR(v);
    /* A valid string is returned unchanged — and the replacement's type is NOT
     * checked in that case (CRuby only validates it when a replacement is used). */
    {
        bool valid = true;
        if (us_ascii) { for (uint32_t i = 0; i < s->len; i++) if ((unsigned char)korb_strbuf_data(s->buf)[i] >= 0x80) { valid = false; break; } }
        else valid = korb_str_utf8_valid(s);
        if (valid) return RESULT_OK(v);
    }
    char repbuf[64]; const char *rep = "\xEF\xBF\xBD"; uint32_t replen = 3;   /* U+FFFD */
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        if (UNLIKELY(!KORB_STRING_P(VALUE_SLICE_GET(a, 0))))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
        const KorbString *const rs = VAL2STR(VALUE_SLICE_GET(a, 0));
        replen = rs->len < sizeof repbuf ? rs->len : (uint32_t)sizeof repbuf;
        memcpy(repbuf, korb_strbuf_data(rs->buf), replen); rep = repbuf;
    }
    const unsigned char *const p = (const unsigned char *)korb_strbuf_data(s->buf); const uint32_t n = s->len;
    size_t cap = n + 1, len = 0; char *out = malloc(cap);
    if (!out) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory");
    #define SCRUB_PUT(ptr, l) do { if (len + (l) + 1 > cap) { cap = (len + (l) + 1) * 2; char *nb = realloc(out, cap); if (!nb) { free(out); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory"); } out = nb; } memcpy(out + len, (ptr), (l)); len += (l); } while (0)
    uint32_t i = 0;
    while (i < n) {
        const uint32_t l = us_ascii ? (p[i] < 0x80 ? 1u : 0u) : korb_utf8_seq_len(p, i, n);
        if (l) { SCRUB_PUT(p + i, l); i += l; }
        else if (us_ascii) { SCRUB_PUT(rep, replen); i++; }   /* each high byte → one replacement */
        else {                                    /* one replacement per maximal invalid sequence */
            const unsigned char b = p[i];
            SCRUB_PUT(rep, replen);
            i++;
            if (b >= 0xC2 && b <= 0xF4)           /* a valid lead byte, truncated → also consume its (too-few) continuations */
                while (i < n && (p[i] & 0xC0) == 0x80) i++;
            /* a stray continuation / invalid lead consumes just itself (one replacement each) */
        }
    }
    #undef SCRUB_PUT
    RESULT r = korb_str_new(c, slots, out, (uint32_t)len);
    free(out);
    return r;
}
/* String#b — a duplicate tagged ASCII-8BIT. */
static RESULT korb_m_str_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t len = SELF_STR->len;
    KorbString *r = korb_str_alloc(c, slots, len);
    memcpy(korb_strbuf_data(r->buf), korb_strbuf_data(SELF_STR->buf), len);   /* SELF_STR re-read after alloc */
    KORB_STR_ENC_SET((VALUE)r, KORB_ENC_BINARY);
    return RESULT_OK((VALUE)r);
}
/* String#-@ → a frozen string (self if already frozen, else a frozen copy). */
static RESULT korb_m_str_uminus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (korb_str_is_frozen(VALUE_REF_GET(self))) return RESULT_OK(VALUE_REF_GET(self));
    slots[0] = VALUE_REF_GET(self);
    RESULT r = korb_send(c, slots + 1, korb_intern(c->vm, "dup", 3), 0, 0);   /* GC-safe copy via #dup */
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (AROH_IS_GC_OBJECT(r.value)) ((AroObjectHeader *)(uintptr_t)r.value)->flags |= KORB_FL_FROZEN;
    return r;
}
/* String#+@ → a mutable string (self if already mutable, else an unfrozen copy). */
static RESULT korb_m_str_plus_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (!korb_str_is_frozen(VALUE_REF_GET(self))) return RESULT_OK(VALUE_REF_GET(self));
    slots[0] = VALUE_REF_GET(self);
    return korb_send(c, slots + 1, korb_intern(c->vm, "dup", 3), 0, 0);       /* mutable copy via #dup */
}
static RESULT korb_m_str_to_sym(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const KorbString *s = SELF_STR;
    return RESULT_OK(ID2SYM(korb_intern(c->vm, korb_strbuf_data(s->buf), s->len)));
}
/* Validate case-mapping options (the mapping itself stays ASCII-only).  op:
 * 0=upcase 1=downcase 2=capitalize 3=swapcase.  Accepts () | :ascii | :turkic |
 * :lithuanian | :fold (downcase only) | {:turkic,:lithuanian}; else ArgumentError. */
static RESULT korb_str_case_opts(CTX *c, VALUE *slots, VALUE_SLICE a, int op) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    if (n == 0) return RESULT_OK(KORB_NIL);
    struct korb_vm *const vm = c->vm;
    const uint32_t s_ascii = korb_intern(vm, "ascii", 5), s_turkic = korb_intern(vm, "turkic", 6),
                   s_lith = korb_intern(vm, "lithuanian", 10), s_fold = korb_intern(vm, "fold", 4);
    uint32_t id[2] = {0, 0};
    for (uint32_t i = 0; i < n && i < 2; i++) {
        const VALUE v = VALUE_SLICE_GET(a, i);
        if (UNLIKELY(!SYMBOL_P(v))) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid option");
        id[i] = SYM2ID(v);
    }
    if (n == 1) {
        const bool ok = id[0] == s_ascii || id[0] == s_turkic || id[0] == s_lith || (id[0] == s_fold && op == 1);
        if (UNLIKELY(!ok)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid option");
    } else if (n == 2) {
        const bool ok = (id[0] == s_turkic && id[1] == s_lith) || (id[0] == s_lith && id[1] == s_turkic);
        if (UNLIKELY(!ok)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid option");
    } else {
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "too many options");
    }
    /* Only :ascii changes behaviour here (restrict to ASCII a-z/A-Z); turkic/
     * lithuanian/fold need Unicode tables and fall back to the default mapping. */
    return RESULT_OK(LONG2FIX(id[0] == s_ascii ? 1 : 0));
}
/* Case-map one codepoint (ASCII + Latin-1 Supplement; other scripts unchanged —
 * full Unicode case folding needs data tables).  to_upper: 1=upcase 0=downcase.
 * Every mapping here preserves UTF-8 byte length (ASCII→ASCII, 2-byte→2-byte). */
static uint32_t korb_case_cp(uint32_t cp, int to_upper) {
    if (to_upper) {
        if (cp >= 'a' && cp <= 'z') return cp - 32;
        if ((cp >= 0xE0 && cp <= 0xF6) || (cp >= 0xF8 && cp <= 0xFE)) return cp - 0x20;  /* à-ö ø-þ → À-Ö Ø-Þ */
        if (cp == 0xFF) return 0x178;                                                   /* ÿ → Ÿ */
    } else {
        if (cp >= 'A' && cp <= 'Z') return cp + 32;
        if ((cp >= 0xC0 && cp <= 0xD6) || (cp >= 0xD8 && cp <= 0xDE)) return cp + 0x20;  /* À-Ö Ø-Þ → à-ö ø-þ */
        if (cp == 0x178) return 0xFF;                                                   /* Ÿ → ÿ */
    }
    return cp;
}
/* UTF-8-aware case transform of src[0..len) into dst (same length).  op: 0=upcase
 * 1=downcase 2=capitalize 3=swapcase.  Returns whether anything changed. */
static bool korb_case_transform(const char *src, char *dst, uint32_t len, int op, bool ascii_only) {
    bool changed = false;
    uint32_t i = 0, ci = 0;
    while (i < len) {
        const unsigned char b = (unsigned char)src[i];
        uint32_t clen = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        if (i + clen > len) clen = 1;
        if (clen >= 3 || (ascii_only && clen >= 2)) { memcpy(dst + i, src + i, clen); i += clen; ci++; continue; }   /* :ascii → leave non-ASCII */
        const uint32_t cp = (clen == 1) ? b : (((uint32_t)(b & 0x1F) << 6) | ((unsigned char)src[i + 1] & 0x3F));
        int to_upper;
        if (op == 0) to_upper = 1;
        else if (op == 1) to_upper = 0;
        else if (op == 2) to_upper = (ci == 0) ? 1 : 0;                                  /* capitalize */
        else {                                                                          /* swapcase */
            if (korb_case_cp(cp, 0) != cp) to_upper = 0;
            else if (korb_case_cp(cp, 1) != cp) to_upper = 1;
            else { memcpy(dst + i, src + i, clen); i += clen; ci++; continue; }
        }
        const uint32_t m = korb_case_cp(cp, to_upper);
        if (m < 0x80) dst[i] = (char)m;
        else { dst[i] = (char)(0xC0 | (m >> 6)); dst[i + 1] = (char)(0x80 | (m & 0x3F)); }
        if (m != cp) changed = true;
        i += clen; ci++;
    }
    return changed;
}

/* transform-into-new-string helper (op: 0=upcase 1=downcase 2=capitalize 3=swapcase, else reverse) */
static RESULT korb_str_transform(CTX *c, VALUE *slots, VALUE_REF self, int op, bool ascii_only) {
    uint32_t len = SELF_STR->len;
    KorbString *r = korb_str_alloc(c, slots, len);
    const KorbString *s = SELF_STR;   /* re-read after alloc (GC may have moved it) */
    KORB_STR_ENC_SET((VALUE)r, KORB_STR_ENC((VALUE)s));   /* preserve encoding */
    if (op >= 0 && op <= 3) { korb_case_transform(korb_strbuf_data(s->buf), korb_strbuf_data(r->buf), len, op, ascii_only); return RESULT_OK((VALUE)r); }
    for (uint32_t i = 0; i < len; i++) korb_strbuf_data(r->buf)[i] = korb_strbuf_data(s->buf)[len - 1 - i];   /* reverse */
    return RESULT_OK((VALUE)r);
}
static RESULT korb_m_str_upcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { RESULT o = korb_str_case_opts(c, slots, a, 0); if (UNLIKELY(o.state != KORB_NORMAL)) return o; return korb_str_transform(c, slots, self, 0, FIX2LONG(o.value) == 1); }
static RESULT korb_m_str_downcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { RESULT o = korb_str_case_opts(c, slots, a, 1); if (UNLIKELY(o.state != KORB_NORMAL)) return o; return korb_str_transform(c, slots, self, 1, FIX2LONG(o.value) == 1); }
static RESULT korb_m_str_capitalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { RESULT o = korb_str_case_opts(c, slots, a, 2); if (UNLIKELY(o.state != KORB_NORMAL)) return o; return korb_str_transform(c, slots, self, 2, FIX2LONG(o.value) == 1); }
/* String#reverse — reverse CHARACTERS (UTF-8 sequences kept intact), not bytes. */
static RESULT korb_m_str_reverse(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const uint32_t enc = KORB_STR_ENC(VALUE_REF_GET(self));
    if (UNLIKELY(KORB_ENC_NEEDS_HOOK(enc)) && !korb_str_bytes_ascii(VALUE_REF_GET(self))) return korb_str_enc_notimpl(c, slots, VALUE_REF_GET(self));
    uint32_t len = SELF_STR->len;
    KorbString *r = korb_str_alloc(c, slots, len);
    char *const sd = korb_str_data(VALUE_REF_GET(self));  /* borrow: no alloc below */
    char *const rd = korb_str_data((VALUE)r);
    KORB_STR_ENC_SET((VALUE)r, enc);                     /* preserve encoding */
    uint32_t wi = len, i = 0;
    while (i < len) {
        const uint32_t clen = korb_str_char_bytes(enc, (const unsigned char *)sd, i, len);   /* char-aware */
        wi -= clen;
        memcpy(rd + wi, sd + i, clen);
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
/* raise NotImplementedError for a character-level op on an unhooked "other"
 * encoding (the per-encoding hooks are future work). */
static RESULT korb_str_enc_notimpl(CTX *c, VALUE *slots, VALUE v) {
    const uint32_t idx = KORB_STR_ENC(v);
    const char *nm = (idx >= KORB_ENC_OTHER_MIN && idx < 8 && c->vm->str_enc_names[idx])
                       ? korb_sym_name(c->vm, c->vm->str_enc_names[idx]) : "this";
    return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "character operations on %s strings are not yet supported", nm);
}
/* byte length of the character at byte offset i (per encoding).  Inlined — it is
 * called per character in the reverse / each_char loops. */
static inline uint32_t korb_str_char_bytes(uint32_t enc, const unsigned char *p, uint32_t i, uint32_t n) {
    if (KORB_ENC_IS_SINGLE_BYTE(enc)) return 1;
    const unsigned char b = p[i];
    uint32_t clen = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    if (i + clen > n) clen = 1;
    return clen;
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
    memcpy(korb_strbuf_data(r->buf), korb_strbuf_data(s->buf) + start, len);
    KORB_STR_ENC_SET((VALUE)r, KORB_STR_ENC(VALUE_REF_GET(sref)));   /* a slice keeps the source encoding */
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
    memcpy(korb_strbuf_data(s->buf) + s->len, korb_strbuf_data(o->buf), on);
    s->len += on; korb_strbuf_data(s->buf)[s->len] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
/* append one element (String or Integer codepoint) onto self */
static RESULT korb_str_append_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_REF oref) {
    VALUE o = VALUE_REF_GET(oref);
    if (KORB_STRING_P(o)) return korb_str_append_str(c, slots, self, oref);
    if (FIXNUM_P(o)) {
        intptr_t cp = FIX2LONG(o);
        const uint32_t enc = KORB_STR_ENC(VALUE_REF_GET(self));
        if (KORB_ENC_IS_SINGLE_BYTE(enc)) {          /* ASCII-8BIT / US-ASCII: append ONE byte, no UTF-8 encoding */
            const intptr_t hi = (enc == KORB_ENC_BINARY) ? 255 : 127;
            if (cp < 0 || cp > hi) return korb_raise(c, slots, KORB_E_RANGE, 0, "%ld out of char range", (long)cp);
            char b = (char)(uint8_t)cp;
            return korb_str_cat(c, slots, self, &b, 1);
        }
        if (cp < 0 || cp > 0x10FFFF) return korb_raise(c, slots, KORB_E_RANGE, 0, "%ld out of char range", (long)cp);
        char buf[4]; uint32_t n = korb_utf8_encode((uint32_t)cp, buf);   /* stable C buffer */
        return korb_str_cat(c, slots, self, buf, n);
    }
    if (KORB_BIGNUM_P(o))   /* a Bignum codepoint is always out of char range (0..0x10FFFF) */
        return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum out of char range");
    { const uint32_t to_str = korb_intern(c->vm, "to_str", 6);   /* coerce a #to_str object → String */
      if (KORB_OBJECT_P(o) && korb_responds_to_coerce_p(c, slots, &o, to_str)) {
          slots[0] = o;                                          /* receiver, rooted across dispatch */
          const RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
          if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
          if (LIKELY(KORB_STRING_P(sr.value))) {
              VALUE_REF sref = SLOTS_PUSH(slots, sr.value);      /* coerced String (self stays rooted) */
              return korb_str_append_str(c, slots, self, sref);
          }
      }
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
    const uint32_t self_len0 = VAL2STR(VALUE_REF_GET(self))->len;   /* original bytes: concat(self,...) appends these, not the growing self */
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        if (UNLIKELY(VALUE_SLICE_GET(a, j) == VALUE_REF_GET(self))) {   /* self-arg → append the original bytes only */
            KorbString *s = korb_str_ensure(c, slots, self, VAL2STR(VALUE_REF_GET(self))->len + self_len0);
            const KorbString *o = VAL2STR(VALUE_REF_GET(self));        /* re-read after grow (s == o) */
            memcpy(korb_strbuf_data(s->buf) + s->len, korb_strbuf_data(o->buf), self_len0);    /* dest past src by >= self_len0 → no overlap */
            s->len += self_len0; korb_strbuf_data(s->buf)[s->len] = '\0';
        }
        else CHECK(korb_str_append_one(c, slots, self, VALUE_SLICE_REF(a, j)));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* String#initialize([str], encoding:, capacity:) — private.  With no positional
 * string, leave self as-is (String.new / capacity:/encoding:-only forms); with a
 * string (or #to_str-able) source, replace self's content with it (like #replace).
 * Returns self.  A trailing kwargs Hash (encoding:/capacity:) is ignored. */
static RESULT korb_m_str_replace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd */
static RESULT korb_m_str_initialize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) == 0) return RESULT_OK(VALUE_REF_GET(self));
    if (KORB_HASH_P(VALUE_SLICE_GET(a, 0))) return RESULT_OK(VALUE_REF_GET(self));   /* kwargs-only (encoding:/capacity:) */
    RESULT r = korb_m_str_replace(c, slots, self, a);   /* replace content from the string source (#replace reads only arg0) */
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_replace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(o))) {                 /* coerce other via #to_str */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(o) && korb_responds_to_coerce_p(c, slots, &o, to_str)) {
            slots[0] = VALUE_REF_GET(self); slots[1] = o;   /* root self + receiver across the dispatch */
            RESULT sr = korb_send_impl(c, slots + 2, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (LIKELY(KORB_STRING_P(sr.value))) { VALUE_REF_SET(VALUE_SLICE_REF(a, 0), sr.value); o = sr.value; self = VALUE_REF_AT(&slots[0]); }
        }
        if (UNLIKELY(!KORB_STRING_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    if (o == VALUE_REF_GET(self)) return RESULT_OK(VALUE_REF_GET(self));   /* s.replace(s) is a no-op (clear-then-append would empty it) */
    VAL2STR(VALUE_REF_GET(self))->len = 0;             /* clear, then append other */
    /* Append at slots+2: the #to_str branch parks `self` at slots[0], so the grow's
     * korb_alloc must not use slots[0]/[1] as scratch (that would stale `self`). */
    return korb_str_append_str(c, slots + 2, self, VALUE_SLICE_REF(a, 0));
}
static RESULT korb_m_str_prepend(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    /* coerce each arg to a String (via #to_str) into a rooted array, so the grow
     * below has them all pinned across any dispatch-induced GC. */
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, VALUE_SLICE_LEN(a)));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        slots[1] = VALUE_SLICE_GET(a, j);
        if (UNLIKELY(!KORB_STRING_P(slots[1]))) {
            const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
            if (KORB_OBJECT_P(slots[1]) && korb_responds_to_coerce(c, slots + 2, slots[1], to_str)) {
                RESULT sr = korb_send_impl(c, slots + 2, to_str, 0, 0, NULL, NULL, KORB_NIL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                slots[1] = sr.value;
            }
            if (!KORB_STRING_P(slots[1])) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, j)));
        }
        CHECK(korb_ary_push_val(c, slots + 2, arr, slots[1]));
    }
    uint32_t pn = 0;
    { const KorbArray *ca = VAL2ARY(slots[0]); for (uint32_t j = 0; j < ca->len; j++) pn += VAL2STR(korb_items_data(ca->items)[j])->len; }
    uint32_t slen = VAL2STR(VALUE_REF_GET(self))->len;
    korb_str_ensure(c, slots + 1, self, slen + pn);              /* single grow; args pinned in arr */
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    memmove(korb_strbuf_data(s->buf) + pn, korb_strbuf_data(s->buf), slen);
    uint32_t off = 0;
    const KorbArray *ca = VAL2ARY(slots[0]);
    for (uint32_t j = 0; j < ca->len; j++) {
        const KorbString *o = VAL2STR(korb_items_data(ca->items)[j]);
        memcpy(korb_strbuf_data(s->buf) + off, korb_strbuf_data(o->buf), o->len); off += o->len;
    }
    s->len = slen + pn; korb_strbuf_data(s->buf)[s->len] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_clear(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    (void)c;(void)slots;(void)a;
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    s->len = 0; korb_strbuf_data(s->buf)[0] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
/* in-place case/reverse (op: 0 upcase 1 downcase 2 capitalize 3 swapcase 4 reverse);
 * returns self if changed, else nil (Ruby bang convention) — reverse! always self. */
static RESULT korb_str_transform_bang(CTX *c, VALUE *slots, VALUE_REF self, int op, bool ascii_only) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));     /* bang mutators check frozen upfront */
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t len = s->len; bool changed = false;
    if (op == 4) {                                     /* reverse! (UTF-8 char-aware) */
        char *tmp = malloc(len ? len : 1);
        if (!tmp) abort();
        uint32_t wi = len, i = 0;
        while (i < len) {
            const unsigned char b = (unsigned char)korb_strbuf_data(s->buf)[i];
            uint32_t clen = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
            if (i + clen > len) clen = 1;
            wi -= clen; memcpy(tmp + wi, korb_strbuf_data(s->buf) + i, clen); i += clen;
        }
        memcpy(korb_strbuf_data(s->buf), tmp, len); free(tmp);
        return RESULT_OK(VALUE_REF_GET(self));
    }
    changed = korb_case_transform(korb_strbuf_data(s->buf), korb_strbuf_data(s->buf), len, op, ascii_only);   /* in place: byte length preserved */
    return RESULT_OK(changed ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_upcase_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { RESULT o = korb_str_case_opts(c, slots, a, 0); if (UNLIKELY(o.state != KORB_NORMAL)) return o; return korb_str_transform_bang(c, slots, self, 0, FIX2LONG(o.value) == 1); }
static RESULT korb_m_str_downcase_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { RESULT o = korb_str_case_opts(c, slots, a, 1); if (UNLIKELY(o.state != KORB_NORMAL)) return o; return korb_str_transform_bang(c, slots, self, 1, FIX2LONG(o.value) == 1); }
static RESULT korb_m_str_capitalize_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { RESULT o = korb_str_case_opts(c, slots, a, 2); if (UNLIKELY(o.state != KORB_NORMAL)) return o; return korb_str_transform_bang(c, slots, self, 2, FIX2LONG(o.value) == 1); }
static RESULT korb_m_str_swapcase_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { RESULT o = korb_str_case_opts(c, slots, a, 3); if (UNLIKELY(o.state != KORB_NORMAL)) return o; return korb_str_transform_bang(c, slots, self, 3, FIX2LONG(o.value) == 1); }
static RESULT korb_m_str_reverse_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)a; return korb_str_transform_bang(c, slots, self, 4, false); }

/* Replace self bytes [bs,be) with replref (a rooted String); do_repl=false deletes. */
static RESULT korb_str_splice(CTX *c, VALUE *slots, VALUE_REF self, uint32_t bs, uint32_t be, VALUE_REF replref, bool do_repl) {
    uint32_t rn = do_repl ? VAL2STR(VALUE_REF_GET(replref))->len : 0;
    uint32_t slen = VAL2STR(VALUE_REF_GET(self))->len;
    uint32_t newlen = slen - (be - bs) + rn;
    KorbString *s = korb_str_ensure(c, slots, self, newlen);   /* alloc; self+repl rooted */
    s = VAL2STR(VALUE_REF_GET(self));
    memmove(korb_strbuf_data(s->buf) + bs + rn, korb_strbuf_data(s->buf) + be, slen - be);
    if (rn) { const KorbString *r = VAL2STR(VALUE_REF_GET(replref)); memcpy(korb_strbuf_data(s->buf) + bs, korb_strbuf_data(r->buf), rn); }
    s->len = newlen; korb_strbuf_data(s->buf)[newlen] = '\0';
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
    slots[0] = *v;                                    /* root the receiver across respond_to?/to_int dispatch */
    if (!korb_responds_to_coerce(c, slots + 1, slots[0], to_int)) { *v = slots[0]; return RESULT_OK(KORB_FALSE); }
    RESULT r = korb_send_impl(c, slots + 1, to_int, 0, 0, NULL, NULL, KORB_NIL);   /* receiver at slots[0] */
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (!korb_to_index(r.value, &tmp)) { *v = r.value; return RESULT_OK(KORB_FALSE); }
    *v = r.value;
    return RESULT_OK(KORB_TRUE);
}
/* If *v isn't an Array, try #to_ary; returns TRUE (with *v the Array) or FALSE (leaves *v). */
static RESULT korb_coerce_to_ary(CTX *c, VALUE *slots, VALUE *v) {
    if (KORB_ARRAY_P(*v)) return RESULT_OK(KORB_TRUE);
    const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
    slots[0] = *v;
    if (!korb_responds_to_coerce(c, slots + 1, slots[0], to_ary)) { *v = slots[0]; return RESULT_OK(KORB_FALSE); }
    RESULT r = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, KORB_NIL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (!KORB_ARRAY_P(r.value)) { *v = r.value; return RESULT_OK(KORB_FALSE); }
    *v = r.value;
    return RESULT_OK(KORB_TRUE);
}
/* If *v isn't a String, try #to_str; returns TRUE (with *v the String) or FALSE (leaves *v). */
static RESULT korb_coerce_to_str(CTX *c, VALUE *slots, VALUE *v) {
    if (KORB_STRING_P(*v)) return RESULT_OK(KORB_TRUE);
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    slots[0] = *v;                                    /* root the receiver across respond_to?/to_str dispatch */
    if (!korb_responds_to_coerce(c, slots + 1, slots[0], to_str)) { *v = slots[0]; return RESULT_OK(KORB_FALSE); }
    RESULT r = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);   /* receiver at slots[0] */
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (!KORB_STRING_P(r.value)) { *v = r.value; return RESULT_OK(KORB_FALSE); }
    *v = r.value;
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_str_idx_conv(CTX *c, VALUE *slots, VALUE v, intptr_t *out);   /* fwd: index→long, Bignum too big → RangeError */
static RESULT korb_str_target_span(CTX *c, VALUE *slots, VALUE_REF self, VALUE idx, VALUE len_v, bool *found, uint32_t *bs, uint32_t *be, bool write) {
    if (KORB_REGEXP_P(idx))                            /* str[re] / str[re, capture] → matched byte span (sets $~) */
        return korb_re_str_span(c, slots, self, idx, len_v, found, bs, be, write);
    if (!KORB_STRING_P(idx) && !KORB_RANGE_P(idx)) {   /* coerce a non-String/Range index via #to_int (before reading self) */
        const char *onm = korb_type_name(idx);
        RESULT cr = korb_coerce_to_int(c, slots, &idx);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (cr.value == KORB_FALSE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", onm);   /* no #to_int, or it gave a non-Integer */
    }
    if (len_v != KORB_NIL) {
        const char *onm = korb_type_name(len_v);
        RESULT cr = korb_coerce_to_int(c, slots, &len_v);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (cr.value == KORB_FALSE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", onm);
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    const uint32_t enc = KORB_STR_ENC(VALUE_REF_GET(self));
    if (UNLIKELY(KORB_ENC_NEEDS_HOOK(enc)) && !korb_str_bytes_ascii(VALUE_REF_GET(self))) return korb_str_enc_notimpl(c, slots, VALUE_REF_GET(self));
    const bool sb = KORB_ENC_IS_SINGLE_BYTE(enc);   /* single-byte enc: char index == byte index */
    uint32_t ncp = sb ? s->len : korb_utf8_count(korb_strbuf_data(s->buf), s->len);
    *found = true;
    if (KORB_STRING_P(idx)) {                          /* substring target */
        const KorbString *sub = VAL2STR(idx);
        int32_t at = korb_byte_find(korb_strbuf_data(s->buf), s->len, korb_strbuf_data(sub->buf), sub->len);
        if (at < 0) {
            *found = false;
            if (write) return korb_raise(c, slots, KORB_E_INDEX, 0, "string not matched");
            return RESULT_OK(KORB_NIL);
        }
        *bs = (uint32_t)at; *be = (uint32_t)at + sub->len;
        return RESULT_OK(KORB_NIL);
    }
    intptr_t st, ln, st_raw = 0;
    if (KORB_RANGE_P(idx)) {
        const KorbRange *r = VAL2RANGE(idx);
        const bool beginless = (r->rbegin == KORB_NIL);
        const bool endless   = (r->rend   == KORB_NIL);
        intptr_t b, e;
        if (beginless) b = 0;
        else { RESULT cr = korb_str_idx_conv(c, slots, r->rbegin, &b); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
        if (endless) e = (intptr_t)ncp;
        else { RESULT cr = korb_str_idx_conv(c, slots, r->rend, &e); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
        const intptr_t b_raw = b;
        if (b < 0) b += ncp;
        if (write && (b < 0 || b > (intptr_t)ncp)) {     /* []= : an out-of-range Range begin is a RangeError (not IndexError) */
            char lo[24] = "", hi[24] = "";               /* the range as written, CRuby's message shape ("-9..1 out of range") */
            if (!beginless) snprintf(lo, sizeof lo, "%ld", (long)b_raw);
            if (!endless)   snprintf(hi, sizeof hi, "%ld", (long)e);
            return korb_raise(c, slots, KORB_E_RANGE, 0, "%s%s%s out of range", lo, r->exclude_end ? "..." : "..", hi);
        }
        if (!endless && e < 0) e += ncp;
        st = b; ln = ((endless || r->exclude_end) ? e - 1 : e) - b + 1; if (ln < 0) ln = 0;
    } else {
        if (UNLIKELY(!korb_to_index(idx, &st))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(idx));
        st_raw = st;                                                 /* the index as written, for the error message */
        if (st < 0) st += ncp;
        if (len_v != KORB_NIL) {
            if (UNLIKELY(!korb_to_index(len_v, &ln))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(len_v));
        } else ln = 1;
    }
    const bool single = (len_v == KORB_NIL && !KORB_RANGE_P(idx));   /* str[i]: one char, nil at end (read only) */
    if (st < 0 || st > (intptr_t)ncp || ln < 0 || (single && !write && st == (intptr_t)ncp)) {
        *found = false;
        if (write && ln < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "negative length %ld", (long)ln);
        if (write && !KORB_RANGE_P(idx)) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld out of string", (long)st_raw);
        return RESULT_OK(KORB_NIL);
    }
    if (single && write && st == (intptr_t)ncp) ln = 0;              /* str[len]=x → append (empty span at end) */
    if (st + ln > (intptr_t)ncp) ln = (intptr_t)ncp - st;
    *bs = sb ? ((uint32_t)st < s->len ? (uint32_t)st : s->len) : korb_utf8_byteoff(korb_strbuf_data(s->buf), s->len, (uint32_t)st);
    *be = sb ? ((uint32_t)(st + ln) < s->len ? (uint32_t)(st + ln) : s->len) : korb_utf8_byteoff(korb_strbuf_data(s->buf), s->len, (uint32_t)(st + ln));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_str_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    uint32_t na = VALUE_SLICE_LEN(a);
    if (UNLIKELY(na < 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE idx = VALUE_SLICE_GET(a, 0);
    VALUE len_v = (na == 3) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    /* Which comes first, locating the target or converting the replacement?
     * CRuby resolves a Regexp/String/Range target up front (a miss there raises
     * without ever asking the replacement for #to_str) and only the plain index
     * path converts first. */
    const bool span_first = !FIXNUM_P(idx) || len_v != KORB_NIL;
    bool found; uint32_t bs = 0, be = 0;
    if (span_first) {
        RESULT sp = korb_str_target_span(c, slots, self, idx, len_v, &found, &bs, &be, true);
        if (UNLIKELY(sp.state != KORB_NORMAL)) return sp;
    }
    slots[0] = VALUE_SLICE_GET(a, na - 1);             /* replacement — #to_str it (may run Ruby → keep it rooted here) */
    if (UNLIKELY(!KORB_STRING_P(slots[0]))) {
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce(c, slots + 1, slots[0], to_str)) {
            RESULT cr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            slots[0] = cr.value;
        }
        if (!KORB_STRING_P(slots[0])) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, na - 1)));
    }
    if (!span_first) {
        RESULT sp = korb_str_target_span(c, slots + 1, self, idx, len_v, &found, &bs, &be, true);   /* slots+1: preserve repl in slots[0] */
        if (UNLIKELY(sp.state != KORB_NORMAL)) return sp;
    } else {                                           /* #to_str could have shortened self meanwhile */
        const uint32_t slen = VAL2STR(VALUE_REF_GET(self))->len;
        if (bs > slen) bs = slen;
        if (be > slen) be = slen;
    }
    if (!found) return korb_raise(c, slots, KORB_E_INDEX, 0, "index out of string");
    CHECK(korb_str_splice(c, slots + 1, self, bs, be, VALUE_REF_AT(&slots[0]), true));   /* bs/be are byte offsets (GC-stable) */
    return RESULT_OK(VALUE_SLICE_GET(a, na - 1));   /* `a[i]=v` evaluates to v (original RHS, not the coerced string) */
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
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t lo = 0, hi = s->len;
    bool has_set = VALUE_SLICE_LEN(a) >= 1;
    if (mode != 2) while (lo < hi && (has_set ? korb_str_sets_match(a, (unsigned char)korb_strbuf_data(s->buf)[lo]) : (unsigned char)korb_strbuf_data(s->buf)[lo] <= ' ')) lo++;
    if (mode != 1) while (hi > lo && (has_set ? korb_str_sets_match(a, (unsigned char)korb_strbuf_data(s->buf)[hi-1]) : (unsigned char)korb_strbuf_data(s->buf)[hi-1] <= ' ')) hi--;
    if (lo == 0 && hi == s->len) return RESULT_OK(KORB_NIL);   /* unchanged */
    uint32_t nlen = hi - lo;
    if (lo) memmove(korb_strbuf_data(s->buf), korb_strbuf_data(s->buf) + lo, nlen);
    s->len = nlen; korb_strbuf_data(s->buf)[nlen] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_strip_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_strip_bang(c, slots, self, a, 0); }
static RESULT korb_m_str_lstrip_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_strip_bang(c, slots, self, a, 1); }
static RESULT korb_m_str_rstrip_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_strip_bang(c, slots, self, a, 2); }
static RESULT korb_m_str_chomp_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t n = s->len;
    if (n == 0) return RESULT_OK(KORB_NIL);           /* empty → no-op → nil, before any sep-type check (CRuby) */
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* chomp!(sep) */
        VALUE sv = VALUE_SLICE_GET(a, 0);
        if (sv == KORB_NIL) return RESULT_OK(KORB_NIL);   /* nil sep → no-op → nil */
        if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
        const KorbString *sep = VAL2STR(sv);
        if (sep->len == 0) { while (n >= 1 && korb_strbuf_data(s->buf)[n-1] == '\n') { if (n >= 2 && korb_strbuf_data(s->buf)[n-2] == '\r') n--; n--; } }
        else if (n >= sep->len && memcmp(korb_strbuf_data(s->buf) + n - sep->len, korb_strbuf_data(sep->buf), sep->len) == 0) n -= sep->len;
        if (n == s->len) return RESULT_OK(KORB_NIL);
        s->len = n; korb_strbuf_data(s->buf)[n] = '\0';
        return RESULT_OK(VALUE_REF_GET(self));
    }
    if (n >= 1 && korb_strbuf_data(s->buf)[n-1] == '\n') { n--; if (n >= 1 && korb_strbuf_data(s->buf)[n-1] == '\r') n--; }
    else if (n >= 1 && korb_strbuf_data(s->buf)[n-1] == '\r') n--;
    else return RESULT_OK(KORB_NIL);
    s->len = n; korb_strbuf_data(s->buf)[n] = '\0';
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_chop_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));     /* chop! checks frozen upfront (even for "") */
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    if (s->len == 0) return RESULT_OK(KORB_NIL);
    uint32_t n = s->len;
    if (n >= 2 && korb_strbuf_data(s->buf)[n-1] == '\n' && korb_strbuf_data(s->buf)[n-2] == '\r') n -= 2;
    else {
        n--;                                           /* back up over one UTF-8 char */
        while (n > 0 && ((unsigned char)korb_strbuf_data(s->buf)[n] & 0xC0) == 0x80) n--;
    }
    s->len = n; korb_strbuf_data(s->buf)[n] = '\0';
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
        if (!korb_charset_match(korb_strbuf_data(set->buf), set->len, ch)) return false;
    }
    return true;
}
/* Decode one UTF-8 codepoint, but treat an invalid lead / truncated / bad
 * continuation as a single raw byte (so byte-range sets like "\x00-\xFF" and
 * binary strings work).  Sets *clen (>=1). */
static uint32_t korb_utf8_dec1(const char *p, uint32_t avail, uint32_t *clen) {
    const unsigned char b0 = (unsigned char)p[0];
    if (b0 < 0x80) { *clen = 1; return b0; }
    const int len = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 1;
    if (len == 1 || (uint32_t)len > avail) { *clen = 1; return b0; }
    for (int k = 1; k < len; k++) if (((unsigned char)p[k] & 0xC0) != 0x80) { *clen = 1; return b0; }
    uint32_t cp = b0 & (0x7Fu >> len);
    for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)p[k] & 0x3F);
    *clen = (uint32_t)len; return cp;
}
/* Codepoint-aware charset match: parse the set as UTF-8 codepoints, honouring
 * `a-b` ranges and a leading `^` complement (a lone "^" is literal). */
static bool korb_charset_match_cp(const char *set, uint32_t n, uint32_t cp) {
    bool neg = false; uint32_t i = 0;
    if (n > 1 && set[0] == '^') { neg = true; i = 1; }
    bool in = false;
    while (i < n) {
        uint32_t cl; const uint32_t lo = korb_utf8_dec1(set + i, n - i, &cl); i += cl;
        if (i < n && set[i] == '-' && i + 1 < n) {   /* lo-hi range */
            i++;
            uint32_t cl2; const uint32_t hi = korb_utf8_dec1(set + i, n - i, &cl2); i += cl2;
            if (lo <= cp && cp <= hi) in = true;
        } else if (lo == cp) in = true;
    }
    return neg ? !in : in;
}
/* All set args must match (intersection), by codepoint. */
static bool korb_str_sets_match_cp(VALUE_SLICE a, uint32_t cp) {
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        const VALUE sv = VALUE_SLICE_GET(a, j);
        if (!KORB_STRING_P(sv)) continue;
        const KorbString *set = VAL2STR(sv);
        if (!korb_charset_match_cp(korb_strbuf_data(set->buf), set->len, cp)) return false;
    }
    return true;
}
/* Coerce every set arg to a String via #to_str (in place), TypeError otherwise. */
static RESULT korb_str_sets_coerce(CTX *c, VALUE *slots, VALUE_SLICE a) {
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        VALUE sv = VALUE_SLICE_GET(a, j);
        if (KORB_STRING_P(sv)) continue;
        if (KORB_OBJECT_P(sv) && korb_responds_to_coerce_p(c, slots, &sv, to_str)) {
            slots[0] = sv;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (KORB_STRING_P(sr.value)) { VALUE_REF_SET(VALUE_SLICE_REF(a, j), sr.value); continue; }
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, j)));
    }
    return RESULT_OK(KORB_NIL);
}
/* A charset spec with a descending range "b-a" (b > a) is invalid in Ruby.
 * Returns the offending pair via lo and hi (else false). Skips a leading '^'. */
static bool korb_charset_bad_range(const char *set, uint32_t n, unsigned char *lo, unsigned char *hi) {
    uint32_t i = 0;
    if (n > 1 && set[0] == '^') i = 1;
    for (; i < n; i++) {
        if (i + 2 < n && set[i+1] == '-') {
            if ((unsigned char)set[i] > (unsigned char)set[i+2]) { *lo = (unsigned char)set[i]; *hi = (unsigned char)set[i+2]; return true; }
            i += 2;
        }
    }
    return false;
}
/* Raise ArgumentError if any set arg in `a` has a descending range. */
static RESULT korb_str_sets_validate(CTX *c, VALUE *slots, VALUE_SLICE a) {
    unsigned char lo, hi;
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        const VALUE sv = VALUE_SLICE_GET(a, j);
        if (KORB_STRING_P(sv) && korb_charset_bad_range(korb_strbuf_data(VAL2STR(sv)->buf), VAL2STR(sv)->len, &lo, &hi))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid range \"%c-%c\" in string transliteration", lo, hi);
    }
    return RESULT_OK(KORB_NIL);
}
/* delete_prefix/suffix (mode 0/1); in_place → bang (self if changed else nil). */
static RESULT korb_str_delfix(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode, bool in_place) {
    if (in_place) KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));   /* delete_prefix!/suffix! check frozen upfront */
    VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv))) {                  /* coerce arg via #to_str */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(pv) && korb_responds_to_coerce_p(c, slots, &pv, to_str)) {
            slots[0] = pv;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            pv = sr.value;
        }
        if (!KORB_STRING_P(pv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *p = VAL2STR(pv);
    uint32_t bs = 0, be = s->len; bool match = false;
    if (p->len <= s->len) {
        if (mode == 0 && memcmp(korb_strbuf_data(s->buf), korb_strbuf_data(p->buf), p->len) == 0) { bs = p->len; match = true; }
        else if (mode == 1 && memcmp(korb_strbuf_data(s->buf) + s->len - p->len, korb_strbuf_data(p->buf), p->len) == 0) { be = s->len - p->len; match = true; }
    }
    if (!in_place) return korb_str_slice_new(c, slots, self, bs, be - bs);
    if (!match) return RESULT_OK(KORB_NIL);
    KorbString *m = VAL2STR(VALUE_REF_GET(self));
    uint32_t nlen = be - bs;
    if (bs) memmove(korb_strbuf_data(m->buf), korb_strbuf_data(m->buf) + bs, nlen);
    m->len = nlen; korb_strbuf_data(m->buf)[nlen] = '\0';
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
    if (VALUE_SLICE_LEN(a) == 1) {
        if (UNLIKELY(!KORB_RANGE_P(VALUE_SLICE_GET(a, 0))))            /* clamp(x) single non-Range → TypeError */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Range)", korb_type_name(VALUE_SLICE_GET(a, 0)));
        const KorbRange *r = VAL2RANGE(VALUE_SLICE_GET(a, 0)); if (UNLIKELY(r->exclude_end)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "cannot clamp with an exclusive range"); lo = r->rbegin; hi = r->rend;
    } else if (VALUE_SLICE_LEN(a) == 2) { lo = VALUE_SLICE_GET(a, 0); hi = VALUE_SLICE_GET(a, 1); }
    else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 2)", (unsigned)VALUE_SLICE_LEN(a));
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
    if (in_place) KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));   /* delete! checks frozen upfront */
    /* CRuby validates the arg count only for a non-empty receiver: ""·delete → ""
     * (no ArgumentError), but "x".delete → ArgumentError. */
    if (UNLIKELY(VALUE_SLICE_LEN(a) == 0)) {
        if (VAL2STR(VALUE_REF_GET(self))->len != 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
        return in_place ? RESULT_OK(KORB_NIL) : korb_str_slice_new(c, slots, self, 0, 0);
    }
    if (VAL2STR(VALUE_REF_GET(self))->len == 0)          /* empty → ""/nil, args unvalidated (CRuby) */
        return in_place ? RESULT_OK(KORB_NIL) : korb_str_slice_new(c, slots, self, 0, 0);
    { RESULT cr = korb_str_sets_coerce(c, slots, a); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
    { RESULT v = korb_str_sets_validate(c, slots, a); if (UNLIKELY(v.state != KORB_NORMAL)) return v; }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t n = s->len;
    KorbString *r = korb_str_alloc(c, slots, n);
    s = VAL2STR(VALUE_REF_GET(self));                   /* re-read after alloc */
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; ) {                     /* iterate by UTF-8 codepoint */
        uint32_t cl; const uint32_t cp = korb_utf8_dec1(korb_strbuf_data(s->buf) + i, n - i, &cl);
        if (cl == 0) cl = 1;
        if (!korb_str_sets_match_cp(a, cp)) { memcpy(korb_strbuf_data(r->buf) + w, korb_strbuf_data(s->buf) + i, cl); w += cl; }
        i += cl;
    }
    r->len = w; korb_strbuf_data(r->buf)[w] = '\0';
    if (!in_place) { KORB_STR_ENC_SET((VALUE)r, KORB_STR_ENC(VALUE_REF_GET(self))); return RESULT_OK((VALUE)r); }
    bool changed = (w != n);
    slots[0] = (VALUE)r;
    KorbString *s2 = korb_str_ensure(c, slots + 1, self, w);
    r = VAL2STR(slots[0]);
    memcpy(korb_strbuf_data(s2->buf), korb_strbuf_data(r->buf), w);
    s2->len = w; korb_strbuf_data(s2->buf)[w] = '\0';
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
/* Codepoint-level tr set expansion: `a-z` ranges into individual codepoints,
 * multibyte chars preserved.  Skips a leading '^' if `skip_caret`. */
static uint32_t korb_tr_expand_cp(const char *s, uint32_t n, uint32_t *out, uint32_t cap) {
    uint32_t k = 0, i = 0;
    while (i < n && k < cap) {
        uint32_t cl; const uint32_t lo = korb_utf8_dec1(s + i, n - i, &cl); const uint32_t ni = i + cl;
        if (ni < n && s[ni] == '-' && ni + 1 < n) {          /* lo-hi range */
            uint32_t cl2; const uint32_t hi = korb_utf8_dec1(s + ni + 1, n - ni - 1, &cl2);
            for (uint32_t ch = lo; ch <= hi && k < cap; ch++) out[k++] = ch;
            i = ni + 1 + cl2;
        } else { out[k++] = lo; i = ni; }
    }
    return k;
}
/* True if a tr set has a descending codepoint range; reports lo/hi. */
static bool korb_charset_bad_range_cp(const char *s, uint32_t n, uint32_t *lo_out, uint32_t *hi_out) {
    uint32_t i = 0;
    if (n > 1 && s[0] == '^') i = 1;
    while (i < n) {
        uint32_t cl; const uint32_t lo = korb_utf8_dec1(s + i, n - i, &cl); const uint32_t ni = i + cl;
        if (ni < n && s[ni] == '-' && ni + 1 < n) {
            uint32_t cl2; const uint32_t hi = korb_utf8_dec1(s + ni + 1, n - ni - 1, &cl2);
            if (lo > hi) { *lo_out = lo; *hi_out = hi; return true; }
            i = ni + 1 + cl2;
        } else i = ni;
    }
    return false;
}
/* String#tr(from, to) — byte-level translate; `^` negation, ranges, to-empty
 * deletes, to-shorter repeats its last char.  (UTF-8 chars beyond ASCII pass.) */
static RESULT korb_m_str_tr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* from_str (coerce via #to_str) */
    if (!KORB_STRING_P(slots[0]) && KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce(c, slots + 1, slots[0], to_str)) {
        RESULT r = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    slots[1] = VALUE_SLICE_GET(a, 1);                    /* to_str (staged after from's coercion) */
    if (!KORB_STRING_P(slots[1]) && KORB_OBJECT_P(slots[1]) && korb_responds_to_coerce(c, slots + 2, slots[1], to_str)) {
        RESULT r = korb_send_impl(c, slots + 2, to_str, 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[1] = r.value;
    }
    if (UNLIKELY(!KORB_STRING_P(slots[0]) || !KORB_STRING_P(slots[1])))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const VALUE fv = slots[0], tv = slots[1];
    const KorbString *fs = VAL2STR(fv), *ts = VAL2STR(tv);
    { uint32_t rlo, rhi;
      if (UNLIKELY(korb_charset_bad_range_cp(korb_strbuf_data(fs->buf), fs->len, &rlo, &rhi) || korb_charset_bad_range_cp(korb_strbuf_data(ts->buf), ts->len, &rlo, &rhi)))
          return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid range in string transliteration"); }
    bool neg = fs->len > 1 && korb_strbuf_data(fs->buf)[0] == '^';   /* a lone "^" is the literal char, not a complement */
    uint32_t fromx[1024], tox[1024];
    uint32_t fn = korb_tr_expand_cp(korb_strbuf_data(fs->buf) + (neg ? 1 : 0), fs->len - (neg ? 1u : 0u), fromx, 1024);
    uint32_t tn = korb_tr_expand_cp(korb_strbuf_data(ts->buf), ts->len, tox, 1024);
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));      /* no GC during the scan */
    for (uint32_t i = 0; i < s->len; ) {
        uint32_t cl; const uint32_t cp = korb_utf8_dec1(korb_strbuf_data(s->buf) + i, s->len - i, &cl);
        int idx = -1;
        for (uint32_t k = 0; k < fn; k++) if (fromx[k] == cp) { idx = (int)k; break; }
        bool match = neg ? (idx < 0) : (idx >= 0);
        if (!match) { fwrite(korb_strbuf_data(s->buf) + i, 1, cl, ms); i += cl; continue; }   /* pass through verbatim */
        i += cl;
        if (tn == 0) continue;                              /* delete */
        char enc[4]; const uint32_t out_cp = neg ? tox[tn - 1] : tox[(uint32_t)idx < tn ? (uint32_t)idx : tn - 1];
        fwrite(enc, 1, korb_utf8_encode(out_cp, enc), ms);
    }
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf, (uint32_t)sz);
    free(buf);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_STR_ENC(VALUE_REF_GET(self)));
    return r;
}
/* tr_s: like tr, but runs of *translated* chars that map to the same output are
 * squeezed to one.  Pre-existing runs (untranslated) are left intact. */
static RESULT korb_m_str_tr_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* from_str (coerce via #to_str) */
    if (!KORB_STRING_P(slots[0]) && KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce(c, slots + 1, slots[0], to_str)) {
        RESULT r = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    slots[1] = VALUE_SLICE_GET(a, 1);                    /* to_str (staged after from's coercion) */
    if (!KORB_STRING_P(slots[1]) && KORB_OBJECT_P(slots[1]) && korb_responds_to_coerce(c, slots + 2, slots[1], to_str)) {
        RESULT r = korb_send_impl(c, slots + 2, to_str, 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[1] = r.value;
    }
    if (UNLIKELY(!KORB_STRING_P(slots[0]) || !KORB_STRING_P(slots[1])))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const VALUE fv = slots[0], tv = slots[1];
    const KorbString *fs = VAL2STR(fv), *ts = VAL2STR(tv);
    { uint32_t rlo, rhi;
      if (UNLIKELY(korb_charset_bad_range_cp(korb_strbuf_data(fs->buf), fs->len, &rlo, &rhi) || korb_charset_bad_range_cp(korb_strbuf_data(ts->buf), ts->len, &rlo, &rhi)))
          return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid range in string transliteration"); }
    bool neg = fs->len > 1 && korb_strbuf_data(fs->buf)[0] == '^';   /* a lone "^" is the literal char, not a complement */
    uint32_t fromx[1024], tox[1024];
    uint32_t fn = korb_tr_expand_cp(korb_strbuf_data(fs->buf) + (neg ? 1 : 0), fs->len - (neg ? 1u : 0u), fromx, 1024);
    uint32_t tn = korb_tr_expand_cp(korb_strbuf_data(ts->buf), ts->len, tox, 1024);
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));      /* no GC during the scan */
    bool prev_tr = false; int64_t prev_out = -1;
    for (uint32_t i = 0; i < s->len; ) {
        uint32_t cl; const uint32_t cp = korb_utf8_dec1(korb_strbuf_data(s->buf) + i, s->len - i, &cl);
        int idx = -1;
        for (uint32_t k = 0; k < fn; k++) if (fromx[k] == cp) { idx = (int)k; break; }
        bool match = neg ? (idx < 0) : (idx >= 0);
        if (!match) { fwrite(korb_strbuf_data(s->buf) + i, 1, cl, ms); i += cl; prev_tr = false; prev_out = -1; continue; }
        i += cl;
        if (tn == 0) { prev_tr = false; prev_out = -1; continue; }   /* delete */
        const uint32_t outc = neg ? tox[tn - 1] : tox[(uint32_t)idx < tn ? (uint32_t)idx : tn - 1];
        if (prev_tr && prev_out == (int64_t)outc) continue;          /* squeeze translated run */
        char enc[4]; fwrite(enc, 1, korb_utf8_encode(outc, enc), ms); prev_tr = true; prev_out = (int64_t)outc;
    }
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf, (uint32_t)sz);
    free(buf);
    return r;
}
/* gsub/sub with a literal String pattern + String|Hash replacement (no regex/block). */
/* Expand a String-pattern gsub/sub replacement's backrefs.  A literal-string
 * match has no capture groups, so only \0/\& (the match), \` (pre), \' (post)
 * and \\ are meaningful; \1..\9 and \+ expand to empty. */
static void korb_str_gsub_emit(FILE *ms, const char *rep, uint32_t rn, const char *src, uint32_t ms0, uint32_t me0, uint32_t sn) {
    for (uint32_t k = 0; k < rn; k++) {
        if (rep[k] == '\\' && k + 1 < rn) {
            const char nx = rep[k + 1];
            if (nx == '0' || nx == '&') { fwrite(src + ms0, 1, me0 - ms0, ms); k++; continue; }
            if (nx == '`')  { fwrite(src, 1, ms0, ms); k++; continue; }
            if (nx == '\'') { fwrite(src + me0, 1, sn - me0, ms); k++; continue; }
            if (nx == '\\') { fputc('\\', ms); k++; continue; }
            if ((nx >= '1' && nx <= '9') || nx == '+') { k++; continue; }   /* no captures → empty */
        }
        fputc(rep[k], ms);
    }
}
static RESULT korb_str_gsub_into(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool global, bool in_place, NODE *block, VALUE *def_env, VALUE *cself) {
    if (in_place) KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));   /* gsub!/sub! on a frozen string → FrozenError, even when nothing matches */
    VALUE pv = VALUE_SLICE_GET(a, 0);
    if (KORB_REGEXP_P(pv)) {                            /* regex pattern → astrogre engine (builtins/regexp.c) */
        NODE *const eff_block = (VALUE_SLICE_LEN(a) >= 2) ? NULL : block;   /* a replacement arg wins over a block */
        return korb_re_str_gsub(c, slots, self, a, pv, global, in_place, eff_block, def_env, cself);
    }
    if (UNLIKELY(!KORB_STRING_P(pv))) {                /* coerce a non-String/Regexp pattern via #to_str */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(pv) && korb_responds_to_coerce_p(c, slots, &pv, to_str)) {
            slots[0] = pv;                             /* recv for the #to_str dispatch (self stays the caller's rooted VALUE_REF) */
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (!KORB_STRING_P(sr.value)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
            pv = sr.value;                             /* the coerced String pattern; downstream roots it like the literal-String path */
        } else return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_type_name(pv));
    }
    /* String pattern → route through the engine as an escaped literal Regexp so
     * $~ / MatchData / block-$~ behave exactly as CRuby.  Falls back to the byte
     * path below when the regex engine can't be loaded. */
    if (korb_re_load(c->vm) != NULL) {
        VALUE litre; RESULT cr = korb_re_literal_regexp(c, slots, pv, &litre);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        NODE *const eff_block = (VALUE_SLICE_LEN(a) >= 2) ? NULL : block;   /* a replacement arg wins over a block */
        return korb_re_str_gsub(c, slots, self, a, litre, global, in_place, eff_block, def_env, cself);
    }
    if (block != NULL && VALUE_SLICE_LEN(a) < 2) {    /* gsub(pat) { |match| ... } — block yields (a replacement arg wins over a block) */
        const KorbString *ps = VAL2STR(pv);
        const uint32_t pn = ps->len; char *const pat = malloc(pn ? pn : 1); memcpy(pat, korb_strbuf_data(ps->buf), pn);
        const KorbString *s0 = VAL2STR(VALUE_REF_GET(self));
        const uint32_t sn = s0->len; char *const src = malloc(sn ? sn : 1); memcpy(src, korb_strbuf_data(s0->buf), sn);  /* stable across block GC */
        char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
        uint32_t i = 0; bool replaced = false;
        while (i < sn || (pn == 0 && i == sn && (global || !replaced))) {
            /* pn==0 (empty pattern) matches at every position, incl. the end. */
            const bool hit = pn == 0 ? (global || !replaced)
                                     : (i + pn <= sn && memcmp(src + i, pat, pn) == 0 && (global || !replaced));
            if (hit) {
                RESULT mr = korb_str_new(c, slots, src + i, pn);   /* the match (src/pat/ms libc-stable) */
                if (UNLIKELY(mr.state != KORB_NORMAL)) { free(pat); free(src); fclose(ms); free(buf); return mr; }
                slots[0] = mr.value;
                RESULT yr = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
                if (UNLIKELY(yr.state != KORB_NORMAL)) { free(pat); free(src); fclose(ms); free(buf); return yr; }
                slots[0] = yr.value;
                if (KORB_STRING_P(slots[0])) { const KorbString *r = VAL2STR(slots[0]); fwrite(korb_strbuf_data(r->buf), 1, r->len, ms); }
                else korb_fprint_to_s(c, ms, slots[0]);
                replaced = true;
                if (pn == 0) { if (i < sn) fputc(src[i], ms); i++; }   /* empty match: still advance one char */
                else i += pn;
            } else { if (i < sn) fputc(src[i], ms);
                     i++; }
        }
        free(pat); free(src); fclose(ms);
        RESULT nr = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
        free(buf);
        if (!in_place) return nr;
        if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
        slots[0] = nr.value;
        const KorbString *res = VAL2STR(slots[0]); const uint32_t w = res->len;
        KorbString *s2 = korb_str_ensure(c, slots + 1, self, w); res = VAL2STR(slots[0]);
        memcpy(korb_strbuf_data(s2->buf), korb_strbuf_data(res->buf), w); s2->len = w; korb_strbuf_data(s2->buf)[w] = '\0';
        return RESULT_OK(replaced ? VALUE_REF_GET(self) : KORB_NIL);
    }
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 2))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "String#gsub/sub without a replacement (Enumerator) is not supported");
    VALUE rv = VALUE_SLICE_GET(a, 1);
    /* snapshot pattern + replacement bytes into stable C buffers (survive grows) */
    const KorbString *ps = VAL2STR(pv);
    uint32_t pn = ps->len; char *pat = malloc(pn ? pn : 1); memcpy(pat, korb_strbuf_data(ps->buf), pn);
    char *rep; uint32_t rn; bool rep_backref = false;
    if (KORB_STRING_P(rv)) {
        const KorbString *rs = VAL2STR(rv);
        rn = rs->len; rep = malloc(rn ? rn : 1); memcpy(rep, korb_strbuf_data(rs->buf), rn);
        rep_backref = memchr(rep, '\\', rn) != NULL;  /* expand \0/\&/\`/\'/\\ per match (CRuby does this for String patterns too) */
    } else if (KORB_HASH_P(rv)) {                     /* hash: matched substring → hash[match].to_s ("" if absent) */
        int32_t idx = korb_hash_find(VAL2HASH(rv), pv);   /* literal pattern → key is the whole match */
        if (idx < 0) { rn = 0; rep = malloc(1); }
        else {
            VALUE val = korb_items_data(VAL2HASH(rv)->items)[2 * idx + 1];
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
    while (i < sn || (pn == 0 && i == sn && (global || !replaced))) {
        const bool hit = pn == 0 ? (global || !replaced)
                                 : (i + pn <= sn && memcmp(korb_strbuf_data(s->buf) + i, pat, pn) == 0 && (global || !replaced));
        if (hit) {
            if (rep_backref) korb_str_gsub_emit(ms, rep, rn, korb_strbuf_data(s->buf), i, i + pn, sn);
            else fwrite(rep, 1, rn, ms);
            replaced = true;
            if (pn == 0) { if (i < sn) fputc(korb_strbuf_data(s->buf)[i], ms); i++; }   /* empty match: emit the replacement then advance a char */
            else i += pn;
        } else {
            if (i < sn) fputc(korb_strbuf_data(s->buf)[i], ms);
            i++;
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
    memcpy(korb_strbuf_data(s2->buf), korb_strbuf_data(res->buf), w);
    s2->len = w; korb_strbuf_data(s2->buf)[w] = '\0';
    return RESULT_OK(replaced ? VALUE_REF_GET(self) : KORB_NIL);
}
static RESULT korb_m_str_gsub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)   { return korb_str_gsub_into(c, slots, self, a, true, false, block, def_env, cself); }
static RESULT korb_m_str_sub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)    { return korb_str_gsub_into(c, slots, self, a, false, false, block, def_env, cself); }
static RESULT korb_m_str_gsub_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_str_gsub_into(c, slots, self, a, true, true, block, def_env, cself); }
static RESULT korb_m_str_sub_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)  { return korb_str_gsub_into(c, slots, self, a, false, true, block, def_env, cself); }
static RESULT korb_m_str_ascii_only(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const uint32_t enc = KORB_STR_ENC(VALUE_REF_GET(self));   /* a non-ASCII-compatible encoding is never ASCII-only */
    if (enc >= KORB_ENC_OTHER_MIN && enc < 8 && c->vm->str_enc_names[enc]) {
        const char *const nm = korb_sym_name(c->vm, c->vm->str_enc_names[enc]);
        if (strncmp(nm, "UTF-16", 6) == 0 || strncmp(nm, "UTF-32", 6) == 0 || strcmp(nm, "UTF-7") == 0)
            return RESULT_OK(KORB_FALSE);
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < s->len; i++)
        if ((unsigned char)korb_strbuf_data(s->buf)[i] >= 0x80) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
static bool korb_str_pure_ascii(const KorbString *s) {
    for (uint32_t i = 0; i < s->len; i++) if ((unsigned char)korb_strbuf_data(s->buf)[i] >= 0x80) return false;
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
    memcpy(korb_strbuf_data(r->buf), korb_strbuf_data(VAL2STR(VALUE_REF_GET(self))->buf), len);   /* re-read after alloc */
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
    if (UNLIKELY(!KORB_STRING_P(sv))) {                /* coerce a non-String separator via #to_str */
        slots[0] = sv;
        if (KORB_OBJECT_P(sv) && korb_responds_to_coerce_p(c, slots, &sv, korb_intern(c->vm, "to_str", 6))) {
            RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            sv = r.value;
        }
        if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(slots[0]));
    }
    slots[0] = sv;                                     /* the (possibly coerced) separator, kept for the result */
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *sep = VAL2STR(slots[0]);
    int32_t at = -1;
    if (sep->len == 0) at = (int32_t)s->len;
    else for (int32_t i = (int32_t)s->len - (int32_t)sep->len; i >= 0; i--)
        if (memcmp(korb_strbuf_data(s->buf) + i, korb_strbuf_data(sep->buf), sep->len) == 0) { at = i; break; }
    uint32_t pre_e, post_s;
    if (at < 0) { pre_e = 0; post_s = 0; }            /* not found → ["","",self] */
    else { pre_e = (uint32_t)at; post_s = (uint32_t)at + sep->len; }
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 3));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    if (at < 0) {
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, "", 0));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, VALUE_REF_GET(self)));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    slots[2] = UNWRAP(korb_str_slice_new(c, slots + 2, self, 0, pre_e));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));   /* the (coerced) separator */
    slots[2] = UNWRAP(korb_str_slice_new(c, slots + 2, self, post_s, VAL2STR(VALUE_REF_GET(self))->len - post_s));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_str_partition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) {                /* coerce a non-String separator via #to_str */
        slots[0] = sv;
        if (KORB_OBJECT_P(sv) && korb_responds_to_coerce_p(c, slots, &sv, korb_intern(c->vm, "to_str", 6))) {
            RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            sv = r.value;
        }
        if (UNLIKELY(!KORB_STRING_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(slots[0]));
    }
    slots[0] = sv;                                     /* the (possibly coerced) separator, kept for the result */
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *sep = VAL2STR(slots[0]);
    int32_t at = (s->len >= sep->len) ? korb_byte_find(korb_strbuf_data(s->buf), s->len, korb_strbuf_data(sep->buf), sep->len) : -1;
    uint32_t post_s = (at < 0) ? 0 : (uint32_t)at + sep->len;   /* before any alloc (sep moves under moving GC) */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 3));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    if (at < 0) {                                     /* not found → [self,"",""] */
        CHECK(korb_ary_push_val(c, slots + 2, dst, VALUE_REF_GET(self)));
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, "", 0));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    slots[2] = UNWRAP(korb_str_slice_new(c, slots + 2, self, 0, (uint32_t)at));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));   /* the (coerced) separator */
    slots[2] = UNWRAP(korb_str_slice_new(c, slots + 2, self, post_s, VAL2STR(VALUE_REF_GET(self))->len - post_s));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_str_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *const s = VAL2STR(VALUE_REF_GET(self));
    const char *const d = korb_strbuf_data(s->buf);
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
    if (UNLIKELY(VALUE_SLICE_LEN(a) == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    if (VAL2STR(VALUE_REF_GET(self))->len == 0) return RESULT_OK(LONG2FIX(0));   /* empty → 0, args unvalidated (CRuby) */
    { RESULT cr = korb_str_sets_coerce(c, slots, a); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
    { RESULT v = korb_str_sets_validate(c, slots, a); if (UNLIKELY(v.state != KORB_NORMAL)) return v; }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t cnt = 0;
    for (uint32_t i = 0; i < s->len; ) {                  /* iterate by UTF-8 codepoint */
        uint32_t cl; const uint32_t cp = korb_utf8_dec1(korb_strbuf_data(s->buf) + i, s->len - i, &cl);
        if (korb_str_sets_match_cp(a, cp)) cnt++;
        i += cl ? cl : 1;
    }
    return RESULT_OK(LONG2FIX(cnt));
}
static RESULT korb_m_str_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t bits = 16;
    if (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) bits = FIX2LONG(VALUE_SLICE_GET(a, 0));
    intptr_t sum = 0;
    for (uint32_t i = 0; i < s->len; i++) sum += (unsigned char)korb_strbuf_data(s->buf)[i];
    if (bits > 0 && bits < 64) sum &= ((intptr_t)1 << bits) - 1;
    return RESULT_OK(LONG2FIX(sum));
}
/* squeeze: collapse runs of identical chars (only those in the sets, if given). */
static RESULT korb_str_squeeze_into(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool in_place) {
    if (in_place) KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));   /* squeeze! checks frozen upfront */
    if (VAL2STR(VALUE_REF_GET(self))->len == 0)          /* empty → ""/nil, args unvalidated (CRuby) */
        return in_place ? RESULT_OK(KORB_NIL) : korb_str_slice_new(c, slots, self, 0, 0);
    { RESULT cr = korb_str_sets_coerce(c, slots, a); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
    { RESULT v = korb_str_sets_validate(c, slots, a); if (UNLIKELY(v.state != KORB_NORMAL)) return v; }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t n = s->len;
    bool has_set = VALUE_SLICE_LEN(a) > 0;
    /* build squeezed bytes into the dst string */
    KorbString *r = korb_str_alloc(c, slots, n);          /* capacity n (worst case) */
    s = VAL2STR(VALUE_REF_GET(self));                      /* re-read after alloc */
    uint32_t w = 0; int64_t prev = -1;
    for (uint32_t i = 0; i < n; ) {                        /* iterate by UTF-8 codepoint */
        uint32_t cl; const uint32_t cp = korb_utf8_dec1(korb_strbuf_data(s->buf) + i, n - i, &cl);
        if (cl == 0) cl = 1;
        bool squeezable = !has_set || korb_str_sets_match_cp(a, cp);
        if (!(squeezable && (int64_t)cp == prev)) { memcpy(korb_strbuf_data(r->buf) + w, korb_strbuf_data(s->buf) + i, cl); w += cl; }
        prev = squeezable ? (int64_t)cp : -1;
        i += cl;
    }
    r->len = w; korb_strbuf_data(r->buf)[w] = '\0';
    if (!in_place) { KORB_STR_ENC_SET((VALUE)r, KORB_STR_ENC(VALUE_REF_GET(self))); return RESULT_OK((VALUE)r); }
    /* copy back into self */
    slots[0] = (VALUE)r;
    bool changed = (w != n);
    KorbString *s2 = korb_str_ensure(c, slots + 1, self, w);
    r = VAL2STR(slots[0]);
    memcpy(korb_strbuf_data(s2->buf), korb_strbuf_data(r->buf), w);
    s2->len = w; korb_strbuf_data(s2->buf)[w] = '\0';
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
    if (UNLIKELY(!KORB_STRING_P(sv))) {                  /* coerce the needle via #to_str */
        RESULT cr = korb_coerce_to_str(c, slots, &sv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (cr.value != KORB_TRUE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    return RESULT_OK(korb_byte_find(korb_strbuf_data(s->buf), s->len, korb_strbuf_data(n->buf), n->len) >= 0 ? KORB_TRUE : KORB_FALSE);
}

static RESULT korb_m_str_start_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE pv = VALUE_SLICE_GET(a, i);
        if (KORB_REGEXP_P(pv)) {                          /* Regexp prefix: matches iff it anchors at byte 0 */
            korb_re_match_t m;
            RESULT rr = korb_re_run(c, slots, pv, VALUE_REF_GET(self), 0, &m);
            if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
            if (rr.value == KORB_TRUE && m.matched && m.starts[0] == 0) {
                slots[0] = VALUE_REF_GET(self); slots[1] = pv;
                VALUE md = UNWRAP(korb_re_build_md(c, slots + 2, slots[0], slots[1], &m));
                korb_re_set_lastmatch(c, md);            /* start_with? sets $~ on a true match */
                return RESULT_OK(KORB_TRUE);
            }
            continue;                                    /* this prefix didn't anchor — try the next arg */
        }
        if (UNLIKELY(!KORB_STRING_P(pv))) {              /* coerce a prefix via #to_str */
            RESULT cr = korb_coerce_to_str(c, slots, &pv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value != KORB_TRUE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, i)));
        }
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));   /* re-read after possible dispatch */
        const KorbString *p = VAL2STR(pv);
        if (p->len <= s->len && memcmp(korb_strbuf_data(s->buf), korb_strbuf_data(p->buf), p->len) == 0) return RESULT_OK(KORB_TRUE);
    }
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_str_end_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE pv = VALUE_SLICE_GET(a, i);
        if (UNLIKELY(!KORB_STRING_P(pv))) {              /* coerce a suffix via #to_str */
            RESULT cr = korb_coerce_to_str(c, slots, &pv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value != KORB_TRUE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, i)));
        }
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));   /* re-read after possible dispatch */
        const KorbString *p = VAL2STR(pv);
        if (p->len <= s->len && memcmp(korb_strbuf_data(s->buf) + s->len - p->len, korb_strbuf_data(p->buf), p->len) == 0) return RESULT_OK(KORB_TRUE);
    }
    return RESULT_OK(KORB_FALSE);
}

/* byte offset of the cidx-th codepoint (clamped to len). */
static uint32_t korb_str_char_to_byte(const KorbString *s, intptr_t cidx) {
    uint32_t b = 0;
    for (intptr_t k = 0; k < cidx && b < s->len; k++) {
        b++;
        while (b < s->len && ((unsigned char)korb_strbuf_data(s->buf)[b] & 0xC0) == 0x80) b++;
    }
    return b;
}
static RESULT korb_m_str_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t boff = 0;
    if (KORB_REGEXP_P(VALUE_SLICE_GET(a, 0))) {       /* index(regexp[, start]) → char index of the match (builtins/regexp.c) */
        long startc = 0;
        if (VALUE_SLICE_LEN(a) >= 2) { intptr_t st = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &st)) startc = (long)st; }
        return korb_re_str_index(c, slots, self, VALUE_SLICE_GET(a, 0), startc, false);
    }
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* index(substr, start): range-check start first */
        intptr_t start;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &start))) {   /* coerce start via #to_int */
            VALUE sv = VALUE_SLICE_GET(a, 1);
            RESULT cr = korb_coerce_to_int(c, slots, &sv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(sv, &start)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        }
        const KorbString *const s0 = VAL2STR(VALUE_REF_GET(self));
        uint32_t ncp = korb_utf8_count(korb_strbuf_data(s0->buf), s0->len);
        if (start < 0) start += ncp;
        if (start < 0 || start > (intptr_t)ncp) return RESULT_OK(KORB_NIL);   /* out of range → nil (needle not coerced) */
        boff = korb_str_char_to_byte(s0, start);
    }
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) {               /* coerce via #to_str, else TypeError (never #to_int) */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (!korb_responds_to_coerce_p(c, slots, &sv, to_str))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
        slots[0] = sv;
        RESULT cr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (UNLIKELY(!KORB_STRING_P(cr.value)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(slots[0]));
        sv = cr.value;
    }
    const KorbString *const s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);   /* re-read s after coercion's GC */
    int32_t b = korb_byte_find(korb_strbuf_data(s->buf) + boff, s->len - boff, korb_strbuf_data(n->buf), n->len);
    if (b < 0) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX(korb_utf8_count(korb_strbuf_data(s->buf), boff + (uint32_t)b)));   /* char index */
}

static RESULT korb_m_str_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int base = 10;
    bool have_base = false;
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* to_i(base): base 0 = auto-detect prefix */
        intptr_t b;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &b))) {   /* coerce base via #to_int */
            VALUE bv = VALUE_SLICE_GET(a, 0);
            RESULT cr = korb_coerce_to_int(c, slots, &bv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(bv, &b)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        }
        base = (int)b; have_base = true;
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    const char *const d = korb_strbuf_data(s->buf); uint32_t i = 0, end = s->len;
    while (i < end && korb_is_ws((unsigned char)d[i])) i++;
    intptr_t sign = 1;
    if (i < end && (d[i] == '+' || d[i] == '-')) { if (d[i] == '-') sign = -1; i++; }
    /* Radix is validated for a truly-EMPTY string or when there's content to
     * parse; a non-empty whitespace/sign-only string returns 0 without checking
     * (CRuby quirk: ""..to_i(1) raises, but "  ".to_i(1) → 0). */
    if (UNLIKELY(have_base && base != 0 && (base < 2 || base > 36) && (end == 0 || i < end)))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid radix %d", base);
    if (i >= end) return RESULT_OK(LONG2FIX(0));      /* blank / sign-only (non-empty) → 0 */
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
    return RESULT_OK(LONG2FIX(korb_str_radix(korb_strbuf_data(s->buf), s->len, 16, false)));
}
static RESULT korb_m_str_oct(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    return RESULT_OK(LONG2FIX(korb_str_radix(korb_strbuf_data(s->buf), s->len, 8, true)));
}

/* String#to_r — lenient parse of [ws][sign]int['/'int | '.'frac]; non-numeric → (0/1). */
static RESULT korb_m_str_to_r(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    const char *const d = korb_strbuf_data(s->buf); const uint32_t len = s->len; uint32_t i = 0;
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
/* Parse one Complex number-part (int / float / rational) at d[*pi] for
 * String#to_c: a leading sign, digit-group underscores, a decimal point,
 * scientific notation (e/E), and an integer `p/q` rational.  Sets *outslot +
 * advances *pi on success. */
static bool korb_str_parse_c_num(CTX *c, VALUE *outslot, const char *d, uint32_t len, uint32_t *pi) {
    uint32_t i = *pi;
    char buf[80]; int bi = 0; bool digit = false, isf = false;
    if (i < len && (d[i] == '+' || d[i] == '-')) buf[bi++] = d[i++];
    while (i < len && (isdigit((unsigned char)d[i]) || d[i] == '_')) { if (d[i] != '_') { if (bi < 78) buf[bi++] = d[i]; digit = true; } i++; }
    if (i + 1 < len && d[i] == '.' && isdigit((unsigned char)d[i+1])) {   /* decimal fraction */
        isf = true; if (bi < 78) buf[bi++] = '.'; i++;
        while (i < len && (isdigit((unsigned char)d[i]) || d[i] == '_')) { if (d[i] != '_' && bi < 78) buf[bi++] = d[i]; i++; }
    }
    if (!digit) return false;
    if (i < len && (d[i] == 'e' || d[i] == 'E')) {                        /* scientific notation */
        uint32_t j = i + 1; if (j < len && (d[j] == '+' || d[j] == '-')) j++;
        if (j < len && isdigit((unsigned char)d[j])) {
            isf = true; if (bi < 78) buf[bi++] = 'e'; i++;
            if (i < len && (d[i] == '+' || d[i] == '-')) { if (bi < 78) buf[bi++] = d[i]; i++; }
            while (i < len && isdigit((unsigned char)d[i])) { if (bi < 78) buf[bi++] = d[i]; i++; }
        }
    }
    buf[bi] = '\0';
    if (!isf && i + 1 < len && d[i] == '/' && isdigit((unsigned char)d[i+1])) {   /* integer p/q rational */
        uint32_t j = i + 1; char db[40]; int dbi = 0;
        while (j < len && (isdigit((unsigned char)d[j]) || d[j] == '_')) { if (d[j] != '_' && dbi < 38) db[dbi++] = d[j]; j++; }
        db[dbi] = '\0'; i = j; *pi = i;
        const intptr_t num = (intptr_t)strtoll(buf, NULL, 10), den = (intptr_t)strtoll(db, NULL, 10);
        *outslot = den ? korb_rat_new(c, outslot, num, den).value : LONG2FIX(0);
        return true;
    }
    *pi = i;
    if (isf) *outslot = korb_float_new(c, outslot, strtod(buf, NULL)).value;
    else     *outslot = LONG2FIX((intptr_t)strtoll(buf, NULL, 10));
    return true;
}
/* String#to_c — real[±imag(i|j)] / pure "Ni" / bare "±i" / polar "m@a"; a
 * non-numeric string → (0+0i). */
static RESULT korb_m_str_to_c(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    const uint32_t len = s->len; uint32_t i = 0;
    /* Copy the bytes to the stack: parsing allocates (Float/Rational/Complex),
     * so a raw pointer into the String's buffer would go stale under moving GC. */
    char sbuf[512];
    slots[0] = LONG2FIX(0); slots[1] = LONG2FIX(0);
    if (len >= sizeof(sbuf)) return korb_cpx_new(c, slots + 2, slots[0], slots[1]);   /* too long for a Complex literal → (0+0i) */
    memcpy(sbuf, korb_strbuf_data(s->buf), len);
    const char *const d = sbuf;
    while (i < len && isspace((unsigned char)d[i])) i++;
    #define IMAG_UNIT(ch) (((ch) | 0x20) == 'i' || ((ch) | 0x20) == 'j')
    if (!korb_str_parse_c_num(c, &slots[2], d, len, &i)) {   /* no leading number: bare ±i */
        uint32_t j = i; intptr_t sg = 1;
        if (j < len && (d[j] == '+' || d[j] == '-')) { if (d[j] == '-') sg = -1; j++; }
        if (j < len && IMAG_UNIT(d[j])) slots[1] = LONG2FIX(sg);
        return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
    }
    if (i < len && d[i] == '@') {                            /* polar form: modulus @ argument */
        i++;
        if (korb_str_parse_c_num(c, &slots[3], d, len, &i)) {
            double m = 0, arg = 0; korb_num_to_d(slots[2], &m); korb_num_to_d(slots[3], &arg);
            slots[0] = korb_float_new(c, slots + 4, m * cos(arg)).value;
            slots[1] = korb_float_new(c, slots + 4, m * sin(arg)).value;
        }
        return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
    }
    if (i < len && IMAG_UNIT(d[i])) {                        /* "Ni" → pure imaginary */
        slots[1] = slots[2];
    } else {
        slots[0] = slots[2];                                /* real part */
        if (i < len && (d[i] == '+' || d[i] == '-')) {      /* ±imag suffix */
            const uint32_t save = i;
            const intptr_t sg = (d[i] == '-') ? -1 : 1;
            if (i + 1 < len && IMAG_UNIT(d[i+1])) slots[1] = LONG2FIX(sg);       /* "a±i" → imag ±1 */
            else if (korb_str_parse_c_num(c, &slots[3], d, len, &i) && i < len && IMAG_UNIT(d[i])) slots[1] = slots[3];
            else i = save;
        }
    }
    return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
    #undef IMAG_UNIT
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
            unsigned char ch = (unsigned char)korb_strbuf_data(s->buf)[start];
            if (has_set ? korb_str_sets_match(a, ch) : (korb_is_ws(ch) || ch == '\0')) start++;
            else break;
        }
    if (mode != 1)
        while (end > start) {
            unsigned char ch = (unsigned char)korb_strbuf_data(s->buf)[end-1];
            if (has_set ? korb_str_sets_match(a, ch) : (korb_is_ws(ch) || ch == '\0')) end--;
            else break;
        }
    return korb_str_slice_new(c, slots, self, start, end - start);
}
static RESULT korb_m_str_strip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_strip(c, slots, self, a, 0); }
static RESULT korb_m_str_lstrip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_strip(c, slots, self, a, 1); }
static RESULT korb_m_str_rstrip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_strip(c, slots, self, a, 2); }

static RESULT korb_m_str_chomp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* chomp(sep) */
        VALUE sv = VALUE_SLICE_GET(a, 0);
        if (sv == KORB_NIL) return korb_str_slice_new(c, slots, self, 0, VAL2STR(VALUE_REF_GET(self))->len);   /* nil sep → unchanged copy */
        if (UNLIKELY(!KORB_STRING_P(sv))) {           /* coerce a non-String separator via #to_str */
            slots[0] = VALUE_REF_GET(self);           /* root self across the dispatch */
            slots[1] = sv;
            if (KORB_OBJECT_P(sv) && korb_responds_to_coerce(c, slots + 2, slots[1], korb_intern(c->vm, "to_str", 6))) {
                RESULT sr = korb_send_impl(c, slots + 2, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                slots[1] = sr.value;
            }
            if (UNLIKELY(!KORB_STRING_P(slots[1]))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
            self = VALUE_REF_AT(&slots[0]); sv = slots[1];   /* self re-rooted; sv now the coerced String */
        }
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));
        uint32_t len = s->len;
        const KorbString *sep = VAL2STR(sv);
        if (sep->len == 1 && korb_strbuf_data(sep->buf)[0] == '\n') {   /* "\n" = the universal line ending: \r\n, \n, or \r */
            if (len >= 2 && korb_strbuf_data(s->buf)[len-2] == '\r' && korb_strbuf_data(s->buf)[len-1] == '\n') len -= 2;
            else if (len >= 1 && (korb_strbuf_data(s->buf)[len-1] == '\n' || korb_strbuf_data(s->buf)[len-1] == '\r')) len -= 1;
        } else if (sep->len == 0) {                   /* "" → strip all trailing \n / \r\n */
            while (len >= 2 && korb_strbuf_data(s->buf)[len-2] == '\r' && korb_strbuf_data(s->buf)[len-1] == '\n') len -= 2;
            while (len >= 1 && korb_strbuf_data(s->buf)[len-1] == '\n') len -= 1;
        } else if (len >= sep->len && memcmp(korb_strbuf_data(s->buf) + len - sep->len, korb_strbuf_data(sep->buf), sep->len) == 0) {
            len -= sep->len;                          /* one trailing occurrence */
        }
        return korb_str_slice_new(c, slots + 2, self, 0, len);   /* slots[0]=self, slots[1]=sep may be live */
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t len = s->len;
    if (len >= 2 && korb_strbuf_data(s->buf)[len-2] == '\r' && korb_strbuf_data(s->buf)[len-1] == '\n') len -= 2;
    else if (len >= 1 && (korb_strbuf_data(s->buf)[len-1] == '\n' || korb_strbuf_data(s->buf)[len-1] == '\r')) len -= 1;
    return korb_str_slice_new(c, slots, self, 0, len);
}

static RESULT korb_m_str_chop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t len = s->len;
    if (len >= 2 && korb_strbuf_data(s->buf)[len-2] == '\r' && korb_strbuf_data(s->buf)[len-1] == '\n') len -= 2;
    else if (len >= 1) {
        len--;                                  /* drop a whole trailing UTF-8 codepoint */
        while (len > 0 && ((unsigned char)korb_strbuf_data(s->buf)[len] & 0xC0) == 0x80) len--;
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
        VALUE e = korb_items_data(d->items)[i];
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
    if (VALUE_SLICE_LEN(a) >= 2 && VALUE_SLICE_GET(a, 1) != KORB_NIL) {
        VALUE lv = VALUE_SLICE_GET(a, 1);
        if (!korb_to_index(lv, &limit)) {                    /* coerce a non-Integer limit via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &lv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            (void)korb_to_index(lv, &limit);
        }
    }
    if (limit == 1) {                                         /* whole string (sep untouched); empty → [] */
        const uint32_t slen = VAL2STR(VALUE_REF_GET(self))->len;
        VALUE_REF d1 = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 1)));
        if (slen > 0) CHECK(korb_ary_push_val(c, slots + 1, d1, UNWRAP(korb_str_slice_new(c, slots + 1, self, 0, slen))));
        return korb_split_finish(c, slots + 1, self, d1, block, def_env, cself);
    }
    if (KORB_REGEXP_P(sepv)) {                                /* regex separator → astrogre (builtins/regexp.c) */
        RESULT sr = korb_re_str_split(c, slots, self, sepv, (long)limit);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        slots[0] = sr.value;
        return korb_split_finish(c, slots + 1, self, VALUE_REF_AT(&slots[0]), block, def_env, cself);
    }
    bool ws = (sepv == KORB_NIL);
    if (!ws) {
        if (UNLIKELY(!KORB_STRING_P(sepv))) {                /* coerce a non-String pattern via #to_str */
            const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
            if (KORB_OBJECT_P(sepv) && korb_responds_to_coerce_p(c, slots, &sepv, to_str)) {
                slots[0] = sepv;                             /* root receiver across the dispatch */
                RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                if (LIKELY(KORB_STRING_P(sr.value))) { VALUE_REF_SET(VALUE_SLICE_REF(a, 0), sr.value); sepv = sr.value; }
            }
            if (UNLIKELY(!KORB_STRING_P(sepv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sepv));
        }
        const KorbString *sp = VAL2STR(sepv);
        if (sp->len == 1 && korb_strbuf_data(sp->buf)[0] == ' ') ws = true;   /* " " behaves as whitespace */
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
            while (cpos + cl < s->len && ((unsigned char)korb_strbuf_data(s->buf)[cpos+cl] & 0xC0) == 0x80) cl++;
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
            uint32_t ws_start = pos;
            while (pos < slen && korb_is_ws((unsigned char)korb_strbuf_data(s->buf)[pos])) pos++;
            if (last_field) {   /* positive limit: remainder (incl. any trailing ws) is the last field, even if empty */
                CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, slen - pos)))); break;
            }
            if (pos >= slen) {  /* end reached; a non-zero limit (+ or -) keeps one trailing "" when trailing ws followed ≥1 field */
                if (limit != 0 && pos > ws_start && VAL2ARY(VALUE_REF_GET(dst))->len > 0)
                    CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_new(c, slots + 1, "", 0))));
                break;
            }
            uint32_t start = pos;
            while (pos < slen && !korb_is_ws((unsigned char)korb_strbuf_data(s->buf)[pos])) pos++;
            CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, start, pos - start))));
        } else {
            const KorbString *sep = VAL2STR(VALUE_REF_GET(sepref));
            uint32_t seplen = sep->len;
            int32_t found = (!last_field && pos <= slen) ? korb_byte_find(korb_strbuf_data(s->buf) + pos, slen - pos, korb_strbuf_data(sep->buf), seplen) : -1;
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
        while (d->len > 0 && KORB_STRING_P(korb_items_data(d->items)[d->len-1]) && VAL2STR(korb_items_data(d->items)[d->len-1])->len == 0) {
            ARO_STORE(c, d->items, &korb_items_data(d->items)[--d->len], KORB_NIL);
        }
    }
    return korb_split_finish(c, slots + 1, self, dst, block, def_env, cself);
}

static RESULT korb_m_str_charlen(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE v = VALUE_REF_GET(self);
    const KorbString *const s = VAL2STR(v);
    const uint32_t enc = KORB_STR_ENC(v);
    if (LIKELY(enc == KORB_ENC_UTF8)) return RESULT_OK(LONG2FIX((intptr_t)korb_utf8_count(korb_strbuf_data(s->buf), s->len)));
    if (KORB_ENC_IS_SINGLE_BYTE(enc) || korb_str_bytes_ascii(v)) return RESULT_OK(LONG2FIX((intptr_t)s->len));
    return korb_str_enc_notimpl(c, slots, v);
}

static RESULT korb_m_str_chars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const uint32_t enc = KORB_STR_ENC(VALUE_REF_GET(self));
    if (UNLIKELY(KORB_ENC_NEEDS_HOOK(enc)) && !korb_str_bytes_ascii(VALUE_REF_GET(self))) return korb_str_enc_notimpl(c, slots, VALUE_REF_GET(self));
    const bool single = KORB_ENC_IS_SINGLE_BYTE(enc);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));
        if (pos >= s->len) break;
        uint32_t cl = 1;                                  /* single-byte enc: 1 byte = 1 char */
        if (!single) while (pos + cl < s->len && ((unsigned char)korb_strbuf_data(s->buf)[pos+cl] & 0xC0) == 0x80) cl++;
        CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, cl))));
        pos += cl;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_str_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(o)) {                              /* coerce via #to_str, else incomparable */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(o) && korb_responds_to_coerce_p(c, slots, &o, to_str)) {
            slots[0] = o;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            o = sr.value;
        }
        if (!KORB_STRING_P(o)) {
            /* not a String and no #to_str: fall back to -(other <=> self), sign-normalized
             * (CRuby rb_invcmp) — e.g. an object that defines #<=> but not #to_str. */
            const VALUE ov = VALUE_SLICE_GET(a, 0);
            const uint32_t cmp = korb_intern(c->vm, "<=>", 3);
            if (KORB_OBJECT_P(ov) && korb_responds_to(c, ov, cmp)) {
                AroObjectHeader *const oh = (AroObjectHeader *)(uintptr_t)ov;
                if (oh->flags & KORB_FL_JOIN_VISITING)       /* ov <=> self already in flight → mutual inverse-compare, CRuby returns nil */
                    return RESULT_OK(KORB_NIL);
                slots[0] = ov;
                slots[1] = VALUE_REF_GET(self);
                oh->flags |= KORB_FL_JOIN_VISITING;
                RESULT ir = korb_send_impl(c, slots + 2, cmp, 0, 1, NULL, NULL, KORB_NIL);   /* ov <=> self */
                ((AroObjectHeader *)(uintptr_t)slots[0])->flags &= ~KORB_FL_JOIN_VISITING;   /* re-read: dispatch may have moved ov */
                if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
                if (FIXNUM_P(ir.value)) {
                    const intptr_t r = FIX2LONG(ir.value);
                    return RESULT_OK(LONG2FIX(r > 0 ? -1 : (r < 0 ? 1 : 0)));   /* negate the sign */
                }
            }
            return RESULT_OK(KORB_NIL);
        }
    }
    return RESULT_OK(LONG2FIX(korb_cmp_values(VALUE_REF_GET(self), o)));
}

/* String#[] — int index, (int,len), Range, or substring match.  Indices are
 * codepoints; results are fresh strings (or nil). */
/* Index/length → intptr_t for String#[]: an Integer (Bignum) too big for a long
 * is a RangeError; a genuinely non-Integer value is a TypeError. */
static RESULT korb_str_idx_conv(CTX *c, VALUE *slots, VALUE v, intptr_t *out) {
    if (LIKELY(korb_to_index(v, out))) return RESULT_OK(KORB_NIL);
    if (KORB_BIGNUM_P(v)) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into `long'");
    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(v));
}
static RESULT korb_m_str_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE i0 = VALUE_SLICE_GET(a, 0);
    if (KORB_REGEXP_P(i0))                              /* s[regexp] / s[regexp, group] → matched text (builtins/regexp.c) */
        return korb_re_str_aref(c, slots, self, i0, VALUE_SLICE_LEN(a) >= 2 ? VALUE_SLICE_GET(a, 1) : KORB_NIL);
    if (UNLIKELY(VALUE_SLICE_LEN(a) >= 2 && (KORB_STRING_P(i0) || KORB_RANGE_P(i0))))   /* the (start, len) form needs an Integer index, not a String/Range */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(i0));
    if (!KORB_STRING_P(i0) && !KORB_RANGE_P(i0)) {     /* coerce a non-String/Range index via #to_int (before reading self) */
        RESULT cr = korb_coerce_to_int(c, slots, &i0);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    }
    const KorbString *s = SELF_STR;
    const uint32_t enc = KORB_STR_ENC(VALUE_REF_GET(self));
    if (UNLIKELY(KORB_ENC_NEEDS_HOOK(enc)) && !korb_str_bytes_ascii(VALUE_REF_GET(self))) return korb_str_enc_notimpl(c, slots, VALUE_REF_GET(self));
    const bool sb = KORB_ENC_IS_SINGLE_BYTE(enc);   /* single-byte enc: char index == byte index */
    uint32_t ncp = sb ? s->len : korb_utf8_count(korb_strbuf_data(s->buf), s->len);
    #define BOFF(ci) (sb ? ((uint32_t)(ci) < s->len ? (uint32_t)(ci) : s->len) : korb_utf8_byteoff(korb_strbuf_data(s->buf), s->len, (uint32_t)(ci)))

    if (KORB_STRING_P(i0)) {                       /* s[substr] → copy of substr if present */
        const KorbString *sub = VAL2STR(i0);
        if (korb_byte_find(korb_strbuf_data(s->buf), s->len, korb_strbuf_data(sub->buf), sub->len) < 0) return RESULT_OK(KORB_NIL);
        return korb_str_slice_new(c, slots, VALUE_SLICE_REF(a, 0), 0, sub->len);
    }
    if (KORB_RANGE_P(i0)) {
        const KorbRange *r = VAL2RANGE(i0);
        const bool beginless = (r->rbegin == KORB_NIL);   /* s[..e] → from 0 */
        const bool endless   = (r->rend   == KORB_NIL);   /* s[b..] → to the end */
        intptr_t b, e;                                     /* Float/to_int bounds coerce via korb_to_index */
        if (beginless) b = 0;
        else { RESULT cr = korb_str_idx_conv(c, slots, r->rbegin, &b); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
        if (endless) e = (intptr_t)ncp;
        else { RESULT cr = korb_str_idx_conv(c, slots, r->rend, &e); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
        if (b < 0) b += ncp;
        if (!endless && e < 0) e += ncp;
        if (b < 0 || b > (intptr_t)ncp) return RESULT_OK(KORB_NIL);
        intptr_t last = (endless || r->exclude_end) ? e - 1 : e;
        intptr_t cnt = last - b + 1;
        if (cnt < 0) cnt = 0;
        if (b + cnt > (intptr_t)ncp) cnt = (intptr_t)ncp - b;
        uint32_t bs = BOFF(b);
        uint32_t es = BOFF(b + cnt);
        return korb_str_slice_new(c, slots, self, bs, es - bs);
    }
    intptr_t i;
    { RESULT cr = korb_str_idx_conv(c, slots, i0, &i); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
    if (i < 0) i += ncp;

    if (VALUE_SLICE_LEN(a) >= 2) {                  /* s[start, len] */
        VALUE lv = VALUE_SLICE_GET(a, 1);
        intptr_t len;
        if (UNLIKELY(!korb_to_index(lv, &len))) {   /* coerce length via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &lv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            { RESULT icr = korb_str_idx_conv(c, slots, lv, &len); if (UNLIKELY(icr.state != KORB_NORMAL)) return icr; }
            s = SELF_STR;                           /* re-read after dispatch */
        }
        if (len < 0 || i < 0 || i > (intptr_t)ncp) return RESULT_OK(KORB_NIL);
        if (i + len > (intptr_t)ncp) len = (intptr_t)ncp - i;
        uint32_t bs = BOFF(i);
        uint32_t es = BOFF(i + len);
        return korb_str_slice_new(c, slots, self, bs, es - bs);
    }
    if (i < 0 || i >= (intptr_t)ncp) return RESULT_OK(KORB_NIL);   /* single codepoint */
    uint32_t bs = BOFF(i);
    uint32_t es = BOFF(i + 1);
    return korb_str_slice_new(c, slots, self, bs, es - bs);
    #undef BOFF
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
                                 RESULT (*arr_fn)(CTX *, VALUE *, VALUE_REF, VALUE_SLICE), const char *name, VALUE_SLICE a) {
    slots[0] = UNWRAP(arr_fn(c, slots, self, a));   /* forward sep/chomp args (each_line) */
    slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), name));
    return korb_enum_new(c, slots + 2, slots[0], slots[1]);
}
static RESULT korb_m_str_each_codepoint(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) return korb_str_each_enum(c, slots, self, korb_m_str_codepoints, "each_codepoint", a);
    for (uint32_t pos = 0; ; ) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        uint32_t cl; uint32_t cp = korb_utf8_decode(korb_strbuf_data(s->buf) + pos, s->len - pos, &cl);
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
    char *buf = malloc(n + 2); memcpy(buf, korb_strbuf_data(s0->buf), n);
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
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_STR_ENC(VALUE_REF_GET(self)));   /* succ preserves encoding */
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
        (ns->len == 0 || memcmp(korb_strbuf_data(ns->buf), korb_strbuf_data(os->buf), ns->len) == 0))
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
    bool neg = fs->len > 1 && korb_strbuf_data(fs->buf)[0] == '^';   /* a lone "^" is the literal char, not a complement */
    unsigned char fromx[512];
    uint32_t fn = korb_tr_expand(korb_strbuf_data(fs->buf) + (neg ? 1 : 0), fs->len - (neg ? 1u : 0u), fromx, 512);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char ch = (unsigned char)korb_strbuf_data(s->buf)[i];
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
        uint32_t cl; uint32_t cp = korb_utf8_decode(korb_strbuf_data(s->buf) + pos, s->len - pos, &cl);
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(cp)));
        pos += cl;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_str_each_byte(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) return korb_str_each_enum(c, slots, self, korb_m_str_bytes, "each_byte", a);
    for (uint32_t pos = 0; ; pos++) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        VALUE bv = LONG2FIX((unsigned char)korb_strbuf_data(s->buf)[pos]);
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
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX((unsigned char)korb_strbuf_data(SELF_STR->buf)[i])));
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
        if (e + seplen <= s->len && memcmp(korb_strbuf_data(s->buf) + e, sep, seplen) == 0) { e += seplen; break; }
        e++;
    }
    return e - pos;
}
/* One line/paragraph starting at pos.  Returns the yielded byte length (chomped if
 * requested); *adv = bytes to advance to the next start.  In paragraph mode
 * (seplen==0) a paragraph ends after the first "\n\n"; extra trailing newlines of
 * that run are skipped (advanced past) but not yielded. */
static uint32_t korb_str_line_span(const KorbString *s, uint32_t pos, const char *sep, uint32_t seplen, bool chomp, bool universal, uint32_t *adv) {
    if (seplen == 0) {                                     /* paragraph mode */
        uint32_t m = pos; bool found = false;
        while (m + 1 < s->len) { if (korb_strbuf_data(s->buf)[m] == '\n' && korb_strbuf_data(s->buf)[m + 1] == '\n') { found = true; break; } m++; }
        if (!found) { *adv = s->len - pos; return *adv; }  /* last paragraph: rest of string (no run to chomp) */
        uint32_t rune = m; while (rune < s->len && korb_strbuf_data(s->buf)[rune] == '\n') rune++;   /* end of the newline run */
        *adv = rune - pos;
        uint32_t yl = (m + 2) - pos;                       /* text + exactly two newlines */
        if (chomp) while (yl > 0 && korb_strbuf_data(s->buf)[pos + yl - 1] == '\n') yl--;
        return yl;
    }
    const uint32_t ll = korb_str_line_len(s, pos, sep, seplen);
    *adv = ll;
    if (chomp && ll >= seplen && memcmp(korb_strbuf_data(s->buf) + pos + ll - seplen, sep, seplen) == 0) {
        uint32_t yl = ll - seplen;                         /* with the default separator, chomp strips \r\n as a unit */
        if (universal && yl > 0 && korb_strbuf_data(s->buf)[pos + yl - 1] == '\r') yl--;
        return yl;
    }
    return ll;
}
/* resolve the line separator arg (a[0]) → bytes; default "\n". */
ARO_BORROW static const char *korb_line_sep(VALUE_SLICE a, uint32_t *seplen) {
    if (VALUE_SLICE_LEN(a) >= 1 && KORB_STRING_P(VALUE_SLICE_GET(a, 0))) {
        const KorbString *sp = VAL2STR(VALUE_SLICE_GET(a, 0));
        *seplen = sp->len; return korb_strbuf_data(sp->buf);
    }
    *seplen = 1; return "\n";
}
/* lines/each_line `chomp:` keyword (trailing Hash). */
static bool korb_line_chomp(CTX *c, VALUE_SLICE a) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) {
        const int32_t idx = korb_hash_find(VAL2HASH(VALUE_SLICE_GET(a, n - 1)), ID2SYM(korb_intern(c->vm, "chomp", 5)));
        if (idx >= 0) return KORB_TRUTHY(korb_items_data(VAL2HASH(VALUE_SLICE_GET(a, n - 1))->items)[2 * idx + 1]);
    }
    return false;
}
/* Coerce a non-String / non-nil line separator via #to_str (in place in `a`);
 * a Symbol or other non-convertible value → TypeError.  A trailing Hash is the
 * chomp: kwarg, not a separator, so it is left alone. */
static RESULT korb_line_coerce_sep(CTX *c, VALUE *slots, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) < 1) return RESULT_OK(KORB_NIL);
    VALUE s0 = VALUE_SLICE_GET(a, 0);
    if (s0 == KORB_NIL || KORB_STRING_P(s0) || KORB_HASH_P(s0)) return RESULT_OK(KORB_NIL);
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    if (KORB_OBJECT_P(s0) && korb_responds_to_coerce_p(c, slots, &s0, to_str)) {
        slots[0] = s0;                                   /* root receiver across the dispatch */
        RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        if (LIKELY(KORB_STRING_P(sr.value))) { VALUE_REF_SET(VALUE_SLICE_REF(a, 0), sr.value); return RESULT_OK(KORB_NIL); }
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(s0));
}
static RESULT korb_m_str_each_line(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    if (block == NULL) return korb_str_each_enum(c, slots, self, korb_m_str_lines, "each_line", a);
    if (SELF_STR->len > 0)
        { RESULT cr = korb_line_coerce_sep(c, slots, a); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) == KORB_NIL) {   /* nil separator → yield the whole string once */
        slots[0] = UNWRAP(korb_str_slice_new(c, slots, self, 0, SELF_STR->len));
        CHECK(korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self));
        return RESULT_OK(VALUE_REF_GET(self));
    }
    char sepbuf[64]; uint32_t seplen;
    { const char *sp = korb_line_sep(a, &seplen); if (seplen > 63) seplen = 63; memcpy(sepbuf, sp, seplen); }
    const bool chomp = korb_line_chomp(c, a);
    const bool universal = !(VALUE_SLICE_LEN(a) >= 1 && KORB_STRING_P(VALUE_SLICE_GET(a, 0)));   /* default $/ → \r\n chomped as a unit */
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        uint32_t ll; uint32_t yl = korb_str_line_span(s, pos, sepbuf, seplen, chomp, universal, &ll);
        slots[0] = UNWRAP(korb_str_slice_new(c, slots, self, pos, yl));   /* root the line (chomped if requested) */
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        pos += ll;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_lines(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (SELF_STR->len > 0)   /* an empty string short-circuits to [] without validating the separator (CRuby) */
        { RESULT cr = korb_line_coerce_sep(c, slots, a); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) == KORB_NIL) {   /* nil sep → whole string as one line */
        VALUE_REF d = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 1)));
        CHECK(korb_ary_push_val(c, slots + 1, d, UNWRAP(korb_str_slice_new(c, slots + 1, self, 0, VAL2STR(VALUE_REF_GET(self))->len))));
        return RESULT_OK(VALUE_REF_GET(d));
    }
    char sepbuf[64]; uint32_t seplen;
    { const char *sp = korb_line_sep(a, &seplen); if (seplen > 63) seplen = 63; memcpy(sepbuf, sp, seplen); }
    const bool chomp = korb_line_chomp(c, a);
    const bool universal = !(VALUE_SLICE_LEN(a) >= 1 && KORB_STRING_P(VALUE_SLICE_GET(a, 0)));   /* default $/ → \r\n chomped as a unit */
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        uint32_t ll; uint32_t yl = korb_str_line_span(s, pos, sepbuf, seplen, chomp, universal, &ll);
        CHECK(korb_ary_push_val(c, slots + 1, dst, UNWRAP(korb_str_slice_new(c, slots + 1, self, pos, yl))));
        pos += ll;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_str_each_char(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) return korb_str_each_enum(c, slots, self, korb_m_str_chars, "each_char", a);
    uint32_t pos = 0;
    for (;;) {
        const KorbString *s = SELF_STR;
        if (pos >= s->len) break;
        uint32_t cl = 1;
        while (pos + cl < s->len && ((unsigned char)korb_strbuf_data(s->buf)[pos+cl] & 0xC0) == 0x80) cl++;
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
    if (VALUE_SLICE_LEN(a) < 1) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of nil into String");
    const bool excl = VALUE_SLICE_LEN(a) >= 2 && KORB_TRUTHY(VALUE_SLICE_GET(a, 1));
    slots[0] = VALUE_REF_GET(self);          /* cur (rooted) */
    slots[1] = VALUE_SLICE_GET(a, 0);        /* end (rooted) */
    if (!KORB_STRING_P(slots[1])) {          /* coerce end via #to_str */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(slots[1]) && korb_responds_to_coerce(c, slots + 2, slots[1], to_str)) {
            RESULT sr = korb_send_impl(c, slots + 2, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            slots[1] = sr.value;
            slots[0] = VALUE_REF_GET(self);  /* re-read after dispatch */
        }
        if (!KORB_STRING_P(slots[1]))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    if (block == NULL) slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 8));   /* collect for the Enumerator */
    /* single-byte begin AND end → iterate by byte value (CRuby fast path:
     * "9".upto("A") = 9 : ; < = > ? @ A, NOT succ which would carry "9"→"10"). */
    if (VAL2STR(slots[0])->len == 1 && VAL2STR(slots[1])->len == 1) {
        const int b = (unsigned char)korb_strbuf_data(VAL2STR(slots[0])->buf)[0];
        const int e = (unsigned char)korb_strbuf_data(VAL2STR(slots[1])->buf)[0];
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
    memcpy(kb, korb_strbuf_data(key->buf), key->len); kb[key->len] = '\0';
    memcpy(sb, korb_strbuf_data(salt->buf), salt->len); sb[salt->len] = '\0';
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

