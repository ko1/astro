/* koruby_precise — array_ext.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- more Array methods -------------------------------------------------- */

/* String#pack("P"/"p") side table (see context.h korb_vm).  A real pointer to
 * the string bytes would dangle under the moving GC, so we copy the bytes into
 * a malloc'd slot and embed its 1-based index in the packed 8 bytes; unpack
 * recovers them.  The store holds raw byte copies (no VALUEs) so it needs no GC
 * root scanning and is unaffected by object motion.  Returns a 1-based index
 * (0 is reserved for nil / "no string"). */
static uint32_t korb_pack_ptr_register(CTX *const c, const char *const data, const uint32_t len) {
    struct korb_vm *const vm = c->vm;
    if (vm->pack_ptr_count == vm->pack_ptr_cap) {
        const uint32_t ncap = vm->pack_ptr_cap ? vm->pack_ptr_cap * 2 : 8;
        vm->pack_ptr_bufs = (char **)realloc(vm->pack_ptr_bufs, (size_t)ncap * sizeof(char *));
        vm->pack_ptr_lens = (uint32_t *)realloc(vm->pack_ptr_lens, (size_t)ncap * sizeof(uint32_t));
        if (!vm->pack_ptr_bufs || !vm->pack_ptr_lens) { fprintf(stderr, "koruby_precise: pack(P) OOM\n"); abort(); }
        vm->pack_ptr_cap = ncap;
    }
    char *const copy = (char *)malloc(len ? len : 1);
    if (!copy) { fprintf(stderr, "koruby_precise: pack(P) OOM\n"); abort(); }
    memcpy(copy, data, len);
    const uint32_t idx = vm->pack_ptr_count++;
    vm->pack_ptr_bufs[idx] = copy;
    vm->pack_ptr_lens[idx] = len;
    return idx + 1;
}

/* Recover the bytes registered by korb_pack_ptr_register; NULL if idx1 is 0 or
 * out of range.  The returned pointer is malloc'd and stable across GC. */
static const char *korb_pack_ptr_lookup(const CTX *const c, const uint64_t idx1, uint32_t *const lenp) {
    const struct korb_vm *const vm = c->vm;
    if (idx1 == 0 || idx1 > vm->pack_ptr_count) return NULL;
    *lenp = vm->pack_ptr_lens[idx1 - 1];
    return vm->pack_ptr_bufs[idx1 - 1];
}

/* Array#pack — template engine over a manually-managed byte buffer (so X can
 * truncate).  Supports C/c, x, X, a/A/Z (strings), B/b (bits), H/h (hex),
 * M (quoted-printable), m (base64), u (uuencode), w (BER), P/p (pointer: a
 * 1-based index into the side table above, recoverable by unpack; 0 for nil).
 * No GC alloc happens between fetching elements and emitting bytes, so the bare
 * array pointer stays valid for the whole loop. */
/* Convert a pack element to an integer for a numeric directive: Fixnum, Bignum
 * (low 64 bits), Float (truncated), or an object via #to_int; nil/true/false/
 * String/etc. → TypeError.  May GC (dispatches #to_int); the caller re-reads its
 * array afterwards.  Uses scratch at `sc` (must not overlap the caller's rooted
 * template slot). */
