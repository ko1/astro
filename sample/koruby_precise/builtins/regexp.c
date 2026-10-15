/* koruby_precise — regexp.c: Regexp + MatchData + String-regex methods.
 * #included into korb_runtime.c AFTER string.c (needs korb_utf8_* / korb_str_slice_new)
 * and set.c (needs korb_obj_singleton).
 *
 * Matching is delegated to astrogre via koruby_regex.so → libastrogre.so (see
 * regex_bridge.c) which returns per-group byte spans + named-capture info.  A
 * MatchData carries the subject, the source Regexp, and a KorbArray of Integer
 * byte offsets [b0,e0,b1,e1,...] (-1 for a group that didn't participate).  $~
 * (and thus $1..$9, $&, $`, $') is kept in the flat const table under "$~". */

static const char *korb_re_arg_type(korb_value v);   /* fwd (defined below) */
static const char *korb_enc_idx_name(const struct korb_vm *vm, uint32_t idx);   /* fwd */

/* Regexp::FIXEDENCODING passed to Regexp.new: no prism flag says "fixed to the
 * source's own encoding", so koruby carries it in a private bit. */
#define KORB_RE_FIXENC 0x10000u

/* The pattern's own encoding, for the match-time compatibility check.  Only the
 * cases the prelude's Regexp#encoding is sure about are reported; /e and /s name
 * encodings koruby never transcodes to, so they stay unknown and the caller
 * falls back to comparing against the source's encoding. */
static uint32_t korb_re_enc_idx(const struct korb_vm *vm, korb_value re, bool *out_fixed, bool *out_known)
{
    const uint32_t f = korb_re_u8_flags(vm, re);
    const korb_value srcv = VAL2RE(re)->source;
    const uint32_t senc = KORB_STRING_P(srcv) ? KORB_STR_ENC(srcv) : KORB_ENC_BINARY;
    *out_known = true;
    if (f & 128u) {   /* /n is NOENCODING, never FIXEDENCODING */
        *out_fixed = false;
        return (KORB_STRING_P(srcv) && korb_str_ascii_only_p(vm, srcv)) ? KORB_ENC_USASCII : KORB_ENC_BINARY;
    }
    if (f & (512u | KORB_RE_U8FIX)) { *out_fixed = true; return KORB_ENC_UTF8; }   /* /u, or \u >= 0x80 etc. */
    if (f & (64u | 256u)) { *out_fixed = true; *out_known = false; return senc; }  /* /e, /s */
    *out_fixed = (f & KORB_RE_FIXENC) != 0 || !korb_enc_ascii_compat_idx(vm, senc) ||
                 (KORB_STRING_P(srcv) && !korb_str_bytes_ascii(srcv));   /* non-ASCII source pins its encoding */
    return senc;
}
/* CRuby's message shape for a regexp/subject encoding mismatch. */
static RESULT korb_re_raise_enc_mismatch(CTX *c, korb_value *slots, uint32_t renc, uint32_t senc)
{
    char msg[160];
    snprintf(msg, sizeof msg, "incompatible encoding regexp match (%s regexp with %s string)",
             korb_enc_inspect_name_of(c->vm, renc), korb_enc_inspect_name_of(c->vm, senc));
    return korb_raise_enc_compat_msg(c, slots, msg);
}

/* True when the pattern pins an encoding, so a subject in a different one may
 * be a CompatibilityError.  An unpinned pattern adapts to any ASCII-compatible
 * subject, which is the overwhelmingly common case — keep it off the call. */
static inline bool korb_re_enc_pinned(korb_value re)
{
    const korb_value srcv = VAL2RE(re)->source;
    return (VAL2RE(re)->flags & (64u | 128u | 256u | 512u | KORB_RE_FIXENC | KORB_RE_U8FIX)) != 0 ||
           (KORB_STRING_P(srcv) && KORB_STR_ENC(srcv) >= KORB_ENC_OTHER_MIN);
}

/* CRuby's rb_reg_prepare_enc: an encoding the pattern can't be read in is a
 * CompatibilityError.  Only the pattern's *explicitly* pinned encodings are
 * enforced, so an ordinary ASCII pattern still matches anything. */
static RESULT korb_re_check_enc(CTX *c, korb_value *slots, korb_value re, korb_value subj)
{
    const uint32_t senc = KORB_STR_ENC(subj);
    bool re_fixed = false, re_known = false;
    uint32_t renc = korb_re_enc_idx(c->vm, re, &re_fixed, &re_known);
    if (!re_fixed && renc == KORB_ENC_UTF8 && KORB_STRING_P(VAL2RE(re)->source) && korb_str_bytes_ascii(VAL2RE(re)->source))
        renc = KORB_ENC_USASCII;                          /* CRuby tags a 7-bit pattern US-ASCII */

    /* Rule 2 of rb_reg_prepare_enc: an ASCII-incompatible subject (UTF-16 etc.)
     * cannot be searched at all, whatever the pattern is. */
    if (!korb_enc_ascii_compat_idx(c->vm, senc)) {
        return korb_re_raise_enc_mismatch(c, slots, renc, senc);
    } else if (re_fixed && re_known && renc != senc) {
        if (!korb_enc_ascii_compat_idx(c->vm, renc) || !korb_str_ascii_only_p(c->vm, subj))
            return korb_re_raise_enc_mismatch(c, slots, renc, senc);
    } else if ((VAL2RE(re)->flags & 128u) && senc != KORB_ENC_BINARY &&
               !korb_str_ascii_only_p(c->vm, subj)) {
        korb_warn(c, slots, "historical binary regexp match /.../n against %s string",
                  korb_enc_idx_name(c->vm, senc));
    }
    /* CRuby also raises ArgumentError on a broken subject.  Not done here: it
     * needs the whole subject walked, and without CRuby's cached coderange that
     * is an O(n) tax on every match (measured ~11% on a regex-heavy bench). */
    return RESULT_OK(KORB_NIL);
}

/* CRuby buckets a match by the subject's coderange first, so a subject whose
 * bytes are not valid in its own encoding is an ArgumentError whatever the
 * pattern matches.  sub/gsub/split/scan/index check at their own entry; this is
 * for the plain match forms.  Deliberately NOT inside korb_re_run — the looping
 * methods call that once per match, and checking there would be O(n^2). */
static bool korb_str_utf8_valid(const KorbString *s);   /* fwd (builtins/string.c) */
static RESULT korb_re_check_subject(CTX *c, korb_value *slots, korb_value subj)
{
    if (LIKELY(!KORB_STRING_P(subj) || KORB_STR_ENC(subj) != KORB_ENC_UTF8)) return RESULT_OK(KORB_NIL);
    /* a frozen subject cannot change: remember it was valid (graphql's lexer matches one
     * frozen 100 KB query at thousands of positions — the walk was 15% of its time) */
    const uint16_t fl = ((const AroObjectHeader *)(uintptr_t)subj)->flags;
    const bool frozen = (fl & KORB_FL_FROZEN) && !(fl & KORB_FL_CHILLED);
    korb_value *const memo = &c->vm->utf8_ok[((uintptr_t)subj >> 4) & 7u];
    if (frozen && *memo == subj) return RESULT_OK(KORB_NIL);
    if (LIKELY(korb_str_utf8_valid(VAL2STR(subj)))) { if (frozen) *memo = subj; return RESULT_OK(KORB_NIL); }
    return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid byte sequence in UTF-8");
}

/* ---- UTF-16/32 pattern over a subject in the same encoding.  astrogre reads
 * only bytes or UTF-8, so both sides are decoded to UTF-8 into malloc'd buffers
 * (callers borrow the subject's bytes: no koruby alloc here) and the match
 * offsets are mapped back to the subject's own bytes. */
typedef struct { char *u8; uint32_t *back; uint32_t len; } korb_re_wide_buf;

/* Own copies of string.c's encoders and korb_enc_fixed_unit behind a call:
 * more call sites of those changed LTO's inlining of String#upcase & co. */
