/* koruby_precise — regexp.c: Regexp + MatchData + String-regex methods.
 * #included into korb_runtime.c AFTER string.c (needs korb_utf8_* / korb_str_slice_new)
 * and set.c (needs korb_obj_singleton).
 *
 * Matching is delegated to astrogre via koruby_regex.so → libastrogre.so (see
 * regex_bridge.c) which returns per-group byte spans + named-capture info.  A
 * MatchData carries the subject, the source Regexp, and a KorbArray of Integer
 * byte offsets [b0,e0,b1,e1,...] (-1 for a group that didn't participate).  $~
 * (and thus $1..$9, $&, $`, $') is kept in the flat const table under "$~". */

static const char *korb_re_arg_type(VALUE v);   /* fwd (defined below) */
static const char *korb_enc_idx_name(const struct korb_vm *vm, uint32_t idx);   /* fwd */

/* Regexp::FIXEDENCODING passed to Regexp.new: no prism flag says "fixed to the
 * source's own encoding", so koruby carries it in a private bit. */
#define KORB_RE_FIXENC 0x10000u

/* The pattern's own encoding, for the match-time compatibility check.  Only the
 * cases the prelude's Regexp#encoding is sure about are reported; /e and /s name
 * encodings koruby never transcodes to, so they stay unknown and the caller
 * falls back to comparing against the source's encoding. */
static uint32_t korb_re_enc_idx(const struct korb_vm *vm, VALUE re, bool *out_fixed, bool *out_known)
{
    const uint32_t f = VAL2RE(re)->flags;
    const VALUE srcv = VAL2RE(re)->source;
    const uint32_t senc = KORB_STRING_P(srcv) ? KORB_STR_ENC(srcv) : KORB_ENC_BINARY;
    *out_known = true;
    if (f & 128u) {   /* /n is NOENCODING, never FIXEDENCODING */
        *out_fixed = false;
        return (KORB_STRING_P(srcv) && korb_str_ascii_only_p(vm, srcv)) ? KORB_ENC_USASCII : KORB_ENC_BINARY;
    }
    if (f & 512u) { *out_fixed = true; return KORB_ENC_UTF8; }          /* /u */
    if (f & (64u | 256u)) { *out_fixed = true; *out_known = false; return senc; }  /* /e, /s */
    *out_fixed = (f & KORB_RE_FIXENC) != 0 || !korb_enc_ascii_compat_idx(vm, senc);
    return senc;
}

/* True when the pattern pins an encoding, so a subject in a different one may
 * be a CompatibilityError.  An unpinned pattern adapts to any ASCII-compatible
 * subject, which is the overwhelmingly common case — keep it off the call. */
static inline bool korb_re_enc_pinned(VALUE re)
{
    const VALUE srcv = VAL2RE(re)->source;
    return (VAL2RE(re)->flags & (64u | 128u | 256u | 512u | KORB_RE_FIXENC)) != 0 ||
           (KORB_STRING_P(srcv) && KORB_STR_ENC(srcv) >= KORB_ENC_OTHER_MIN);
}

/* CRuby's rb_reg_prepare_enc: an encoding the pattern can't be read in is a
 * CompatibilityError.  Only the pattern's *explicitly* pinned encodings are
 * enforced, so an ordinary ASCII pattern still matches anything. */
