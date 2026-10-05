/* Native JSON parser: the fast path of JSON::Ext::ParserConfig#parse (lib/json/ext/parser.rb).
 *
 * __json_parse(source, flags, max_nesting) parses a UTF-8 / binary String into the same
 * objects the Ruby port builds.  Anything it does not handle — a syntax error, a duplicate
 * key, too deep nesting, a symbol key that is not valid UTF-8 — answers the source itself,
 * and the caller re-parses with the Ruby port, which raises with the exact message,
 * line / column and json_path.  on_load / decimal_class never come here.
 *
 * The source bytes are copied once per call, so the scan pointer stays valid while the
 * objects being built move under the GC.  Recursion is capped at KJ_MAX_DEPTH; deeper
 * input (max_nesting: false) goes to the iterative Ruby parser. */

#include "../vendor/json/fast_float_parser.h"

enum {
    KJ_SYMBOLIZE = 1, KJ_FREEZE = 2, KJ_ALLOW_NAN = 4, KJ_TRAILING_COMMA = 8,
    KJ_COMMENTS = 16, KJ_CONTROL = 32, KJ_INVALID_ESCAPE = 64, KJ_DUP_KEY = 128,
};
#define KJ_MAX_DEPTH 1000
#define KJ_KCACHE 128

struct kj_state {
    const unsigned char *p, *end;
    korb_value *stack;              /* rooted slot holding the rvalue stack Array */
    korb_value *kcache;             /* rooted slot: KJ_KCACHE interned keys (json's key cache), nil = empty */
    uint32_t flags;
    long max_nesting;
    int depth;
    bool fail;
    char *buf;                 /* unescape scratch, grown on demand */
    size_t bufcap;
};

/* "no value": the parse gave up and the Ruby port takes over */
#define KJ_FAIL(st) ((st)->fail = true, RESULT_OK(KORB_UNDEF))

static bool kj_skip_ws(struct kj_state *const st)
{
    const unsigned char *p = st->p;
    while (p < st->end) {
        const unsigned char ch = *p;
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            p++;
            while (st->end - p >= 8) {                     /* indentation: eight spaces at a time */
                uint64_t w;
                memcpy(&w, p, 8);
                if (w != 0x2020202020202020ULL) break;
                p += 8;
            }
            continue;
        }
        if (ch != '/') break;
        if (!(st->flags & KJ_COMMENTS) || p + 1 >= st->end) { st->p = p; return false; }
        if (p[1] == '/') {
            const unsigned char *const nl = memchr(p + 2, '\n', (size_t)(st->end - p - 2));
            p = nl ? nl + 1 : st->end;
        } else if (p[1] == '*') {
            const unsigned char *q = p + 2;
            for (;;) {
                const unsigned char *const star = memchr(q, '*', (size_t)(st->end - q));
                if (!star || star + 1 >= st->end) { st->p = p; return false; }
                if (star[1] == '/') { p = star + 2; break; }
                q = star + 1;
            }
        } else { st->p = p; return false; }
    }
    st->p = p;
    return true;
}

