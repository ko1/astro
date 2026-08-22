/* koruby_precise — string_ext.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- more String methods ------------------------------------------------- */

/* String#% : printf-style formatting. Single arg or an Array of args.
 * Supports d/i/u, f/e/E/g/G, x/X/o, b, s, c, p, %% with C flags/width/precision,
 * `*` dynamic width/precision, `N$` positional refs, and `%<name>spec` / `%{name}`
 * named refs from a Hash arg (binary `b` honors width/0-flag manually). */
/* Render an Integer (Fixnum or Bignum) in base 2/8/16 for sprintf %b/%o/%x.
 * Negatives use CRuby's two's-complement ".." notation: the digits of
 * base^(d+1) - |z| (d = digit count of |z|), prefixed with "..", which the
 * leading fill digit (f/7/1) makes an unambiguous infinite sign extension. */
static void korb_fmt_radix(FILE *ms, const korb_mp_t z, int base, bool upper,
                           bool left, bool zero, bool alt, bool plus, bool space,
                           int width, bool has_prec, int prec) {
    const bool neg = korb_mp_sgn(z) < 0;
    /* An explicit + / space flag forces a *signed* representation (-|n|);
     * otherwise a negative uses the two's-complement ".." notation. */
    const bool signed_mode = neg && (plus || space);
    const bool tc = neg && !signed_mode;
    char *digits;
    if (tc) {
        korb_mp_t a; korb_mp_init(a); korb_mp_abs(a, z);
        const size_t d = korb_mp_sizeinbase(a, base);
        korb_mp_t p; korb_mp_init(p);
        korb_mp_ui_pow_ui(p, (unsigned long)base, (unsigned long)(d + 1));
        korb_mp_sub(p, p, a);
        digits = korb_mp_get_str(NULL, base, p);          /* d+1 chars; leading run of fill digits */
        korb_mp_clear(a); korb_mp_clear(p);
        /* collapse the leading fill-digit run to a single one (CRuby keeps exactly
         * one sign digit: -256 → "f00", not "ff00"). */
        const char fd = base == 16 ? 'f' : base == 8 ? '7' : '1';
        size_t skip = 0, dl = strlen(digits);
        while (skip + 1 < dl && digits[skip] == fd && digits[skip + 1] == fd) skip++;
        if (skip) memmove(digits, digits + skip, dl - skip + 1);
    } else if (signed_mode) {
        korb_mp_t a; korb_mp_init(a); korb_mp_abs(a, z);
        digits = korb_mp_get_str(NULL, base, a);          /* magnitude only; sign added below */
        korb_mp_clear(a);
    } else {
        digits = korb_mp_get_str(NULL, base, z);
    }
    size_t dlen = strlen(digits);
    if (has_prec && prec == 0 && korb_mp_sgn(z) == 0) dlen = 0;   /* %.0x/%.0o/%.0b of 0 → no digits (CRuby) */
    const char fill = base == 16 ? 'f' : base == 8 ? '7' : '1';
    if (upper) for (size_t k = 0; k < dlen; k++) digits[k] = (char)toupper((unsigned char)digits[k]);
    const char ufill = upper ? (char)toupper((unsigned char)fill) : fill;

    /* precision = minimum digit count; CRuby counts the leading ".." within it. */
    int min_digits = 0;
    if (has_prec) min_digits = tc ? (prec > 2 ? prec - 2 : 0) : prec;
    const int pad_digits = (int)dlen < min_digits ? min_digits - (int)dlen : 0;

    char pre[2] = { 0, 0 }; int pi = 0;                  /* explicit sign character */
    if (signed_mode)      pre[pi++] = '-';
    else if (!neg) { if (plus) pre[pi++] = '+'; else if (space) pre[pi++] = ' '; }
    char altb[2] = { 0, 0 }; int altn = 0;              /* # alternate-form prefix */
    if (alt) {
        if (base == 16)     { if (korb_mp_sgn(z) != 0) { altb[0] = '0'; altb[1] = upper ? 'X' : 'x'; altn = 2; } }
        else if (base == 2) { if (korb_mp_sgn(z) != 0) { altb[0] = '0'; altb[1] = upper ? 'B' : 'b'; altn = 2; } }
        /* octal `#` means "starts with 0", so it applies to 0 as well — but only
         * when the digits do not already begin with one ("%#o" % 0 is "0", not
         * "00", while "%#.0o" % 0 renders no digits and so needs the prefix). */
        else if (!tc && (dlen == 0 || digits[0] != '0')) { altb[0] = '0'; altn = 1; }
    }
    const int dots = tc ? 2 : 0;
    const int content = pi + altn + dots + pad_digits + (int)dlen;
    int spad = 0, zpad = 0;
    if (width > content) {
        if (left)                    spad = width - content;
        else if (zero && !has_prec)  zpad = width - content;   /* pad with fill digits */
        else                         spad = width - content;
    }
    if (!left) for (int k = 0; k < spad; k++) fputc(' ', ms);
    for (int k = 0; k < pi; k++)   fputc(pre[k], ms);
    for (int k = 0; k < altn; k++) fputc(altb[k], ms);
    if (tc) { fputc('.', ms); fputc('.', ms); }
    for (int k = 0; k < zpad; k++) fputc(tc ? ufill : '0', ms);
    for (int k = 0; k < pad_digits; k++) fputc(tc ? ufill : '0', ms);
    fwrite(digits, 1, dlen, ms);
    if (left) for (int k = 0; k < spad; k++) fputc(' ', ms);
    free(digits);
}

/* Emit `nbytes` of a single %c character to `ms`, honouring width + '-' parsed
 * from `spec` (a %c pads to `width` characters, one char here). */
static void korb_fmt_emit_c(FILE *ms, const char *bytes, int nbytes, const char *spec, int si) {
    bool left = false; int width = 0;
    for (int k = 1; k < si; k++) {
        const char sc = spec[k];
        if (sc == '-') left = true;
        else if (isdigit((unsigned char)sc)) width = width * 10 + (sc - '0');
    }
    const int pad = width > 1 ? width - 1 : 0;
    if (left) { fwrite(bytes, 1, (size_t)nbytes, ms); for (int p = 0; p < pad; p++) fputc(' ', ms); }
    else      { for (int p = 0; p < pad; p++) fputc(' ', ms); fwrite(bytes, 1, (size_t)nbytes, ms); }
}

/* Coerce a format argument to an Integer exactly as Kernel#Integer would
 * (String "0x.."/"0b.."/underscores, #to_int→#to_i, Float trunc); `arg` is
 * rooted in slots[1] across the call.  Returns a Fixnum/Bignum or raises. */
static RESULT korb_fmt_integer(CTX *c, VALUE *slots, VALUE arg) {
    slots[1] = arg;
    return korb_bi_integer(c, slots + 2, VALUE_SLICE_MAKE(&slots[1], 1));
}
/* Coerce a format argument to a double as Kernel#Float would. */
static RESULT korb_fmt_float(CTX *c, VALUE *slots, VALUE arg, double *out) {
    slots[1] = arg;
    RESULT r = korb_bi_float(c, slots + 2, VALUE_SLICE_MAKE(&slots[1], 1));
    if (r.state == KORB_NORMAL) (void)korb_num_to_d(r.value, out);
    return r;
}

/* Parse a `%<name>` reference at fmt[*pi] (which must be '<').  The named
 * value comes from args[0] (a Hash).  Returns 1 and sets *out_arg on success
 * (advancing *pi past '>'); returns -1 with *out_err set on a bad-hash or
 * missing-key error.  `<name>` may appear anywhere in a directive (before or
 * after flags/width/precision), so this is called at each parse point. */
static int korb_fmt_named_arg(CTX *c, VALUE *slots, const char *fmt, uint32_t flen,
                              uint32_t *const pi, const VALUE *args, uint32_t argn,
                              VALUE *const out_arg, RESULT *const out_err) {
    uint32_t i = *pi;
    i++; const uint32_t nstart = i;                      /* caller guarantees fmt[i] == '<' */
    while (i < flen && fmt[i] != '>') i++;
    const VALUE nh = (argn >= 1) ? args[0] : KORB_NIL;
    if (i >= flen || !KORB_HASH_P(nh)) {
        *out_err = korb_raise(c, slots + 1, KORB_E_ARGUMENT, 0, "one hash required");
        return -1;
    }
    const VALUE key_sym = ID2SYM(korb_intern(c->vm, fmt + nstart, i - nstart));
    const int32_t hidx = korb_hash_find(VAL2HASH(nh), key_sym);
    if (UNLIKELY(hidx < 0)) {
        char km[256]; snprintf(km, sizeof km, "key<%.*s> not found", (int)(i - nstart), fmt + nstart);
        *out_err = korb_raise_key(c, slots + 1, nh, key_sym, km);  /* KeyError w/ #receiver + #key */
        return -1;
    }
    *out_arg = korb_items_data(VAL2HASH(nh)->items)[2 * hidx + 1];
    *pi = i + 1;                                          /* step past '>' */
    return 1;
}

