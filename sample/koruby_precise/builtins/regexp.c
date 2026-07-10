/* koruby_precise — regexp.c: Regexp + MatchData + String-regex methods.
 * #included into korb_runtime.c AFTER string.c (needs korb_utf8_* / korb_str_slice_new)
 * and set.c (needs korb_obj_singleton).
 *
 * Matching is delegated to astrogre via koruby_regex.so → libastrogre.so (see
 * regex_bridge.c) which returns per-group byte spans + named-capture info.  A
 * MatchData carries the subject, the source Regexp, and a KorbArray of Integer
 * byte offsets [b0,e0,b1,e1,...] (-1 for a group that didn't participate).  $~
 * (and thus $1..$9, $&, $`, $') is kept in the flat const table under "$~". */

/* ---- low-level engine call (no koruby alloc inside → subject bytes stable) */
static RESULT korb_re_run(CTX *c, VALUE *slots, VALUE re, VALUE subj, size_t startb, korb_re_match_t *m) {
    if (UNLIKELY(!KORB_REGEXP_P(re) || !KORB_STRING_P(subj))) return RESULT_OK(KORB_FALSE);
    const korb_re_exec_fn_t fn = korb_re_load(c->vm);
    if (UNLIKELY(fn == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Regexp engine (koruby_regex.so) unavailable");
    const KorbString *const pat = VAL2STR(VAL2RE(re)->source), *const s = VAL2STR(subj);
    if (startb > s->len) { m->matched = 0; return RESULT_OK(KORB_FALSE); }
    const int rc = fn(pat->buf->data, pat->len, VAL2RE(re)->flags, s->buf->data, s->len, startb, m);
    if (UNLIKELY(rc < 0)) return korb_raise(c, slots, KORB_E_REGEXP, 0, "invalid regular expression");
    return RESULT_OK(rc == 1 ? KORB_TRUE : KORB_FALSE);
}

/* GC-safe byte-slice of the string rooted at *subjslot (re-reads after alloc).
 * b<0 → nil (unmatched group). */
static RESULT korb_re_slice(CTX *c, VALUE *slots, VALUE *subjslot, long b, long e) {
    if (b < 0) return RESULT_OK(KORB_NIL);
    return korb_str_slice_new(c, slots, VALUE_REF_AT(subjslot), (uint32_t)b, (uint32_t)(e - b));
}

/* byte offset → character index within `s` (UTF-8). */
static long korb_re_bchar(const KorbString *s, long boff) {
    if (boff <= 0) return 0;
    if (boff >= (long)s->len) boff = (long)s->len;
    return (long)korb_utf8_count(s->buf->data, (uint32_t)boff);
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

/* ---- MatchData helpers --------------------------------------------------- */
static long korb_md_off(const KorbMatchData *md, int i, int which) {
    const KorbArray *a = VAL2ARY(md->offsets);
    const uint32_t idx = (uint32_t)(2 * i + which);
    if (idx >= a->len) return -1;
    return FIX2LONG(a->items->data[idx]);
}
static int korb_md_ngroups(const KorbMatchData *md) { return (int)(VAL2ARY(md->offsets)->len / 2); }
static RESULT korb_md_group(CTX *c, VALUE *slots, VALUE mdv, int i) {
    KorbMatchData *md = VAL2MD(mdv);
    if (i < 0 || i >= korb_md_ngroups(md)) return RESULT_OK(KORB_NIL);
    const long b = korb_md_off(md, i, 0), e = korb_md_off(md, i, 1);
    if (b < 0 || e < 0) return RESULT_OK(KORB_NIL);
    slots[0] = mdv;
    const uint32_t mlen = (uint32_t)(e - b);
    KorbString *r = korb_str_alloc(c, slots + 1, mlen);
    memcpy(r->buf->data, VAL2STR(VAL2MD(slots[0])->subject)->buf->data + b, mlen);
    return RESULT_OK((VALUE)r);
}
/* resolve a named group in md's regexp → group number, or -1. */
static int korb_md_name_idx(CTX *c, VALUE mdv, const char *name, uint32_t nlen) {
    KorbMatchData *md = VAL2MD(mdv);
    if (!KORB_REGEXP_P(md->regexp)) return -1;
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (nf == NULL) return -1;
    const KorbString *pat = VAL2STR(VAL2RE(md->regexp)->source);
    const uint32_t flags = VAL2RE(md->regexp)->flags;
    for (int k = 0; ; k++) {
        int gi = -1;
        const char *gn = nf(pat->buf->data, pat->len, flags, k, &gi);
        if (gn == NULL) break;
        if (strlen(gn) == nlen && memcmp(gn, name, nlen) == 0) return gi;
    }
    return -1;
}
static RESULT korb_m_md_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE mdv = VALUE_REF_GET(self);
    const VALUE k = VALUE_SLICE_GET(a, 0);
    if (SYMBOL_P(k) || KORB_STRING_P(k)) {
        const char *nm; uint32_t nl;
        if (SYMBOL_P(k)) { nm = korb_sym_name(c->vm, SYM2ID(k)); nl = (uint32_t)strlen(nm); }
        else { nm = VAL2STR(k)->buf->data; nl = VAL2STR(k)->len; }
        int gi = korb_md_name_idx(c, mdv, nm, nl);
        if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        return korb_md_group(c, slots, mdv, gi);
    }
    intptr_t i = 0;
    if (!korb_to_index(k, &i)) return RESULT_OK(KORB_NIL);
    const int n = korb_md_ngroups(VAL2MD(mdv));
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
        intptr_t i = 0; VALUE g = KORB_NIL;
        if (korb_to_index(VALUE_SLICE_GET(a, j), &i)) { if (i < 0) i += n; if (i >= 0 && i < n) g = UNWRAP(korb_md_group(c, slots + 2, slots[0], (int)i)); }
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
static RESULT korb_m_md_begin(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; intptr_t i = 0; korb_to_index(VALUE_SLICE_GET(a, 0), &i);
    KorbMatchData *md = VAL2MD(VALUE_REF_GET(self)); const long b = korb_md_off(md, (int)i, 0);
    return RESULT_OK(b < 0 ? KORB_NIL : LONG2FIX(korb_re_bchar(VAL2STR(md->subject), b)));
}
static RESULT korb_m_md_end(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; intptr_t i = 0; korb_to_index(VALUE_SLICE_GET(a, 0), &i);
    KorbMatchData *md = VAL2MD(VALUE_REF_GET(self)); const long e = korb_md_off(md, (int)i, 1);
    return RESULT_OK(e < 0 ? KORB_NIL : LONG2FIX(korb_re_bchar(VAL2STR(md->subject), e)));
}
static RESULT korb_m_md_offset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t i = 0; korb_to_index(VALUE_SLICE_GET(a, 0), &i);
    KorbMatchData *md = VAL2MD(VALUE_REF_GET(self));
    const long b = korb_md_off(md, (int)i, 0), e = korb_md_off(md, (int)i, 1);
    slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
    if (b < 0) { CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), KORB_NIL)); CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), KORB_NIL)); }
    else { const KorbString *s = VAL2STR(VAL2MD(slots[0])->subject);
           CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), LONG2FIX(korb_re_bchar(s, b))));
           CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), LONG2FIX(korb_re_bchar(s, e)))); }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_md_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(korb_md_ngroups(VAL2MD(VALUE_REF_GET(self))))); }