static int kj_hex(const unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool kj_buf_reserve(struct kj_state *const st, const size_t need)
{
    if (need <= st->bufcap) return true;
    size_t cap = st->bufcap ? st->bufcap * 2 : 256;
    while (cap < need) cap *= 2;
    char *const nb = realloc(st->buf, cap);
    if (!nb) return false;
    st->buf = nb; st->bufcap = cap;
    return true;
}

/* json_string_unescape(): [s, e) holds escapes; the result goes to st->buf. */
static bool kj_unescape(struct kj_state *const st, const unsigned char *s, const unsigned char *const e, size_t *const outlen)
{
    if (!kj_buf_reserve(st, (size_t)(e - s))) return false;
    char *o = st->buf;
    while (s < e) {
        const unsigned char ch = *s;
        if (ch != '\\') { *o++ = (char)ch; s++; continue; }
        if (s + 1 >= e) return false;
        const unsigned char esc = s[1];
        s += 2;
        switch (esc) {
          case '"': case '/': case '\\': *o++ = (char)esc; break;
          case 'n': *o++ = '\n'; break;
          case 'r': *o++ = '\r'; break;
          case 't': *o++ = '\t'; break;
          case 'b': *o++ = '\b'; break;
          case 'f': *o++ = '\f'; break;
          case 'u': {
            if (e - s < 4) return false;
            const int h0 = kj_hex(s[0]), h1 = kj_hex(s[1]), h2 = kj_hex(s[2]), h3 = kj_hex(s[3]);
            if ((h0 | h1 | h2 | h3) < 0) return false;
            uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
            s += 4;
            if ((cp & 0xFC00) == 0xD800) {                 /* high surrogate: a \uDCxx must follow */
                if (e - s < 6 || s[0] != '\\' || s[1] != 'u') return false;
                const int l0 = kj_hex(s[2]), l1 = kj_hex(s[3]), l2 = kj_hex(s[4]), l3 = kj_hex(s[5]);
                if ((l0 | l1 | l2 | l3) < 0) return false;
                const uint32_t lo = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
                if ((lo & 0xFC00) != 0xDC00) return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                s += 6;
            } else if ((cp & 0xFC00) == 0xDC00) {
                return false;
            }
            if (cp <= 0x7F) { *o++ = (char)cp; }
            else if (cp <= 0x7FF) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
            else if (cp <= 0xFFFF) { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
            else { *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
                   *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
            break;
          }
          default:
            if (!(st->flags & KJ_INVALID_ESCAPE) || esc < 0x20) return false;
            *o++ = (char)esc;
            break;
        }
    }
    *outlen = (size_t)(o - st->buf);
    return true;
}

static bool kj_utf8_valid(const unsigned char *p, const size_t n)
{
    for (size_t i = 0; i < n; ) {
        if (p[i] < 0x80) { i++; continue; }
        const uint32_t cl = korb_utf8_valid_len(p + i, (uint32_t)(n - i));
        if (cl == 0) return false;
        i += cl;
    }
    return true;
}

/* st->p at the opening quote.  A key becomes an interned String or a Symbol. */
static RESULT kj_string(CTX *c, korb_value *slots, struct kj_state *const st, const bool is_key)
{
    const unsigned char *const s = st->p + 1;
    const unsigned char *q = s;
    bool escaped = false;
    for (;;) {
        while (st->end - q >= 8) {                          /* no '"', '\\' or control byte in the next eight */
            uint64_t w;
            memcpy(&w, q, 8);
            const uint64_t lo = 0x0101010101010101ULL, hi = 0x8080808080808080ULL;
            const uint64_t dq = w ^ (lo * '"'), bs = w ^ (lo * '\\');
            if ((((w - lo * 0x20) & ~w) | ((dq - lo) & ~dq) | ((bs - lo) & ~bs)) & hi) break;
            q += 8;
        }
        if (q >= st->end) return KJ_FAIL(st);
        const unsigned char ch = *q;
        if (ch == '"') break;
        if (ch == '\\') { escaped = true; q += 2; continue; }
        if (ch < 0x20 && !(st->flags & KJ_CONTROL)) return KJ_FAIL(st);
        q++;
    }
    st->p = q + 1;
    const char *bytes = (const char *)s;
    size_t len = (size_t)(q - s);
    if (escaped) {
        if (!kj_unescape(st, s, q, &len)) return KJ_FAIL(st);
        bytes = st->buf;
    }
    if (is_key && (st->flags & KJ_SYMBOLIZE)) {
        if (!kj_utf8_valid((const unsigned char *)bytes, len)) return KJ_FAIL(st);   /* to_sym raises: let Ruby */
        return RESULT_OK(ID2SYM(korb_intern_enc(c->vm, bytes, (uint32_t)len, KORB_ENC_UTF8)));
    }
    if (is_key && len <= 64) {                              /* repeated keys skip the fstring table */
        if (*st->kcache == KORB_NIL) {                      /* made at the first key: scalar documents pay nothing */
            const korb_value kc = UNWRAP(korb_ary_new(c, slots, KJ_KCACHE));
            VAL2ARY(kc)->len = KJ_KCACHE;                  /* all nil; the GC visits elements below len only */
            *st->kcache = kc;
        }
        const uint32_t h = ((uint32_t)len * 31u + (unsigned char)bytes[0] * 7u + (unsigned char)bytes[len ? len - 1 : 0]) & (KJ_KCACHE - 1);
        const korb_value hit = korb_ary_ptr(VAL2ARY(*st->kcache))[h];
        if (KORB_STRING_P(hit) && VAL2STR(hit)->len == len && memcmp(korb_str_ptr(VAL2STR(hit)), bytes, len) == 0)
            return RESULT_OK(hit);
        const korb_value k = korb_fstr_get(c, slots, bytes, (uint32_t)len, KORB_ENC_UTF8);
        const KorbArray *const kc = VAL2ARY(*st->kcache);   /* re-read: the fstring may have been allocated */
        ARO_STORE(c, korb_ary_owner(kc), &korb_ary_ptr(kc)[h], k);
        return RESULT_OK(k);
    }
    if (is_key || (st->flags & KJ_FREEZE))                  /* -str, as the Ruby port does */
        return RESULT_OK(korb_fstr_get(c, slots, bytes, (uint32_t)len, KORB_ENC_UTF8));
    return korb_str_new(c, slots, bytes, (uint32_t)len);
}

/* Digits at st->p into *m (wrapping past 19 digits is fine: the caller then takes
 * the strtod path); the count. */
static size_t kj_digits(struct kj_state *const st, const unsigned char **const pp, uint64_t *const m)
{
    const unsigned char *p = *pp;
    const unsigned char *const s = p;
    uint64_t v = *m;
    while (st->end - p >= 8) {                              /* fast_float's eight-digits-at-once (little endian) */
        uint64_t w;
        memcpy(&w, p, 8);
        if (((w + 0x4646464646464646ULL) | (w - 0x3030303030303030ULL)) & 0x8080808080808080ULL) break;
        w -= 0x3030303030303030ULL;
        w = (w * 10) + (w >> 8);
        w = (((w & 0x000000FF000000FFULL) * 0x000F424000000064ULL) +
             (((w >> 16) & 0x000000FF000000FFULL) * 0x0000271000000001ULL)) >> 32;
        v = v * 100000000ULL + (uint32_t)w;
        p += 8;
    }
    while (p < st->end && *p >= '0' && *p <= '9') { v = v * 10 + (uint64_t)(*p - '0'); p++; }
    *m = v; *pp = p;
    return (size_t)(p - s);
}

/* json's json_parse_number + json_decode_float: the same conversions CRuby's json
 * extension makes (Eisel-Lemire up to 18 significant digits), so the doubles agree. */
static RESULT kj_number(CTX *c, korb_value *slots, struct kj_state *const st)
{
    const unsigned char *const start = st->p;
    const unsigned char *p = start;
    const bool neg = *p == '-';
    if (neg) p++;
    const unsigned char *const dstart = p;
    uint64_t mantissa = 0;
    size_t mdigits = kj_digits(st, &p, &mantissa);
    if (mdigits == 0 || (mdigits > 1 && *dstart == '0')) {
        if (neg && (st->flags & KJ_ALLOW_NAN) && st->end - p >= 8 && memcmp(p, "Infinity", 8) == 0) {
            st->p = p + 8;
            return korb_float_new(c, slots, -INFINITY);
        }
        return KJ_FAIL(st);
    }
    const size_t idigits = mdigits;
    bool integer = true;
    int64_t exponent = 0;
    if (p < st->end && *p == '.') {
        integer = false;
        p++;
        const size_t f = kj_digits(st, &p, &mantissa);
        if (f == 0) return KJ_FAIL(st);
        mdigits += f;
        exponent = -(int64_t)f;
    }
    if (p < st->end && (*p == 'e' || *p == 'E')) {
        integer = false;
        p++;
        bool eneg = false;
        if (p < st->end && (*p == '+' || *p == '-')) { eneg = *p == '-'; p++; }
        uint64_t e = 0;
        const size_t ed = kj_digits(st, &p, &e);
        if (ed == 0) return KJ_FAIL(st);
        if (ed >= 19 || e > (uint64_t)INT32_MAX * 2) exponent = eneg ? INT64_MIN / 2 : INT64_MAX / 2;
        else exponent += eneg ? -(int64_t)e : (int64_t)e;
    }
    st->p = p;
    if (integer) {
        if (idigits <= 18) return RESULT_OK(LONG2FIX(neg ? -(long)mantissa : (long)mantissa));
        korb_value out;
        if (!korb_str_to_int(c, slots, (const char *)start, (uint32_t)(p - start), 10, &out)) return KJ_FAIL(st);
        return RESULT_OK(out);
    }
    if (exponent > INT32_MAX) return korb_float_new(c, slots, neg ? -INFINITY : INFINITY);
    if (exponent < INT32_MIN) return korb_float_new(c, slots, neg ? -0.0 : 0.0);
    if (mdigits > 18 || (int64_t)mdigits + exponent < -307) {
        if ((int64_t)mdigits + exponent < -324) return korb_float_new(c, slots, neg ? -0.0 : 0.0);
        const size_t n = (size_t)(p - start);
        char small[64];
        char *const tok = n < sizeof small ? small : malloc(n + 1);
        if (!tok) abort();
        memcpy(tok, start, n);
        tok[n] = '\0';
        const double d = strtod(tok, NULL);
        if (tok != small) free(tok);
        return korb_float_new(c, slots, d);
    }
    return korb_float_new(c, slots, ffp_s2d(exponent, mantissa, neg));
}

static bool kj_keyword(struct kj_state *const st, const char *const w, const size_t n)
{
    if ((size_t)(st->end - st->p) < n || memcmp(st->p, w, n) != 0) return false;
    st->p += n;
    return true;
}

static void kj_freeze(const korb_value v) { ((AroObjectHeader *)(uintptr_t)v)->flags |= KORB_FL_FROZEN; }

static RESULT kj_value(CTX *c, korb_value *slots, struct kj_state *st);

/* Values of the open containers wait on one rooted Array (json's rvalue stack);
 * a container is made at its exact size once it closes. */
static RESULT kj_push(CTX *c, korb_value *slots, struct kj_state *const st, const korb_value v)
{
    return korb_ary_push_val(c, slots, KORB_REF_AT(st->stack), v);
}

static RESULT kj_array(CTX *c, korb_value *slots, struct kj_state *const st)
{
    st->p++;                                                /* '[' */
    if (!kj_skip_ws(st)) return KJ_FAIL(st);
    const uint32_t base = VAL2ARY(*st->stack)->len;
    if (st->p < st->end && *st->p == ']') {
        st->p++;
    } else {
        if (++st->depth > KJ_MAX_DEPTH || (st->max_nesting != 0 && st->depth > st->max_nesting)) return KJ_FAIL(st);
        for (;;) {
            const korb_value v = UNWRAP(kj_value(c, slots, st));
            if (st->fail) return RESULT_OK(KORB_UNDEF);
            CHECK(kj_push(c, slots, st, v));
            if (!kj_skip_ws(st) || st->p >= st->end) return KJ_FAIL(st);
            if (*st->p == ',') {
                st->p++;
                if (!kj_skip_ws(st)) return KJ_FAIL(st);
                if ((st->flags & KJ_TRAILING_COMMA) && st->p < st->end && *st->p == ']') { st->p++; break; }
                continue;
            }
            if (*st->p != ']') return KJ_FAIL(st);
            st->p++;
            break;
        }
        st->depth--;
    }
    const uint32_t n = VAL2ARY(*st->stack)->len - base;
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    KorbArray *const a = VAL2ARY(slots[0]);
    KorbArray *const sa = VAL2ARY(*st->stack);              /* re-read after the alloc */
    ARO_STORE_BULK(c, korb_ary_owner(a), korb_ary_ptr(a), korb_ary_ptr(sa) + base, n);
    a->len = n;
    sa->len = base;
    if (st->flags & KJ_FREEZE) kj_freeze(slots[0]);
    return RESULT_OK(slots[0]);
}

/* Keys are interned Strings or Symbols, so a duplicate is the same object. */
static bool kj_keys_unique(const korb_value *const kv, const uint32_t n)
{
    if (n <= 32) {
        for (uint32_t i = 1; i < n; i++)
            for (uint32_t j = 0; j < i; j++)
                if (kv[2 * i] == kv[2 * j]) return false;
        return true;
    }
    uint32_t cap = 64;
    while (cap < n * 2) cap *= 2;
    korb_value *const set = calloc(cap, sizeof(korb_value));
    if (!set) abort();
    bool uniq = true;
    for (uint32_t i = 0; i < n && uniq; i++) {
        const korb_value k = kv[2 * i];
        uint32_t h = (uint32_t)((k >> 3) * 0x9E3779B1u) & (cap - 1);
        while (set[h] && set[h] != k) h = (h + 1) & (cap - 1);
        if (set[h] == k) uniq = false;
        set[h] = k;
    }
    free(set);
    return uniq;
}

static RESULT kj_object(CTX *c, korb_value *slots, struct kj_state *const st)
{
    st->p++;                                                /* '{' */
    if (!kj_skip_ws(st)) return KJ_FAIL(st);
    const uint32_t base = VAL2ARY(*st->stack)->len;
    if (st->p < st->end && *st->p == '}') {
        st->p++;
    } else {
        if (++st->depth > KJ_MAX_DEPTH || (st->max_nesting != 0 && st->depth > st->max_nesting)) return KJ_FAIL(st);
        for (;;) {
            if (st->p >= st->end || *st->p != '"') return KJ_FAIL(st);
            const korb_value k = UNWRAP(kj_string(c, slots, st, true));
            if (st->fail) return RESULT_OK(KORB_UNDEF);
            CHECK(kj_push(c, slots, st, k));
            if (!kj_skip_ws(st) || st->p >= st->end || *st->p != ':') return KJ_FAIL(st);
            st->p++;
            if (!kj_skip_ws(st)) return KJ_FAIL(st);
            const korb_value v = UNWRAP(kj_value(c, slots, st));
            if (st->fail) return RESULT_OK(KORB_UNDEF);
            CHECK(kj_push(c, slots, st, v));
            if (!kj_skip_ws(st) || st->p >= st->end) return KJ_FAIL(st);
            if (*st->p == ',') {
                st->p++;
                if (!kj_skip_ws(st)) return KJ_FAIL(st);
                if ((st->flags & KJ_TRAILING_COMMA) && st->p < st->end && *st->p == '}') { st->p++; break; }
                continue;
            }
            if (*st->p != '}') return KJ_FAIL(st);
            st->p++;
            break;
        }
        st->depth--;
    }
    const uint32_t n = (VAL2ARY(*st->stack)->len - base) / 2;
    const bool uniq = kj_keys_unique(korb_ary_ptr(VAL2ARY(*st->stack)) + base, n);
    if (!uniq && !(st->flags & KJ_DUP_KEY)) return KJ_FAIL(st);   /* ParserError: the Ruby port reports it */
    slots[0] = UNWRAP(korb_hash_new(c, slots, n));
    const korb_ref hash = KORB_REF_AT(&slots[0]);
    if (uniq) {                                             /* fill in one go, then index once */
        KorbHash *const h = VAL2HASH(slots[0]);
        const KorbArray *const sa = VAL2ARY(*st->stack);
        ARO_STORE_BULK(c, korb_hash_owner(h), korb_hash_pairs(h), korb_ary_ptr(sa) + base, 2 * (size_t)n);
        h->len = n;
        if (n >= KORB_HASH_INDEX_THRESHOLD) CHECK(korb_hash_index_build(c, slots + 1, hash));
    } else {                                                /* allow_duplicate_key: the last value wins */
        for (uint32_t i = 0; i < n; i++) {
            slots[1] = korb_ary_ptr(VAL2ARY(*st->stack))[base + 2 * i];
            const korb_value v = korb_ary_ptr(VAL2ARY(*st->stack))[base + 2 * i + 1];
            CHECK(korb_hash_set(c, slots + 2, hash, KORB_REF_AT(&slots[1]), v));
        }
    }
    VAL2ARY(*st->stack)->len = base;
    if (st->flags & KJ_FREEZE) kj_freeze(KORB_REF_GET(hash));
    return RESULT_OK(KORB_REF_GET(hash));
}

/* st->p at the first byte of a value (whitespace already skipped). */
static RESULT kj_value(CTX *c, korb_value *slots, struct kj_state *const st)
{
    if (st->p >= st->end) return KJ_FAIL(st);
    switch (*st->p) {
      case '"': return kj_string(c, slots, st, false);
      case '{': return kj_object(c, slots, st);
      case '[': return kj_array(c, slots, st);
      case '-': case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
        return kj_number(c, slots, st);
      case 'n': return kj_keyword(st, "null", 4) ? RESULT_OK(KORB_NIL) : KJ_FAIL(st);
      case 't': return kj_keyword(st, "true", 4) ? RESULT_OK(KORB_TRUE) : KJ_FAIL(st);
      case 'f': return kj_keyword(st, "false", 5) ? RESULT_OK(KORB_FALSE) : KJ_FAIL(st);
      case 'N':
        if ((st->flags & KJ_ALLOW_NAN) && kj_keyword(st, "NaN", 3)) return korb_float_new(c, slots, NAN);
        return KJ_FAIL(st);
      case 'I':
        if ((st->flags & KJ_ALLOW_NAN) && kj_keyword(st, "Infinity", 8)) return korb_float_new(c, slots, INFINITY);
        return KJ_FAIL(st);
      default:
        return KJ_FAIL(st);
    }
}

/* __json_parse(source, flags, max_nesting) → the value, or `source` itself (which no
 * parse returns) to say "use the Ruby parser"; that includes a source that is not a
 * UTF-8 / binary String. */
static RESULT
korb_bi_json_parse(CTX *c, korb_value *slots, korb_slice args)
{
    const korb_value fallback = KORB_SLICE_LEN(args) >= 1 ? KORB_SLICE_GET(args, 0) : KORB_NIL;
    if (KORB_SLICE_LEN(args) != 3 || !KORB_STRING_P(fallback) ||
        (KORB_STR_ENC(fallback) != KORB_ENC_UTF8 && KORB_STR_ENC(fallback) != KORB_ENC_BINARY) ||
        (((const AroObjectHeader *)(uintptr_t)fallback)->flags & KORB_FL_HAS_KLASS) ||
        !FIXNUM_P(KORB_SLICE_GET(args, 1)) || !FIXNUM_P(KORB_SLICE_GET(args, 2)))
        return RESULT_OK(fallback);
    const KorbString *const src = VAL2STR(KORB_SLICE_GET(args, 0));
    const size_t n = src->len;
    unsigned char *const copy = malloc(n ? n : 1);
    if (!copy) abort();
    memcpy(copy, korb_str_ptr(src), n);
    struct kj_state st = {
        .p = copy, .end = copy + n,
        .flags = (uint32_t)FIX2LONG(KORB_SLICE_GET(args, 1)),
        .max_nesting = FIX2LONG(KORB_SLICE_GET(args, 2)),
    };
    RESULT r = korb_ary_new(c, slots, 32);
    if (r.state != KORB_NORMAL) { free(copy); return r; }
    slots[0] = r.value;
    st.stack = &slots[0];
    slots[1] = KORB_NIL;
    st.kcache = &slots[1];
    if (kj_skip_ws(&st)) {
        r = kj_value(c, slots + 2, &st);
        if (r.state == KORB_NORMAL && !st.fail && (!kj_skip_ws(&st) || st.p != st.end)) st.fail = true;
    } else {
        st.fail = true;
    }
    free(copy);
    free(st.buf);
    if (r.state != KORB_NORMAL) return r;
    return RESULT_OK(st.fail ? KORB_SLICE_GET(args, 0) : r.value);   /* re-read: parsing may have moved it */
}

/* ---- generator ------------------------------------------------------------
 * __json_generate(obj, indent, space, space_before, object_nl, array_nl, max_nesting,
 * depth, allow_nan) — the fast path of Gen#run (lib/json/ext/generator.rb) for a
 * State without ascii_only / script_safe / strict / sort_keys / as_json.  It
 * writes exact Hash / Array / String / Integer / Float / nil / true / false and
 * answers nil on anything else (another class, a subclass, mixed
 * key types, an encoding it would have to check or convert, NaN without
 * allow_nan, too deep nesting), which the Ruby generator then handles in full.
 * No Ruby object is made until the result String, so the walk cannot move
 * the objects it reads. */

#include "../vendor/json/fpconv.c"

struct kjg {
    char *buf; size_t len, cap;
    const char *indent, *space, *space_before, *object_nl, *array_nl;
    uint32_t indent_n, space_n, space_before_n, object_nl_n, array_nl_n;
    long max_nesting, depth;
    bool allow_nan, fail;
    struct korb_vm *vm;
};

static __attribute__((noinline)) void kjg_grow(struct kjg *const g, const size_t add)
{
    size_t cap = g->cap ? g->cap * 2 : 1024;
    while (cap < g->len + add) cap *= 2;
    char *const nb = realloc(g->buf, cap);
    if (!nb) abort();
    g->buf = nb; g->cap = cap;
}
static inline void kjg_reserve(struct kjg *const g, const size_t add)
{
    if (UNLIKELY(g->len + add > g->cap)) kjg_grow(g, add);
}
static inline void kjg_put(struct kjg *const g, const char *const s, const size_t n)
{
    kjg_reserve(g, n);
    char *const d = g->buf + g->len;
    if (n <= 8) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }   /* separators and layout: no libc call */
    else memcpy(d, s, n);
    g->len += n;
}
static inline void kjg_ch(struct kjg *const g, const char ch) { kjg_reserve(g, 1); g->buf[g->len++] = ch; }
static void kjg_indent(struct kjg *const g, const long depth)
{
    if (g->indent_n == 0) return;
    for (long i = 0; i < depth; i++) kjg_put(g, g->indent, g->indent_n);
}

/* ascii-only, or UTF-8 and well-formed (Gen#valid_string?) — for the layout strings */
static bool kjg_valid(const unsigned char *const p, const size_t n, const uint32_t enc)
{
    bool ascii = true;
    for (size_t i = 0; i < n && ascii; i++) ascii = p[i] < 0x80;
    return ascii || (enc == KORB_ENC_UTF8 && kj_utf8_valid(p, n));
}

/* no byte in w needs a look: none < 0x20, '"', '\\' or >= 0x80 (a set bit may be a
 * false positive above a real hit, never a miss) */
static inline bool kjg_word_clean(const uint64_t w)
{
    const uint64_t lo = 0x0101010101010101ULL, hi = 0x8080808080808080ULL;
    const uint64_t q = w ^ (lo * '"'), b = w ^ (lo * '\\');
    return (((w - lo * 0x20) | w | ((q - lo) & ~q) | ((b - lo) & ~b)) & hi) == 0;
}

/* json's convert_UTF8_to_JSON (basic table: '"', '\\', C0 controls), validating the
 * UTF-8 in the same pass (Gen#valid_string?).  false = leave the string to Ruby. */
static bool kjg_string(struct kjg *const g, const unsigned char *const p, const size_t n, const uint32_t enc)
{
    static const char hex[] = "0123456789abcdef";
    kjg_reserve(g, n + 2);
    g->buf[g->len++] = '"';
    size_t run = 0, i = 0;
    while (i < n) {
        if (i + 8 <= n) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            if (kjg_word_clean(w)) { i += 8; continue; }
        }
        const unsigned char ch = p[i];
        if (ch >= 0x80) {
            const uint32_t cl = enc == KORB_ENC_UTF8 ? korb_utf8_valid_len(p + i, (uint32_t)(n - i)) : 0;
            if (cl == 0) return false;
            i += cl;
            continue;
        }
        if (ch >= 0x20 && ch != '"' && ch != '\\') { i++; continue; }
        kjg_put(g, (const char *)p + run, i - run);
        run = ++i;
        switch (ch) {
          case '"': kjg_put(g, "\\\"", 2); break;
          case '\\': kjg_put(g, "\\\\", 2); break;
          case '\b': kjg_put(g, "\\b", 2); break;
          case '\f': kjg_put(g, "\\f", 2); break;
          case '\n': kjg_put(g, "\\n", 2); break;
          case '\r': kjg_put(g, "\\r", 2); break;
          case '\t': kjg_put(g, "\\t", 2); break;
          default: { const char u[6] = { '\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 0xf] }; kjg_put(g, u, 6); }
        }
    }
    kjg_put(g, (const char *)p + run, n - run);
    kjg_ch(g, '"');
    return true;
}

static void kjg_long(struct kjg *const g, const long v)
{
    char tmp[24];
    char *e = tmp + sizeof tmp, *q = e;
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    do { *--q = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *--q = '-';
    kjg_put(g, q, (size_t)(e - q));
}

static void kjg_value(struct kjg *g, korb_value v);

static bool kjg_nest(struct kjg *const g)
{
    g->depth++;
    if (g->max_nesting != 0 && g->depth > g->max_nesting) { g->fail = true; return false; }
    return true;
}

static void kjg_array(struct kjg *const g, const korb_value v)
{
    if (!kjg_nest(g)) return;
    const KorbArray *const a = VAL2ARY(v);
    if (a->len == 0) { kjg_put(g, "[]", 2); g->depth--; return; }
    kjg_ch(g, '[');
    kjg_put(g, g->array_nl, g->array_nl_n);
    for (uint32_t i = 0; i < a->len && !g->fail; i++) {
        if (i > 0) { kjg_ch(g, ','); kjg_put(g, g->array_nl, g->array_nl_n); }
        kjg_indent(g, g->depth);
        kjg_value(g, korb_ary_ptr(a)[i]);
    }
    g->depth--;
    if (g->array_nl_n) { kjg_put(g, g->array_nl, g->array_nl_n); kjg_indent(g, g->depth); }
    kjg_ch(g, ']');
}

static void kjg_object(struct kjg *const g, const korb_value v)
{
    if (!kjg_nest(g)) return;
    const KorbHash *const h = VAL2HASH(v);
    if (h->len == 0) { kjg_put(g, "{}", 2); g->depth--; return; }
    kjg_ch(g, '{');
    int first_type = 0;                                     /* 1 = String keys, 2 = Symbol keys */
    for (uint32_t i = 0; i < h->len && !g->fail; i++) {
        const korb_value k = korb_hash_pairs(h)[2 * i];
        const int type = SYMBOL_P(k) ? 2 : (KORB_STRING_P(k) && !(((const AroObjectHeader *)(uintptr_t)k)->flags & KORB_FL_HAS_KLASS)) ? 1 : 0;
        if (type == 0 || (first_type != 0 && type != first_type)) { g->fail = true; return; }   /* other / mixed keys: Ruby */
        first_type = type;
        if (i > 0) kjg_ch(g, ',');
        kjg_put(g, g->object_nl, g->object_nl_n);
        kjg_indent(g, g->depth);
        const unsigned char *kp; size_t kn; uint32_t enc;
        if (type == 1) { const KorbString *const s = VAL2STR(k); kp = (const unsigned char *)korb_str_ptr(s); kn = s->len; enc = KORB_STR_ENC(k); }
        else { const uint32_t id = SYM2ID(k); kp = (const unsigned char *)korb_sym_name(g->vm, id); kn = g->vm->sym_lens[id]; enc = g->vm->sym_encs[id]; }
        if (!kjg_string(g, kp, kn, enc)) { g->fail = true; return; }
        kjg_put(g, g->space_before, g->space_before_n);
        kjg_ch(g, ':');
        kjg_put(g, g->space, g->space_n);
        kjg_value(g, korb_hash_pairs(h)[2 * i + 1]);
    }
    g->depth--;
    if (g->object_nl_n) { kjg_put(g, g->object_nl, g->object_nl_n); kjg_indent(g, g->depth); }
    kjg_ch(g, '}');
}

static void kjg_value(struct kjg *const g, const korb_value v)
{
    if (g->fail) return;
    if (v == KORB_NIL) { kjg_put(g, "null", 4); return; }
    if (v == KORB_TRUE) { kjg_put(g, "true", 4); return; }
    if (v == KORB_FALSE) { kjg_put(g, "false", 5); return; }
    if (FIXNUM_P(v)) {
        kjg_long(g, FIX2LONG(v));
        return;
    }
    if (KORB_FLOAT_P(v)) {
        const double d = korb_float_val(v);
        if (isnan(d) || isinf(d)) {
            if (!g->allow_nan) { g->fail = true; return; }   /* GeneratorError: Ruby raises it */
            if (isnan(d)) kjg_put(g, "NaN", 3);
            else if (d > 0) kjg_put(g, "Infinity", 8);
            else kjg_put(g, "-Infinity", 9);
            return;
        }
        kjg_reserve(g, 32);
        g->len += (size_t)fpconv_dtoa(d, g->buf + g->len);
        return;
    }
    if (!AROH_IS_GC_OBJECT(v) || (((const AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_HAS_KLASS)) { g->fail = true; return; }
    if (KORB_STRING_P(v)) {
        const KorbString *const s = VAL2STR(v);
        const unsigned char *const p = (const unsigned char *)korb_str_ptr(s);
        if (!kjg_string(g, p, s->len, KORB_STR_ENC(v))) g->fail = true;
        return;
    }
    if (KORB_ARRAY_P(v)) { kjg_array(g, v); return; }
    if (KORB_HASH_P(v)) {
        if (VAL2HASH(v)->head.flags & KORB_FL_CMP_BY_ID) { g->fail = true; return; }
        kjg_object(g, v);
        return;
    }
    g->fail = true;   /* Bignum (CRuby calls its #to_s), Symbol, Fragment, any other object: Ruby */
}

static bool kjg_str_arg(const korb_value v, const char **const p, uint32_t *const n)
{
    if (v == KORB_NIL) { *p = ""; *n = 0; return true; }
    if (!KORB_STRING_P(v)) return false;
    *p = korb_str_ptr(VAL2STR(v)); *n = VAL2STR(v)->len;
    return kjg_valid((const unsigned char *)*p, *n, KORB_STR_ENC(v));   /* else `buf << indent` may raise: Ruby */
}

static RESULT
korb_bi_json_generate(CTX *c, korb_value *slots, korb_slice args)
{
    const korb_value fallback = KORB_NIL;
    if (KORB_SLICE_LEN(args) != 9 || !FIXNUM_P(KORB_SLICE_GET(args, 6)) || !FIXNUM_P(KORB_SLICE_GET(args, 7)))
        return RESULT_OK(fallback);
    struct kjg g = { .vm = c->vm, .max_nesting = FIX2LONG(KORB_SLICE_GET(args, 6)),
                     .depth = FIX2LONG(KORB_SLICE_GET(args, 7)), .allow_nan = KORB_TRUTHY(KORB_SLICE_GET(args, 8)) };
    if (!kjg_str_arg(KORB_SLICE_GET(args, 1), &g.indent, &g.indent_n) ||
        !kjg_str_arg(KORB_SLICE_GET(args, 2), &g.space, &g.space_n) ||
        !kjg_str_arg(KORB_SLICE_GET(args, 3), &g.space_before, &g.space_before_n) ||
        !kjg_str_arg(KORB_SLICE_GET(args, 4), &g.object_nl, &g.object_nl_n) ||
        !kjg_str_arg(KORB_SLICE_GET(args, 5), &g.array_nl, &g.array_nl_n))
        return RESULT_OK(fallback);
    kjg_value(&g, KORB_SLICE_GET(args, 0));
    if (g.fail) { free(g.buf); return RESULT_OK(fallback); }
    const RESULT r = korb_str_new(c, slots, g.buf ? g.buf : "", (uint32_t)g.len);
    free(g.buf);
    return r;
}