static __attribute__((noinline)) uint32_t korb_re_enc_unit(const struct korb_vm *const vm, const uint32_t enc, bool *const be) {
    return korb_enc_fixed_unit(vm, enc, be);
}
static uint32_t korb_re_u8_put(char *const dst, const uint32_t cp) {
    if (cp < 0x80)    { dst[0] = (char)cp; return 1; }
    if (cp < 0x800)   { dst[0] = (char)(0xC0 | (cp >> 6)); dst[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { dst[0] = (char)(0xE0 | (cp >> 12)); dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        dst[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    dst[0] = (char)(0xF0 | (cp >> 18)); dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
/* Decode one character of valid UTF-8 (our own output); *cl gets its length. */
static uint32_t korb_re_u8_get(const unsigned char *const p, const uint32_t avail, uint32_t *const cl) {
    const unsigned char b0 = p[0];
    uint32_t cp, n;
    if (b0 < 0x80) { *cl = 1; return b0; }
    if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; n = 2; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; n = 3; }
    else { cp = b0 & 0x07; n = 4; }
    for (uint32_t k = 1; k < n && k < avail; k++) cp = (cp << 6) | (p[k] & 0x3F);
    *cl = n;
    return cp;
}
static inline uint32_t korb_re_wide_unit_at(const unsigned char *const p, const uint32_t unit, const bool be) {
    uint32_t v = 0;
    for (uint32_t k = 0; k < unit; k++) v = (v << 8) | p[be ? k : unit - 1 - k];
    return v;
}
/* false on a malformed sequence.  back[u8 off] = source byte offset of that
 * character (back[len] = n). */
static bool korb_re_wide_decode(const unsigned char *const restrict p, const uint32_t n, const uint32_t unit,
                                const bool be, const bool want_back,
                                korb_re_wide_buf *const restrict out)
{
    out->u8 = NULL; out->back = NULL; out->len = 0;
    if (n % unit) return false;
    const size_t cap = (size_t)n * 2 + 1;               /* UTF-16: <= 3 bytes per unit; UTF-32: <= 4 per 4 */
    char *const u8 = malloc(cap);
    uint32_t *const back = want_back ? malloc(cap * sizeof(uint32_t)) : NULL;
    if (u8 == NULL || (want_back && back == NULL)) { free(u8); free(back); return false; }
    uint32_t w = 0, i = 0;
    while (i < n) {
        uint32_t cp = korb_re_wide_unit_at(p + i, unit, be), cl = unit;
        if (unit == 2 && cp >= 0xD800 && cp <= 0xDFFF) {
            if (cp >= 0xDC00 || i + 4 > n) goto bad;
            const uint32_t lo = korb_re_wide_unit_at(p + i + 2, 2, be);
            if (lo < 0xDC00 || lo > 0xDFFF) goto bad;
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u); cl = 4;
        } else if (unit == 4 && (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) goto bad;
        const uint32_t k = korb_re_u8_put(u8 + w, cp);
        if (back) for (uint32_t j = 0; j < k; j++) back[w + j] = i;
        w += k; i += cl;
    }
    if (back) back[w] = n;
    out->u8 = u8; out->back = back; out->len = w;
    return true;
  bad:
    free(u8); free(back);
    return false;
}
/* An ASCII-compatible multi-byte subject (Shift_JIS, EUC-JP, ...): each character
 * becomes its codepoint, or a private-use one when it has none, so `.` and `\X`
 * step whole characters.  false on a malformed sequence (CRuby raises too). */
static bool korb_re_mb_decode(const struct korb_vm *const vm, const uint32_t enc, const unsigned char *const restrict p,
                              const uint32_t n, const bool want_back, korb_re_wide_buf *const restrict out)
{
    out->u8 = NULL; out->back = NULL; out->len = 0;
    const signed char (*const trans)[256] = korb_enc_trans_of(vm, enc);
    if (trans == NULL) return false;
    struct korb_tc tc;
    const bool have_tc = korb_tc_find(korb_enc_name_of(vm, enc), &tc) && tc.kind == KTC_MB;
    const size_t cap = (size_t)n * 4 + 1;
    char *const u8 = malloc(cap);
    uint32_t *const back = want_back ? malloc(cap * sizeof(uint32_t)) : NULL;
    if (u8 == NULL || (want_back && back == NULL)) { free(u8); free(back); return false; }
    uint32_t w = 0, i = 0;
    while (i < n) {
        const int32_t l = korb_enc_trans_len(trans, p, i, n);
        if (l <= 0) { free(u8); free(back); return false; }
        uint32_t cp;
        if (l == 1 && p[i] < 0x80) cp = p[i];
        else if (!have_tc || korb_tc_decode(&tc, p + i, (size_t)l, &cp) != (uint32_t)l) {
            uint32_t v = 0;
            for (int32_t k = 0; k < l; k++) v = (v << 8) | p[i + k];
            cp = (l <= 2 ? 0xF0000u : 0x100000u) + (v & 0xFFFFu);
        }
        const uint32_t k = korb_re_u8_put(u8 + w, cp);
        if (back) for (uint32_t j = 0; j < k; j++) back[w + j] = i;
        w += k; i += (uint32_t)l;
    }
    if (back) back[w] = n;
    out->u8 = u8; out->back = back; out->len = w;
    return true;
}
/* The code unit (2/4) when `re` is a UTF-16/32 pattern and `senc` is its own
 * encoding; 1 for an ASCII-compatible multi-byte `senc` with a pattern in it or
 * ASCII-only; else 0. */
static uint32_t korb_re_wide_unit(const struct korb_vm *const vm, const korb_value re, const uint32_t senc, bool *const be) {
    const korb_value src = VAL2RE(re)->source;
    if (!KORB_STRING_P(src)) return 0;
    if (KORB_STR_ENC(src) == senc) {
        const uint32_t u = korb_re_enc_unit(vm, senc, be);
        if (u) return u;
    } else if (!korb_str_bytes_ascii(src)) return 0;
    return senc >= KORB_ENC_OTHER_MIN && !KORB_ENC_SB(vm, senc) && korb_enc_ascii_compat_idx(vm, senc) &&
           korb_enc_trans_of(vm, senc) != NULL ? 1 : 0;
}
/* A scanner re-matches the same subject from advancing offsets; decoding it on
 * every call made a UTF-16 CSV parse O(n^2) in the decode.  One entry per VM,
 * validated by content (no mutation hook on strings), so the hit costs a memcmp. */
typedef struct { uint32_t enc, n; char *wide; korb_re_wide_buf u; } korb_re_wide_cache;

static const korb_re_wide_buf *korb_re_wide_subject(struct korb_vm *const vm, const char *const sb, const uint32_t n,
                                                    const uint32_t enc, const uint32_t unit, const bool be)
{
    korb_re_wide_cache *wc = vm->re_wide_cache;
    if (wc && wc->enc == enc && wc->n == n && memcmp(wc->wide, sb, n) == 0) return &wc->u;
    korb_re_wide_buf u;
    if (!(unit == 1 ? korb_re_mb_decode(vm, enc, (const unsigned char *)sb, n, true, &u)
                    : korb_re_wide_decode((const unsigned char *)sb, n, unit, be, true, &u))) return NULL;
    char *const wide = malloc(n ? n : 1);
    if (wide == NULL) { free(u.u8); free(u.back); return NULL; }
    memcpy(wide, sb, n);
    if (wc == NULL) {
        wc = calloc(1, sizeof *wc);
        if (wc == NULL) { free(wide); free(u.u8); free(u.back); return NULL; }
        vm->re_wide_cache = wc;
    }
    free(wc->wide); free(wc->u.u8); free(wc->u.back);
    wc->enc = enc; wc->n = n; wc->wide = wide; wc->u = u;
    return &wc->u;
}
/* Byte length of the character at i, for an empty match's one-character step:
 * UTF-8 as before, the UTF-16/32 families by code unit / surrogate pair. */
static inline uint32_t korb_re_char_len(const struct korb_vm *const vm, const uint32_t enc,
                                        const unsigned char *const p, const uint32_t i, const uint32_t n) {
    if (LIKELY(enc < KORB_ENC_OTHER_MIN)) { const uint32_t l = korb_utf8_seq_len(p, i, n); return l ? l : 1; }
    bool be = false;
    const uint32_t unit = korb_re_enc_unit(vm, enc, &be);
    if (unit == 0) {
        const signed char (*const trans)[256] = korb_enc_trans_of(vm, enc);
        if (trans && korb_enc_ascii_compat_idx(vm, enc)) { const int32_t l = korb_enc_trans_len(trans, p, i, n); return l > 0 ? (uint32_t)l : 1; }
        const uint32_t l = korb_utf8_seq_len(p, i, n); return l ? l : 1;   /* as before */
    }
    if (i + unit > n) return n - i;
    if (unit == 2) {
        const uint32_t hi = korb_re_wide_unit_at(p + i, 2, be);
        if (hi >= 0xD800 && hi <= 0xDBFF && i + 4 <= n) return 4;
    }
    return unit;
}
/* The engine's return codes, plus -3 = malformed subject, -4 = malformed pattern.  m may be NULL. */
static __attribute__((noinline, cold)) int
korb_re_exec_wide(struct korb_vm *const vm, const korb_re_exec_fn_t fn, const korb_value re,
                             const char *const sb, const uint32_t slen, const uint32_t senc,
                             const uint32_t unit, const bool be, const size_t startb, korb_re_match_t *const m)
{
    if (startb > slen) { if (m) m->matched = 0; return 0; }
    const KorbString *const pat = VAL2STR(VAL2RE(re)->source);
    korb_re_wide_buf pw;
    if (unit == 1 && korb_str_bytes_ascii((korb_value)(uintptr_t)pat)) {
        pw.u8 = malloc(pat->len ? pat->len : 1); pw.len = pat->len;
        if (pw.u8 == NULL) return -4;
        memcpy(pw.u8, korb_str_ptr(pat), pat->len);
    } else if (!(unit == 1 ? korb_re_mb_decode(vm, senc, (const unsigned char *)korb_str_ptr(pat), pat->len, false, &pw)
                           : korb_re_wide_decode((const unsigned char *)korb_str_ptr(pat), pat->len, unit, be, false, &pw))) return -4;
    const korb_re_wide_buf *const sw = korb_re_wide_subject(vm, sb, slen, senc, unit, be);
    if (sw == NULL) { free(pw.u8); return -3; }
    uint32_t lo = 0, hi = sw->len;                       /* first UTF-8 offset whose character starts at/after startb */
    while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (sw->back[mid] < startb) lo = mid + 1; else hi = mid; }
    const uint32_t *const back = sw->back;               /* the engine does not re-enter here: sw stays live */
    const int rc = fn(pw.u8, pw.len, VAL2RE(re)->flags & ~(KORB_RE_DERIVED | 128u), sw->u8, sw->len, lo, m);
    if (rc == 1 && m)
        for (int g = 0; g <= m->n_groups && g < KORB_RE_MAX_GROUPS; g++)
            if (m->starts[g] >= 0) { m->starts[g] = back[m->starts[g]]; m->ends[g] = back[m->ends[g]]; }
    free(pw.u8);
    return rc;
}
/* korb_re_run for a pattern whose source is in the subject's own encoding:
 * true/false when that is UTF-16/32, nil to fall through.  Out of line so
 * korb_re_run keeps no address-taken local (a stack canary on every match). */
static __attribute__((noinline, cold)) RESULT
korb_re_run_wide(CTX *c, korb_value *slots, korb_value re, korb_value subj, size_t startb, korb_re_match_t *m,
                 const korb_re_exec_fn_t fn)
{
    bool be = false;
    const uint32_t unit = korb_re_wide_unit(c->vm, re, KORB_STR_ENC(subj), &be);
    if (unit == 0) return RESULT_OK(KORB_NIL);
    const KorbString *const s = VAL2STR(subj);
    const int rc = korb_re_exec_wide(c->vm, fn, re, korb_str_ptr(s), s->len, KORB_STR_ENC(subj), unit, be, startb, m);
    if (UNLIKELY(rc < 0)) {
        const char *const en = korb_enc_name_of(c->vm, KORB_STR_ENC(subj));
        if (rc == -3) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid byte sequence in %s", en);
        if (rc == -4) return korb_raise(c, slots, KORB_E_REGEXP, 0, "invalid multibyte character in %s regexp", en);
        if (rc == -2) return korb_raise(c, slots, KORB_E_REGEXP, 0, "regexp match stack overflow");
        if (rc == -5) return korb_raise_nested(c, slots, "Regexp", "TimeoutError", "regexp match timeout");
        const char *const em = korb_re_error(c->vm);
        return korb_raise(c, slots, KORB_E_REGEXP, 0, "%s", em ? em : "invalid regular expression");
    }
    return RESULT_OK(rc == 1 ? KORB_TRUE : KORB_FALSE);
}

/* A pattern written in a single-byte encoding (BINARY, ISO-8859-x, Windows-125x)
 * with bytes past ASCII matches byte-wise whatever the subject: its \xAD is a
 * character of its own, not a broken UTF-8 lead (prawn's Windows-1252 line wrap). */
static inline bool korb_re_pat_bytewise_p(const struct korb_vm *const vm, const korb_value re) {
    const korb_value src = VAL2RE(re)->source;
    if (!KORB_STRING_P(src)) return false;
    const uint32_t e = KORB_STR_ENC(src);
    return (e == KORB_ENC_BINARY || (e >= KORB_ENC_OTHER_MIN && KORB_ENC_SB(vm, e))) && !korb_str_bytes_ascii(src);
}
/* The engine's match time limit: a Regexp.new(timeout:) one for its own matches,
 * Regexp.timeout otherwise. */
static __attribute__((cold, noinline)) void korb_re_timeout_set(const struct korb_vm *const vm, const korb_value secs) {
    if (vm->re_timeout_fn == NULL) return;
    const double d = FIXNUM_P(secs) ? (double)FIX2LONG(secs) : KORB_FLOAT_P(secs) ? korb_float_val(secs) : vm->re_timeout_sec;
    ((void (*)(double))vm->re_timeout_fn)(d);
}
static __attribute__((cold, noinline)) void korb_re_timeout_restore(const struct korb_vm *const vm) {
    if (vm->re_timeout_fn) ((void (*)(double))vm->re_timeout_fn)(vm->re_timeout_sec);
}
/* Regexp.__set_timeout(secs | nil) — the prelude's Regexp.timeout= */
static RESULT korb_m_re_set_timeout(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots; (void)self;
    const korb_value v = KORB_SLICE_GET(a, 0);
    (void)korb_re_load(c->vm);
    c->vm->re_timeout_sec = FIXNUM_P(v) ? (double)FIX2LONG(v) : KORB_FLOAT_P(v) ? korb_float_val(v) : 0.0;
    korb_re_timeout_restore(c->vm);
    return RESULT_OK(v);
}
/* korb_re_run's engine failure: -5 past the timeout, -2 the C-stack floor, else a bad pattern */
static __attribute__((cold, noinline)) RESULT korb_re_run_raise(CTX *c, korb_value *slots, const int rc) {
    if (rc == -5) return korb_raise_nested(c, slots, "Regexp", "TimeoutError", "regexp match timeout");
    if (rc == -2) return korb_raise(c, slots, KORB_E_REGEXP, 0, "regexp match stack overflow");
    const char *const m = korb_re_error(c->vm);
    return korb_raise(c, slots, KORB_E_REGEXP, 0, "%s", m ? m : "invalid regular expression");
}
/* korb_re_run for a Regexp.new(timeout:): the engine runs under its own limit */
static __attribute__((cold, noinline)) RESULT
korb_re_run_timed(CTX *c, korb_value *slots, const KorbRegexp *const rp, const KorbString *const pat,
                  const unsigned eff_flags, const KorbString *const s, const size_t startb, korb_re_match_t *m,
                  const korb_re_exec_fn_t fn)
{
    korb_re_timeout_set(c->vm, rp->timeout);
    const int rc = fn(korb_str_ptr(pat), pat->len, eff_flags, korb_str_ptr(s), s->len, startb, m);
    korb_re_timeout_restore(c->vm);
    if (UNLIKELY(rc < 0)) return korb_re_run_raise(c, slots, rc);
    return RESULT_OK(rc == 1 ? KORB_TRUE : KORB_FALSE);
}
/* ---- low-level engine call (no koruby alloc inside → subject bytes stable) */
static RESULT korb_re_run(CTX *c, korb_value *slots, korb_value re, korb_value subj, size_t startb, korb_re_match_t *m) {
    if (UNLIKELY(!KORB_REGEXP_P(re) || !KORB_STRING_P(subj))) return RESULT_OK(KORB_FALSE);
    const korb_re_exec_fn_t fn = korb_re_load(c->vm);
    if (UNLIKELY(fn == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Regexp engine (koruby_regex.so) unavailable");
    korb_re_sync_floor(c);   /* lazy first-load happens above; make sure the floor is set for THIS stack */
    /* Also gate on the SUBJECT: rule 2 (ASCII-incompatible subject) does not
     * depend on the pattern, so a plain ASCII regexp must reach the check too. */
    const bool sb_subj = KORB_ENC_SB(c->vm, KORB_STR_ENC(subj));
    if (sb_subj) korb_re_u8_flags(c->vm, re);            /* U8FIX decides the pin and the mode below */
    if (UNLIKELY(korb_re_enc_pinned(re) || KORB_STR_ENC(subj) >= KORB_ENC_OTHER_MIN)) {
        if (KORB_STR_ENC(subj) >= KORB_ENC_OTHER_MIN && KORB_STRING_P(VAL2RE(re)->source)) {
            const RESULT wr = korb_re_run_wide(c, slots, re, subj, startb, m, fn);
            if (wr.state != KORB_NORMAL || wr.value != KORB_NIL) return wr;
        }
        CHECK(korb_re_check_enc(c, slots, re, subj));
    }
    const KorbString *const pat = VAL2STR(VAL2RE(re)->source), *const s = VAL2STR(subj);
    if (startb > s->len) { m->matched = 0; return RESULT_OK(KORB_FALSE); }
    /* Encoding: a /n regex, or a single-byte subject (BINARY / US-ASCII), matches
     * byte-wise (astrogre PR_FLAGS_ASCII_8BIT=128); UTF-8 subjects match by
     * codepoint.  So `.` on a binary string consumes one byte, not a codepoint. */
    unsigned eff_flags = VAL2RE(re)->flags;
    if (sb_subj && !(eff_flags & (512u | KORB_RE_U8FIX))) eff_flags |= 128u;   /* a UTF-8-fixed pattern passed korb_re_check_enc: the subject is ASCII */
    if (UNLIKELY(KORB_STR_ENC(VAL2RE(re)->source) != KORB_ENC_UTF8) && korb_re_pat_bytewise_p(c->vm, re)) eff_flags |= 128u;
    eff_flags &= ~KORB_RE_DERIVED;
    /* The Regexp keeps the engine's handle for (source, eff_flags): the per-match re-hash / memcmp of the
     * source (railsbench: kre_get 0.6% of cycles) goes away; a stale handle (-3: the slot was reused) is re-taken. */
    int rc = -3;
    KorbRegexp *const rp = VAL2RE(re);
    if (LIKELY(c->vm->re_exec_h_fn != NULL)) {
        if (LIKELY(rp->rvalid && rp->rflags == eff_flags))
            rc = ((korb_re_exec_h_fn_t)c->vm->re_exec_h_fn)(rp->rslot, rp->rgen, korb_str_ptr(s), s->len, startb, m);
        if (rc == -3) {
            /* a Regexp.new(timeout:) never keeps a handle, so its matches all come here */
            if (UNLIKELY(rp->timeout != KORB_NIL))
                return korb_re_run_timed(c, slots, rp, pat, eff_flags, s, startb, m, fn);
            unsigned slot, gen;
            if (((korb_re_handle_fn_t)c->vm->re_handle_fn)(korb_str_ptr(pat), pat->len, eff_flags, &slot, &gen) == 0) {
                rp->rslot = slot; rp->rgen = gen; rp->rflags = eff_flags; rp->rvalid = 1;
                rc = ((korb_re_exec_h_fn_t)c->vm->re_exec_h_fn)(slot, gen, korb_str_ptr(s), s->len, startb, m);
            }
        }
    }
    if (rc == -3) rc = fn(korb_str_ptr(pat), pat->len, eff_flags, korb_str_ptr(s), s->len, startb, m);
    if (UNLIKELY(rc < 0)) return korb_re_run_raise(c, slots, rc);
    return RESULT_OK(rc == 1 ? KORB_TRUE : KORB_FALSE);
}

/* GC-safe byte-slice of the string rooted at *subjslot (re-reads after alloc).
 * b<0 → nil (unmatched group). */
static RESULT korb_re_slice(CTX *c, korb_value *slots, korb_value *subjslot, long b, long e) {
    if (b < 0) return RESULT_OK(KORB_NIL);
    return korb_str_slice_new(c, slots, KORB_REF_AT(subjslot), (uint32_t)b, (uint32_t)(e - b));
}

/* byte offset → character index within `s`.  For single-byte encodings
 * (US-ASCII / ASCII-8BIT) a byte IS a character, so return the offset as-is
 * (else begin/end and StringScanner-style byte callers desync on multibyte). */
static long korb_re_bchar(const struct korb_vm *vm, const KorbString *s, long boff) {
    if (boff <= 0) return 0;
    if (boff >= (long)s->len) boff = (long)s->len;
    return (long)korb_enc_charcount(vm, KORB_STR_ENC((korb_value)(uintptr_t)s), korb_str_ptr(s), (uint32_t)boff);
}

/* Build a MatchData from a completed match `m` over `subj` with source `re`. */
static RESULT korb_re_build_md(CTX *c, korb_value *slots, korb_value subj, korb_value re, const korb_re_match_t *m) {
    slots[0] = subj; slots[1] = re;
    const int ng = m->n_groups;
    slots[2] = UNWRAP(korb_ary_new(c, slots + 3, (uint32_t)(2 * (ng + 1))));
    korb_ref off = KORB_REF_AT(&slots[2]);
    for (int i = 0; i <= ng; i++) {
        CHECK(korb_ary_push_val(c, slots + 3, off, LONG2FIX(m->starts[i])));
        CHECK(korb_ary_push_val(c, slots + 3, off, LONG2FIX(m->ends[i])));
    }
    KorbMatchData *md = korb_alloc(c, slots + 3, sizeof(KorbMatchData), KORB_OBJ_MATCHDATA);
    ARO_STORE(c, md, (korb_value *)(uintptr_t)&md->subject, slots[0]);
    ARO_STORE(c, md, (korb_value *)(uintptr_t)&md->regexp,  slots[1]);
    ARO_STORE(c, md, (korb_value *)(uintptr_t)&md->offsets, slots[2]);
    return RESULT_OK((korb_value)md);
}

/* $~ : stored in the flat const table under "$~".  The table is append-only, so
 * the row is found once and then addressed directly — a match used to pay a
 * linear scan (get) plus the whole const_define path (set). */
static uint32_t korb_re_tilde_sym(struct korb_vm *vm) { return korb_intern_lit(vm, "$~", 2); }
korb_value *korb_re_tilde_cell(CTX *c) {
    struct korb_vm *const vm = c->vm;
    if (UNLIKELY(vm->tilde_idx1 == 0)) {
        const uint32_t sym = korb_re_tilde_sym(vm);
        uint32_t ix = korb_const_index(vm, sym);
        if (ix == UINT32_MAX) { korb_const_define(c, sym, KORB_NIL); ix = korb_const_index(vm, sym); }
        vm->tilde_idx1 = ix + 1;
    }
    return &vm->const_vals[vm->tilde_idx1 - 1];
}
static void korb_re_set_lastmatch(CTX *c, korb_value md_or_nil) { *korb_re_tilde_cell(c) = md_or_nil; }
static korb_value korb_re_get_lastmatch(CTX *c) { return *korb_re_tilde_cell(c); }
/* Give $~ its own frozen copy of the subject, for callers about to mutate it
 * in place (gsub!/sub!/slice!): CRuby's MatchData#string is a frozen snapshot. */
RESULT korb_re_lastmatch_detach(CTX *c, korb_value *slots) {
    slots[0] = korb_re_get_lastmatch(c);
    if (!KORB_MATCHDATA_P(slots[0]) || !KORB_STRING_P(VAL2MD(slots[0])->subject)) return RESULT_OK(KORB_NIL);
    slots[1] = VAL2MD(slots[0])->subject;
    const uint32_t n = VAL2STR(slots[1])->len;
    slots[2] = UNWRAP(korb_str_slice_new(c, slots + 2, KORB_REF_AT(&slots[1]), 0, n));
    ((AroObjectHeader *)(uintptr_t)slots[2])->flags |= KORB_FL_FROZEN;
    ARO_STORE(c, VAL2MD(slots[0]), (korb_value *)(uintptr_t)&VAL2MD(slots[0])->subject, slots[2]);
    return RESULT_OK(KORB_NIL);
}

/* Regexp#=== that also sets $~ (like CRuby's `when /re/` / grep).  Returns the
 * bool result; on a match, $~ becomes the MatchData, else nil.  Only called for
 * Regexp patterns (the caller checks KORB_REGEXP_P first). */
bool korb_re_caseeq_backref(CTX *c, korb_value *slots, korb_value pat, korb_value val) {
    /* MatchData#[]/#pre_match slice their subject as a String, so the subject
     * stored in $~ must be a String — coerce a Symbol to its name here (a fresh
     * String), never store the Symbol itself. */
    slots[1] = pat;                                        /* root FIRST: the Symbol coercion below allocates */
    if (SYMBOL_P(val)) {
        const char *const nm = korb_sym_name(c->vm, SYM2ID(val));
        const RESULT sr = korb_str_new(c, slots + 2, nm, (uint32_t)strlen(nm));
        if (UNLIKELY(sr.state != KORB_NORMAL)) return false;
        val = sr.value;
    } else if (!KORB_STRING_P(val)) { korb_re_set_lastmatch(c, KORB_NIL); return false; }
    slots[0] = val;                                        /* subject (may have just been allocated) */
    const korb_re_exec_fn_t fn = korb_re_load(c->vm);
    if (UNLIKELY(fn == NULL)) return false;
    korb_re_sync_floor(c);
    korb_re_match_t m;
    bool wbe = false;
    const uint32_t wunit = UNLIKELY(KORB_STR_ENC(slots[0]) >= KORB_ENC_OTHER_MIN)
                         ? korb_re_wide_unit(c->vm, slots[1], KORB_STR_ENC(slots[0]), &wbe) : 0;
    if (UNLIKELY(wunit)) {
        const KorbString *const s = VAL2STR(slots[0]);
        if (korb_re_exec_wide(c->vm, fn, slots[1], korb_str_ptr(s), s->len, KORB_STR_ENC(slots[0]), wunit, wbe, 0, &m) != 1) {
            korb_re_set_lastmatch(c, KORB_NIL); return false;
        }
    } else
    { const KorbString *const s = VAL2STR(slots[0]);       /* re-derive: str_new above may have GC'd */
      const KorbString *const p = VAL2STR(VAL2RE(slots[1])->source);
      unsigned eff = VAL2RE(slots[1])->flags;             /* see korb_re_run: a single-byte subject matches byte-wise */
      if (KORB_ENC_SB(c->vm, KORB_STR_ENC(slots[0])) &&
          (!(korb_re_u8_flags(c->vm, slots[1]) & (512u | KORB_RE_U8FIX)) || !korb_str_bytes_ascii(slots[0]))) eff |= 128u;
      if (UNLIKELY(KORB_STR_ENC(VAL2RE(slots[1])->source) != KORB_ENC_UTF8) && korb_re_pat_bytewise_p(c->vm, slots[1])) eff |= 128u;
      eff &= ~KORB_RE_DERIVED;
      const int r = fn(korb_str_ptr(p), p->len, eff,
                       korb_str_ptr(s), s->len, 0, &m);   /* fn does not allocate → borrows stable */
      if (r != 1) { korb_re_set_lastmatch(c, KORB_NIL); return false; } }
    const RESULT mdr = korb_re_build_md(c, slots + 2, slots[0], slots[1], &m);
    if (UNLIKELY(mdr.state != KORB_NORMAL)) return false;
    korb_re_set_lastmatch(c, mdr.value);
    return true;
}

/* ---- MatchData helpers --------------------------------------------------- */
static long korb_md_off(const KorbMatchData *md, int i, int which) {
    const KorbArray *a = VAL2ARY(md->offsets);
    const uint32_t idx = (uint32_t)(2 * i + which);
    if (idx >= a->len) return -1;
    return FIX2LONG(korb_ary_ptr(a)[idx]);
}
static int korb_md_ngroups(const KorbMatchData *md) { return (int)(VAL2ARY(md->offsets)->len / 2); }
static RESULT korb_md_group(CTX *c, korb_value *slots, korb_value mdv, int i) {
    KorbMatchData *md = VAL2MD(mdv);
    if (i < 0 || i >= korb_md_ngroups(md)) return RESULT_OK(KORB_NIL);
    const long b = korb_md_off(md, i, 0), e = korb_md_off(md, i, 1);
    if (b < 0 || e < 0) return RESULT_OK(KORB_NIL);
    /* Slice via korb_str_slice_new so the capture inherits the SUBJECT's
     * encoding (like pre_match/post_match) — a match over an ASCII-8BIT string
     * must yield ASCII-8BIT captures, else String#length counts UTF-8 chars and
     * byte-based callers (e.g. StringScanner) desync.  Also GC-safe (re-reads). */
    slots[0] = md->subject;
    return korb_str_slice_new(c, slots + 1, KORB_REF_AT(&slots[0]), (uint32_t)b, (uint32_t)(e - b));
}
/* resolve a named group in md's regexp → group number, or -1. */
static int korb_md_name_idx(CTX *c, korb_value mdv, const char *name, uint32_t nlen) {
    KorbMatchData *md = VAL2MD(mdv);
    if (!KORB_REGEXP_P(md->regexp)) return -1;
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (nf == NULL) return -1;
    const KorbString *pat = VAL2STR(VAL2RE(md->regexp)->source);
    const uint32_t flags = VAL2RE(md->regexp)->flags;
    int best = -1, last_defined = -1;   /* duplicate names → the last group that participated (else last defined → nil) */
    const korb_re_name_groups_fn_t gf = (korb_re_name_groups_fn_t)c->vm->re_name_groups_fn;
    int gis[32];
    const int gn = gf != NULL ? gf(korb_str_ptr(pat), pat->len, flags, name, nlen, gis, 32) : -2;
    if (gn >= 0 && gn < 32) {
        for (int j = 0; j < gn; j++) { last_defined = gis[j]; if (korb_md_off(md, gis[j], 0) >= 0) best = gis[j]; }
        return best >= 0 ? best : last_defined;
    }
    for (int k = 0; ; k++) {
        int gi = -1;
        const char *gn = nf(korb_str_ptr(pat), pat->len, flags, k, &gi);
        if (gn == NULL) break;
        if (strlen(gn) == nlen && memcmp(gn, name, nlen) == 0) {
            last_defined = gi;
            if (korb_md_off(md, gi, 0) >= 0) best = gi;
        }
    }
    return best >= 0 ? best : last_defined;
}
/* Collect groups [lo, lo+len) into a fresh Array, Array#[start,length]-style:
 * clamp the upper bound to the group count (no trailing nils beyond the end). */
static RESULT korb_md_group_slice(CTX *c, korb_value *slots, korb_value mdv, long lo, long len) {
    slots[0] = mdv;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)(len > 0 ? len : 0)));
    const int n = korb_md_ngroups(VAL2MD(slots[0]));
    long hi = lo + len; if (hi > n) hi = n;
    for (long gi = lo; gi < hi; gi++) {
        slots[2] = UNWRAP(korb_md_group(c, slots + 2, slots[0], (int)gi));   /* participating→str, else nil */
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_aref(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const korb_value mdv = KORB_REF_GET(self);
    const korb_value k = KORB_SLICE_GET(a, 0);
    if (SYMBOL_P(k) || KORB_STRING_P(k)) {                /* [name] → named group */
        const char *nm; uint32_t nl;
        if (SYMBOL_P(k)) { nm = korb_sym_name(c->vm, SYM2ID(k)); nl = (uint32_t)strlen(nm); }
        else { nm = korb_str_ptr(VAL2STR(k)); nl = VAL2STR(k)->len; }
        int gi = korb_md_name_idx(c, mdv, nm, nl);
        if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        return korb_md_group(c, slots, mdv, gi);
    }
    const int n = korb_md_ngroups(VAL2MD(mdv));
    if (KORB_SLICE_LEN(a) >= 2) {                        /* [start, length] → Array */
        korb_sword_t st = 0, ln = 0;
        if (!korb_to_index(k, &st) || !korb_to_index(KORB_SLICE_GET(a, 1), &ln)) return korb_raise_no_int(c, slots, k);
        if (st < 0) st += n;
        if (st < 0 || st > n || ln < 0) return RESULT_OK(KORB_NIL);
        return korb_md_group_slice(c, slots, mdv, st, ln);
    }
    if (KORB_RANGE_P(k)) {                                /* [range] → Array */
        const KorbRange *r = VAL2RANGE(k);
        korb_sword_t b = 0, e;
        if (r->rbegin != KORB_NIL) { if (!korb_to_index(r->rbegin, &b)) return korb_raise_no_int(c, slots, r->rbegin); }
        if (b < 0) b += n;
        if (r->rend == KORB_NIL) e = n; else { if (!korb_to_index(r->rend, &e)) return korb_raise_no_int(c, slots, r->rend); if (e < 0) e += n; if (!r->exclude_end) e += 1; }
        if (b < 0 || b > n) return RESULT_OK(KORB_NIL);
        long len = e - b; if (len < 0) len = 0;
        return korb_md_group_slice(c, slots, mdv, b, len);
    }
    korb_sword_t i = 0;                                       /* [int] → single group */
    if (!korb_to_index(k, &i)) return RESULT_OK(KORB_NIL);
    if (i < 0) i += n;
    if (i < 0 || i >= n) return RESULT_OK(KORB_NIL);
    return korb_md_group(c, slots, mdv, (int)i);
}
static RESULT korb_m_md_to_a(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    slots[0] = KORB_REF_GET(self);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    const int n = korb_md_ngroups(VAL2MD(slots[0]));
    for (int i = 0; i < n; i++) {
        slots[2] = UNWRAP(korb_md_group(c, slots + 2, slots[0], i));
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_captures(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    slots[0] = KORB_REF_GET(self);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    const int n = korb_md_ngroups(VAL2MD(slots[0]));
    for (int i = 1; i < n; i++) {
        slots[2] = UNWRAP(korb_md_group(c, slots + 2, slots[0], i));
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_values_at(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    slots[0] = KORB_REF_GET(self);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, KORB_SLICE_LEN(a)));
    const int n = korb_md_ngroups(VAL2MD(slots[0]));
    for (uint32_t j = 0; j < KORB_SLICE_LEN(a); j++) {
        const korb_value arg = KORB_SLICE_GET(a, j);
        if (KORB_RANGE_P(arg)) {                          /* a Range arg expands to its indices */
            const KorbRange *r = VAL2RANGE(arg);
            korb_sword_t b = 0, e;
            if (r->rbegin != KORB_NIL) korb_to_index(r->rbegin, &b);
            const korb_sword_t raw_b = b;
            if (b < 0) b += n;
            if (r->rend == KORB_NIL) e = n; else { e = 0; korb_to_index(r->rend, &e); if (e < 0) e += n; if (!r->exclude_end) e += 1; }
            /* The message names the range as written, not as resolved. */
            if (UNLIKELY(b < 0 || b > n)) {
                char rs[64];
                if (r->rend == KORB_NIL) {
                    snprintf(rs, sizeof rs, "%ld%s", (long)raw_b, r->exclude_end ? "..." : "..");
                } else {
                    korb_sword_t raw_e = 0; korb_to_index(r->rend, &raw_e);
                    snprintf(rs, sizeof rs, "%ld%s%ld", (long)raw_b,
                             r->exclude_end ? "..." : "..", (long)raw_e);
                }
                return korb_raise(c, slots, KORB_E_RANGE, 0, "%s out of range", rs);
            }
            for (long gi = b; gi < e; gi++) {
                slots[2] = (gi >= 0 && gi < n) ? UNWRAP(korb_md_group(c, slots + 2, slots[0], (int)gi)) : KORB_NIL;
                CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[1]), slots[2]));
            }
            continue;
        }
        korb_sword_t i = 0; korb_value g = KORB_NIL;
        if (SYMBOL_P(arg) || KORB_STRING_P(arg)) {        /* named-capture reference */
            const char *nm; uint32_t nl;
            if (SYMBOL_P(arg)) { nm = korb_sym_name(c->vm, SYM2ID(arg)); nl = (uint32_t)strlen(nm); }
            else { nm = korb_str_ptr(VAL2STR(arg)); nl = VAL2STR(arg)->len; }
            const int gi = korb_md_name_idx(c, slots[0], nm, nl);
            if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
            g = UNWRAP(korb_md_group(c, slots + 2, slots[0], gi));
        } else if (korb_to_index(arg, &i)) {              /* Integer index (out-of-range → nil) */
            if (i < 0) i += n;
            if (i >= 0 && i < n) g = UNWRAP(korb_md_group(c, slots + 2, slots[0], (int)i));
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_re_arg_type(arg));
        }
        slots[2] = g;
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_to_s(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)a; return korb_md_group(c, slots, KORB_REF_GET(self), 0); }
static RESULT korb_m_md_pre(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const long b0 = korb_md_off(VAL2MD(KORB_REF_GET(self)), 0, 0);
    slots[0] = VAL2MD(KORB_REF_GET(self))->subject;     /* slice the SUBJECT, not the MatchData */
    return korb_re_slice(c, slots + 1, &slots[0], 0, b0);
}
static RESULT korb_m_md_post(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const KorbMatchData *md = VAL2MD(KORB_REF_GET(self));
    const long e0 = korb_md_off(md, 0, 1), slen = (long)VAL2STR(md->subject)->len;
    slots[0] = md->subject;
    return korb_re_slice(c, slots + 1, &slots[0], e0, slen);
}
/* Resolve a begin/end/offset arg (Integer, or Symbol/String name) to a group
 * index; raises IndexError on an unknown name or out-of-range index. */
static RESULT korb_md_arg_gi(CTX *c, korb_value *slots, korb_value mdv, korb_value arg, int *gi_out) {
    if (SYMBOL_P(arg) || KORB_STRING_P(arg)) {
        const char *nm; uint32_t nl;
        if (SYMBOL_P(arg)) { nm = korb_sym_name(c->vm, SYM2ID(arg)); nl = (uint32_t)strlen(nm); }
        else { nm = korb_str_ptr(VAL2STR(arg)); nl = VAL2STR(arg)->len; }
        int gi = korb_md_name_idx(c, mdv, nm, nl);
        if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        *gi_out = gi; return RESULT_OK(KORB_TRUE);
    }
    korb_sword_t i = 0;
    if (!korb_to_index(arg, &i)) {                        /* coerce via #to_int */
        slots[0] = mdv;                                   /* park: the probe below runs Ruby */
        korb_value av = arg; RESULT cr = korb_coerce_to_int(c, slots + 1, &av);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        mdv = slots[0];                                   /* re-read the (moved) MatchData */
        if (!korb_to_index(av, &i)) return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_re_arg_type(av));
    }
    const int n = korb_md_ngroups(VAL2MD(mdv));           /* begin/end/offset take no negative index */
    if (i < 0 || i >= n) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %d out of matches", (int)i);
    *gi_out = (int)i; return RESULT_OK(KORB_TRUE);
}
/* which: 0=begin 1=end; bytes=false → char offset, true → byte offset. */
static RESULT korb_md_pos(CTX *c, korb_value *slots, korb_ref self, korb_value arg, int which, bool bytes) {
    int gi = 0; RESULT r = korb_md_arg_gi(c, slots, KORB_REF_GET(self), arg, &gi);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    KorbMatchData *md = VAL2MD(KORB_REF_GET(self));
    const long o = korb_md_off(md, gi, which);
    if (o < 0) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX(bytes ? o : korb_re_bchar(c->vm, VAL2STR(md->subject), o)));
}
static RESULT korb_m_md_begin(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_md_pos(c, slots, self, KORB_SLICE_GET(a, 0), 0, false); }
static RESULT korb_m_md_end(CTX *c, korb_value *slots, korb_ref self, korb_slice a)   { return korb_md_pos(c, slots, self, KORB_SLICE_GET(a, 0), 1, false); }
static RESULT korb_m_md_bytebegin(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_md_pos(c, slots, self, KORB_SLICE_GET(a, 0), 0, true); }
static RESULT korb_m_md_byteend(CTX *c, korb_value *slots, korb_ref self, korb_slice a)   { return korb_md_pos(c, slots, self, KORB_SLICE_GET(a, 0), 1, true); }
/* offset(n)/byteoffset(n) → [begin, end] (char- resp. byte-based), accepting an
 * Integer index or a Symbol/String named-capture reference. */
static RESULT korb_md_offset_impl(CTX *c, korb_value *slots, korb_ref self, korb_value arg, bool bytes) {
    int gi = 0; RESULT r = korb_md_arg_gi(c, slots, KORB_REF_GET(self), arg, &gi);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    KorbMatchData *md = VAL2MD(KORB_REF_GET(self));
    const long b = korb_md_off(md, gi, 0), e = korb_md_off(md, gi, 1);
    slots[0] = KORB_REF_GET(self); slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
    if (b < 0) { CHECK(korb_ary_push_val(c, slots + 2, KORB_REF_AT(&slots[1]), KORB_NIL)); CHECK(korb_ary_push_val(c, slots + 2, KORB_REF_AT(&slots[1]), KORB_NIL)); }
    else { const KorbString *s = VAL2STR(VAL2MD(slots[0])->subject);
           CHECK(korb_ary_push_val(c, slots + 2, KORB_REF_AT(&slots[1]), LONG2FIX(bytes ? b : korb_re_bchar(c->vm, s, b))));
           CHECK(korb_ary_push_val(c, slots + 2, KORB_REF_AT(&slots[1]), LONG2FIX(bytes ? e : korb_re_bchar(c->vm, s, e)))); }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_offset(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_md_offset_impl(c, slots, self, KORB_SLICE_GET(a, 0), false); }
static RESULT korb_m_md_byteoffset(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_md_offset_impl(c, slots, self, KORB_SLICE_GET(a, 0), true); }
/* match(n) → the nth match substring (or nil); match_length(n) → its length. */
static RESULT korb_m_md_match(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    int gi = 0; RESULT r = korb_md_arg_gi(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), &gi);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return korb_md_group(c, slots, KORB_REF_GET(self), gi);
}
static RESULT korb_m_md_match_length(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    int gi = 0; RESULT r = korb_md_arg_gi(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), &gi);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    slots[0] = UNWRAP(korb_md_group(c, slots, KORB_REF_GET(self), gi));
    if (slots[0] == KORB_NIL) return RESULT_OK(KORB_NIL);
    const KorbString *g = VAL2STR(slots[0]);
    return RESULT_OK(LONG2FIX((long)korb_enc_charcount(c->vm, KORB_STR_ENC(slots[0]), korb_str_ptr(g), g->len)));
}
/* Ruby 4.1: `md.integer_at(n)` is `md[n]&.to_i`, except that a capture with no
 * digits to parse answers nil rather than 0 (rb_int_parse_cstr semantics). */
