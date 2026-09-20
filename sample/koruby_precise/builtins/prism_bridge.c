/* prism の serialize API への橋渡し。
 *
 * CRuby の prism は C 拡張が Prism.dump/lex/parse_lex/... を提供し、Ruby 側
 * (prism/serialize.rb) がそのバイト列を木に戻す。koruby はその C 拡張の代わりに
 * この builtin 1 本を出し、lib/prism/prism.rb が Prism の API を組み立てる。
 * これで prism の Ruby ライブラリ一式 —— Prism::Translation::Ripper を含む ——
 * が無改造で載る (= Ripper が使えるようになる)。
 *
 * __prism_dump(kind, source, options) -> String (serialize されたバイト列)
 *   kind: 0 parse / 1 lex / 2 parse_lex / 3 parse_comments / 4 success?
 * options は prism の options 形式のバイト列 (Ruby 側が組み立てる) か nil。 */
static RESULT
korb_bi_prism_dump(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    const VALUE kv = VALUE_SLICE_GET(args, 0);
    const VALUE sv = VALUE_SLICE_GET(args, 1);
    const VALUE ov = VALUE_SLICE_LEN(args) > 2 ? VALUE_SLICE_GET(args, 2) : KORB_NIL;
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
    memcpy(src, korb_strbuf_data(ss->buf), slen);
    src[slen] = '\0';

    char *opts = NULL;
    if (ov != KORB_NIL) {
        const KorbString *const os = VAL2STR(ov);
        opts = malloc(os->len + 1);
        if (!opts) { fprintf(stderr, "koruby_precise: oom (prism options)\n"); abort(); }
        memcpy(opts, korb_strbuf_data(os->buf), os->len);
        opts[os->len] = '\0';
    }

    const korb_sword_t kind = FIX2LONG(kv);
    if (kind == 5) {                              /* pm_version(): Ruby 側の VERSION */
        free(src); free(opts);
        const char *const v = pm_version();
        return korb_str_new(c, slots, v, (uint32_t)strlen(v));
    }
    if (kind == 4) {
        const bool ok = pm_parse_success_p(src, slen, opts);
        free(src); free(opts);
        return RESULT_OK(ok ? KORB_TRUE : KORB_FALSE);
    }

    pm_buffer_t buffer;
    if (!pm_buffer_init(&buffer)) { fprintf(stderr, "koruby_precise: oom (prism buffer)\n"); abort(); }
    switch (kind) {
      case 1:  pm_serialize_lex(&buffer, src, slen, opts); break;
      case 2:  pm_serialize_parse_lex(&buffer, src, slen, opts); break;
      case 3:  pm_serialize_parse_comments(&buffer, src, slen, opts); break;
      default: pm_serialize_parse(&buffer, src, slen, opts); break;
    }
    const RESULT r = korb_str_new(c, slots, pm_buffer_value(&buffer), (uint32_t)pm_buffer_length(&buffer));
    pm_buffer_free(&buffer);
    free(src); free(opts);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
    return r;
}
