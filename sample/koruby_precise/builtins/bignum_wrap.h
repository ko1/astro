/* koruby_precise — 多倍長整数 backend: wrap。
 *
 * 「多倍長」をやめて 64bit の 2 の補数で wrap する backend。外部依存が無いので
 * GMP を持ち込めない構成 (wasm など) 用。**Ruby とは意味論が違う**:
 *
 *   2**64            → 0            (GMP backend なら 18446744073709551616)
 *   (10**30).to_s    → 頭が落ちた値
 *
 * つまり 2**63 を跨いだ計算の答えは合わない。合わない代わりに、どの演算も
 * 定数時間で終わり malloc もしない。Integer の意味論そのもの (Fixnum からの
 * 昇格、丸め、to_s の基数変換など) は bignum.c 側が持っているので、ここは
 * 純粋に「多倍長の代わり」だけを提供する。
 *
 * korb_mp_t を配列型にしてあるのは mpz_t と同じで、呼び出し側の
 * `korb_mp_t z; korb_mp_init(z); f(z);` がそのまま通るようにするため。 */
#ifndef KORB_BIGNUM_WRAP_H
#define KORB_BIGNUM_WRAP_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

typedef int64_t        korb_mp_t[1];
typedef const int64_t *korb_mp_srcptr;
typedef int64_t       *korb_mp_ptr;
typedef uint64_t       korb_mp_limb_t;
typedef long           korb_mp_size_t;
typedef unsigned long  korb_mp_bitcnt_t;

/* 符号なしで計算して戻す = 2 の補数で wrap (符号付きの overflow は UB なので) */
#define KORB_MPW_U(x)  ((uint64_t)(x))
#define KORB_MPW_S(x)  ((int64_t)(uint64_t)(x))

static inline void korb_mp_init(korb_mp_ptr r)                  { *r = 0; }
static inline void korb_mp_clear(korb_mp_ptr r)                 { (void)r; }
static inline void korb_mp_init_set(korb_mp_ptr r, korb_mp_srcptr a) { *r = *a; }
static inline void korb_mp_init_set_si(korb_mp_ptr r, long v)   { *r = (int64_t)v; }
static inline void korb_mp_init_set_ui(korb_mp_ptr r, unsigned long v) { *r = KORB_MPW_S(v); }
static inline void korb_mp_set_ui(korb_mp_ptr r, unsigned long v)     { *r = KORB_MPW_S(v); }
static inline void korb_mp_swap(korb_mp_ptr a, korb_mp_ptr b)   { const int64_t t = *a; *a = *b; *b = t; }

/* double → 整数。範囲外は wrap ではなく飽和させる (UB を避けるため) */
static inline void korb_mp_set_d(korb_mp_ptr r, double d) {
    if (!(d > -9.2233720368547758e18 && d < 9.2233720368547758e18)) {
        *r = (d < 0) ? INT64_MIN : INT64_MAX;
    } else {
        *r = (int64_t)d;
    }
}
static inline void korb_mp_init_set_d(korb_mp_ptr r, double d)  { korb_mp_set_d(r, d); }

static inline void korb_mp_add(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) { *r = KORB_MPW_S(KORB_MPW_U(*a) + KORB_MPW_U(*b)); }
static inline void korb_mp_sub(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) { *r = KORB_MPW_S(KORB_MPW_U(*a) - KORB_MPW_U(*b)); }
static inline void korb_mp_mul(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) { *r = KORB_MPW_S(KORB_MPW_U(*a) * KORB_MPW_U(*b)); }
static inline void korb_mp_add_ui(korb_mp_ptr r, korb_mp_srcptr a, unsigned long b) { *r = KORB_MPW_S(KORB_MPW_U(*a) + (uint64_t)b); }
static inline void korb_mp_sub_ui(korb_mp_ptr r, korb_mp_srcptr a, unsigned long b) { *r = KORB_MPW_S(KORB_MPW_U(*a) - (uint64_t)b); }
static inline void korb_mp_mul_ui(korb_mp_ptr r, korb_mp_srcptr a, unsigned long b) { *r = KORB_MPW_S(KORB_MPW_U(*a) * (uint64_t)b); }
static inline void korb_mp_neg(korb_mp_ptr r, korb_mp_srcptr a) { *r = KORB_MPW_S(0u - KORB_MPW_U(*a)); }
static inline void korb_mp_abs(korb_mp_ptr r, korb_mp_srcptr a) { *r = (*a < 0) ? KORB_MPW_S(0u - KORB_MPW_U(*a)) : *a; }
static inline void korb_mp_com(korb_mp_ptr r, korb_mp_srcptr a) { *r = KORB_MPW_S(~KORB_MPW_U(*a)); }
static inline void korb_mp_and(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) { *r = *a & *b; }
static inline void korb_mp_ior(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) { *r = *a | *b; }
static inline void korb_mp_xor(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) { *r = *a ^ *b; }