static bool korb_md_int_parsable(const char *const restrict d, const uint32_t len, int base) {
    uint32_t i = 0;
    while (i < len && korb_is_ws((unsigned char)d[i])) i++;
    if (i < len && (d[i] == '+' || d[i] == '-')) i++;
    if (i + 1 < len && d[i] == '0') {                  /* the base-prefix rule String#to_i uses */
        const char p = d[i + 1] | 0x20;
        const int pb = p == 'x' ? 16 : p == 'b' ? 2 : p == 'o' ? 8 : p == 'd' ? 10 : 0;
        if (pb && (base == 0 || base == pb)) { base = pb; i += 2; }
        else if (base == 0) base = 8;
    }
    if (base == 0) base = 10;
    if (i >= len) return false;
    const char ch = d[i];
    const int dig = (ch >= '0' && ch <= '9') ? ch - '0'
                  : ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') ? (ch | 0x20) - 'a' + 10 : 99;
    return dig < base;
}
static RESULT korb_m_md_integer_at(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const uint32_t argc = KORB_SLICE_LEN(a);
    if (UNLIKELY(argc < 1 || argc > 2))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 1..2)", argc);
    const int n = korb_md_ngroups(VAL2MD(KORB_REF_GET(self)));
    const korb_value k = KORB_SLICE_GET(a, 0);
    korb_sword_t nth = 0;
    if (FIXNUM_P(k)) {                                 /* CRuby: only a Fixnum is an index; anything else is a group NAME */
        nth = FIX2LONG(k);                             /* out of range is nil, never an error */
        if (nth >= n) return RESULT_OK(KORB_NIL);
        if (nth < 0 && (nth += n) <= 0) return RESULT_OK(KORB_NIL);
    }
    else if (SYMBOL_P(k) || KORB_STRING_P(k)) {
        const char *nm; uint32_t nl;
        if (SYMBOL_P(k)) { nm = korb_sym_name(c->vm, SYM2ID(k)); nl = (uint32_t)strlen(nm); }
        else { nm = korb_str_ptr(VAL2STR(k)); nl = VAL2STR(k)->len; }
        const int gi = korb_md_name_idx(c, KORB_REF_GET(self), nm, nl);
        if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        nth = gi;
    }
    else {                                             /* CRuby spells the value itself here; its class is close enough */
        return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %s", korb_coerce_name(c, k));
    }
    korb_sword_t base = 10;
    if (argc == 2) {
        korb_value bv = KORB_SLICE_GET(a, 1);
        if (UNLIKELY(!korb_to_index(bv, &base))) {
            RESULT cr = korb_coerce_to_int(c, slots, &bv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (UNLIKELY(cr.value == KORB_FALSE || !korb_to_index(bv, &base)))
                return korb_raise_no_int(c, slots, KORB_SLICE_GET(a, 1));
        }
        if (UNLIKELY(base > INT32_MAX || base < INT32_MIN))
            return korb_raise(c, slots, KORB_E_RANGE, 0, "integer %ld too big to convert to 'int'", (long)base);
        if (UNLIKELY(base < 0 || (base != 0 && (base < 2 || base > 36))))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid radix %d", (int)base);
    }
    slots[0] = UNWRAP(korb_md_group(c, slots, KORB_REF_GET(self), (int)nth));
    if (slots[0] == KORB_NIL) return RESULT_OK(KORB_NIL);
    {
        const KorbString *const s = VAL2STR(slots[0]);
        if (!korb_md_int_parsable(korb_str_ptr(s), s->len, (int)base)) return RESULT_OK(KORB_NIL);
    }
    slots[1] = LONG2FIX(base);
    return korb_m_str_to_i(c, slots + 2, KORB_REF_AT(&slots[0]), KORB_SLICE_MAKE(&slots[1], 1));
}
static RESULT korb_m_md_size(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(korb_md_ngroups(VAL2MD(KORB_REF_GET(self))))); }
static RESULT korb_m_md_string(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a; slots[0] = KORB_REF_GET(self);
    slots[1] = VAL2MD(slots[0])->subject;
    if ((((const AroObjectHeader *)(uintptr_t)slots[1])->flags & (KORB_FL_FROZEN | KORB_FL_CHILLED)) == KORB_FL_FROZEN)
        return RESULT_OK(slots[1]);                       /* already a snapshot (a chilled literal is not frozen) */
    const uint32_t n = VAL2STR(slots[1])->len;
    slots[2] = UNWRAP(korb_str_slice_new(c, slots + 2, KORB_REF_AT(&slots[1]), 0, n));   /* CRuby: a frozen copy */
    ((AroObjectHeader *)(uintptr_t)slots[2])->flags |= KORB_FL_FROZEN;
    slots[0] = KORB_REF_GET(self);                            /* re-read: the copy allocated */
    ARO_STORE(c, VAL2MD(slots[0]), &VAL2MD(slots[0])->subject, slots[2]);   /* the same String on every call */
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_md_regexp(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots;(void)a; return RESULT_OK(VAL2MD(KORB_REF_GET(self))->regexp); }
static RESULT korb_md_names_into(CTX *c, korb_value *slots, korb_value mdv_or_re, bool is_md, korb_ref dst_ary) {
    if (c->vm->re_named_fn == NULL) korb_re_load(c->vm);   /* ensure the engine is loaded (names before any match) */
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    korb_value rev = is_md ? VAL2MD(mdv_or_re)->regexp : mdv_or_re;
    if (!nf || !KORB_REGEXP_P(rev)) return RESULT_OK(KORB_REF_GET(dst_ary));
    slots[0] = rev;
    for (int k = 0; ; k++) {
        const KorbString *pat = VAL2STR(VAL2RE(slots[0])->source);   /* re-fetch (korb_str_new below moves GC) */
        const uint32_t flags = VAL2RE(slots[0])->flags;
        int gi = -1; const char *gn = nf(korb_str_ptr(pat), pat->len, flags, k, &gi);
        if (!gn) break;
        const uint32_t gnl = (uint32_t)strlen(gn);
        bool dup = false;                                /* each name only once */
        const KorbArray *da = VAL2ARY(KORB_REF_GET(dst_ary));
        for (uint32_t j = 0; j < da->len; j++) { const KorbString *e = VAL2STR(korb_ary_ptr(da)[j]); if (e->len == gnl && memcmp(korb_str_ptr(e), gn, gnl) == 0) { dup = true; break; } }
        if (dup) continue;
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, gn, gnl));
        CHECK(korb_ary_push_val(c, slots + 2, dst_ary, slots[1]));
    }
    return RESULT_OK(KORB_REF_GET(dst_ary));
}
static RESULT korb_m_md_names(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a; slots[0] = KORB_REF_GET(self); slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    return korb_md_names_into(c, slots + 2, slots[0], true, KORB_REF_AT(&slots[1]));
}
/* Read a trailing `name: true/false` kwarg Hash (Symbol-keyed); default when absent. */
static bool korb_kw_bool(CTX *c, korb_slice a, const char *name, bool dflt) {
    const uint32_t n = KORB_SLICE_LEN(a);
    if (n == 0 || !KORB_HASH_P(KORB_SLICE_GET(a, n - 1))) return dflt;
    const korb_value key = ID2SYM(korb_intern(c->vm, name, (uint32_t)strlen(name)));
    int32_t idx = korb_hash_find(VAL2HASH(KORB_SLICE_GET(a, n - 1)), key);
    if (idx < 0) return dflt;
    return KORB_TRUTHY(korb_hash_pairs(VAL2HASH(KORB_SLICE_GET(a, n - 1)))[2 * idx + 1]);
}
static RESULT korb_m_md_named_captures(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const bool symkeys = korb_kw_bool(c, a, "symbolize_names", false);
    slots[0] = KORB_REF_GET(self); slots[1] = UNWRAP(korb_hash_new(c, slots + 1, 0));
    KorbMatchData *md = VAL2MD(slots[0]);
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (nf && KORB_REGEXP_P(md->regexp)) {
        for (int k = 0; ; k++) {
            /* re-fetch pattern bytes each iteration: korb_md_group below allocates
             * and the moving GC can relocate the Regexp's source string. */
            const KorbString *pat = VAL2STR(VAL2RE(VAL2MD(slots[0])->regexp)->source);
            const uint32_t flags = VAL2RE(VAL2MD(slots[0])->regexp)->flags;
            int gi = -1; const char *gn = nf(korb_str_ptr(pat), pat->len, flags, k, &gi);
            if (!gn) break;
            /* duplicate names → the value of the last group that participated */
            const int best = korb_md_name_idx(c, slots[0], gn, (uint32_t)strlen(gn));
            if (symkeys) slots[2] = ID2SYM(korb_intern(c->vm, gn, (uint32_t)strlen(gn)));
            else slots[2] = UNWRAP(korb_str_new(c, slots + 2, gn, (uint32_t)strlen(gn)));
            slots[3] = UNWRAP(korb_md_group(c, slots + 3, slots[0], best));
            CHECK(korb_hash_set(c, slots + 4, KORB_REF_AT(&slots[1]), KORB_REF_AT(&slots[2]), slots[3]));
        }
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_eq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots; const korb_value o = KORB_SLICE_GET(a, 0), s = KORB_REF_GET(self);
    if (!KORB_MATCHDATA_P(o)) return RESULT_OK(KORB_FALSE);
    const KorbMatchData *m1 = VAL2MD(s), *m2 = VAL2MD(o);
    const KorbString *s1 = VAL2STR(m1->subject), *s2 = VAL2STR(m2->subject);
    if (s1->len != s2->len || memcmp(korb_str_ptr(s1), korb_str_ptr(s2), s1->len) != 0) return RESULT_OK(KORB_FALSE);
    if (KORB_REGEXP_P(m1->regexp) && KORB_REGEXP_P(m2->regexp)) {
        if (((VAL2RE(m1->regexp)->flags ^ VAL2RE(m2->regexp)->flags) & ~KORB_RE_DERIVED) != 0) return RESULT_OK(KORB_FALSE);
        const KorbString *p1 = VAL2STR(VAL2RE(m1->regexp)->source), *p2 = VAL2STR(VAL2RE(m2->regexp)->source);
        if (p1->len != p2->len || memcmp(korb_str_ptr(p1), korb_str_ptr(p2), p1->len) != 0) return RESULT_OK(KORB_FALSE);
    } else if (m1->regexp != m2->regexp) return RESULT_OK(KORB_FALSE);
    const KorbArray *o1 = VAL2ARY(m1->offsets), *o2 = VAL2ARY(m2->offsets);
    if (o1->len != o2->len) return RESULT_OK(KORB_FALSE);
    for (uint32_t i = 0; i < o1->len; i++) if (korb_ary_ptr(o1)[i] != korb_ary_ptr(o2)[i]) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_md_hash(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c; (void)slots; (void)a;
    const KorbMatchData *md = VAL2MD(KORB_REF_GET(self));
    const KorbString *s = VAL2STR(md->subject);
    uint64_t h = 1469598103934665603ULL;                 /* FNV-1a over subject + offsets */
    for (uint32_t i = 0; i < s->len; i++) { h ^= (unsigned char)korb_str_ptr(s)[i]; h *= 1099511628211ULL; }
    const KorbArray *o = VAL2ARY(md->offsets);
    for (uint32_t i = 0; i < o->len; i++) { h ^= (uint64_t)FIX2LONG(korb_ary_ptr(o)[i]); h *= 1099511628211ULL; }
    return RESULT_OK(LONG2FIX((korb_sword_t)(h & (((korb_word_t)1 << 62) - 1))));
}
/* deconstruct_keys(keys) → named captures as a Symbol-keyed Hash (keys: nil = all,
 * or an Array selecting a subset; returns {} early if a requested key is absent). */
static RESULT korb_m_md_deconstruct_keys(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const korb_value keys = KORB_SLICE_GET(a, 0);
    if (keys != KORB_NIL && !KORB_ARRAY_P(keys)) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Array)", korb_re_arg_type(keys));
    slots[0] = KORB_REF_GET(self); slots[1] = UNWRAP(korb_hash_new(c, slots + 1, 0));
    KorbMatchData *md = VAL2MD(slots[0]);
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (!nf || !KORB_REGEXP_P(md->regexp)) return RESULT_OK(slots[1]);
    if (keys != KORB_NIL) {                               /* subset by the given Symbol keys */
        const uint32_t klen = VAL2ARY(KORB_SLICE_GET(a, 0))->len;   /* re-fetch: korb_hash_new above moved GC */
        uint32_t ng = 0;                                  /* count named groups: more keys than groups → {} */
        for (int k = 0; ; k++) {
            const KorbString *pat = VAL2STR(VAL2RE(VAL2MD(slots[0])->regexp)->source);
            const uint32_t flags = VAL2RE(VAL2MD(slots[0])->regexp)->flags;
            int gi = -1; if (!nf(korb_str_ptr(pat), pat->len, flags, k, &gi)) break;
            ng++;
        }
        if (klen > ng) return RESULT_OK(slots[1]);
        for (uint32_t i = 0; i < klen; i++) {
            const korb_value kv = korb_ary_ptr(VAL2ARY(KORB_SLICE_GET(a, 0)))[i];   /* re-fetch (moving GC) */
            if (!SYMBOL_P(kv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Symbol)", korb_re_arg_type(kv));
            const char *nm = korb_sym_name(c->vm, SYM2ID(kv)); const uint32_t nl = (uint32_t)strlen(nm);
            const int gi = korb_md_name_idx(c, slots[0], nm, nl);
            if (gi < 0) break;                            /* unknown key → stop (partial match) */
            slots[2] = kv;
            slots[3] = UNWRAP(korb_md_group(c, slots + 3, slots[0], gi));
            CHECK(korb_hash_set(c, slots + 4, KORB_REF_AT(&slots[1]), KORB_REF_AT(&slots[2]), slots[3]));
        }
        return RESULT_OK(slots[1]);
    }
    for (int k = 0; ; k++) {
        const KorbString *pat = VAL2STR(VAL2RE(VAL2MD(slots[0])->regexp)->source);   /* re-fetch (GC-moving) */
        const uint32_t flags = VAL2RE(VAL2MD(slots[0])->regexp)->flags;
        int gi = -1; const char *gn = nf(korb_str_ptr(pat), pat->len, flags, k, &gi);
        if (!gn) break;
        const int best = korb_md_name_idx(c, slots[0], gn, (uint32_t)strlen(gn));
        slots[2] = ID2SYM(korb_intern(c->vm, gn, (uint32_t)strlen(gn)));
        slots[3] = UNWRAP(korb_md_group(c, slots + 3, slots[0], best));
        CHECK(korb_hash_set(c, slots + 4, KORB_REF_AT(&slots[1]), KORB_REF_AT(&slots[2]), slots[3]));
    }
    return RESULT_OK(slots[1]);
}

/* ---- Regexp instance methods --------------------------------------------- */
static const char *korb_re_arg_type(korb_value v) {
    if (v == KORB_NIL) return "nil";
    if (v == KORB_TRUE) return "true";
    if (v == KORB_FALSE) return "false";
    return korb_type_name(v);
}
/* coerce arg → subject String (Symbol → name), or nil for non-strings. */
static RESULT korb_re_subject(CTX *c, korb_value *slots, korb_value v, korb_value *out) {
    if (KORB_STRING_P(v)) { *out = v; return RESULT_OK(KORB_TRUE); }
    if (SYMBOL_P(v)) {                                   /* the whole name (it may hold NUL), in its encoding */
        const uint32_t id = SYM2ID(v);
        *out = UNWRAP(korb_str_new(c, slots, korb_sym_name(c->vm, id), c->vm->sym_lens[id]));
        KORB_STR_ENC_SET(*out, c->vm->sym_encs[id]);
        return RESULT_OK(KORB_TRUE);
    }
    *out = KORB_NIL; return RESULT_OK(KORB_FALSE);
}
/* `re =~ str` / `str =~ re` core: set $~, return match CHAR index (or nil). */
static RESULT korb_re_match_set(CTX *c, korb_value *slots, korb_value re, korb_value str) {
    if (!KORB_REGEXP_P(re)) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    if (!KORB_STRING_P(str)) {                            /* coerce Symbol / #to_str; else no match */
        slots[0] = re;                                    /* root re across the coercion alloc */
        if (SYMBOL_P(str)) {
            CHECK(korb_re_subject(c, slots + 1, str, &str));   /* the whole name, in its encoding */
        } else if (KORB_OBJECT_P(str) && korb_responds_to_coerce_p(c, slots + 1, &str, korb_intern_lit(c->vm, "to_str", 6))) {
            slots[1] = str;
            RESULT sr = korb_send_impl(c, slots + 2, korb_intern_lit(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (!KORB_STRING_P(sr.value))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into String (%s#to_str gives %s)", korb_coerce_name(c, slots[1]), korb_coerce_name(c, slots[1]), korb_coerce_name(c, sr.value));
            str = sr.value;
        } else { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
        re = slots[0];
    }
    slots[0] = re; slots[1] = str;
    CHECK(korb_re_check_subject(c, slots + 2, slots[1]));
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[0], slots[1], 0, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    const long cidx = korb_re_bchar(c->vm, VAL2STR(slots[1]), m.starts[0]);
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[1], slots[0], &m));
    korb_re_set_lastmatch(c, slots[2]);
    return RESULT_OK(LONG2FIX(cidx));
}
/* String#=~ — a String operand is a TypeError; a non-Regexp object gets the
 * question turned around (`obj =~ self`), which is how Regexp-alikes work. */
static RESULT korb_m_str_match_op(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const korb_value pat = KORB_SLICE_GET(a, 0);
    const bool str_recv = KORB_STRING_P(KORB_REF_GET(self));   /* shared with NilClass#=~ / Symbol#=~ */
    if (UNLIKELY(str_recv && KORB_STRING_P(pat)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "type mismatch: String given");
    if (UNLIKELY(str_recv && !KORB_REGEXP_P(pat) && KORB_OBJECT_P(pat))) {
        slots[0] = pat; slots[1] = KORB_REF_GET(self);
        return korb_send(c, slots + 2, korb_intern_lit(c->vm, "=~", 2), 0, 1);
    }
    return korb_re_match_set(c, slots, pat, KORB_REF_GET(self));
}
/* `Regexp.allocate` leaves a plain object behind, and a subclass whose
 * #initialize never reached `super` leaves a KORB_RE_UNINIT payload: every
 * Regexp method must refuse both rather than quietly answer nil / //. */
#define KORB_RE_CHECK(c, slots, self) do { \
    if (UNLIKELY(!KORB_REGEXP_P(KORB_REF_GET(self)) || \
                 (VAL2RE(KORB_REF_GET(self))->flags & KORB_RE_UNINIT))) \
        return korb_raise((c), (slots), KORB_E_TYPE, 0, "uninitialized Regexp"); \
} while (0)
static RESULT korb_m_re_match_op(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_RE_CHECK(c, slots, self);
    korb_value v = KORB_SLICE_GET(a, 0);
    /* only nil is "no match" (=== answers false instead, so this check is not in korb_re_match_set) */
    if (UNLIKELY(v != KORB_NIL && !KORB_STRING_P(v) && !SYMBOL_P(v) &&
                 !(KORB_OBJECT_P(v) && korb_responds_to_coerce_p(c, slots, &v, korb_intern_lit(c->vm, "to_str", 6)))))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, v));
    return korb_re_match_set(c, slots, KORB_REF_GET(self), v);
}
/* A character index in the SUBJECT's own encoding → a byte offset.  Only UTF-8
 * needs the stepping walk: a single-byte encoding (BINARY / US-ASCII /
 * ISO-8859-x) indexes by byte.  Walking a BINARY string as UTF-8 made
 * `bin.index(/x/, pos)` skip past every multi-byte-looking run in it — marcel's
 * XML sniffer scans a transcoded prefix exactly that way. */
static size_t korb_re_startb(const struct korb_vm *const vm, korb_value sv, long startc) {
    if (startc <= 0) return 0;
    const KorbString *const s = VAL2STR(sv);
    return korb_enc_byteoff(vm, KORB_STR_ENC(sv), korb_str_ptr(s), s->len, (uint32_t)startc);
}
/* Character count in the subject's own encoding (same rule as above). */
static long korb_re_ncp(const struct korb_vm *const vm, korb_value sv) {
    const KorbString *const s = VAL2STR(sv);
    return (long)korb_enc_charcount(vm, KORB_STR_ENC(sv), korb_str_ptr(s), s->len);
}
static RESULT korb_m_re_match_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {   /* match?(str[, pos]) — no $~ */
    KORB_RE_CHECK(c, slots, self);
    korb_value subj; slots[0] = KORB_REF_GET(self);
    if (UNWRAP(korb_re_subject(c, slots + 1, KORB_SLICE_GET(a, 0), &subj)) != KORB_TRUE) return RESULT_OK(KORB_FALSE);
    slots[1] = subj;
    size_t startb = 0;
    if (KORB_SLICE_LEN(a) >= 2) {                    /* optional char start position */
        korb_sword_t pos = 0; if (korb_to_index(KORB_SLICE_GET(a, 1), &pos)) {
            const long ncp = korb_re_ncp(c->vm, slots[1]);
            if (pos < 0) pos += ncp;
            if (pos < 0 || pos > ncp) return RESULT_OK(KORB_FALSE);
            startb = korb_re_startb(c->vm, slots[1], (long)pos);
        }
    }
    CHECK(korb_re_check_subject(c, slots + 2, slots[1]));
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[0], slots[1], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    return RESULT_OK(rr.value == KORB_TRUE ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_re_case_eq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {   /* === (sets $~) */
    KORB_RE_CHECK(c, slots, self);
    RESULT r = korb_re_match_set(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0));
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(r.value == KORB_NIL ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_re_source(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)a; KORB_RE_CHECK(c, slots, self); return RESULT_OK(VAL2RE(KORB_REF_GET(self))->source); }
/* Regexp#timeout — what Regexp.new(timeout:) was given, nil otherwise (its
 * matches run under it: korb_re_run). */
static RESULT korb_m_re_timeout(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)a; KORB_RE_CHECK(c, slots, self); return RESULT_OK(VAL2RE(KORB_REF_GET(self))->timeout); }
/* Regexp#~ — match against $_, like `$_ =~ self`. */
static RESULT korb_m_re_match_lastline(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    slots[0] = KORB_REF_GET(self);
    const korb_value ll = korb_const_get(c->vm, korb_intern_lit(c->vm, "$_", 2));
    if (!KORB_STRING_P(ll)) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }   /* before the check (CRuby) */
    KORB_RE_CHECK(c, slots, self);
    slots[1] = ll;
    return korb_re_match_set(c, slots + 2, slots[0], slots[1]);
}
/* The pattern bytes as stored, nil for a `Regexp.allocate` husk: #encoding and
 * #source are defined over this so neither has to re-enter the other. */
static RESULT korb_m_re_source_raw(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c; (void)slots; (void)a;
    const korb_value v = KORB_REF_GET(self);
    return RESULT_OK(KORB_REGEXP_P(v) && !(VAL2RE(v)->flags & KORB_RE_UNINIT) ? VAL2RE(v)->source : KORB_NIL);
}
/* prism flag bits → Ruby's Regexp option bits.  The encoding flags (/u /e /s /n)
 * all mean "the pattern's encoding is fixed": Ruby reports FIXEDENCODING (16)
 * for /u /e /s and NOENCODING (32) for /n. */
static int korb_re_ruby_opts(uint32_t flags) {
    int o = 0;
    if (flags & 4u)  o |= 1;    /* IGNORECASE */
    if (flags & 8u)  o |= 2;    /* EXTENDED */
    if (flags & 16u) o |= 4;    /* MULTILINE */
    if (flags & (64u | 256u | 512u | KORB_RE_FIXENC | KORB_RE_U8FIX)) o |= 16;   /* /u /e /s and Regexp::FIXEDENCODING */
    if (flags & 128u) o |= 32;                  /* ASCII-8BIT (/n) → NOENCODING */
    return o;
}
static RESULT korb_m_re_options(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)a; KORB_RE_CHECK(c, slots, self); return RESULT_OK(LONG2FIX(korb_re_ruby_opts(korb_re_u8_flags(c->vm, KORB_REF_GET(self))))); }
/* /e and /s name an encoding no Ruby-visible option bit distinguishes; #encoding
 * asks for it here. */
static RESULT korb_m_re_enc_hint(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a; const uint32_t f = VAL2RE(KORB_REF_GET(self))->flags;
    const char *const nm = (f & 64u) ? "EUC-JP" : (f & 256u) ? "Windows-31J" : NULL;
    return nm ? korb_str_new(c, slots, nm, (uint32_t)strlen(nm)) : RESULT_OK(KORB_NIL);
}
static RESULT korb_m_re_casefold(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)a; KORB_RE_CHECK(c, slots, self); return RESULT_OK((VAL2RE(KORB_REF_GET(self))->flags & 4u) ? KORB_TRUE : KORB_FALSE); }
/* True when the `(` at s[0] is closed by the `)` at s[len-1], i.e. the source is
 * one group.  Skips escapes and character-class bodies so a `)` inside `[)]`
 * doesn't close anything. */
