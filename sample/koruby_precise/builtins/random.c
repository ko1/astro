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
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(seed)) {
        mpz_t z; mpz_init(z); mpz_abs(z, VAL2BIG(seed)->z);
        size_t count = 0;
        uint32_t *w = (uint32_t *)mpz_export(NULL, &count, -1 /*LSW first*/, sizeof(uint32_t), 0, 0, z);
        mpz_clear(z);
        if (count == 0) korb_mt_init_genrand(st, 0);
        else if (count == 1) korb_mt_init_genrand(st, w[0]);
        else korb_mt_init_by_array(st, w, (int)count);
        free(w);
        return;
    }
#endif
    korb_mt_init_genrand(st, 0);
}
/* non-deterministic seed for Random.new (no arg) / first Kernel#rand. */
static uint32_t korb_mt_entropy(const void *salt) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_nsec) ^ (uint32_t)(ts.tv_sec * 2654435761u) ^ (uint32_t)(uintptr_t)salt;
}

/* @__mt ivar holds a binary String of one KorbMT. */
/* ivar name as a Symbol VALUE (korb_ivar_get/set take ID2SYM(id), not a raw id). */
static VALUE korb_mt_sym(struct korb_vm *const vm)   { return ID2SYM(korb_intern(vm, "__mt", 4)); }
static VALUE korb_seed_sym(struct korb_vm *const vm) { return ID2SYM(korb_intern(vm, "__seed", 6)); }

static KorbMT *korb_default_rng(struct korb_vm *vm);   /* fwd */
static KorbMT *korb_rng_of(CTX *const c, VALUE rndobj) {
    const VALUE s = korb_ivar_get(c, rndobj, korb_mt_sym(c->vm));
    return KORB_STRING_P(s) ? (KorbMT *)VAL2STR(s)->buf->data : NULL;
}
/* True if v is a Random instance (its class is, or derives from, Random). */
static bool korb_is_random(CTX *const c, VALUE v) {
    if (!KORB_OBJECT_P(v)) return false;
    VALUE cls = korb_class_obj_of(c, v);
    const VALUE rc = korb_builtin_class_obj(c->vm, KORB_C_RANDOM);
    while (KORB_CLASS_P(cls)) { if (cls == rc) return true; cls = VAL2CLASS(cls)->superclass; }
    return false;
}
/* Pick the generator for shuffle/sample: a trailing `random:` kwarg Random, else
 * the per-vm default.  Call AFTER any allocation (the returned KorbMT* points
 * into a Random's @__mt String buffer, which a later GC could move). */
static KorbMT *korb_rng_from_kwargs(CTX *const c, VALUE_SLICE a) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    if (n >= 1) {
        const VALUE last = VALUE_SLICE_GET(a, n - 1);
        if (KORB_HASH_P(last)) {
            const int32_t hi = korb_hash_find(VAL2HASH(last), ID2SYM(korb_intern(c->vm, "random", 6)));
            if (hi >= 0) {
                const VALUE rng = VAL2HASH(last)->items->data[2 * hi + 1];
                if (korb_is_random(c, rng)) { KorbMT *st = korb_rng_of(c, rng); if (st) return st; }
            }
        }
    }
    return korb_default_rng(c->vm);
}

/* core rand: st is the generator, args is [] | [n] | [range].  Returns a Float
 * in [0,1) for no arg / 0 / Float, an Integer in [0,n) for positive Integer n,
 * or a value within a Range. */
static RESULT korb_rand_core(CTX *c, VALUE *slots, KorbMT *st, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) == 0 || VALUE_SLICE_GET(a, 0) == KORB_NIL)
        return korb_float_new(c, slots, korb_mt_real(st));
    const VALUE arg = VALUE_SLICE_GET(a, 0);
    if (FIXNUM_P(arg)) {
        intptr_t n = FIX2LONG(arg);
        if (n == 0) return korb_float_new(c, slots, korb_mt_real(st));
        if (n < 0)  return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid argument - %ld", (long)n);
        if ((uintptr_t)n <= 0xFFFFFFFFu) return RESULT_OK(LONG2FIX((intptr_t)korb_mt_limited(st, (uint32_t)(n - 1))));
        /* n > 2^32: draw two words */
        uint64_t lim = (uint64_t)n - 1, mask = lim; mask|=mask>>1;mask|=mask>>2;mask|=mask>>4;mask|=mask>>8;mask|=mask>>16;mask|=mask>>32;
        uint64_t v; do { v = ((uint64_t)korb_mt_next(st) | ((uint64_t)korb_mt_next(st) << 32)) & mask; } while (v > lim);
        return RESULT_OK(LONG2FIX((intptr_t)v));
    }
    double f;
    if (korb_num_to_d(arg, &f)) {
        if (f <= 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid argument");
        return korb_float_new(c, slots, korb_mt_real(st) * f);
    }
    return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid argument");
}

/* Random#initialize(seed = nil). */
static RESULT korb_m_random_init(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE seed = (VALUE_SLICE_LEN(a) >= 1) ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    if (UNLIKELY(seed != KORB_NIL && !FIXNUM_P(seed) && !KORB_BIGNUM_P(seed)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(seed));
    CHECK(korb_ivar_set(c, slots, self, korb_seed_sym(c->vm), seed));   /* @__seed (may GC; seed re-read below) */
    KorbString *const s = korb_str_alloc(c, slots, (uint32_t)sizeof(KorbMT));   /* binary state buffer (may GC) */
    slots[0] = (VALUE)s;                                                /* root */
    KorbMT *const st = (KorbMT *)s->buf->data;
    const VALUE seed2 = korb_ivar_get(c, VALUE_REF_GET(self), korb_seed_sym(c->vm));   /* re-read post-GC */
    if (seed2 == KORB_NIL) korb_mt_init_genrand(st, korb_mt_entropy(st));
    else                   korb_mt_seed_int(st, seed2);
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