static RESULT korb_m_md_string(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self); const KorbString *s = VAL2STR(VAL2MD(slots[0])->subject);
    return korb_str_new(c, slots + 1, s->buf->data, s->len);
}
static RESULT korb_m_md_regexp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VAL2MD(VALUE_REF_GET(self))->regexp); }
static RESULT korb_md_names_into(CTX *c, VALUE *slots, VALUE mdv_or_re, bool is_md, VALUE_REF dst_ary) {
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    VALUE rev = is_md ? VAL2MD(mdv_or_re)->regexp : mdv_or_re;
    if (!nf || !KORB_REGEXP_P(rev)) return RESULT_OK(VALUE_REF_GET(dst_ary));
    slots[0] = rev;
    const KorbString *pat = VAL2STR(VAL2RE(slots[0])->source); const uint32_t flags = VAL2RE(slots[0])->flags;
    for (int k = 0; ; k++) {
        int gi = -1; const char *gn = nf(pat->buf->data, pat->len, flags, k, &gi);
        if (!gn) break;
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, gn, (uint32_t)strlen(gn)));
        CHECK(korb_ary_push_val(c, slots + 2, dst_ary, slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst_ary));
}
static RESULT korb_m_md_names(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    return korb_md_names_into(c, slots + 2, slots[0], true, VALUE_REF_AT(&slots[1]));
}
static RESULT korb_m_md_named_captures(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_hash_new(c, slots + 1, 0));
    KorbMatchData *md = VAL2MD(slots[0]);
    korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
    if (nf && KORB_REGEXP_P(md->regexp)) {
        const KorbString *pat = VAL2STR(VAL2RE(md->regexp)->source); const uint32_t flags = VAL2RE(md->regexp)->flags;
        for (int k = 0; ; k++) {
            int gi = -1; const char *gn = nf(pat->buf->data, pat->len, flags, k, &gi);
            if (!gn) break;
            slots[2] = UNWRAP(korb_str_new(c, slots + 2, gn, (uint32_t)strlen(gn)));
            slots[3] = UNWRAP(korb_md_group(c, slots + 3, slots[0], gi));
            CHECK(korb_hash_set(c, slots + 4, VALUE_REF_AT(&slots[1]), VALUE_REF_AT(&slots[2]), slots[3]));
        }
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
    if (!KORB_REGEXP_P(re) || !KORB_STRING_P(str)) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[0] = re; slots[1] = str;
    korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[0], slots[1], 0, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    const long cidx = korb_re_bchar(VAL2STR(slots[1]), m.starts[0]);
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[1], slots[0], &m));
    korb_re_set_lastmatch(c, slots[2]);
    return RESULT_OK(LONG2FIX(cidx));
}
static RESULT korb_m_str_match_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_re_match_set(c, slots, VALUE_SLICE_GET(a, 0), VALUE_REF_GET(self)); }
static RESULT korb_m_re_match_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_re_match_set(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)); }
static RESULT korb_m_re_match_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* match? — no $~ */
    VALUE subj; slots[0] = VALUE_REF_GET(self);
    if (UNWRAP(korb_re_subject(c, slots + 1, VALUE_SLICE_GET(a, 0), &subj)) != KORB_TRUE) return RESULT_OK(KORB_FALSE);
    slots[1] = subj; korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[0], slots[1], 0, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    return RESULT_OK(rr.value == KORB_TRUE ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_re_case_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* === (sets $~) */
    RESULT r = korb_re_match_set(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0));
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(r.value == KORB_NIL ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_re_source(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VAL2RE(VALUE_REF_GET(self))->source); }
static int korb_re_ruby_opts(uint32_t flags) { int o = 0; if (flags & 4u) o |= 1; if (flags & 8u) o |= 2; if (flags & 16u) o |= 4; return o; }
static RESULT korb_m_re_options(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(korb_re_ruby_opts(VAL2RE(VALUE_REF_GET(self))->flags))); }
static RESULT korb_m_re_casefold(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK((VAL2RE(VALUE_REF_GET(self))->flags & 4u) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_re_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self);
    const uint32_t f = VAL2RE(slots[0])->flags; const KorbString *src = VAL2STR(VAL2RE(slots[0])->source);
    char on[4]; int no = 0; char off[4]; int nf = 0;
    if (f & 16u) on[no++]='m'; else off[nf++]='m';
    if (f & 4u)  on[no++]='i'; else off[nf++]='i';
    if (f & 8u)  on[no++]='x'; else off[nf++]='x';
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    fputs("(?", ms); fwrite(on, 1, (size_t)no, ms);
    if (nf) { fputc('-', ms); fwrite(off, 1, (size_t)nf, ms); }
    fputc(':', ms); fwrite(src->buf->data, 1, src->len, ms); fputc(')', ms);
    fclose(ms); RESULT r = korb_str_new(c, slots + 1, buf, (uint32_t)z); free(buf); return r;
}
static RESULT korb_m_re_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self);
    const uint32_t f = VAL2RE(slots[0])->flags; const KorbString *src = VAL2STR(VAL2RE(slots[0])->source);
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    fputc('/', ms); fwrite(src->buf->data, 1, src->len, ms); fputc('/', ms);
    if (f & 16u) fputc('m', ms);
    if (f & 4u) fputc('i', ms);
    if (f & 8u) fputc('x', ms);
    fclose(ms); RESULT r = korb_str_new(c, slots + 1, buf, (uint32_t)z); free(buf); return r;
}
static RESULT korb_m_re_names(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = VALUE_REF_GET(self); slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    return korb_md_names_into(c, slots + 2, slots[0], false, VALUE_REF_AT(&slots[1]));
}
static RESULT korb_m_re_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; const VALUE o = VALUE_SLICE_GET(a, 0), s = VALUE_REF_GET(self);
    if (!KORB_REGEXP_P(o) || VAL2RE(s)->flags != VAL2RE(o)->flags) return RESULT_OK(KORB_FALSE);
    const KorbString *a1 = VAL2STR(VAL2RE(s)->source), *b1 = VAL2STR(VAL2RE(o)->source);
    if (a1->len != b1->len || memcmp(a1->buf->data, b1->buf->data, a1->len) != 0) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_re_hash(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; const KorbString *src = VAL2STR(VAL2RE(VALUE_REF_GET(self))->source);
    uint32_t h = korb_str_hash(src->buf->data, src->len) ^ (VAL2RE(VALUE_REF_GET(self))->flags * 2654435761u);
    return RESULT_OK(LONG2FIX((long)h));
}
/* Regexp#match(str[,pos]) → MatchData|nil (sets $~); block form yields it. */
static RESULT korb_m_re_match(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = VALUE_REF_GET(self); VALUE subj;
    if (VALUE_SLICE_GET(a, 0) == KORB_NIL) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    if (UNWRAP(korb_re_subject(c, slots + 2, VALUE_SLICE_GET(a, 0), &subj)) != KORB_TRUE)
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(VALUE_SLICE_GET(a, 0)));
    slots[1] = subj;
    long startc = 0; if (VALUE_SLICE_LEN(a) >= 2) { intptr_t p = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &p)) startc = (long)p; }
    const KorbString *s = VAL2STR(slots[1]);
    if (startc < 0) startc += (long)korb_utf8_count(s->buf->data, s->len);
    size_t startb = (startc <= 0) ? 0 : korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)startc);
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
    if (KORB_STRING_P(pv)) { slots[0] = pv; *out = UNWRAP(korb_regexp_new(c, slots + 1, slots[0], 0)); return RESULT_OK(KORB_TRUE); }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_re_arg_type(pv));
}
static RESULT korb_m_str_match(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    VALUE subj;
    if (UNWRAP(korb_re_subject(c, slots, VALUE_REF_GET(self), &subj)) != KORB_TRUE) return RESULT_OK(KORB_NIL);
    slots[0] = subj; VALUE re; RESULT cr = korb_re_coerce_pat(c, slots + 1, VALUE_SLICE_GET(a, 0), &re);
    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    slots[1] = re;
    long startc = 0; if (VALUE_SLICE_LEN(a) >= 2) { intptr_t p = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &p) && p > 0) startc = (long)p; }
    const KorbString *s = VAL2STR(slots[0]); size_t startb = (startc <= 0) ? 0 : korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)startc);
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
    long startc = 0; if (VALUE_SLICE_LEN(a) >= 2) { intptr_t p = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &p) && p > 0) startc = (long)p; }
    const KorbString *s = VAL2STR(slots[0]); size_t startb = (startc <= 0) ? 0 : korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)startc);
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
        memcpy(r->buf->data, VAL2STR(slots[0])->buf->data + m->starts[0], ml);
        return RESULT_OK((VALUE)r);
    }
    slots[0] = mdv; slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)m->n_groups));
    for (int i = 1; i <= m->n_groups; i++) { slots[2] = UNWRAP(korb_md_group(c, slots + 2, slots[0], i)); CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[1]), slots[2])); }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_str_scan(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    VALUE re; RESULT cr = korb_re_coerce_pat(c, slots, VALUE_SLICE_GET(a, 0), &re);
    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
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
        if (block) { RESULT yr = korb_block_yield(c, slots + 5, block, def_env, &slots[4], 1, cself); if (UNLIKELY(yr.state != KORB_NORMAL)) return yr; }
        else CHECK(korb_ary_push_val(c, slots + 5, VALUE_REF_AT(&slots[2]), slots[4]));
        off = (m.ends[0] > m.starts[0]) ? m.ends[0] : m.starts[0] + 1;
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
            else if (nx == '`') { fwrite(subj, 1, (size_t)m->starts[0], ms); i++; continue; }
            else if (nx == '\'') { fwrite(subj + m->ends[0], 1, (size_t)(slen - m->ends[0]), ms); i++; continue; }
            else if (nx == '\\') { fputc('\\', ms); i++; continue; }
            else if (nx == 'k' && i + 2 < rn && rep[i + 2] == '<') {
                uint32_t j = i + 3; while (j < rn && rep[j] != '>') j++;
                int gi = -1; korb_re_named_fn_t nf = (korb_re_named_fn_t)c->vm->re_named_fn;
                if (nf && KORB_REGEXP_P(re)) { const KorbString *pat = VAL2STR(VAL2RE(re)->source); const uint32_t fl = VAL2RE(re)->flags;
                    for (int k = 0; ; k++) { int idx = -1; const char *gn = nf(pat->buf->data, pat->len, fl, k, &idx); if (!gn) break;
                        if (strlen(gn) == j - (i + 3) && memcmp(gn, rep + i + 3, j - (i + 3)) == 0) { gi = idx; break; } } }
                if (gi >= 0 && gi <= m->n_groups && m->starts[gi] >= 0) fwrite(subj + m->starts[gi], 1, (size_t)(m->ends[gi] - m->starts[gi]), ms);
                i = j; continue;
            }
        }
        fputc(rep[i], ms);
    }
}
RESULT korb_re_str_gsub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, VALUE re, bool global, bool in_place, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = VALUE_REF_GET(self); slots[1] = re;
    const KorbString *s0 = VAL2STR(slots[0]); const uint32_t sn = s0->len;
    char *const src = malloc(sn ? sn : 1); memcpy(src, s0->buf->data, sn);
    char *rep = NULL; uint32_t rn = 0; VALUE hashrep = KORB_NIL;
    if (block == NULL) {
        if (VALUE_SLICE_LEN(a) < 2) { free(src); return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "String#gsub/sub without a replacement is not supported"); }
        VALUE rv = VALUE_SLICE_GET(a, 1);
        if (KORB_STRING_P(rv)) { const KorbString *rs = VAL2STR(rv); rn = rs->len; rep = malloc(rn ? rn : 1); memcpy(rep, rs->buf->data, rn); }
        else if (KORB_HASH_P(rv)) { hashrep = rv; slots[2] = rv; }
        else { free(src); return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(rv)); }
    }
    char *buf = NULL; size_t bz = 0; FILE *ms = open_memstream(&buf, &bz);
    long off = 0; bool replaced = false;
    while (off <= (long)sn) {
        korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 3, slots[1], slots[0], (size_t)off, &m);
        if (UNLIKELY(rr.state != KORB_NORMAL)) { free(src); free(rep); fclose(ms); free(buf); return rr; }
        if (rr.value != KORB_TRUE) break;
        const long ms0 = m.starts[0], me0 = m.ends[0];
        fwrite(src + off, 1, (size_t)(ms0 - off), ms);
        if (block) {
            slots[3] = UNWRAP(korb_re_build_md(c, slots + 3, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[3]);
            slots[4] = UNWRAP(korb_md_group(c, slots + 4, slots[3], 0));
            RESULT yr = korb_block_yield(c, slots + 5, block, def_env, &slots[4], 1, cself);
            if (UNLIKELY(yr.state != KORB_NORMAL)) { free(src); fclose(ms); free(buf); return yr; }
            slots[4] = yr.value;
            if (KORB_STRING_P(slots[4])) { const KorbString *r = VAL2STR(slots[4]); fwrite(r->buf->data, 1, r->len, ms); }
            else korb_fprint_to_s(c, ms, slots[4]);
        } else if (hashrep != KORB_NIL) {
            const uint32_t ml = (uint32_t)(me0 - ms0);
            slots[3] = UNWRAP(korb_str_new(c, slots + 3, src + ms0, ml));
            int32_t idx = korb_hash_find(VAL2HASH(slots[2]), slots[3]);
            if (idx >= 0) korb_fprint_to_s(c, ms, VAL2HASH(slots[2])->items->data[2 * idx + 1]);
        } else {
            korb_re_expand_repl(c, ms, rep, rn, src, sn, &m, slots[1]);
        }
        replaced = true;
        if (me0 > ms0) off = me0;
        else { if (ms0 < (long)sn) { uint32_t cl = korb_utf8_seq_len((const unsigned char *)src, (uint32_t)ms0, sn); if (!cl) cl = 1; fwrite(src + ms0, 1, cl, ms); off = ms0 + cl; } else off = ms0 + 1; }
        if (!global) break;
    }
    if (off <= (long)sn) fwrite(src + off, 1, (size_t)(sn - off), ms);
    fclose(ms); free(src); free(rep);
    RESULT nr = korb_str_new(c, slots + 3, buf ? buf : "", (uint32_t)bz); free(buf);
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    if (!in_place) return nr;
    slots[3] = nr.value; const KorbString *res = VAL2STR(slots[3]); const uint32_t w = res->len;
    KorbString *s2 = korb_str_ensure(c, slots + 4, self, w); res = VAL2STR(slots[3]);
    memcpy(s2->buf->data, res->buf->data, w); s2->len = w; s2->buf->data[w] = '\0';
    return RESULT_OK(replaced ? VALUE_REF_GET(self) : KORB_NIL);
}

