/* MD5 / SHA-1 / SHA-256 / SHA-512 over a whole String.  lib/digest.rb buffers
 * the message and hashes it in one go, so one call per digest is enough and the
 * compression functions need no streaming state.  The Ruby versions these
 * replace were ~25x slower than CRuby's (20k HMAC-SHA1 rounds: 4.0s vs 0.16s),
 * which is what a PBKDF2 costs.  #included into korb_runtime.c's TU.
 *
 * The digest is built in a C buffer first: the input bytes live in a movable
 * String, so nothing may allocate until the last byte has been read. */

#define KORB_ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define KORB_ROTR32(v, n) (((v) >> (n)) | ((v) << (32 - (n))))
#define KORB_ROTR64(v, n) (((v) >> (n)) | ((v) << (64 - (n))))

static void korb_dg_put32be(uint8_t *const p, const uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void korb_dg_put64be(uint8_t *const p, const uint64_t v) {
    korb_dg_put32be(p, (uint32_t)(v >> 32)); korb_dg_put32be(p + 4, (uint32_t)v);
}
static uint32_t korb_dg_get32be(const uint8_t *const p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t korb_dg_get64be(const uint8_t *const p) {
    return ((uint64_t)korb_dg_get32be(p) << 32) | korb_dg_get32be(p + 4);
}
static uint32_t korb_dg_get32le(const uint8_t *const p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- MD5 ---------------------------------------------------------------- */

static const uint32_t korb_md5_k[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};
static const uint8_t korb_md5_s[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static void korb_md5_block(uint32_t h[4], const uint8_t *const blk) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = korb_dg_get32le(blk + 4 * i);
    uint32_t a = h[0], b = h[1], cc = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & cc) | (~b & d);       g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc);       g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d;                g = (3 * i + 5) & 15; }
        else             { f = cc ^ (b | ~d);             g = (7 * i) & 15; }
        const uint32_t tmp = d;
        d = cc; cc = b;
        b = b + KORB_ROTL32(a + f + korb_md5_k[i] + m[g], korb_md5_s[i]);
        a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += cc; h[3] += d;
}

/* ---- SHA-1 -------------------------------------------------------------- */

static void korb_sha1_block(uint32_t h[5], const uint8_t *const blk) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = korb_dg_get32be(blk + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = KORB_ROTL32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);            k = 0x5a827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8f1bbcdcu; }
        else             { f = b ^ cc ^ d;                     k = 0xca62c1d6u; }
        const uint32_t t = KORB_ROTL32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = KORB_ROTL32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += cc; h[3] += d; h[4] += e;
}

/* ---- SHA-256 ------------------------------------------------------------ */

static const uint32_t korb_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void korb_sha256_block(uint32_t h[8], const uint8_t *const blk) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = korb_dg_get32be(blk + 4 * i);
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = KORB_ROTR32(w[i-15], 7) ^ KORB_ROTR32(w[i-15], 18) ^ (w[i-15] >> 3);
        const uint32_t s1 = KORB_ROTR32(w[i-2], 17) ^ KORB_ROTR32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = KORB_ROTR32(e, 6) ^ KORB_ROTR32(e, 11) ^ KORB_ROTR32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + S1 + ch + korb_sha256_k[i] + w[i];
        const uint32_t S0 = KORB_ROTR32(a, 2) ^ KORB_ROTR32(a, 13) ^ KORB_ROTR32(a, 22);
        const uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += cc; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ---- SHA-512 / SHA-384 -------------------------------------------------- */

static const uint64_t korb_sha512_k[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
};