static RESULT korb_m_str_format(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE single = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    if (VALUE_SLICE_LEN(a) == 1 && !KORB_ARRAY_P(single) && KORB_OBJECT_P(single)) {   /* a single non-Array arg is coerced via #to_ary (before caching fmt → GC-safe) */
        slots[0] = VALUE_REF_GET(self);                  /* root self across the dispatch */
        slots[1] = single;
        if (korb_responds_to_coerce(c, slots + 2, slots[1], korb_intern(c->vm, "to_ary", 6))) {
            const char *onm = korb_type_name(slots[1]);
            RESULT ar = korb_send_impl(c, slots + 2, korb_intern(c->vm, "to_ary", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
            if (KORB_ARRAY_P(ar.value)) slots[1] = ar.value;   /* use the array (rooted in slots[1]) */
            else if (ar.value != KORB_NIL)                     /* to_ary gave a non-Array → TypeError (CRuby) */
                return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "can't convert %s to Array (%s#to_ary gives %s)",
                                  onm, onm, korb_type_name(ar.value));
            /* to_ary → nil: keep the object itself (slots[1] auto-forwarded by GC) as the single arg */
        }
        self = VALUE_REF_AT(&slots[0]);                  /* self re-rooted after the possible GC */
        single = slots[1];                               /* array or the (GC-forwarded) object */
    }
    const KorbString *fs = VAL2STR(VALUE_REF_GET(self));
    uint32_t flen = fs->len;
    /* Copy the format string to a stack buffer so it is immune to any GC a
     * per-directive #to_int/#to_f arg coercion may trigger; a format > the buffer
     * (essentially never) keeps the raw pointer and skips that coercion. */
    char fmtbuf[1024]; const char *fmt; const bool fmt_stable = flen < sizeof(fmtbuf);
    if (fmt_stable) { memcpy(fmtbuf, korb_strbuf_data(fs->buf), flen); fmtbuf[flen] = '\0'; fmt = fmtbuf; }
    else fmt = korb_strbuf_data(fs->buf);
    slots[0] = single;                                   /* root the args source for the whole loop */
    const VALUE *args; uint32_t argn;
    #define FMT_REREAD_ARGS() do { if (KORB_ARRAY_P(slots[0])) { args = korb_items_data(VAL2ARY(slots[0])->items); argn = VAL2ARY(slots[0])->len; } else { args = &slots[0]; argn = VALUE_SLICE_LEN(a); } } while (0)
    FMT_REREAD_ARGS();
    /* Reuse the vm's cached memstream (rewind) instead of mallocing+zeroing a
     * fresh stdio buffer per call.  A re-entrant format (via a user #to_s /
     * #inspect inside %s/%p) takes its own open_memstream. */
    struct korb_vm *const vm = c->vm;
    char *buf = NULL; size_t sz = 0; FILE *ms; const bool shared = !vm->fmt_busy;
    if (shared) {
        if (!vm->fmt_stream) {
            vm->fmt_stream = open_memstream(&vm->fmt_buf, &vm->fmt_sz);
            if (!vm->fmt_stream) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
        }
        vm->fmt_busy = true;
        ms = vm->fmt_stream;
        rewind(ms);                                      /* reuse buffer from offset 0 */
    } else {
        ms = open_memstream(&buf, &sz);
        if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    }
    uint32_t ai = 0; bool err = false; const char *errmsg = NULL;
    /* the result's encoding: the format's, widened by every String argument that
     * is not plain 7-bit (CRuby's rb_enc_check per argument) */
    uint32_t renc = KORB_STR_ENC(VALUE_REF_GET(self)), enc_a = 0, enc_b = 0;
    VALUE enc_src = VALUE_REF_GET(self);
    bool enc_err = false;
    /* CRuby refuses to mix `%1$s` with `%s` in one format string; which one came
     * first decides the wording of the error. */
    bool saw_numbered = false, saw_unnumbered = false, saw_named = false;
    char mixmsg[64];
    RESULT coerce_err = RESULT_OK(KORB_NIL); bool has_coerce_err = false;   /* a #to_int/#to_f arg coercion that raised */
    const uint32_t fmt_to_int = korb_intern(c->vm, "to_int", 6);   /* %c fallback coercion */
    for (uint32_t i = 0; i < flen; i++) {
        if (fmt[i] != '%') { fputc(fmt[i], ms); continue; }
        char spec[80]; int si = 0; spec[si++] = '%';
        i++;
        if (i < flen && fmt[i] == '%') { fputc('%', ms); continue; }
        /* %<name>spec / %{name}: pull the arg from a Hash by name.  A `<name>`
         * reference may sit anywhere in the directive (before/after any of
         * flags, width, precision) — see TRY_NAMED, applied at each stage. */
        VALUE named_arg = KORB_NIL; bool has_named = false;
        #define TRY_NAMED() do { \
            if (!has_named && i < flen && fmt[i] == '<') { \
                int _nr = korb_fmt_named_arg(c, slots, fmt, flen, &i, args, argn, &named_arg, &coerce_err); \
                if (_nr < 0) { has_coerce_err = true; err = true; } \
                else if (_nr > 0) has_named = true; \
            } \
        } while (0)
        if (i < flen && fmt[i] == '{') {                   /* %{name}: to_s, no type/width/precision */
            saw_named = true;                              /* named: the Hash argument is not "unused" */
            i++; const uint32_t nstart = i;
            while (i < flen && fmt[i] != '}') i++;
            const VALUE nh = (argn >= 1) ? args[0] : KORB_NIL;
            if (i >= flen || !KORB_HASH_P(nh)) { err = true; errmsg = "malformed format sequence"; break; }
            const VALUE key_sym = ID2SYM(korb_intern(c->vm, fmt + nstart, i - nstart));
            const int32_t hidx = korb_hash_find(VAL2HASH(nh), key_sym);
            if (hidx >= 0) {
                named_arg = korb_items_data(VAL2HASH(nh)->items)[2 * hidx + 1];
            } else {
                VALUE dv = KORB_NIL;                        /* absent key: honor a non-nil default */
                if (VAL2HASH(nh)->default_proc != KORB_NIL || VAL2HASH(nh)->default_val != KORB_NIL) {
                    slots[1] = nh; slots[2] = key_sym;     /* nh[key] (proc / value) */
                    RESULT dr = korb_send_impl(c, slots + 3, korb_intern(c->vm, "[]", 2), 0, 1, NULL, NULL, NULL);
                    if (UNLIKELY(dr.state != KORB_NORMAL)) { coerce_err = dr; has_coerce_err = true; err = true; break; }
                    FMT_REREAD_ARGS();
                    dv = dr.value;
                }
                if (dv == KORB_NIL) {                       /* no default, or default gave nil → KeyError (w/ #receiver + #key) */
                    char km[256]; snprintf(km, sizeof km, "key{%.*s} not found", (int)(i - nstart), fmt + nstart);
                    coerce_err = korb_raise_key(c, slots + 1, nh, key_sym, km);
                    has_coerce_err = true; err = true; break;
                }
                named_arg = dv;
            }
            if (KORB_STRING_P(named_arg)) fwrite(korb_strbuf_data(VAL2STR(named_arg)->buf), 1, VAL2STR(named_arg)->len, ms);
            else if (KORB_OBJECT_P(named_arg) && fmt_stable) {   /* user object: dispatch #to_s (honours overrides) */
                slots[1] = named_arg;
                RESULT sr = korb_send_impl(c, slots + 2, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) { coerce_err = sr; has_coerce_err = true; err = true; break; }
                FMT_REREAD_ARGS();
                if (KORB_STRING_P(sr.value)) fwrite(korb_strbuf_data(VAL2STR(sr.value)->buf), 1, VAL2STR(sr.value)->len, ms);
                else korb_fprint_to_s(c, ms, sr.value);
            }
            else korb_fprint_to_s(c, ms, named_arg);
            continue;                                      /* for-loop i++ steps past the close */
        }
        TRY_NAMED(); if (err) break;                       /* %<name> right after '%' */
        /* %N$ positional index (1-based); only when not named. */
        int explicit_idx = -1;
        if (!has_named) {
            const uint32_t save = i; int num = 0; bool any = false;
            while (i < flen && isdigit((unsigned char)fmt[i])) { num = num * 10 + (fmt[i] - '0'); any = true; i++; }
            if (any && i < flen && fmt[i] == '$') { explicit_idx = num - 1; i++; }
            else i = save;                                 /* plain width digits → reparse below */
        }
        while (i < flen && strchr("-+ 0#", fmt[i])) { if (si < 70) spec[si++] = fmt[i]; i++; }
        TRY_NAMED(); if (err) break;                       /* %flags<name>… */
        if (i < flen && fmt[i] == '*') {                   /* dynamic width: `*` (next arg) or `*N$` (positional) */
            i++;
            VALUE wv;
            { int wnum = 0; bool wany = false; const uint32_t sv = i;
              while (i < flen && isdigit((unsigned char)fmt[i])) { wnum = wnum * 10 + (fmt[i]-'0'); wany = true; i++; }
              if (wany && i < flen && fmt[i] == '$') { i++; wv = ((uint32_t)(wnum-1) < argn) ? args[wnum-1] : KORB_NIL; }
              else { i = sv; wv = (ai < argn) ? args[ai++] : KORB_NIL; } }
            intptr_t w;
            if (!korb_to_index(wv, &w) && KORB_OBJECT_P(wv) && korb_responds_to(c, wv, fmt_to_int)) {
                slots[1] = wv;                              /* a `*` width may be any #to_int object */
                RESULT wr = korb_send_impl(c, slots + 2, fmt_to_int, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(wr.state != KORB_NORMAL)) { coerce_err = wr; has_coerce_err = true; err = true; break; }
                FMT_REREAD_ARGS();
                wv = wr.value;
            }
            if (!korb_to_index(wv, &w)) { err = true; errmsg = "width too big"; break; }
            if (w < 0) { if (si < 70) spec[si++] = '-'; w = -w; }                           /* negative width → left-justify */
            si += snprintf(spec + si, sizeof(spec) - (size_t)si, "%ld", (long)w);
        } else while (i < flen && isdigit((unsigned char)fmt[i])) { if (si < 70) spec[si++] = fmt[i]; i++; }
        TRY_NAMED(); if (err) break;                       /* %flagsWIDTH<name>… */
        if (i < flen && fmt[i] == '.') {
            i++;
            if (i < flen && fmt[i] == '*') {               /* dynamic precision: `*` or `*N$` */
                i++;
                VALUE pv;
                { int pnum = 0; bool pany = false; const uint32_t sv = i;
                  while (i < flen && isdigit((unsigned char)fmt[i])) { pnum = pnum * 10 + (fmt[i]-'0'); pany = true; i++; }
                  if (pany && i < flen && fmt[i] == '$') { i++; pv = ((uint32_t)(pnum-1) < argn) ? args[pnum-1] : KORB_NIL; }
                  else { i = sv; pv = (ai < argn) ? args[ai++] : KORB_NIL; } }
                intptr_t pl;
                if (!korb_to_index(pv, &pl)) { err = true; errmsg = "precision too big"; break; }
                if (pl >= 0) si += snprintf(spec + si, sizeof(spec) - (size_t)si, ".%ld", (long)pl);   /* negative precision → ignored (CRuby) */
            } else {
                { const uint32_t ps = i; long long pv2 = 0;      /* a literal precision must fit an int */
                  while (i < flen && isdigit((unsigned char)fmt[i])) { if (pv2 < 1000000000LL) pv2 = pv2 * 10 + (fmt[i]-'0'); i++; }
                  if (pv2 > 2147483647LL / 2) { err = true; errmsg = "precision too big"; break; }
                  i = ps; }
                if (si < 70) spec[si++] = '.';
                while (i < flen && isdigit((unsigned char)fmt[i])) { if (si < 70) spec[si++] = fmt[i]; i++; }
            }
        }
        /* trailing positional value index `N$` (after width/precision), e.g.
         * `%*1$.*2$3$d` — the value comes from arg N.  Only digits here can be
         * this index (plain width digits were consumed above). */
        if (explicit_idx < 0 && !has_named && i < flen && isdigit((unsigned char)fmt[i])) {
            const uint32_t save = i; int num = 0;
            while (i < flen && isdigit((unsigned char)fmt[i])) { num = num * 10 + (fmt[i] - '0'); i++; }
            if (i < flen && fmt[i] == '$') { explicit_idx = num - 1; i++; }
            else i = save;
        }
        TRY_NAMED(); if (err) break;                       /* %flagsWIDTH.PREC<name>type */
        #undef TRY_NAMED
        if (i >= flen) { err = true; errmsg = "incomplete format specifier; use %% (double %) instead"; break; }
        char conv;
        if (fmt[i] == '{') {                               /* %[flags][width][.prec]{name}: named value to_s (implicit 's') */
            saw_named = true;
            i++; const uint32_t nstart = i;
            while (i < flen && fmt[i] != '}') i++;
            const VALUE nh = (argn >= 1) ? args[0] : KORB_NIL;
            if (i >= flen || !KORB_HASH_P(nh)) { err = true; errmsg = "malformed format sequence"; break; }
            const VALUE ksym = ID2SYM(korb_intern(c->vm, fmt + nstart, i - nstart));
            const int32_t hidx = korb_hash_find(VAL2HASH(nh), ksym);
            if (UNLIKELY(hidx < 0)) {
                char km[256]; snprintf(km, sizeof km, "key{%.*s} not found", (int)(i - nstart), fmt + nstart);
                coerce_err = korb_raise_key(c, slots + 1, nh, ksym, km);  /* KeyError w/ #receiver + #key */
                has_coerce_err = true; err = true; break;
            }
            named_arg = korb_items_data(VAL2HASH(nh)->items)[2 * hidx + 1]; has_named = true; conv = 's';   /* i at '}'; loop i++ steps past it */
        } else conv = fmt[i];
        const bool sequential = (!has_named && explicit_idx < 0);
        if (conv != '%' && !has_named) {
            if (explicit_idx >= 0) {
                if (saw_unnumbered) { snprintf(mixmsg, sizeof mixmsg, "numbered(%d) after unnumbered(%u)", explicit_idx + 1, ai); errmsg = mixmsg; err = true; break; }
                saw_numbered = true;
            } else {
                if (saw_numbered) { snprintf(mixmsg, sizeof mixmsg, "unnumbered(%u) mixed with numbered", ai + 1); errmsg = mixmsg; err = true; break; }
                saw_unnumbered = true;
            }
        } else if (has_named) saw_named = true;
        if (sequential && conv != '%' && ai >= argn) { err = true; errmsg = "too few arguments"; break; }
        if (explicit_idx >= 0 && (uint32_t)explicit_idx >= argn && conv != '%') { err = true; errmsg = "too few arguments"; break; }
        VALUE arg = has_named ? named_arg
                  : (explicit_idx >= 0 ? ((uint32_t)explicit_idx < argn ? args[explicit_idx] : KORB_NIL)
                                       : ((ai < argn) ? args[ai] : KORB_NIL));
        if (sequential && conv != '%') ai++;               /* advance only for a sequential arg */
        switch (conv) {
          case 'd': case 'i': case 'u': {
            /* Coerce to an Integer as Kernel#Integer (String base-0 / #to_int→#to_i
             * / Float trunc / Rational trunc), then render Fixnum or Bignum. */
            if (!FIXNUM_P(arg) && !KORB_BIGNUM_P(arg)) {
                RESULT ir = korb_fmt_integer(c, slots, arg);
                if (UNLIKELY(ir.state != KORB_NORMAL)) { coerce_err = ir; has_coerce_err = true; err = true; break; }
                FMT_REREAD_ARGS(); arg = ir.value;
            }
            if (KORB_BIGNUM_P(arg)) {                     /* Bignum: let GMP honour the flags/width via %Zd */
                spec[si++] = 'Z'; spec[si++] = 'd'; spec[si] = '\0';
                korb_mp_t z; korb_to_mpz(arg, z);
                korb_mp_fprintf(ms, spec, z);
                korb_mp_clear(z);
                break;
            }
            spec[si++] = 'l'; spec[si++] = 'd'; spec[si] = '\0';
            fprintf(ms, spec, (long)FIX2LONG(arg));
            break;
          }
          case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {   /* a/A = hex float (C printf) */
            double v;
            if (UNLIKELY(!korb_num_to_d(arg, &v))) {     /* coerce via Kernel#Float (String parse / #to_f) */
                RESULT fr = korb_fmt_float(c, slots, arg, &v);
                if (UNLIKELY(fr.state != KORB_NORMAL)) { coerce_err = fr; has_coerce_err = true; err = true; break; }
                FMT_REREAD_ARGS();
            }
            if (UNLIKELY(isinf(v) || isnan(v))) {
                /* CRuby renders non-finite floats as "Inf"/"NaN" (fixed casing,
                 * never C's inf/INF/nan), honouring sign + width but ignoring
                 * precision and the 0-pad flag (width is space-padded). */
                bool left = false, plus = false, space = false; int width = 0;
                for (int k = 1; k < si; k++) {
                    const char sc = spec[k];
                    if (sc == '-') left = true;
                    else if (sc == '+') plus = true;
                    else if (sc == ' ') space = true;
                    else if (sc == '.') break;                 /* precision: stop scanning width */
                    else if (isdigit((unsigned char)sc)) { if (sc != '0' || width) width = width * 10 + (sc - '0'); }
                }
                char body[8]; int bi = 0;
                if (isnan(v))            { /* NaN has no sign */ }
                else if (v < 0)          body[bi++] = '-';
                else if (plus)           body[bi++] = '+';
                else if (space)          body[bi++] = ' ';
                const char *word = isnan(v) ? "NaN" : "Inf";
                body[bi++] = word[0]; body[bi++] = word[1]; body[bi++] = word[2];
                const int pad = width > bi ? width - bi : 0;
                if (left) { fwrite(body, 1, (size_t)bi, ms); for (int p = 0; p < pad; p++) fputc(' ', ms); }
                else      { for (int p = 0; p < pad; p++) fputc(' ', ms); fwrite(body, 1, (size_t)bi, ms); }
                break;
            }
            spec[si++] = conv; spec[si] = '\0';
            fprintf(ms, spec, v);
            break;
          }
          case 'x': case 'X': case 'o': case 'b': case 'B': {
            if (!FIXNUM_P(arg) && !KORB_BIGNUM_P(arg)) {  /* coerce via Kernel#Integer (String / #to_int→#to_i / Rational trunc) */
                RESULT ir = korb_fmt_integer(c, slots, arg);
                if (UNLIKELY(ir.state != KORB_NORMAL)) { coerce_err = ir; has_coerce_err = true; err = true; break; }
                FMT_REREAD_ARGS(); arg = ir.value;
            }
            const int base = (conv == 'o') ? 8 : (conv == 'b' || conv == 'B') ? 2 : 16;
            const bool upper = (conv == 'X' || conv == 'B');
            bool left = false, zero = false, alt = false, plus = false, space = false, has_prec = false;
            int width = 0, prec = 0; bool in_prec = false;   /* parse flags / width / precision from spec */
            for (int k = 1; k < si; k++) {
                const char sc = spec[k];
                if (sc == '.') { in_prec = true; has_prec = true; }
                else if (isdigit((unsigned char)sc)) {
                    if (in_prec) prec = prec * 10 + (sc - '0');
                    else if (sc != '0' || width) width = width * 10 + (sc - '0');
                    else zero = true;                                    /* leading 0 = pad flag */
                }
                else if (sc == '-') left = true;
                else if (sc == '#') alt = true;
                else if (sc == '+') plus = true;
                else if (sc == ' ') space = true;
            }
            korb_mp_t z; korb_to_mpz(arg, z);
            korb_fmt_radix(ms, z, base, upper, left, zero, alt, plus, space, width, has_prec, prec);
            korb_mp_clear(z);
            break;
          }
          case 's': {
            spec[si++] = 's'; spec[si] = '\0';
            if (KORB_STRING_P(arg)) {
                if (!korb_str_ascii_only_p(c->vm, arg)) {   /* only a non-7-bit argument decides */
                    uint32_t ne;
                    if (UNLIKELY(!korb_str_enc_combine(c->vm, enc_src, arg, &ne))) {
                        enc_a = KORB_STR_ENC(enc_src); enc_b = KORB_STR_ENC(arg);
                        enc_err = true; err = true; break;
                    }
                    renc = ne; enc_src = arg;              /* later arguments check against it */
                }
                /* a precision counts CHARACTERS (CRuby); fprintf would count
                 * bytes, so a multi-byte argument is pre-truncated and the
                 * precision dropped from the spec. */
                int pdot = -1;
                for (int k = 1; k < si; k++) if (spec[k] == '.') { pdot = k; break; }
                if (pdot >= 0 && !KORB_ENC_SB(c->vm, KORB_STR_ENC(arg))) {
                    int prec = 0;
                    for (int k = pdot + 1; k < si && isdigit((unsigned char)spec[k]); k++) prec = prec * 10 + (spec[k] - '0');
                    const KorbString *const as = VAL2STR(arg);
                    const char *const ab = korb_strbuf_data(as->buf);
                    uint32_t bl = 0;                          /* byte length of the first `prec` characters */
                    for (int ch = 0; ch < prec && bl < as->len; ch++) {
                        uint32_t adv = 1;                     /* UTF-8 lead byte → sequence length */
                        const unsigned char b = (unsigned char)ab[bl];
                        if (b >= 0xF0) adv = 4; else if (b >= 0xE0) adv = 3; else if (b >= 0xC0) adv = 2;
                        bl += (bl + adv <= as->len) ? adv : (as->len - bl);
                    }
                    /* the width counts characters too, so pad here rather than
                     * handing a byte-counting fprintf a width */
                    bool left = false; int width = 0;
                    for (int k = 1; k < pdot; k++) {
                        if (spec[k] == '-') left = true;
                        else if (isdigit((unsigned char)spec[k])) width = width * 10 + (spec[k] - '0');
                    }
                    uint32_t cc = 0;                          /* characters kept */
                    for (uint32_t bi2 = 0; bi2 < bl; cc++) {
                        const unsigned char b2 = (unsigned char)ab[bi2];
                        bi2 += (b2 >= 0xF0) ? 4 : (b2 >= 0xE0) ? 3 : (b2 >= 0xC0) ? 2 : 1;
                    }
                    const int pad = (width > (int)cc) ? width - (int)cc : 0;
                    if (!left) for (int p2 = 0; p2 < pad; p2++) fputc(' ', ms);
                    fwrite(ab, 1, bl, ms);
                    if (left) for (int p2 = 0; p2 < pad; p2++) fputc(' ', ms);
                    break;
                }
                fprintf(ms, spec, korb_strbuf_data(VAL2STR(arg)->buf));
            }
            else if (KORB_OBJECT_P(arg) && fmt_stable) {   /* user object: dispatch #to_s (honours overrides) */
                slots[1] = arg;
                RESULT sr = korb_send_impl(c, slots + 2, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) { coerce_err = sr; has_coerce_err = true; err = true; break; }
                FMT_REREAD_ARGS();
                char *tb = NULL; size_t tsz = 0; FILE *tms = open_memstream(&tb, &tsz);
                if (tms) {
                    if (KORB_STRING_P(sr.value)) fwrite(korb_strbuf_data(VAL2STR(sr.value)->buf), 1, VAL2STR(sr.value)->len, tms);
                    else korb_fprint_to_s(c, tms, sr.value);
                    fclose(tms);
                }
                fprintf(ms, spec, tb ? tb : ""); free(tb);
            }
            else {
                char *tb = NULL; size_t tsz = 0; FILE *tms = open_memstream(&tb, &tsz);
                if (tms) { korb_fprint_to_s(c, tms, arg); fclose(tms); }
                fprintf(ms, spec, tb ? tb : ""); free(tb);
            }
            break;
          }
          case 'p': {
            char *tb = NULL; size_t tsz = 0; FILE *tms = open_memstream(&tb, &tsz);
            if (KORB_OBJECT_P(arg) && fmt_stable) {        /* dispatch #inspect (honours overrides) */
                slots[1] = arg;
                RESULT ir = korb_send_impl(c, slots + 2, korb_intern(c->vm, "inspect", 7), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(ir.state != KORB_NORMAL)) { if (tms) fclose(tms); free(tb); coerce_err = ir; has_coerce_err = true; err = true; break; }
                FMT_REREAD_ARGS();
                if (tms) {
                    if (KORB_STRING_P(ir.value)) fwrite(korb_strbuf_data(VAL2STR(ir.value)->buf), 1, VAL2STR(ir.value)->len, tms);
                    else korb_fprint_inspect(c, tms, ir.value);
                    fclose(tms);
                }
            } else if (tms) { korb_fprint_inspect(c, tms, arg); fclose(tms); }
            spec[si++] = 's'; spec[si] = '\0';
            fprintf(ms, spec, tb ? tb : ""); free(tb);
            break;
          }
          case 'c': {
            /* %c: Integer → codepoint; String → first char; else #to_str, then
             * #to_ary (single elem), then #to_int; nothing → TypeError. */
            long cp = -1;                                  /* codepoint, or -1 → use `cbytes` */
            const char *cbytes = NULL; int cnbytes = 0;
            if (FIXNUM_P(arg)) cp = FIX2LONG(arg);
            else if (KORB_STRING_P(arg)) {
                const KorbString *cs = VAL2STR(arg);
                if (cs->len == 0) { cbytes = ""; cnbytes = 0; }   /* %c of "" → no character (still pads to width) */
                else { cnbytes = (int)korb_utf8_seq_len((const unsigned char *)korb_strbuf_data(cs->buf), 0, cs->len); if (cnbytes <= 0) cnbytes = 1;
                       cbytes = korb_strbuf_data(cs->buf); }
            }
            else if (KORB_OBJECT_P(arg) && fmt_stable) {
                /* Root arg in slots[1] across the (GC-point) respond_to?/coerce
                 * dispatches — a bare C-local would go stale when a prior arg's
                 * allocation triggers a moving GC (real bug, hit by mock chains). */
                slots[1] = arg;
                const uint32_t c_to_str = korb_intern(c->vm, "to_str", 6), c_to_ary = korb_intern(c->vm, "to_ary", 6);
                uint32_t cm = korb_responds_to_coerce(c, slots + 2, slots[1], c_to_str) ? c_to_str
                            : korb_responds_to_coerce(c, slots + 2, slots[1], c_to_ary) ? c_to_ary
                            : korb_responds_to_coerce(c, slots + 2, slots[1], fmt_to_int) ? fmt_to_int : 0;
                if (!cm) { coerce_err = korb_raise(c, slots + 2, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(slots[1])); has_coerce_err = true; err = true; break; }
                RESULT cr = korb_send_impl(c, slots + 2, cm, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(cr.state != KORB_NORMAL)) { coerce_err = cr; has_coerce_err = true; err = true; break; }
                FMT_REREAD_ARGS();
                VALUE cv = cr.value;
                if (KORB_ARRAY_P(cv)) cv = VAL2ARY(cv)->len > 0 ? korb_items_data(VAL2ARY(cv)->items)[0] : KORB_NIL;
                if (FIXNUM_P(cv)) cp = FIX2LONG(cv);
                else if (KORB_STRING_P(cv)) { slots[1] = cv;  /* root the coerced String */
                    const KorbString *cs = VAL2STR(slots[1]);
                    if (cs->len == 0) { coerce_err = korb_raise(c, slots + 1, KORB_E_ARGUMENT, 0, "%%c requires a character"); has_coerce_err = true; err = true; break; }
                    cnbytes = (int)korb_utf8_seq_len((const unsigned char *)korb_strbuf_data(cs->buf), 0, cs->len); if (cnbytes <= 0) cnbytes = 1;
                    cbytes = korb_strbuf_data(cs->buf); }
                else { coerce_err = korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(arg)); has_coerce_err = true; err = true; break; }
            }
            else { coerce_err = korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(arg)); has_coerce_err = true; err = true; break; }
            char enc[4];
            if (cp >= 0) { cnbytes = (int)korb_utf8_encode((uint32_t)cp, enc); cbytes = enc; }
            korb_fmt_emit_c(ms, cbytes, cnbytes, spec, si);
            break;
          }
          default:
            snprintf(mixmsg, sizeof mixmsg, "malformed format string - %%%c", conv);
            errmsg = mixmsg; err = true; break;
        }
        if (err) break;
    }
    /* $DEBUG turns a format string that consumed fewer arguments than it was
     * given into an error (CRuby); positional/named directives opt out. */
    if (!err && !saw_numbered && !saw_named && ai < argn &&
        KORB_TRUTHY(korb_const_get(vm, korb_intern(vm, "$DEBUG", 6)))) {
        err = true; errmsg = "too many arguments for format string";
    }
    #undef FMT_REREAD_ARGS
    if (has_coerce_err) {                                /* a #to_int/#to_f arg coercion raised → propagate it */
        if (shared) vm->fmt_busy = false; else free(buf);
        return coerce_err;
    }
    /* Finalize: for the shared stream, flush + take the byte count from ftell
     * (current write position) and read from the cached buffer without closing;
     * for a nested stream, close to populate buf/sz. */
    const char *out; size_t outlen;
    if (shared) {
        fflush(ms);
        long pos = ftell(ms);
        out = vm->fmt_buf; outlen = pos > 0 ? (size_t)pos : 0;
    } else {
        fclose(ms);
        out = buf; outlen = sz;
    }
    if (err) {
        if (shared) vm->fmt_busy = false; else free(buf);
        if (enc_err) return korb_raise_enc_compat(c, slots, enc_a, enc_b);
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "%s", errmsg ? errmsg : "format error");
    }
    if (UNLIKELY(!saw_numbered && !saw_named && ai < argn &&
                 korb_const_get(vm, korb_intern(vm, "$VERBOSE", 8)) == KORB_TRUE))   /* only in verbose mode (CRuby) */
        korb_warn(c, slots, "too many arguments for format string");
    RESULT r = korb_str_new(c, slots, out ? out : "", (uint32_t)outlen);
    if (shared) vm->fmt_busy = false; else free(buf);
    /* the result carries the format's encoding, widened by the arguments (CRuby) */
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, renc);
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
/* coerce `o` to a String via #to_str for casecmp; returns false (→ caller yields
 * nil) when not a String and not #to_str-coercible.  GC-safe: parks `o` in slots[0]. */