static bool korb_re_one_group(const char *restrict s, size_t len) {
    size_t depth = 0;
    bool in_class = false;
    for (size_t i = 0; i < len; i++) {
        const char ch = s[i];
        if (ch == '\\') { i++; continue; }
        if (in_class) { if (ch == ']') in_class = false; continue; }
        if (ch == '[') {
            in_class = true;
            if (i + 1 < len && s[i + 1] == '^') i++;
            if (i + 1 < len && s[i + 1] == ']') i++;   /* leading `]` is a member */
            continue;
        }
        if (ch == '(') depth++;
        else if (ch == ')') { if (--depth == 0) return i == len - 1; }
    }
    return false;
}

/* `(?mix-mix:source)` — a Regexp's own options travel with its source, which is
 * what makes an embedded copy (Regexp#to_s, Regexp.union) keep them.  Like
 * CRuby, an option group wrapping the whole source is folded into that header
 * instead of being nested: `/(?i:.)/.to_s` is `(?i-mx:.)`. */
static void korb_re_write_to_s(const struct korb_vm *const vm, FILE *ms, korb_value re) {
    const uint32_t f = VAL2RE(re)->flags;
    const korb_value srcv = VAL2RE(re)->source;                      /* nil for an allocate'd / never-compiled Regexp */
    const KorbString *const src = KORB_STRING_P(srcv) ? VAL2STR(srcv) : NULL;
    bool m = (f & 16u) != 0, i = (f & 4u) != 0, x = (f & 8u) != 0;
    const char *body = src ? korb_str_ptr(src) : "";
    size_t len = src ? src->len : 0;
    korb_re_wide_buf wb = { NULL, NULL, 0 };           /* a UTF-16/32 source is written as UTF-8 (callers re-encode) */
    bool wbe = false;
    const uint32_t wunit = src ? korb_re_enc_unit(vm, KORB_STR_ENC(srcv), &wbe) : 0;
    if (UNLIKELY(wunit) && korb_re_wide_decode((const unsigned char *)body, (uint32_t)len, wunit, wbe, false, &wb)) {
        body = wb.u8; len = wb.len;
    }

    /* `(?opts)rest` sets options for the rest and can repeat; `(?opts:body)`
     * folds in only when its `)` really is the last byte. */
    while (len >= 4 && body[0] == '(' && body[1] == '?') {
        bool nm = m, ni = i, nx = x, off = false, ok = true;
        size_t p = 2;
        for (; p < len; p++) {
            const char ch = body[p];
            if (ch == '-' && !off) { off = true; continue; }
            if (ch == 'm')      nm = !off;
            else if (ch == 'i') ni = !off;
            else if (ch == 'x') nx = !off;
            else { ok = (ch == ':' || ch == ')'); break; }
        }
        if (!ok || p >= len) break;
        if (body[p] == ')') { m = nm; i = ni; x = nx; body += p + 1; len -= p + 1; continue; }
        if (!korb_re_one_group(body, len)) break;
        m = nm; i = ni; x = nx;
        body += p + 1; len -= p + 2;   /* drop the header and the trailing ) */
        break;
    }

    char on[4]; int no = 0; char neg[4]; int nf = 0;
    if (m) on[no++]='m'; else neg[nf++]='m';
    if (i) on[no++]='i'; else neg[nf++]='i';
    if (x) on[no++]='x'; else neg[nf++]='x';
    fputs("(?", ms); fwrite(on, 1, (size_t)no, ms);
    if (nf) { fputc('-', ms); fwrite(neg, 1, (size_t)nf, ms); }
    fputc(':', ms);
    for (size_t k = 0; k < len; k++) {                 /* a bare `/` is escaped, as in #inspect (CRuby rb_reg_expr_str) */
        if (body[k] == '\\' && k + 1 < len) { fputc('\\', ms); fputc(body[++k], ms); continue; }
        if (body[k] == '/') fputc('\\', ms);
        fputc(body[k], ms);
    }
    fputc(')', ms);
    free(wb.u8);
}
/* Re-encode UTF-8 bytes as UTF-16/32 (the inverse of korb_re_wide_decode). */
static void korb_re_u8_to_wide(FILE *const ms, const char *const b, const uint32_t n, const uint32_t unit, const bool be) {
    for (uint32_t i = 0; i < n; ) {
        uint32_t cl = 1; const uint32_t cp = korb_re_u8_get((const unsigned char *)b + i, n - i, &cl);
        uint32_t units[2] = { cp, 0 }; int nu = 1;
        if (unit == 2 && cp >= 0x10000) { units[0] = 0xD800u + ((cp - 0x10000u) >> 10); units[1] = 0xDC00u + (cp & 0x3FFu); nu = 2; }
        for (int u = 0; u < nu; u++)
            for (uint32_t k = 0; k < unit; k++) fputc((int)((units[u] >> (8 * (be ? unit - 1 - k : k))) & 0xFF), ms);
        i += cl ? cl : 1;
    }
}
static RESULT korb_m_re_to_s(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a; KORB_RE_CHECK(c, slots, self); slots[0] = KORB_REF_GET(self);
    if (KORB_STRING_P(VAL2RE(slots[0])->to_s)) {   /* kept from an earlier call (Rails composes patterns with /#{re}/ per request) */
        slots[1] = VAL2RE(slots[0])->to_s;
        const uint32_t n = VAL2STR(slots[1])->len;
        KorbString *const s = korb_str_alloc(c, slots + 2, n);
        memcpy(korb_str_ptr(s), korb_str_ptr(VAL2STR(slots[1])), n);   /* re-read after the alloc */
        KORB_STR_ENC_SET((korb_value)s, KORB_STR_ENC(slots[1]));
        return RESULT_OK((korb_value)s);
    }
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    korb_re_write_to_s(c->vm, ms, slots[0]);
    fclose(ms);
    const korb_value srcv = VAL2RE(slots[0])->source;
    const uint32_t senc = KORB_STRING_P(srcv) ? KORB_STR_ENC(srcv) : KORB_ENC_USASCII;
    bool wbe = false;
    const uint32_t wunit = korb_re_enc_unit(c->vm, senc, &wbe);
    if (UNLIKELY(wunit)) {                                /* CRuby: to_s stays in the pattern's encoding */
        char *wbuf = NULL; size_t wz = 0; FILE *const ws = open_memstream(&wbuf, &wz);
        korb_re_u8_to_wide(ws, buf, (uint32_t)z, wunit, wbe); fclose(ws); free(buf); buf = wbuf; z = wz;
    }
    RESULT r = korb_str_new(c, slots + 1, buf, (uint32_t)z);
    if (LIKELY(r.state == KORB_NORMAL) && wunit) KORB_STR_ENC_SET(r.value, senc);
    if (LIKELY(r.state == KORB_NORMAL)) {                 /* keep a private copy for the next call */
        slots[1] = r.value;
        const RESULT k = korb_str_new(c, slots + 2, buf, (uint32_t)z);
        if (LIKELY(k.state == KORB_NORMAL)) {
            KORB_STR_ENC_SET(k.value, KORB_STR_ENC(slots[1]));
            ARO_STORE(c, VAL2RE(slots[0]), (korb_value *)(uintptr_t)&VAL2RE(slots[0])->to_s, k.value);
        }
        r = RESULT_OK(slots[1]);
    }
    free(buf);
    return r;
}
static RESULT korb_m_re_inspect(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (UNLIKELY(!KORB_REGEXP_P(KORB_REF_GET(self)) || (VAL2RE(KORB_REF_GET(self))->flags & KORB_RE_UNINIT))) {
        const korb_value k = korb_dispatch_class(c, KORB_REF_GET(self));   /* #<Regexp:0x…> (CRuby's rb_any_to_s) */
        char cn[192], b[240];
        if (VAL2CLASS(k)->name_sym) korb_class_qname_into(c, k, cn, sizeof cn); else korb_desc_inspect(c, k, cn, sizeof cn);
        const int n = snprintf(b, sizeof b, "#<%s:0x%016zx>", cn, korb_inspect_addr(c, KORB_REF_GET(self)));
        return korb_str_new(c, slots, b, (uint32_t)n);
    }
    (void)a; slots[0] = KORB_REF_GET(self);
    const uint32_t f = VAL2RE(slots[0])->flags;
    const korb_value srcv = VAL2RE(slots[0])->source;                /* nil for an allocate'd / never-compiled Regexp */
    const KorbString *const src = KORB_STRING_P(srcv) ? VAL2STR(srcv) : NULL;
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    fputc('/', ms);
    /* escape bare forward slashes; pass through existing "\x" escape pairs verbatim */
    const char *p = src ? korb_str_ptr(src) : ""; const char *const end = p + (src ? src->len : 0);
    while (p < end) {
        if (*p == '\\' && p + 1 < end) { fputc('\\', ms); fputc(p[1], ms); p += 2; continue; }
        if (*p == '/') { fputc('\\', ms); fputc('/', ms); p++; continue; }
        fputc(*p++, ms);
    }
    fputc('/', ms);
    if (f & 16u) fputc('m', ms);
    if (f & 4u) fputc('i', ms);
    if (f & 8u) fputc('x', ms);
    if (f & 128u) fputc('n', ms);   /* NOENCODING (/n) */
    fclose(ms); RESULT r = korb_str_new(c, slots + 1, buf, (uint32_t)z); free(buf); return r;
}
static RESULT korb_m_re_names(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a; KORB_RE_CHECK(c, slots, self); slots[0] = KORB_REF_GET(self); slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    return korb_md_names_into(c, slots + 2, slots[0], false, KORB_REF_AT(&slots[1]));
}
/* Regexp#named_captures → { "name" => [group numbers] } (dup names accumulate). */
static RESULT korb_m_re_named_captures(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a; KORB_RE_CHECK(c, slots, self);
    if (c->vm->re_named_fn == NULL) korb_re_load(c->vm);
    slots[0] = KORB_REF_GET(self); slots[1] = UNWRAP(korb_hash_new(c, slots + 1, 0));
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (!nf) return RESULT_OK(slots[1]);
    for (int k = 0; ; k++) {
        const KorbString *pat = VAL2STR(VAL2RE(slots[0])->source);   /* re-fetch (GC-moving) */
        const uint32_t flags = VAL2RE(slots[0])->flags;
        int gi = -1; const char *gn = nf(korb_str_ptr(pat), pat->len, flags, k, &gi);
        if (!gn) break;
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, gn, (uint32_t)strlen(gn)));   /* name key */
        int32_t idx = korb_hash_find(VAL2HASH(slots[1]), slots[2]);
        if (idx >= 0) slots[3] = korb_hash_pairs(VAL2HASH(slots[1]))[2 * idx + 1];     /* existing array */
        else { slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 1)); CHECK(korb_hash_set(c, slots + 4, KORB_REF_AT(&slots[1]), KORB_REF_AT(&slots[2]), slots[3])); }
        CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), LONG2FIX(gi)));
    }
    return RESULT_OK(slots[1]);
}
/* == and hash ignore the /n (NOENCODING, 128) flag — CRuby treats it as
 * encoding metadata, not part of the compiled-pattern identity. */