static RESULT korb_pack_int_val(CTX *c, VALUE *sc, VALUE e, int64_t *out) {
    if (FIXNUM_P(e)) { *out = FIX2LONG(e); return RESULT_OK(KORB_NIL); }
    if (KORB_FLOAT_P(e)) { *out = (int64_t)korb_float_val(e); return RESULT_OK(KORB_NIL); }
    if (KORB_BIGNUM_P(e)) { korb_mp_t z; korb_to_mpz(e, z); uint64_t lo = (uint64_t)korb_mp_get_ui(z); *out = (korb_mp_sgn(z) < 0) ? -(int64_t)lo : (int64_t)lo; korb_mp_clear(z); return RESULT_OK(KORB_NIL); }
    if (KORB_OBJECT_P(e) && korb_responds_to_coerce(c, sc, e, korb_intern(c->vm, "to_int", 6))) {
        sc[0] = e;
        RESULT r = korb_send_impl(c, sc + 1, korb_intern(c->vm, "to_int", 6), 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (FIXNUM_P(r.value)) { *out = FIX2LONG(r.value); return RESULT_OK(KORB_NIL); }
        if (KORB_BIGNUM_P(r.value)) { korb_mp_t z; korb_to_mpz(r.value, z); uint64_t lo = (uint64_t)korb_mp_get_ui(z); *out = (korb_mp_sgn(z) < 0) ? -(int64_t)lo : (int64_t)lo; korb_mp_clear(z); return RESULT_OK(KORB_NIL); }
        return korb_raise(c, sc, KORB_E_TYPE, 0, "can't convert Object to Integer (Object#to_int gives %s)", korb_type_name(r.value));
    }
    return korb_raise(c, sc, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(e));
}
static RESULT korb_m_ary_pack(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    /* Template lives in slots[0] for the whole call (rooted) so it survives an
     * element #to_str dispatch; self is a rooted VALUE_REF (GC re-reads it), so
     * `ary`/`t` are re-derived after any coercion.  Arity is -1 so a trailing
     * `buffer:` kwargs Hash can be honoured (pack appends into it, zero-copy). */
    const uint32_t na = VALUE_SLICE_LEN(a);
    if (UNLIKELY(na < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    slots[0] = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(slots[0]))) {             /* coerce the template via #to_str */
        if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce(c, slots + 1, slots[0], korb_intern(c->vm, "to_str", 6))) {
            RESULT sr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            slots[0] = sr.value;
        }
        if (UNLIKELY(!KORB_STRING_P(slots[0])))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    const KorbString *t = VAL2STR(slots[0]);
    const KorbArray *ary = SELF_ARY;
    uint8_t *ob = NULL; size_t olen = 0, ocap = 0;
    #define PK_RESERVE(n) do { if (olen + (size_t)(n) > ocap) { ocap = (olen + (size_t)(n)) * 2 + 64; ob = (uint8_t *)realloc(ob, ocap); if (!ob) { fprintf(stderr, "koruby_precise: pack OOM\n"); abort(); } } } while (0)
    #define PK_PUT(b)     do { PK_RESERVE(1); ob[olen++] = (uint8_t)(b); } while (0)
    #define PK_PUTS(p,n)  do { PK_RESERVE(n); memcpy(ob + olen, (p), (n)); olen += (size_t)(n); } while (0)
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t ti = 0, ai = 0;                             /* template / array cursors */
    unsigned enc_state = 1;                              /* 1 = US-ASCII, 2 = UTF-8, 0 = ASCII-8BIT */
    unsigned errtype = 0; const char *errmsg = NULL; char bad = 0; bool has_bad = false;
    char errbuf[128];                                    /* for messages naming the offending type */
    while (ti < t->len) {
        const char d = korb_strbuf_data(t->buf)[ti++];
        if (d == ' ' || d == '\t' || d == '\n' || d == '\r' || d == '\v' || d == '\f') continue;
        if (d == '#') { while (ti < t->len && korb_strbuf_data(t->buf)[ti] != '\n') ti++; continue; }   /* comment to EOL */
        bool bang = false, force_little = false, force_big = false; char bangch = 0;   /* `!`/`_`=native size, `<`=little, `>`=big (any order) */
        for (;;) {
            if (ti < t->len && (korb_strbuf_data(t->buf)[ti] == '!' || korb_strbuf_data(t->buf)[ti] == '_')) { bang = true; bangch = korb_strbuf_data(t->buf)[ti]; ti++; }
            else if (ti < t->len && korb_strbuf_data(t->buf)[ti] == '<') { force_little = true; ti++; }
            else if (ti < t->len && korb_strbuf_data(t->buf)[ti] == '>') { force_big = true; ti++; }
            else break;
        }
        if ((force_little || force_big) && !strchr("sSiIlLqQjJ", d)) {   /* `<`/`>` only after integer types */
            errtype = KORB_E_ARGUMENT; errmsg = "'<' allowed only after types sSiIlLqQjJ"; break;
        }
        if (bang && !strchr("sSiIlLqQjJ", d)) {                         /* `!`/`_` only after integer types (not floats/strings) */
            errtype = KORB_E_ARGUMENT; errmsg = (bangch == '_') ? "'_' allowed only after types sSiIlLqQjJ" : "'!' allowed only after types sSiIlLqQjJ"; break;
        }
        bool star = false, has_cnt = false; long cnt = 1;
        if (ti < t->len && korb_strbuf_data(t->buf)[ti] == '*') { star = true; ti++; }
        else if (ti < t->len && korb_strbuf_data(t->buf)[ti] >= '0' && korb_strbuf_data(t->buf)[ti] <= '9') {
            has_cnt = true; cnt = 0; while (ti < t->len && korb_strbuf_data(t->buf)[ti] >= '0' && korb_strbuf_data(t->buf)[ti] <= '9') cnt = cnt * 10 + (korb_strbuf_data(t->buf)[ti++] - '0');
        }
        /* Result encoding, CRuby's enc_info state machine: US-ASCII while every
         * directive is 7-bit-clean (U / M / m / u), UTF-8 if a U appeared, and
         * ASCII-8BIT as soon as any other directive is used. */
        if (enc_state != 0) {
            if (d == 'U') { if (enc_state == 1) enc_state = 2; }
            else if (d != 'M' && d != 'm' && d != 'u') enc_state = 0;
        }
        if (d == 'C' || d == 'c') {
            uint32_t emit = star ? (ary->len - ai) : (uint32_t)cnt;
            for (uint32_t k = 0; k < emit; k++) {
                if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
                VALUE e = korb_items_data(ary->items)[ai++];
                int64_t b;
                RESULT ir = korb_pack_int_val(c, slots + 2, e, &b);
                if (UNLIKELY(ir.state != KORB_NORMAL)) { free(ob); return ir; }
                ary = SELF_ARY;                          /* re-read after a possible #to_int GC */
                PK_PUT(b & 0xFF);
            }
        } else if (d == 'N' || d == 'n' || d == 'V' || d == 'v' || d == 'L' || d == 'l' ||
                   d == 'S' || d == 's' || d == 'Q' || d == 'q' || d == 'I' || d == 'i' ||
                   d == 'J' || d == 'j') {   /* multi-byte ints (J/j = native intptr = 8-byte LE here) */
            int sz; bool big;
            switch (d) {
                case 'N': sz = 4; big = true;  break;   case 'n': sz = 2; big = true;  break;
                case 'V': sz = 4; big = false; break;   case 'v': sz = 2; big = false; break;
                case 'S': case 's': sz = 2; big = false; break;
                case 'Q': case 'q': case 'J': case 'j': sz = 8; big = false; break;
                default:  sz = (bang && (d == 'L' || d == 'l')) ? 8 : 4; big = false; break;   /* L/l/I/i = 4-byte native; l!/L! = long = 8 */
            }
            if (force_little) big = false;                /* `<` / `>` override endianness */
            if (force_big) big = true;
            uint32_t emit = star ? (ary->len - ai) : (uint32_t)cnt;
            for (uint32_t k = 0; k < emit; k++) {
                if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
                VALUE e = korb_items_data(ary->items)[ai++];
                int64_t iv;
                RESULT ir = korb_pack_int_val(c, slots + 2, e, &iv);
                if (UNLIKELY(ir.state != KORB_NORMAL)) { free(ob); return ir; }
                ary = SELF_ARY;                          /* re-read after a possible #to_int GC */
                const uint64_t v = (uint64_t)iv;
                for (int b = 0; b < sz; b++) PK_PUT((v >> (8 * (big ? (sz - 1 - b) : b))) & 0xFF);
            }
        } else if (d == 'e' || d == 'E' || d == 'g' || d == 'G' || d == 'f' || d == 'F' || d == 'd' || d == 'D') {   /* IEEE floats */
            int sz; bool big;
            switch (d) {
                case 'g': sz = 4; big = true;  break;   case 'G': sz = 8; big = true;  break;
                case 'E': sz = 8; big = false; break;   case 'd': case 'D': sz = 8; big = false; break;
                default:  sz = 4; big = false; break;   /* e/f/F = 4-byte little/native */
            }
            uint32_t emit = star ? (ary->len - ai) : (uint32_t)cnt;
            for (uint32_t k = 0; k < emit; k++) {
                if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
                VALUE e = korb_items_data(ary->items)[ai++];
                double dv;
                if (!korb_num_to_d(e, &dv)) { errtype = KORB_E_TYPE; errmsg = "no implicit conversion to float"; break; }   /* nil/true/false/String → TypeError */
                unsigned char tmp[8];
                if (sz == 4) { float f = (float)dv; memcpy(tmp, &f, 4); } else memcpy(tmp, &dv, 8);
                for (int b = 0; b < sz; b++) PK_PUT(tmp[big ? (sz - 1 - b) : b]);
            }
        } else if (d == 'U') {                            /* UTF-8: codepoints → bytes (extended, up to 6) */
            uint32_t emit = star ? (ary->len - ai) : (uint32_t)cnt;
            for (uint32_t k = 0; k < emit; k++) {
                if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
                VALUE e = korb_items_data(ary->items)[ai++];
                int64_t cpv;
                RESULT ir = korb_pack_int_val(c, slots + 2, e, &cpv);   /* #to_int coercion / TypeError */
                if (UNLIKELY(ir.state != KORB_NORMAL)) { free(ob); return ir; }
                ary = SELF_ARY;                          /* re-read after a possible #to_int GC */
                if (cpv < 0 || cpv > 0x7FFFFFFF) { errtype = KORB_E_RANGE; errmsg = "pack(U): value out of range"; break; }
                const uint32_t cp = (uint32_t)cpv;
                if (cp < 0x80) PK_PUT(cp);
                else if (cp < 0x800) { PK_PUT(0xC0 | (cp >> 6)); PK_PUT(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { PK_PUT(0xE0 | (cp >> 12)); PK_PUT(0x80 | ((cp >> 6) & 0x3F)); PK_PUT(0x80 | (cp & 0x3F)); }
                else if (cp < 0x200000) { PK_PUT(0xF0 | (cp >> 18)); PK_PUT(0x80 | ((cp >> 12) & 0x3F)); PK_PUT(0x80 | ((cp >> 6) & 0x3F)); PK_PUT(0x80 | (cp & 0x3F)); }
                else if (cp < 0x4000000) { PK_PUT(0xF8 | (cp >> 24)); PK_PUT(0x80 | ((cp >> 18) & 0x3F)); PK_PUT(0x80 | ((cp >> 12) & 0x3F)); PK_PUT(0x80 | ((cp >> 6) & 0x3F)); PK_PUT(0x80 | (cp & 0x3F)); }
                else { PK_PUT(0xFC | (cp >> 30)); PK_PUT(0x80 | ((cp >> 24) & 0x3F)); PK_PUT(0x80 | ((cp >> 18) & 0x3F)); PK_PUT(0x80 | ((cp >> 12) & 0x3F)); PK_PUT(0x80 | ((cp >> 6) & 0x3F)); PK_PUT(0x80 | (cp & 0x3F)); }
            }
        } else if (d == 'x') {
            uint32_t emit = star ? 0 : (uint32_t)cnt;
            for (uint32_t k = 0; k < emit; k++) PK_PUT(0);
        } else if (d == 'X') {                            /* back up cnt bytes (X* → 0) */
            size_t back = star ? 0 : (size_t)cnt;
            if (back > olen) { errtype = KORB_E_ARGUMENT; errmsg = "X outside of string"; break; }
            olen -= back;
        } else if (d == '@') {                            /* absolute output position: NUL-fill or truncate (@* → 0) */
            const size_t pos = star ? 0 : (size_t)cnt;
            if (pos > olen) { PK_RESERVE(pos - olen); memset(ob + olen, 0, pos - olen); }
            olen = pos;
        } else if (d == 'w') {                            /* BER-compressed integer (star/count → all/n) */
            uint32_t emit = star ? (ary->len - ai) : (uint32_t)cnt;
            for (uint32_t rr = 0; rr < emit; rr++) {
                if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
                VALUE e = korb_items_data(ary->items)[ai++];
                int64_t iv;
                RESULT ir = korb_pack_int_val(c, slots + 2, e, &iv);   /* #to_int; nil/true/false → TypeError */
                if (UNLIKELY(ir.state != KORB_NORMAL)) { free(ob); return ir; }
                ary = SELF_ARY;                          /* re-read after a possible #to_int GC */
                if (iv < 0) { errtype = KORB_E_ARGUMENT; errmsg = "can't compress negative numbers"; break; }
                uint64_t v = (uint64_t)iv;
                uint8_t tmp[16]; int n = 0;
                tmp[n++] = (uint8_t)(v & 0x7f); v >>= 7;
                while (v) { tmp[n++] = (uint8_t)((v & 0x7f) | 0x80); v >>= 7; }
                while (n) PK_PUT(tmp[--n]);                /* big-endian, high bit on all but last */
            }
        } else if (d == 'P' || d == 'p') {                /* pointer: 1-based side-table index (0 = nil) */
            if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
            VALUE e = korb_items_data(ary->items)[ai++];
            uint64_t idx = 0;
            if (KORB_STRING_P(e)) { const KorbString *es = VAL2STR(e); idx = korb_pack_ptr_register(c, korb_strbuf_data(es->buf), es->len); }
            for (int k = 0; k < 8; k++) PK_PUT((idx >> (8 * k)) & 0xff);
        } else if (d == 'a' || d == 'A' || d == 'Z' || d == 'B' || d == 'b' || d == 'H' || d == 'h' || d == 'M' || d == 'm' || d == 'u') {
            if (ai >= ary->len) { errtype = KORB_E_ARGUMENT; errmsg = "too few arguments"; break; }
            VALUE e = korb_items_data(ary->items)[ai++];
            const bool coerce = (d == 'M');   /* only M to_s's its operand; m/u demand a String (#to_str) */
            char cobuf[64]; const char *ed; uint32_t elen;
            if (KORB_STRING_P(e)) { const KorbString *es = VAL2STR(e); ed = korb_strbuf_data(es->buf); elen = es->len; }
            else if (e == KORB_NIL) { ed = ""; elen = 0; }
            else if (coerce && FIXNUM_P(e)) { elen = korb_fmt_int((intptr_t)FIX2LONG(e), 10, cobuf); ed = cobuf; }
            else if (coerce && SYMBOL_P(e)) { ed = korb_sym_name(c->vm, SYM2ID(e)); elen = (uint32_t)strlen(ed); }
            else if (coerce && KORB_FLOAT_P(e)) { elen = korb_float_to_s(korb_float_val(e), cobuf); ed = cobuf; }
            else if (coerce && e == KORB_TRUE) { ed = "true"; elen = 4; }
            else if (coerce && e == KORB_FALSE) { ed = "false"; elen = 5; }
            else if (!coerce && KORB_OBJECT_P(e)) {       /* a/A/Z/B/b/H/h: coerce the element via #to_str */
                slots[1] = e;
                if (!korb_responds_to_coerce(c, slots + 2, slots[1], korb_intern(c->vm, "to_str", 6))) {
                    snprintf(errbuf, sizeof errbuf, "no implicit conversion of %s into String", korb_type_name(slots[1]));
                    errtype = KORB_E_TYPE; errmsg = errbuf; break;
                }
                RESULT sr = korb_send_impl(c, slots + 2, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) { free(ob); return sr; }
                if (!KORB_STRING_P(sr.value)) { errtype = KORB_E_TYPE; errmsg = "no implicit conversion into String"; break; }
                slots[1] = sr.value;                      /* root the coerced String; ed used immediately (no GC before emit) */
                ed = korb_strbuf_data(VAL2STR(slots[1])->buf); elen = VAL2STR(slots[1])->len;
                ary = SELF_ARY; t = VAL2STR(slots[0]);    /* re-read: the dispatch may have moved them */
            }
            else if (coerce) {                            /* M/m/u: coerce any other value via #to_s (Bignum/Array/object) */
                slots[1] = e;
                RESULT sr = korb_send_impl(c, slots + 2, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) { free(ob); return sr; }
                if (KORB_STRING_P(sr.value)) { slots[1] = sr.value; ed = korb_strbuf_data(VAL2STR(slots[1])->buf); elen = VAL2STR(slots[1])->len; }
                else {                                    /* #to_s gave a non-String → the default object representation */
                    char *db = NULL; size_t dsz = 0; FILE *dms = open_memstream(&db, &dsz);
                    if (dms) { korb_fprint_to_s(c, dms, e); fclose(dms); }
                    elen = (uint32_t)dsz; if (elen >= sizeof cobuf) elen = sizeof cobuf - 1;
                    memcpy(cobuf, db ? db : "", elen); free(db); ed = cobuf;
                }
                ary = SELF_ARY; t = VAL2STR(slots[0]);
            }
            else {
                snprintf(errbuf, sizeof errbuf, "no implicit conversion of %s into String", korb_type_name(e));
                errtype = KORB_E_TYPE; errmsg = errbuf; break;
            }
            if (d == 'a' || d == 'A') {
                uint32_t want = star ? elen : (uint32_t)cnt;
                const char pad = (d == 'A') ? ' ' : '\0';
                for (uint32_t k = 0; k < want; k++) PK_PUT(k < elen ? ed[k] : pad);
            } else if (d == 'Z') {                        /* 'Z*' appends a NUL; 'Z<n>' is 'a<n>' (take n, NUL-pad) */
                if (star) { PK_PUTS(ed, elen); PK_PUT(0); }
                else { const uint32_t want = (uint32_t)cnt; for (uint32_t k = 0; k < want; k++) PK_PUT(k < elen ? ed[k] : 0); }
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
                const long wrap = (has_cnt && cnt > 1) ? cnt : 72;   /* CRuby: M<n> (n>=2) → n; 0/1/none → 72 */
                long col = 0; char last = 0;
                static const char HX[] = "0123456789ABCDEF";
                for (uint32_t k = 0; k < elen; k++) {
                    const unsigned char ch = (unsigned char)ed[k];
                    if (ch == '\n') { if (last == ' ' || last == '\t') PK_PUTS("=\n", 2); PK_PUT('\n'); col = 0; last = '\n'; continue; }
                    char enc[3]; int en;
                    if (ch == '=' || ch > 126 || (ch < 32 && ch != '\t')) { en = 3; enc[0] = '='; enc[1] = HX[ch >> 4]; enc[2] = HX[ch & 15]; }
                    else { en = 1; enc[0] = (char)ch; }
                    PK_PUTS(enc, en); col += en; last = enc[en - 1];
                    /* the break comes after the column overflows, so a line can
                     * run to wrap+2 characters (CRuby's qpencode) */
                    if (col > wrap) { PK_PUTS("=\n", 2); col = 0; last = '\n'; }
                }
                if (col > 0) PK_PUTS("=\n", 2);
            } else {                                      /* m (base64) / u (uuencode) */
                /* `m0` = base64 with no line breaks (and no trailing newline);
                 * `m` / `m*` / `m1` / `m2` = 45 bytes per line; `mN` (N>=3) = N. */
                const bool m_nowrap = (d == 'm' && has_cnt && cnt == 0);
                long per;
                if (d == 'm' && has_cnt && (cnt == 1 || cnt == 2)) per = 45;
                else per = has_cnt ? cnt : 45;
                if (m_nowrap) per = elen > 0 ? (long)elen : 3;
                else { per = (per / 3) * 3; if (per < 3) per = 3; }
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
                    if (!m_nowrap) PK_PUT('\n');             /* m0 emits no line break */
                }
            }
        } else {
            bad = d; has_bad = true; break;
        }
        if (errmsg) break;
    }
    if (errmsg) { free(ob); return korb_raise(c, slots, errtype, 0, "%s", errmsg); }
    if (has_bad) { free(ob); return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unknown pack directive '%c' in '%.*s'", bad, (int)t->len, korb_strbuf_data(t->buf)); }
    /* `buffer:` keyword (trailing kwargs Hash) → append the packed bytes into the
     * given String and return it, instead of allocating a fresh result. */
    if (na >= 2 && KORB_HASH_P(VALUE_SLICE_GET(a, na - 1))) {
        const KorbHash *kh = VAL2HASH(VALUE_SLICE_GET(a, na - 1));
        const int32_t bi = korb_hash_find(kh, ID2SYM(korb_intern(c->vm, "buffer", 6)));
        if (bi >= 0) {
            slots[1] = korb_items_data(kh->items)[2 * bi + 1];                 /* root the buffer String across the cat's GC */
            if (UNLIKELY(!KORB_STRING_P(slots[1]))) { free(ob); return korb_raise(c, slots, KORB_E_TYPE, 0, "buffer must be String"); }
            RESULT ar = korb_str_cat(c, slots + 2, VALUE_REF_AT(&slots[1]), ob ? (const char *)ob : "", (uint32_t)olen);
            free(ob);
            if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
            return RESULT_OK(slots[1]);
        }
    }
    RESULT r = korb_str_new(c, slots, ob ? (const char *)ob : "", (uint32_t)olen);
    free(ob);
    /* Result encoding follows the directives used: U alone yields UTF-8, the
     * 7-bit encoders (M/m/u) US-ASCII, everything else ASCII-8BIT (CRuby). */
    if (LIKELY(r.state == KORB_NORMAL))
        KORB_STR_ENC_SET(r.value, enc_state == 2 ? KORB_ENC_UTF8
                                : enc_state == 1 ? KORB_ENC_USASCII : KORB_ENC_BINARY);
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
    uint32_t na = VALUE_SLICE_LEN(a);
    if (UNLIKELY(na < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* template (rooted across the coercion) */
    if (UNLIKELY(!KORB_STRING_P(slots[0]))) {            /* coerce the template via #to_str (as pack does) */
        if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce(c, slots + 1, slots[0], korb_intern(c->vm, "to_str", 6))) {
            const RESULT sr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            slots[0] = sr.value;
        }
        if (UNLIKELY(!KORB_STRING_P(slots[0])))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    const VALUE tv = slots[0];
    long off = 0;                                        /* optional offset: keyword */
    if (na >= 2 && KORB_HASH_P(VALUE_SLICE_GET(a, na - 1))) {
        const KorbHash *h = VAL2HASH(VALUE_SLICE_GET(a, na - 1));
        const int32_t oi = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "offset", 6)));
        if (oi >= 0) {
            const VALUE ov = korb_items_data(h->items)[2 * oi + 1];
            if (UNLIKELY(!FIXNUM_P(ov)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(ov));
            off = FIX2LONG(ov);
            if (UNLIKELY(off < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "offset can't be negative");
            if (UNLIKELY((uint32_t)off > VAL2STR(VALUE_REF_GET(self))->len))
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "offset outside of string");
        }
    }
    slots[0] = tv;                                        /* template */
    slots[1] = VALUE_REF_GET(self);                      /* subject */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 3, 0));    /* result */
    VALUE_REF res = VALUE_REF_AT(&slots[2]);
    uint32_t ti = 0, si = (uint32_t)off;
    while (ti < VAL2STR(slots[0])->len) {
        const KorbString *t = VAL2STR(slots[0]);
        const char d = korb_strbuf_data(t->buf)[ti++];
        if (d == ' ' || d == '\t' || d == '\n' || d == '\r' || d == '\v' || d == '\f') continue;
        if (d == '#') {                                   /* comment: to the newline (or the end) */
            while (ti < t->len && korb_strbuf_data(t->buf)[ti] != '\n') ti++;
            continue;
        }
        bool bang = false, force_little = false, force_big = false;   /* `!`/`_`=native size, `<`/`>`=endianness */
        char bangch = '!';
        for (;;) {
            if (ti < t->len && (korb_strbuf_data(t->buf)[ti] == '!' || korb_strbuf_data(t->buf)[ti] == '_')) { bang = true; bangch = korb_strbuf_data(t->buf)[ti]; ti++; }
            else if (ti < t->len && korb_strbuf_data(t->buf)[ti] == '<') { force_little = true; ti++; }
            else if (ti < t->len && korb_strbuf_data(t->buf)[ti] == '>') { force_big = true; ti++; }
            else break;
        }
        if ((force_little || force_big) && !strchr("sSiIlLqQjJ", d))
            return korb_raise(c, slots + 3, KORB_E_ARGUMENT, 0, "'<' allowed only after types sSiIlLqQjJ");
        if (bang && !strchr("sSiIlLqQjJ", d))
            return korb_raise(c, slots + 3, KORB_E_ARGUMENT, 0, "'%c' allowed only after types sSiIlLqQjJ", bangch);
        bool star = false; long cnt = (d == '@') ? 0 : 1;   /* a bare '@' means position 0 (CRuby) */
        if (ti < t->len && korb_strbuf_data(t->buf)[ti] == '*') { star = true; ti++; }
        else if (ti < t->len && korb_strbuf_data(t->buf)[ti] >= '0' && korb_strbuf_data(t->buf)[ti] <= '9') {
            cnt = 0; while (ti < t->len && korb_strbuf_data(t->buf)[ti] >= '0' && korb_strbuf_data(t->buf)[ti] <= '9') cnt = cnt * 10 + (korb_strbuf_data(t->buf)[ti++] - '0');
        }
        const uint32_t slen = VAL2STR(slots[1])->len;
        if (d == 'a' || d == 'A' || d == 'Z') {          /* string slice (one value) */
            const uint32_t avail = slen - si;
            uint32_t take = star ? avail : ((uint32_t)cnt < avail ? (uint32_t)cnt : avail);
            uint32_t vlen = take;
            if (d == 'Z') {
                for (uint32_t k = 0; k < take; k++) if (korb_strbuf_data(VAL2STR(slots[1])->buf)[si + k] == '\0') { vlen = k; break; }
                /* 'Z*' consumes the NUL too, so the next Z* starts after it */
                if (star && vlen < take) take = vlen + 1;
            } else if (d == 'A') {
                while (vlen > 0) { const char ch = korb_strbuf_data(VAL2STR(slots[1])->buf)[si + vlen - 1]; if (ch == ' ' || ch == '\0') vlen--; else break; }
            }
            KorbString *r = korb_str_alloc(c, slots + 3, vlen);   /* may move slots[0..2] */
            memcpy(korb_strbuf_data(r->buf), korb_strbuf_data(VAL2STR(slots[1])->buf) + si, vlen);   /* re-read subject */
            slots[3] = (VALUE)r;
            KORB_STR_ENC_SET(slots[3], KORB_ENC_BINARY);          /* a/A/Z yield ASCII-8BIT */
            CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
            si += take;
        } else if (d == 'x') {                            /* skip bytes forward (no value) */
            uint32_t skip = star ? (slen - si) : (uint32_t)cnt;
            if (UNLIKELY(si + skip > slen)) return korb_raise(c, slots + 3, KORB_E_ARGUMENT, 0, "x outside of string");
            si += skip;
        } else if (d == 'X') {                            /* move the read index back (no value) */
            const uint32_t back = star ? (slen - si) : (uint32_t)cnt;   /* '*' = the bytes left, as CRuby */
            if (UNLIKELY(back > si)) return korb_raise(c, slots + 3, KORB_E_ARGUMENT, 0, "X outside of string");
            si -= back;
        } else if (d == '@') {                            /* absolute read index (no value) */
            const uint32_t pos = star ? (slen - si) : (uint32_t)cnt;
            if (UNLIKELY(pos > slen)) return korb_raise(c, slots + 3, KORB_E_ARGUMENT, 0, "@ outside of string");
            si = pos;
        } else if (d == 'C' || d == 'c') {                /* unsigned / signed byte */
            const long reps = star ? (long)(slen - si) : cnt;
            for (long r = 0; r < reps; r++) {
                if (si >= VAL2STR(slots[1])->len) { CHECK(korb_ary_push_val(c, slots + 3, res, KORB_NIL)); continue; }
                int b = (unsigned char)korb_strbuf_data(VAL2STR(slots[1])->buf)[si++];
                CHECK(korb_ary_push_val(c, slots + 3, res, LONG2FIX(d == 'c' ? (int8_t)b : b)));
            }
        } else if (d == 'J' || d == 'j' || d == 'Q' || d == 'q') {   /* 8-byte int (J/Q unsigned, j/q signed); < > override */
            const bool sgn = (d == 'j' || d == 'q'), big = force_big;
            const long reps = star ? (long)((slen - si) / 8) : cnt;
            for (long r = 0; r < reps; r++) {
                const KorbString *s0 = VAL2STR(slots[1]);
                if (si + 8 > s0->len) { CHECK(korb_ary_push_val(c, slots + 3, res, KORB_NIL)); si = s0->len; continue; }
                uint64_t v = 0;
                for (int k = 0; k < 8; k++) { const KorbString *s = VAL2STR(slots[1]); v |= (uint64_t)(unsigned char)korb_strbuf_data(s->buf)[si + k] << (8 * (big ? (7 - k) : k)); }
                si += 8;
                if (!sgn && (v >> 63)) {                  /* unsigned 64 > INT64_MAX → Bignum */
                    korb_mp_t z; korb_mp_init_set_ui(z, (unsigned long)v); RESULT br = korb_big_from_mpz(c, slots + 4, z); korb_mp_clear(z);
                    if (UNLIKELY(br.state != KORB_NORMAL)) return br;
                    CHECK(korb_ary_push_val(c, slots + 3, res, br.value));
                } else
                { slots[4] = UNWRAP(korb_intptr_to_val(c, slots + 4, (intptr_t)(int64_t)v)); CHECK(korb_ary_push_val(c, slots + 3, res, slots[4])); }
            }
        } else if (d == 'U') {                            /* UTF-8 codepoints */
            for (long r = 0; (star || r < cnt); r++) {
                const KorbString *s = VAL2STR(slots[1]);
                if (si >= s->len) break;                  /* nothing left: CRuby stops, no nil padding */
                const unsigned char b0 = (unsigned char)korb_strbuf_data(s->buf)[si];
                uint32_t cp; int len;
                if (b0 < 0x80)            { cp = b0;        len = 1; }
                else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; len = 2; }
                else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; len = 3; }
                else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; len = 4; }
                else                      { cp = b0;        len = 1; }   /* invalid lead → one byte */
                if (UNLIKELY(si + (uint32_t)len > s->len))                /* truncated sequence */
                    return korb_raise(c, slots + 3, KORB_E_ARGUMENT, 0,
                                      "malformed UTF-8 character (expected %d bytes, given %u bytes)",
                                      len, s->len - si);
                for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)korb_strbuf_data(s->buf)[si + k] & 0x3F);
                si += (uint32_t)len;
                CHECK(korb_ary_push_val(c, slots + 3, res, LONG2FIX((intptr_t)cp)));
            }
        } else if (d == 'u') {                            /* uuencode decode (one value, consumes rest) */
            const KorbString *s = VAL2STR(slots[1]);
            unsigned char *const out = malloc((size_t)(s->len - si) + 4);
            uint32_t olen = 0, k = si;
            while (k < s->len) {
                /* each line: a length byte (chars-32), then 4-char groups of 6 bits */
                int nbytes = ((unsigned char)korb_strbuf_data(s->buf)[k++] - ' ') & 0x3F;
                while (nbytes > 0 && k < s->len) {
                    int q[4] = {0, 0, 0, 0};
                    int got = 0;
                    while (got < 4 && k < s->len) {
                        const int ch = (unsigned char)korb_strbuf_data(s->buf)[k];
                        if (ch == '\n' || ch == '\r') break;
                        q[got++] = (ch - ' ') & 0x3F;
                        k++;
                    }
                    const unsigned char trio[3] = {
                        (unsigned char)((q[0] << 2) | (q[1] >> 4)),
                        (unsigned char)((q[1] << 4) | (q[2] >> 2)),
                        (unsigned char)((q[2] << 6) | q[3]),
                    };
                    for (int j = 0; j < 3 && nbytes > 0; j++, nbytes--) out[olen++] = trio[j];
                    if (got < 4) break;                   /* line ended early */
                }
                while (k < s->len && korb_strbuf_data(s->buf)[k] != '\n') k++;   /* skip the line's tail */
                if (k < s->len) k++;                                             /* and the newline */
            }
            KorbString *r = korb_str_alloc(c, slots + 3, olen);   /* may move slots; out is libc-stable */
            memcpy(korb_strbuf_data(r->buf), out, olen); free(out);
            slots[3] = (VALUE)r;
            KORB_STR_ENC_SET(slots[3], KORB_ENC_BINARY);
            CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
            si = VAL2STR(slots[1])->len;
        } else if (d == 'P' || d == 'p') {                /* pointer: recover bytes via side table */
            uint64_t idx = 0;
            for (int k = 0; k < 8; k++) { const KorbString *s = VAL2STR(slots[1]); if (si < s->len) idx |= (uint64_t)(unsigned char)korb_strbuf_data(s->buf)[si] << (8 * k); si++; }
            uint32_t plen = 0;
            const char *const pd = korb_pack_ptr_lookup(c, idx, &plen);   /* malloc'd, stable across GC */
            if (!pd) { CHECK(korb_ary_push_val(c, slots + 3, res, KORB_NIL)); }
            else {
                /* 'P' takes cnt bytes (the count is a length; '*' → all); 'p' is
                 * a NUL-terminated pointer → the whole string. */
                const uint32_t take = (d == 'p' || star) ? plen : ((uint32_t)cnt < plen ? (uint32_t)cnt : plen);
                KorbString *r = korb_str_alloc(c, slots + 3, take);       /* may move slots */
                memcpy(korb_strbuf_data(r->buf), pd, take);
                slots[3] = (VALUE)r;
                CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
            }
        } else if (d == 'N' || d == 'n' || d == 'V' || d == 'v') {   /* endian unsigned int (N/n=big, V/v=little) */
            const int sz = (d == 'N' || d == 'V') ? 4 : 2;
            const bool big = (d == 'N' || d == 'n');
            const long reps = star ? (long)((slen - si) / sz) : cnt;
            for (long r = 0; r < reps; r++) {
                const KorbString *s = VAL2STR(slots[1]);
                if (si + (uint32_t)sz > s->len) { CHECK(korb_ary_push_val(c, slots + 3, res, KORB_NIL)); si = s->len; continue; }
                uint32_t v = 0;
                for (int k = 0; k < sz; k++) v |= (uint32_t)(unsigned char)korb_strbuf_data(s->buf)[si + k] << (8 * (big ? (sz - 1 - k) : k));
                si += (uint32_t)sz;
                CHECK(korb_ary_push_val(c, slots + 3, res, LONG2FIX((intptr_t)v)));
            }
        } else if (d == 'S' || d == 's' || d == 'L' || d == 'l' || d == 'I' || d == 'i') {   /* native-endian int (S/L/I unsigned, s/l/i signed); < > override, ! _ = native long size */
            const int sz = (d == 'S' || d == 's') ? 2 : ((bang && (d == 'L' || d == 'l')) ? 8 : 4);
            const bool sgn = (d == 's' || d == 'l' || d == 'i');
            const bool big = force_big;                  /* default little (native x86); `>` = big */
            (void)force_little;
            const long reps = star ? (long)((slen - si) / sz) : cnt;
            for (long r = 0; r < reps; r++) {
                const KorbString *s = VAL2STR(slots[1]);
                if (si + (uint32_t)sz > s->len) { CHECK(korb_ary_push_val(c, slots + 3, res, KORB_NIL)); si = s->len; continue; }
                uint64_t v = 0;
                for (int k = 0; k < sz; k++) v |= (uint64_t)(unsigned char)korb_strbuf_data(s->buf)[si + k] << (8 * (big ? (sz - 1 - k) : k));
                si += (uint32_t)sz;
                if (sz < 8) {                            /* 2/4 bytes always fit a Fixnum */
                    const intptr_t iv = sgn ? (sz == 2 ? (intptr_t)(int16_t)v : (intptr_t)(int32_t)v) : (intptr_t)v;
                    CHECK(korb_ary_push_val(c, slots + 3, res, LONG2FIX(iv)));
                }
                else if (!sgn && (v >> 63)) {            /* unsigned 64 > INT64_MAX → Bignum */
                    korb_mp_t z; korb_mp_init_set_ui(z, (unsigned long)v); RESULT br = korb_big_from_mpz(c, slots + 4, z); korb_mp_clear(z);
                    if (UNLIKELY(br.state != KORB_NORMAL)) return br;
                    CHECK(korb_ary_push_val(c, slots + 3, res, br.value));
                }
                else {                                   /* signed 64, or unsigned that fits: promote past Fixnum if needed */
                    slots[4] = UNWRAP(korb_intptr_to_val(c, slots + 4, (intptr_t)(int64_t)v));
                    CHECK(korb_ary_push_val(c, slots + 3, res, slots[4]));
                }
            }
        } else if (d == 'e' || d == 'E' || d == 'g' || d == 'G' || d == 'f' || d == 'F' || d == 'd' || d == 'D') {   /* IEEE float (e/E=little, g/G=big, f/F/d/D=native; e/g/f/F=32, rest=64) */
            const int sz = (d == 'e' || d == 'g' || d == 'f' || d == 'F') ? 4 : 8;
            const bool big = (d == 'g' || d == 'G');
            const long reps = star ? (long)((slen - si) / sz) : cnt;
            for (long r = 0; r < reps; r++) {
                const KorbString *s = VAL2STR(slots[1]);
                if (si + (uint32_t)sz > s->len) { CHECK(korb_ary_push_val(c, slots + 3, res, KORB_NIL)); si = s->len; continue; }
                unsigned char tmp[8];
                for (int k = 0; k < sz; k++) tmp[k] = (unsigned char)korb_strbuf_data(s->buf)[si + (big ? (sz - 1 - k) : k)];   /* → native (LE) order */
                si += (uint32_t)sz;
                double dv; if (sz == 4) { float f; memcpy(&f, tmp, 4); dv = (double)f; } else memcpy(&dv, tmp, 8);
                slots[3] = UNWRAP(korb_float_new(c, slots + 3, dv));
                CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
            }
        } else if (d == 'w') {                            /* BER compressed integer */
            for (long r = 0; (star || r < cnt); r++) {
                const KorbString *s = VAL2STR(slots[1]);
                if (si >= s->len) break;
                uint64_t v = 0;
                while (si < s->len) { const unsigned char b = (unsigned char)korb_strbuf_data(s->buf)[si++]; v = (v << 7) | (uint64_t)(b & 0x7f); if (!(b & 0x80)) break; }
                CHECK(korb_ary_push_val(c, slots + 3, res, LONG2FIX((intptr_t)v)));
            }
        } else if (d == 'b' || d == 'B') {                /* bit string (b=LSB-first, B=MSB-first) */
            const uint32_t avail = (VAL2STR(slots[1])->len - si) * 8;
            const uint32_t nbits = star ? avail : ((uint32_t)cnt < avail ? (uint32_t)cnt : avail);
            KorbString *r = korb_str_alloc(c, slots + 3, nbits);   /* may move slots */
            const KorbString *s = VAL2STR(slots[1]);              /* re-read */
            for (uint32_t k = 0; k < nbits; k++) {
                const unsigned char byte = (unsigned char)korb_strbuf_data(s->buf)[si + k / 8];
                const int bit = (d == 'B') ? ((byte >> (7 - (k % 8))) & 1) : ((byte >> (k % 8)) & 1);
                korb_strbuf_data(r->buf)[k] = (char)('0' + bit);
            }
            r->len = nbits; korb_strbuf_data(r->buf)[nbits] = '\0';
            slots[3] = (VALUE)r;
            KORB_STR_ENC_SET(slots[3], KORB_ENC_USASCII);         /* bit strings are US-ASCII */
            CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
            si += (nbits + 7) / 8;
        } else if (d == 'h' || d == 'H') {                /* hex string (h=low-nibble-first, H=high-first) */
            const uint32_t avail = (VAL2STR(slots[1])->len - si) * 2;
            const uint32_t nnib = star ? avail : ((uint32_t)cnt < avail ? (uint32_t)cnt : avail);
            KorbString *r = korb_str_alloc(c, slots + 3, nnib);   /* may move slots */
            const KorbString *s = VAL2STR(slots[1]);              /* re-read */
            for (uint32_t k = 0; k < nnib; k++) {
                const unsigned char byte = (unsigned char)korb_strbuf_data(s->buf)[si + k / 2];
                const int nib = (d == 'H') ? ((k % 2 == 0) ? (byte >> 4) : (byte & 0xF))
                                           : ((k % 2 == 0) ? (byte & 0xF) : (byte >> 4));
                korb_strbuf_data(r->buf)[k] = "0123456789abcdef"[nib];
            }
            r->len = nnib; korb_strbuf_data(r->buf)[nnib] = '\0';
            slots[3] = (VALUE)r;
            KORB_STR_ENC_SET(slots[3], KORB_ENC_USASCII);         /* hex strings are US-ASCII */
            CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
            si += (nnib + 1) / 2;
        } else if (d == 'm') {                            /* base64 decode (one value, consumes rest) */
            const KorbString *s = VAL2STR(slots[1]);
            unsigned char *const out = malloc((size_t)(s->len - si) * 3 / 4 + 4);
            uint32_t olen = 0; int quad[4], qn = 0;
            for (uint32_t k = si; k < s->len; k++) {
                const int ch = (unsigned char)korb_strbuf_data(s->buf)[k];
                int v;
                if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
                else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
                else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
                else if (ch == '+') v = 62;
                else if (ch == '/') v = 63;
                else if (ch == '=') break;                /* padding → done */
                else continue;                            /* skip newlines / non-base64 */
                quad[qn++] = v;
                if (qn == 4) { out[olen++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4)); out[olen++] = (unsigned char)((quad[1] << 4) | (quad[2] >> 2)); out[olen++] = (unsigned char)((quad[2] << 6) | quad[3]); qn = 0; }
            }
            if (qn >= 2) { out[olen++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4)); if (qn >= 3) out[olen++] = (unsigned char)((quad[1] << 4) | (quad[2] >> 2)); }
            KorbString *r = korb_str_alloc(c, slots + 3, olen);   /* may move slots; out is libc-stable */
            memcpy(korb_strbuf_data(r->buf), out, olen); free(out);
            slots[3] = (VALUE)r;
            KORB_STR_ENC_SET(slots[3], KORB_ENC_BINARY);          /* decoded payload is ASCII-8BIT */
            CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
            si = VAL2STR(slots[1])->len;
        } else if (d == 'M') {                            /* quoted-printable decode (count/`*` ignored) */
            const KorbString *s = VAL2STR(slots[1]);
            unsigned char *const out = malloc((size_t)(s->len - si) + 4);
            uint32_t olen = 0, k = si;
            while (k < s->len) {
                const int ch = (unsigned char)korb_strbuf_data(s->buf)[k];
                if (ch == '=' && k + 1 < s->len) {
                    const int c1 = (unsigned char)korb_strbuf_data(s->buf)[k + 1];
                    if (c1 == '\n') { k += 2; continue; }                                  /* soft line break */
                    if (c1 == '\r' && k + 2 < s->len && korb_strbuf_data(s->buf)[k + 2] == '\n') { k += 3; continue; }
                    #define KORB_HEXV(x) ((x) >= '0' && (x) <= '9' ? (x) - '0' : (x) >= 'A' && (x) <= 'F' ? (x) - 'A' + 10 : (x) >= 'a' && (x) <= 'f' ? (x) - 'a' + 10 : -1)
                    if (k + 2 < s->len) {
                        const int hi = KORB_HEXV(c1), lo = KORB_HEXV((unsigned char)korb_strbuf_data(s->buf)[k + 2]);
                        if (hi >= 0 && lo >= 0) { out[olen++] = (unsigned char)(hi * 16 + lo); k += 3; continue; }
                    }
                    #undef KORB_HEXV
                    out[olen++] = '='; k++;                                                /* lone '=' kept */
                } else { out[olen++] = (unsigned char)ch; k++; }
            }
            KorbString *r = korb_str_alloc(c, slots + 3, olen);   /* may move slots; out is libc-stable */
            memcpy(korb_strbuf_data(r->buf), out, olen); free(out);
            slots[3] = (VALUE)r;
            KORB_STR_ENC_SET(slots[3], KORB_ENC_BINARY);          /* decoded payload is ASCII-8BIT */
            CHECK(korb_ary_push_val(c, slots + 4, res, slots[3]));
            si = VAL2STR(slots[1])->len;
        } else {                                          /* unknown directive → ArgumentError (CRuby) */
            const KorbString *tt = VAL2STR(slots[0]);
            return korb_raise(c, slots + 3, KORB_E_ARGUMENT, 0, "unknown unpack directive '%c' in '%.*s'", d, (int)tt->len, korb_strbuf_data(tt->buf));
        }
    }
    return RESULT_OK(VALUE_REF_GET(res));
}
/* String#unpack1(template) — the first value of unpack (or nil). */
static RESULT korb_m_str_unpack1(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT r = korb_m_str_unpack(c, slots, self, a);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    const KorbArray *arr = VAL2ARY(r.value);
    return RESULT_OK(arr->len > 0 ? korb_items_data(arr->items)[0] : KORB_NIL);
}