static RESULT korb_re_check_enc(CTX *c, VALUE *slots, VALUE re, VALUE subj)
{
    const uint32_t senc = KORB_STR_ENC(subj);
    bool re_fixed = false, re_known = false;
    const uint32_t renc = korb_re_enc_idx(c->vm, re, &re_fixed, &re_known);

    /* Rule 2 of rb_reg_prepare_enc: an ASCII-incompatible subject (UTF-16 etc.)
     * cannot be searched at all, whatever the pattern is. */
    if (!korb_enc_ascii_compat_idx(c->vm, senc)) {
        return korb_raise_enc_compat(c, slots, renc, senc);
    } else if (re_fixed && re_known && renc != senc) {
        if (!korb_enc_ascii_compat_idx(c->vm, renc) || !korb_str_ascii_only_p(c->vm, subj))
            return korb_raise_enc_compat(c, slots, renc, senc);
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

/* ---- low-level engine call (no koruby alloc inside → subject bytes stable) */
static RESULT korb_re_run(CTX *c, VALUE *slots, VALUE re, VALUE subj, size_t startb, korb_re_match_t *m) {
    if (UNLIKELY(!KORB_REGEXP_P(re) || !KORB_STRING_P(subj))) return RESULT_OK(KORB_FALSE);
    const korb_re_exec_fn_t fn = korb_re_load(c->vm);
    if (UNLIKELY(fn == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Regexp engine (koruby_regex.so) unavailable");
    korb_re_sync_floor(c);   /* lazy first-load happens above; make sure the floor is set for THIS stack */
    /* Also gate on the SUBJECT: rule 2 (ASCII-incompatible subject) does not
     * depend on the pattern, so a plain ASCII regexp must reach the check too. */
    if (UNLIKELY(korb_re_enc_pinned(re) || KORB_STR_ENC(subj) >= KORB_ENC_OTHER_MIN))
        CHECK(korb_re_check_enc(c, slots, re, subj));
    const KorbString *const pat = VAL2STR(VAL2RE(re)->source), *const s = VAL2STR(subj);
    if (startb > s->len) { m->matched = 0; return RESULT_OK(KORB_FALSE); }
    /* Encoding: a /n regex, or a single-byte subject (BINARY / US-ASCII), matches
     * byte-wise (astrogre PR_FLAGS_ASCII_8BIT=128); UTF-8 subjects match by
     * codepoint.  So `.` on a binary string consumes one byte, not a codepoint. */
    unsigned eff_flags = VAL2RE(re)->flags;
    if (KORB_ENC_SB(c->vm, KORB_STR_ENC(subj))) eff_flags |= 128u;
    const int rc = fn(korb_strbuf_data(pat->buf), pat->len, eff_flags, korb_strbuf_data(s->buf), s->len, startb, m);
    if (UNLIKELY(rc == -2)) return korb_raise(c, slots, KORB_E_REGEXP, 0, "regexp match stack overflow");
    if (UNLIKELY(rc < 0)) {
        const char *const m = korb_re_error(c->vm);
        return korb_raise(c, slots, KORB_E_REGEXP, 0, "%s", m ? m : "invalid regular expression");
    }
    return RESULT_OK(rc == 1 ? KORB_TRUE : KORB_FALSE);
}

/* GC-safe byte-slice of the string rooted at *subjslot (re-reads after alloc).
 * b<0 → nil (unmatched group). */
static RESULT korb_re_slice(CTX *c, VALUE *slots, VALUE *subjslot, long b, long e) {
    if (b < 0) return RESULT_OK(KORB_NIL);
    return korb_str_slice_new(c, slots, VALUE_REF_AT(subjslot), (uint32_t)b, (uint32_t)(e - b));
}

/* byte offset → character index within `s`.  For single-byte encodings
 * (US-ASCII / ASCII-8BIT) a byte IS a character, so return the offset as-is
 * (else begin/end and StringScanner-style byte callers desync on multibyte). */
static long korb_re_bchar(const struct korb_vm *vm, const KorbString *s, long boff) {
    if (boff <= 0) return 0;
    if (boff >= (long)s->len) boff = (long)s->len;
    if (KORB_ENC_SB(vm, KORB_STR_ENC((VALUE)(uintptr_t)s))) return boff;
    return (long)korb_utf8_count(korb_strbuf_data(s->buf), (uint32_t)boff);
}

/* Build a MatchData from a completed match `m` over `subj` with source `re`. */
static RESULT korb_re_build_md(CTX *c, VALUE *slots, VALUE subj, VALUE re, const korb_re_match_t *m) {
    slots[0] = subj; slots[1] = re;
    const int ng = m->n_groups;
    slots[2] = UNWRAP(korb_ary_new(c, slots + 3, (uint32_t)(2 * (ng + 1))));
    VALUE_REF off = VALUE_REF_AT(&slots[2]);
    for (int i = 0; i <= ng; i++) {
        CHECK(korb_ary_push_val(c, slots + 3, off, LONG2FIX(m->starts[i])));
        CHECK(korb_ary_push_val(c, slots + 3, off, LONG2FIX(m->ends[i])));
    }
    KorbMatchData *md = korb_alloc(c, slots + 3, sizeof(KorbMatchData), KORB_OBJ_MATCHDATA);
    ARO_STORE(c, md, (VALUE *)(uintptr_t)&md->subject, slots[0]);
    ARO_STORE(c, md, (VALUE *)(uintptr_t)&md->regexp,  slots[1]);
    ARO_STORE(c, md, (VALUE *)(uintptr_t)&md->offsets, slots[2]);
    return RESULT_OK((VALUE)md);
}

/* $~ : stored in the flat const table under "$~". */
static uint32_t korb_re_tilde_sym(struct korb_vm *vm) { return korb_intern(vm, "$~", 2); }
static void korb_re_set_lastmatch(CTX *c, VALUE md_or_nil) { korb_const_define(c, korb_re_tilde_sym(c->vm), md_or_nil); }
static VALUE korb_re_get_lastmatch(CTX *c) { return korb_const_get(c->vm, korb_re_tilde_sym(c->vm)); }

/* Regexp#=== that also sets $~ (like CRuby's `when /re/` / grep).  Returns the
 * bool result; on a match, $~ becomes the MatchData, else nil.  Only called for
 * Regexp patterns (the caller checks KORB_REGEXP_P first). */
bool korb_re_caseeq_backref(CTX *c, VALUE *slots, VALUE pat, VALUE val) {
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
    { const KorbString *const s = VAL2STR(slots[0]);       /* re-derive: str_new above may have GC'd */
      const KorbString *const p = VAL2STR(VAL2RE(slots[1])->source);
      const int r = fn(korb_strbuf_data(p->buf), p->len, VAL2RE(slots[1])->flags,
                       korb_strbuf_data(s->buf), s->len, 0, &m);   /* fn does not allocate → borrows stable */
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
    return FIX2LONG(korb_items_data(a->items)[idx]);
}
static int korb_md_ngroups(const KorbMatchData *md) { return (int)(VAL2ARY(md->offsets)->len / 2); }
static RESULT korb_md_group(CTX *c, VALUE *slots, VALUE mdv, int i) {
    KorbMatchData *md = VAL2MD(mdv);
    if (i < 0 || i >= korb_md_ngroups(md)) return RESULT_OK(KORB_NIL);
    const long b = korb_md_off(md, i, 0), e = korb_md_off(md, i, 1);
    if (b < 0 || e < 0) return RESULT_OK(KORB_NIL);
    /* Slice via korb_str_slice_new so the capture inherits the SUBJECT's
     * encoding (like pre_match/post_match) — a match over an ASCII-8BIT string
     * must yield ASCII-8BIT captures, else String#length counts UTF-8 chars and
     * byte-based callers (e.g. StringScanner) desync.  Also GC-safe (re-reads). */
    slots[0] = md->subject;
    return korb_str_slice_new(c, slots + 1, VALUE_REF_AT(&slots[0]), (uint32_t)b, (uint32_t)(e - b));
}
/* resolve a named group in md's regexp → group number, or -1. */
static int korb_md_name_idx(CTX *c, VALUE mdv, const char *name, uint32_t nlen) {
    KorbMatchData *md = VAL2MD(mdv);
    if (!KORB_REGEXP_P(md->regexp)) return -1;
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (nf == NULL) return -1;
    const KorbString *pat = VAL2STR(VAL2RE(md->regexp)->source);
    const uint32_t flags = VAL2RE(md->regexp)->flags;
    int best = -1, last_defined = -1;   /* duplicate names → the last group that participated (else last defined → nil) */
    for (int k = 0; ; k++) {
        int gi = -1;
        const char *gn = nf(korb_strbuf_data(pat->buf), pat->len, flags, k, &gi);
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
static RESULT korb_md_group_slice(CTX *c, VALUE *slots, VALUE mdv, long lo, long len) {
    slots[0] = mdv;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)(len > 0 ? len : 0)));
    const int n = korb_md_ngroups(VAL2MD(slots[0]));
    long hi = lo + len; if (hi > n) hi = n;
    for (long gi = lo; gi < hi; gi++) {
        slots[2] = UNWRAP(korb_md_group(c, slots + 2, slots[0], (int)gi));   /* participating→str, else nil */
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE mdv = VALUE_REF_GET(self);
    const VALUE k = VALUE_SLICE_GET(a, 0);
    if (SYMBOL_P(k) || KORB_STRING_P(k)) {                /* [name] → named group */
        const char *nm; uint32_t nl;
        if (SYMBOL_P(k)) { nm = korb_sym_name(c->vm, SYM2ID(k)); nl = (uint32_t)strlen(nm); }
        else { nm = korb_strbuf_data(VAL2STR(k)->buf); nl = VAL2STR(k)->len; }
        int gi = korb_md_name_idx(c, mdv, nm, nl);
        if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        return korb_md_group(c, slots, mdv, gi);
    }
    const int n = korb_md_ngroups(VAL2MD(mdv));
    if (VALUE_SLICE_LEN(a) >= 2) {                        /* [start, length] → Array */
        korb_sword_t st = 0, ln = 0;
        if (!korb_to_index(k, &st) || !korb_to_index(VALUE_SLICE_GET(a, 1), &ln)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (st < 0) st += n;
        if (st < 0 || st > n || ln < 0) return RESULT_OK(KORB_NIL);
        return korb_md_group_slice(c, slots, mdv, st, ln);
    }
    if (KORB_RANGE_P(k)) {                                /* [range] → Array */
        const KorbRange *r = VAL2RANGE(k);
        korb_sword_t b = 0, e;
        if (r->rbegin != KORB_NIL) { if (!korb_to_index(r->rbegin, &b)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); }
        if (b < 0) b += n;
        if (r->rend == KORB_NIL) e = n; else { if (!korb_to_index(r->rend, &e)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); if (e < 0) e += n; if (!r->exclude_end) e += 1; }
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
static RESULT korb_m_md_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = VALUE_REF_GET(self);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    const int n = korb_md_ngroups(VAL2MD(slots[0]));
    for (int i = 0; i < n; i++) {
        slots[2] = UNWRAP(korb_md_group(c, slots + 2, slots[0], i));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_captures(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = VALUE_REF_GET(self);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    const int n = korb_md_ngroups(VAL2MD(slots[0]));
    for (int i = 1; i < n; i++) {
        slots[2] = UNWRAP(korb_md_group(c, slots + 2, slots[0], i));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_values_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = VALUE_REF_GET(self);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, VALUE_SLICE_LEN(a)));
    const int n = korb_md_ngroups(VAL2MD(slots[0]));
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        const VALUE arg = VALUE_SLICE_GET(a, j);
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
                CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[1]), slots[2]));
            }
            continue;
        }
        korb_sword_t i = 0; VALUE g = KORB_NIL;
        if (SYMBOL_P(arg) || KORB_STRING_P(arg)) {        /* named-capture reference */
            const char *nm; uint32_t nl;
            if (SYMBOL_P(arg)) { nm = korb_sym_name(c->vm, SYM2ID(arg)); nl = (uint32_t)strlen(nm); }
            else { nm = korb_strbuf_data(VAL2STR(arg)->buf); nl = VAL2STR(arg)->len; }
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
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_md_group(c, slots, VALUE_REF_GET(self), 0); }
static RESULT korb_m_md_pre(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const long b0 = korb_md_off(VAL2MD(VALUE_REF_GET(self)), 0, 0);
    slots[0] = VAL2MD(VALUE_REF_GET(self))->subject;     /* slice the SUBJECT, not the MatchData */
    return korb_re_slice(c, slots + 1, &slots[0], 0, b0);
}
static RESULT korb_m_md_post(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbMatchData *md = VAL2MD(VALUE_REF_GET(self));
    const long e0 = korb_md_off(md, 0, 1), slen = (long)VAL2STR(md->subject)->len;
    slots[0] = md->subject;
    return korb_re_slice(c, slots + 1, &slots[0], e0, slen);
}
/* Resolve a begin/end/offset arg (Integer, or Symbol/String name) to a group
 * index; raises IndexError on an unknown name or out-of-range index. */
static RESULT korb_md_arg_gi(CTX *c, VALUE *slots, VALUE mdv, VALUE arg, int *gi_out) {
    if (SYMBOL_P(arg) || KORB_STRING_P(arg)) {
        const char *nm; uint32_t nl;
        if (SYMBOL_P(arg)) { nm = korb_sym_name(c->vm, SYM2ID(arg)); nl = (uint32_t)strlen(nm); }
        else { nm = korb_strbuf_data(VAL2STR(arg)->buf); nl = VAL2STR(arg)->len; }
        int gi = korb_md_name_idx(c, mdv, nm, nl);
        if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        *gi_out = gi; return RESULT_OK(KORB_TRUE);
    }
    korb_sword_t i = 0;
    if (!korb_to_index(arg, &i)) {                        /* coerce via #to_int */
        VALUE av = arg; RESULT cr = korb_coerce_to_int(c, slots, &av);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(av, &i)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_re_arg_type(arg));
    }
    const int n = korb_md_ngroups(VAL2MD(mdv));           /* begin/end/offset take no negative index */
    if (i < 0 || i >= n) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %d out of matches", (int)i);
    *gi_out = (int)i; return RESULT_OK(KORB_TRUE);
}
/* which: 0=begin 1=end; bytes=false → char offset, true → byte offset. */
static RESULT korb_md_pos(CTX *c, VALUE *slots, VALUE_REF self, VALUE arg, int which, bool bytes) {
    int gi = 0; RESULT r = korb_md_arg_gi(c, slots, VALUE_REF_GET(self), arg, &gi);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    KorbMatchData *md = VAL2MD(VALUE_REF_GET(self));
    const long o = korb_md_off(md, gi, which);
    if (o < 0) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX(bytes ? o : korb_re_bchar(c->vm, VAL2STR(md->subject), o)));
}
static RESULT korb_m_md_begin(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_md_pos(c, slots, self, VALUE_SLICE_GET(a, 0), 0, false); }
static RESULT korb_m_md_end(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_md_pos(c, slots, self, VALUE_SLICE_GET(a, 0), 1, false); }
static RESULT korb_m_md_bytebegin(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_md_pos(c, slots, self, VALUE_SLICE_GET(a, 0), 0, true); }
static RESULT korb_m_md_byteend(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_md_pos(c, slots, self, VALUE_SLICE_GET(a, 0), 1, true); }
/* offset(n)/byteoffset(n) → [begin, end] (char- resp. byte-based), accepting an
 * Integer index or a Symbol/String named-capture reference. */
static RESULT korb_md_offset_impl(CTX *c, VALUE *slots, VALUE_REF self, VALUE arg, bool bytes) {
    int gi = 0; RESULT r = korb_md_arg_gi(c, slots, VALUE_REF_GET(self), arg, &gi);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    KorbMatchData *md = VAL2MD(VALUE_REF_GET(self));
    const long b = korb_md_off(md, gi, 0), e = korb_md_off(md, gi, 1);
    slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
    if (b < 0) { CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), KORB_NIL)); CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), KORB_NIL)); }
    else { const KorbString *s = VAL2STR(VAL2MD(slots[0])->subject);
           CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), LONG2FIX(bytes ? b : korb_re_bchar(c->vm, s, b))));
           CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), LONG2FIX(bytes ? e : korb_re_bchar(c->vm, s, e)))); }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_offset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_md_offset_impl(c, slots, self, VALUE_SLICE_GET(a, 0), false); }