/* Regexp#== besides the source (CRuby rb_reg_equal): i/m/x and FIXEDENCODING here, the
 * encoding by #encoding (internal bits such as a literal's /u marker differ for equal patterns). */
static uint32_t korb_re_ident(const struct korb_vm *const vm, const korb_value re) {
    return (uint32_t)korb_re_ruby_opts(korb_re_u8_flags(vm, re)) & (7u | 16u);
}
static RESULT korb_m_re_eq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const korb_value o = KORB_SLICE_GET(a, 0), s = KORB_REF_GET(self);
    if (o == s) return RESULT_OK(KORB_TRUE);
    if (!KORB_REGEXP_P(o)) return RESULT_OK(KORB_FALSE);
    KORB_RE_CHECK(c, slots, self);
    if (UNLIKELY(VAL2RE(o)->flags & KORB_RE_UNINIT)) return korb_raise(c, slots, KORB_E_TYPE, 0, "uninitialized Regexp");
    if (korb_re_ident(c->vm, s) != korb_re_ident(c->vm, o)) return RESULT_OK(KORB_FALSE);
    const KorbString *a1 = VAL2STR(VAL2RE(s)->source), *b1 = VAL2STR(VAL2RE(o)->source);
    if (a1->len != b1->len || memcmp(korb_str_ptr(a1), korb_str_ptr(b1), a1->len) != 0) return RESULT_OK(KORB_FALSE);
    slots[0] = o;
    slots[1] = UNWRAP(korb_send_impl(c, slots + 1, korb_intern_lit(c->vm, "encoding", 8), 0, 0, NULL, NULL, NULL));
    slots[2] = KORB_REF_GET(self);
    const korb_value se = UNWRAP(korb_send_impl(c, slots + 3, korb_intern_lit(c->vm, "encoding", 8), 0, 0, NULL, NULL, NULL));
    return RESULT_OK(se == slots[1] ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_re_hash(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a; KORB_RE_CHECK(c, slots, self);
    const KorbString *src = VAL2STR(VAL2RE(KORB_REF_GET(self))->source);
    uint32_t h = korb_str_hash(korb_str_ptr(src), src->len) ^ (korb_re_ident(c->vm, KORB_REF_GET(self)) * 2654435761u);
    return RESULT_OK(LONG2FIX((long)h));
}
/* Regexp#match(str[,pos]) → MatchData|nil (sets $~); block form yields it. */
static RESULT korb_m_re_match(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    KORB_RE_CHECK(c, slots, self);
    slots[0] = KORB_REF_GET(self); korb_value subj;
    if (KORB_SLICE_GET(a, 0) == KORB_NIL) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    if (UNWRAP(korb_re_subject(c, slots + 2, KORB_SLICE_GET(a, 0), &subj)) != KORB_TRUE)
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(KORB_SLICE_GET(a, 0)));
    slots[1] = subj;
    long startc = 0;
    if (KORB_SLICE_LEN(a) >= 2) {                  /* pos: #to_int'd, not silently ignored */
        korb_sword_t p = 0;
        korb_value pv0 = KORB_SLICE_GET(a, 1);
        if (UNLIKELY(!korb_to_index(pv0, &p))) {
            const RESULT cr = korb_coerce_to_int(c, slots + 2, &pv0);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value != KORB_TRUE || !korb_to_index(pv0, &p))
                return korb_raise_no_int(c, slots + 2, KORB_SLICE_GET(a, 1));
        }
        startc = (long)p;
    }
    /* pos is a character index in the subject's own encoding (a byte index only
     * where a character IS a byte). */
    if (startc < 0) startc += korb_re_ncp(c->vm, slots[1]);
    size_t startb = (startc <= 0) ? 0 : korb_re_startb(c->vm, slots[1], startc);
    CHECK(korb_re_check_subject(c, slots + 2, slots[1]));
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[0], slots[1], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[1], slots[0], &m));
    korb_re_set_lastmatch(c, slots[2]);
    if (block != NULL) return korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
    return RESULT_OK(slots[2]);
}

/* Regexp#__match_byte(str, byteoff) — #match with the position given in BYTES.
 * StringScanner keeps a byte cursor; converting it to a character index for
 * #match walked the subject on every scan, which made scanning a large string
 * O(n^2) (crack's 2.4MB JSON took >5 min instead of 1.5s).  Private: the public
 * #match keeps its character-index contract. */
static RESULT korb_m_re_match_byte(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_RE_CHECK(c, slots, self);
    slots[0] = KORB_REF_GET(self);
    korb_value subj;
    if (UNWRAP(korb_re_subject(c, slots + 2, KORB_SLICE_GET(a, 0), &subj)) != KORB_TRUE)
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(KORB_SLICE_GET(a, 0)));
    slots[1] = subj;
    korb_sword_t off = 0;
    if (UNLIKELY(!korb_to_index(KORB_SLICE_GET(a, 1), &off)))
        return korb_raise_no_int(c, slots + 2, KORB_SLICE_GET(a, 1));
    const KorbString *const s = VAL2STR(slots[1]);
    if (off < 0) off += (korb_sword_t)s->len;
    if (off < 0 || off > (korb_sword_t)s->len) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[0], slots[1], (size_t)off, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[1], slots[0], &m));
    korb_re_set_lastmatch(c, slots[2]);
    return RESULT_OK(slots[2]);
}

/* ---- String#match / match? ----------------------------------------------- */
static RESULT korb_re_coerce_pat(CTX *c, korb_value *slots, korb_value pv, korb_value *out) {
    if (KORB_REGEXP_P(pv)) { *out = pv; return RESULT_OK(KORB_TRUE); }
    if (KORB_STRING_P(pv)) { slots[0] = pv; *out = UNWRAP(korb_re_alloc(c, slots + 1, slots[0], 0)); return RESULT_OK(KORB_TRUE); }
    if (KORB_OBJECT_P(pv)) {                             /* a #to_str object is the pattern source */
        korb_value cv = pv;
        const uint32_t to_str = korb_intern_lit(c->vm, "to_str", 6);
        if (korb_responds_to_coerce_p(c, slots, &cv, to_str)) {
            slots[0] = cv;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (KORB_STRING_P(sr.value)) {
                slots[0] = sr.value;
                *out = UNWRAP(korb_re_alloc(c, slots + 1, slots[0], 0));
                return RESULT_OK(KORB_TRUE);
            }
        }
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_re_arg_type(pv));
}
/* `str.match(pat, pos)` — a NEGATIVE pos counts characters back from the end;
 * once out of range on either side there is no match at all.  Returns false when
 * the position cannot be used, else the (non-negative) character offset. */
static bool
korb_re_start_char(CTX *c, korb_value subj, korb_value posv, long *out)
{
    korb_sword_t p = 0;
    if (!korb_to_index(posv, &p)) { *out = 0; return true; }   /* unusable → treat as 0, as before */
    if (p < 0) {
        const KorbString *const s = VAL2STR(subj);
        const uint32_t nch = korb_enc_charcount(c->vm, KORB_STR_ENC(subj), korb_str_ptr(s), s->len);
        p += (korb_sword_t)nch;
        if (p < 0) return false;
    }
    *out = (long)p;
    return true;
}
static RESULT korb_m_str_match(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    korb_value subj;
    if (UNWRAP(korb_re_subject(c, slots, KORB_REF_GET(self), &subj)) != KORB_TRUE) return RESULT_OK(KORB_NIL);
    slots[0] = subj; korb_value re; RESULT cr = korb_re_coerce_pat(c, slots + 1, KORB_SLICE_GET(a, 0), &re);
    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    slots[1] = re;
    if (UNLIKELY(KORB_REGEXP_P(KORB_SLICE_GET(a, 0)) && !korb_exact_class_p(c, re, KORB_C_REGEXP))) {
        /* CRuby delegates to pattern.match(self[, pos]), so a subclass override is honoured */
        const uint32_t argc = KORB_SLICE_LEN(a) >= 2 ? 2 : 1;
        slots[2] = re; slots[3] = KORB_REF_GET(self);
        if (argc == 2) slots[4] = KORB_SLICE_GET(a, 1);
        return korb_send_impl(c, slots + 3 + argc, korb_intern_lit(c->vm, "match", 5), 0, argc, block, def_env, cself);
    }
    long startc = 0;
    if (KORB_SLICE_LEN(a) >= 2 && !korb_re_start_char(c, slots[0], KORB_SLICE_GET(a, 1), &startc))
        { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }   /* out of range → no match */
    size_t startb = korb_re_startb(c->vm, slots[0], startc);
    CHECK(korb_re_check_subject(c, slots + 2, slots[0]));
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[0], slots[1], &m));
    korb_re_set_lastmatch(c, slots[2]);
    if (block != NULL) return korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_str_match_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value subj;
    if (UNWRAP(korb_re_subject(c, slots, KORB_REF_GET(self), &subj)) != KORB_TRUE) return RESULT_OK(KORB_FALSE);
    slots[0] = subj; korb_value re; RESULT cr = korb_re_coerce_pat(c, slots + 1, KORB_SLICE_GET(a, 0), &re);
    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    slots[1] = re;
    long startc = 0;
    if (KORB_SLICE_LEN(a) >= 2 && !korb_re_start_char(c, slots[0], KORB_SLICE_GET(a, 1), &startc))
        return RESULT_OK(KORB_FALSE);                    /* out of range → no match */
    size_t startb = korb_re_startb(c->vm, slots[0], startc);
    CHECK(korb_re_check_subject(c, slots + 2, slots[0]));
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    return RESULT_OK(rr.value == KORB_TRUE ? KORB_TRUE : KORB_FALSE);
}

/* ---- String#scan --------------------------------------------------------- */
static RESULT korb_re_scan_elem(CTX *c, korb_value *slots, korb_value subj, korb_value mdv, const korb_re_match_t *m) {
    if (m->n_groups == 0) {
        slots[0] = subj; const uint32_t ml = (uint32_t)(m->ends[0] - m->starts[0]);
        KorbString *r = korb_str_alloc(c, slots + 1, ml);
        memcpy(korb_str_ptr(r), korb_str_ptr(VAL2STR(slots[0])) + m->starts[0], ml);
        KORB_STR_ENC_SET((korb_value)r, KORB_STR_ENC(slots[0]));   /* result keeps self's encoding */
        return RESULT_OK((korb_value)r);
    }
    slots[0] = mdv; slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)m->n_groups));
    for (int i = 1; i <= m->n_groups; i++) { slots[2] = UNWRAP(korb_md_group(c, slots + 2, slots[0], i)); CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[1]), slots[2])); }
    return RESULT_OK(slots[1]);
}
/* Coerce a String#scan pattern: a Regexp as-is, but a String matches LITERALLY
 * (unlike #match, which reads it as a regex source), so escape it first. */
static RESULT korb_re_coerce_pat_literal(CTX *c, korb_value *slots, korb_value pv, korb_value *out) {
    if (KORB_REGEXP_P(pv)) { *out = pv; return RESULT_OK(KORB_TRUE); }
    if (KORB_STRING_P(pv)) {                             /* a Symbol is a TypeError, like any non-String */
        const char *b = korb_str_ptr(VAL2STR(pv)); const uint32_t n = VAL2STR(pv)->len;
        const uint32_t penc = KORB_STR_ENC(pv);          /* read before any alloc moves pv */
        /* escaping at most doubles it: a plain buffer, not a stdio stream (str.sub("x", y) paid fputc / fwrite per char) */
        char sbuf[256];
        const size_t cap = (size_t)n * 2u + 1u;
        char *const buf = cap <= sizeof sbuf ? sbuf : malloc(cap);
        if (!buf) { fprintf(stderr, "koruby_precise: oom (pattern escape)\n"); abort(); }
        size_t z = 0;
        /* Step CHARACTERS: a Shift_JIS trail byte can be '^' or '-', and escaping
         * it would cut the character in half. */
        for (uint32_t i = 0; i < n; ) {
            const uint32_t cl = korb_str_char_bytes(c->vm, penc, (const unsigned char *)b, i, n);
            const uint32_t step = cl ? cl : 1;
            if (cl == 1 && strchr("\\.*+?()[]{}|-^$", (unsigned char)b[i])) buf[z++] = '\\';
            memcpy(buf + z, b + i, cl); z += cl;
            i += step;
        }
        const RESULT er = korb_str_new(c, slots, buf, (uint32_t)z);
        if (buf != sbuf) free(buf);
        slots[0] = UNWRAP(er);
        KORB_STR_ENC_SET(slots[0], penc);                /* the literal pattern keeps the string's encoding */
        *out = UNWRAP(korb_re_alloc(c, slots + 1, slots[0], 0));
        return RESULT_OK(KORB_TRUE);
    }
    if (KORB_OBJECT_P(pv)) {                             /* #to_str-convertible pattern (matches literally) */
        korb_value cv = pv;
        const uint32_t to_str = korb_intern_lit(c->vm, "to_str", 6);
        if (korb_responds_to_coerce_p(c, slots, &cv, to_str)) {
            slots[0] = cv;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (KORB_STRING_P(sr.value)) return korb_re_coerce_pat_literal(c, slots, sr.value, out);
        }
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_re_arg_type(pv));
}
static RESULT korb_m_str_scan(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    korb_value re = KORB_NIL; RESULT cr = korb_re_coerce_pat_literal(c, slots, KORB_SLICE_GET(a, 0), &re);
    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    korb_re_set_lastmatch(c, KORB_NIL);              /* $~ ← nil, then last match (or stays nil if none) */
    slots[0] = KORB_REF_GET(self); slots[1] = re;
    CHECK(korb_re_check_subject(c, slots + 2, slots[0]));
    slots[2] = block ? KORB_NIL : UNWRAP(korb_ary_new(c, slots + 2, 0));
    long off = 0;
    for (;;) {
        const KorbString *s = VAL2STR(slots[0]); if (off > (long)s->len) break;
        korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 3, slots[1], slots[0], (size_t)off, &m);
        if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
        if (rr.value != KORB_TRUE) break;
        slots[3] = UNWRAP(korb_re_build_md(c, slots + 3, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[3]);
        slots[4] = UNWRAP(korb_re_scan_elem(c, slots + 4, slots[0], slots[3], &m));
        if (block) {
            RESULT yr = korb_block_yield(c, slots + 5, block, def_env, &slots[4], 1, cself);
            if (UNLIKELY(yr.state != KORB_NORMAL)) return yr;
            korb_re_set_lastmatch(c, slots[3]);        /* the block may have matched → restore this iteration's $~ */
        }
        else CHECK(korb_ary_push_val(c, slots + 5, KORB_REF_AT(&slots[2]), slots[4]));
        off = (m.ends[0] > m.starts[0]) ? m.ends[0] : m.starts[0] + 1;
        if (UNLIKELY(KORB_STR_ENC(slots[0]) >= KORB_ENC_OTHER_MIN) && m.ends[0] == m.starts[0] &&
            m.starts[0] < (long)VAL2STR(slots[0])->len) {    /* UTF-16/32 etc.: no UTF-8 continuation bytes */
            const KorbString *s2 = VAL2STR(slots[0]);
            off = m.starts[0] + korb_re_char_len(c->vm, KORB_STR_ENC(slots[0]), (const unsigned char *)korb_str_ptr(s2),
                                                 (uint32_t)m.starts[0], s2->len);
        } else if (KORB_STR_ENC(slots[0]) < KORB_ENC_OTHER_MIN && !KORB_ENC_SB(c->vm, KORB_STR_ENC(slots[0]))) {   /* an empty match advances one CHARACTER,
                                                              * not one byte — but a single-byte encoding
                                                              * has no continuation bytes to skip */
            const KorbString *s2 = VAL2STR(slots[0]);
            while (off < (long)s2->len && ((unsigned char)korb_str_ptr(s2)[off] & 0xC0) == 0x80) off++;
        }
    }
    return RESULT_OK(block ? slots[0] : slots[2]);
}

