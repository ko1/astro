/* koruby_precise — bignum backend: GMP.
 *
 * The whole backend is a rename: korb_mp_* IS mpz_*, so this build is exactly
 * what koruby did before the seam existed.  Semantics: real arbitrary
 * precision, i.e. Ruby's.  See bignum_backend.h for the backend contract. */
#ifndef KORB_BIGNUM_GMP_H
#define KORB_BIGNUM_GMP_H

#include <gmp.h>

typedef mpz_t       korb_mp_t;
typedef mpz_srcptr  korb_mp_srcptr;
typedef mpz_ptr     korb_mp_ptr;
typedef mp_limb_t   korb_mp_limb_t;
typedef mp_size_t   korb_mp_size_t;
typedef mp_bitcnt_t korb_mp_bitcnt_t;

#define korb_mp_abs          mpz_abs
#define korb_mp_add          mpz_add
#define korb_mp_add_ui       mpz_add_ui
#define korb_mp_and          mpz_and
#define korb_mp_cdiv_q       mpz_cdiv_q
#define korb_mp_cdiv_qr      mpz_cdiv_qr
#define korb_mp_clear        mpz_clear
#define korb_mp_cmp          mpz_cmp
#define korb_mp_cmp_ui       mpz_cmp_ui
#define korb_mp_cmpabs_ui    mpz_cmpabs_ui
#define korb_mp_com          mpz_com
#define korb_mp_divexact     mpz_divexact
#define korb_mp_even_p       mpz_even_p
#define korb_mp_export       mpz_export
#define korb_mp_fdiv_q       mpz_fdiv_q
#define korb_mp_fdiv_q_2exp  mpz_fdiv_q_2exp
#define korb_mp_fdiv_q_ui    mpz_fdiv_q_ui
#define korb_mp_fdiv_qr      mpz_fdiv_qr
#define korb_mp_fdiv_r       mpz_fdiv_r
#define korb_mp_fits_slong_p mpz_fits_slong_p
#define korb_mp_fits_ulong_p mpz_fits_ulong_p
#define korb_mp_gcd          mpz_gcd
#define korb_mp_get_d        mpz_get_d
#define korb_mp_get_d_2exp   mpz_get_d_2exp
#define korb_mp_get_si       mpz_get_si
#define korb_mp_get_str      mpz_get_str
#define korb_mp_get_ui       mpz_get_ui
#define korb_mp_getlimbn     mpz_getlimbn
#define korb_mp_import       mpz_import
#define korb_mp_init         mpz_init
#define korb_mp_init_set     mpz_init_set
#define korb_mp_init_set_d   mpz_init_set_d
#define korb_mp_init_set_si  mpz_init_set_si
#define korb_mp_init_set_str mpz_init_set_str
#define korb_mp_init_set_ui  mpz_init_set_ui
#define korb_mp_ior          mpz_ior
#define korb_mp_lcm          mpz_lcm
#define korb_mp_mul          mpz_mul
#define korb_mp_mul_2exp     mpz_mul_2exp
#define korb_mp_mul_ui       mpz_mul_ui
#define korb_mp_neg          mpz_neg
#define korb_mp_odd_p        mpz_odd_p
#define korb_mp_pow_ui       mpz_pow_ui
#define korb_mp_powm         mpz_powm
#define korb_mp_set_d        mpz_set_d
#define korb_mp_set_ui       mpz_set_ui
#define korb_mp_sgn          mpz_sgn
#define korb_mp_size         mpz_size
#define korb_mp_sizeinbase   mpz_sizeinbase
#define korb_mp_sqrt         mpz_sqrt
#define korb_mp_sub          mpz_sub
#define korb_mp_sub_ui       mpz_sub_ui
#define korb_mp_swap         mpz_swap
#define korb_mp_tdiv_qr      mpz_tdiv_qr
#define korb_mp_ui_pow_ui    mpz_ui_pow_ui
#define korb_mp_xor          mpz_xor

#define korb_mp_fprintf      gmp_fprintf

/* 有理数 (Rational の内部計算で使う)。 */
typedef mpq_t      korb_mq_t;
typedef mpq_srcptr korb_mq_srcptr;
typedef mpq_ptr    korb_mq_ptr;
#define korb_mq_add            mpq_add
#define korb_mq_canonicalize   mpq_canonicalize
#define korb_mq_clear          mpq_clear
#define korb_mq_cmp            mpq_cmp
#define korb_mq_denref         mpq_denref
#define korb_mq_div            mpq_div
#define korb_mq_div_2exp       mpq_div_2exp
#define korb_mq_get_d          mpq_get_d
#define korb_mq_init           mpq_init
#define korb_mq_numref         mpq_numref
#define korb_mq_set_d          mpq_set_d
#define korb_mq_set_den        mpq_set_den
#define korb_mq_set_num        mpq_set_num
#define korb_mq_set_si         mpq_set_si
#define korb_mq_set_str        mpq_set_str
#define korb_mq_set_z          mpq_set_z
#define korb_mq_sgn            mpq_sgn

/* get_str が返した文字列の解放 (GMP の allocator 経由) */
static inline void korb_mp_strfree(char *s, size_t n) {
    void (*freefn)(void *, size_t);
    mp_get_memory_functions(NULL, NULL, &freefn);
    freefn(s, n);
}

/* GC hooks: the limbs are external malloc, so the collector needs their size
 * and a way to release them (see AROH_FINALIZE in context.h). */
static inline size_t korb_mp_extbytes(korb_mp_srcptr z) {
    return (size_t)mpz_size(z) * sizeof(mp_limb_t);
}
static inline void korb_mp_free(korb_mp_ptr z) { mpz_clear(z); }

#endif
