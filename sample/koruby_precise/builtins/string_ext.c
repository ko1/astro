/* koruby_precise — string_ext.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- more String methods ------------------------------------------------- */

/* String#% : printf-style formatting. Single arg or an Array of args.
 * Supports d/i/u, f/e/E/g/G, x/X/o, b, s, c, p, %% with C flags/width/precision
 * (binary `b` honors width/0-flag manually; `*` dynamic width is unsupported). */
static RESULT korb_m_str_format(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbString *fs = VAL2STR(VALUE_REF_GET(self));
    const char *fmt = fs->buf->data; uint32_t flen = fs->len;
    VALUE single = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    const VALUE *args; uint32_t argn;
    if (KORB_ARRAY_P(single)) { args = VAL2ARY(single)->items->data; argn = VAL2ARY(single)->len; }
    else { args = &single; argn = VALUE_SLICE_LEN(a); }
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    uint32_t ai = 0; bool err = false; const char *errmsg = NULL;
    for (uint32_t i = 0; i < flen; i++) {
        if (fmt[i] != '%') { fputc(fmt[i], ms); continue; }
        char spec[64]; int si = 0; spec[si++] = '%';
        i++;
        if (i < flen && fmt[i] == '%') { fputc('%', ms); continue; }
        while (i < flen && strchr("-+ 0#", fmt[i])) { if (si < 58) spec[si++] = fmt[i]; i++; }
        while (i < flen && isdigit((unsigned char)fmt[i])) { if (si < 58) spec[si++] = fmt[i]; i++; }
        if (i < flen && fmt[i] == '.') { if (si < 58) spec[si++] = '.'; i++; while (i < flen && isdigit((unsigned char)fmt[i])) { if (si < 58) spec[si++] = fmt[i]; i++; } }
        if (i >= flen) { err = true; errmsg = "malformed format sequence"; break; }
        char conv = fmt[i];
        VALUE arg = (ai < argn) ? args[ai] : KORB_NIL;
        switch (conv) {
          case 'd': case 'i': case 'u': {
            intptr_t v;
            if (FIXNUM_P(arg)) v = FIX2LONG(arg);
            else if (KORB_FLOAT_P(arg)) v = (intptr_t)VAL2FLT(arg)->val;
            else { err = true; errmsg = "expected a number"; break; }
            spec[si++] = 'l'; spec[si++] = 'd'; spec[si] = '\0';
            fprintf(ms, spec, (long)v); ai++;
            break;
          }
          case 'f': case 'e': case 'E': case 'g': case 'G': {
            double v; if (!korb_num_to_d(arg, &v)) { err = true; errmsg = "expected a number"; break; }
            spec[si++] = conv; spec[si] = '\0';
            fprintf(ms, spec, v); ai++;
            break;
          }
          case 'x': case 'X': case 'o': {
            intptr_t v; if (FIXNUM_P(arg)) v = FIX2LONG(arg); else { err = true; errmsg = "expected Integer"; break; }
            spec[si++] = 'l'; spec[si++] = conv; spec[si] = '\0';
            fprintf(ms, spec, (long)v); ai++;
            break;
          }
          case 'b': case 'B': {
            intptr_t v; if (FIXNUM_P(arg)) v = FIX2LONG(arg); else { err = true; errmsg = "expected Integer"; break; }
            bool left = false, zero = false; int width = 0;          /* parse flags/width from spec */
            for (int k = 1; k < si; k++) {
                char sc = spec[k];
                if (sc == '-') left = true;
                else if (sc == '0') zero = true;
                else if (sc == '+' || sc == ' ' || sc == '#') { /* ignored for binary */ }
                else if (isdigit((unsigned char)sc)) width = width * 10 + (sc - '0');
                else break;
            }
            char tmp[80]; uint32_t n = korb_fmt_int(v, 2, tmp);
            int pad = width > (int)n ? width - (int)n : 0;
            if (!left) { char padc = zero ? '0' : ' '; for (int p = 0; p < pad; p++) fputc(padc, ms); }
            fwrite(tmp, 1, n, ms);
            if (left) for (int p = 0; p < pad; p++) fputc(' ', ms);
            ai++;
            break;
          }
          case 's': {
            spec[si++] = 's'; spec[si] = '\0';
            if (KORB_STRING_P(arg)) { fprintf(ms, spec, VAL2STR(arg)->buf->data); }
            else {
                char *tb = NULL; size_t tsz = 0; FILE *tms = open_memstream(&tb, &tsz);
                if (tms) { korb_fprint_to_s(c, tms, arg); fclose(tms); }
                fprintf(ms, spec, tb ? tb : ""); free(tb);
            }
            ai++;
            break;
          }
          case 'p': {
            char *tb = NULL; size_t tsz = 0; FILE *tms = open_memstream(&tb, &tsz);
            if (tms) { korb_fprint_inspect(c, tms, arg); fclose(tms); }
            spec[si++] = 's'; spec[si] = '\0';
            fprintf(ms, spec, tb ? tb : ""); free(tb); ai++;
            break;
          }
          case 'c': {
            if (FIXNUM_P(arg)) fputc((int)FIX2LONG(arg), ms);
            else if (KORB_STRING_P(arg) && VAL2STR(arg)->len > 0) fwrite(VAL2STR(arg)->buf->data, 1, 1, ms);
            ai++;
            break;
          }
          default: err = true; errmsg = "malformed format sequence"; break;
        }
        if (err) break;
    }
    fclose(ms);
    if (err) { free(buf); return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "%s", errmsg ? errmsg : "format error"); }
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}