static RESULT korb_m_md_byteoffset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_md_offset_impl(c, slots, self, VALUE_SLICE_GET(a, 0), true); }
/* match(n) → the nth match substring (or nil); match_length(n) → its length. */
static RESULT korb_m_md_match(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int gi = 0; RESULT r = korb_md_arg_gi(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), &gi);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return korb_md_group(c, slots, VALUE_REF_GET(self), gi);
}
static RESULT korb_m_md_match_length(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int gi = 0; RESULT r = korb_md_arg_gi(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), &gi);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    slots[0] = UNWRAP(korb_md_group(c, slots, VALUE_REF_GET(self), gi));
    if (slots[0] == KORB_NIL) return RESULT_OK(KORB_NIL);
    const KorbString *g = VAL2STR(slots[0]);
    return RESULT_OK(LONG2FIX((long)korb_utf8_count(korb_strbuf_data(g->buf), g->len)));
}
static RESULT korb_m_md_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(korb_md_ngroups(VAL2MD(VALUE_REF_GET(self))))); }
static RESULT korb_m_md_string(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self); const KorbString *s = VAL2STR(VAL2MD(slots[0])->subject);
    return korb_str_new(c, slots + 1, korb_strbuf_data(s->buf), s->len);
}
static RESULT korb_m_md_regexp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VAL2MD(VALUE_REF_GET(self))->regexp); }
static RESULT korb_md_names_into(CTX *c, VALUE *slots, VALUE mdv_or_re, bool is_md, VALUE_REF dst_ary) {
    if (c->vm->re_named_fn == NULL) korb_re_load(c->vm);   /* ensure the engine is loaded (names before any match) */
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    VALUE rev = is_md ? VAL2MD(mdv_or_re)->regexp : mdv_or_re;
    if (!nf || !KORB_REGEXP_P(rev)) return RESULT_OK(VALUE_REF_GET(dst_ary));
    slots[0] = rev;
    for (int k = 0; ; k++) {
        const KorbString *pat = VAL2STR(VAL2RE(slots[0])->source);   /* re-fetch (korb_str_new below moves GC) */
        const uint32_t flags = VAL2RE(slots[0])->flags;
        int gi = -1; const char *gn = nf(korb_strbuf_data(pat->buf), pat->len, flags, k, &gi);
        if (!gn) break;
        const uint32_t gnl = (uint32_t)strlen(gn);
        bool dup = false;                                /* each name only once */
        const KorbArray *da = VAL2ARY(VALUE_REF_GET(dst_ary));
        for (uint32_t j = 0; j < da->len; j++) { const KorbString *e = VAL2STR(korb_items_data(da->items)[j]); if (e->len == gnl && memcmp(korb_strbuf_data(e->buf), gn, gnl) == 0) { dup = true; break; } }
        if (dup) continue;
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, gn, gnl));
        CHECK(korb_ary_push_val(c, slots + 2, dst_ary, slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst_ary));
}
static RESULT korb_m_md_names(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    return korb_md_names_into(c, slots + 2, slots[0], true, VALUE_REF_AT(&slots[1]));
}
/* Read a trailing `name: true/false` kwarg Hash (Symbol-keyed); default when absent. */
static bool korb_kw_bool(CTX *c, VALUE_SLICE a, const char *name, bool dflt) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    if (n == 0 || !KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) return dflt;
    const VALUE key = ID2SYM(korb_intern(c->vm, name, (uint32_t)strlen(name)));
    int32_t idx = korb_hash_find(VAL2HASH(VALUE_SLICE_GET(a, n - 1)), key);
    if (idx < 0) return dflt;
    return KORB_TRUTHY(korb_items_data(VAL2HASH(VALUE_SLICE_GET(a, n - 1))->items)[2 * idx + 1]);
}
static RESULT korb_m_md_named_captures(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const bool symkeys = korb_kw_bool(c, a, "symbolize_names", false);
    slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_hash_new(c, slots + 1, 0));
    KorbMatchData *md = VAL2MD(slots[0]);
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (nf && KORB_REGEXP_P(md->regexp)) {
        for (int k = 0; ; k++) {
            /* re-fetch pattern bytes each iteration: korb_md_group below allocates
             * and the moving GC can relocate the Regexp's source string. */
            const KorbString *pat = VAL2STR(VAL2RE(VAL2MD(slots[0])->regexp)->source);
            const uint32_t flags = VAL2RE(VAL2MD(slots[0])->regexp)->flags;
            int gi = -1; const char *gn = nf(korb_strbuf_data(pat->buf), pat->len, flags, k, &gi);
            if (!gn) break;
            /* duplicate names → the value of the last group that participated */
            const int best = korb_md_name_idx(c, slots[0], gn, (uint32_t)strlen(gn));
            if (symkeys) slots[2] = ID2SYM(korb_intern(c->vm, gn, (uint32_t)strlen(gn)));
            else slots[2] = UNWRAP(korb_str_new(c, slots + 2, gn, (uint32_t)strlen(gn)));
            slots[3] = UNWRAP(korb_md_group(c, slots + 3, slots[0], best));
            CHECK(korb_hash_set(c, slots + 4, VALUE_REF_AT(&slots[1]), VALUE_REF_AT(&slots[2]), slots[3]));
        }
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; const VALUE o = VALUE_SLICE_GET(a, 0), s = VALUE_REF_GET(self);
    if (!KORB_MATCHDATA_P(o)) return RESULT_OK(KORB_FALSE);
    const KorbMatchData *m1 = VAL2MD(s), *m2 = VAL2MD(o);
    const KorbString *s1 = VAL2STR(m1->subject), *s2 = VAL2STR(m2->subject);
    if (s1->len != s2->len || memcmp(korb_strbuf_data(s1->buf), korb_strbuf_data(s2->buf), s1->len) != 0) return RESULT_OK(KORB_FALSE);
    if (KORB_REGEXP_P(m1->regexp) && KORB_REGEXP_P(m2->regexp)) {
        if (VAL2RE(m1->regexp)->flags != VAL2RE(m2->regexp)->flags) return RESULT_OK(KORB_FALSE);
        const KorbString *p1 = VAL2STR(VAL2RE(m1->regexp)->source), *p2 = VAL2STR(VAL2RE(m2->regexp)->source);
        if (p1->len != p2->len || memcmp(korb_strbuf_data(p1->buf), korb_strbuf_data(p2->buf), p1->len) != 0) return RESULT_OK(KORB_FALSE);
    } else if (m1->regexp != m2->regexp) return RESULT_OK(KORB_FALSE);
    const KorbArray *o1 = VAL2ARY(m1->offsets), *o2 = VAL2ARY(m2->offsets);
    if (o1->len != o2->len) return RESULT_OK(KORB_FALSE);
    for (uint32_t i = 0; i < o1->len; i++) if (korb_items_data(o1->items)[i] != korb_items_data(o2->items)[i]) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_md_hash(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)a;
    const KorbMatchData *md = VAL2MD(VALUE_REF_GET(self));
    const KorbString *s = VAL2STR(md->subject);
    uint64_t h = 1469598103934665603ULL;                 /* FNV-1a over subject + offsets */
    for (uint32_t i = 0; i < s->len; i++) { h ^= (unsigned char)korb_strbuf_data(s->buf)[i]; h *= 1099511628211ULL; }
    const KorbArray *o = VAL2ARY(md->offsets);
    for (uint32_t i = 0; i < o->len; i++) { h ^= (uint64_t)FIX2LONG(korb_items_data(o->items)[i]); h *= 1099511628211ULL; }
    return RESULT_OK(LONG2FIX((korb_sword_t)(h & (((korb_word_t)1 << 62) - 1))));
}
/* deconstruct_keys(keys) → named captures as a Symbol-keyed Hash (keys: nil = all,
 * or an Array selecting a subset; returns {} early if a requested key is absent). */
static RESULT korb_m_md_deconstruct_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE keys = VALUE_SLICE_GET(a, 0);
    if (keys != KORB_NIL && !KORB_ARRAY_P(keys)) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Array)", korb_re_arg_type(keys));
    slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_hash_new(c, slots + 1, 0));
    KorbMatchData *md = VAL2MD(slots[0]);
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (!nf || !KORB_REGEXP_P(md->regexp)) return RESULT_OK(slots[1]);
    if (keys != KORB_NIL) {                               /* subset by the given Symbol keys */
        const uint32_t klen = VAL2ARY(VALUE_SLICE_GET(a, 0))->len;   /* re-fetch: korb_hash_new above moved GC */
        uint32_t ng = 0;                                  /* count named groups: more keys than groups → {} */
        for (int k = 0; ; k++) {
            const KorbString *pat = VAL2STR(VAL2RE(VAL2MD(slots[0])->regexp)->source);
            const uint32_t flags = VAL2RE(VAL2MD(slots[0])->regexp)->flags;
            int gi = -1; if (!nf(korb_strbuf_data(pat->buf), pat->len, flags, k, &gi)) break;
            ng++;
        }
        if (klen > ng) return RESULT_OK(slots[1]);
        for (uint32_t i = 0; i < klen; i++) {
            const VALUE kv = korb_items_data(VAL2ARY(VALUE_SLICE_GET(a, 0))->items)[i];   /* re-fetch (moving GC) */
            if (!SYMBOL_P(kv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Symbol)", korb_re_arg_type(kv));
            const char *nm = korb_sym_name(c->vm, SYM2ID(kv)); const uint32_t nl = (uint32_t)strlen(nm);
            const int gi = korb_md_name_idx(c, slots[0], nm, nl);
            if (gi < 0) break;                            /* unknown key → stop (partial match) */
            slots[2] = kv;
            slots[3] = UNWRAP(korb_md_group(c, slots + 3, slots[0], gi));
            CHECK(korb_hash_set(c, slots + 4, VALUE_REF_AT(&slots[1]), VALUE_REF_AT(&slots[2]), slots[3]));
        }
        return RESULT_OK(slots[1]);
    }
    for (int k = 0; ; k++) {
        const KorbString *pat = VAL2STR(VAL2RE(VAL2MD(slots[0])->regexp)->source);   /* re-fetch (GC-moving) */
        const uint32_t flags = VAL2RE(VAL2MD(slots[0])->regexp)->flags;
        int gi = -1; const char *gn = nf(korb_strbuf_data(pat->buf), pat->len, flags, k, &gi);
        if (!gn) break;
        const int best = korb_md_name_idx(c, slots[0], gn, (uint32_t)strlen(gn));
        slots[2] = ID2SYM(korb_intern(c->vm, gn, (uint32_t)strlen(gn)));
        slots[3] = UNWRAP(korb_md_group(c, slots + 3, slots[0], best));
        CHECK(korb_hash_set(c, slots + 4, VALUE_REF_AT(&slots[1]), VALUE_REF_AT(&slots[2]), slots[3]));
    }
    return RESULT_OK(slots[1]);
}