/* ---- String#split with a Regexp (forward-declared for string.c) ---------- */
RESULT korb_re_str_split(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, long limit) {
    slots[0] = VALUE_REF_GET(self); slots[1] = re; slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 0));
    VALUE_REF res = VALUE_REF_AT(&slots[2]);
    long last = 0, off = 0, count = 0;
    for (;;) {
        if (limit > 0 && count == limit - 1) break;
        const KorbString *s = VAL2STR(slots[0]); if (off > (long)s->len) break;
        korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 3, slots[1], slots[0], (size_t)off, &m);
        if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
        if (rr.value != KORB_TRUE) break;
        long ms0 = m.starts[0], me0 = m.ends[0];
        if (me0 == ms0) {
            if (ms0 >= (long)VAL2STR(slots[0])->len) break;
            const KorbString *s2 = VAL2STR(slots[0]);
            uint32_t cl = korb_utf8_seq_len((const unsigned char *)s2->buf->data, (uint32_t)ms0, s2->len); if (!cl) cl = 1;
            if ((long)(ms0 + cl) <= last) { off = ms0 + cl; continue; }
            slots[3] = UNWRAP(korb_re_slice(c, slots + 3, &slots[0], last, ms0 + cl));
            CHECK(korb_ary_push_val(c, slots + 4, res, slots[3])); count++;
            last = ms0 + cl; off = ms0 + cl; continue;
        }
        slots[3] = UNWRAP(korb_re_slice(c, slots + 3, &slots[0], last, ms0));
        CHECK(korb_ary_push_val(c, slots + 4, res, slots[3])); count++;
        for (int g = 1; g <= m.n_groups; g++) { slots[3] = UNWRAP(korb_re_slice(c, slots + 3, &slots[0], m.starts[g], m.ends[g])); CHECK(korb_ary_push_val(c, slots + 4, res, slots[3])); }
        last = me0; off = me0;
    }
    slots[3] = UNWRAP(korb_re_slice(c, slots + 3, &slots[0], last, (long)VAL2STR(slots[0])->len));
    CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
    if (limit == 0) {
        KorbArray *ra = VAL2ARY(VALUE_REF_GET(res));
        while (ra->len > 0 && KORB_STRING_P(ra->items->data[ra->len - 1]) && VAL2STR(ra->items->data[ra->len - 1])->len == 0) { ra->len--; ra = VAL2ARY(VALUE_REF_GET(res)); }
    }
    return RESULT_OK(VALUE_REF_GET(res));
}