static RESULT korb_m_ary_take(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (UNLIKELY(KORB_BIGNUM_P(nv))) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into `long'");
    if (UNLIKELY(!korb_to_index(nv, &n))) {              /* coerce count via #to_int (like Array#drop) */
        RESULT cr = korb_coerce_to_int(c, slots, &nv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(nv, &n)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
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
        uint32_t idx; CHECK(korb_rand_upto(c, slots, a, len - 1, &idx));
        return RESULT_OK(korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[idx]);   /* re-read self after any #rand GC */
    }
    intptr_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) {   /* count coerces via #to_int */
        VALUE cv = VALUE_SLICE_GET(a, 0);
        RESULT ci = korb_coerce_to_int(c, slots, &cv);
        if (UNLIKELY(ci.state != KORB_NORMAL)) return ci;
        if (ci.value != KORB_TRUE || !korb_to_index(cv, &n))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative sample number");
    if ((uint32_t)n > (intptr_t)len) n = len;
    if (n == 0) return korb_ary_new(c, slots, 0);

    if (n <= 10) {
        /* CRuby ary_sample n<=10: draw all randoms FIRST (alloc would move the
         * rng buffer), then build distinct indices, then materialize. */
        long rnds[10], idx[10], sorted[10];
        for (intptr_t i = 0; i < n; i++) { uint32_t rv; CHECK(korb_rand_upto(c, slots, a, (uint32_t)(len - i - 1), &rv)); rnds[i] = (long)rv; }
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
        for (intptr_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(src->items)[idx[i]]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    /* n>10: copy then partial Fisher-Yates (random, not CRuby-bit-exact). */
    const RESULT cp = korb_ary_subseq(c, slots, self, 0, len);
    if (UNLIKELY(cp.state != KORB_NORMAL)) return cp;
    slots[0] = cp.value;
    for (intptr_t i = 0; i < n; i++) {
        uint32_t rj; CHECK(korb_rand_upto(c, slots + 1, a, (uint32_t)(len - i - 1), &rj));   /* slots+1: keep the copy at slots[0] */
        const uint32_t j = i + rj;
        KorbArrayItems *const it = VAL2ARY(slots[0])->items;   /* re-read after any #rand GC */
        const VALUE t = korb_items_data(it)[i]; korb_items_data(it)[i] = korb_items_data(it)[j]; korb_items_data(it)[j] = t;
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
    for (uint32_t i = len; i > 1; i--) {                          /* j in [0, i-1]; swap (i-1, j) */
        uint32_t j; CHECK(korb_rand_upto(c, slots + 1, a, i - 1, &j));   /* slots+1: keep the copy at slots[0] */
        KorbArrayItems *const it = VAL2ARY(slots[0])->items;      /* re-read after any #rand GC */
        const VALUE t = korb_items_data(it)[i - 1]; korb_items_data(it)[i - 1] = korb_items_data(it)[j]; korb_items_data(it)[j] = t;
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_ary_shuffle_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    const uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = len; i > 1; i--) {                          /* in-place Fisher-Yates */
        uint32_t j; CHECK(korb_rand_upto(c, slots, a, i - 1, &j));
        KorbArrayItems *const it = VAL2ARY(VALUE_REF_GET(self))->items;   /* re-read after any #rand GC */
        const VALUE t = korb_items_data(it)[i - 1]; korb_items_data(it)[i - 1] = korb_items_data(it)[j]; korb_items_data(it)[j] = t;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (UNLIKELY(KORB_BIGNUM_P(nv))) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into `long'");
    if (UNLIKELY(!korb_to_index(nv, &n))) {              /* coerce count via #to_int */
        RESULT cr = korb_coerce_to_int(c, slots, &nv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(nv, &n)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to drop negative size");
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if ((uint32_t)n > len) n = len;
    return korb_ary_subseq(c, slots, self, (uint32_t)n, len - (uint32_t)n);
}
static RESULT korb_m_ary_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    /* Two passes: collect the survivors (dispatching a user element's #== is
     * GC-unsafe against in-place compaction, so build a fresh `kept` array first,
     * then copy it back with no intervening dispatch). */
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* needle (rooted) */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 3, VAL2ARY(VALUE_REF_GET(self))->len));   /* survivors (rooted) */
    slots[2] = KORB_NIL;                                 /* last deleted element (rooted) */
    bool found = false;
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(self))->len; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        bool eq;
        if (KORB_OBJECT_P(e) || KORB_OBJECT_P(slots[0])) {   /* user #== → dispatch element == needle */
            slots[3] = e; slots[4] = slots[0];
            RESULT r = korb_send_impl(c, slots + 5, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            eq = KORB_TRUTHY(r.value);
            e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* re-read after the (GC-capable) dispatch */
        } else {
            eq = korb_value_eq(e, slots[0]);
        }
        if (eq) { slots[2] = e; found = true; }
        else CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[1]), e));
    }
    if (!found) {
        if (block != NULL) return korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        return RESULT_OK(KORB_NIL);
    }
    KORB_CHECK_FROZEN(c, slots + 3, VALUE_REF_GET(self));    /* it would modify → FrozenError if frozen */
    KorbArray *const ary = VAL2ARY(VALUE_REF_GET(self));
    const KorbArray *const kept = VAL2ARY(slots[1]);
    KorbArrayItems *const it = ary->items;                   /* copy survivors back in place (no dispatch → GC-safe) */
    for (uint32_t i = 0; i < kept->len; i++)      ARO_STORE(c, it, &korb_items_data(it)[i], korb_items_data(kept->items)[i]);
    for (uint32_t i = kept->len; i < ary->len; i++) ARO_STORE(c, it, &korb_items_data(it)[i], KORB_NIL);
    ary->len = kept->len;
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_ary_delete_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t i;
    if (!korb_to_index(iv, &i)) {                        /* coerce index via #to_int */
        RESULT cr = korb_coerce_to_int(c, slots, &iv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(iv, &i)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));       /* re-read after possible dispatch */
    if (i < 0) i += ary->len;
    if (i < 0 || (uint32_t)i >= ary->len) return RESULT_OK(KORB_NIL);
    KorbArrayItems *it = ary->items;
    VALUE removed = korb_items_data(it)[i];
    for (uint32_t r = (uint32_t)i; r + 1 < ary->len; r++) ARO_STORE(c, it, &korb_items_data(it)[r], korb_items_data(it)[r + 1]);
    ary->len--; ARO_STORE(c, it, &korb_items_data(it)[ary->len], KORB_NIL);
    return RESULT_OK(removed);
}
static RESULT korb_m_ary_rindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block != NULL && VALUE_SLICE_LEN(a) > 0) korb_warn(c, slots, "given block not used");   /* arg wins */
    if (block != NULL && VALUE_SLICE_LEN(a) == 0) {   /* block form (arg, if given, wins per CRuby) */
        for (int32_t i = (int32_t)VAL2ARY(VALUE_REF_GET(self))->len - 1; i >= 0; i--) {
            if (i >= (int32_t)VAL2ARY(VALUE_REF_GET(self))->len) continue;   /* re-check size: the block may have shrunk self */
            slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_NIL && r.value != KORB_FALSE) return RESULT_OK(LONG2FIX(i));
        }
        return RESULT_OK(KORB_NIL);
    }
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* needle (root across element == dispatch) */
    for (int32_t i = (int32_t)VAL2ARY(VALUE_REF_GET(self))->len - 1; i >= 0; i--) {
        const VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        if (KORB_OBJECT_P(e) || KORB_OBJECT_P(slots[0])) {  /* user == → dispatch (element == needle) */
            slots[1] = e; slots[2] = slots[0];
            RESULT r = korb_send_impl(c, slots + 3, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
        } else if (korb_value_eq(e, slots[0])) {
            return RESULT_OK(LONG2FIX(i));
        }
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_rotate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t sh = 1;
    if (VALUE_SLICE_LEN(a) >= 1) {
        VALUE cv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(cv, &sh))) {         /* coerce via #to_int */
            const VALUE orig = cv;
            RESULT cr = korb_coerce_to_int(c, slots, &cv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(cv, &sh)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(orig));
        }
    }
    const uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;   /* read after any coercion dispatch */
    if (len == 0) return korb_ary_subseq(c, slots, self, 0, 0);
    intptr_t s = ((sh % (intptr_t)len) + (intptr_t)len) % (intptr_t)len;   /* normalized left rotation */
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, len)));
    for (uint32_t i = 0; i < len; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[(s + i) % len];
        CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* zip rows: [ self[i], other0[i], other1[i], ... ]. With a block, yield each row
 * and return nil; otherwise collect rows into an array. dst lives at slots[1]
 * (block path leaves it nil/unused), rows built at slots[2]. */
static RESULT korb_m_ary_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    const uint32_t k = VALUE_SLICE_LEN(a);
    if (VAL2ARY(VALUE_REF_GET(self))->len == 0)             /* empty self → no rows, and the args are never validated (CRuby) */
        return block != NULL ? RESULT_OK(KORB_NIL) : korb_ary_new(c, slots, 0);
    /* coerce each arg: Array/Range/ArithSeq stay as-is (korb_zip_elem indexes them
     * lazily, so infinite sequences don't materialize); a user object with #to_ary
     * (or #to_a) is converted up-front. */
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, k));
    VALUE_REF cargs = VALUE_REF_AT(&slots[0]);
    const uint32_t to_ary_id = korb_intern(c->vm, "to_ary", 6), to_a_id = korb_intern(c->vm, "to_a", 4);
    const uint32_t each_id = korb_intern(c->vm, "each", 4), to_enum_id = korb_intern(c->vm, "to_enum", 7);
    for (uint32_t j = 0; j < k; j++) {
        slots[1] = VALUE_SLICE_GET(a, j);                     /* candidate (rooted) */
        /* Array/Range/ArithSeq index lazily via korb_zip_elem; anything else is
         * materialized here (#to_ary, else #each via #to_enum → #to_a). */
        if (!KORB_ARRAY_P(slots[1]) && !KORB_RANGE_P(slots[1]) && !KORB_ARITHSEQ_P(slots[1])) {
            bool done = false;
            if (korb_responds_to_coerce(c, slots + 2, slots[1], to_ary_id)) {   /* #to_ary conversion (honors respond_to?) */
                RESULT r = korb_send_impl(c, slots + 2, to_ary_id, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                if (KORB_ARRAY_P(r.value)) { slots[1] = r.value; done = true; }
            }
            if (!done && korb_responds_to_coerce(c, slots + 2, slots[1], each_id)) {   /* responds to #each → arg.to_enum(:each).to_a */
                slots[2] = ID2SYM(each_id);                   /* the :each argument to #to_enum */
                RESULT er = korb_send_impl(c, slots + 3, to_enum_id, 0, 1, NULL, NULL, NULL);   /* recv=slots[1], arg=slots[2] */
                if (UNLIKELY(er.state != KORB_NORMAL)) return er;
                slots[2] = er.value;                          /* the enumerator (rooted) */
                RESULT r = korb_send_impl(c, slots + 3, to_a_id, 0, 0, NULL, NULL, NULL);   /* recv at slots[2] */
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                if (KORB_ARRAY_P(r.value)) { slots[1] = r.value; done = true; }
            }
            if (UNLIKELY(!done))                              /* neither Array-like nor iterable */
                return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "wrong argument type %s (must respond to :each)", korb_type_name(VALUE_SLICE_GET(a, j)));
        }
        CHECK(korb_ary_push_val(c, slots + 2, cargs, slots[1]));
    }
    const uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    slots[1] = (block == NULL) ? UNWRAP(korb_ary_new(c, slots + 2, n)) : KORB_NIL;   /* dst */
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) {
        slots[2] = UNWRAP(korb_ary_new(c, slots + 3, k + 1));              /* row at slots[2] */
        VALUE_REF row = VALUE_REF_AT(&slots[2]);
        CHECK(korb_ary_push_val(c, slots + 3, row, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i]));
        for (uint32_t j = 0; j < k; j++)
            CHECK(korb_ary_push_val(c, slots + 3, row, korb_zip_elem(korb_items_data(VAL2ARY(VALUE_REF_GET(cargs))->items)[j], i)));
        if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        } else {
            CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        }
    }
    return RESULT_OK(block != NULL ? KORB_NIL : VALUE_REF_GET(dst));
}

