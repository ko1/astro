/* koruby_precise — random.c: CRuby-compatible MT19937 PRNG + Random class.
 * #included into korb_runtime.c's TU.  The per-instance MT state lives in a
 * binary String ivar (@__mt); the Kernel#rand default generator lives inline in
 * the vm (no GC edges).  Bit-exact with CRuby: single-word seeds use
 * init_genrand, multi-word use init_by_array, rand(n) uses mask+reject. */
#include <time.h>

#define KORB_MT_N 624
#define KORB_MT_M 397
/* KorbMT is defined in context.h (so the vm can embed the default generator). */

static void korb_mt_init_genrand(KorbMT *const st, uint32_t s) {
    st->mt[0] = s;
    for (st->mti = 1; st->mti < KORB_MT_N; st->mti++)
        st->mt[st->mti] = 1812433253UL * (st->mt[st->mti - 1] ^ (st->mt[st->mti - 1] >> 30)) + st->mti;
}
static void korb_mt_init_by_array(KorbMT *const st, const uint32_t *const key, int klen) {
    korb_mt_init_genrand(st, 19650218UL);
    int i = 1, j = 0, k = (KORB_MT_N > klen ? KORB_MT_N : klen);
    for (; k; k--) {
        st->mt[i] = (st->mt[i] ^ ((st->mt[i-1] ^ (st->mt[i-1] >> 30)) * 1664525UL)) + key[j] + (uint32_t)j;
        if (++i >= KORB_MT_N) { st->mt[0] = st->mt[KORB_MT_N-1]; i = 1; }
        if (++j >= klen) j = 0;
    }
    for (k = KORB_MT_N - 1; k; k--) {
        st->mt[i] = (st->mt[i] ^ ((st->mt[i-1] ^ (st->mt[i-1] >> 30)) * 1566083941UL)) - (uint32_t)i;
        if (++i >= KORB_MT_N) { st->mt[0] = st->mt[KORB_MT_N-1]; i = 1; }
    }
    st->mt[0] = 0x80000000UL;
}
static uint32_t korb_mt_next(KorbMT *const st) {
    static const uint32_t mag01[2] = { 0, 0x9908b0dfUL };
    uint32_t y;
    if (st->mti >= KORB_MT_N) {
        int kk;
        for (kk = 0; kk < KORB_MT_N - KORB_MT_M; kk++) {
            y = (st->mt[kk] & 0x80000000UL) | (st->mt[kk+1] & 0x7fffffffUL);
            st->mt[kk] = st->mt[kk + KORB_MT_M] ^ (y >> 1) ^ mag01[y & 1];
        }
        for (; kk < KORB_MT_N - 1; kk++) {
            y = (st->mt[kk] & 0x80000000UL) | (st->mt[kk+1] & 0x7fffffffUL);
            st->mt[kk] = st->mt[kk + (KORB_MT_M - KORB_MT_N)] ^ (y >> 1) ^ mag01[y & 1];
        }
        y = (st->mt[KORB_MT_N-1] & 0x80000000UL) | (st->mt[0] & 0x7fffffffUL);
        st->mt[KORB_MT_N-1] = st->mt[KORB_MT_M-1] ^ (y >> 1) ^ mag01[y & 1];
        st->mti = 0;
    }
    y = st->mt[st->mti++];
    y ^= y >> 11; y ^= (y << 7) & 0x9d2c5680UL; y ^= (y << 15) & 0xefc60000UL; y ^= y >> 18;
    return y;
}
static uint32_t korb_mt_make_mask(uint32_t x) { x|=x>>1; x|=x>>2; x|=x>>4; x|=x>>8; x|=x>>16; return x; }
/* uniform in [0, limit] (inclusive), CRuby limited_rand for a single 32-bit word. */
static uint32_t korb_mt_limited(KorbMT *const st, uint32_t limit) {
    if (limit == 0) return 0;
    const uint32_t mask = korb_mt_make_mask(limit);
    uint32_t v;
    do { v = korb_mt_next(st) & mask; } while (v > limit);
    return v;
}
/* real in [0,1) from two draws (CRuby int_pair_to_real_exclusive / genrand_res53). */
static double korb_mt_real(KorbMT *const st) {
    const uint32_t a = korb_mt_next(st) >> 5, b = korb_mt_next(st) >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

/* seed an MT from a non-negative Integer (Fixnum or Bignum), CRuby rand_init. */
static void korb_mt_seed_int(KorbMT *const st, VALUE seed) {
    if (FIXNUM_P(seed)) {
        intptr_t s = FIX2LONG(seed);
        uint64_t a = (s < 0) ? (~(uint64_t)s + 1u) : (uint64_t)s;   /* abs (two's complement) */
        if (a <= 0xFFFFFFFFULL) korb_mt_init_genrand(st, (uint32_t)a);
        else { uint32_t key[2] = { (uint32_t)(a & 0xFFFFFFFFu), (uint32_t)(a >> 32) }; korb_mt_init_by_array(st, key, 2); }
        return;
    }
    if (KORB_BIGNUM_P(seed)) {
        korb_mp_t z; korb_mp_init(z); korb_mp_abs(z, VAL2BIG(seed)->z);
        size_t count = 0;
        uint32_t *w = (uint32_t *)korb_mp_export(NULL, &count, -1 /*LSW first*/, sizeof(uint32_t), 0, 0, z);
        korb_mp_clear(z);
        if (count == 0) korb_mt_init_genrand(st, 0);
        else if (count == 1) korb_mt_init_genrand(st, w[0]);
        else korb_mt_init_by_array(st, w, (int)count);
        free(w);
        return;
    }
    korb_mt_init_genrand(st, 0);
}
/* non-deterministic seed for Random.new (no arg) / first Kernel#rand. */
static uint32_t korb_mt_entropy(const void *salt) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_nsec) ^ (uint32_t)(ts.tv_sec * 2654435761u) ^ (uint32_t)(uintptr_t)salt;
}