/* ---- String#[] / index with a Regexp (forward-declared) ------------------ */
RESULT korb_re_str_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, VALUE group_or_nil) {
    slots[0] = VALUE_REF_GET(self); slots[1] = re; korb_re_match_t m;
    RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], 0, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[2]);
    int gi = 0;
    if (group_or_nil != KORB_NIL) {
        if (SYMBOL_P(group_or_nil) || KORB_STRING_P(group_or_nil)) {
            const char *nm; uint32_t nl;
            if (SYMBOL_P(group_or_nil)) { nm = korb_sym_name(c->vm, SYM2ID(group_or_nil)); nl = (uint32_t)strlen(nm); } else { nm = VAL2STR(group_or_nil)->buf->data; nl = VAL2STR(group_or_nil)->len; }
            gi = korb_md_name_idx(c, slots[2], nm, nl);
            if (gi < 0) return korb_raise(c, slots, KORB_E_INDEX, 0, "undefined group name reference: %.*s", (int)nl, nm);
        } else { intptr_t g = 0; korb_to_index(group_or_nil, &g); gi = (int)g; }
    }
    return korb_md_group(c, slots, slots[2], gi);
}
RESULT korb_re_str_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, long startc) {
    slots[0] = VALUE_REF_GET(self); slots[1] = re;
    const KorbString *s = VAL2STR(slots[0]);
    if (startc < 0) startc += (long)korb_utf8_count(s->buf->data, s->len);
    if (startc < 0) return RESULT_OK(KORB_NIL);
    size_t startb = (startc == 0) ? 0 : korb_utf8_byteoff(s->buf->data, s->len, (uint32_t)startc);
    korb_re_match_t m; RESULT rr = korb_re_run(c, slots + 2, slots[1], slots[0], startb, &m);
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (rr.value != KORB_TRUE) { korb_re_set_lastmatch(c, KORB_NIL); return RESULT_OK(KORB_NIL); }
    slots[2] = UNWRAP(korb_re_build_md(c, slots + 2, slots[0], slots[1], &m)); korb_re_set_lastmatch(c, slots[2]);
    return RESULT_OK(LONG2FIX(korb_re_bchar(VAL2STR(slots[0]), m.starts[0])));
}

