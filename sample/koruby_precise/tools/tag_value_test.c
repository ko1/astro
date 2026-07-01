#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef intptr_t VALUE;

/* ---- copied verbatim from the new context.h ---- */
#define KORB_NIL       ((VALUE)0)
#define KORB_SPECIAL(n)    ((VALUE)(((uintptr_t)(n) << 4) | 0x4u))
#define KORB_SPECIAL_P(v)  (((uintptr_t)(v) & 0xFu) == 0x4u)
#define KORB_FALSE     KORB_SPECIAL(0)
#define KORB_TRUE      KORB_SPECIAL(1)
#define KORB_UNDEF     KORB_SPECIAL(2)
#define FIXNUM_P(v)    (((uintptr_t)(v) & 1u) != 0)
#define LONG2FIX(i)    ((VALUE)(((uintptr_t)(intptr_t)(i) << 1) | 1u))
#define FIX2LONG(v)    (((intptr_t)(v)) >> 1)
#define SYMBOL_P(v)    (((uintptr_t)(v) & 0xFu) == 0xCu)
#define ID2SYM(id)     ((VALUE)(((uintptr_t)(id) << 4) | 0xCu))
#define SYM2ID(v)      ((uint32_t)((uintptr_t)(v) >> 4))
#define FLONUM_P(v)    (((uintptr_t)(v) & 3u) == 2u)
#define KORB_FLO_ZERO  ((VALUE)(intptr_t)0x8000000000000002ULL)
static inline VALUE korb_d2flo(double d) {
    union { double d; uintptr_t v; } t; t.d = d;
    if (t.v == 0u) return KORB_FLO_ZERO;
    unsigned top3 = (unsigned)((t.v >> 60) & 7u);
    if (top3 != 3u && top3 != 4u) return 0;
    uintptr_t e = ((t.v << 3 | t.v >> 61) & ~(uintptr_t)1u) | 2u;
    if (e == (uintptr_t)KORB_FLO_ZERO) return 0;
    return (VALUE)e;
}
static inline double korb_flo2d(VALUE fv) {
    uintptr_t v = (uintptr_t)fv;
    if (v == (uintptr_t)KORB_FLO_ZERO) return 0.0;
    uintptr_t b63 = (uintptr_t)v >> 63;
    union { double d; uintptr_t v; } t;
    uintptr_t x = (2u - b63) | ((uintptr_t)v & ~(uintptr_t)3u);
    t.v = (x >> 3) | (x << 61);
    return t.d;
}
#define KORB_TRUTHY(v)   (((uintptr_t)(v) & ~(uintptr_t)4u) != 0)
#define AROH_IS_GC_OBJECT(v)  ((v) != 0 && ((uintptr_t)(v) & 7u) == 0)

static int fails = 0;
#define CK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while(0)

/* every value must classify into exactly one category */
static void classify_check(VALUE v, const char *what) {
    int n = (v==KORB_NIL) + KORB_SPECIAL_P(v) + FIXNUM_P(v) + SYMBOL_P(v) + FLONUM_P(v) + AROH_IS_GC_OBJECT(v);
    if (n != 1) { printf("FAIL: %s (v=%llx) classified into %d categories\n", what, (unsigned long long)v, n); fails++; }
}