static bool korb_casecmp_coerce(CTX *c, VALUE *slots, VALUE *o, RESULT *err) {
    if (LIKELY(KORB_STRING_P(*o))) return true;
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    slots[0] = *o;                                    /* root before respond_to?/to_str dispatch */
    if (!korb_responds_to_coerce(c, slots + 1, slots[0], to_str)) { *o = slots[0]; return false; }
    RESULT cr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
    if (UNLIKELY(cr.state != KORB_NORMAL)) { *err = cr; return false; }
    if (!KORB_STRING_P(cr.value)) return false;
    *o = cr.value;
    return true;
}
static RESULT korb_m_str_casecmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    RESULT err = RESULT_OK(KORB_NIL);
    if (!korb_casecmp_coerce(c, slots, &o, &err)) return err.state != KORB_NORMAL ? err : RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *t = VAL2STR(o);
    return RESULT_OK(LONG2FIX(korb_ci_cmp(korb_strbuf_data(s->buf), s->len, korb_strbuf_data(t->buf), t->len)));
}
static RESULT korb_m_str_casecmp_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    RESULT err = RESULT_OK(KORB_NIL);
    if (!korb_casecmp_coerce(c, slots, &o, &err)) return err.state != KORB_NORMAL ? err : RESULT_OK(KORB_NIL);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *t = VAL2STR(o);
    return RESULT_OK(korb_ci_cmp(korb_strbuf_data(s->buf), s->len, korb_strbuf_data(t->buf), t->len) == 0 ? KORB_TRUE : KORB_FALSE);
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
    /* A Bignum offset/length that still fits a C long is just a (far) out-of-range
     * position → nil; one that does not fit is the conversion failure CRuby
     * reports as RangeError. */
    if (UNLIKELY(KORB_BIGNUM_P(iv))) {
        if (!korb_mp_fits_slong_p(VAL2BIG(iv)->z))
            return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'long'");
        return RESULT_OK(KORB_NIL);                      /* |index| > bytesize */
    }
    if (VALUE_SLICE_LEN(a) >= 2 && KORB_BIGNUM_P(VALUE_SLICE_GET(a, 1))) {
        const VALUE lv = VALUE_SLICE_GET(a, 1);
        if (!korb_mp_fits_slong_p(VAL2BIG(lv)->z))
            return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'long'");
        if (korb_mp_sgn(VAL2BIG(lv)->z) < 0) return RESULT_OK(KORB_NIL);
    }
    if (UNLIKELY(!korb_to_index(iv, &i))) {              /* coerce index via #to_int (self via VALUE_REF; bn is a value) */
        RESULT cr = korb_coerce_to_int(c, slots, &iv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(iv, &i)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
        bn = VAL2STR(VALUE_REF_GET(self))->len;
    }
    if (i < 0) i += bn;
    const bool two_arg = VALUE_SLICE_LEN(a) >= 2;
    if (i < 0 || i > (intptr_t)bn || (!two_arg && i == (intptr_t)bn)) return RESULT_OK(KORB_NIL);   /* byteslice(i): nil at end */
    intptr_t lentmp = 1;
    if (two_arg && !korb_to_index(VALUE_SLICE_GET(a, 1), &lentmp)) {   /* #to_int on the length */
        VALUE lv = VALUE_SLICE_GET(a, 1);
        RESULT cr = korb_coerce_to_int(c, slots, &lv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(lv, &lentmp))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(lv));
    }
    const intptr_t len0 = two_arg ? lentmp : 1;
    intptr_t len = len0;
    if (len < 0) return RESULT_OK(KORB_NIL);
    if (i + len > (intptr_t)bn) len = (intptr_t)bn - i;
    return korb_str_slice_new(c, slots, self, (uint32_t)i, (uint32_t)len);
}
/* String#insert(index, str) — insert str before the char at index (negative
 * index counts from the end, inserting after); mutates and returns self. */
