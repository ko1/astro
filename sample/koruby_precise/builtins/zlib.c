/* zlib primitives — compiled only when zlib.h was found (see ZLIB_OK in the
 * Makefile); lib/zlib.rb builds the Ruby API on top and raises LoadError when
 * these are absent.  #included into korb_runtime.c's TU. */
#ifdef KORB_HAVE_ZLIB
#include <zlib.h>

/* Zlib.crc32(str = "", crc = 0) / Zlib.adler32 — the running checksum. */
static RESULT korb_m_zlib_sum(CTX *c, VALUE *slots, VALUE_SLICE a, bool adler) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    uLong sum = adler ? adler32(0L, Z_NULL, 0) : crc32(0L, Z_NULL, 0);
    if (n >= 2) {
        const VALUE sv = VALUE_SLICE_GET(a, 1);
        if (UNLIKELY(!FIXNUM_P(sv) && !KORB_BIGNUM_P(sv)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(sv));
        sum = (uLong)(uint32_t)(FIXNUM_P(sv) ? (uint64_t)FIX2LONG(sv) : 0);
    }
    if (n >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        const VALUE s = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!KORB_STRING_P(s)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(s));
        const KorbString *const ks = VAL2STR(s);
        sum = adler ? adler32(sum, (const Bytef *)korb_strbuf_data(ks->buf), ks->len)
                    : crc32(sum, (const Bytef *)korb_strbuf_data(ks->buf), ks->len);
    }
    return RESULT_OK(LONG2FIX((korb_sword_t)(uint32_t)sum));
}
static RESULT korb_m_zlib_crc32(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)self; return korb_m_zlib_sum(c, slots, a, false); }
static RESULT korb_m_zlib_adler32(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; return korb_m_zlib_sum(c, slots, a, true); }

static RESULT korb_m_zlib_crc_table(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    const z_crc_t *const t = get_crc_table();
    slots[0] = UNWRAP(korb_ary_new(c, slots, 256));
    const VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < 256; i++)
        CHECK(korb_ary_push_val(c, slots + 1, arr, LONG2FIX((korb_sword_t)(uint32_t)t[i])));
    return RESULT_OK(VALUE_REF_GET(arr));
}

/* __zlib_deflate(str, level, window_bits) → the compressed String.
 * window_bits picks the wrapper: 15 = zlib, -15 = raw, 31 = gzip. */
static RESULT korb_m_zlib_deflate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    const int level = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
    const int wbits = (int)FIX2LONG(VALUE_SLICE_GET(a, 2));
    const KorbString *const ks = VAL2STR(sv);
    z_stream zs; memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "deflateInit2 failed");
    const uLong cap = deflateBound(&zs, ks->len) + 64;
    char *const out = malloc(cap ? cap : 1);
    if (!out) { deflateEnd(&zs); abort(); }
    zs.next_in = (Bytef *)korb_strbuf_data(ks->buf);
    zs.avail_in = ks->len;
    zs.next_out = (Bytef *)out;
    zs.avail_out = (uInt)cap;
    const int rc = deflate(&zs, Z_FINISH);
    const uLong wrote = cap - zs.avail_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) { free(out); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "deflate failed"); }
    const RESULT r = korb_str_new(c, slots, out, (uint32_t)wrote);
    free(out);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
    return r;
}

/* __zlib_inflate(str, window_bits) → the decompressed String.  A truncated or
 * corrupt stream raises; the Ruby layer maps that to Zlib::DataError etc. */
static RESULT korb_m_zlib_inflate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE sv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(sv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    const int wbits = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
    const KorbString *const ks = VAL2STR(sv);
    z_stream zs; memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, wbits) != Z_OK)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "inflateInit2 failed");
    zs.next_in = (Bytef *)korb_strbuf_data(ks->buf);
    zs.avail_in = ks->len;
    size_t cap = ks->len * 4 + 256, len = 0;
    char *out = malloc(cap);
    if (!out) { inflateEnd(&zs); abort(); }
    int rc;
    for (;;) {
        zs.next_out = (Bytef *)(out + len);
        zs.avail_out = (uInt)(cap - len);
        rc = inflate(&zs, Z_NO_FLUSH);
        len = cap - zs.avail_out;
        if (rc == Z_STREAM_END || rc == Z_BUF_ERROR) break;
        if (rc != Z_OK) break;
        if (len == cap) { cap *= 2; char *const g = realloc(out, cap); if (!g) { free(out); inflateEnd(&zs); abort(); } out = g; }
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
        free(out);
        const int kind = (rc == Z_DATA_ERROR) ? 1 : (rc == Z_NEED_DICT ? 2 : 0);
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "zlib inflate error %d", kind);
    }
    const RESULT r = korb_str_new(c, slots, out, (uint32_t)len);
    free(out);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
    return r;
}

static RESULT korb_m_zlib_version(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    const char *const v = zlibVersion();
    return korb_str_new(c, slots, v, (uint32_t)strlen(v));
}

static void korb_init_zlib(CTX *c) {
    const VALUE obj = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
    korb_class_def_cfn(c, obj, "__zlib_crc32",     korb_m_zlib_crc32,     -1);
    korb_class_def_cfn(c, obj, "__zlib_adler32",   korb_m_zlib_adler32,   -1);
    korb_class_def_cfn(c, obj, "__zlib_crc_table", korb_m_zlib_crc_table,  0);
    korb_class_def_cfn(c, obj, "__zlib_deflate",   korb_m_zlib_deflate,    3);
    korb_class_def_cfn(c, obj, "__zlib_inflate",   korb_m_zlib_inflate,    2);
    korb_class_def_cfn(c, obj, "__zlib_version",   korb_m_zlib_version,    0);
}
#else
static void korb_init_zlib(CTX *c) { (void)c; }
#endif /* KORB_HAVE_ZLIB */