static inline int korb_mp_sgn(korb_mp_srcptr a) { return (*a > 0) - (*a < 0); }
static inline int korb_mp_cmp(korb_mp_srcptr a, korb_mp_srcptr b) { return (*a > *b) - (*a < *b); }
static inline int korb_mp_cmp_ui(korb_mp_srcptr a, unsigned long b) {
    if (*a < 0) return -1;
    const uint64_t ua = (uint64_t)*a;
    return (ua > (uint64_t)b) - (ua < (uint64_t)b);
}
static inline int korb_mp_cmpabs_ui(korb_mp_srcptr a, unsigned long b) {
    const uint64_t ua = (*a < 0) ? (0u - KORB_MPW_U(*a)) : KORB_MPW_U(*a);
    return (ua > (uint64_t)b) - (ua < (uint64_t)b);
}
static inline int korb_mp_even_p(korb_mp_srcptr a) { return (*a & 1) == 0; }
static inline int korb_mp_odd_p(korb_mp_srcptr a)  { return (*a & 1) != 0; }
static inline int korb_mp_fits_slong_p(korb_mp_srcptr a) { (void)a; return 1; }
static inline int korb_mp_fits_ulong_p(korb_mp_srcptr a) { return *a >= 0; }

/* 除算。0 除算は呼び出し側 (bignum.c) が先に弾いている */
static inline void korb_mp_tdiv_qr(korb_mp_ptr q, korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) {
    const int64_t qq = KORB_MPW_S(KORB_MPW_U(*a) / KORB_MPW_U(*b) * 0 + (uint64_t)(*a / *b));  /* trunc */
    const int64_t rr = *a - KORB_MPW_S(KORB_MPW_U(qq) * KORB_MPW_U(*b));
    if (q) *q = qq;
    if (r) *r = rr;
}
static inline void korb_mp_fdiv_qr(korb_mp_ptr q, korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) {
    int64_t qq = *a / *b, rr = *a % *b;
    if (rr != 0 && ((rr < 0) != (*b < 0))) { qq -= 1; rr += *b; }     /* 床方向に寄せる */
    if (q) *q = qq;
    if (r) *r = rr;
}
static inline void korb_mp_cdiv_qr(korb_mp_ptr q, korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) {
    int64_t qq = *a / *b, rr = *a % *b;
    if (rr != 0 && ((rr < 0) == (*b < 0))) { qq += 1; rr -= *b; }     /* 天井方向に寄せる */
    if (q) *q = qq;
    if (r) *r = rr;
}
static inline void korb_mp_fdiv_q(korb_mp_ptr q, korb_mp_srcptr a, korb_mp_srcptr b) { korb_mp_t t; korb_mp_fdiv_qr(q, t, a, b); }
static inline void korb_mp_fdiv_r(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) { korb_mp_t t; korb_mp_fdiv_qr(t, r, a, b); }
static inline void korb_mp_cdiv_q(korb_mp_ptr q, korb_mp_srcptr a, korb_mp_srcptr b) { korb_mp_t t; korb_mp_cdiv_qr(q, t, a, b); }
static inline void korb_mp_divexact(korb_mp_ptr q, korb_mp_srcptr a, korb_mp_srcptr b) { *q = *a / *b; }
static inline unsigned long korb_mp_fdiv_q_ui(korb_mp_ptr q, korb_mp_srcptr a, unsigned long b) {
    korb_mp_t bb; *bb = KORB_MPW_S(b); korb_mp_t r;
    korb_mp_fdiv_qr(q, r, a, bb);
    return (unsigned long)*r;
}