static RESULT korb_m_str_insert(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* index arg */
    slots[1] = VALUE_SLICE_GET(a, 1);                    /* other string (rooted across coercions) */
    intptr_t idx;
    if (UNLIKELY(!korb_to_index(slots[0], &idx))) {      /* coerce index via #to_int */
        VALUE iv0 = slots[0];
        RESULT cr = korb_coerce_to_int(c, slots + 2, &iv0);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(iv0, &idx)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    }
    if (UNLIKELY(!KORB_STRING_P(slots[1]))) {            /* coerce other via #to_str */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(slots[1]) && korb_responds_to_coerce(c, slots + 2, slots[1], to_str)) {
            RESULT sr = korb_send_impl(c, slots + 2, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            slots[1] = sr.value;
        }
        if (!KORB_STRING_P(slots[1])) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 1)));
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t ncp = korb_utf8_count(korb_strbuf_data(s->buf), s->len);
    intptr_t pos = idx >= 0 ? idx : (intptr_t)ncp + idx + 1;
    if (UNLIKELY(pos < 0 || pos > (intptr_t)ncp)) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld out of string", (long)idx);
    uint32_t boff = korb_str_char_to_byte(s, (uint32_t)pos);
    uint32_t inn = VAL2STR(slots[1])->len, newlen = s->len + inn;
    char *out = malloc(newlen ? newlen : 1);
    s = VAL2STR(VALUE_REF_GET(self));
    memcpy(out, korb_strbuf_data(s->buf), boff);
    memcpy(out + boff, korb_strbuf_data(VAL2STR(slots[1])->buf), inn);
    memcpy(out + boff + inn, korb_strbuf_data(s->buf) + boff, s->len - boff);
    KorbString *ns = korb_str_ensure(c, slots, self, newlen);
    memcpy(korb_strbuf_data(ns->buf), out, newlen); ns->len = newlen; korb_strbuf_data(ns->buf)[newlen] = '\0';
    free(out);
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Render a Range as "<begin><..|...><end>" (for bytesplice's RangeError text). */
static void korb_range_desc(CTX *c, VALUE rng, char *buf, size_t sz) {
    const KorbRange *r = VAL2RANGE(rng);
    char *b = NULL; size_t z = 0; FILE *ms = open_memstream(&b, &z);
    if (ms) {
        if (r->rbegin != KORB_NIL) korb_fprint_inspect(c, ms, r->rbegin);
        fputs(r->exclude_end ? "..." : "..", ms);
        if (r->rend != KORB_NIL) korb_fprint_inspect(c, ms, r->rend);
        fclose(ms);
    }
    snprintf(buf, sz, "%s", b ? b : "");
    free(b);
}
/* String#bytesplice(index, length, str) / (range, str) [+ str_range | str_index,str_length]
 * — replace bytes in place, return self. */
/* A byte offset lands on a UTF-8 codepoint boundary iff it's the end, or the
 * byte there is not a continuation byte (0b10xxxxxx). */
static bool korb_str_bpos_ok(const KorbString *s, uint32_t p) {
    return p >= s->len || ((unsigned char)korb_strbuf_data(s->buf)[p] & 0xC0) != 0x80;
}
static RESULT korb_m_str_bytesplice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t bn = s->len;
    intptr_t start = 0, dellen = 0; VALUE repl; uint32_t repl_pos;
    intptr_t start_arg = 0;                    /* index as written (for the error text) */
    if (VALUE_SLICE_LEN(a) >= 2 && KORB_RANGE_P(VALUE_SLICE_GET(a, 0))) {
        const KorbRange *r = VAL2RANGE(VALUE_SLICE_GET(a, 0));
        if (r->rbegin != KORB_NIL && UNLIKELY(!korb_to_index(r->rbegin, &start))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (start < 0) start += bn;
        intptr_t e; if (r->rend == KORB_NIL) e = bn; else { if (UNLIKELY(!korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); if (e < 0) e += bn; if (!r->exclude_end) e += 1; }
        dellen = e - start;
        repl = VALUE_SLICE_GET(a, 1); repl_pos = 1;
    } else {
        /* 2-arg form requires a Range first argument (an Integer there means the
         * caller forgot the length) */
        if (UNLIKELY(VALUE_SLICE_LEN(a) == 2))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Range)",
                              korb_type_name(VALUE_SLICE_GET(a, 0)));
        if (UNLIKELY(VALUE_SLICE_LEN(a) < 3 || !korb_to_index(VALUE_SLICE_GET(a, 0), &start) || !korb_to_index(VALUE_SLICE_GET(a, 1), &dellen)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        start_arg = start;
        if (start < 0) start += bn;
        repl = VALUE_SLICE_GET(a, 2); repl_pos = 2;
    }

    /* Trailing args after the replacement select a sub-span of it:
     *   0 extra          → the whole replacement
     *   1 extra (Range)  → str_range
     *   2 extra (Int,Int)→ str_index, str_length (index form only). */
    const uint32_t nafter = VALUE_SLICE_LEN(a) - (repl_pos + 1);
    const bool src_is_range = (nafter == 1 && KORB_RANGE_P(VALUE_SLICE_GET(a, repl_pos + 1)));
    if (UNLIKELY(!(nafter == 0 || src_is_range || (nafter == 2 && repl_pos == 2))))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 2, 3, or 5)", (unsigned)VALUE_SLICE_LEN(a));
    /* index form uses IndexError; range form uses RangeError (with the range's text). */
    if (UNLIKELY(start < 0 || start > (intptr_t)bn)) {
        if (repl_pos == 1) { char rb[96]; korb_range_desc(c, VALUE_SLICE_GET(a, 0), rb, sizeof rb); return korb_raise(c, slots, KORB_E_RANGE, 0, "%s out of range", rb); }
        /* CRuby names the index as written, not the one adjusted by bytesize */
        return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld out of string", (long)start_arg);
    }
    if (UNLIKELY(dellen < 0)) return korb_raise(c, slots, KORB_E_INDEX, 0, "negative length %ld", (long)dellen);
    if (start + dellen > (intptr_t)bn) dellen = (intptr_t)bn - start;
    if (KORB_STR_ENC(VALUE_REF_GET(self)) == KORB_ENC_UTF8) {   /* the deleted span must be whole codepoints */
        s = VAL2STR(VALUE_REF_GET(self));
        if (UNLIKELY(!korb_str_bpos_ok(s, (uint32_t)start)))
            return korb_raise(c, slots, KORB_E_INDEX, 0, "offset %ld does not land on character boundary", (long)start);
        if (UNLIKELY(!korb_str_bpos_ok(s, (uint32_t)(start + dellen))))
            return korb_raise(c, slots, KORB_E_INDEX, 0, "offset %ld does not land on character boundary", (long)(start + dellen));
    }
    if (UNLIKELY(!KORB_STRING_P(repl))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(repl));
    const KorbString *rs = VAL2STR(repl); uint32_t rn = rs->len, roff = 0;
    if (src_is_range) {                                    /* replacement sub-span given as a Range */
        const KorbRange *sr = VAL2RANGE(VALUE_SLICE_GET(a, repl_pos + 1));
        intptr_t si = 0, e;
        if (sr->rbegin != KORB_NIL && UNLIKELY(!korb_to_index(sr->rbegin, &si))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (si < 0) si += rn;
        if (sr->rend == KORB_NIL) e = rn; else { if (UNLIKELY(!korb_to_index(sr->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer"); if (e < 0) e += rn; if (!sr->exclude_end) e += 1; }
        if (UNLIKELY(si < 0 || si > (intptr_t)rn)) { char rb[96]; korb_range_desc(c, VALUE_SLICE_GET(a, repl_pos + 1), rb, sizeof rb); return korb_raise(c, slots, KORB_E_RANGE, 0, "%s out of range", rb); }
        intptr_t sl = e - si; if (sl < 0) sl = 0;
        if (si + sl > (intptr_t)rn) sl = (intptr_t)rn - si;
        roff = (uint32_t)si; rn = (uint32_t)sl;
    } else if (nafter == 2) {                              /* 5-arg form: str[str_index, str_length] */
        intptr_t si, sl;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, repl_pos + 1), &si) || !korb_to_index(VALUE_SLICE_GET(a, repl_pos + 2), &sl)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (si < 0) si += rn;
        if (UNLIKELY(si < 0 || si > (intptr_t)rn)) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld out of string", (long)si);
        if (UNLIKELY(sl < 0)) return korb_raise(c, slots, KORB_E_INDEX, 0, "negative length %ld", (long)sl);
        if (si + sl > (intptr_t)rn) sl = (intptr_t)rn - si;
        roff = (uint32_t)si; rn = (uint32_t)sl;
    }
    if (KORB_STR_ENC(repl) == KORB_ENC_UTF8) {             /* the replacement sub-span must be whole codepoints */
        if (UNLIKELY(!korb_str_bpos_ok(rs, roff)))
            return korb_raise(c, slots, KORB_E_INDEX, 0, "offset %u does not land on character boundary", roff);
        if (UNLIKELY(!korb_str_bpos_ok(rs, roff + rn)))
            return korb_raise(c, slots, KORB_E_INDEX, 0, "offset %u does not land on character boundary", roff + rn);
    }
    uint32_t sufoff = (uint32_t)(start + dellen), suflen = bn - sufoff;
    uint32_t newlen = (uint32_t)start + rn + suflen;
    char *out = malloc(newlen ? newlen : 1);                /* assemble full new content (no GC) */
    s = VAL2STR(VALUE_REF_GET(self));
    memcpy(out, korb_strbuf_data(s->buf), (size_t)start);
    memcpy(out + start, korb_strbuf_data(VAL2STR(repl)->buf) + roff, rn);
    memcpy(out + start + rn, korb_strbuf_data(s->buf) + sufoff, suflen);
    KorbString *ns = korb_str_ensure(c, slots, self, newlen);   /* may move; out is libc-stable */
    memcpy(korb_strbuf_data(ns->buf), out, newlen); ns->len = newlen; korb_strbuf_data(ns->buf)[newlen] = '\0';
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
    return RESULT_OK(LONG2FIX((unsigned char)korb_strbuf_data(s->buf)[i]));
}
static RESULT korb_m_str_setbyte(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    VALUE iv = VALUE_SLICE_GET(a, 0), bv = VALUE_SLICE_GET(a, 1);
    intptr_t i, b;                                          /* index/value coerce via #to_int (Float truncates) */
    slots[0] = VALUE_REF_GET(self);                         /* root self across the coercion dispatches */
    if (UNLIKELY(!korb_to_index(iv, &i))) {
        RESULT cr = korb_coerce_to_int(c, slots + 1, &iv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (cr.value == KORB_FALSE || !korb_to_index(iv, &i)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    if (UNLIKELY(!korb_to_index(bv, &b))) {
        RESULT cr = korb_coerce_to_int(c, slots + 1, &bv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (cr.value == KORB_FALSE || !korb_to_index(bv, &b)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
    }
    self = VALUE_REF_AT(&slots[0]);                         /* re-root after possible GC */
    KorbString *s = VAL2STR(VALUE_REF_GET(self));
    intptr_t idx = i; if (idx < 0) idx += s->len;
    if (UNLIKELY(idx < 0 || (uint32_t)idx >= s->len)) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld out of string", (long)i);
    korb_strbuf_data(s->buf)[idx] = (char)(b & 0xFF);
    return RESULT_OK(VALUE_SLICE_GET(a, 1));                /* returns the original value argument */
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
    return RESULT_OK(ID2SYM(korb_intern(c->vm, korb_strbuf_data(rs->buf), rs->len)));
}
static RESULT korb_m_str_swapcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
/* Symbol case-fold: materialise name as String, run str transform op
 * (0=upcase 1=downcase 2=capitalize), re-intern to a Symbol. */
static RESULT korb_sym_case(CTX *c, VALUE *slots, VALUE_REF self, int op) {
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    slots[0] = UNWRAP(korb_str_new(c, slots, nm, (uint32_t)strlen(nm)));
    RESULT r = korb_str_transform(c, slots + 1, VALUE_REF_AT(&slots[0]), op, false);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    slots[1] = r.value;   /* root the new string across the (alloc'ing) intern */
    const KorbString *rs = VAL2STR(slots[1]);
    return RESULT_OK(ID2SYM(korb_intern(c->vm, korb_strbuf_data(rs->buf), rs->len)));
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
    VALUE lo, hi;
    if (VALUE_SLICE_LEN(a) == 1) {
        if (UNLIKELY(!KORB_RANGE_P(VALUE_SLICE_GET(a, 0))))            /* clamp(x) single non-Range → TypeError */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Range)", korb_type_name(VALUE_SLICE_GET(a, 0)));
        const KorbRange *r = VAL2RANGE(VALUE_SLICE_GET(a, 0)); if (UNLIKELY(r->exclude_end)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "cannot clamp with an exclusive range"); lo = r->rbegin; hi = r->rend;
    } else if (VALUE_SLICE_LEN(a) == 2) { lo = VALUE_SLICE_GET(a, 0); hi = VALUE_SLICE_GET(a, 1); }
    else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 2)", (unsigned)VALUE_SLICE_LEN(a));
    const char *s = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    if (lo != KORB_NIL && SYMBOL_P(lo) && strcmp(s, korb_sym_name(c->vm, SYM2ID(lo))) < 0) return RESULT_OK(lo);
    if (hi != KORB_NIL && SYMBOL_P(hi) && strcmp(s, korb_sym_name(c->vm, SYM2ID(hi))) > 0) return RESULT_OK(hi);
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
    return RESULT_OK(ID2SYM(korb_intern(c->vm, korb_strbuf_data(rs->buf), rs->len)));
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
/* coerce the search arg (slots[0]) via #to_str, returns true if it is now a String. */
static bool korb_str_search_coerce(CTX *c, VALUE *slots) {
    if (KORB_STRING_P(slots[0])) return true;
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce(c, slots + 1, slots[0], to_str)) {
        RESULT cr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
        if (cr.state == KORB_NORMAL) slots[0] = cr.value;
    }
    return KORB_STRING_P(slots[0]);
}
static RESULT korb_m_str_byteindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (KORB_REGEXP_P(VALUE_SLICE_GET(a, 0))) {       /* byteindex(regexp[, start_byte]) */
        long startc = 0;
        if (VALUE_SLICE_LEN(a) >= 2) { intptr_t st = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &st)) startc = (long)st; }
        return korb_re_str_index(c, slots, self, VALUE_SLICE_GET(a, 0), startc, true);
    }
    slots[0] = VALUE_SLICE_GET(a, 0);                 /* search (coerce via #to_str) */
    if (!korb_str_search_coerce(c, slots)) return RESULT_OK(KORB_NIL);
    intptr_t start = 0;
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* byteindex(substr, start_byte) */
        VALUE ov = VALUE_SLICE_GET(a, 1);
        if (UNLIKELY(!korb_to_index(ov, &start))) {
            RESULT cr = korb_coerce_to_int(c, slots + 1, &ov);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(ov, &start)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        }
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(slots[0]);   /* read after dispatch */
    uint32_t off = 0;
    if (VALUE_SLICE_LEN(a) >= 2) {
        if (start < 0) start += s->len;
        if (start < 0 || start > (intptr_t)s->len) return RESULT_OK(KORB_NIL);
        off = (uint32_t)start;
    }
    int32_t b = korb_byte_find(korb_strbuf_data(s->buf) + off, s->len - off, korb_strbuf_data(n->buf), n->len);
    return RESULT_OK(b < 0 ? KORB_NIL : LONG2FIX(off + (uint32_t)b));
}
static RESULT korb_m_str_byterindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (KORB_REGEXP_P(VALUE_SLICE_GET(a, 0))) {       /* byterindex(regexp[, stop_byte]) */
        long stop = 0; bool have_stop = false;
        if (VALUE_SLICE_LEN(a) >= 2) { intptr_t st = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &st)) { stop = (long)st; have_stop = true; } }
        return korb_re_str_rindex(c, slots, self, VALUE_SLICE_GET(a, 0), stop, true, have_stop);
    }
    slots[0] = VALUE_SLICE_GET(a, 0);                 /* search (coerce via #to_str) */
    if (!korb_str_search_coerce(c, slots)) return RESULT_OK(KORB_NIL);
    intptr_t stop; bool have_stop = false;
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* byterindex(substr, stop_byte) */
        VALUE ov = VALUE_SLICE_GET(a, 1);
        if (UNLIKELY(!korb_to_index(ov, &stop))) {
            RESULT cr = korb_coerce_to_int(c, slots + 1, &ov);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(ov, &stop)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        }
        have_stop = true;
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(slots[0]);   /* read after dispatch */
    if (n->len > s->len) return RESULT_OK(KORB_NIL);
    int32_t hi = (int32_t)(s->len - n->len);
    if (have_stop) {
        if (stop < 0) stop += s->len;
        if (stop < 0) return RESULT_OK(KORB_NIL);
        if (stop < hi) hi = (int32_t)stop;
    }
    for (int32_t i = hi; i >= 0; i--)
        if (memcmp(korb_strbuf_data(s->buf) + i, korb_strbuf_data(n->buf), n->len) == 0) return RESULT_OK(LONG2FIX(i));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_str_chr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    if (s->len == 0) return korb_str_new(c, slots, "", 0);
    uint32_t cl = 1;                                  /* one UTF-8 codepoint */
    while (cl < s->len && ((unsigned char)korb_strbuf_data(s->buf)[cl] & 0xC0) == 0x80) cl++;
    return korb_str_slice_new(c, slots, self, 0, cl);
}
/* String#ord — codepoint of the first character (UTF-8); empty → ArgumentError. */
static RESULT korb_m_str_ord(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE v = VALUE_REF_GET(self);
    const KorbString *s = VAL2STR(v);
    if (UNLIKELY(s->len == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "empty string");
    const uint32_t enc = KORB_STR_ENC(v);
    if (UNLIKELY(KORB_ENC_NEEDS_HOOK(c->vm, enc)) && !korb_str_bytes_ascii(v)) return korb_str_enc_notimpl(c, slots, v);
    const unsigned char *d = (const unsigned char *)korb_strbuf_data(s->buf);
    if (KORB_ENC_SB(c->vm, enc)) return RESULT_OK(LONG2FIX(d[0]));   /* single-byte: the first byte */
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
    if (KORB_REGEXP_P(VALUE_SLICE_GET(a, 0))) {       /* rindex(regexp[, stop_char]) */
        long stop = 0; bool have_stop = false;
        if (VALUE_SLICE_LEN(a) >= 2 && VALUE_SLICE_GET(a, 1) != KORB_NIL) {
            intptr_t st;
            if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &st))) {
                VALUE ov = VALUE_SLICE_GET(a, 1); RESULT cr = korb_coerce_to_int(c, slots, &ov);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                if (!korb_to_index(ov, &st)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
            }
            stop = (long)st; have_stop = true;
        }
        return korb_re_str_rindex(c, slots, self, VALUE_SLICE_GET(a, 0), stop, false, have_stop);
    }
    VALUE sv = VALUE_SLICE_GET(a, 0);
    /* the position arg (2-arg form) is validated BEFORE the pattern: an out-of-
     * range stop returns nil even for a non-String pattern (CRuby order). */
    bool have_stop = false; int32_t stopb = 0;
    if (VALUE_SLICE_LEN(a) >= 2) {
        intptr_t stop;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &stop))) {   /* coerce start via #to_int */
            VALUE sv = VALUE_SLICE_GET(a, 1);
            RESULT cr = korb_coerce_to_int(c, slots, &sv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(sv, &stop)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        }
        const KorbString *const s0 = VAL2STR(VALUE_REF_GET(self));
        uint32_t ncp = korb_utf8_count(korb_strbuf_data(s0->buf), s0->len);
        if (stop < 0) stop += ncp;
        if (stop < 0) return RESULT_OK(KORB_NIL);
        stopb = (int32_t)korb_str_char_to_byte(s0, stop);   /* byte offset survives a later GC move */
        have_stop = true;
    }
    if (UNLIKELY(!KORB_STRING_P(sv))) {               /* coerce via #to_str, else TypeError (never #to_int) */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (!korb_responds_to_coerce_p(c, slots, &sv, to_str))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "type mismatch: %s given", korb_type_name(sv));
        slots[0] = sv;
        RESULT cr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (UNLIKELY(!KORB_STRING_P(cr.value)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "type mismatch: %s given", korb_type_name(slots[0]));
        sv = cr.value;
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(self)), *n = VAL2STR(sv);
    if (n->len > s->len) return RESULT_OK(KORB_NIL);
    int32_t hi = (int32_t)(s->len - n->len);          /* last byte where a match can begin */
    if (have_stop && stopb < hi) hi = stopb;
    for (int32_t i = hi; i >= 0; i--)
        if (memcmp(korb_strbuf_data(s->buf) + i, korb_strbuf_data(n->buf), n->len) == 0)
            return RESULT_OK(LONG2FIX(korb_utf8_count(korb_strbuf_data(s->buf), (uint32_t)i)));
    return RESULT_OK(KORB_NIL);
}
/* String#undump — inverse of #dump: parse a "..."-wrapped, escaped literal back
 * to the original bytes.  (.force_encoding(...) suffix is ignored.) */