/* ---- Regexp class methods ------------------------------------------------ */
static RESULT korb_m_re_escape(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; VALUE v = VALUE_SLICE_GET(a, 0);
    if (SYMBOL_P(v)) { const char *nm = korb_sym_name(c->vm, SYM2ID(v)); slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm))); }
    else if (KORB_STRING_P(v)) slots[0] = v;
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(v));
    const KorbString *s = VAL2STR(slots[0]);
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    for (uint32_t i = 0; i < s->len; i++) { unsigned char ch = (unsigned char)s->buf->data[i];
        if (strchr("\\.*+?()[]{}|-^$", ch)) { fputc('\\', ms); fputc(ch, ms); }
        else if (ch == '\n') fputs("\\n", ms); else if (ch == '\r') fputs("\\r", ms);
        else if (ch == '\t') fputs("\\t", ms); else if (ch == '\f') fputs("\\f", ms);
        else if (ch == ' ') fputs("\\ ", ms); else fputc(ch, ms); }
    fclose(ms); RESULT r = korb_str_new(c, slots + 1, buf ? buf : "", (uint32_t)z); free(buf); return r;
}
static RESULT korb_m_re_new(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; VALUE src = VALUE_SLICE_GET(a, 0); uint32_t flags = 0;
    if (KORB_REGEXP_P(src)) { slots[0] = VAL2RE(src)->source; flags = VAL2RE(src)->flags; }
    else if (KORB_STRING_P(src)) slots[0] = src;
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(src));
    if (VALUE_SLICE_LEN(a) >= 2) {
        VALUE opt = VALUE_SLICE_GET(a, 1);
        if (opt == KORB_TRUE) flags |= 4u;
        else if (opt == KORB_NIL || opt == KORB_FALSE) {}
        else if (FIXNUM_P(opt)) { long o = FIX2LONG(opt); if (o & 1) flags |= 4u; if (o & 2) flags |= 8u; if (o & 4) flags |= 16u; }
        else flags |= 4u;
    }
    (void)korb_re_load(c->vm);
    korb_re_valid_fn_t vf = (korb_re_valid_fn_t)c->vm->re_valid_fn;
    if (vf) { const KorbString *ps = VAL2STR(slots[0]); if (!vf(ps->buf->data, ps->len, flags)) return korb_raise(c, slots, KORB_E_REGEXP, 0, "invalid regular expression"); }
    return korb_regexp_new(c, slots + 1, slots[0], flags);
}
static RESULT korb_m_re_union(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; slots[0] = KORB_NIL;
    char *buf = NULL; size_t z = 0; FILE *ms = open_memstream(&buf, &z);
    uint32_t n = VALUE_SLICE_LEN(a); VALUE items = KORB_NIL;
    if (n == 1 && KORB_ARRAY_P(VALUE_SLICE_GET(a, 0))) { items = VALUE_SLICE_GET(a, 0); n = VAL2ARY(items)->len; }
    if (n == 0) fputs("(?!)", ms);
    for (uint32_t i = 0; i < n; i++) {
        VALUE it = (items != KORB_NIL) ? VAL2ARY(items)->items->data[i] : VALUE_SLICE_GET(a, i);
        if (i) fputc('|', ms);
        if (KORB_REGEXP_P(it)) { const KorbString *s = VAL2STR(VAL2RE(it)->source); fwrite(s->buf->data, 1, s->len, ms); }
        else if (KORB_STRING_P(it)) { const KorbString *s = VAL2STR(it);
            for (uint32_t j = 0; j < s->len; j++) { unsigned char ch = (unsigned char)s->buf->data[j]; if (strchr("\\.*+?()[]{}|-^$", ch)) fputc('\\', ms); fputc(ch, ms); } }
        else { fclose(ms); free(buf); return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_re_arg_type(it)); }
    }
    fclose(ms); slots[0] = UNWRAP(korb_str_new(c, slots, buf ? buf : "", (uint32_t)z)); free(buf);
    return korb_regexp_new(c, slots + 1, slots[0], 0);
}
static RESULT korb_m_re_last_match(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; VALUE md = korb_re_get_lastmatch(c);
    if (VALUE_SLICE_LEN(a) == 0) return RESULT_OK(md == 0 ? KORB_NIL : md);
    if (md == 0 || md == KORB_NIL || !KORB_MATCHDATA_P(md)) return RESULT_OK(KORB_NIL);
    intptr_t i = 0; korb_to_index(VALUE_SLICE_GET(a, 0), &i);
    return korb_md_group(c, slots, md, (int)i);
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
    slots[1] = korb_obj_singleton(c, slots + 1, slots[0]).value;
    korb_class_def_cfn(c, slots[1], "escape", korb_m_re_escape, 1);
    korb_class_def_cfn(c, slots[1], "quote",  korb_m_re_escape, 1);
    korb_class_def_cfn(c, slots[1], "new",     korb_m_re_new, -1);
    korb_class_def_cfn(c, slots[1], "compile", korb_m_re_new, -1);
    korb_class_def_cfn(c, slots[1], "union",   korb_m_re_union, -1);
    korb_class_def_cfn(c, slots[1], "last_match", korb_m_re_last_match, -1);
}