/* ---- String#gsub/sub with a Regexp (forward-declared for string.c) -------- */
static void korb_re_expand_repl(CTX *c, struct korb_bb *const ms, const char *rep, uint32_t rn, const char *subj, uint32_t slen, const korb_re_match_t *m, korb_value re) {
    for (uint32_t i = 0; i < rn; i++) {
        if (rep[i] == '\\' && i + 1 < rn) {
            char nx = rep[i + 1];
            if (nx >= '0' && nx <= '9') { int g = nx - '0'; if (g <= m->n_groups && m->starts[g] >= 0) korb_bb_put(ms, subj + m->starts[g], (size_t)(m->ends[g] - m->starts[g])); i++; continue; }
            else if (nx == '&') { if (m->starts[0] >= 0) korb_bb_put(ms, subj + m->starts[0], (size_t)(m->ends[0] - m->starts[0])); i++; continue; }
            else if (nx == '+') { for (int g = m->n_groups; g >= 1; g--) if (m->starts[g] >= 0) { korb_bb_put(ms, subj + m->starts[g], (size_t)(m->ends[g] - m->starts[g])); break; } i++; continue; }
            else if (nx == '`') { korb_bb_put(ms, subj, (size_t)m->starts[0]); i++; continue; }
            else if (nx == '\'') { korb_bb_put(ms, subj + m->ends[0], (size_t)(slen - m->ends[0])); i++; continue; }
            else if (nx == '\\') { korb_bb_putc(ms, '\\'); i++; continue; }
            else if (nx == 'k' && i + 2 < rn && rep[i + 2] == '<') {
                uint32_t j = i + 3; while (j < rn && rep[j] != '>') j++;
                int gi = -1; korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
                if (nf && KORB_REGEXP_P(re)) { const KorbString *pat = VAL2STR(VAL2RE(re)->source); const uint32_t fl = VAL2RE(re)->flags;
                    /* A name may be on SEVERAL groups (one per alternative, as in
                     * rubocop's `(?<name>..)::#<Class:\k<name>>|#<Class:(?<name>..)>`);
                     * the one that took part is the answer, last match winning —
                     * same rule MatchData#[name] follows.  Stopping at the first
                     * name match emitted the empty unmatched alternative. */
                    for (int k = 0; ; k++) { int idx = -1; const char *gn = nf(korb_str_ptr(pat), pat->len, fl, k, &idx); if (!gn) break;
                        if (strlen(gn) != j - (i + 3) || memcmp(gn, rep + i + 3, j - (i + 3)) != 0) continue;
                        if (idx <= m->n_groups && m->starts[idx] >= 0) gi = idx;   /* participated */
                        else if (gi < 0) gi = idx; } }
                if (gi >= 0 && gi <= m->n_groups && m->starts[gi] >= 0) korb_bb_put(ms, subj + m->starts[gi], (size_t)(m->ends[gi] - m->starts[gi]));
                i = j; continue;
            }
        }
        korb_bb_putc(ms, rep[i]);
    }
}
/* korb_re_expand_repl for a UTF-16/32 replacement: `\\1` etc. are code units,
 * not bytes.  (\\k<name> is not expanded here.) */
static void korb_re_expand_repl_wide(struct korb_bb *const ms, const char *rep, uint32_t rn, const char *subj, uint32_t slen,
                                     const korb_re_match_t *m, const uint32_t unit, const bool be) {
    const unsigned char *const r = (const unsigned char *)rep;
    for (uint32_t i = 0; i + unit <= rn; i += unit) {
        if (korb_re_wide_unit_at(r + i, unit, be) == '\\' && i + 2 * unit <= rn) {
            const uint32_t nx = korb_re_wide_unit_at(r + i + unit, unit, be);
            long b = -1, e = -1;
            if (nx >= '0' && nx <= '9') { const int g = (int)(nx - '0'); if (g <= m->n_groups) { b = m->starts[g]; e = m->ends[g]; } }
            else if (nx == '&') { b = m->starts[0]; e = m->ends[0]; }
            else if (nx == '+') { for (int g = m->n_groups; g >= 1; g--) if (m->starts[g] >= 0) { b = m->starts[g]; e = m->ends[g]; break; } }
            else if (nx == '`') { b = 0; e = m->starts[0]; }
            else if (nx == '\'') { b = m->ends[0]; e = (long)slen; }
            else if (nx == '\\') { korb_bb_put(ms, r + i, unit); i += unit; continue; }
            else { korb_bb_put(ms, r + i, unit); continue; }
            if (b >= 0 && e >= b) korb_bb_put(ms, subj + b, (size_t)(e - b));
            i += unit;
            continue;
        }
        korb_bb_put(ms, r + i, unit);
    }
}
/* String/Symbol → a literal (all-metachars-escaped) Regexp, for String-pattern
 * gsub/sub/split routed through the engine (so $~ / MatchData behave). */
RESULT korb_re_literal_regexp(CTX *c, korb_value *slots, korb_value pv, korb_value *out) {
    return korb_re_coerce_pat_literal(c, slots, pv, out);
}
/* Emit v into ms as a String, dispatching a user-defined #to_s (CRuby coerces
 * gsub block results and Hash-replacement values with #to_s). */
static RESULT korb_emit_to_s(CTX *c, korb_value *slots, struct korb_bb *const ms, korb_value v) {
    if (KORB_STRING_P(v)) { const KorbString *r = VAL2STR(v); korb_bb_put(ms, korb_str_ptr(r), r->len); return RESULT_OK(KORB_NIL); }
    slots[0] = v;
    RESULT sr = korb_send_impl(c, slots + 1, korb_intern_lit(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    if (KORB_STRING_P(sr.value)) { const KorbString *r = VAL2STR(sr.value); korb_bb_put(ms, korb_str_ptr(r), r->len); }
    else KORB_BB_VIA_FILE(ms, korb_fprint_to_s(c, vf_, sr.value));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_hash_aref(CTX *c, korb_value *slots, korb_ref self, korb_slice a);   /* fwd (hash.c) */
RESULT korb_re_str_gsub(CTX *c, korb_value *slots, korb_ref self, korb_slice a, korb_value re, bool global, bool in_place, NODE *block, korb_value *def_env, korb_value *cself) {
    slots[0] = KORB_REF_GET(self); slots[1] = re;
    const KorbString *s0 = VAL2STR(slots[0]); const uint32_t sn = s0->len;
    char *const src = malloc(sn ? sn : 1); memcpy(src, korb_str_ptr(s0), sn);
    char *rep = NULL; uint32_t rn = 0; korb_value hashrep = KORB_NIL;
    uint32_t rep_enc = KORB_ENC_UTF8; bool rep_asc = true;   /* a String replacement's own encoding joins the result's */
    if (block == NULL) {
        if (KORB_SLICE_LEN(a) < 2) {   /* sub → ArgumentError; gsub(pat) → Enumerator over the matches */
            if (!global) { free(src); return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 1, expected 2)"); }
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 0));
            korb_ref acc = KORB_REF_AT(&slots[2]);
            long eoff = 0;
            while (eoff <= (long)sn) {
                korb_re_match_t em; RESULT er = korb_re_run(c, slots + 3, slots[1], slots[0], (size_t)eoff, &em);
                if (UNLIKELY(er.state != KORB_NORMAL)) { free(src); return er; }
                if (er.value != KORB_TRUE) break;
                slots[3] = UNWRAP(korb_re_slice(c, slots + 3, &slots[0], em.starts[0], em.ends[0]));
                RESULT pr = korb_ary_push_val(c, slots + 4, acc, slots[3]);
                if (UNLIKELY(pr.state != KORB_NORMAL)) { free(src); return pr; }
                if (em.ends[0] > em.starts[0]) eoff = em.ends[0];
                else {                                  /* zero-width match: step one character */
                    uint32_t cl = (em.starts[0] < (long)sn) ? korb_re_char_len(c->vm, KORB_STR_ENC(slots[0]), (const unsigned char *)src, (uint32_t)em.starts[0], sn) : 1;
                    eoff = em.starts[0] + (cl ? cl : 1);
                }
            }
            free(src);
            slots[3] = UNWRAP(korb_enum_desc(c, slots + 3, KORB_REF_GET(self), "gsub"));
            RESULT nr = korb_enum_new(c, slots + 4, KORB_REF_GET(acc), slots[3]);
            if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
            VAL2ENUM(nr.value)->size_unknown = 1;       /* CRuby's gsub enumerator reports no size */
            return nr;
        }
        korb_value rv = KORB_SLICE_GET(a, 1);
        if (KORB_STRING_P(rv)) { const KorbString *rs = VAL2STR(rv); rn = rs->len; rep = malloc(rn ? rn : 1); memcpy(rep, korb_str_ptr(rs), rn);
                                 rep_enc = KORB_STR_ENC(rv); rep_asc = korb_str_ascii_only_p(c->vm, rv); }
        else if (KORB_HASH_P(rv)) { hashrep = rv; slots[2] = rv; }
        else {                          /* coerce replacement via #to_str */
            const uint32_t to_str = korb_intern_lit(c->vm, "to_str", 6);
            if (KORB_OBJECT_P(rv) && korb_responds_to_coerce_p(c, slots + 3, &rv, to_str)) {
                slots[3] = rv;
                RESULT sr = korb_send_impl(c, slots + 4, to_str, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) { free(src); return sr; }
                if (!KORB_STRING_P(sr.value)) {   /* it answered, with the wrong type: CRuby names the method */
                    free(src);
                    const char *const rn0 = korb_coerce_name(c, slots[3]);
                    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into String (%s#to_str gives %s)",
                                      rn0, rn0, korb_coerce_name(c, sr.value));
                }
                const KorbString *rs = VAL2STR(sr.value); rn = rs->len; rep = malloc(rn ? rn : 1); memcpy(rep, korb_str_ptr(rs), rn);
                rep_enc = KORB_STR_ENC(sr.value); rep_asc = korb_str_ascii_only_p(c->vm, sr.value);
            } else { free(src); return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, rv)); }
        }
    }
    struct korb_bb bb_; struct korb_bb *const ms = &bb_; korb_bb_init(ms);
    long off = 0; bool replaced = false;
    /* the result encoding is negotiated with every replacement, as CRuby does */
    const uint32_t senc = KORB_STR_ENC(KORB_REF_GET(self));
    uint32_t renc = senc;  bool rasc = true;
    bool renc_bad = false;  uint32_t renc_other = 0;
    /* only the source bytes that SURVIVE count: replacing the sole non-ASCII
     * run leaves an ASCII-only left side, which then yields to the replacement */
    #define KORB_GSUB_KEEP(p, n) do { \
        const unsigned char *kp_ = (const unsigned char *)(p); const size_t kn_ = (n); \
        for (size_t ki_ = 0; ki_ < kn_; ki_++) if (kp_[ki_] >= 0x80) { \
            if (!renc_bad && !korb_str_enc_fold_raw(c->vm, &renc, &rasc, senc, false)) \
                { renc_bad = true; renc_other = senc; } \
            break; \
        } \
        korb_bb_put(ms, (p), kn_); \
    } while (0)
    korb_re_match_t last_m; bool have_last = false;   /* POD copy of the final match → $~ after the loop */
    while (off <= (long)sn) {
        korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 3, slots[1], slots[0], (size_t)off, &m);
        if (UNLIKELY(rr.state != KORB_NORMAL)) { free(src); free(rep); korb_bb_free(ms); return rr; }
        if (rr.value != KORB_TRUE) break;
        korb_re_match_copy(&last_m, &m); have_last = true;
        const long ms0 = m.starts[0], me0 = m.ends[0];
        KORB_GSUB_KEEP(src + off, (size_t)(ms0 - off));
        if (block) {
            slots[3] = UNWRAP(korb_re_build_md(c, slots + 3, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[3]);
            slots[4] = UNWRAP(korb_md_group(c, slots + 4, slots[3], 0));
            RESULT yr = korb_block_yield(c, slots + 5, block, def_env, &slots[4], 1, cself);
            if (UNLIKELY(yr.state != KORB_NORMAL)) { free(src); korb_bb_free(ms); return yr; }
            if (in_place && UNLIKELY(VAL2STR(slots[0])->len != sn)) {   /* the block resized self under us */
                free(src); korb_bb_free(ms);
                return korb_raise(c, slots + 4, KORB_E_RUNTIME, 0, "string modified");
            }
            slots[4] = yr.value;
            if (KORB_STRING_P(slots[4]) && !renc_bad && !korb_str_enc_fold(c->vm, &renc, &rasc, slots[4]))
                { renc_bad = true; renc_other = KORB_STR_ENC(slots[4]); }
            RESULT er = korb_emit_to_s(c, slots + 5, ms, slots[4]);
            if (UNLIKELY(er.state != KORB_NORMAL)) { free(src); free(rep); korb_bb_free(ms); return er; }
        } else if (hashrep != KORB_NIL) {
            const uint32_t ml = (uint32_t)(me0 - ms0);
            slots[3] = UNWRAP(korb_str_new(c, slots + 3, src + ms0, ml));   /* whole match = hash key */
            KORB_STR_ENC_SET(slots[3], senc);   /* the match carries the SUBJECT's encoding, or a binary
                                                 * high-byte key misses a binary table (URI's escape table) */
            /* CRuby's rb_hash_aref: the C lookup with the default / default_proc / a Ruby #default, never a
             * subclass's or a redefined #[] (railsbench: 24k #[] sends an iteration) */
            slots[4] = slots[2];
            slots[5] = slots[3];
            RESULT hr = korb_m_hash_aref(c, slots + 6, KORB_REF_AT(&slots[4]), KORB_SLICE_MAKE(&slots[5], 1));
            if (UNLIKELY(hr.state != KORB_NORMAL)) { free(src); free(rep); korb_bb_free(ms); return hr; }
            slots[4] = hr.value;
            if (KORB_STRING_P(slots[4]) && !renc_bad && !korb_str_enc_fold(c->vm, &renc, &rasc, slots[4]))
                { renc_bad = true; renc_other = KORB_STR_ENC(slots[4]); }
            RESULT er = korb_emit_to_s(c, slots + 5, ms, slots[4]);        /* nil → "", else #to_s */
            if (UNLIKELY(er.state != KORB_NORMAL)) { free(src); free(rep); korb_bb_free(ms); return er; }
        } else {
            if (!rep_asc && !renc_bad && !korb_str_enc_fold_raw(c->vm, &renc, &rasc, rep_enc, false))
                { renc_bad = true; renc_other = rep_enc; }
            bool rbe = false;
            const uint32_t runit = UNLIKELY(senc >= KORB_ENC_OTHER_MIN) ? korb_re_enc_unit(c->vm, senc, &rbe) : 0;
            if (UNLIKELY(runit)) korb_re_expand_repl_wide(ms, rep, rn, src, sn, &m, runit, rbe);
            else korb_re_expand_repl(c, ms, rep, rn, src, sn, &m, slots[1]);
        }
        replaced = true;
        if (me0 > ms0) off = me0;
        else {
            if (ms0 < (long)sn) {
                uint32_t cl = KORB_ENC_SB(c->vm, senc) ? 1u
                            : korb_re_char_len(c->vm, senc, (const unsigned char *)src, (uint32_t)ms0, sn);
                if (!cl) cl = 1;
                KORB_GSUB_KEEP(src + ms0, cl);
                off = ms0 + cl;
            } else {
                off = ms0 + 1;
            }
        }
        if (!global) break;
    }
    if (off <= (long)sn) KORB_GSUB_KEEP(src + off, (size_t)(sn - off));
    #undef KORB_GSUB_KEEP
    free(src); free(rep);
    /* $~ = MatchData of the last match (nil if none), for access after gsub returns */
    if (have_last) {
        slots[3] = UNWRAP(korb_re_build_md(c, slots + 3, slots[0], slots[1], &last_m)); korb_re_set_lastmatch(c, slots[3]);
        if (in_place) CHECK(korb_re_lastmatch_detach(c, slots + 4));   /* $~ keeps the pre-substitution text */
    }
    else korb_re_set_lastmatch(c, KORB_NIL);
    if (renc_bad) { korb_bb_free(ms); return korb_raise_enc_compat(c, slots + 3, KORB_STR_ENC(KORB_REF_GET(self)), renc_other); }
    RESULT nr = korb_str_new(c, slots + 3, ms->p, ms->len); korb_bb_free(ms);
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    KORB_STR_ENC_SET(nr.value, renc);   /* self's encoding, widened by the replacements */
    if (!in_place) return nr;
    slots[3] = nr.value; const KorbString *res = VAL2STR(slots[3]); const uint32_t w = res->len;
    KorbString *s2 = korb_str_ensure(c, slots + 4, self, w); res = VAL2STR(slots[3]);
    memcpy(korb_str_ptr(s2), korb_str_ptr(res), w); s2->len = w; korb_str_ptr(s2)[w] = '\0';
    if (replaced) KORB_STR_ENC_SET(KORB_REF_GET(self), renc);   /* gsub! widens self's encoding too */
    return RESULT_OK(replaced ? KORB_REF_GET(self) : KORB_NIL);
}

/* ---- String#split with a Regexp (forward-declared for string.c) ---------- */
/* Emit a field [beg, beg+len) of subj into res, deferring empty fields when
 * empty_count >= 0 (limit omitted/0 → trailing empties suppressed).  Mirrors
 * MRI's split_string()/SPLIT_STR: on a non-empty field, flush any deferred
 * empties first, then push the substring. */
static RESULT korb_re_split_emit(CTX *c, korb_value *slots, korb_ref res, korb_value *subj, long beg, long len, long *empty_count) {
    if (*empty_count >= 0 && len == 0) { (*empty_count)++; return RESULT_OK(KORB_NIL); }
    while (*empty_count > 0) { slots[0] = UNWRAP(korb_str_new(c, slots, "", 0)); CHECK(korb_ary_push_val(c, slots + 1, res, slots[0])); (*empty_count)--; }
    slots[0] = UNWRAP(korb_re_slice(c, slots, subj, beg, beg + len));
    CHECK(korb_ary_push_val(c, slots + 1, res, slots[0]));
    return RESULT_OK(KORB_NIL);
}
RESULT korb_re_str_split(CTX *c, korb_value *slots, korb_ref self, korb_value re, long limit) {
    slots[0] = KORB_REF_GET(self); slots[1] = re; slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 0));
    korb_ref res = KORB_REF_AT(&slots[2]);
    const long len0 = (long)VAL2STR(slots[0])->len;
    if (len0 == 0) return RESULT_OK(KORB_REF_GET(res));      /* "".split(x) → [] (any limit) */
    /* empty_count: 0 → defer/suppress trailing empties (limit omitted/0);
     * -1 → push all empties immediately (negative or positive limit). */
    long empty_count = (limit == 0) ? 0 : -1;
    long beg = 0, start = 0, i = 1;
    int last_null = 0;
    for (;;) {
        const KorbString *s = VAL2STR(slots[0]); const long len = (long)s->len;
        if (start > len) break;
        korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 5, slots[1], slots[0], (size_t)start, &m);
        if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
        if (rr.value != KORB_TRUE) break;
        const long b0 = m.starts[0], e0 = m.ends[0];
        if (start == b0 && b0 == e0) {                       /* zero-width match at the search start */
            const unsigned char *d = (const unsigned char *)korb_str_ptr(VAL2STR(slots[0]));
            if (last_null == 1) {
                const uint32_t cl = korb_re_char_len(c->vm, KORB_STR_ENC(slots[0]), d, (uint32_t)beg, (uint32_t)len);
                CHECK(korb_re_split_emit(c, slots + 5, res, &slots[0], beg, cl, &empty_count));
                beg = start;
            } else {
                if (start == len) start++;
                else start += korb_re_char_len(c->vm, KORB_STR_ENC(slots[0]), d, (uint32_t)start, (uint32_t)len);
                last_null = 1;
                continue;
            }
        } else {
            CHECK(korb_re_split_emit(c, slots + 5, res, &slots[0], beg, b0 - beg, &empty_count));
            beg = start = e0;
        }
        last_null = 0;
        for (int g = 1; g <= m.n_groups; g++) {              /* captures (non-participating omitted) */
            if (m.starts[g] < 0) continue;
            CHECK(korb_re_split_emit(c, slots + 5, res, &slots[0], m.starts[g], m.ends[g] - m.starts[g], &empty_count));
        }
        if (limit > 0 && limit <= ++i) break;
    }
    const long len = (long)VAL2STR(slots[0])->len;          /* trailing field */
    if (len > 0 && (limit != 0 || len > beg))
        CHECK(korb_re_split_emit(c, slots + 5, res, &slots[0], beg, len - beg, &empty_count));
    return RESULT_OK(KORB_REF_GET(res));
}

/* ---- String#[] / index with a Regexp (forward-declared) ------------------ */
/* Byte span [*bs, *be) of a Regexp match on self (optional capture group by
 * index or name); sets $~ and *found. Used by String#[]= and String#slice!.
 * `write` (String#[]=) reports every miss as the IndexError CRuby raises there;
 * a read just leaves *found false. */