/* @__mt ivar holds a binary String of one KorbMT. */
/* ivar name as a Symbol VALUE (korb_ivar_get/set take ID2SYM(id), not a raw id). */
static VALUE korb_mt_sym(struct korb_vm *const vm)   { return ID2SYM(korb_intern(vm, "@__mt", 5)); }
static VALUE korb_seed_sym(struct korb_vm *const vm) { return ID2SYM(korb_intern(vm, "@__seed", 7)); }

static KorbMT *korb_default_rng(struct korb_vm *vm);   /* fwd */
ARO_BORROW static KorbMT *korb_rng_of(CTX *const c, VALUE rndobj) {
    const VALUE s = korb_ivar_get(c, rndobj, korb_mt_sym(c->vm));
    return KORB_STRING_P(s) ? (KorbMT *)korb_strbuf_data(VAL2STR(s)->buf) : NULL;
}
/* True if v is a Random instance (its class is, or derives from, Random). */
/* Random.urandom(n) → n bytes of OS entropy as an ASCII-8BIT String. */
static RESULT korb_m_random_urandom(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    intptr_t n = 0;
    if (VALUE_SLICE_LEN(a) < 1 || UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative string size (or size too big)");
    char *buf = (char *)malloc((size_t)(n ? n : 1));
    if (!buf) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "out of memory");
    const int fd = open("/dev/urandom", O_RDONLY);
    const size_t got = fd >= 0 ? korb_fd_read_n(fd, buf, (size_t)n) : 0;
    if (fd >= 0) close(fd);
    for (size_t i = got; i < (size_t)n; i++) buf[i] = 0;   /* pad if the read fell short */
    RESULT r = korb_str_new(c, slots, buf, (uint32_t)n);
    free(buf);
    if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
    return r;
}
static bool korb_is_random(CTX *const c, VALUE v) {
    if (!KORB_OBJECT_P(v)) return false;
    VALUE cls = korb_class_obj_of(c, v);
    const VALUE rc = korb_builtin_class_obj(c->vm, KORB_C_RANDOM);
    while (KORB_CLASS_P(cls)) { if (cls == rc) return true; cls = VAL2CLASS(cls)->superclass; }
    return false;
}

/* A random source for shuffle/sample: an MT generator, or a user object passed
 * via `random:` whose #rand(n) is dispatched. */