RESULT korb_str_mod(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs) {
    slots[0] = rhs;
    return korb_m_str_format(c, slots + 1, lhs, VALUE_SLICE_MAKE(slots, 1));
}

static int korb_ci_cmp(const char *a, uint32_t al, const char *b, uint32_t bl) {
    uint32_t m = al < bl ? al : bl;
    for (uint32_t i = 0; i < m; i++) {
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return (al > bl) - (al < bl);
}
static RESULT korb_m_str_casecmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(o)) return RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *t = VAL2STR(o);
    return RESULT_OK(LONG2FIX(korb_ci_cmp(s->buf->data, s->len, t->buf->data, t->len)));
}
static RESULT korb_m_str_casecmp_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(o)) return RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *t = VAL2STR(o);
    return RESULT_OK(korb_ci_cmp(s->buf->data, s->len, t->buf->data, t->len) == 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_str_byteslice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t bn = s->len;
    VALUE iv = VALUE_SLICE_GET(a, 0);
    if (KORB_RANGE_P(iv)) {                            /* byteslice(range) */
        const KorbRange *r = VAL2RANGE(iv);
        intptr_t b = 0, e;
        if (r->rbegin != KORB_NIL && UNLIKELY(!korb_to_index(r->rbegin, &b))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (b < 0) b += bn;
        if (b < 0 || b > (intptr_t)bn) return RESULT_OK(KORB_NIL);
        if (r->rend == KORB_NIL) e = bn;
        else { if (UNLIKELY(!korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); if (e < 0) e += bn; if (!r->exclude_end) e += 1; }
        intptr_t len = e - b; if (len < 0) len = 0; if (b + len > (intptr_t)bn) len = (intptr_t)bn - b;
        return korb_str_slice_new(c, slots, self, (uint32_t)b, (uint32_t)len);
    }
    intptr_t i;
    if (UNLIKELY(!korb_to_index(iv, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
    if (i < 0) i += bn;
    const bool two_arg = VALUE_SLICE_LEN(a) >= 2;
    if (i < 0 || i > (intptr_t)bn || (!two_arg && i == (intptr_t)bn)) return RESULT_OK(KORB_NIL);   /* byteslice(i): nil at end */
    intptr_t lentmp;
    intptr_t len = (two_arg && korb_to_index(VALUE_SLICE_GET(a, 1), &lentmp)) ? lentmp : 1;
    if (len < 0) return RESULT_OK(KORB_NIL);
    if (i + len > (intptr_t)bn) len = (intptr_t)bn - i;
    return korb_str_slice_new(c, slots, self, (uint32_t)i, (uint32_t)len);
}
/* String#insert(index, str) — insert str before the char at index (negative
 * index counts from the end, inserting after); mutates and returns self. */
static RESULT korb_m_str_insert(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t idx;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &idx))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    VALUE iv = VALUE_SLICE_GET(a, 1);
    if (UNLIKELY(!KORB_STRING_P(iv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(iv));
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t ncp = korb_utf8_count(s->buf->data, s->len);
    intptr_t pos = idx >= 0 ? idx : (intptr_t)ncp + idx + 1;
    if (UNLIKELY(pos < 0 || pos > (intptr_t)ncp)) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld out of string", (long)idx);
    uint32_t boff = korb_str_char_to_byte(s, (uint32_t)pos);
    uint32_t inn = VAL2STR(iv)->len, newlen = s->len + inn;
    char *out = malloc(newlen ? newlen : 1);
    s = VAL2STR(VALUE_REF_GET(self));
    memcpy(out, s->buf->data, boff);
    memcpy(out + boff, VAL2STR(iv)->buf->data, inn);
    memcpy(out + boff + inn, s->buf->data + boff, s->len - boff);
    KorbString *ns = korb_str_ensure(c, slots, self, newlen);
    memcpy(ns->buf->data, out, newlen); ns->len = newlen; ns->buf->data[newlen] = '\0';
    free(out);
    return RESULT_OK(VALUE_REF_GET(self));
}
/* String#bytesplice(index, length, str) / (range, str) — replace bytes in place,
 * return self. */
static RESULT korb_m_str_bytesplice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t bn = s->len;
    intptr_t start = 0, dellen = 0; VALUE repl;
    if (VALUE_SLICE_LEN(a) >= 2 && KORB_RANGE_P(VALUE_SLICE_GET(a, 0))) {
        const KorbRange *r = VAL2RANGE(VALUE_SLICE_GET(a, 0));
        if (r->rbegin != KORB_NIL && UNLIKELY(!korb_to_index(r->rbegin, &start))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (start < 0) start += bn;
        intptr_t e; if (r->rend == KORB_NIL) e = bn; else { if (UNLIKELY(!korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); if (e < 0) e += bn; if (!r->exclude_end) e += 1; }
        dellen = e - start;
        repl = VALUE_SLICE_GET(a, 1);
    } else {
        if (UNLIKELY(VALUE_SLICE_LEN(a) < 3 || !korb_to_index(VALUE_SLICE_GET(a, 0), &start) || !korb_to_index(VALUE_SLICE_GET(a, 1), &dellen)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (start < 0) start += bn;
        repl = VALUE_SLICE_GET(a, 2);
    }
    if (UNLIKELY(start < 0 || start > (intptr_t)bn)) return korb_raise(c, slots, KORB_E_RANGE, 0, "index %ld out of string", (long)start);
    if (dellen < 0) dellen = 0;
    if (start + dellen > (intptr_t)bn) dellen = (intptr_t)bn - start;
    if (UNLIKELY(!KORB_STRING_P(repl))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(repl));
    const KorbString *rs = VAL2STR(repl); uint32_t rn = rs->len;
    uint32_t sufoff = (uint32_t)(start + dellen), suflen = bn - sufoff;
    uint32_t newlen = (uint32_t)start + rn + suflen;
    char *out = malloc(newlen ? newlen : 1);                /* assemble full new content (no GC) */
    s = VAL2STR(VALUE_REF_GET(self));
    memcpy(out, s->buf->data, (size_t)start);
    memcpy(out + start, VAL2STR(repl)->buf->data, rn);
    memcpy(out + start + rn, s->buf->data + sufoff, suflen);
    KorbString *ns = korb_str_ensure(c, slots, self, newlen);   /* may move; out is libc-stable */
    memcpy(ns->buf->data, out, newlen); ns->len = newlen; ns->buf->data[newlen] = '\0';
    free(out);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_str_getbyte(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t i;
    if (!korb_to_index(VALUE_SLICE_GET(a, 0), &i)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (i < 0) i += s->len;
    if (i < 0 || (uint32_t)i >= s->len) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX((unsigned char)s->buf->data[i]));
}
static RESULT korb_m_str_setbyte(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE iv = VALUE_SLICE_GET(a, 0), bv = VALUE_SLICE_GET(a, 1);
    intptr_t i, b;                                          /* index/value coerce via to_int (Float truncates) */
    if (UNLIKELY(!korb_to_index(iv, &i) || !korb_to_index(bv, &b))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t idx = i; if (idx < 0) idx += s->len;
    if (UNLIKELY(idx < 0 || (uint32_t)idx >= s->len)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld out of string", (long)i);
    s->buf->data[idx] = (char)(b & 0xFF);
    return RESULT_OK(bv);                                   /* returns the original value argument */
}
static RESULT korb_m_sym_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm)));
    return korb_m_str_aref(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
}
static RESULT korb_m_str_succ(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_sym_succ(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm)));
    RESULT r = korb_m_str_succ(c, slots + 1, VALUE_REF_AT(&slots[0]), a);   /* succ'd string */
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    const KorbString *rs = VAL2STR(r.value);
    return RESULT_OK(ID2SYM(korb_intern(c->vm, rs->buf->data, rs->len)));
}
static RESULT korb_m_str_swapcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
/* Symbol case-fold: materialise name as String, run str transform op
 * (0=upcase 1=downcase 2=capitalize), re-intern to a Symbol. */
static RESULT korb_sym_case(CTX *c, VALUE *slots, VALUE_REF self, int op) {
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm)));
    RESULT r = korb_str_transform(c, slots + 1, VALUE_REF_AT(&slots[0]), op);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    slots[1] = r.value;   /* root the new string across the (alloc'ing) intern */
    const KorbString *rs = VAL2STR(slots[1]);
    return RESULT_OK(ID2SYM(korb_intern(c->vm, rs->buf->data, rs->len)));
}
static RESULT korb_m_sym_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!SYMBOL_P(o)) return RESULT_OK(KORB_NIL);
    int r = strcmp(korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self))), korb_sym_name(c->vm, SYM2ID(o)));
    return RESULT_OK(LONG2FIX(r < 0 ? -1 : (r > 0 ? 1 : 0)));
}
/* relational op on Symbols by name. op: 0='<' 1='<=' 2='>' 3='>=' */
static RESULT korb_m_sym_rel(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int op) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!SYMBOL_P(o))) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of Symbol with %s failed", korb_type_name(o));
    int r = strcmp(korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self))), korb_sym_name(c->vm, SYM2ID(o)));
    bool t = op == 0 ? r < 0 : op == 1 ? r <= 0 : op == 2 ? r > 0 : r >= 0;
    return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_sym_lt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_sym_rel(c, slots, self, a, 0); }