/* シフト。64 以上のシフトは C では UB なので 0 に潰す */
static inline void korb_mp_mul_2exp(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_bitcnt_t n) {
    *r = (n >= 64) ? 0 : KORB_MPW_S(KORB_MPW_U(*a) << n);
}
static inline void korb_mp_fdiv_q_2exp(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_bitcnt_t n) {
    if (n >= 64) { *r = (*a < 0) ? -1 : 0; return; }
    *r = *a >> n;                                   /* 算術シフト = 床方向 */
}

static inline void korb_mp_gcd(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) {
    uint64_t x = (*a < 0) ? (0u - KORB_MPW_U(*a)) : KORB_MPW_U(*a);
    uint64_t y = (*b < 0) ? (0u - KORB_MPW_U(*b)) : KORB_MPW_U(*b);
    while (y) { const uint64_t t = x % y; x = y; y = t; }
    *r = KORB_MPW_S(x);
}
static inline void korb_mp_lcm(korb_mp_ptr r, korb_mp_srcptr a, korb_mp_srcptr b) {
    if (*a == 0 || *b == 0) { *r = 0; return; }
    korb_mp_t g; korb_mp_gcd(g, a, b);
    korb_mp_t q; korb_mp_divexact(q, a, g);
    korb_mp_mul(r, q, b);
    korb_mp_abs(r, r);
}
static inline void korb_mp_pow_ui(korb_mp_ptr r, korb_mp_srcptr a, unsigned long e) {
    uint64_t base = KORB_MPW_U(*a), acc = 1;
    while (e) { if (e & 1) acc *= base; base *= base; e >>= 1; }
    *r = KORB_MPW_S(acc);
}
static inline void korb_mp_ui_pow_ui(korb_mp_ptr r, unsigned long b, unsigned long e) {
    korb_mp_t bb; *bb = KORB_MPW_S(b); korb_mp_pow_ui(r, bb, e);
}
static inline void korb_mp_powm(korb_mp_ptr r, korb_mp_srcptr b, korb_mp_srcptr e, korb_mp_srcptr m) {
    if (*m == 0) { *r = 0; return; }
    const uint64_t mm = (*m < 0) ? (0u - KORB_MPW_U(*m)) : KORB_MPW_U(*m);
    uint64_t base = KORB_MPW_U(*b) % mm, acc = 1 % mm, ee = KORB_MPW_U(*e);
    while (ee) { if (ee & 1) acc = acc * base % mm; base = base * base % mm; ee >>= 1; }
    *r = KORB_MPW_S(acc);
}
static inline void korb_mp_sqrt(korb_mp_ptr r, korb_mp_srcptr a) {
    if (*a <= 0) { *r = 0; return; }
    uint64_t x = (uint64_t)sqrt((double)*a);
    while (x != 0 && x > (uint64_t)*a / x) x--;                  /* 下向きに補正 */
    while ((x + 1) <= (uint64_t)*a / (x + 1)) x++;               /* 上向きに補正 */
    *r = KORB_MPW_S(x);
}

static inline long   korb_mp_get_si(korb_mp_srcptr a) { return (long)*a; }
static inline unsigned long korb_mp_get_ui(korb_mp_srcptr a) { return (unsigned long)KORB_MPW_U(*a); }
static inline double korb_mp_get_d(korb_mp_srcptr a)  { return (double)*a; }
static inline double korb_mp_get_d_2exp(long *e, korb_mp_srcptr a) {
    if (*a == 0) { *e = 0; return 0.0; }
    int ex = 0;
    const double f = frexp((double)*a, &ex);
    *e = ex;
    return f;
}

/* limb は 64bit 1 本ぶんだけ */
static inline size_t korb_mp_size(korb_mp_srcptr a) { return *a == 0 ? 0 : 1; }
static inline korb_mp_limb_t korb_mp_getlimbn(korb_mp_srcptr a, korb_mp_size_t n) {
    if (n != 0) return 0;
    return (*a < 0) ? (0u - KORB_MPW_U(*a)) : KORB_MPW_U(*a);
}
static inline size_t korb_mp_sizeinbase(korb_mp_srcptr a, int base) {
    uint64_t v = (*a < 0) ? (0u - KORB_MPW_U(*a)) : KORB_MPW_U(*a);
    size_t n = 0;
    do { n++; v /= (uint64_t)base; } while (v);
    return n;
}