/* ---- Regexp instance methods --------------------------------------------- */
static const char *korb_re_arg_type(VALUE v) {
    if (v == KORB_NIL) return "nil";
    if (v == KORB_TRUE) return "true";
    if (v == KORB_FALSE) return "false";
    return korb_type_name(v);
}
/* coerce arg → subject String (Symbol → name), or nil for non-strings. */
static RESULT korb_re_subject(CTX *c, VALUE *slots, VALUE v, VALUE *out) {
    if (KORB_STRING_P(v)) { *out = v; return RESULT_OK(KORB_TRUE); }
    if (SYMBOL_P(v)) { const char *nm = korb_sym_name(c->vm, SYM2ID(v)); *out = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm))); return RESULT_OK(KORB_TRUE); }
    *out = KORB_NIL; return RESULT_OK(KORB_FALSE);
}
/* `re =~ str` / `str =~ re` core: set $~, return match CHAR index (or nil). */
static RESULT korb_re_match_set(CTX *c, VALUE *slots, VALUE re, VALUE str) {
    if (!KORB_REGEXP_P(re)) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    if (!KORB_STRING_P(str)) {                            /* coerce Symbol / #to_str; else no match */
        slots[0] = re;                                    /* root re across the coercion alloc */
        if (SYMBOL_P(str)) {
            const char *nm = korb_sym_name(c->vm, SYM2ID(str));
            str = UNWRAP(korb_str_new(c, slots + 1, nm, (uint32_t)strlen(nm)));
        } else if (KORB_OBJECT_P(str) && korb_responds_to_coerce_p(c, slots + 1, &str, korb_intern(c->vm, "to_str", 6))) {
            slots[1] = str;
            RESULT sr = korb_send_impl(c, slots + 2, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (!KORB_STRING_P(sr.value)) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
            str = sr.value;
        } else { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
        re = slots[0];
    }
    slots[0] = re; slots[1] = str;
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
static RESULT korb_m_str_match_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE pat = VALUE_SLICE_GET(a, 0);
    const bool str_recv = KORB_STRING_P(VALUE_REF_GET(self));   /* shared with NilClass#=~ / Symbol#=~ */
    if (UNLIKELY(str_recv && KORB_STRING_P(pat)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "type mismatch: String given");
    if (UNLIKELY(str_recv && !KORB_REGEXP_P(pat) && KORB_OBJECT_P(pat))) {
        slots[0] = pat; slots[1] = VALUE_REF_GET(self);
        return korb_send(c, slots + 2, korb_intern(c->vm, "=~", 2), 0, 1);
    }
    return korb_re_match_set(c, slots, pat, VALUE_REF_GET(self));
}
/* `Regexp.allocate` leaves a plain object behind: every Regexp method must
 * refuse it rather than quietly answer nil. */
#define KORB_RE_CHECK(c, slots, self) do { \
    if (UNLIKELY(!KORB_REGEXP_P(VALUE_REF_GET(self)))) \
        return korb_raise((c), (slots), KORB_E_TYPE, 0, "uninitialized Regexp"); \
} while (0)
static RESULT korb_m_re_match_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { KORB_RE_CHECK(c, slots, self); return korb_re_match_set(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)); }
static RESULT korb_m_re_match_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* match?(str[, pos]) — no $~ */
    KORB_RE_CHECK(c, slots, self);
    VALUE subj; slots[0] = VALUE_REF_GET(self);
    if (UNWRAP(korb_re_subject(c, slots + 1, VALUE_SLICE_GET(a, 0), &subj)) != KORB_TRUE) return RESULT_OK(KORB_FALSE);
    slots[1] = subj;
    size_t startb = 0;
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* optional char start position */
        korb_sword_t pos = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &pos)) {
            const KorbString *s = VAL2STR(slots[1]); const long ncp = (long)korb_utf8_count(korb_strbuf_data(s->buf), s->len);
            if (pos < 0) pos += ncp;
            if (pos < 0 || pos > ncp) return RESULT_OK(KORB_FALSE);
            startb = (pos == 0) ? 0 : korb_utf8_byteoff(korb_strbuf_data(s->buf), s->len, (uint32_t)pos);
        }
    }
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[0], slots[1], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    return RESULT_OK(rr.value == KORB_TRUE ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_re_case_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* === (sets $~) */
    RESULT r = korb_re_match_set(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0));
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(r.value == KORB_NIL ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_re_source(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; KORB_RE_CHECK(c, slots, self); return RESULT_OK(VAL2RE(VALUE_REF_GET(self))->source); }
/* prism flag bits → Ruby's Regexp option bits.  The encoding flags (/u /e /s /n)
 * all mean "the pattern's encoding is fixed": Ruby reports FIXEDENCODING (16)
 * for /u /e /s and NOENCODING (32) for /n. */
static int korb_re_ruby_opts(uint32_t flags) {
    int o = 0;
    if (flags & 4u)  o |= 1;    /* IGNORECASE */
    if (flags & 8u)  o |= 2;    /* EXTENDED */
    if (flags & 16u) o |= 4;    /* MULTILINE */
    if (flags & (64u | 256u | 512u | KORB_RE_FIXENC)) o |= 16;   /* /u /e /s and Regexp::FIXEDENCODING */
    if (flags & 128u) o |= 32;                  /* ASCII-8BIT (/n) → NOENCODING */
    return o;
}
static RESULT korb_m_re_options(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; KORB_RE_CHECK(c, slots, self); return RESULT_OK(LONG2FIX(korb_re_ruby_opts(VAL2RE(VALUE_REF_GET(self))->flags))); }
/* /e and /s name an encoding no Ruby-visible option bit distinguishes; #encoding
 * asks for it here. */
static RESULT korb_m_re_enc_hint(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const uint32_t f = VAL2RE(VALUE_REF_GET(self))->flags;
    const char *const nm = (f & 64u) ? "EUC-JP" : (f & 256u) ? "Windows-31J" : NULL;
    return nm ? korb_str_new(c, slots, nm, (uint32_t)strlen(nm)) : RESULT_OK(KORB_NIL);
}
static RESULT korb_m_re_casefold(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK((VAL2RE(VALUE_REF_GET(self))->flags & 4u) ? KORB_TRUE : KORB_FALSE); }
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
static void korb_re_write_to_s(FILE *ms, VALUE re) {
    const uint32_t f = VAL2RE(re)->flags;
    const VALUE srcv = VAL2RE(re)->source;                      /* nil for an allocate'd / never-compiled Regexp */
    const KorbString *const src = KORB_STRING_P(srcv) ? VAL2STR(srcv) : NULL;
    bool m = (f & 16u) != 0, i = (f & 4u) != 0, x = (f & 8u) != 0;
    const char *body = src ? korb_strbuf_data(src->buf) : "";
    size_t len = src ? src->len : 0;

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
    fputc(':', ms); fwrite(body, 1, len, ms); fputc(')', ms);
}
static RESULT korb_m_re_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self);
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    korb_re_write_to_s(ms, slots[0]);
    fclose(ms); RESULT r = korb_str_new(c, slots + 1, buf, (uint32_t)z); free(buf); return r;
}
static RESULT korb_m_re_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self);
    const uint32_t f = VAL2RE(slots[0])->flags;
    const VALUE srcv = VAL2RE(slots[0])->source;                /* nil for an allocate'd / never-compiled Regexp */
    const KorbString *const src = KORB_STRING_P(srcv) ? VAL2STR(srcv) : NULL;
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    fputc('/', ms);
    /* escape bare forward slashes; pass through existing "\x" escape pairs verbatim */
    const char *p = src ? korb_strbuf_data(src->buf) : ""; const char *const end = p + (src ? src->len : 0);
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
static RESULT korb_m_re_names(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    return korb_md_names_into(c, slots + 2, slots[0], false, VALUE_REF_AT(&slots[1]));
}
/* Regexp#named_captures → { "name" => [group numbers] } (dup names accumulate). */
static RESULT korb_m_re_named_captures(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (c->vm->re_named_fn == NULL) korb_re_load(c->vm);
    slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_hash_new(c, slots + 1, 0));
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (!nf) return RESULT_OK(slots[1]);
    for (int k = 0; ; k++) {
        const KorbString *pat = VAL2STR(VAL2RE(slots[0])->source);   /* re-fetch (GC-moving) */
        const uint32_t flags = VAL2RE(slots[0])->flags;
        int gi = -1; const char *gn = nf(korb_strbuf_data(pat->buf), pat->len, flags, k, &gi);
        if (!gn) break;
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, gn, (uint32_t)strlen(gn)));   /* name key */
        int32_t idx = korb_hash_find(VAL2HASH(slots[1]), slots[2]);
        if (idx >= 0) slots[3] = korb_items_data(VAL2HASH(slots[1])->items)[2 * idx + 1];     /* existing array */
        else { slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 1)); CHECK(korb_hash_set(c, slots + 4, VALUE_REF_AT(&slots[1]), VALUE_REF_AT(&slots[2]), slots[3])); }
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), LONG2FIX(gi)));
    }
    return RESULT_OK(slots[1]);
}
/* == and hash ignore the /n (NOENCODING, 128) flag — CRuby treats it as
 * encoding metadata, not part of the compiled-pattern identity. */