static RESULT korb_m_sym_le(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_sym_rel(c, slots, self, a, 1); }
static RESULT korb_m_sym_gt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_sym_rel(c, slots, self, a, 2); }
static RESULT korb_m_sym_ge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_sym_rel(c, slots, self, a, 3); }
static RESULT korb_m_sym_casecmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!SYMBOL_P(o)) return RESULT_OK(KORB_NIL);
    int r = strcasecmp(korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self))), korb_sym_name(c->vm, SYM2ID(o)));
    return RESULT_OK(LONG2FIX(r < 0 ? -1 : (r > 0 ? 1 : 0)));
}
static RESULT korb_m_sym_casecmp_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!SYMBOL_P(o)) return RESULT_OK(KORB_NIL);
    return RESULT_OK(strcasecmp(korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self))), korb_sym_name(c->vm, SYM2ID(o))) == 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_sym_between(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE lo = VALUE_SLICE_GET(a, 0), hi = VALUE_SLICE_GET(a, 1);
    (void)lo; (void)hi;
    return korb_num_between(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), VALUE_SLICE_GET(a, 1));
}
static RESULT korb_m_sym_clamp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE lo = VALUE_SLICE_GET(a, 0), hi = VALUE_SLICE_GET(a, 1);
    const char *s = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    if (SYMBOL_P(lo) && strcmp(s, korb_sym_name(c->vm, SYM2ID(lo))) < 0) return RESULT_OK(lo);
    if (SYMBOL_P(hi) && strcmp(s, korb_sym_name(c->vm, SYM2ID(hi))) > 0) return RESULT_OK(hi);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_sym_upcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { (void)a; return korb_sym_case(c, slots, self, 0); }