static RESULT korb_m_str_undump(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    if (s->len < 2 || korb_strbuf_data(s->buf)[0] != '"')
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "invalid dumped string; not wrapped with '\"' nor '\"...\".force_encoding(\"...\")' form");
    unsigned char *const out = malloc(s->len);
    uint32_t olen = 0, i = 1; bool closed = false;
    #define KORB_HEXV(x) ((x) >= '0' && (x) <= '9' ? (x) - '0' : (x) >= 'A' && (x) <= 'F' ? (x) - 'A' + 10 : (x) >= 'a' && (x) <= 'f' ? (x) - 'a' + 10 : -1)
    while (i < s->len) {
        const int ch = (unsigned char)korb_strbuf_data(s->buf)[i];
        if (ch == '"') { closed = true; i++; break; }
        if (ch != '\\') { out[olen++] = (unsigned char)ch; i++; continue; }
        if (++i >= s->len) break;
        const int e = (unsigned char)korb_strbuf_data(s->buf)[i++];
        switch (e) {
            case 'n': out[olen++] = '\n'; break;   case 't': out[olen++] = '\t'; break;
            case 'r': out[olen++] = '\r'; break;   case 'a': out[olen++] = 7;    break;
            case 'b': out[olen++] = 8;    break;   case 'v': out[olen++] = 11;   break;
            case 'f': out[olen++] = 12;   break;   case 'e': out[olen++] = 27;   break;
            case 's': out[olen++] = ' ';  break;   case '0': out[olen++] = 0;    break;
            case '"': out[olen++] = '"';  break;   case '\\': out[olen++] = '\\'; break;
            case '#': out[olen++] = '#';  break;
            case 'x': {                            /* \xHH */
                if (i + 1 < s->len) { const int hi = KORB_HEXV((unsigned char)korb_strbuf_data(s->buf)[i]), lo = KORB_HEXV((unsigned char)korb_strbuf_data(s->buf)[i + 1]);
                    if (hi >= 0 && lo >= 0) { out[olen++] = (unsigned char)(hi * 16 + lo); i += 2; break; } }
                out[olen++] = 'x'; break;
            }
            case 'u': {                            /* \uHHHH or \u{HHHH ...} → UTF-8 */
                uint32_t cp = 0; bool brace = (i < s->len && korb_strbuf_data(s->buf)[i] == '{');
                if (brace) i++;
                int nd = 0; while (i < s->len) { const int hv = KORB_HEXV((unsigned char)korb_strbuf_data(s->buf)[i]); if (hv < 0) break; cp = cp * 16 + (uint32_t)hv; i++; nd++; if (!brace && nd == 4) break; }
                if (brace && i < s->len && korb_strbuf_data(s->buf)[i] == '}') i++;
                if (cp < 0x80) out[olen++] = (unsigned char)cp;
                else if (cp < 0x800) { out[olen++] = (unsigned char)(0xC0 | (cp >> 6)); out[olen++] = (unsigned char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out[olen++] = (unsigned char)(0xE0 | (cp >> 12)); out[olen++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); out[olen++] = (unsigned char)(0x80 | (cp & 0x3F)); }
                else { out[olen++] = (unsigned char)(0xF0 | (cp >> 18)); out[olen++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F)); out[olen++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); out[olen++] = (unsigned char)(0x80 | (cp & 0x3F)); }
                break;
            }
            default: out[olen++] = (unsigned char)e; break;   /* unknown escape → literal */
        }
    }
    #undef KORB_HEXV
    if (!closed) { free(out); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "invalid dumped string; not wrapped with '\"' nor '\"...\".force_encoding(\"...\")' form"); }
    KorbString *r = korb_str_alloc(c, slots, olen);   /* may move; out is libc-stable */
    memcpy(korb_strbuf_data(r->buf), out, olen); free(out);
    korb_strbuf_data(r->buf)[olen] = '\0'; r->len = olen;
    return RESULT_OK((VALUE)r);
}
static RESULT korb_m_str_swapcase(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT oo = korb_str_case_opts(c, slots, a, 3); if (UNLIKELY(oo.state != KORB_NORMAL)) return oo;
    const bool ascii_only = FIX2LONG(oo.value) == 1;
    uint32_t len = VAL2STR(VALUE_REF_GET(self))->len;
    KorbString *r = korb_str_alloc(c, slots, len);
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));     /* re-read after GC */
    korb_case_transform(korb_strbuf_data(s->buf), korb_strbuf_data(r->buf), len, 3, ascii_only);   /* swapcase (ASCII + Latin-1) */
    KORB_STR_ENC_SET((VALUE)r, KORB_STR_ENC(VALUE_REF_GET(self)));   /* preserve source encoding */
    return RESULT_OK((VALUE)r);
}
/* ljust(0)/rjust(1)/center(2) — char-width padding via a transient buffer */
static RESULT korb_str_pad(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE wv = VALUE_SLICE_GET(a, 0);
    { RESULT cr = korb_coerce_to_int(c, slots, &wv); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }   /* width #to_int */
    intptr_t width;
    if (UNLIKELY(!korb_to_index(wv, &width))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    VALUE padv = (VALUE_SLICE_LEN(a) >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    if (padv != KORB_NIL && !KORB_STRING_P(padv)) {       /* padstr #to_str */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(padv) && korb_responds_to_coerce_p(c, slots, &padv, to_str)) {
            slots[0] = padv;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            padv = sr.value;
        }
        if (UNLIKELY(!KORB_STRING_P(padv)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 1)));
    }
    slots[0] = padv;                                      /* root pad across self alloc */
    const KorbString *padstr = (padv != KORB_NIL) ? VAL2STR(padv) : NULL;
    if (padstr && padstr->len == 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "zero width padding");
    const KorbString *s = VAL2STR(VALUE_REF_GET(self));
    uint32_t ncp = korb_utf8_count(korb_strbuf_data(s->buf), s->len);
    if (width <= (intptr_t)ncp) return korb_str_slice_new(c, slots, self, 0, s->len);
    uint32_t total_pad = (uint32_t)width - ncp;
    uint32_t left = mode == 1 ? total_pad : mode == 2 ? total_pad / 2 : 0;
    uint32_t right = total_pad - left;
    const char *pb = padstr ? korb_strbuf_data(padstr->buf) : " ";
    uint32_t pl = padstr ? padstr->len : 1;
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    for (uint32_t i = 0; i < left; i++)  fputc(pb[i % pl], ms);   /* byte-cycle pad (ASCII pad exact) */
    fwrite(korb_strbuf_data(s->buf), 1, s->len, ms);
    for (uint32_t i = 0; i < right; i++) fputc(pb[i % pl], ms);
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_STR_ENC(VALUE_REF_GET(self)));   /* preserve self's encoding (ASCII pad) */
    return r;
}
static RESULT korb_m_str_ljust(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_pad(c, slots, self, a, 0); }
static RESULT korb_m_str_rjust(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_str_pad(c, slots, self, a, 1); }
static RESULT korb_m_str_center(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_str_pad(c, slots, self, a, 2); }