/* `|` union (in self then other, deduped) / `&` intersection (in both, self order, deduped) */
static RESULT korb_m_ary_union(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {  /* coerce each operand via #to_ary before building */
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_ARRAY_P(ov))) {
            RESULT cr = korb_coerce_to_ary(c, slots, &ov);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value != KORB_TRUE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, k)));
            VALUE_REF_SET(VALUE_SLICE_REF(a, k), ov);
        }
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(self))->len; i++) {
        slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* e, rooted across #eql? dispatch + push */
        bool has; CHECK(korb_arr_member_eql(c, slots + 2, dst, slots[1], &has));
        if (!has) CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
    }
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {  /* union(*others) */
        for (uint32_t i = 0; i < VAL2ARY(VALUE_SLICE_GET(a, k))->len; i++) {
            slots[1] = korb_items_data(VAL2ARY(VALUE_SLICE_GET(a, k))->items)[i];
            bool has; CHECK(korb_arr_member_eql(c, slots + 2, dst, slots[1], &has));
            if (!has) CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_intersect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {  /* coerce each operand via #to_ary */
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_ARRAY_P(ov))) {
            RESULT cr = korb_coerce_to_ary(c, slots, &ov);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value != KORB_TRUE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, k)));
            VALUE_REF_SET(VALUE_SLICE_REF(a, k), ov);
        }
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    bool no_args = VALUE_SLICE_LEN(a) == 0;             /* intersection() → plain copy of self, no dedup */
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(self))->len; i++) {
        slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* e, rooted across #eql? dispatch + push */
        if (no_args) { CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1])); continue; }
        bool in_all = true;                              /* element must be in every other array */
        for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
            bool has; CHECK(korb_arr_member_eql(c, slots + 2, VALUE_SLICE_REF(a, k), slots[1], &has));
            if (!has) { in_all = false; break; }
        }
        if (!in_all) continue;
        bool dup; CHECK(korb_arr_member_eql(c, slots + 2, dst, slots[1], &dup));
        if (!dup) CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