RESULT korb_re_str_span(CTX *c, korb_value *slots, korb_ref self, korb_value re, korb_value group_or_nil, bool *found, uint32_t *bs, uint32_t *be, bool write) {
    /* the capture argument outlives the match run (which allocates), so it is
     * parked in a slot rather than kept in a C local */
    slots[0] = KORB_REF_GET(self); slots[1] = re; slots[2] = group_or_nil; korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 3, slots[1], slots[0], 0, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) {
        korb_re_set_lastmatch(c, KORB_NIL); *found = false;
        if (write) return korb_raise(c, slots, KORB_E_INDEX, 0, "regexp not matched");
        return RESULT_OK(KORB_NIL);
    }
    slots[3] = UNWRAP(korb_re_build_md(c, slots + 3, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[3]);
    int gi = 0;
    if (slots[2] != KORB_NIL) {
        if (SYMBOL_P(slots[2]) || KORB_STRING_P(slots[2])) {
            const char *nm; uint32_t nl;
            if (SYMBOL_P(slots[2])) { nm = korb_sym_name(c->vm, SYM2ID(slots[2])); nl = (uint32_t)strlen(nm); } else { nm = korb_str_ptr(VAL2STR(slots[2])); nl = VAL2STR(slots[2])->len; }
            gi = korb_md_name_idx(c, slots[3], nm, nl);
            if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        } else {
            korb_value gv = slots[2];
            const char *const on = korb_coerce_name(c, gv);
            RESULT cr = korb_coerce_to_int(c, slots + 4, &gv);   /* self/re/group/md stay rooted in slots[0..3] */
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value == KORB_FALSE)
                return korb_raise(c, slots + 4, KORB_E_TYPE, 0, "can't convert %s into Integer (%s#to_int gives %s)", on, on, korb_coerce_name(c, gv));
            korb_sword_t g = 0; korb_to_index(gv, &g);
            /* a negative capture index counts back from the last group; -n is out
             * of range as soon as n reaches the group count (group 0 included). */
            const int ngroups = m.n_groups + 1;
            if (g < 0 ? -g >= ngroups : g >= ngroups) {
                *found = false;
                if (write) return korb_raise(c, slots + 4, KORB_E_INDEX, 0, "index %ld out of regexp", (long)g);
                return RESULT_OK(KORB_NIL);
            }
            gi = (int)(g < 0 ? g + ngroups : g);
        }
    }
    if (m.starts[gi] < 0) {                            /* the group did not participate in the match */
        *found = false;
        if (write) return korb_raise(c, slots, KORB_E_INDEX, 0, "regexp group %d not matched", gi);
        return RESULT_OK(KORB_NIL);
    }
    *bs = (uint32_t)m.starts[gi]; *be = (uint32_t)m.ends[gi]; *found = true;
    return RESULT_OK(KORB_TRUE);
}
RESULT korb_re_str_aref(CTX *c, korb_value *slots, korb_ref self, korb_value re, korb_value group_or_nil) {
    slots[0] = KORB_REF_GET(self); slots[1] = re; slots[2] = group_or_nil; korb_re_match_t m;   /* group arg: rooted across the match run */
    RESULT rr = korb_re_run(c, slots + 3, slots[1], slots[0], 0, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[3] = UNWRAP(korb_re_build_md(c, slots + 3, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[3]);
    int gi = 0;
    if (slots[2] != KORB_UNDEF) {
        if (SYMBOL_P(slots[2]) || KORB_STRING_P(slots[2])) {
            const char *nm; uint32_t nl;
            if (SYMBOL_P(slots[2])) { nm = korb_sym_name(c->vm, SYM2ID(slots[2])); nl = (uint32_t)strlen(nm); } else { nm = korb_str_ptr(VAL2STR(slots[2])); nl = VAL2STR(slots[2])->len; }
            gi = korb_md_name_idx(c, slots[3], nm, nl);
            if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        } else {
            korb_sword_t g = 0;
            if (!korb_to_index(slots[2], &g)) {          /* #to_int coercion; nil/other → TypeError */
                if (slots[2] == KORB_NIL)
                    return korb_raise(c, slots + 4, KORB_E_TYPE, 0, "no implicit conversion from nil to integer");
                korb_value gv = slots[2];
                RESULT cr = korb_coerce_to_int(c, slots + 4, &gv);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                if (!korb_to_index(gv, &g))
                    return korb_raise_no_int(c, slots + 4, slots[2]);
            }
            if (g < 0) {                                  /* counts back from the last group; can't reach 0 */
                g += korb_md_ngroups(VAL2MD(slots[3]));
                if (g <= 0) return RESULT_OK(KORB_NIL);
            }
            gi = (int)g;
        }
    }
    return korb_md_group(c, slots + 4, slots[3], gi);
}
RESULT korb_re_str_index(CTX *c, korb_value *slots, korb_ref self, korb_value re, long startc, bool bytes) {
    slots[0] = KORB_REF_GET(self); slots[1] = re;
    CHECK(korb_re_check_subject(c, slots + 2, slots[0]));
    const KorbString *s = VAL2STR(slots[0]);
    const long ncp = bytes ? (long)s->len : korb_re_ncp(c->vm, slots[0]);
    if (startc < 0) startc += ncp;
    if (startc < 0 || startc > ncp) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    size_t startb = bytes ? (size_t)startc : korb_re_startb(c->vm, slots[0], startc);
    korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[2]);
    return RESULT_OK(LONG2FIX(bytes ? m.starts[0] : korb_re_bchar(c->vm, VAL2STR(slots[0]), m.starts[0])));
}
/* rindex/byterindex with a Regexp: last match starting at position <= stop
 * (char position for rindex, byte for byterindex). Considers overlapping starts. */
RESULT korb_re_str_rindex(CTX *c, korb_value *slots, korb_ref self, korb_value re, long stop, bool bytes, bool have_stop) {
    slots[0] = KORB_REF_GET(self); slots[1] = re;
    const KorbString *s0 = VAL2STR(slots[0]); const long slen = (long)s0->len;
    const long ncp = bytes ? slen : korb_re_ncp(c->vm, slots[0]);
    if (!have_stop) stop = ncp;
    if (stop < 0) stop += ncp;
    if (stop < 0) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    const long stopb = bytes ? stop : (stop >= ncp ? slen : (long)korb_re_startb(c->vm, slots[0], stop));
    korb_re_match_t last_m; bool have = false; long off = 0;
    {   /* `\G` is where the search starts — the limit, for a backward search (CRuby): prelude's __rindex_g */
        const KorbString *const src = KORB_STRING_P(VAL2RE(slots[1])->source) ? VAL2STR(VAL2RE(slots[1])->source) : NULL;
        if (src != NULL && korb_byte_find(korb_str_ptr(src), src->len, "\\G", 2) >= 0) {
            slots[2] = slots[0]; slots[3] = slots[1]; slots[4] = LONG2FIX(stopb); slots[5] = bytes ? KORB_TRUE : KORB_FALSE;
            const RESULT mr = korb_send(c, slots + 6, korb_intern_lit(c->vm, "__rindex_g", 10), 0, 3);   /* [pos, md] | nil */
            if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
            if (!KORB_ARRAY_P(mr.value) || VAL2ARY(mr.value)->len != 2) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
            korb_re_set_lastmatch(c, korb_ary_ptr(VAL2ARY(mr.value))[1]);
            return RESULT_OK(korb_ary_ptr(VAL2ARY(mr.value))[0]);
        }
    }
    while (off <= slen) {
        korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], (size_t)off, &m);
        if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
        if (rr.value != KORB_TRUE) break;
        if (m.starts[0] > stopb) break;                  /* start past the limit → later ones are too */
        korb_re_match_copy(&last_m, &m); have = true;
        off = m.starts[0] + 1;                            /* allow overlapping starts (progress: starts[0] >= off) */
    }
    if (!have) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[0], slots[1], &last_m)); korb_re_set_lastmatch(c, slots[2]);
    return RESULT_OK(LONG2FIX(bytes ? last_m.starts[0] : korb_re_bchar(c->vm, VAL2STR(slots[0]), last_m.starts[0])));
}

/* ---- Regexp class methods ------------------------------------------------ */
/* One ASCII byte of Regexp.escape into out[0..1]; returns the length. */
static uint32_t korb_re_escape_ascii(char *const out, const unsigned char ch) {
    char e = 0;
    switch (ch) {
      case '\n': e = 'n'; break;
      case '\r': e = 'r'; break;
      case '\t': e = 't'; break;
      case '\f': e = 'f'; break;
      case '\v': e = 'v'; break;
      case ' ':  e = ' '; break;
      default:
        if (ch != 0 && strchr("\\.*+?()[]{}|-^$#", ch)) e = (char)ch;   /* `#` too: it would start #{ } */
    }
    if (e) { out[0] = '\\'; out[1] = e; return 2; }
    out[0] = (char)ch; return 1;
}
/* One byte of Regexp.escape (also a String part of Regexp.union). */
static void korb_re_escape_byte(FILE *const ms, const unsigned char ch) {
    char e[2];
    fwrite(e, 1, korb_re_escape_ascii(e, ch), ms);
}
/* Regexp.escape of a UTF-16/32 string: the escapes are characters in that
 * encoding (a lone '\\' byte would split a code unit). */
static void korb_re_escape_wide(FILE *const ms, const unsigned char *const p, const uint32_t n,
                                const uint32_t unit, const bool be) {
    for (uint32_t i = 0; i + unit <= n; i += unit) {
        const uint32_t cu = korb_re_wide_unit_at(p + i, unit, be);
        if (cu >= 0x80) { fwrite(p + i, 1, unit, ms); continue; }
        char e[2]; const uint32_t el = korb_re_escape_ascii(e, (unsigned char)cu);
        for (uint32_t k = 0; k < el; k++) {
            char w[4] = {0};
            w[be ? unit - 1 : 0] = e[k];
            fwrite(w, 1, unit, ms);
        }
    }
    if (n % unit) fwrite(p + n - n % unit, 1, n % unit, ms);
}
static RESULT korb_m_re_escape(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)self; korb_value v = KORB_SLICE_GET(a, 0);
    if (SYMBOL_P(v)) {
        const uint32_t id = SYM2ID(v);
        slots[0] = UNWRAP(korb_str_new(c, slots, korb_sym_name(c->vm, id), c->vm->sym_lens[id]));
        KORB_STR_ENC_SET(slots[0], c->vm->sym_encs[id]);
    }
    else if (KORB_STRING_P(v)) slots[0] = v;
    else if (KORB_OBJECT_P(v) && korb_responds_to_coerce_p(c, slots, &v, korb_intern_lit(c->vm, "to_str", 6))) {   /* parslet's Slice */
        slots[0] = v;
        slots[0] = UNWRAP(korb_send_impl(c, slots + 1, korb_intern_lit(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL));
        if (!KORB_STRING_P(slots[0]))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into String (%s#to_str gives %s)",
                              korb_re_arg_type(KORB_SLICE_GET(a, 0)), korb_re_arg_type(KORB_SLICE_GET(a, 0)), korb_re_arg_type(slots[0]));
    }
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(v));
    const KorbString *s = VAL2STR(slots[0]);
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    bool wbe = false;
    const uint32_t wunit = korb_re_enc_unit(c->vm, KORB_STR_ENC(slots[0]), &wbe);
    if (UNLIKELY(wunit)) korb_re_escape_wide(ms, (const unsigned char *)korb_str_ptr(s), s->len, wunit, wbe);
    else for (uint32_t i = 0; i < s->len; i++) korb_re_escape_byte(ms, (unsigned char)korb_str_ptr(s)[i]);
    fclose(ms);
    /* The escape keeps the source's encoding, except that an all-ASCII input
     * yields US-ASCII whatever it was tagged with (CRuby). */
    const uint32_t senc = korb_str_ascii_only_p(c->vm, slots[0])
                        ? KORB_ENC_USASCII : KORB_STR_ENC(slots[0]);
    RESULT r = korb_str_new(c, slots + 1, buf ? buf : "", (uint32_t)z); free(buf);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, senc);
    return r;
}
/* Regexp.new of a UTF-16/32 source: it is matched as UTF-8, so validate that
 * form.  1 ok, 0 engine rejects, -1 malformed, 2 not UTF-16/32 (caller validates). */
static __attribute__((noinline)) int korb_re_valid_wide(const struct korb_vm *const vm, const korb_re_valid_fn_t vf,
                                                        const korb_value src, const uint32_t flags) {
    bool be = false;
    const uint32_t unit = korb_re_enc_unit(vm, KORB_STR_ENC(src), &be);
    if (unit == 0) return 2;
    const KorbString *const ps = VAL2STR(src);
    korb_re_wide_buf pw;
    if (!korb_re_wide_decode((const unsigned char *)korb_str_ptr(ps), ps->len, unit, be, false, &pw)) return -1;
    const bool ok = vf(pw.u8, pw.len, flags);
    free(pw.u8);
    return ok ? 1 : 0;
}
/* An interpolated literal's pattern is checked when it is built, as Regexp.new does (`/#{"("}/`
 * raises RegexpError there, not at the first match).  `src` must be rooted by the caller. */
RESULT korb_re_validate_pub(CTX *c, korb_value *slots, korb_value src, uint32_t flags) {
    (void)korb_re_load(c->vm);
    const korb_re_valid_fn_t vf = (korb_re_valid_fn_t)c->vm->re_valid_fn;
    if (!vf || !KORB_STRING_P(src)) return RESULT_OK(KORB_NIL);
    const KorbString *const ps = VAL2STR(src);
    const int wv = UNLIKELY(KORB_STR_ENC(src) >= KORB_ENC_OTHER_MIN) ? korb_re_valid_wide(c->vm, vf, src, flags) : 2;
    const uint32_t se = KORB_STR_ENC(src);
    const unsigned vflags = ((se == KORB_ENC_BINARY || (se >= KORB_ENC_OTHER_MIN && KORB_ENC_SB(c->vm, se))) &&
                             !korb_str_bytes_ascii(src)) ? (flags | 128u) : flags;
    if (UNLIKELY(wv < 0)) return korb_raise(c, slots, KORB_E_REGEXP, 0, "invalid multibyte character");
    if (wv == 0 || (wv == 2 && !vf(korb_str_ptr(ps), ps->len, vflags))) {
        char why[288]; const char *const m = korb_re_error(c->vm);
        snprintf(why, sizeof why, "%s: /%.*s/", m ? m : "invalid regular expression", (int)ps->len, korb_str_ptr(ps));
        return korb_raise(c, slots, KORB_E_REGEXP, 0, "%s", why);
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_re_new(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (UNLIKELY(korb_sub_overrides_init_p(c, KORB_REF_GET(self),
                                           korb_builtin_class_obj(c->vm, KORB_C_REGEXP)))) {
        /* Regexp subclass with its own #initialize: give it an uninitialized
         * payload and let `super` (the prelude's #initialize → #__init_pattern)
         * fill in the source and flags, as CRuby's allocate + #initialize does. */
        slots[0] = KORB_REF_GET(self);                    /* root the subclass */
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, "", 0));
        slots[1] = UNWRAP(korb_re_alloc(c, slots + 2, slots[1], KORB_RE_UNINIT));
        ((AroObjectHeader *)(uintptr_t)slots[1])->flags |= KORB_FL_HAS_KLASS;
        ((AroObjectHeader *)(uintptr_t)slots[1])->flags &= ~(uint32_t)KORB_FL_FROZEN;   /* only a plain Regexp is frozen (CRuby re.c) */
        korb_klass_override_set(c, slots[1], slots[0]);    /* both rooted; set does not GC */
        return korb_sub_new_init(c, slots + 2, KORB_REF_AT(&slots[1]), a, NULL, NULL, NULL);
    }
    korb_value src = KORB_SLICE_GET(a, 0); uint32_t flags = 0;
    bool from_regexp = false;
    if (KORB_REGEXP_P(src)) {
        if (UNLIKELY(VAL2RE(src)->flags & KORB_RE_UNINIT)) return korb_raise(c, slots, KORB_E_TYPE, 0, "uninitialized Regexp");
        slots[0] = VAL2RE(src)->source; flags = VAL2RE(src)->flags; from_regexp = true;
    }
    else if (KORB_STRING_P(src)) slots[0] = src;
    else {                                                /* #to_str coercion, else TypeError */
        slots[0] = src;
        if (korb_responds_to(c, src, korb_intern_lit(c->vm, "to_str", 6))) {
            RESULT r = korb_send_impl(c, slots + 1, korb_intern_lit(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (!KORB_STRING_P(r.value)) {   /* CRuby names the method that misbehaved, not the conversion */
                const char *const cn = korb_coerce_name(c, slots[0]);
                return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into String (%s#to_str gives %s)",
                                  cn, cn, korb_coerce_name(c, r.value));
            }
            slots[0] = r.value;
        } else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, src));
    }
    korb_value timeout_v = KORB_NIL;                           /* Regexp.new(src, timeout:) */
    /* A Regexp first argument carries its own options; any 2nd/3rd arg is ignored (with a warning, as in CRuby). */
    if (from_regexp && KORB_SLICE_LEN(a) >= 2 && KORB_SLICE_GET(a, 1) != KORB_NIL)
        korb_warn_caller(c, slots + 1, a.p + KORB_FLINK_OFF, "flags ignored");
    if (!from_regexp && KORB_SLICE_LEN(a) >= 2) {
        korb_value opt = KORB_SLICE_GET(a, 1);
        if (opt == KORB_NIL || opt == KORB_FALSE) {}
        else if (opt == KORB_TRUE) flags |= 4u;           /* the documented boolean form: no warning */
        else if (FIXNUM_P(opt)) { const long o = FIX2LONG(opt);
            if (o & 1)  flags |= 4u;
            if (o & 2)  flags |= 8u;
            if (o & 4)  flags |= 16u;
            if (o & 16) flags |= KORB_RE_FIXENC;
            if (o & 32) flags |= 128u;
        }
        else if (KORB_STRING_P(opt)) {                    /* a String of flag chars: i/m/x */
            const KorbString *fs = VAL2STR(opt);
            for (uint32_t k = 0; k < fs->len; k++) {
                switch (korb_str_ptr(fs)[k]) {
                  case 'i': flags |= 4u;  break;          /* IGNORECASE */
                  case 'm': flags |= 16u; break;          /* MULTILINE */
                  case 'x': flags |= 8u;  break;          /* EXTENDED */
                  default:                            /* CRuby names the whole option string, not the offending char */
                    return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unknown regexp option: %.*s", (int)fs->len, korb_str_ptr(fs));
                }
            }
        }
        else if (KORB_HASH_P(opt) && (((const AroObjectHeader *)(uintptr_t)opt)->flags & KORB_FL_KWARGS)) {
            /* Regexp.new(src, options = 0, timeout: nil): the keyword arrives as
             * a kwargs Hash, not as the positional `options`. */
            const KorbHash *const oh = VAL2HASH(opt);
            for (uint32_t k = 0; k < oh->len; k++) {
                const korb_value key = korb_hash_pairs(oh)[2 * k];
                if (SYMBOL_P(key) && SYM2ID(key) == korb_intern_lit(c->vm, "timeout", 7)) continue;
                slots[1] = key;
                return korb_raise(c, slots + 2, KORB_E_ARGUMENT, 0, "unknown keyword: :%s",
                                  SYMBOL_P(key) ? korb_sym_name(c->vm, SYM2ID(key)) : "?");
            }
            const int32_t ti = korb_hash_find(VAL2HASH(opt), ID2SYM(korb_intern_lit(c->vm, "timeout", 7)));
            if (ti >= 0) timeout_v = korb_hash_pairs(VAL2HASH(opt))[2 * ti + 1];
        }
        else {
            flags |= 4u;                                  /* any other truthy value → IGNORECASE, with CRuby's warning */
            slots[1] = opt;                               /* CRuby names the value itself (its #inspect) */
            char *ib = NULL; size_t iz = 0;
            FILE *const ims = open_memstream(&ib, &iz);
            if (ims) { korb_fprint_inspect_s(c, slots + 2, ims, slots[1]); fclose(ims); }
            korb_warn_caller(c, slots + 2, a.p + KORB_FLINK_OFF, "expected true or false as ignorecase: %s", ib ? ib : "");
            free(ib);
        }
    }
    /* CRuby tags a 7-bit ASCII pattern US-ASCII, and #source reports that — so
     * the pattern String cannot be shared with the caller's. */
    if (!(flags & (64u | 128u | 256u | 512u | KORB_RE_FIXENC)) &&   /* /n /u /e /s pin their own */
        korb_enc_ascii_compat_idx(c->vm, KORB_STR_ENC(slots[0])) &&  /* UTF-16LE etc. keep theirs */
        korb_str_bytes_ascii(slots[0]) && KORB_STR_ENC(slots[0]) != KORB_ENC_USASCII) {
        const RESULT dr = korb_str_dup_pub(c, slots + 1, &slots[0]);
        if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
        slots[0] = dr.value;
        KORB_STR_ENC_SET(slots[0], KORB_ENC_USASCII);
    }
    (void)korb_re_load(c->vm);
    korb_re_valid_fn_t vf0 = (korb_re_valid_fn_t)c->vm->re_valid_fn;
    korb_re_valid_fn_t vf = vf0;
    if (vf) {
        const KorbString *ps = VAL2STR(slots[0]);
        const int wv = UNLIKELY(KORB_STR_ENC(slots[0]) >= KORB_ENC_OTHER_MIN) ? korb_re_valid_wide(c->vm, vf, slots[0], flags) : 2;
        const uint32_t se = KORB_STR_ENC(slots[0]);
        const unsigned vflags = ((se == KORB_ENC_BINARY || (se >= KORB_ENC_OTHER_MIN && KORB_ENC_SB(c->vm, se))) &&
                                 !korb_str_bytes_ascii(slots[0])) ? (flags | 128u) : flags;   /* see korb_re_pat_bytewise_p */
        if (UNLIKELY(wv < 0)) return korb_raise(c, slots, KORB_E_REGEXP, 0, "invalid multibyte character");
        if (wv == 0 || (wv == 2 && !vf(korb_str_ptr(ps), ps->len, vflags))) {
            /* the engine's own reason, quoted like CRuby ("<why>: /<pattern>/") */
            char why[288]; const char *const m = korb_re_error(c->vm);
            const KorbString *const ps2 = VAL2STR(slots[0]);
            snprintf(why, sizeof why, "%s: /%.*s/", m ? m : "invalid regular expression",
                     (int)ps2->len, korb_str_ptr(ps2));
            return korb_raise(c, slots, KORB_E_REGEXP, 0, "%s", why);
        }
    }
    const RESULT rr = korb_re_alloc(c, slots + 1, slots[0], flags);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (timeout_v != KORB_NIL)                            /* no alloc between here and the store */
        ARO_STORE(c, (KorbRegexp *)(uintptr_t)rr.value, &VAL2RE(rr.value)->timeout, timeout_v);
    /* Regexp subclass: tag the instance with the receiver class (the same
     * side-table route Range/String/Array subclasses take). */
    if (KORB_CLASS_P(KORB_REF_GET(self)) &&
        KORB_REF_GET(self) != korb_builtin_class_obj(c->vm, KORB_C_REGEXP)) {
        slots[1] = rr.value;
        slots[2] = KORB_REF_GET(self);
        ((AroObjectHeader *)(uintptr_t)slots[1])->flags |= KORB_FL_HAS_KLASS;
        ((AroObjectHeader *)(uintptr_t)slots[1])->flags &= ~(uint32_t)KORB_FL_FROZEN;   /* only a plain Regexp is frozen (CRuby re.c) */
        korb_klass_override_set(c, slots[1], slots[2]);   /* both rooted; set does not GC */
        return RESULT_OK(slots[1]);
    }
    return rr;
}
/* Fill the payload `new` allocated for a Regexp subclass (KORB_RE_UNINIT).  A
 * Regexp's whole state is source + flags, so a throwaway built by the normal
 * constructor supplies both; an already-built Regexp is CRuby's TypeError.
 * Reached only as `super` from a subclass's #initialize (see the prelude). */