#define KORB_RE_ID_FLAGS(f) ((f) & ~128u)
static RESULT korb_m_re_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; const VALUE o = VALUE_SLICE_GET(a, 0), s = VALUE_REF_GET(self);
    if (!KORB_REGEXP_P(o) || KORB_RE_ID_FLAGS(VAL2RE(s)->flags) != KORB_RE_ID_FLAGS(VAL2RE(o)->flags)) return RESULT_OK(KORB_FALSE);
    const KorbString *a1 = VAL2STR(VAL2RE(s)->source), *b1 = VAL2STR(VAL2RE(o)->source);
    if (a1->len != b1->len || memcmp(korb_strbuf_data(a1->buf), korb_strbuf_data(b1->buf), a1->len) != 0) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_re_hash(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; const KorbString *src = VAL2STR(VAL2RE(VALUE_REF_GET(self))->source);
    uint32_t h = korb_str_hash(korb_strbuf_data(src->buf), src->len) ^ (KORB_RE_ID_FLAGS(VAL2RE(VALUE_REF_GET(self))->flags) * 2654435761u);
    return RESULT_OK(LONG2FIX((long)h));
}
/* Regexp#match(str[,pos]) → MatchData|nil (sets $~); block form yields it. */
static RESULT korb_m_re_match(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    KORB_RE_CHECK(c, slots, self);
    slots[0] = VALUE_REF_GET(self); VALUE subj;
    if (VALUE_SLICE_GET(a, 0) == KORB_NIL) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    if (UNWRAP(korb_re_subject(c, slots + 2, VALUE_SLICE_GET(a, 0), &subj)) != KORB_TRUE)
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(VALUE_SLICE_GET(a, 0)));
    slots[1] = subj;
    long startc = 0; if (VALUE_SLICE_LEN(a) >= 2) { korb_sword_t p = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &p)) startc = (long)p; }
    const KorbString *s = VAL2STR(slots[1]);
    /* pos is a character index in the subject's encoding — a byte index for
     * single-byte encodings (US-ASCII / ASCII-8BIT), else a UTF-8 char index. */
    const bool sb = KORB_ENC_SB(c->vm, KORB_STR_ENC(slots[1]));
    if (startc < 0) startc += sb ? (long)s->len : (long)korb_utf8_count(korb_strbuf_data(s->buf), s->len);
    size_t startb = (startc <= 0) ? 0 : (sb ? (size_t)startc : korb_utf8_byteoff(korb_strbuf_data(s->buf), s->len, (uint32_t)startc));
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[0], slots[1], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[1], slots[0], &m));
    korb_re_set_lastmatch(c, slots[2]);
    if (block != NULL) return korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
    return RESULT_OK(slots[2]);
}

/* ---- String#match / match? ----------------------------------------------- */
static RESULT korb_re_coerce_pat(CTX *c, VALUE *slots, VALUE pv, VALUE *out) {
    if (KORB_REGEXP_P(pv)) { *out = pv; return RESULT_OK(KORB_TRUE); }
    if (KORB_STRING_P(pv)) { slots[0] = pv; *out = UNWRAP(korb_re_alloc(c, slots + 1, slots[0], 0)); return RESULT_OK(KORB_TRUE); }
    if (KORB_OBJECT_P(pv)) {                             /* a #to_str object is the pattern source */
        VALUE cv = pv;
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
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
korb_re_start_char(CTX *c, VALUE subj, VALUE posv, long *out)
{
    korb_sword_t p = 0;
    if (!korb_to_index(posv, &p)) { *out = 0; return true; }   /* unusable → treat as 0, as before */
    if (p < 0) {
        const KorbString *const s = VAL2STR(subj);
        const uint32_t nch = (KORB_STR_ENC(subj) == KORB_ENC_UTF8)
                           ? korb_utf8_count(korb_strbuf_data(s->buf), s->len) : s->len;
        p += (korb_sword_t)nch;
        if (p < 0) return false;
    }
    *out = (long)p;
    (void)c;
    return true;
}
static RESULT korb_m_str_match(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    VALUE subj;
    if (UNWRAP(korb_re_subject(c, slots, VALUE_REF_GET(self), &subj)) != KORB_TRUE) return RESULT_OK(KORB_NIL);
    slots[0] = subj; VALUE re; RESULT cr = korb_re_coerce_pat(c, slots + 1, VALUE_SLICE_GET(a, 0), &re);
    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    slots[1] = re;
    long startc = 0;
    if (VALUE_SLICE_LEN(a) >= 2 && !korb_re_start_char(c, slots[0], VALUE_SLICE_GET(a, 1), &startc))
        { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }   /* out of range → no match */
    const KorbString *s = VAL2STR(slots[0]); size_t startb = (startc <= 0) ? 0 : korb_utf8_byteoff(korb_strbuf_data(s->buf), s->len, (uint32_t)startc);
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[0], slots[1], &m));
    korb_re_set_lastmatch(c, slots[2]);
    if (block != NULL) return korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_str_match_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE subj;
    if (UNWRAP(korb_re_subject(c, slots, VALUE_REF_GET(self), &subj)) != KORB_TRUE) return RESULT_OK(KORB_FALSE);
    slots[0] = subj; VALUE re; RESULT cr = korb_re_coerce_pat(c, slots + 1, VALUE_SLICE_GET(a, 0), &re);
    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    slots[1] = re;
    long startc = 0;
    if (VALUE_SLICE_LEN(a) >= 2 && !korb_re_start_char(c, slots[0], VALUE_SLICE_GET(a, 1), &startc))
        return RESULT_OK(KORB_FALSE);                    /* out of range → no match */
    const KorbString *s = VAL2STR(slots[0]); size_t startb = (startc <= 0) ? 0 : korb_utf8_byteoff(korb_strbuf_data(s->buf), s->len, (uint32_t)startc);
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    return RESULT_OK(rr.value == KORB_TRUE ? KORB_TRUE : KORB_FALSE);
}

/* ---- String#scan --------------------------------------------------------- */
static RESULT korb_re_scan_elem(CTX *c, VALUE *slots, VALUE subj, VALUE mdv, const korb_re_match_t *m) {
    if (m->n_groups == 0) {
        slots[0] = subj; const uint32_t ml = (uint32_t)(m->ends[0] - m->starts[0]);
        KorbString *r = korb_str_alloc(c, slots + 1, ml);
        memcpy(korb_strbuf_data(r->buf), korb_strbuf_data(VAL2STR(slots[0])->buf) + m->starts[0], ml);
        KORB_STR_ENC_SET((VALUE)r, KORB_STR_ENC(slots[0]));   /* result keeps self's encoding */
        return RESULT_OK((VALUE)r);
    }
    slots[0] = mdv; slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)m->n_groups));
    for (int i = 1; i <= m->n_groups; i++) { slots[2] = UNWRAP(korb_md_group(c, slots + 2, slots[0], i)); CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[1]), slots[2])); }
    return RESULT_OK(slots[1]);
}
/* Coerce a String#scan pattern: a Regexp as-is, but a String matches LITERALLY
 * (unlike #match, which reads it as a regex source), so escape it first. */
static RESULT korb_re_coerce_pat_literal(CTX *c, VALUE *slots, VALUE pv, VALUE *out) {
    if (KORB_REGEXP_P(pv)) { *out = pv; return RESULT_OK(KORB_TRUE); }
    if (KORB_STRING_P(pv) || SYMBOL_P(pv)) {
        const char *b; uint32_t n;
        if (SYMBOL_P(pv)) { const char *nm = korb_sym_name(c->vm, SYM2ID(pv)); b = nm; n = (uint32_t)strlen(nm); }
        else { b = korb_strbuf_data(VAL2STR(pv)->buf); n = VAL2STR(pv)->len; }
        char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
        for (uint32_t i = 0; i < n; i++) { unsigned char ch = (unsigned char)b[i]; if (strchr("\\.*+?()[]{}|-^$", ch)) fputc('\\', ms); fputc(ch, ms); }
        fclose(ms);
        slots[0] = UNWRAP(korb_str_new(c, slots, buf ? buf : "", (uint32_t)z)); free(buf);
        *out = UNWRAP(korb_re_alloc(c, slots + 1, slots[0], 0));
        return RESULT_OK(KORB_TRUE);
    }
    if (KORB_OBJECT_P(pv)) {                             /* #to_str-convertible pattern (matches literally) */
        VALUE cv = pv;
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (korb_responds_to_coerce_p(c, slots, &cv, to_str)) {
            slots[0] = cv;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (KORB_STRING_P(sr.value)) return korb_re_coerce_pat_literal(c, slots, sr.value, out);
        }
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_re_arg_type(pv));
}
static RESULT korb_m_str_scan(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    VALUE re = KORB_NIL; RESULT cr = korb_re_coerce_pat_literal(c, slots, VALUE_SLICE_GET(a, 0), &re);
    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    korb_re_set_lastmatch(c, KORB_NIL);              /* $~ ← nil, then last match (or stays nil if none) */
    slots[0] = VALUE_REF_GET(self); slots[1] = re;
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
        else CHECK(korb_ary_push_val(c, slots + 5, VALUE_REF_AT(&slots[2]), slots[4]));
        off = (m.ends[0] > m.starts[0]) ? m.ends[0] : m.starts[0] + 1;
        {   /* an empty match advances one CHARACTER, not one byte */
            const KorbString *s2 = VAL2STR(slots[0]);
            while (off < (long)s2->len && ((unsigned char)korb_strbuf_data(s2->buf)[off] & 0xC0) == 0x80) off++;
        }
    }
    return RESULT_OK(block ? slots[0] : slots[2]);
}

/* ---- String#gsub/sub with a Regexp (forward-declared for string.c) -------- */
static void korb_re_expand_repl(CTX *c, FILE *ms, const char *rep, uint32_t rn, const char *subj, uint32_t slen, const korb_re_match_t *m, VALUE re) {
    for (uint32_t i = 0; i < rn; i++) {
        if (rep[i] == '\\' && i + 1 < rn) {
            char nx = rep[i + 1];
            if (nx >= '0' && nx <= '9') { int g = nx - '0'; if (g <= m->n_groups && m->starts[g] >= 0) fwrite(subj + m->starts[g], 1, (size_t)(m->ends[g] - m->starts[g]), ms); i++; continue; }
            else if (nx == '&') { if (m->starts[0] >= 0) fwrite(subj + m->starts[0], 1, (size_t)(m->ends[0] - m->starts[0]), ms); i++; continue; }
            else if (nx == '+') { for (int g = m->n_groups; g >= 1; g--) if (m->starts[g] >= 0) { fwrite(subj + m->starts[g], 1, (size_t)(m->ends[g] - m->starts[g]), ms); break; } i++; continue; }
            else if (nx == '`') { fwrite(subj, 1, (size_t)m->starts[0], ms); i++; continue; }
            else if (nx == '\'') { fwrite(subj + m->ends[0], 1, (size_t)(slen - m->ends[0]), ms); i++; continue; }
            else if (nx == '\\') { fputc('\\', ms); i++; continue; }
            else if (nx == 'k' && i + 2 < rn && rep[i + 2] == '<') {
                uint32_t j = i + 3; while (j < rn && rep[j] != '>') j++;
                int gi = -1; korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
                if (nf && KORB_REGEXP_P(re)) { const KorbString *pat = VAL2STR(VAL2RE(re)->source); const uint32_t fl = VAL2RE(re)->flags;
                    for (int k = 0; ; k++) { int idx = -1; const char *gn = nf(korb_strbuf_data(pat->buf), pat->len, fl, k, &idx); if (!gn) break;
                        if (strlen(gn) == j - (i + 3) && memcmp(gn, rep + i + 3, j - (i + 3)) == 0) { gi = idx; break; } } }
                if (gi >= 0 && gi <= m->n_groups && m->starts[gi] >= 0) fwrite(subj + m->starts[gi], 1, (size_t)(m->ends[gi] - m->starts[gi]), ms);
                i = j; continue;
            }
        }
        fputc(rep[i], ms);
    }
}
/* String/Symbol → a literal (all-metachars-escaped) Regexp, for String-pattern
 * gsub/sub/split routed through the engine (so $~ / MatchData behave). */