static RESULT korb_m_sym_downcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_sym_case(c, slots, self, 1); }
static RESULT korb_m_sym_capitalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_sym_case(c, slots, self, 2); }
static RESULT korb_m_sym_swapcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm)));
    RESULT r = korb_m_str_swapcase(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    slots[1] = r.value;   /* root the new string across the (alloc'ing) intern */
    const KorbString *rs = VAL2STR(slots[1]);
    return RESULT_OK(ID2SYM(korb_intern(c->vm, rs->buf->data, rs->len)));
}
static RESULT korb_m_sym_start_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm)));
    return korb_m_str_start_with(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
}
static RESULT korb_m_sym_end_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm)));
    return korb_m_str_end_with(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
}
static RESULT korb_m_str_byteindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(sv)) return RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    uint32_t off = 0;
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* byteindex(substr, start_byte) */
        intptr_t start;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &start))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        if (start < 0) start += s->len;
        if (start < 0 || start > (intptr_t)s->len) return RESULT_OK(KORB_NIL);
        off = (uint32_t)start;
    }
    int32_t b = korb_byte_find(s->buf->data + off, s->len - off, n->buf->data, n->len);
    return RESULT_OK(b < 0 ? KORB_NIL : LONG2FIX(off + (uint32_t)b));
}
static RESULT korb_m_str_byterindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(sv)) return RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    if (n->len > s->len) return RESULT_OK(KORB_NIL);
    int32_t hi = (int32_t)(s->len - n->len);
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* byterindex(substr, stop_byte) */
        intptr_t stop;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &stop))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        if (stop < 0) stop += s->len;
        if (stop < 0) return RESULT_OK(KORB_NIL);
        if (stop < hi) hi = (int32_t)stop;
    }
    for (int32_t i = hi; i >= 0; i--)
        if (memcmp(s->buf->data + i, n->buf->data, n->len) == 0) return RESULT_OK(LONG2FIX(i));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_str_chr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    if (s->len == 0) return korb_str_new(c, slots, "", 0);
    uint32_t cl = 1;                                  /* one UTF-8 codepoint */
    while (cl < s->len && ((unsigned char)s->buf->data[cl] & 0xC0) == 0x80) cl++;
    return korb_str_slice_new(c, slots, self, 0, cl);
}
/* String#ord — codepoint of the first character (UTF-8); empty → ArgumentError. */
static RESULT korb_m_str_ord(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    if (UNLIKELY(s->len == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "empty string");
    const unsigned char *d = (const unsigned char *)s->buf->data;
    unsigned char c0 = d[0]; uint32_t cp, n;
    if (c0 < 0x80)             { cp = c0;        n = 1; }
    else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; n = 2; }
    else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; n = 3; }
    else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; n = 4; }
    else                       { cp = c0;        n = 1; }   /* invalid lead → raw byte */
    for (uint32_t k = 1; k < n && k < s->len; k++) cp = (cp << 6) | (d[k] & 0x3F);
    return RESULT_OK(LONG2FIX((intptr_t)cp));
}
static RESULT korb_m_str_rindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE sv = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(sv)) return RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    if (n->len > s->len) return RESULT_OK(KORB_NIL);
    int32_t hi = (int32_t)(s->len - n->len);          /* last byte where a match can begin */
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* rindex(substr, stop): last match at/before stop */
        intptr_t stop;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &stop))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        uint32_t ncp = korb_utf8_count(s->buf->data, s->len);
        if (stop < 0) stop += ncp;
        if (stop < 0) return RESULT_OK(KORB_NIL);
        int32_t stopb = (int32_t)korb_str_char_to_byte(s, stop);
        if (stopb < hi) hi = stopb;
    }
    for (int32_t i = hi; i >= 0; i--)
        if (memcmp(s->buf->data + i, n->buf->data, n->len) == 0)
            return RESULT_OK(LONG2FIX(korb_utf8_count(s->buf->data, (uint32_t)i)));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_str_swapcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t len = VAL2STR(VALUE_REF_GET(self))->len;
    KorbString *r = korb_str_alloc(c, slots, len);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));     /* re-read after GC */
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s->buf->data[i];
        r->buf->data[i] = (char)(isupper(ch) ? tolower(ch) : islower(ch) ? toupper(ch) : ch);
    }
    return RESULT_OK((VALUE)r);
}
/* ljust(0)/rjust(1)/center(2) — char-width padding via a transient buffer */
static RESULT korb_str_pad(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE wv = VALUE_SLICE_GET(a, 0);
    intptr_t width;
    if (UNLIKELY(!korb_to_index(wv, &width))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(wv));
    const KorbString *padstr = (VALUE_SLICE_LEN(a) >= 2 && KORB_STRING_P(VALUE_SLICE_GET(a, 1))) ? VAL2STR(VALUE_SLICE_GET(a, 1)) : NULL;
    if (padstr && padstr->len == 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "zero width padding");
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t ncp = korb_utf8_count(s->buf->data, s->len);
    if (width <= (intptr_t)ncp) return korb_str_slice_new(c, slots, self, 0, s->len);
    uint32_t total_pad = (uint32_t)width - ncp;
    uint32_t left = mode == 1 ? total_pad : mode == 2 ? total_pad / 2 : 0;
    uint32_t right = total_pad - left;
    const char *pb = padstr ? padstr->buf->data : " ";
    uint32_t pl = padstr ? padstr->len : 1;
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    for (uint32_t i = 0; i < left; i++)  fputc(pb[i % pl], ms);   /* byte-cycle pad (ASCII pad exact) */
    fwrite(s->buf->data, 1, s->len, ms);
    for (uint32_t i = 0; i < right; i++) fputc(pb[i % pl], ms);
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_str_ljust(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_pad(c, slots, self, a, 0); }
static RESULT korb_m_str_rjust(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_pad(c, slots, self, a, 1); }
static RESULT korb_m_str_center(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_pad(c, slots, self, a, 2); }

static void