/* get_str: GMP と同じく buf==NULL なら malloc して返す (呼び出し側が free) */
static inline char *korb_mp_get_str(char *buf, int base, korb_mp_srcptr a) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[72];
    int n = 0;
    uint64_t v = (*a < 0) ? (0u - KORB_MPW_U(*a)) : KORB_MPW_U(*a);
    const int b = (base < 0) ? -base : base;
    do { tmp[n++] = digits[v % (uint64_t)b]; v /= (uint64_t)b; } while (v);
    const int neg = (*a < 0);
    if (buf == NULL) { buf = (char *)malloc((size_t)n + (size_t)neg + 1); if (!buf) return NULL; }
    char *p = buf;
    if (neg) *p++ = '-';
    while (n) *p++ = tmp[--n];
    *p = '\0';
    return buf;
}
static inline int korb_mp_init_set_str(korb_mp_ptr r, const char *s, int base) {
    char *end = NULL;
    *r = (int64_t)strtoll(s, &end, base);
    return (end && *end == '\0' && end != s) ? 0 : -1;
}

/* export/import: GMP と同じ引数だが 64bit ぶんだけ扱う。order/endian は
 * 呼び出し側 (random.c の seed) が使う LSW-first / native のみ意味を持つ。 */
static inline void *korb_mp_export(void *rop, size_t *countp, int order, size_t size,
                                   int endian, size_t nails, korb_mp_srcptr a) {
    (void)order; (void)endian; (void)nails;
    uint64_t v = (*a < 0) ? (0u - KORB_MPW_U(*a)) : KORB_MPW_U(*a);
    const size_t n = (v == 0) ? 0 : (8 + size - 1) / size;
    if (rop == NULL) { rop = malloc(n ? n * size : 1); if (!rop) return NULL; }
    memset(rop, 0, n ? n * size : 1);
    for (size_t i = 0; i < n; i++) memcpy((char *)rop + i * size, (const char *)&v + i * size, size);
    if (countp) *countp = n;
    return rop;
}
static inline void korb_mp_import(korb_mp_ptr r, size_t count, int order, size_t size,
                                  int endian, size_t nails, const void *op) {
    (void)order; (void)endian; (void)nails;
    uint64_t v = 0;
    const size_t n = (count * size > 8) ? (8 / size) : count;
    for (size_t i = 0; i < n; i++) memcpy((char *)&v + i * size, (const char *)op + i * size, size);
    *r = KORB_MPW_S(v);
}

/* gmp_fprintf の代わり。spec は "%<flags><width>Zd" の形しか来ないので、
 * Z を長さ修飾子 ll に置き換えて普通の fprintf に流す。 */
static inline int korb_mp_fprintf(FILE *f, const char *spec, korb_mp_srcptr a) {
    char buf[64];
    size_t j = 0;
    for (size_t i = 0; spec[i] && j + 3 < sizeof buf; i++) {
        if (spec[i] == 'Z') { buf[j++] = 'l'; buf[j++] = 'l'; continue; }
        buf[j++] = spec[i];
    }
    buf[j] = '\0';
    return fprintf(f, buf, (long long)*a);
}

/* 有理数。分子・分母とも 64bit で wrap する (Rational の内部計算用)。 */
struct korb_mq_s { int64_t n, d; };
typedef struct korb_mq_s        korb_mq_t[1];
typedef const struct korb_mq_s *korb_mq_srcptr;
typedef struct korb_mq_s       *korb_mq_ptr;