static void korb_sha512_block(uint64_t h[8], const uint8_t *const blk) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) w[i] = korb_dg_get64be(blk + 8 * i);
    for (int i = 16; i < 80; i++) {
        const uint64_t s0 = KORB_ROTR64(w[i-15], 1) ^ KORB_ROTR64(w[i-15], 8) ^ (w[i-15] >> 7);
        const uint64_t s1 = KORB_ROTR64(w[i-2], 19) ^ KORB_ROTR64(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint64_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        const uint64_t S1 = KORB_ROTR64(e, 14) ^ KORB_ROTR64(e, 18) ^ KORB_ROTR64(e, 41);
        const uint64_t ch = (e & f) ^ (~e & g);
        const uint64_t t1 = hh + S1 + ch + korb_sha512_k[i] + w[i];
        const uint64_t S0 = KORB_ROTR64(a, 28) ^ KORB_ROTR64(a, 34) ^ KORB_ROTR64(a, 39);
        const uint64_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint64_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += cc; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ---- the message walk --------------------------------------------------- */

/* kind: 0 = MD5, 1 = SHA-1, 2 = SHA-256, 3 = SHA-512, 4 = SHA-384.
 * Writes the digest into `out` and answers its length. */
static uint32_t korb_dg_run(const int kind, const uint8_t *const msg, const size_t len, uint8_t *const out)
{
    const bool wide = (kind >= 3);
    const size_t blocksz = wide ? 128u : 64u;
    const size_t lenfield = wide ? 16u : 8u;
    uint32_t h32[8];
    uint64_t h64[8];
    switch (kind) {
      case 0: h32[0] = 0x67452301u; h32[1] = 0xefcdab89u; h32[2] = 0x98badcfeu; h32[3] = 0x10325476u; break;
      case 1: h32[0] = 0x67452301u; h32[1] = 0xefcdab89u; h32[2] = 0x98badcfeu; h32[3] = 0x10325476u;
              h32[4] = 0xc3d2e1f0u; break;
      case 2: h32[0] = 0x6a09e667u; h32[1] = 0xbb67ae85u; h32[2] = 0x3c6ef372u; h32[3] = 0xa54ff53au;
              h32[4] = 0x510e527fu; h32[5] = 0x9b05688cu; h32[6] = 0x1f83d9abu; h32[7] = 0x5be0cd19u; break;
      case 3: h64[0] = 0x6a09e667f3bcc908ull; h64[1] = 0xbb67ae8584caa73bull; h64[2] = 0x3c6ef372fe94f82bull;
              h64[3] = 0xa54ff53a5f1d36f1ull; h64[4] = 0x510e527fade682d1ull; h64[5] = 0x9b05688c2b3e6c1full;
              h64[6] = 0x1f83d9abfb41bd6bull; h64[7] = 0x5be0cd19137e2179ull; break;
      default: h64[0] = 0xcbbb9d5dc1059ed8ull; h64[1] = 0x629a292a367cd507ull; h64[2] = 0x9159015a3070dd17ull;
               h64[3] = 0x152fecd8f70e5939ull; h64[4] = 0x67332667ffc00b31ull; h64[5] = 0x8eb44a8768581511ull;
               h64[6] = 0xdb0c2e0d64f98fa7ull; h64[7] = 0x47b5481dbefa4fa4ull; break;
    }

    size_t off = 0;
    for (; len - off >= blocksz; off += blocksz) {
        if (wide)           korb_sha512_block(h64, msg + off);
        else if (kind == 0) korb_md5_block(h32, msg + off);
        else if (kind == 1) korb_sha1_block(h32, msg + off);
        else                korb_sha256_block(h32, msg + off);
    }
    /* tail + padding: at most two more blocks */
    uint8_t tail[256];
    const size_t rest = len - off;
    memcpy(tail, msg + off, rest);
    tail[rest] = 0x80u;
    const size_t total = rest + 1;
    const size_t need = blocksz - lenfield;             /* room left for the bit count */
    const size_t end = (total <= need) ? blocksz : 2 * blocksz;
    memset(tail + total, 0, end - total);
    const uint64_t bits = (uint64_t)len * 8u;
    if (kind == 0) {                                    /* MD5 appends the count little-endian */
        for (int i = 0; i < 8; i++) tail[end - 8 + i] = (uint8_t)(bits >> (8 * i));
    } else {
        if (wide) memset(tail + end - 16, 0, 8);        /* the high half of the 128-bit count */
        korb_dg_put64be(tail + end - 8, bits);
    }
    for (size_t p = 0; p < end; p += blocksz) {
        if (wide)           korb_sha512_block(h64, tail + p);
        else if (kind == 0) korb_md5_block(h32, tail + p);
        else if (kind == 1) korb_sha1_block(h32, tail + p);
        else                korb_sha256_block(h32, tail + p);
    }

    switch (kind) {
      case 0:   /* MD5 is little-endian on the way out */
        for (int i = 0; i < 4; i++) {
            out[4*i] = (uint8_t)h32[i]; out[4*i+1] = (uint8_t)(h32[i] >> 8);
            out[4*i+2] = (uint8_t)(h32[i] >> 16); out[4*i+3] = (uint8_t)(h32[i] >> 24);
        }
        return 16;
      case 1: for (int i = 0; i < 5; i++) korb_dg_put32be(out + 4 * i, h32[i]); return 20;
      case 2: for (int i = 0; i < 8; i++) korb_dg_put32be(out + 4 * i, h32[i]); return 32;
      case 3: for (int i = 0; i < 8; i++) korb_dg_put64be(out + 8 * i, h64[i]); return 64;
      default: for (int i = 0; i < 6; i++) korb_dg_put64be(out + 8 * i, h64[i]); return 48;
    }
}

static RESULT korb_m_digest_of(CTX *c, korb_value *slots, korb_slice a, const int kind)
{
    const korb_value s = KORB_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(s)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, s));
    uint8_t out[64];
    uint32_t n;
    {   /* the input bytes are borrowed from a movable String: no alloc in here */
        const KorbString *const ks = VAL2STR(s);
        n = korb_dg_run(kind, (const uint8_t *)korb_strbuf_data(ks->buf), ks->len, out);
    }
    const RESULT r = korb_str_new(c, slots, (const char *)out, n);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
    return r;
}

static RESULT korb_m_digest_md5(CTX *c, korb_value *slots, korb_ref self, korb_slice a)    { (void)self; return korb_m_digest_of(c, slots, a, 0); }
static RESULT korb_m_digest_sha1(CTX *c, korb_value *slots, korb_ref self, korb_slice a)   { (void)self; return korb_m_digest_of(c, slots, a, 1); }
static RESULT korb_m_digest_sha256(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self; return korb_m_digest_of(c, slots, a, 2); }
static RESULT korb_m_digest_sha512(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self; return korb_m_digest_of(c, slots, a, 3); }
static RESULT korb_m_digest_sha384(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self; return korb_m_digest_of(c, slots, a, 4); }

static void korb_init_digest(CTX *c) {
    const korb_value obj = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
    korb_class_def_cfn(c, obj, "__digest_md5",    korb_m_digest_md5,    1);
    korb_class_def_cfn(c, obj, "__digest_sha1",   korb_m_digest_sha1,   1);
    korb_class_def_cfn(c, obj, "__digest_sha256", korb_m_digest_sha256, 1);
    korb_class_def_cfn(c, obj, "__digest_sha512", korb_m_digest_sha512, 1);
    korb_class_def_cfn(c, obj, "__digest_sha384", korb_m_digest_sha384, 1);
}