static RESULT korb_m_re_init_pattern(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (UNLIKELY(!KORB_REGEXP_P(KORB_REF_GET(self)) ||
                 !(VAL2RE(KORB_REF_GET(self))->flags & KORB_RE_UNINIT)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "already initialized regexp");
    if (UNLIKELY(KORB_SLICE_LEN(a) < 1 || KORB_SLICE_LEN(a) > 3))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 1..3)",
                          KORB_SLICE_LEN(a));
    slots[0] = korb_builtin_class_obj(c->vm, KORB_C_REGEXP);   /* the throwaway is a plain Regexp */
    const RESULT dr = korb_m_re_new(c, slots + 1, KORB_REF_AT(&slots[0]), a);
    if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
    slots[1] = dr.value;
    KorbRegexp *const me = VAL2RE(KORB_REF_GET(self));        /* re-read: korb_m_re_new allocates */
    const KorbRegexp *const built = VAL2RE(slots[1]);
    me->flags = built->flags;                                  /* clears KORB_RE_UNINIT */
    me->ci    = built->ci;
    me->to_s  = KORB_NIL;
    ARO_STORE(c, me, (korb_value *)(uintptr_t)&me->source, built->source);
    if (!(me->head.flags & KORB_FL_HAS_KLASS)) me->head.flags |= KORB_FL_FROZEN;   /* a plain Regexp (Regexp.allocate) freezes (CRuby) */
    return RESULT_OK(KORB_REF_GET(self));
}

/* the name of a header encoding index, for the union's error message */
static const char *korb_enc_idx_name(const struct korb_vm *vm, uint32_t idx) {
    if (idx == KORB_ENC_UTF8) return "UTF-8";
    if (idx == KORB_ENC_USASCII) return "US-ASCII";
    if (idx == KORB_ENC_BINARY) return "ASCII-8BIT";
    return (idx < KORB_STR_ENC_MAX && vm->str_enc_names[idx]) ? korb_sym_name(vm, vm->str_enc_names[idx]) : "unknown";
}
/* the UTF-16/32, UTF-7 and stateful families are not ASCII-compatible */
static bool korb_re_enc_ascii_compat(const struct korb_vm *vm, uint32_t idx) {
    if (idx < KORB_ENC_OTHER_MIN || idx >= KORB_STR_ENC_MAX || vm->str_enc_names[idx] == 0) return true;
    const char *const nm = korb_sym_name(vm, vm->str_enc_names[idx]);
    return !(strncasecmp(nm, "UTF-16", 6) == 0 || strncasecmp(nm, "UTF-32", 6) == 0 ||
             strcasecmp(nm, "UTF-7") == 0 || strncasecmp(nm, "ISO-2022", 8) == 0 ||
             strncasecmp(nm, "CP502", 5) == 0);
}
/* a Regexp part's encoding index, via its own #encoding (the prelude knows the
 * /n /u /e /s rules).  false when it cannot be determined. */
static bool korb_re_part_enc(CTX *c, korb_value *slots, korb_ref reref, uint32_t *idx, bool *fixed) {
    /* the Regexp is read from the caller's scanned slot each time: the dispatches
     * below allocate, and a raw korb_value would go stale under the moving GC */
    slots[0] = KORB_REF_GET(reref);
    const RESULT er = korb_send(c, slots + 1, korb_intern_lit(c->vm, "encoding", 8), 0, 0);
    if (er.state != KORB_NORMAL || er.value == KORB_NIL) return false;
    slots[0] = er.value;
    const RESULT nr = korb_send(c, slots + 1, korb_intern_lit(c->vm, "name", 4), 0, 0);
    if (nr.state != KORB_NORMAL || !KORB_STRING_P(nr.value)) return false;
    slots[0] = nr.value;
    char nb[64];                                   /* the registry takes a NUL-terminated name */
    const KorbString *const ns = VAL2STR(slots[0]);
    uint32_t n = ns->len < sizeof nb - 1 ? ns->len : (uint32_t)(sizeof nb - 1);
    memcpy(nb, korb_str_ptr(ns), n); nb[n] = '\0';
    *idx = korb_enc_index_for_name(c->vm, nb);
    slots[0] = KORB_REF_GET(reref);
    const RESULT fr = korb_send(c, slots + 1, korb_intern_lit(c->vm, "fixed_encoding?", 15), 0, 0);
    *fixed = (fr.state == KORB_NORMAL) && KORB_TRUTHY(fr.value);
    return true;
}
static RESULT korb_m_re_union(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)self;
    slots[0] = KORB_NIL;
    slots[3] = KORB_NIL;                                 /* the Array form's element list (parked) */
    slots[4] = KORB_NIL;                                 /* the part being appended (parked: we dispatch) */
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    uint32_t n = KORB_SLICE_LEN(a);
    if (n == 1 && KORB_ARRAY_P(KORB_SLICE_GET(a, 0))) { slots[3] = KORB_SLICE_GET(a, 0); n = VAL2ARY(slots[3])->len; }
    #define KORB_UNION_ITEM(i) (slots[3] != KORB_NIL ? korb_ary_ptr(VAL2ARY(slots[3]))[(i)] : KORB_SLICE_GET(a, (i)))
    if (n == 0) fputs("(?!)", ms);
    if (n == 1) {                                       /* a single Regexp argument IS the union (CRuby) */
        slots[4] = KORB_UNION_ITEM(0);
        if (KORB_REGEXP_P(slots[4])) { fclose(ms); free(buf); return RESULT_OK(slots[4]); }
        if (!KORB_STRING_P(slots[4]) && korb_responds_to(c, slots[4], korb_intern_lit(c->vm, "to_regexp", 9))) {
            const RESULT rr = korb_send(c, slots + 5, korb_intern_lit(c->vm, "to_regexp", 9), 0, 0);
            if (UNLIKELY(rr.state != KORB_NORMAL)) { fclose(ms); free(buf); return rr; }
            if (KORB_REGEXP_P(rr.value)) { fclose(ms); free(buf); return RESULT_OK(rr.value); }
        }
    }
    /* rb_reg_s_union's three buckets: an ASCII-incompatible part, an
     * ASCII-compatible part that pins its encoding, and plain ASCII-only parts. */
    uint32_t incompat = UINT32_MAX, cfixed = UINT32_MAX;
    bool asciionly = false;
    #define KORB_UNION_FAIL(...) do { fclose(ms); free(buf); \
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, __VA_ARGS__); } while (0)
    for (uint32_t i = 0; i < n; i++) {
        slots[4] = KORB_UNION_ITEM(i);
        if (i) fputc('|', ms);
        uint32_t e = UINT32_MAX; bool part_fixed = false, part_asciionly = false;
        if (!KORB_REGEXP_P(slots[4]) && !KORB_STRING_P(slots[4]) && !SYMBOL_P(slots[4]) &&
            korb_responds_to(c, slots[4], korb_intern_lit(c->vm, "to_regexp", 9))) {
            const RESULT rr = korb_send(c, slots + 5, korb_intern_lit(c->vm, "to_regexp", 9), 0, 0);   /* rb_check_regexp_type */
            if (UNLIKELY(rr.state != KORB_NORMAL)) { fclose(ms); free(buf); return rr; }
            if (UNLIKELY(!KORB_REGEXP_P(rr.value))) {
                fclose(ms); free(buf);
                return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Regexp", korb_re_arg_type(slots[4]));
            }
            slots[4] = rr.value;
        }
        if (KORB_REGEXP_P(slots[4])) {
            if (!korb_re_part_enc(c, slots + 5, KORB_REF_AT(&slots[4]), &e, &part_fixed)) e = KORB_ENC_USASCII;
            korb_re_write_to_s(c->vm, ms, slots[4]);            /* after the dispatches: slots[4] is re-read */
            part_asciionly = !part_fixed;
        } else if (KORB_STRING_P(slots[4]) || SYMBOL_P(slots[4]) ||
                   korb_responds_to(c, slots[4], korb_intern_lit(c->vm, "to_str", 6))) {
            if (SYMBOL_P(slots[4])) {                    /* a Symbol unions as its name */
                const char *const sn = korb_sym_name(c->vm, SYM2ID(slots[4]));
                const RESULT sr = korb_str_new(c, slots + 5, sn, (uint32_t)strlen(sn));
                if (UNLIKELY(sr.state != KORB_NORMAL)) { fclose(ms); free(buf); return sr; }
                slots[4] = sr.value;
            } else if (!KORB_STRING_P(slots[4])) {       /* #to_str coercion (CRuby's StringValue) */
                const RESULT sr = korb_send(c, slots + 5, korb_intern_lit(c->vm, "to_str", 6), 0, 0);
                if (UNLIKELY(sr.state != KORB_NORMAL)) { fclose(ms); free(buf); return sr; }
                if (UNLIKELY(!KORB_STRING_P(sr.value))) {
                    fclose(ms); free(buf);
                    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String",
                                      korb_re_arg_type(slots[4]));
                }
                slots[4] = sr.value;
            }
            const KorbString *const s2 = VAL2STR(slots[4]);   /* no dispatch below: the borrow is safe */
            bool nonascii = false, pbe = false;
            korb_re_wide_buf pw;
            if (UNLIKELY(korb_re_enc_unit(c->vm, KORB_STR_ENC(slots[4]), &pbe)) &&   /* escaped as UTF-8, re-encoded below */
                korb_re_wide_decode((const unsigned char *)korb_str_ptr(s2), s2->len,
                                    korb_re_enc_unit(c->vm, KORB_STR_ENC(slots[4]), &pbe), pbe, false, &pw)) {
                for (uint32_t j = 0; j < pw.len; j++) {
                    const unsigned char ch = (unsigned char)pw.u8[j];
                    if (ch >= 0x80) fputc(ch, ms); else korb_re_escape_byte(ms, ch);
                }
                free(pw.u8);
            } else
            for (uint32_t j = 0; j < s2->len; j++) {
                const unsigned char ch = (unsigned char)korb_str_ptr(s2)[j];
                if (ch >= 0x80) nonascii = true;
                korb_re_escape_byte(ms, ch);
            }
            e = KORB_STR_ENC(slots[4]);
            part_fixed = nonascii;                       /* a non-ASCII String pins its encoding */
            part_asciionly = !nonascii;
        } else {
            fclose(ms); free(buf);
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String",
                              korb_re_arg_type(slots[4]));
        }
        if (!korb_re_enc_ascii_compat(c->vm, e)) {
            if (incompat == UINT32_MAX) incompat = e;
            else if (incompat != e)
                KORB_UNION_FAIL("incompatible encodings: %s and %s",
                                korb_enc_idx_name(c->vm, incompat), korb_enc_idx_name(c->vm, e));
        } else if (part_fixed) {
            if (cfixed == UINT32_MAX) cfixed = e;
            else if (cfixed != e)
                KORB_UNION_FAIL("incompatible encodings: %s and %s",
                                korb_enc_idx_name(c->vm, cfixed), korb_enc_idx_name(c->vm, e));
        } else if (part_asciionly) asciionly = true;
        if (incompat != UINT32_MAX) {
            if (asciionly) KORB_UNION_FAIL("ASCII incompatible encoding: %s", korb_enc_idx_name(c->vm, incompat));
            if (cfixed != UINT32_MAX)
                KORB_UNION_FAIL("incompatible encodings: %s and %s",
                                korb_enc_idx_name(c->vm, incompat), korb_enc_idx_name(c->vm, cfixed));
        }
    }
    #undef KORB_UNION_FAIL
    #undef KORB_UNION_ITEM
    fclose(ms);
    const uint32_t renc = (incompat != UINT32_MAX) ? incompat : cfixed;
    bool wbe = false;
    const uint32_t wunit = renc != UINT32_MAX ? korb_re_enc_unit(c->vm, renc, &wbe) : 0;
    if (UNLIKELY(wunit)) {                               /* the parts were written as UTF-8 */
        char *wbuf = NULL; size_t wz = 0; FILE *const ws = open_memstream(&wbuf, &wz);
        korb_re_u8_to_wide(ws, buf ? buf : "", (uint32_t)z, wunit, wbe); fclose(ws); free(buf); buf = wbuf; z = wz;
    }
    slots[0] = UNWRAP(korb_str_new(c, slots, buf ? buf : "", (uint32_t)z)); free(buf);
    if (renc != UINT32_MAX) KORB_STR_ENC_SET(slots[0], renc);   /* the union carries the imposed encoding */
    return korb_re_alloc(c, slots + 1, slots[0], 0);      /* CRuby builds it from the source alone */
}
static RESULT korb_m_re_last_match(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)self; korb_value md = korb_re_get_lastmatch(c);
    if (KORB_SLICE_LEN(a) == 0) return RESULT_OK(md == 0 ? KORB_NIL : md);
    if (md == 0 || md == KORB_NIL || !KORB_MATCHDATA_P(md)) return RESULT_OK(KORB_NIL);
    slots[0] = md;
    const korb_value k = KORB_SLICE_GET(a, 0);
    if (SYMBOL_P(k) || KORB_STRING_P(k)) {                /* a name, as MatchData#[] takes one */
        const char *nm; uint32_t nl;
        if (SYMBOL_P(k)) { nm = korb_sym_name(c->vm, SYM2ID(k)); nl = (uint32_t)strlen(nm); }
        else { nm = korb_str_ptr(VAL2STR(k)); nl = VAL2STR(k)->len; }
        const int gi = korb_md_name_idx(c, slots[0], nm, nl);
        if (gi < 0) return korb_raise(c, slots + 1, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        return korb_md_group(c, slots + 1, slots[0], gi);
    }
    slots[1] = k;
    CHECK(korb_coerce_to_int_pub(c, slots + 2, &slots[1]));
    if (!FIXNUM_P(slots[1]))
        return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    korb_sword_t i = FIX2LONG(slots[1]);
    const int n = korb_md_ngroups(VAL2MD(slots[0]));
    if (i < 0) i += n;
    if (i < 0 || i >= n) return RESULT_OK(KORB_NIL);
    return korb_md_group(c, slots + 2, slots[0], (int)i);
}

/* ---- $~ backref accessor (node_backref) — kind: 0=$& 1=$` 2=$' 3=$+ 100+n=$n */
RESULT korb_backref(CTX *c, korb_value *slots, int kind) {
    korb_value md = korb_re_get_lastmatch(c);
    if (md == 0 || md == KORB_NIL || !KORB_MATCHDATA_P(md)) return RESULT_OK(KORB_NIL);
    if (kind >= 100) { slots[0] = md; return korb_md_group(c, slots, slots[0], kind - 100); }
    if (kind == 0)   { slots[0] = md; return korb_md_group(c, slots, slots[0], 0); }
    if (kind == 3) {                                      /* $+ : last group that matched */
        slots[0] = md; const int n = korb_md_ngroups(VAL2MD(slots[0]));
        for (int i = n - 1; i >= 1; i--) if (korb_md_off(VAL2MD(slots[0]), i, 0) >= 0) return korb_md_group(c, slots, slots[0], i);
        return RESULT_OK(KORB_NIL);
    }
    /* $` (pre-match) / $' (post-match): slice the *subject* (not the MatchData). */
    const long b0 = korb_md_off(VAL2MD(md), 0, 0), e0 = korb_md_off(VAL2MD(md), 0, 1);
    slots[0] = VAL2MD(md)->subject;                       /* root the subject across the slice alloc */
    const long slen = (long)VAL2STR(slots[0])->len;
    if (kind == 1) return korb_re_slice(c, slots + 1, &slots[0], 0, b0);
    if (kind == 2) return korb_re_slice(c, slots + 1, &slots[0], e0, slen);
    return RESULT_OK(KORB_NIL);
}

/* ---- registration -------------------------------------------------------- */
static void korb_init_regexp(CTX *c, korb_value *slots) {
    struct korb_vm *const vm = c->vm;
    slots[0] = korb_const_get(vm, korb_intern_lit(vm, "Regexp", 6));
    korb_const_define_owned(c, korb_intern_lit(vm, "IGNORECASE", 10), LONG2FIX(1), slots[0]);
    korb_const_define_owned(c, korb_intern_lit(vm, "EXTENDED",   8),  LONG2FIX(2), slots[0]);
    korb_const_define_owned(c, korb_intern_lit(vm, "MULTILINE",  9),  LONG2FIX(4), slots[0]);
    /* Encoding option bits: reported by #options, never set by koruby's engine. */
    korb_const_define_owned(c, korb_intern_lit(vm, "FIXEDENCODING", 13), LONG2FIX(16), slots[0]);
    korb_const_define_owned(c, korb_intern_lit(vm, "NOENCODING",    10), LONG2FIX(32), slots[0]);
    slots[1] = korb_obj_singleton(c, slots + 1, slots[0]).value;
    korb_class_def_cfn(c, slots[1], "escape", korb_m_re_escape, 1);
    korb_class_def_cfn(c, slots[1], "quote",  korb_m_re_escape, 1);
    korb_class_def_cfn(c, slots[1], "new",     korb_m_re_new, -1);
    korb_class_def_cfn(c, slots[1], "compile", korb_m_re_new, -1);
    korb_class_def_cfn(c, slots[1], "union",   korb_m_re_union, -1);
    korb_class_def_cfn(c, slots[1], "last_match", korb_m_re_last_match, -1);
    korb_class_def_cfn(c, slots[1], "__set_timeout", korb_m_re_set_timeout, 1);
}

/* Parse-time check for a Regexp LITERAL: CRuby rejects a bad pattern while
 * compiling (SyntaxError), not on first evaluation.  Returns NULL when the
 * engine accepts the pattern, else its reason. */
const char *
korb_re_literal_error(CTX *c, const char *pat, uint32_t len, uint32_t flags)
{
    (void)korb_re_load(c->vm);
    const korb_re_valid_fn_t vf = (korb_re_valid_fn_t)c->vm->re_valid_fn;
    if (vf == NULL || vf(pat, len, flags)) return NULL;
    const char *const m = korb_re_error(c->vm);
    return m ? m : "invalid regular expression";
}
/* Onigmo's compile-time warning for the literal korb_re_literal_error just
 * accepted (`a{0,1}+`: nested repeat operator), NULL if none. */
const char *
korb_re_literal_warning(CTX *c)
{
    return c->vm->re_warn_fn ? ((const char *(*)(void))c->vm->re_warn_fn)() : NULL;
}