static inline void korb_mq_init(korb_mq_ptr q)  { q->n = 0; q->d = 1; }
static inline void korb_mq_clear(korb_mq_ptr q) { (void)q; }
static inline korb_mp_ptr korb_mq_numref(korb_mq_ptr q) { return &q->n; }
static inline korb_mp_ptr korb_mq_denref(korb_mq_ptr q) { return &q->d; }
static inline void korb_mq_set_num(korb_mq_ptr q, korb_mp_srcptr v) { q->n = *v; }
static inline void korb_mq_set_den(korb_mq_ptr q, korb_mp_srcptr v) { q->d = *v; }
static inline void korb_mq_set_z(korb_mq_ptr q, korb_mp_srcptr v)   { q->n = *v; q->d = 1; }
static inline void korb_mq_set_si(korb_mq_ptr q, long n, unsigned long d) { q->n = (int64_t)n; q->d = KORB_MPW_S(d); }
static inline int  korb_mq_sgn(korb_mq_srcptr q) { const int s = (q->n > 0) - (q->n < 0); return (q->d < 0) ? -s : s; }
static inline double korb_mq_get_d(korb_mq_srcptr q) { return (double)q->n / (double)q->d; }

static inline void korb_mq_canonicalize(korb_mq_ptr q) {
    if (q->d == 0) return;
    korb_mp_t g; korb_mp_gcd(g, &q->n, &q->d);
    if (*g > 1) { q->n /= *g; q->d /= *g; }
    if (q->d < 0) { q->n = KORB_MPW_S(0u - KORB_MPW_U(q->n)); q->d = KORB_MPW_S(0u - KORB_MPW_U(q->d)); }
}
static inline void korb_mq_set_d(korb_mq_ptr q, double d) {
    /* 2 の冪の分母で表す (double は 2 進なので有限桁で収まる) */
    int e = 0;
    double m = frexp(d, &e);
    for (int i = 0; i < 64 && m != floor(m); i++) { m *= 2.0; e--; }
    q->n = (int64_t)m; q->d = 1;
    if (e > 0)      korb_mp_mul_2exp(&q->n, &q->n, (korb_mp_bitcnt_t)e);
    else if (e < 0) korb_mp_mul_2exp(&q->d, &q->d, (korb_mp_bitcnt_t)(-e));
    korb_mq_canonicalize(q);
}
static inline int korb_mq_set_str(korb_mq_ptr q, const char *s, int base) {
    const char *slash = strchr(s, '/');
    char *end = NULL;
    q->n = (int64_t)strtoll(s, &end, base);
    if (!end || end == s) return -1;
    q->d = slash ? (int64_t)strtoll(slash + 1, &end, base) : 1;
    return (end && *end == '\0') ? 0 : -1;
}
static inline void korb_mq_add(korb_mq_ptr r, korb_mq_srcptr a, korb_mq_srcptr b) {
    const int64_t n = KORB_MPW_S(KORB_MPW_U(a->n) * KORB_MPW_U(b->d) + KORB_MPW_U(b->n) * KORB_MPW_U(a->d));
    const int64_t d = KORB_MPW_S(KORB_MPW_U(a->d) * KORB_MPW_U(b->d));
    r->n = n; r->d = d;
    korb_mq_canonicalize(r);
}
static inline void korb_mq_div(korb_mq_ptr r, korb_mq_srcptr a, korb_mq_srcptr b) {
    const int64_t n = KORB_MPW_S(KORB_MPW_U(a->n) * KORB_MPW_U(b->d));
    const int64_t d = KORB_MPW_S(KORB_MPW_U(a->d) * KORB_MPW_U(b->n));
    r->n = n; r->d = d;
    korb_mq_canonicalize(r);
}
static inline void korb_mq_div_2exp(korb_mq_ptr r, korb_mq_srcptr a, korb_mp_bitcnt_t e) {
    r->n = a->n; r->d = a->d;
    korb_mp_mul_2exp(&r->d, &r->d, e);
    korb_mq_canonicalize(r);
}
static inline int korb_mq_cmp(korb_mq_srcptr a, korb_mq_srcptr b) {
    const int64_t l = KORB_MPW_S(KORB_MPW_U(a->n) * KORB_MPW_U(b->d));
    const int64_t r = KORB_MPW_S(KORB_MPW_U(b->n) * KORB_MPW_U(a->d));
    return (l > r) - (l < r);
}

static inline void korb_mp_strfree(char *s, size_t n) { (void)n; free(s); }

/* GC フック: 外部 malloc は無いので何もしない */
static inline size_t korb_mp_extbytes(korb_mp_srcptr a) { (void)a; return 0; }
static inline void   korb_mp_free(korb_mp_ptr a)        { (void)a; }

#endif