/* Enumerator Enumerable methods that aren't intrinsic to Enumerator: force the
 * (eager) value array and delegate to the Array version.  Here at the end of the
 * last builtin include so every korb_m_ary_* (array / range / array_int_ext /
 * array_ext) is in scope.  These all return fresh values (hash / array /
 * enumerator / index), never the receiver, so plain delegation is correct. */
#define ENUM_DELEGATE_BLK(name, aryfn)                                                       \
static RESULT korb_m_enum_##name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,         \
                                 NODE *block, VALUE *def_env, VALUE *cself) {                 \
    RESULT av = korb_m_enum_to_a(c, slots, self, a);                                          \
    if (UNLIKELY(av.state != KORB_NORMAL)) return av;                                         \
    slots[0] = av.value;                                                                      \
    return aryfn(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);            \
}
ENUM_DELEGATE_BLK(group_by,    korb_m_ary_group_by)
ENUM_DELEGATE_BLK(partition,   korb_m_ary_partition)
ENUM_DELEGATE_BLK(minmax,      korb_m_ary_minmax)
ENUM_DELEGATE_BLK(zip,         korb_m_ary_zip)
ENUM_DELEGATE_BLK(find_index,  korb_m_ary_find_index)
ENUM_DELEGATE_BLK(chunk_while, korb_m_ary_chunk_while)
ENUM_DELEGATE_BLK(slice_when,  korb_m_ary_slice_when)
ENUM_DELEGATE_BLK(chunk,       korb_m_ary_chunk)
#undef ENUM_DELEGATE_BLK
static RESULT korb_m_enum_tally(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT av = korb_m_enum_to_a(c, slots, self, a);
    if (UNLIKELY(av.state != KORB_NORMAL)) return av;
    slots[0] = av.value;
    return korb_m_ary_tally(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
}

static void