int main(void) {
    /* singletons distinct + correctly tagged, not heap, not each-other */
    CK(KORB_FALSE == 4 && KORB_TRUE == 20 && KORB_UNDEF == 36, "special values");
    CK(KORB_SPECIAL_P(KORB_FALSE) && KORB_SPECIAL_P(KORB_TRUE) && KORB_SPECIAL_P(KORB_UNDEF), "special_p");
    CK(!AROH_IS_GC_OBJECT(KORB_NIL) && !AROH_IS_GC_OBJECT(KORB_FALSE) && !AROH_IS_GC_OBJECT(KORB_TRUE) && !AROH_IS_GC_OBJECT(KORB_UNDEF), "specials not heap");
    CK(!FLONUM_P(KORB_FALSE) && !FLONUM_P(KORB_TRUE) && !FLONUM_P(KORB_UNDEF) && !FLONUM_P(KORB_NIL), "specials not flonum");
    CK(!SYMBOL_P(KORB_FALSE) && !SYMBOL_P(KORB_TRUE), "specials not symbol");
    classify_check(KORB_FALSE, "false"); classify_check(KORB_TRUE, "true"); classify_check(KORB_UNDEF, "undef");
    /* nil is its own thing (category count 0 is allowed only for nil) */
    CK(!KORB_SPECIAL_P(KORB_NIL) && !FIXNUM_P(KORB_NIL) && !SYMBOL_P(KORB_NIL) && !FLONUM_P(KORB_NIL) && !AROH_IS_GC_OBJECT(KORB_NIL), "nil is nil");

    /* truthy */
    CK(!KORB_TRUTHY(KORB_NIL) && !KORB_TRUTHY(KORB_FALSE), "nil/false falsy");
    CK(KORB_TRUTHY(KORB_TRUE) && KORB_TRUTHY(KORB_UNDEF) && KORB_TRUTHY(LONG2FIX(0)) && KORB_TRUTHY(LONG2FIX(1)), "others truthy");

    /* fixnums round-trip + classify */
    long fixv[] = {0,1,-1,2,-2,42,-42,1000000,-1000000,(long)1<<40,-((long)1<<40)};
    for (unsigned i=0;i<sizeof(fixv)/sizeof(fixv[0]);i++){
        VALUE f = LONG2FIX(fixv[i]);
        CK(FIXNUM_P(f) && FIX2LONG(f)==fixv[i], "fixnum roundtrip");
        classify_check(f, "fixnum");
    }
    /* symbols round-trip + classify */
    for (uint32_t id=0; id<100000; id++){
        VALUE s = ID2SYM(id);
        CK(SYMBOL_P(s) && SYM2ID(s)==id, "symbol roundtrip");
        if (id<50) classify_check(s, "symbol");
    }

    /* flonum round-trip over a broad sweep + classify + magic-collision */
    long reps=0, boxed=0;
    /* structured: sign x exponent x mantissa patterns */
    for (int sign=0; sign<2; sign++){
      for (int exp=-260; exp<=260; exp++){
        double mants[] = {1.0, 1.5, 1.25, 1.3333333, 1.9999999, 1.0000001, 1.7320508};
        for (unsigned mi=0; mi<sizeof(mants)/sizeof(mants[0]); mi++){
            double d = ldexp(mants[mi], exp);
            if (sign) d = -d;
            VALUE fv = korb_d2flo(d);
            if (fv == 0) { boxed++; continue; }             /* heap-boxed: fine */
            reps++;
            CK(FLONUM_P(fv), "flonum tagged");
            classify_check(fv, "flonum");
            double back = korb_flo2d(fv);
            if (back != d) { printf("FAIL: flonum roundtrip d=%.17g back=%.17g fv=%llx\n", d, back, (unsigned long long)fv); fails++; }
        }
      }
    }
    /* specific tricky values */
    double tricky[] = {0.0, 1.0, -1.0, 2.0, -2.0, 0.5, -0.5, 3.14159265358979, 1e-100, 1e100,
                       ldexp(1.0,-255), -ldexp(1.0,-255), ldexp(1.0,255), M_PI, M_E, 100.0, -100.0};
    for (unsigned i=0;i<sizeof(tricky)/sizeof(tricky[0]);i++){
        VALUE fv = korb_d2flo(tricky[i]);
        if (fv==0){ boxed++; continue; }
        double back = korb_flo2d(fv);
        if (back != tricky[i] && !(tricky[i]==0.0 && back==0.0)) { printf("FAIL: tricky d=%.17g back=%.17g\n", tricky[i], back); fails++; }
    }
    CK(korb_d2flo(0.0)==KORB_FLO_ZERO && korb_flo2d(KORB_FLO_ZERO)==0.0, "+0.0 magic");

    printf("flonum: %ld representable, %ld heap-boxed\n", reps, boxed);
    printf(fails? "==> %d FAILURES\n" : "==> ALL PASS\n", fails);
    return fails ? 1 : 0;
}
