/* prism の serialize API への橋渡し。
 *
 * CRuby の prism は C 拡張が Prism.dump/lex/parse_lex/... を提供し、Ruby 側
 * (prism/serialize.rb) がそのバイト列を木に戻す。koruby はその C 拡張の代わりに
 * この builtin 1 本を出し、lib/prism/prism.rb が Prism の API を組み立てる。
 * これで prism の Ruby ライブラリ一式 —— Prism::Translation::Ripper を含む ——
 * が無改造で載る (= Ripper が使えるようになる)。
 *
 * __prism_dump(kind, source, options[, format]) -> String (serialize されたバイト列)
 *   kind: 0 parse / 1 lex / 2 parse_lex / 3 parse_comments / 4 success? / 5 version
 *         6 errors_format (raise_error:) → nil (エラー無し) か "<level byte><enc>\0<message>"
 * options は prism の options 形式のバイト列 (Ruby 側が組み立てる) か nil。 */
static RESULT
korb_bi_prism_dump(CTX *c, korb_value *slots, korb_slice args)
{
    const korb_value kv = KORB_SLICE_GET(args, 0);
    const korb_value sv = KORB_SLICE_GET(args, 1);
    const korb_value ov = KORB_SLICE_LEN(args) > 2 ? KORB_SLICE_GET(args, 2) : KORB_NIL;
    if (UNLIKELY(!FIXNUM_P(kv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (UNLIKELY(!KORB_STRING_P(sv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, sv));
    if (UNLIKELY(ov != KORB_NIL && !KORB_STRING_P(ov)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, ov));

    /* prism は source も options も自分では触らないが、この先で alloc が走るので
     * バイト列は先に libc 側へ写す (moving GC で String が動く)。 */
    const KorbString *const ss = VAL2STR(sv);
    const size_t slen = ss->len;
    uint8_t *const src = malloc(slen + 1);
    if (!src) { fprintf(stderr, "koruby_precise: oom (prism source)\n"); abort(); }
    memcpy(src, korb_str_ptr(ss), slen);
    src[slen] = '\0';

    char *opts = NULL;
    if (ov != KORB_NIL) {
        const KorbString *const os = VAL2STR(ov);
        opts = malloc(os->len + 1);
        if (!opts) { fprintf(stderr, "koruby_precise: oom (prism options)\n"); abort(); }
        memcpy(opts, korb_str_ptr(os), os->len);
        opts[os->len] = '\0';
    }

    const korb_sword_t kind = FIX2LONG(kv);
    if (kind == 5) {                              /* pm_version(): Ruby 側の VERSION */
        free(src); free(opts);
        const char *const v = pm_version();
        return korb_str_new(c, slots, v, (uint32_t)strlen(v));
    }
    if (kind == 4) {
        const bool ok = pm_serialize_parse_success_p(src, slen, opts);
        free(src); free(opts);
        return RESULT_OK(ok ? KORB_TRUE : KORB_FALSE);
    }

    pm_buffer_t *const buffer = pm_buffer_new();
    if (kind == 6) {
        const korb_value fv = KORB_SLICE_LEN(args) > 3 ? KORB_SLICE_GET(args, 3) : KORB_NIL;
        const pm_errors_format_type_t fmt = FIXNUM_P(fv) ? (pm_errors_format_type_t)FIX2LONG(fv) : PM_ERRORS_FORMAT_PLAIN;
        const int8_t level = pm_serialize_parse_errors_format(buffer, src, slen, opts, fmt);
        free(src); free(opts);
        if (level < 0) { pm_buffer_free(buffer); return RESULT_OK(KORB_NIL); }
        const size_t blen = pm_buffer_length(buffer);
        char *const out = malloc(blen + 1);
        if (!out) { fprintf(stderr, "koruby_precise: oom (prism errors)\n"); abort(); }
        out[0] = (char)level;
        memcpy(out + 1, pm_buffer_value(buffer), blen);
        pm_buffer_free(buffer);
        const RESULT r = korb_str_new(c, slots, out, (uint32_t)(blen + 1));
        free(out);
        if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
        return r;
    }
    switch (kind) {
      case 1:  pm_serialize_lex(buffer, src, slen, opts); break;
      case 2:  pm_serialize_parse_lex(buffer, src, slen, opts); break;
      case 3:  pm_serialize_parse_comments(buffer, src, slen, opts); break;
      default: pm_serialize_parse(buffer, src, slen, opts); break;
    }
    const RESULT r = korb_str_new(c, slots, pm_buffer_value(buffer), (uint32_t)pm_buffer_length(buffer));
    pm_buffer_free(buffer);
    free(src); free(opts);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
    return r;
}