RESULT korb_re_literal_regexp(CTX *c, VALUE *slots, VALUE pv, VALUE *out) {
    return korb_re_coerce_pat_literal(c, slots, pv, out);
}
/* Emit v into ms as a String, dispatching a user-defined #to_s (CRuby coerces
 * gsub block results and Hash-replacement values with #to_s). */
static RESULT korb_emit_to_s(CTX *c, VALUE *slots, FILE *ms, VALUE v) {
    if (KORB_STRING_P(v)) { const KorbString *r = VAL2STR(v); fwrite(korb_strbuf_data(r->buf), 1, r->len, ms); return RESULT_OK(KORB_NIL); }
    slots[0] = v;
    RESULT sr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    if (KORB_STRING_P(sr.value)) { const KorbString *r = VAL2STR(sr.value); fwrite(korb_strbuf_data(r->buf), 1, r->len, ms); }
    else korb_fprint_to_s(c, ms, sr.value);
    return RESULT_OK(KORB_NIL);
}
RESULT korb_re_str_gsub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, VALUE re, bool global, bool in_place, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = VALUE_REF_GET(self); slots[1] = re;
    const KorbString *s0 = VAL2STR(slots[0]); const uint32_t sn = s0->len;
    char *const src = malloc(sn ? sn : 1); memcpy(src, korb_strbuf_data(s0->buf), sn);
    char *rep = NULL; uint32_t rn = 0; VALUE hashrep = KORB_NIL;
    if (block == NULL) {
        if (VALUE_SLICE_LEN(a) < 2) {   /* sub → ArgumentError; gsub(pat) → Enumerator over the matches */
            if (!global) { free(src); return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 1, expected 2)"); }
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 0));
            VALUE_REF acc = VALUE_REF_AT(&slots[2]);
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
                    uint32_t cl = (em.starts[0] < (long)sn) ? korb_utf8_seq_len((const unsigned char *)src, (uint32_t)em.starts[0], sn) : 1;
                    eoff = em.starts[0] + (cl ? cl : 1);
                }
            }
            free(src);
            slots[3] = UNWRAP(korb_enum_desc(c, slots + 3, VALUE_REF_GET(self), "gsub"));
            RESULT nr = korb_enum_new(c, slots + 4, VALUE_REF_GET(acc), slots[3]);
            if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
            VAL2ENUM(nr.value)->size_unknown = 1;       /* CRuby's gsub enumerator reports no size */
            return nr;
        }
        VALUE rv = VALUE_SLICE_GET(a, 1);
        if (KORB_STRING_P(rv)) { const KorbString *rs = VAL2STR(rv); rn = rs->len; rep = malloc(rn ? rn : 1); memcpy(rep, korb_strbuf_data(rs->buf), rn); }
        else if (KORB_HASH_P(rv)) { hashrep = rv; slots[2] = rv; }
        else {                          /* coerce replacement via #to_str */
            const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
            if (KORB_OBJECT_P(rv) && korb_responds_to_coerce_p(c, slots + 3, &rv, to_str)) {
                slots[3] = rv;
                RESULT sr = korb_send_impl(c, slots + 4, to_str, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) { free(src); return sr; }
                if (!KORB_STRING_P(sr.value)) { free(src); return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(rv)); }
                const KorbString *rs = VAL2STR(sr.value); rn = rs->len; rep = malloc(rn ? rn : 1); memcpy(rep, korb_strbuf_data(rs->buf), rn);
            } else { free(src); return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(rv)); }
        }
    }
    char *buf = NULL; size_t bz = 0; FILE *ms = open_memstream(&buf, &bz);
    long off = 0; bool replaced = false;
    /* the result encoding is negotiated with every replacement, as CRuby does */
    const uint32_t senc = KORB_STR_ENC(VALUE_REF_GET(self));
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
        fwrite((p), 1, kn_, ms); \
    } while (0)
    korb_re_match_t last_m; bool have_last = false;   /* POD copy of the final match → $~ after the loop */
    while (off <= (long)sn) {
        korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 3, slots[1], slots[0], (size_t)off, &m);
        if (UNLIKELY(rr.state != KORB_NORMAL)) { free(src); free(rep); fclose(ms); free(buf); return rr; }
        if (rr.value != KORB_TRUE) break;
        last_m = m; have_last = true;
        const long ms0 = m.starts[0], me0 = m.ends[0];
        KORB_GSUB_KEEP(src + off, (size_t)(ms0 - off));
        if (block) {
            slots[3] = UNWRAP(korb_re_build_md(c, slots + 3, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[3]);
            slots[4] = UNWRAP(korb_md_group(c, slots + 4, slots[3], 0));
            RESULT yr = korb_block_yield(c, slots + 5, block, def_env, &slots[4], 1, cself);
            if (UNLIKELY(yr.state != KORB_NORMAL)) { free(src); fclose(ms); free(buf); return yr; }
            slots[4] = yr.value;
            if (KORB_STRING_P(slots[4]) && !renc_bad && !korb_str_enc_fold(c->vm, &renc, &rasc, slots[4]))
                { renc_bad = true; renc_other = KORB_STR_ENC(slots[4]); }
            RESULT er = korb_emit_to_s(c, slots + 5, ms, slots[4]);
            if (UNLIKELY(er.state != KORB_NORMAL)) { free(src); free(rep); fclose(ms); free(buf); return er; }
        } else if (hashrep != KORB_NIL) {
            const uint32_t ml = (uint32_t)(me0 - ms0);
            slots[3] = UNWRAP(korb_str_new(c, slots + 3, src + ms0, ml));   /* whole match = hash key */
            slots[4] = slots[2];                                            /* hash recv     (base[-1]) */
            slots[5] = slots[3];                                           /* key arg       */
            RESULT hr = korb_send_impl(c, slots + 6, korb_intern(c->vm, "[]", 2), 0, 1, NULL, NULL, NULL);  /* respects default / default_proc */
            if (UNLIKELY(hr.state != KORB_NORMAL)) { free(src); free(rep); fclose(ms); free(buf); return hr; }
            slots[4] = hr.value;
            if (KORB_STRING_P(slots[4]) && !renc_bad && !korb_str_enc_fold(c->vm, &renc, &rasc, slots[4]))
                { renc_bad = true; renc_other = KORB_STR_ENC(slots[4]); }
            RESULT er = korb_emit_to_s(c, slots + 5, ms, slots[4]);        /* nil → "", else #to_s */
            if (UNLIKELY(er.state != KORB_NORMAL)) { free(src); free(rep); fclose(ms); free(buf); return er; }
        } else {
            korb_re_expand_repl(c, ms, rep, rn, src, sn, &m, slots[1]);
        }
        replaced = true;
        if (me0 > ms0) off = me0;
        else { if (ms0 < (long)sn) { uint32_t cl = korb_utf8_seq_len((const unsigned char *)src, (uint32_t)ms0, sn); if (!cl) cl = 1; KORB_GSUB_KEEP(src + ms0, cl); off = ms0 + cl; } else off = ms0 + 1; }
        if (!global) break;
    }
    if (off <= (long)sn) KORB_GSUB_KEEP(src + off, (size_t)(sn - off));
    #undef KORB_GSUB_KEEP
    fclose(ms); free(src); free(rep);
    /* $~ = MatchData of the last match (nil if none), for access after gsub returns */
    if (have_last) { slots[3] = UNWRAP(korb_re_build_md(c, slots + 3, slots[0], slots[1], &last_m)); korb_re_set_lastmatch(c, slots[3]); }
    else korb_re_set_lastmatch(c, KORB_NIL);
    if (renc_bad) { free(buf); return korb_raise_enc_compat(c, slots + 3, KORB_STR_ENC(VALUE_REF_GET(self)), renc_other); }
    RESULT nr = korb_str_new(c, slots + 3, buf ? buf : "", (uint32_t)bz); free(buf);
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    KORB_STR_ENC_SET(nr.value, renc);   /* self's encoding, widened by the replacements */
    if (!in_place) return nr;
    slots[3] = nr.value; const KorbString *res = VAL2STR(slots[3]); const uint32_t w = res->len;
    KorbString *s2 = korb_str_ensure(c, slots + 4, self, w); res = VAL2STR(slots[3]);
    memcpy(korb_strbuf_data(s2->buf), korb_strbuf_data(res->buf), w); s2->len = w; korb_strbuf_data(s2->buf)[w] = '\0';
    if (replaced) KORB_STR_ENC_SET(VALUE_REF_GET(self), renc);   /* gsub! widens self's encoding too */
    return RESULT_OK(replaced ? VALUE_REF_GET(self) : KORB_NIL);
}

/* ---- String#split with a Regexp (forward-declared for string.c) ---------- */
/* Emit a field [beg, beg+len) of subj into res, deferring empty fields when
 * empty_count >= 0 (limit omitted/0 → trailing empties suppressed).  Mirrors
 * MRI's split_string()/SPLIT_STR: on a non-empty field, flush any deferred
 * empties first, then push the substring. */