typedef struct { KorbMT *mt; VALUE obj; } KorbRandSrc;
static KorbRandSrc korb_rand_src_from_kwargs(CTX *const c, VALUE_SLICE a) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    if (n >= 1) {
        const VALUE last = VALUE_SLICE_GET(a, n - 1);
        if (KORB_HASH_P(last)) {
            const int32_t hi = korb_hash_find(VAL2HASH(last), ID2SYM(korb_intern(c->vm, "random", 6)));
            if (hi >= 0) {
                const VALUE rng = korb_items_data(VAL2HASH(last)->items)[2 * hi + 1];
                if (korb_is_random(c, rng)) { KorbMT *st = korb_rng_of(c, rng); if (st) { KorbRandSrc s = { st, KORB_NIL }; return s; } }
                else if (KORB_OBJECT_P(rng)) { KorbRandSrc s = { NULL, rng }; return s; }   /* custom RNG → dispatch #rand */
            }
        }
    }
    KorbRandSrc s = { korb_default_rng(c->vm), KORB_NIL };
    return s;
}
/* Draw an integer in [0, bound] inclusive.  Re-resolves the source from `a` each
 * call (whose kwargs Hash lives in the caller's rooted slots, so it stays valid
 * across the #rand dispatch's GC); the MT path allocates nothing. */
static RESULT korb_rand_upto(CTX *const c, VALUE *slots, VALUE_SLICE a, uint32_t bound, uint32_t *out) {
    KorbRandSrc src = korb_rand_src_from_kwargs(c, a);
    if (src.obj == KORB_NIL) { *out = korb_mt_limited(src.mt, bound); return RESULT_OK(KORB_NIL); }
    slots[0] = src.obj; slots[1] = LONG2FIX((intptr_t)bound + 1);   /* rng.rand(bound+1) → [0, bound] */
    RESULT r = korb_send(c, slots + 2, korb_intern(c->vm, "rand", 4), 0, 1);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    intptr_t v;
    if (UNLIKELY(!korb_to_index(r.value, &v)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(r.value));
    if (UNLIKELY(v < 0 || (uintptr_t)v > bound))
        return korb_raise(c, slots, KORB_E_RANGE, 0, "random number too big %ld", (long)v);
    *out = (uint32_t)v;
    return RESULT_OK(KORB_NIL);
}

/* core rand: st is the generator, args is [] | [n] | [range].  Returns a Float
 * in [0,1) for no arg / 0 / Float, an Integer in [0,n) for positive Integer n,
 * or a value within a Range. */
/* draw an integer in [0, n) for n >= 1 (n may exceed 2^32). */
static intptr_t korb_rand_below(KorbMT *st, uintptr_t n) {
    if (n <= 0xFFFFFFFFu) return (intptr_t)korb_mt_limited(st, (uint32_t)(n - 1));
    uint64_t lim = (uint64_t)n - 1, mask = lim;
    mask |= mask>>1; mask |= mask>>2; mask |= mask>>4; mask |= mask>>8; mask |= mask>>16; mask |= mask>>32;
    uint64_t v; do { v = ((uint64_t)korb_mt_next(st) | ((uint64_t)korb_mt_next(st) << 32)) & mask; } while (v > lim);
    return (intptr_t)v;
}
static RESULT korb_rand_core(CTX *c, VALUE *slots, KorbMT *st, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) == 0 || VALUE_SLICE_GET(a, 0) == KORB_NIL)
        return korb_float_new(c, slots, korb_mt_real(st));
    VALUE arg = VALUE_SLICE_GET(a, 0);
    if (KORB_RANGE_P(arg)) {                          /* rand(a..b) / rand(a...b) */
        const KorbRange *const r = VAL2RANGE(arg);
        const VALUE lo = r->rbegin, hi = r->rend;
        const bool excl = r->exclude_end;
        if (FIXNUM_P(lo) && FIXNUM_P(hi)) {           /* integer range → Integer */
            const intptr_t lv = FIX2LONG(lo), hv = FIX2LONG(hi);
            const intptr_t span = hv - lv + (excl ? 0 : 1);
            if (span <= 0) return RESULT_OK(KORB_NIL);
            return RESULT_OK(LONG2FIX(lv + korb_rand_below(st, (uintptr_t)span)));
        }
        double dlo, dhi;                              /* otherwise a Float in [lo, hi) */
        if (korb_num_to_d(lo, &dlo) && korb_num_to_d(hi, &dhi)) {
            if (dhi < dlo || (dhi == dlo && excl)) return RESULT_OK(KORB_NIL);
            return korb_float_new(c, slots, dlo + korb_mt_real(st) * (dhi - dlo));
        }
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "bad value for range");
    }
    if (!FIXNUM_P(arg) && !KORB_FLOAT_P(arg) && KORB_OBJECT_P(arg) &&    /* coerce via #to_int */
        korb_responds_to_coerce_p(c, slots, &arg, korb_intern(c->vm, "to_int", 6))) {
        slots[0] = arg;
        RESULT ir = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_int", 6), 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
        arg = ir.value;
    }
    if (FIXNUM_P(arg)) {
        intptr_t n = FIX2LONG(arg);
        if (n == 0) return korb_float_new(c, slots, korb_mt_real(st));
        if (n < 0)  n = -n;                           /* the sign is ignored */
        return RESULT_OK(LONG2FIX(korb_rand_below(st, (uintptr_t)n)));
    }
    double f;
    if (korb_num_to_d(arg, &f)) {
        if (f < 0) f = -f;                            /* the sign is ignored */
        if (f == 0) return korb_float_new(c, slots, korb_mt_real(st));
        return korb_float_new(c, slots, korb_mt_real(st) * f);
    }
    return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid argument");
}

