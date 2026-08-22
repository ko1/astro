/* koruby_precise — 多倍長整数 backend の選択と契約。
 *
 * koruby が多倍長整数に触るのは korb_mp_* だけで、その実体を build 時に選ぶ。
 * GC の BARUBY_GC と同じ形の build-time switch。
 *
 *   KORB_BIGNUM=gmp   GMP。真の任意精度 = Ruby の意味論。
 *   KORB_BIGNUM=wrap  C の整数と同じく 64bit で wrap する。**Ruby とは意味論が
 *                     違う**（2**64 を跨ぐと答えが変わる）。外部依存が無いので
 *                     wasm など GMP を持ち込めない構成用。
 *
 * backend が提供するもの:
 *   型     korb_mp_t / _srcptr / _ptr / _limb_t / _size_t / _bitcnt_t
 *   演算   korb_mp_<op> (mpz_<op> と同じ形。使っているものだけで足りる)
 *   GC     korb_mp_extbytes(z)  外部 malloc しているバイト数
 *          korb_mp_free(z)      その解放 (context.h の AROH_FINALIZE から)
 *
 * KorbBignum の payload 型は backend が決める (context.h はこのヘッダ経由でしか
 * 知らない)。 */
#ifndef KORB_BIGNUM_BACKEND_H
#define KORB_BIGNUM_BACKEND_H

#define KORB_BIGNUM_GMP  1
#define KORB_BIGNUM_WRAP 2

#ifndef KORB_BIGNUM
#  define KORB_BIGNUM KORB_BIGNUM_GMP
#endif

#if   KORB_BIGNUM == KORB_BIGNUM_GMP
#  include "builtins/bignum_gmp.h"
#elif KORB_BIGNUM == KORB_BIGNUM_WRAP
#  include "builtins/bignum_wrap.h"
#else
#  error "unknown KORB_BIGNUM backend"
#endif

#endif