static RESULT korb_re_split_emit(CTX *c, VALUE *slots, VALUE_REF res, VALUE *subj, long beg, long len, long *empty_count) {
    if (*empty_count >= 0 && len == 0) { (*empty_count)++; return RESULT_OK(KORB_NIL); }
    while (*empty_count > 0) { slots[0] = UNWRAP(korb_str_new(c, slots, "", 0)); CHECK(korb_ary_push_val(c, slots + 1, res, slots[0])); (*empty_count)--; }
    slots[0] = UNWRAP(korb_re_slice(c, slots, subj, beg, beg + len));
    CHECK(korb_ary_push_val(c, slots + 1, res, slots[0]));
    return RESULT_OK(KORB_NIL);
}
RESULT korb_re_str_split(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, long limit) {
    slots[0] = VALUE_REF_GET(self); slots[1] = re; slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 0));
    VALUE_REF res = VALUE_REF_AT(&slots[2]);
    const long len0 = (long)VAL2STR(slots[0])->len;
    if (len0 == 0) return RESULT_OK(VALUE_REF_GET(res));      /* "".split(x) → [] (any limit) */
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
            const unsigned char *d = (const unsigned char *)korb_strbuf_data(VAL2STR(slots[0])->buf);
            if (last_null == 1) {
                uint32_t cl = korb_utf8_seq_len(d, (uint32_t)beg, (uint32_t)len); if (!cl) cl = 1;
                CHECK(korb_re_split_emit(c, slots + 5, res, &slots[0], beg, cl, &empty_count));
                beg = start;
            } else {
                if (start == len) start++;
                else { uint32_t cl = korb_utf8_seq_len(d, (uint32_t)start, (uint32_t)len); if (!cl) cl = 1; start += cl; }
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
    return RESULT_OK(VALUE_REF_GET(res));
}

/* ---- String#[] / index with a Regexp (forward-declared) ------------------ */
/* Byte span [*bs, *be) of a Regexp match on self (optional capture group by
 * index or name); sets $~ and *found. Used by String#[]= and String#slice!.
 * `write` (String#[]=) reports every miss as the IndexError CRuby raises there;
 * a read just leaves *found false. */
RESULT korb_re_str_span(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, VALUE group_or_nil, bool *found, uint32_t *bs, uint32_t *be, bool write) {
    /* the capture argument outlives the match run (which allocates), so it is
     * parked in a slot rather than kept in a C local */
    slots[0] = VALUE_REF_GET(self); slots[1] = re; slots[2] = group_or_nil; korb_re_match_t m;
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
            if (SYMBOL_P(slots[2])) { nm = korb_sym_name(c->vm, SYM2ID(slots[2])); nl = (uint32_t)strlen(nm); } else { nm = korb_strbuf_data(VAL2STR(slots[2])->buf); nl = VAL2STR(slots[2])->len; }
            gi = korb_md_name_idx(c, slots[3], nm, nl);
            if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        } else {
            VALUE gv = slots[2];
            const char *const on = korb_coerce_name(c, gv);
            RESULT cr = korb_coerce_to_int(c, slots + 4, &gv);   /* self/re/group/md stay rooted in slots[0..3] */
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value == KORB_FALSE)
                return korb_raise(c, slots + 4, KORB_E_TYPE, 0, "can't convert %s to Integer (%s#to_int gives %s)", on, on, korb_type_name(gv));
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
RESULT korb_re_str_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, VALUE group_or_nil) {
    slots[0] = VALUE_REF_GET(self); slots[1] = re; slots[2] = group_or_nil; korb_re_match_t m;   /* group arg: rooted across the match run */
    RESULT rr = korb_re_run(c, slots + 3, slots[1], slots[0], 0, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[3] = UNWRAP(korb_re_build_md(c, slots + 3, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[3]);
    int gi = 0;
    if (slots[2] != KORB_UNDEF) {
        if (SYMBOL_P(slots[2]) || KORB_STRING_P(slots[2])) {
            const char *nm; uint32_t nl;
            if (SYMBOL_P(slots[2])) { nm = korb_sym_name(c->vm, SYM2ID(slots[2])); nl = (uint32_t)strlen(nm); } else { nm = korb_strbuf_data(VAL2STR(slots[2])->buf); nl = VAL2STR(slots[2])->len; }
            gi = korb_md_name_idx(c, slots[3], nm, nl);
            if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        } else {
            korb_sword_t g = 0;
            if (!korb_to_index(slots[2], &g)) {          /* #to_int coercion; nil/other → TypeError */
                if (slots[2] == KORB_NIL)
                    return korb_raise(c, slots + 4, KORB_E_TYPE, 0, "no implicit conversion from nil to integer");
                VALUE gv = slots[2];
                RESULT cr = korb_coerce_to_int(c, slots + 4, &gv);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                if (!korb_to_index(gv, &g))
                    return korb_raise_no_int(c, slots + 4, slots[2]);
            }
            if (g < 0) g += korb_md_ngroups(VAL2MD(slots[3]));   /* negative capture index counts from the last group */
            gi = (int)g;
        }
    }
    return korb_md_group(c, slots + 4, slots[3], gi);
}
RESULT korb_re_str_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, long startc, bool bytes) {
    slots[0] = VALUE_REF_GET(self); slots[1] = re;
    const KorbString *s = VAL2STR(slots[0]);
    const long ncp = bytes ? (long)s->len : (long)korb_utf8_count(korb_strbuf_data(s->buf), s->len);
    if (startc < 0) startc += ncp;
    if (startc < 0 || startc > ncp) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    size_t startb = bytes ? (size_t)startc : ((startc == 0) ? 0 : korb_utf8_byteoff(korb_strbuf_data(s->buf), s->len, (uint32_t)startc));
    korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[2]);
    return RESULT_OK(LONG2FIX(bytes ? m.starts[0] : korb_re_bchar(c->vm, VAL2STR(slots[0]), m.starts[0])));
}
/* rindex/byterindex with a Regexp: last match starting at position <= stop
 * (char position for rindex, byte for byterindex). Considers overlapping starts. */
RESULT korb_re_str_rindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, long stop, bool bytes, bool have_stop) {
    slots[0] = VALUE_REF_GET(self); slots[1] = re;
    const KorbString *s0 = VAL2STR(slots[0]); const long slen = (long)s0->len;
    const long ncp = bytes ? slen : (long)korb_utf8_count(korb_strbuf_data(s0->buf), slen);
    if (!have_stop) stop = ncp;
    if (stop < 0) stop += ncp;
    if (stop < 0) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    const long stopb = bytes ? stop : (stop >= ncp ? slen : (long)korb_utf8_byteoff(korb_strbuf_data(s0->buf), slen, (uint32_t)stop));
    korb_re_match_t last_m; bool have = false; long off = 0;
    while (off <= slen) {
        korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], (size_t)off, &m);
        if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
        if (rr.value != KORB_TRUE) break;
        if (m.starts[0] > stopb) break;                  /* start past the limit → later ones are too */
        last_m = m; have = true;
        off = m.starts[0] + 1;                            /* allow overlapping starts (progress: starts[0] >= off) */
    }
    if (!have) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[0], slots[1], &last_m)); korb_re_set_lastmatch(c, slots[2]);
    return RESULT_OK(LONG2FIX(bytes ? last_m.starts[0] : korb_re_bchar(c->vm, VAL2STR(slots[0]), last_m.starts[0])));
}