/* Random#initialize(seed = nil). */
static RESULT korb_m_random_init(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE seed = (VALUE_SLICE_LEN(a) >= 1) ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    if (seed != KORB_NIL && !FIXNUM_P(seed) && !KORB_BIGNUM_P(seed)) {
        /* any Numeric seed is truncated to an Integer (Float/Rational/Complex);
         * anything else goes through #to_int */
        slots[0] = seed;
        const char *const on = korb_coerce_name(c, slots[0]);
        const uint32_t mid = (KORB_FLOAT_P(slots[0]) || KORB_RATIONAL_P(slots[0]) || KORB_COMPLEX_P(slots[0])) ? korb_intern(c->vm, "to_i", 4) : korb_intern(c->vm, "to_int", 6);
        if (UNLIKELY(!korb_responds_to(c, slots[0], mid)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", on);
        const RESULT ir = korb_send(c, slots + 1, mid, 0, 0);
        if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
        if (UNLIKELY(!FIXNUM_P(ir.value) && !KORB_BIGNUM_P(ir.value)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", on);
        seed = ir.value;
    }
    if (seed == KORB_NIL) seed = LONG2FIX((intptr_t)(korb_mt_entropy(slots) & 0x3FFFFFFF));   /* #seed reports the drawn value */
    CHECK(korb_ivar_set(c, slots, self, korb_seed_sym(c->vm), seed));   /* @__seed (may GC; seed re-read below) */
    KorbString *const s = korb_str_alloc(c, slots, (uint32_t)sizeof(KorbMT));   /* binary state buffer (may GC) */
    slots[0] = (VALUE)s;                                                /* root */
    KorbMT *const st = (KorbMT *)korb_strbuf_data(s->buf);
    const VALUE seed2 = korb_ivar_get(c, VALUE_REF_GET(self), korb_seed_sym(c->vm));   /* re-read post-GC */
    korb_mt_seed_int(st, seed2);
    CHECK(korb_ivar_set(c, slots + 1, self, korb_mt_sym(c->vm), slots[0]));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_random_rand(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbMT *const st = korb_rng_of(c, VALUE_REF_GET(self));
    if (UNLIKELY(st == NULL)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "uninitialized Random");
    return korb_rand_core(c, slots, st, a);
}
static RESULT korb_m_random_seed(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    return RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), korb_seed_sym(c->vm)));
}

/* Kernel#rand / srand use a per-vm default generator (no GC edges). */
static KorbMT *korb_default_rng(struct korb_vm *const vm) {
    if (!vm->default_rng_seeded) { korb_mt_init_genrand(&vm->default_rng, korb_mt_entropy(&vm->default_rng)); vm->default_rng_seeded = true; }
    return &vm->default_rng;
}
static RESULT korb_bi_rand(CTX *c, VALUE *slots, VALUE_SLICE a) {
    return korb_rand_core(c, slots, korb_default_rng(c->vm), a);
}
static RESULT korb_bi_srand(CTX *c, VALUE *slots, VALUE_SLICE a) {
    const VALUE old = c->vm->default_rng_seeded ? c->vm->default_rng_seed : LONG2FIX(0);
    VALUE seed = (VALUE_SLICE_LEN(a) >= 1) ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    if (seed == KORB_NIL) korb_mt_init_genrand(&c->vm->default_rng, korb_mt_entropy(&c->vm->default_rng));
    else if (FIXNUM_P(seed) || KORB_BIGNUM_P(seed)) korb_mt_seed_int(&c->vm->default_rng, seed);
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    c->vm->default_rng_seeded = true;
    c->vm->default_rng_seed = seed;
    return RESULT_OK(old);
}