/* ---- Regexp class methods ------------------------------------------------ */
static RESULT korb_m_re_escape(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; VALUE v = VALUE_SLICE_GET(a, 0);
    if (SYMBOL_P(v)) { const char *nm = korb_sym_name(c->vm, SYM2ID(v)); slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm))); }
    else if (KORB_STRING_P(v)) slots[0] = v;
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(v));
    const KorbString *s = VAL2STR(slots[0]);
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    for (uint32_t i = 0; i < s->len; i++) { unsigned char ch = (unsigned char)korb_strbuf_data(s->buf)[i];
        if (strchr("\\.*+?()[]{}|-^$", ch)) { fputc('\\', ms); fputc(ch, ms); }
        else if (ch == '\n') fputs("\\n", ms); else if (ch == '\r') fputs("\\r", ms);
        else if (ch == '\t') fputs("\\t", ms); else if (ch == '\f') fputs("\\f", ms);
        else if (ch == ' ') fputs("\\ ", ms); else fputc(ch, ms); }
    fclose(ms);
    /* The escape keeps the source's encoding, except that an all-ASCII input
     * yields US-ASCII whatever it was tagged with (CRuby). */
    const uint32_t senc = korb_str_ascii_only_p(c->vm, slots[0])
                        ? KORB_ENC_USASCII : KORB_STR_ENC(slots[0]);
    RESULT r = korb_str_new(c, slots + 1, buf ? buf : "", (uint32_t)z); free(buf);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, senc);
    return r;
}
static RESULT korb_m_re_new(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE src = VALUE_SLICE_GET(a, 0); uint32_t flags = 0;
    bool from_regexp = false;
    if (KORB_REGEXP_P(src)) { slots[0] = VAL2RE(src)->source; flags = VAL2RE(src)->flags; from_regexp = true; }
    else if (KORB_STRING_P(src)) slots[0] = src;
    else {                                                /* #to_str coercion, else TypeError */
        slots[0] = src;
        if (korb_responds_to(c, src, korb_intern(c->vm, "to_str", 6))) {
            RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (!KORB_STRING_P(r.value))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(src));
            slots[0] = r.value;
        } else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(src));
    }
    /* A Regexp first argument carries its own options; any 2nd/3rd arg is ignored (with a warning, as in CRuby). */
    if (from_regexp && VALUE_SLICE_LEN(a) >= 2 && VALUE_SLICE_GET(a, 1) != KORB_NIL)
        korb_warn(c, slots + 1, "flags ignored");
    if (!from_regexp && VALUE_SLICE_LEN(a) >= 2) {
        VALUE opt = VALUE_SLICE_GET(a, 1);
        if (opt == KORB_NIL || opt == KORB_FALSE) {}
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
                switch (korb_strbuf_data(fs->buf)[k]) {
                  case 'i': flags |= 4u;  break;          /* IGNORECASE */
                  case 'm': flags |= 16u; break;          /* MULTILINE */
                  case 'x': flags |= 8u;  break;          /* EXTENDED */
                  default:                            /* CRuby names the whole option string, not the offending char */
                    return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unknown regexp option: %.*s", (int)fs->len, korb_strbuf_data(fs->buf));
                }
            }
        }
        else {
            flags |= 4u;                                  /* any other truthy value → IGNORECASE, with CRuby's warning */
            slots[1] = opt;                               /* CRuby names the value itself (its #inspect) */
            char *ib = NULL; size_t iz = 0;
            FILE *const ims = open_memstream(&ib, &iz);
            if (ims) { korb_fprint_inspect_s(c, slots + 2, ims, slots[1]); fclose(ims); }
            korb_warn(c, slots + 2, "expected true or false as ignorecase: %s", ib ? ib : "");
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
        if (!vf(korb_strbuf_data(ps->buf), ps->len, flags)) {
            /* the engine's own reason, quoted like CRuby ("<why>: /<pattern>/") */
            char why[288]; const char *const m = korb_re_error(c->vm);
            const KorbString *const ps2 = VAL2STR(slots[0]);
            snprintf(why, sizeof why, "%s: /%.*s/", m ? m : "invalid regular expression",
                     (int)ps2->len, korb_strbuf_data(ps2->buf));
            return korb_raise(c, slots, KORB_E_REGEXP, 0, "%s", why);
        }
    }
    const RESULT rr = korb_re_alloc(c, slots + 1, slots[0], flags);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    /* Regexp subclass: tag the instance with the receiver class (the same
     * side-table route Range/String/Array subclasses take). */
    if (KORB_CLASS_P(VALUE_REF_GET(self)) &&
        VALUE_REF_GET(self) != korb_builtin_class_obj(c->vm, KORB_C_REGEXP)) {
        slots[1] = rr.value;
        slots[2] = VALUE_REF_GET(self);
        ((AroObjectHeader *)(uintptr_t)slots[1])->flags |= KORB_FL_HAS_KLASS;
        korb_klass_override_set(c, slots[1], slots[2]);   /* both rooted; set does not GC */
        return RESULT_OK(slots[1]);
    }
    return rr;
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
static bool korb_re_part_enc(CTX *c, VALUE *slots, VALUE_REF reref, uint32_t *idx, bool *fixed) {
    /* the Regexp is read from the caller's scanned slot each time: the dispatches
     * below allocate, and a raw VALUE would go stale under the moving GC */
    slots[0] = VALUE_REF_GET(reref);
    const RESULT er = korb_send(c, slots + 1, korb_intern(c->vm, "encoding", 8), 0, 0);
    if (er.state != KORB_NORMAL || er.value == KORB_NIL) return false;
    slots[0] = er.value;
    const RESULT nr = korb_send(c, slots + 1, korb_intern(c->vm, "name", 4), 0, 0);
    if (nr.state != KORB_NORMAL || !KORB_STRING_P(nr.value)) return false;
    slots[0] = nr.value;
    char nb[64];                                   /* the registry takes a NUL-terminated name */
    const KorbString *const ns = VAL2STR(slots[0]);
    uint32_t n = ns->len < sizeof nb - 1 ? ns->len : (uint32_t)(sizeof nb - 1);
    memcpy(nb, korb_strbuf_data(ns->buf), n); nb[n] = '\0';
    *idx = korb_enc_index_for_name(c->vm, nb);
    slots[0] = VALUE_REF_GET(reref);
    const RESULT fr = korb_send(c, slots + 1, korb_intern(c->vm, "fixed_encoding?", 15), 0, 0);
    *fixed = (fr.state == KORB_NORMAL) && KORB_TRUTHY(fr.value);
    return true;
}
static RESULT korb_m_re_union(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    slots[0] = KORB_NIL;
    slots[3] = KORB_NIL;                                 /* the Array form's element list (parked) */
    slots[4] = KORB_NIL;                                 /* the part being appended (parked: we dispatch) */
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    uint32_t n = VALUE_SLICE_LEN(a);
    if (n == 1 && KORB_ARRAY_P(VALUE_SLICE_GET(a, 0))) { slots[3] = VALUE_SLICE_GET(a, 0); n = VAL2ARY(slots[3])->len; }
    #define KORB_UNION_ITEM(i) (slots[3] != KORB_NIL ? korb_items_data(VAL2ARY(slots[3])->items)[(i)] : VALUE_SLICE_GET(a, (i)))
    if (n == 0) fputs("(?!)", ms);
    if (n == 1) {                                       /* a single Regexp argument IS the union (CRuby) */
        slots[4] = KORB_UNION_ITEM(0);
        if (KORB_REGEXP_P(slots[4])) { fclose(ms); free(buf); return RESULT_OK(slots[4]); }
        if (!KORB_STRING_P(slots[4]) && korb_responds_to(c, slots[4], korb_intern(c->vm, "to_regexp", 9))) {
            const RESULT rr = korb_send(c, slots + 5, korb_intern(c->vm, "to_regexp", 9), 0, 0);
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
            korb_responds_to(c, slots[4], korb_intern(c->vm, "to_regexp", 9))) {
            const RESULT rr = korb_send(c, slots + 5, korb_intern(c->vm, "to_regexp", 9), 0, 0);   /* rb_check_regexp_type */
            if (UNLIKELY(rr.state != KORB_NORMAL)) { fclose(ms); free(buf); return rr; }
            if (UNLIKELY(!KORB_REGEXP_P(rr.value))) {
                fclose(ms); free(buf);
                return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Regexp", korb_re_arg_type(slots[4]));
            }
            slots[4] = rr.value;
        }
        if (KORB_REGEXP_P(slots[4])) {
            if (!korb_re_part_enc(c, slots + 5, VALUE_REF_AT(&slots[4]), &e, &part_fixed)) e = KORB_ENC_USASCII;
            korb_re_write_to_s(ms, slots[4]);            /* after the dispatches: slots[4] is re-read */
            part_asciionly = !part_fixed;
        } else if (KORB_STRING_P(slots[4]) || SYMBOL_P(slots[4]) ||
                   korb_responds_to(c, slots[4], korb_intern(c->vm, "to_str", 6))) {
            if (SYMBOL_P(slots[4])) {                    /* a Symbol unions as its name */
                const char *const sn = korb_sym_name(c->vm, SYM2ID(slots[4]));
                const RESULT sr = korb_str_new(c, slots + 5, sn, (uint32_t)strlen(sn));
                if (UNLIKELY(sr.state != KORB_NORMAL)) { fclose(ms); free(buf); return sr; }
                slots[4] = sr.value;
            } else if (!KORB_STRING_P(slots[4])) {       /* #to_str coercion (CRuby's StringValue) */
                const RESULT sr = korb_send(c, slots + 5, korb_intern(c->vm, "to_str", 6), 0, 0);
                if (UNLIKELY(sr.state != KORB_NORMAL)) { fclose(ms); free(buf); return sr; }
                if (UNLIKELY(!KORB_STRING_P(sr.value))) {
                    fclose(ms); free(buf);
                    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String",
                                      korb_re_arg_type(slots[4]));
                }
                slots[4] = sr.value;
            }
            const KorbString *const s2 = VAL2STR(slots[4]);   /* no dispatch below: the borrow is safe */
            bool nonascii = false;
            for (uint32_t j = 0; j < s2->len; j++) {
                const unsigned char ch = (unsigned char)korb_strbuf_data(s2->buf)[j];
                if (ch >= 0x80) nonascii = true;
                if (strchr("\\.*+?()[]{}|-^$", ch)) fputc('\\', ms);
                fputc(ch, ms);
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
    fclose(ms); slots[0] = UNWRAP(korb_str_new(c, slots, buf ? buf : "", (uint32_t)z)); free(buf);
    const uint32_t renc = (incompat != UINT32_MAX) ? incompat : cfixed;
    if (renc != UINT32_MAX) KORB_STR_ENC_SET(slots[0], renc);   /* the union carries the imposed encoding */
    return korb_re_alloc(c, slots + 1, slots[0], 0);      /* CRuby builds it from the source alone */
}
static RESULT korb_m_re_last_match(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; VALUE md = korb_re_get_lastmatch(c);
    if (VALUE_SLICE_LEN(a) == 0) return RESULT_OK(md == 0 ? KORB_NIL : md);
    if (md == 0 || md == KORB_NIL || !KORB_MATCHDATA_P(md)) return RESULT_OK(KORB_NIL);
    slots[0] = md;
    const VALUE k = VALUE_SLICE_GET(a, 0);
    if (SYMBOL_P(k) || KORB_STRING_P(k)) {                /* a name, as MatchData#[] takes one */
        const char *nm; uint32_t nl;
        if (SYMBOL_P(k)) { nm = korb_sym_name(c->vm, SYM2ID(k)); nl = (uint32_t)strlen(nm); }
        else { nm = korb_strbuf_data(VAL2STR(k)->buf); nl = VAL2STR(k)->len; }
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
RESULT korb_backref(CTX *c, VALUE *slots, int kind) {
    VALUE md = korb_re_get_lastmatch(c);
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
static void korb_init_regexp(CTX *c, VALUE *slots) {
    struct korb_vm *const vm = c->vm;
    slots[0] = korb_const_get(vm, korb_intern(vm, "Regexp", 6));
    korb_const_define_owned(c, korb_intern(vm, "IGNORECASE", 10), LONG2FIX(1), slots[0]);
    korb_const_define_owned(c, korb_intern(vm, "EXTENDED",   8),  LONG2FIX(2), slots[0]);
    korb_const_define_owned(c, korb_intern(vm, "MULTILINE",  9),  LONG2FIX(4), slots[0]);
    /* Encoding option bits: reported by #options, never set by koruby's engine. */
    korb_const_define_owned(c, korb_intern(vm, "FIXEDENCODING", 13), LONG2FIX(16), slots[0]);
    korb_const_define_owned(c, korb_intern(vm, "NOENCODING",    10), LONG2FIX(32), slots[0]);
    slots[1] = korb_obj_singleton(c, slots + 1, slots[0]).value;
    korb_class_def_cfn(c, slots[1], "escape", korb_m_re_escape, 1);
    korb_class_def_cfn(c, slots[1], "quote",  korb_m_re_escape, 1);
    korb_class_def_cfn(c, slots[1], "new",     korb_m_re_new, -1);
    korb_class_def_cfn(c, slots[1], "compile", korb_m_re_new, -1);
    korb_class_def_cfn(c, slots[1], "union",   korb_m_re_union, -1);
    korb_class_def_cfn(c, slots[1], "last_match", korb_m_re_last_match, -1);
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
