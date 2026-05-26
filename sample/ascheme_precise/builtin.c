// ascheme — R5RS primitives (builtin functions) extracted from main.c.
//
// This compilation unit hosts:
//   * the PRIM macro + ARGV/ARG_FIX helpers
//   * the numeric tower (enum num_kind / num_kind_of / require_number /
//     to_mpz / to_mpq / common_kind / negate)
//   * the exported binary arithmetic / comparison helpers
//     (add2 / sub2 / mul2 / div2 / cmp2) used both by variadic prims
//     and by the specialized arith node fast paths
//   * the variadic numeric / list / string / vector / higher-order /
//     I/O / port primitives (prim_*)
//   * the PRIM_TABLE[] used by main.c's install_prims at startup
//   * the PRIM_*_VAL global sentinel pointers consulted by the
//     specialized arith / pred / vec node fast paths to detect user
//     redefinition of a primitive
//
// `install_prims` (= startup wiring) remains in main.c because it sets
// PORT_STDIN/STDOUT/STDERR which live there and because it is the
// natural anchor for startup-order code.

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>

#include "context.h"
#include "precise_gc/gc.h"
#include "node.h"

// ---------------------------------------------------------------------------
// Primitives (R5RS subset).  Argument count is checked in scm_apply via
// (min_argc, max_argc); each prim assumes its arity has already been
// validated.
// ---------------------------------------------------------------------------

#define PRIM(name) static VALUE prim_##name(CTX *c, int argc, VALUE *argv)
#define ARGV(i) argv[i]
#define ARG_FIX(i) (SCM_IS_FIXNUM(ARGV(i)) ? SCM_FIXVAL(ARGV(i)) : (scm_error(c, "expected integer"), (int64_t)0))

// ---------------------------------------------------------------------------
// Numeric tower: fixnum < bignum < rational < flonum < complex.
//   add2/sub2/mul2/div2 promote both operands to the wider kind, perform the
//   op, then collapse back via scm_normalize_int / scm_make_rational_q /
//   scm_simplify_complex.  Variadic primitives fold these binary ops.
// ---------------------------------------------------------------------------

enum num_kind { NK_FIX = 0, NK_BIG, NK_RAT, NK_FLT, NK_CPX, NK_NONE = -1 };

static enum num_kind
num_kind_of(VALUE v)
{
    if (SCM_IS_FIXNUM(v))   return NK_FIX;
    if (scm_is_bignum(v))   return NK_BIG;
    if (scm_is_rational(v)) return NK_RAT;
    if (scm_is_double(v))   return NK_FLT;
    if (scm_is_complex(v))  return NK_CPX;
    return NK_NONE;
}

static void
require_number(CTX *c, VALUE v, const char *what)
{
    if (num_kind_of(v) == NK_NONE) scm_error(c, "%s: not a number", what);
}

// Linux x86_64: long is 64-bit so mpz_set_si covers full int64_t.  Wrap it
// in case we ever build on a platform where this is no longer true.
static void
mpz_init_si64(mpz_t z, int64_t v)
{
    mpz_init(z);
#if LONG_MAX >= INT64_MAX
    mpz_set_si(z, (long)v);
#else
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
    mpz_set_str(z, buf, 10);
#endif
}

static void
to_mpz(VALUE v, mpz_t out)
{
    if (SCM_IS_FIXNUM(v))      mpz_init_si64(out, SCM_FIXVAL(v));
    else if (scm_is_bignum(v)) mpz_init_set(out, SCM_PTR(v)->mpz);
    else                       mpz_init(out);   // shouldn't happen
}

static void
to_mpq(VALUE v, mpq_t out)
{
    mpq_init(out);
    if (SCM_IS_FIXNUM(v)) {
#if LONG_MAX >= INT64_MAX
        mpq_set_si(out, (long)SCM_FIXVAL(v), 1);
#else
        char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)SCM_FIXVAL(v));
        mpz_set_str(mpq_numref(out), buf, 10);
        mpz_set_ui(mpq_denref(out), 1);
#endif
    }
    else if (scm_is_bignum(v)) {
        mpz_set(mpq_numref(out), SCM_PTR(v)->mpz);
        mpz_set_ui(mpq_denref(out), 1);
    }
    else if (scm_is_rational(v)) mpq_set(out, SCM_PTR(v)->mpq);
    else if (scm_is_double(v))   mpq_set_d(out, scm_get_double(v));
}

static void
get_complex(VALUE v, double *re, double *im)
{
    if (scm_is_complex(v)) { *re = SCM_PTR(v)->cpx.re; *im = SCM_PTR(v)->cpx.im; }
    else { *re = scm_get_double(v); *im = 0.0; }
}

static enum num_kind
common_kind(VALUE a, VALUE b)
{
    enum num_kind ka = num_kind_of(a), kb = num_kind_of(b);
    return ka > kb ? ka : kb;
}

VALUE
add2(CTX *c, VALUE a, VALUE b)
{
    // Fast path: both fixnums.  Bypasses require_number + common_kind +
    // switch.  Two fixnums in 63-bit range can overflow int64 only if both
    // operands are extreme; the builtin lets us bail to the bignum path
    // in that rare case.
    if (LIKELY(SCM_IS_FIXNUM(a) & SCM_IS_FIXNUM(b))) {
        int64_t r;
        if (LIKELY(!__builtin_add_overflow((int64_t)SCM_FIXVAL(a),
                                           (int64_t)SCM_FIXVAL(b), &r) &&
                    r >= SCM_FIXNUM_MIN && r <= SCM_FIXNUM_MAX))
            return SCM_FIX(r);
    }
    // Inline-flonum fast path.  Most "scientific" doubles round-trip
    // through the rotated encoding, so this avoids both the kind switch
    // and the heap allocation.
    if (LIKELY(SCM_IS_FLONUM(a) & SCM_IS_FLONUM(b))) {
        return scm_make_double(c, scm_flonum_to_double(a) + scm_flonum_to_double(b));
    }
    require_number(c, a, "+"); require_number(c, b, "+");
    switch (common_kind(a, b)) {
    case NK_FIX: {
        int64_t r;
        if (!__builtin_add_overflow((int64_t)SCM_FIXVAL(a), (int64_t)SCM_FIXVAL(b), &r))
            return scm_make_int(c, r);
        // overflow → fall through to bignum path
    }
        // fallthrough
    case NK_BIG: {
        mpz_t za, zb, r; to_mpz(a, za); to_mpz(b, zb); mpz_init(r);
        mpz_add(r, za, zb);
        VALUE rv = scm_normalize_int(c, r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r); return rv;
    }
    case NK_RAT: {
        mpq_t qa, qb, r; to_mpq(a, qa); to_mpq(b, qb); mpq_init(r);
        mpq_add(r, qa, qb);
        VALUE rv = scm_make_rational_q(c, r);
        mpq_clear(qa); mpq_clear(qb); mpq_clear(r); return rv;
    }
    case NK_FLT:
        return scm_make_double(c, scm_get_double(a) + scm_get_double(b));
    case NK_CPX: {
        double ar, ai, br, bi;
        get_complex(a, &ar, &ai); get_complex(b, &br, &bi);
        return scm_simplify_complex(c, ar + br, ai + bi);
    }
    default: scm_error(c, "+: not a number");
    }
}

VALUE
sub2(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(SCM_IS_FIXNUM(a) & SCM_IS_FIXNUM(b))) {
        int64_t r;
        if (LIKELY(!__builtin_sub_overflow((int64_t)SCM_FIXVAL(a),
                                           (int64_t)SCM_FIXVAL(b), &r) &&
                    r >= SCM_FIXNUM_MIN && r <= SCM_FIXNUM_MAX))
            return SCM_FIX(r);
    }
    if (LIKELY(SCM_IS_FLONUM(a) & SCM_IS_FLONUM(b))) {
        return scm_make_double(c, scm_flonum_to_double(a) - scm_flonum_to_double(b));
    }
    require_number(c, a, "-"); require_number(c, b, "-");
    switch (common_kind(a, b)) {
    case NK_FIX: {
        int64_t r;
        if (!__builtin_sub_overflow((int64_t)SCM_FIXVAL(a), (int64_t)SCM_FIXVAL(b), &r))
            return scm_make_int(c, r);
    }
        // fallthrough
    case NK_BIG: {
        mpz_t za, zb, r; to_mpz(a, za); to_mpz(b, zb); mpz_init(r);
        mpz_sub(r, za, zb);
        VALUE rv = scm_normalize_int(c, r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r); return rv;
    }
    case NK_RAT: {
        mpq_t qa, qb, r; to_mpq(a, qa); to_mpq(b, qb); mpq_init(r);
        mpq_sub(r, qa, qb);
        VALUE rv = scm_make_rational_q(c, r);
        mpq_clear(qa); mpq_clear(qb); mpq_clear(r); return rv;
    }
    case NK_FLT:
        return scm_make_double(c, scm_get_double(a) - scm_get_double(b));
    case NK_CPX: {
        double ar, ai, br, bi;
        get_complex(a, &ar, &ai); get_complex(b, &br, &bi);
        return scm_simplify_complex(c, ar - br, ai - bi);
    }
    default: scm_error(c, "-: not a number");
    }
}

VALUE
mul2(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(SCM_IS_FIXNUM(a) & SCM_IS_FIXNUM(b))) {
        int64_t r;
        if (LIKELY(!__builtin_mul_overflow((int64_t)SCM_FIXVAL(a),
                                           (int64_t)SCM_FIXVAL(b), &r) &&
                    r >= SCM_FIXNUM_MIN && r <= SCM_FIXNUM_MAX))
            return SCM_FIX(r);
    }
    if (LIKELY(SCM_IS_FLONUM(a) & SCM_IS_FLONUM(b))) {
        return scm_make_double(c, scm_flonum_to_double(a) * scm_flonum_to_double(b));
    }
    require_number(c, a, "*"); require_number(c, b, "*");
    switch (common_kind(a, b)) {
    case NK_FIX: {
        int64_t r;
        if (!__builtin_mul_overflow((int64_t)SCM_FIXVAL(a), (int64_t)SCM_FIXVAL(b), &r))
            return scm_make_int(c, r);
    }
        // fallthrough
    case NK_BIG: {
        mpz_t za, zb, r; to_mpz(a, za); to_mpz(b, zb); mpz_init(r);
        mpz_mul(r, za, zb);
        VALUE rv = scm_normalize_int(c, r);
        mpz_clear(za); mpz_clear(zb); mpz_clear(r); return rv;
    }
    case NK_RAT: {
        mpq_t qa, qb, r; to_mpq(a, qa); to_mpq(b, qb); mpq_init(r);
        mpq_mul(r, qa, qb);
        VALUE rv = scm_make_rational_q(c, r);
        mpq_clear(qa); mpq_clear(qb); mpq_clear(r); return rv;
    }
    case NK_FLT:
        return scm_make_double(c, scm_get_double(a) * scm_get_double(b));
    case NK_CPX: {
        double ar, ai, br, bi;
        get_complex(a, &ar, &ai); get_complex(b, &br, &bi);
        return scm_simplify_complex(c, ar*br - ai*bi, ar*bi + ai*br);
    }
    default: scm_error(c, "*: not a number");
    }
}

static VALUE
div2(CTX *c, VALUE a, VALUE b)
{
    require_number(c, a, "/"); require_number(c, b, "/");
    enum num_kind k = common_kind(a, b);
    if (k <= NK_RAT) {
        // Exact arithmetic: a / b is rational.
        mpq_t qa, qb, r; to_mpq(a, qa); to_mpq(b, qb); mpq_init(r);
        if (mpq_sgn(qb) == 0) {
            mpq_clear(qa); mpq_clear(qb); mpq_clear(r);
            scm_error(c, "/: division by zero");
        }
        mpq_div(r, qa, qb);
        VALUE rv = scm_make_rational_q(c, r);
        mpq_clear(qa); mpq_clear(qb); mpq_clear(r); return rv;
    }
    if (k == NK_FLT) {
        double rhs = scm_get_double(b);
        if (rhs == 0) scm_error(c, "/: division by zero");
        return scm_make_double(c, scm_get_double(a) / rhs);
    }
    // complex division
    double ar, ai, br, bi;
    get_complex(a, &ar, &ai); get_complex(b, &br, &bi);
    double denom = br*br + bi*bi;
    if (denom == 0) scm_error(c, "/: division by zero");
    return scm_simplify_complex(c, (ar*br + ai*bi) / denom, (ai*br - ar*bi) / denom);
}

static VALUE
negate(CTX *c, VALUE a)
{
    require_number(c, a, "-");
    switch (num_kind_of(a)) {
    case NK_FIX: {
        int64_t v = SCM_FIXVAL(a);
        if (v == INT64_MIN) {     // theoretical edge — fixnum range narrower than int64 anyway
            mpz_t z; mpz_init_si64(z, v); mpz_neg(z, z);
            VALUE rv = scm_normalize_int(c, z); mpz_clear(z); return rv;
        }
        return scm_make_int(c, -v);
    }
    case NK_BIG: {
        mpz_t z; mpz_init(z); mpz_neg(z, SCM_PTR(a)->mpz);
        VALUE rv = scm_normalize_int(c, z); mpz_clear(z); return rv;
    }
    case NK_RAT: {
        mpq_t q; mpq_init(q); mpq_neg(q, SCM_PTR(a)->mpq);
        VALUE rv = scm_make_rational_q(c, q); mpq_clear(q); return rv;
    }
    case NK_FLT: return scm_make_double(c, -scm_get_double(a));
    case NK_CPX: return scm_simplify_complex(c, -SCM_PTR(a)->cpx.re, -SCM_PTR(a)->cpx.im);
    default: scm_error(c, "-: not a number");
    }
}

// 3-way compare, real numbers only.  Returns -1, 0, 1, or aborts.
int
cmp2(CTX *c, VALUE a, VALUE b)
{
    if (LIKELY(SCM_IS_FIXNUM(a) & SCM_IS_FIXNUM(b))) {
        int64_t x = SCM_FIXVAL(a), y = SCM_FIXVAL(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (LIKELY(SCM_IS_FLONUM(a) & SCM_IS_FLONUM(b))) {
        double x = scm_flonum_to_double(a), y = scm_flonum_to_double(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    require_number(c, a, "comparison"); require_number(c, b, "comparison");
    if (scm_is_complex(a) || scm_is_complex(b)) scm_error(c, "comparison: complex not ordered");
    switch (common_kind(a, b)) {
    case NK_FIX: {
        int64_t x = SCM_FIXVAL(a), y = SCM_FIXVAL(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    case NK_BIG: {
        mpz_t za, zb; to_mpz(a, za); to_mpz(b, zb);
        int r = mpz_cmp(za, zb);
        mpz_clear(za); mpz_clear(zb);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    case NK_RAT: {
        mpq_t qa, qb; to_mpq(a, qa); to_mpq(b, qb);
        int r = mpq_cmp(qa, qb);
        mpq_clear(qa); mpq_clear(qb);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    case NK_FLT: {
        double x = scm_get_double(a), y = scm_get_double(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    default: return 0;
    }
}

PRIM(plus)
{
    if (argc == 0) return SCM_FIX(0);
    VALUE r = argv[0];
    require_number(c, r, "+");
    for (int i = 1; i < argc; i++) r = add2(c, r, argv[i]);
    return r;
}

PRIM(minus)
{
    if (argc == 0) scm_error(c, "-: need at least 1 arg");
    if (argc == 1) return negate(c, argv[0]);
    VALUE r = argv[0];
    for (int i = 1; i < argc; i++) r = sub2(c, r, argv[i]);
    return r;
}

PRIM(mul)
{
    if (argc == 0) return SCM_FIX(1);
    VALUE r = argv[0];
    require_number(c, r, "*");
    for (int i = 1; i < argc; i++) r = mul2(c, r, argv[i]);
    return r;
}

PRIM(div)
{
    if (argc == 0) scm_error(c, "/: need at least 1 arg");
    VALUE r = argv[0];
    if (argc == 1) {
        require_number(c, r, "/");
        return div2(c, SCM_FIX(1), r);
    }
    for (int i = 1; i < argc; i++) r = div2(c, r, argv[i]);
    return r;
}

#define DEF_NUM_CMP(name, op)                                                  \
PRIM(name)                                                                     \
{                                                                              \
    for (int i = 1; i < argc; i++) {                                           \
        if (!(cmp2(c, argv[i-1], argv[i]) op 0)) return SCM_FALSE;             \
    }                                                                          \
    return SCM_TRUE;                                                           \
}
DEF_NUM_CMP(num_eq, ==)
DEF_NUM_CMP(num_lt, <)
DEF_NUM_CMP(num_gt, >)
DEF_NUM_CMP(num_le, <=)
DEF_NUM_CMP(num_ge, >=)

PRIM(modulo)
{
    require_number(c, argv[0], "modulo"); require_number(c, argv[1], "modulo");
    if (cmp2(c, argv[1], SCM_FIX(0)) == 0) scm_error(c, "modulo: division by zero");
    if (SCM_IS_FIXNUM(argv[0]) && SCM_IS_FIXNUM(argv[1])) {
        int64_t a = SCM_FIXVAL(argv[0]), b = SCM_FIXVAL(argv[1]);
        int64_t r = a % b;
        if ((r != 0) && ((r < 0) != (b < 0))) r += b;
        return scm_make_int(c, r);
    }
    mpz_t za, zb, r; to_mpz(argv[0], za); to_mpz(argv[1], zb); mpz_init(r);
    mpz_mod(r, za, zb);                  // mpz_mod returns 0..|zb|-1
    if (mpz_sgn(r) != 0 && mpz_sgn(r) != mpz_sgn(zb)) mpz_add(r, r, zb);
    VALUE rv = scm_normalize_int(c, r);
    mpz_clear(za); mpz_clear(zb); mpz_clear(r);
    return rv;
}

PRIM(remainder)
{
    require_number(c, argv[0], "remainder"); require_number(c, argv[1], "remainder");
    if (cmp2(c, argv[1], SCM_FIX(0)) == 0) scm_error(c, "remainder: division by zero");
    if (SCM_IS_FIXNUM(argv[0]) && SCM_IS_FIXNUM(argv[1])) {
        return scm_make_int(c, SCM_FIXVAL(argv[0]) % SCM_FIXVAL(argv[1]));
    }
    mpz_t za, zb, r; to_mpz(argv[0], za); to_mpz(argv[1], zb); mpz_init(r);
    mpz_tdiv_r(r, za, zb);
    VALUE rv = scm_normalize_int(c, r);
    mpz_clear(za); mpz_clear(zb); mpz_clear(r);
    return rv;
}

PRIM(quotient)
{
    require_number(c, argv[0], "quotient"); require_number(c, argv[1], "quotient");
    if (cmp2(c, argv[1], SCM_FIX(0)) == 0) scm_error(c, "quotient: division by zero");
    if (SCM_IS_FIXNUM(argv[0]) && SCM_IS_FIXNUM(argv[1])) {
        return scm_make_int(c, SCM_FIXVAL(argv[0]) / SCM_FIXVAL(argv[1]));
    }
    mpz_t za, zb, r; to_mpz(argv[0], za); to_mpz(argv[1], zb); mpz_init(r);
    mpz_tdiv_q(r, za, zb);
    VALUE rv = scm_normalize_int(c, r);
    mpz_clear(za); mpz_clear(zb); mpz_clear(r);
    return rv;
}

PRIM(gcd_p)
{
    if (argc == 0) return SCM_FIX(0);
    mpz_t a, b, r; mpz_init(a); to_mpz(argv[0], a);
    if (mpz_sgn(a) < 0) mpz_abs(a, a);
    for (int i = 1; i < argc; i++) {
        to_mpz(argv[i], b); mpz_init(r);
        mpz_gcd(r, a, b);
        mpz_set(a, r); mpz_clear(b); mpz_clear(r);
    }
    VALUE rv = scm_normalize_int(c, a); mpz_clear(a); return rv;
}

PRIM(lcm_p)
{
    if (argc == 0) return SCM_FIX(1);
    mpz_t a, b, r; mpz_init(a); to_mpz(argv[0], a);
    mpz_abs(a, a);
    for (int i = 1; i < argc; i++) {
        to_mpz(argv[i], b); mpz_init(r);
        mpz_lcm(r, a, b);
        mpz_set(a, r); mpz_clear(b); mpz_clear(r);
    }
    VALUE rv = scm_normalize_int(c, a); mpz_clear(a); return rv;
}

PRIM(abs)
{
    require_number(c, argv[0], "abs");
    switch (num_kind_of(argv[0])) {
    case NK_FIX: {
        int64_t v = SCM_FIXVAL(argv[0]);
        return scm_make_int(c, v < 0 ? -v : v);
    }
    case NK_BIG: {
        mpz_t z; mpz_init(z); mpz_abs(z, SCM_PTR(argv[0])->mpz);
        VALUE rv = scm_normalize_int(c, z); mpz_clear(z); return rv;
    }
    case NK_RAT: {
        mpq_t q; mpq_init(q); mpq_abs(q, SCM_PTR(argv[0])->mpq);
        VALUE rv = scm_make_rational_q(c, q); mpq_clear(q); return rv;
    }
    case NK_FLT: {
        double d = scm_get_double(argv[0]);
        return scm_make_double(c, d < 0 ? -d : d);
    }
    default: scm_error(c, "abs: not a real number");
    }
}

PRIM(min) {
    VALUE m = argv[0];
    bool inexact = scm_is_inexact(m);
    for (int i = 1; i < argc; i++) {
        if (scm_is_inexact(argv[i])) inexact = true;
        if (cmp2(c, argv[i], m) < 0) m = argv[i];
    }
    return inexact && !scm_is_inexact(m) ? scm_make_double(c, scm_get_double(m)) : m;
}
PRIM(max) {
    VALUE m = argv[0];
    bool inexact = scm_is_inexact(m);
    for (int i = 1; i < argc; i++) {
        if (scm_is_inexact(argv[i])) inexact = true;
        if (cmp2(c, argv[i], m) > 0) m = argv[i];
    }
    return inexact && !scm_is_inexact(m) ? scm_make_double(c, scm_get_double(m)) : m;
}

PRIM(zero_p)     { (void)argc; return cmp2(c, argv[0], SCM_FIX(0)) == 0 ? SCM_TRUE : SCM_FALSE; }
PRIM(positive_p) { (void)argc; return cmp2(c, argv[0], SCM_FIX(0)) >  0 ? SCM_TRUE : SCM_FALSE; }
PRIM(negative_p) { (void)argc; return cmp2(c, argv[0], SCM_FIX(0)) <  0 ? SCM_TRUE : SCM_FALSE; }
PRIM(odd_p) {
    (void)argc;
    if (SCM_IS_FIXNUM(argv[0])) return (SCM_FIXVAL(argv[0]) & 1) ? SCM_TRUE : SCM_FALSE;
    if (scm_is_bignum(argv[0])) return mpz_odd_p(SCM_PTR(argv[0])->mpz) ? SCM_TRUE : SCM_FALSE;
    scm_error(c, "odd?: not an integer");
}
PRIM(even_p) {
    (void)argc;
    if (SCM_IS_FIXNUM(argv[0])) return (SCM_FIXVAL(argv[0]) & 1) ? SCM_FALSE : SCM_TRUE;
    if (scm_is_bignum(argv[0])) return mpz_even_p(SCM_PTR(argv[0])->mpz) ? SCM_TRUE : SCM_FALSE;
    scm_error(c, "even?: not an integer");
}

PRIM(number_p)   { (void)c; (void)argc; return scm_is_number(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(integer_p)  { (void)c; (void)argc; return scm_is_integer_value(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(real_p)     { (void)c; (void)argc; return scm_is_real(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(rational_p) { (void)c; (void)argc; return scm_is_real(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(complex_p)  { (void)c; (void)argc; return scm_is_number(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(exact_p)    { (void)c; (void)argc; return scm_is_exact(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(inexact_p)  { (void)c; (void)argc; return scm_is_inexact(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(exact_to_inexact) {
    (void)argc;
    if (scm_is_complex(argv[0])) return argv[0];
    return scm_make_double(c, scm_get_double(argv[0]));
}
PRIM(inexact_to_exact) {
    (void)argc;
    if (scm_is_exact(argv[0])) return argv[0];
    if (scm_is_double(argv[0])) {
        double d = scm_get_double(argv[0]);
        if (d == (double)(int64_t)d) return scm_make_int(c, (int64_t)d);
        // approximate as rational via GMP
        mpq_t q; mpq_init(q); mpq_set_d(q, d);
        VALUE rv = scm_make_rational_q(c, q); mpq_clear(q);
        return rv;
    }
    scm_error(c, "inexact->exact: not a real number");
}

PRIM(cons)   { (void)c; (void)argc; return scm_cons(c, argv[0], argv[1]); }
PRIM(car)    {
    if (!scm_is_pair(argv[0])) scm_error(c, "car: not a pair");
    return SCM_PTR(argv[0])->pair.car;
}
PRIM(cdr)    {
    if (!scm_is_pair(argv[0])) scm_error(c, "cdr: not a pair");
    return SCM_PTR(argv[0])->pair.cdr;
}
PRIM(set_car) {
    (void)argc;
    if (!scm_is_pair(argv[0])) scm_error(c, "set-car!: not a pair");
    struct sobj *p = SCM_PTR(argv[0]);
    aro_gc_wb(c, p, (VALUE *)&p->pair.car, argv[1]);
    return SCM_UNSPEC;
}
PRIM(set_cdr) {
    (void)argc;
    if (!scm_is_pair(argv[0])) scm_error(c, "set-cdr!: not a pair");
    struct sobj *p = SCM_PTR(argv[0]);
    aro_gc_wb(c, p, (VALUE *)&p->pair.cdr, argv[1]);
    return SCM_UNSPEC;
}
PRIM(pair_p) { (void)c; (void)argc; return scm_is_pair(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(null_p) { (void)c; (void)argc; return scm_is_null(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(list)   {
    (void)c;
    /* Park argv copies + accumulator across the per-iteration scm_cons. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 1 + argc);    /* sp[0]=r, sp[1..]=argv copies */
    sp[0] = SCM_NIL;
    for (int i = 0; i < argc; i++) sp[1 + i] = argv[i];
    for (int i = argc - 1; i >= 0; i--) sp[0] = scm_cons(c, sp[1 + i], sp[0]);
    VALUE r = sp[0];
    SP_POP(c, sp);
    return r;
}
PRIM(list_p) {
    // R5RS: circular lists return #f.  Floyd's tortoise-and-hare detects
    // cycles in linear time without auxiliary storage.
    (void)c; (void)argc;
    VALUE slow = argv[0], fast = argv[0];
    while (scm_is_pair(fast)) {
        fast = SCM_PTR(fast)->pair.cdr;
        if (!scm_is_pair(fast)) break;
        fast = SCM_PTR(fast)->pair.cdr;
        slow = SCM_PTR(slow)->pair.cdr;
        if (slow == fast) return SCM_FALSE;
    }
    return fast == SCM_NIL ? SCM_TRUE : SCM_FALSE;
}
PRIM(length) {
    int n = 0;
    VALUE v = argv[0];
    while (scm_is_pair(v)) { n++; v = SCM_PTR(v)->pair.cdr; }
    if (v != SCM_NIL) scm_error(c, "length: not a proper list");
    (void)argc;
    return SCM_FIX(n);
}
PRIM(reverse) {
    (void)argc;
    /* Park v + r across the per-iteration scm_cons (= GC trigger). */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);     /* sp[0]=v, sp[1]=r */
    sp[0] = argv[0];
    sp[1] = SCM_NIL;
    while (scm_is_pair(sp[0])) {
        sp[1] = scm_cons(c, SCM_PTR(sp[0])->pair.car, sp[1]);
        sp[0] = SCM_PTR(sp[0])->pair.cdr;
    }
    if (sp[0] != SCM_NIL) scm_error(c, "reverse: not a proper list");
    VALUE r = sp[1];
    SP_POP(c, sp);
    return r;
}
PRIM(append) {
    (void)c;
    if (argc == 0) return SCM_NIL;
    /* sp[0]=result, sp[1]=list (current input), sp[2]=tmp (reversed list)
     * Each scm_cons inside the loops can trigger GC. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 3);
    sp[0] = argv[argc - 1];
    for (int i = argc - 2; i >= 0; i--) {
        sp[1] = argv[i];
        sp[2] = SCM_NIL;
        while (scm_is_pair(sp[1])) {
            sp[2] = scm_cons(c, SCM_PTR(sp[1])->pair.car, sp[2]);
            sp[1] = SCM_PTR(sp[1])->pair.cdr;
        }
        while (scm_is_pair(sp[2])) {
            sp[0] = scm_cons(c, SCM_PTR(sp[2])->pair.car, sp[0]);
            sp[2] = SCM_PTR(sp[2])->pair.cdr;
        }
    }
    VALUE result = sp[0];
    SP_POP(c, sp);
    return result;
}
PRIM(list_ref) {
    VALUE v = argv[0];
    int64_t i = ARG_FIX(1);
    while (i-- > 0 && scm_is_pair(v)) v = SCM_PTR(v)->pair.cdr;
    if (!scm_is_pair(v)) scm_error(c, "list-ref: out of range");
    (void)argc;
    return SCM_PTR(v)->pair.car;
}
PRIM(list_tail) {
    VALUE v = argv[0];
    int64_t i = ARG_FIX(1);
    while (i-- > 0 && scm_is_pair(v)) v = SCM_PTR(v)->pair.cdr;
    (void)argc;
    return v;
}
static bool scm_eq(VALUE a, VALUE b) { return a == b; }
static bool scm_eqv_impl(VALUE a, VALUE b) {
    if (a == b) return true;
    if (scm_is_double(a) && scm_is_double(b)) return scm_get_double(a) == scm_get_double(b);
    if (scm_is_char(a) && scm_is_char(b))     return SCM_PTR(a)->ch == SCM_PTR(b)->ch;
    return false;
}
static bool scm_equal_impl(VALUE a, VALUE b) {
    if (scm_eqv_impl(a, b)) return true;
    if (scm_is_pair(a) && scm_is_pair(b))
        return scm_equal_impl(SCM_PTR(a)->pair.car, SCM_PTR(b)->pair.car) &&
               scm_equal_impl(SCM_PTR(a)->pair.cdr, SCM_PTR(b)->pair.cdr);
    if (scm_is_string(a) && scm_is_string(b)) {
        struct sobj *sa = SCM_PTR(a), *sb = SCM_PTR(b);
        return sa->str.len == sb->str.len && memcmp(sa->str.chars, sb->str.chars, sa->str.len) == 0;
    }
    if (scm_is_vector(a) && scm_is_vector(b)) {
        struct sobj *va = SCM_PTR(a), *vb = SCM_PTR(b);
        if (va->vec.len != vb->vec.len) return false;
        for (size_t i = 0; i < va->vec.len; i++)
            if (!scm_equal_impl(va->vec.items[i], vb->vec.items[i])) return false;
        return true;
    }
    return false;
}
PRIM(eq_p)     { (void)c; (void)argc; return scm_eq(argv[0], argv[1]) ? SCM_TRUE : SCM_FALSE; }
PRIM(eqv_p)    { (void)c; (void)argc; return scm_eqv_impl(argv[0], argv[1]) ? SCM_TRUE : SCM_FALSE; }
PRIM(equal_p)  { (void)c; (void)argc; return scm_equal_impl(argv[0], argv[1]) ? SCM_TRUE : SCM_FALSE; }

PRIM(memv) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        if (scm_eqv_impl(k, SCM_PTR(l)->pair.car)) return l;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(memq) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        if (k == SCM_PTR(l)->pair.car) return l;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(member) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        if (scm_equal_impl(k, SCM_PTR(l)->pair.car)) return l;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(assq) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        VALUE p = SCM_PTR(l)->pair.car;
        if (scm_is_pair(p) && SCM_PTR(p)->pair.car == k) return p;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(assv) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        VALUE p = SCM_PTR(l)->pair.car;
        if (scm_is_pair(p) && scm_eqv_impl(SCM_PTR(p)->pair.car, k)) return p;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}
PRIM(assoc) {
    (void)argc; (void)c;
    VALUE k = argv[0]; VALUE l = argv[1];
    while (scm_is_pair(l)) {
        VALUE p = SCM_PTR(l)->pair.car;
        if (scm_is_pair(p) && scm_equal_impl(SCM_PTR(p)->pair.car, k)) return p;
        l = SCM_PTR(l)->pair.cdr;
    }
    return SCM_FALSE;
}

PRIM(boolean_p)  { (void)c; (void)argc; return scm_is_bool(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(not_p)      { (void)c; (void)argc; return scm_is_false(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(symbol_p)   { (void)c; (void)argc; return scm_is_symbol(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(procedure_p){ (void)c; (void)argc; return scm_is_proc(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(string_p)   { (void)c; (void)argc; return scm_is_string(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_p)     { (void)c; (void)argc; return scm_is_char(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(vector_p)   { (void)c; (void)argc; return scm_is_vector(argv[0]) ? SCM_TRUE : SCM_FALSE; }

PRIM(symbol_to_string) {
    (void)argc;
    if (!scm_is_symbol(argv[0])) scm_error(c, "symbol->string: not a symbol");
    return scm_make_string(c, SCM_PTR(argv[0])->sym.name, strlen(SCM_PTR(argv[0])->sym.name));
}
PRIM(string_to_symbol) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "string->symbol: not a string");
    return scm_intern(c, SCM_PTR(argv[0])->str.chars);
}

PRIM(number_to_string) {
    (void)argc;
    int base = 10;
    if (argc >= 2) base = (int)ARG_FIX(1);
    char buf[128];
    if (SCM_IS_FIXNUM(argv[0])) {
        if (base == 10) snprintf(buf, sizeof(buf), "%lld", (long long)SCM_FIXVAL(argv[0]));
        else {
            mpz_t z; mpz_init_si64(z, SCM_FIXVAL(argv[0]));
            char *s = mpz_get_str(NULL, base, z);
            VALUE rv = scm_make_string(c, s, strlen(s));
            mpz_clear(z);
            return rv;
        }
    }
    else if (scm_is_bignum(argv[0])) {
        char *s = mpz_get_str(NULL, base, SCM_PTR(argv[0])->mpz);
        return scm_make_string(c, s, strlen(s));
    }
    else if (scm_is_rational(argv[0])) {
        char *s = mpq_get_str(NULL, base, SCM_PTR(argv[0])->mpq);
        return scm_make_string(c, s, strlen(s));
    }
    else if (scm_is_double(argv[0])) {
        snprintf(buf, sizeof(buf), "%.15g", scm_get_double(argv[0]));
    }
    else if (scm_is_complex(argv[0])) {
        snprintf(buf, sizeof(buf), "%.15g%+.15gi",
                 SCM_PTR(argv[0])->cpx.re, SCM_PTR(argv[0])->cpx.im);
    }
    else scm_error(c, "number->string: not a number");
    return scm_make_string(c, buf, strlen(buf));
}
PRIM(string_to_number) {
    (void)argc; (void)c;
    if (!scm_is_string(argv[0])) scm_error(c, "string->number: not a string");
    int base = 10;
    if (argc >= 2) base = (int)ARG_FIX(1);
    // Always re-read `s` after any allocation: argv[0] is rooted (build_frame_for
    // parks it on c->sp), but the C-local char* into the string's byte payload
    // would dangle across a moving GC.  Same applies to `slash`.
    const char *s = SCM_PTR(argv[0])->str.chars;
    // rational?
    if (strchr(s, '/')) {
        /* throwaway buffer for splitting "num/den"; not retained past this call. */
        size_t slen = strlen(s);
        char *copy = (char *)aro_gc_alloc_byte_raw(c, sizeof(AroObjectHeader) + slen + 1);
        copy += sizeof(AroObjectHeader);
        /* Re-read s after the alloc — under a moving GC the previous pointer
         * is now stale (= the string's chars buffer was relocated). */
        s = SCM_PTR(argv[0])->str.chars;
        memcpy(copy, s, slen + 1);
        char *sl = strchr(copy, '/');
        if (!sl) return SCM_FALSE;
        *sl = '\0';
        mpz_t num, den;
        if (mpz_init_set_str(num, copy, base) == 0) {
            if (mpz_init_set_str(den, sl + 1, base) == 0 && mpz_sgn(den) != 0) {
                VALUE rv = scm_make_rational_zz(c, num, den);
                mpz_clear(num); mpz_clear(den); return rv;
            }
            mpz_clear(num);
        }
        return SCM_FALSE;
    }
    // integer?
    mpz_t z;
    if (mpz_init_set_str(z, s, base) == 0) {
        VALUE rv = scm_normalize_int(c, z); mpz_clear(z); return rv;
    }
    mpz_clear(z);
    // Re-read s before strtod — scm_normalize_int (called above before
    // mpz_clear) may have allocated.
    s = SCM_PTR(argv[0])->str.chars;
    // double?
    char *end;
    double d = strtod(s, &end);
    if (*end == '\0' && end != s) return scm_make_double(c, d);
    return SCM_FALSE;
}

PRIM(string_length) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "string-length: not a string");
    return SCM_FIX(SCM_PTR(argv[0])->str.len);
}
PRIM(string_ref) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "string-ref: not a string");
    size_t i = (size_t)ARG_FIX(1);
    if (i >= SCM_PTR(argv[0])->str.len) scm_error(c, "string-ref: out of range");
    return scm_make_char(c, (unsigned char)SCM_PTR(argv[0])->str.chars[i]);
}
PRIM(string_eq) {
    (void)c;
    for (int i = 1; i < argc; i++) {
        struct sobj *a = SCM_PTR(argv[i-1]), *b = SCM_PTR(argv[i]);
        if (a->str.len != b->str.len || memcmp(a->str.chars, b->str.chars, a->str.len) != 0) return SCM_FALSE;
    }
    return SCM_TRUE;
}

static int
str_cmp(struct sobj *a, struct sobj *b)
{
    size_t n = a->str.len < b->str.len ? a->str.len : b->str.len;
    int c = memcmp(a->str.chars, b->str.chars, n);
    if (c != 0) return c;
    return a->str.len < b->str.len ? -1 : a->str.len > b->str.len ? 1 : 0;
}

#define DEF_STR_CMP(name, op)                                                 \
PRIM(name) {                                                                  \
    (void)c;                                                                  \
    for (int i = 1; i < argc; i++) {                                          \
        if (!(str_cmp(SCM_PTR(argv[i-1]), SCM_PTR(argv[i])) op 0))            \
            return SCM_FALSE;                                                 \
    }                                                                         \
    return SCM_TRUE;                                                          \
}
DEF_STR_CMP(string_lt, <)
DEF_STR_CMP(string_gt, >)
DEF_STR_CMP(string_le, <=)
DEF_STR_CMP(string_ge, >=)
#undef DEF_STR_CMP

PRIM(string_ci_eq) {
    (void)c;
    for (int i = 1; i < argc; i++) {
        struct sobj *a = SCM_PTR(argv[i-1]), *b = SCM_PTR(argv[i]);
        if (a->str.len != b->str.len) return SCM_FALSE;
        for (size_t j = 0; j < a->str.len; j++)
            if (tolower((unsigned char)a->str.chars[j]) !=
                tolower((unsigned char)b->str.chars[j])) return SCM_FALSE;
    }
    return SCM_TRUE;
}

PRIM(char_ci_eq) {
    (void)c;
    for (int i = 1; i < argc; i++)
        if (tolower(SCM_PTR(argv[i-1])->ch) != tolower(SCM_PTR(argv[i])->ch))
            return SCM_FALSE;
    return SCM_TRUE;
}

PRIM(string_copy) {
    (void)argc; (void)c;
    struct sobj *s = SCM_PTR(argv[0]);
    return scm_make_string(c, s->str.chars, s->str.len);
}

PRIM(string_set) {
    (void)argc;
    struct sobj *s = SCM_PTR(argv[0]);
    size_t i = (size_t)ARG_FIX(1);
    if (i >= s->str.len) scm_error(c, "string-set!: out of range");
    s->str.chars[i] = (char)SCM_PTR(argv[2])->ch;
    return SCM_UNSPEC;
}

PRIM(string_p_proc) {
    (void)argc; (void)c;
    return scm_make_string(c, SCM_PTR(argv[0])->sym.name,
                           strlen(SCM_PTR(argv[0])->sym.name));
}
PRIM(make_string) {
    char fill = ' ';
    if (argc >= 2 && scm_is_char(argv[1])) fill = (char)SCM_PTR(argv[1])->ch;
    (void)c;
    return scm_make_string_n(c, (size_t)ARG_FIX(0), fill);
}
PRIM(string_append) {
    (void)c;
    size_t total = 0;
    for (int i = 0; i < argc; i++) total += SCM_PTR(argv[i])->str.len;
    VALUE r = scm_make_string_n(c, total, ' ');
    char *out = SCM_PTR(r)->str.chars;
    for (int i = 0; i < argc; i++) {
        memcpy(out, SCM_PTR(argv[i])->str.chars, SCM_PTR(argv[i])->str.len);
        out += SCM_PTR(argv[i])->str.len;
    }
    return r;
}
PRIM(substring) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "substring: not a string");
    size_t start = (size_t)ARG_FIX(1), end = (size_t)ARG_FIX(2);
    if (start > end || end > SCM_PTR(argv[0])->str.len) scm_error(c, "substring: out of range");
    return scm_make_string(c, SCM_PTR(argv[0])->str.chars + start, end - start);
}
PRIM(string_to_list) {
    (void)argc;
    /* sp[0]=source string sobj, sp[1]=accumulator r */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);
    sp[0] = argv[0];
    sp[1] = SCM_NIL;
    size_t len = SCM_PTR(sp[0])->str.len;
    for (size_t i = len; i--; ) {
        /* Re-read chars from the parked sobj each iteration — its byte
         * payload may move under moving GC. */
        unsigned char ch = (unsigned char)SCM_PTR(sp[0])->str.chars[i];
        VALUE chv = scm_make_char(c, ch);
        sp[1] = scm_cons(c, chv, sp[1]);
    }
    VALUE r = sp[1];
    SP_POP(c, sp);
    return r;
}
PRIM(list_to_string) {
    (void)argc;
    /* sp[0]=source list (l), sp[1]=result string, sp[2]=walking iter
     * (= live across scm_cons/scm_make alloc, though only one big alloc
     * here for the result). */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 3);
    sp[0] = argv[0];
    sp[1] = SCM_NIL;
    size_t n = 0;
    for (sp[2] = sp[0]; scm_is_pair(sp[2]); sp[2] = SCM_PTR(sp[2])->pair.cdr) n++;
    sp[1] = scm_make_string_n(c, n, ' ');
    size_t i = 0;
    for (sp[2] = sp[0]; scm_is_pair(sp[2]); sp[2] = SCM_PTR(sp[2])->pair.cdr) {
        VALUE ch = SCM_PTR(sp[2])->pair.car;
        if (!scm_is_char(ch)) scm_error(c, "list->string: not a char");
        SCM_PTR(sp[1])->str.chars[i++] = (char)SCM_PTR(ch)->ch;
    }
    VALUE r = sp[1];
    SP_POP(c, sp);
    return r;
}
PRIM(string_form) {
    // (string c1 c2 ...) → string
    (void)c;
    VALUE r = scm_make_string_n(c, (size_t)argc, ' ');
    char *out = SCM_PTR(r)->str.chars;
    for (int i = 0; i < argc; i++) out[i] = (char)SCM_PTR(argv[i])->ch;
    return r;
}

PRIM(char_eq) {
    (void)c;
    for (int i = 1; i < argc; i++) if (SCM_PTR(argv[i-1])->ch != SCM_PTR(argv[i])->ch) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_lt) {
    (void)c;
    for (int i = 1; i < argc; i++) if (!(SCM_PTR(argv[i-1])->ch < SCM_PTR(argv[i])->ch)) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_le) {
    (void)c;
    for (int i = 1; i < argc; i++) if (!(SCM_PTR(argv[i-1])->ch <= SCM_PTR(argv[i])->ch)) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_gt) {
    (void)c;
    for (int i = 1; i < argc; i++) if (!(SCM_PTR(argv[i-1])->ch > SCM_PTR(argv[i])->ch)) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_ge) {
    (void)c;
    for (int i = 1; i < argc; i++) if (!(SCM_PTR(argv[i-1])->ch >= SCM_PTR(argv[i])->ch)) return SCM_FALSE;
    return SCM_TRUE;
}
PRIM(char_to_integer) { (void)c; (void)argc; return SCM_FIX(SCM_PTR(argv[0])->ch); }
PRIM(integer_to_char) { (void)c; (void)argc; return scm_make_char(c, (uint32_t)ARG_FIX(0)); }
PRIM(char_alphabetic_p) { (void)c; (void)argc; return isalpha(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_numeric_p)    { (void)c; (void)argc; return isdigit(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_whitespace_p) { (void)c; (void)argc; return isspace(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_upper_p)      { (void)c; (void)argc; return isupper(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_lower_p)      { (void)c; (void)argc; return islower(SCM_PTR(argv[0])->ch) ? SCM_TRUE : SCM_FALSE; }
PRIM(char_upcase)       { (void)c; (void)argc; return scm_make_char(c, toupper(SCM_PTR(argv[0])->ch)); }
PRIM(char_downcase)     { (void)c; (void)argc; return scm_make_char(c, tolower(SCM_PTR(argv[0])->ch)); }

PRIM(make_vector) {
    size_t n = (size_t)ARG_FIX(0);
    VALUE fill = argc >= 2 ? argv[1] : SCM_UNSPEC;
    (void)c;
    return scm_make_vector(c, n, fill);
}
PRIM(vector_form) {
    (void)c;
    VALUE r = scm_make_vector(c, (size_t)argc, SCM_UNSPEC);
    /* items[] backing is the holder; SCM_PTR(r)->vec.items - sizeof(header)
     * is the payload base.  Compute it once. */
    struct sobj *vobj = SCM_PTR(r);
    char *items_base = (char *)vobj->vec.items - sizeof(AroObjectHeader);
    for (int i = 0; i < argc; i++) {
        aro_gc_wb(c, items_base, &vobj->vec.items[i], argv[i]);
    }
    return r;
}
PRIM(vector_length) {
    (void)argc;
    if (!scm_is_vector(argv[0])) scm_error(c, "vector-length: not a vector");
    return SCM_FIX(SCM_PTR(argv[0])->vec.len);
}
PRIM(vector_ref) {
    (void)argc;
    if (!scm_is_vector(argv[0])) scm_error(c, "vector-ref: not a vector");
    size_t i = (size_t)ARG_FIX(1);
    if (i >= SCM_PTR(argv[0])->vec.len) scm_error(c, "vector-ref: out of range");
    return SCM_PTR(argv[0])->vec.items[i];
}
PRIM(vector_set) {
    (void)argc;
    if (!scm_is_vector(argv[0])) scm_error(c, "vector-set!: not a vector");
    size_t i = (size_t)ARG_FIX(1);
    struct sobj *vobj = SCM_PTR(argv[0]);
    if (i >= vobj->vec.len) scm_error(c, "vector-set!: out of range");
    char *items_base = (char *)vobj->vec.items - sizeof(AroObjectHeader);
    aro_gc_wb(c, items_base, &vobj->vec.items[i], argv[2]);
    return SCM_UNSPEC;
}
PRIM(vector_fill) {
    (void)argc;
    if (!scm_is_vector(argv[0])) scm_error(c, "vector-fill!: not a vector");
    struct sobj *vobj = SCM_PTR(argv[0]);
    char *items_base = (char *)vobj->vec.items - sizeof(AroObjectHeader);
    for (size_t i = 0; i < vobj->vec.len; i++) {
        aro_gc_wb(c, items_base, &vobj->vec.items[i], argv[1]);
    }
    return SCM_UNSPEC;
}
PRIM(vector_to_list) {
    (void)argc;
    /* sp[0]=vector sobj, sp[1]=accumulator r */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);
    sp[0] = argv[0];
    sp[1] = SCM_NIL;
    size_t vlen = SCM_PTR(sp[0])->vec.len;
    for (size_t i = vlen; i--; ) {
        /* Reload vector + items each iteration — moving GC may relocate
         * the vec sobj and the items[] byte payload underneath. */
        VALUE item = SCM_PTR(sp[0])->vec.items[i];
        sp[1] = scm_cons(c, item, sp[1]);
    }
    VALUE r = sp[1];
    SP_POP(c, sp);
    return r;
}
PRIM(list_to_vector) {
    (void)argc;
    /* sp[0]=list head, sp[1]=result vector, sp[2]=walking iter */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 3);
    sp[0] = argv[0];
    sp[1] = SCM_NIL;
    size_t n = 0;
    for (sp[2] = sp[0]; scm_is_pair(sp[2]); sp[2] = SCM_PTR(sp[2])->pair.cdr) n++;
    sp[1] = scm_make_vector(c, n, SCM_UNSPEC);
    size_t i = 0;
    {
        struct sobj *vobj = SCM_PTR(sp[1]);
        char *items_base = (char *)vobj->vec.items - sizeof(AroObjectHeader);
        for (sp[2] = sp[0]; scm_is_pair(sp[2]); sp[2] = SCM_PTR(sp[2])->pair.cdr) {
            /* No alloc inside the loop — vobj/items_base stay valid. */
            aro_gc_wb(c, items_base, &vobj->vec.items[i],
                      SCM_PTR(sp[2])->pair.car);
            i++;
        }
    }
    VALUE r = sp[1];
    SP_POP(c, sp);
    return r;
}

PRIM(display) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    scm_display(fp, argv[0], false);
    return SCM_UNSPEC;
}
PRIM(write) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    scm_display(fp, argv[0], true);
    return SCM_UNSPEC;
}
PRIM(newline) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    fputc('\n', fp);
    return SCM_UNSPEC;
}
PRIM(write_char) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    fputc((int)SCM_PTR(argv[0])->ch, fp);
    return SCM_UNSPEC;
}

PRIM(read_form) { (void)argc; (void)argv; return scm_read(c, stdin); }
PRIM(eof_p)     { (void)c; (void)argc; return argv[0] == SCM_EOFV ? SCM_TRUE : SCM_FALSE; }

PRIM(error_p)   { (void)argc; scm_error(c, "%s", scm_is_string(argv[0]) ? SCM_PTR(argv[0])->str.chars : "error"); }

PRIM(apply_p) {
    if (argc < 2) scm_error(c, "apply: needs at least 2 args");
    /* Park fn + the spread list across the size walk + scm_apply (= GC).
     *   sp[0]      = fn
     *   sp[1]      = spread list (= argv[argc-1])
     *   sp[2..]    = total VALUE slots = prefix args + spread-list items
     *                (collected before the scm_apply call so they're
     *                rooted as one contiguous argv passed to scm_apply). */
    int prefix = argc - 2;
    int extra = 0;
    for (VALUE p = argv[argc - 1]; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) extra++;
    int total = prefix + extra;
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2 + total);
    sp[0] = argv[0];
    sp[1] = argv[argc - 1];
    for (int i = 0; i < prefix; i++) sp[2 + i] = argv[1 + i];
    {
        int i = prefix;
        VALUE p = sp[1];
        while (scm_is_pair(p)) {
            sp[2 + i] = SCM_PTR(p)->pair.car;
            p = SCM_PTR(p)->pair.cdr;
            i++;
        }
    }
    VALUE r = scm_apply(c, sp[0], total, sp + 2);
    SP_POP(c, sp);
    return r;
}

// (map fn list1 list2 ...) — applies fn elementwise to len of shortest.
PRIM(map_p) {
    if (argc < 2) scm_error(c, "map: needs fn + list");
    int nlists = argc - 1;
    /* sp layout:
     *   sp[0]                = fn
     *   sp[1]                = result head (= forms list)
     *   sp[2]                = last appended cell
     *   sp[3 .. 3+nlists-1]  = list cursors
     *   sp[3+nlists .. ]     = per-call args[]
     */
    int args_off = 3 + nlists;
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, args_off + nlists);
    sp[0] = argv[0];
    sp[1] = SCM_NIL;
    sp[2] = SCM_NIL;
    for (int i = 0; i < nlists; i++) sp[3 + i] = argv[i + 1];
    for (;;) {
        for (int i = 0; i < nlists; i++) if (!scm_is_pair(sp[3 + i])) {
            VALUE r = sp[1];
            SP_POP(c, sp);
            return r;
        }
        for (int i = 0; i < nlists; i++) {
            sp[args_off + i] = SCM_PTR(sp[3 + i])->pair.car;
            sp[3 + i] = SCM_PTR(sp[3 + i])->pair.cdr;
        }
        VALUE v = scm_apply(c, sp[0], nlists, sp + args_off);
        VALUE cell = scm_cons(c, v, SCM_NIL);
        if (sp[1] == SCM_NIL) { sp[1] = cell; }
        else {
            struct sobj *last = SCM_PTR(sp[2]);
            aro_gc_wb(c, last, (VALUE *)&last->pair.cdr, cell);
        }
        sp[2] = cell;
    }
}

PRIM(for_each_p) {
    if (argc < 2) scm_error(c, "for-each: needs fn + list");
    int nlists = argc - 1;
    /* sp[0] = fn, sp[1..nlists] = cursors, sp[1+nlists..] = per-call args. */
    int args_off = 1 + nlists;
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, args_off + nlists);
    sp[0] = argv[0];
    for (int i = 0; i < nlists; i++) sp[1 + i] = argv[i + 1];
    for (;;) {
        for (int i = 0; i < nlists; i++) if (!scm_is_pair(sp[1 + i])) {
            SP_POP(c, sp);
            return SCM_UNSPEC;
        }
        for (int i = 0; i < nlists; i++) {
            sp[args_off + i] = SCM_PTR(sp[1 + i])->pair.car;
            sp[1 + i] = SCM_PTR(sp[1 + i])->pair.cdr;
        }
        scm_apply(c, sp[0], nlists, sp + args_off);
    }
}

#include <time.h>
PRIM(time_now) {
    (void)c; (void)argc; (void)argv;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return scm_make_double(c, ts.tv_sec + ts.tv_nsec * 1e-9);
}

PRIM(exit_p) {
    (void)c;
    int code = 0;
    if (argc > 0 && SCM_IS_FIXNUM(argv[0])) code = (int)SCM_FIXVAL(argv[0]);
    exit(code);
}

PRIM(gensym) {
    (void)argc; (void)argv;
    static int seq = 0;
    char buf[64]; snprintf(buf, sizeof(buf), "|g%d|", ++seq);
    (void)c;
    return scm_intern(c, buf);
}

// --- Promises (delay/force) -------------------------------------------------
//
// (delay expr) is lowered at compile time to (|make-promise| (lambda () expr));
// `force` runs the thunk on first call and caches the result thereafter.
// R5RS §6.4 allows `force` on a non-promise to act as identity.

PRIM(make_promise) {
    (void)argc;
    if (!scm_is_proc(argv[0])) scm_error(c, "delay: thunk must be a procedure");
    struct sobj *o = scm_alloc(c, OBJ_PROMISE);
    /* o is freshly young — WB fast-path returns immediately. */
    aro_gc_wb(c, o, (VALUE *)&o->promise.thunk, argv[0]);
    aro_gc_wb(c, o, (VALUE *)&o->promise.value, SCM_UNSPEC);
    o->promise.forced = false;
    return SCM_OBJ_VAL(o);
}

PRIM(force_p) {
    (void)argc;
    if (!scm_is_promise(argv[0])) return argv[0];
    if (SCM_PTR(argv[0])->promise.forced) return SCM_PTR(argv[0])->promise.value;
    /* Park the promise VALUE + the thunk result across the scm_apply call:
     * the thunk body allocates freely and a moving GC relocates the
     * promise sobj.  After scm_apply, reload via the parked slot. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);
    sp[0] = argv[0];                                  /* promise */
    sp[1] = SCM_PTR(sp[0])->promise.thunk;             /* thunk VALUE */
    VALUE result = scm_apply(c, sp[1], 0, NULL);
    struct sobj *po = SCM_PTR(sp[0]);
    /* Re-check after recursive force could have already memoized us.
     * po may be OLD by now (= long-lived promise); result is fresh/young.
     * WB is mandatory. */
    if (!po->promise.forced) {
        aro_gc_wb(c, po, (VALUE *)&po->promise.value, result);
        po->promise.forced = true;
    }
    VALUE r = po->promise.value;
    SP_POP(c, sp);
    return r;
}

PRIM(promise_p) { (void)c; (void)argc; return scm_is_promise(argv[0]) ? SCM_TRUE : SCM_FALSE; }

// --- Ports ------------------------------------------------------------------

// Non-static: install_prims (= main.c) calls this to set up PORT_STDIN /
// PORT_STDOUT / PORT_STDERR at startup.
VALUE
port_make(CTX *c, FILE *fp, bool input, bool owned)
{
    struct sobj *o = scm_alloc(c, OBJ_PORT);
    o->port.fp = fp;
    o->port.input = input;
    o->port.closed = false;
    o->port.owned = owned;
    return SCM_OBJ_VAL(o);
}

/* Stdports kept file-wide (instead of static) so aro_scheme_visit_roots
 * can see them.  They are heap-allocated port sobj's; the alias addresses
 * are program-static. */
VALUE PORT_STDIN  = 0;
VALUE PORT_STDOUT = 0;
VALUE PORT_STDERR = 0;

PRIM(open_input_file) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "open-input-file: not a string");
    FILE *fp = fopen(SCM_PTR(argv[0])->str.chars, "r");
    if (!fp) scm_error(c, "open-input-file: cannot open '%s'", SCM_PTR(argv[0])->str.chars);
    return port_make(c, fp, true, true);
}
PRIM(open_output_file) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "open-output-file: not a string");
    FILE *fp = fopen(SCM_PTR(argv[0])->str.chars, "w");
    if (!fp) scm_error(c, "open-output-file: cannot open '%s'", SCM_PTR(argv[0])->str.chars);
    return port_make(c, fp, false, true);
}
PRIM(close_input_port) {
    (void)argc;
    if (scm_is_port(argv[0])) {
        struct sobj *p = SCM_PTR(argv[0]);
        if (p->port.owned && !p->port.closed) { fclose(p->port.fp); p->port.closed = true; }
    }
    (void)c; return SCM_UNSPEC;
}
PRIM(close_output_port) { return prim_close_input_port(c, argc, argv); }
PRIM(input_port_p)  { (void)c; (void)argc; return scm_is_port(argv[0]) && SCM_PTR(argv[0])->port.input  ? SCM_TRUE : SCM_FALSE; }
PRIM(output_port_p) { (void)c; (void)argc; return scm_is_port(argv[0]) && !SCM_PTR(argv[0])->port.input ? SCM_TRUE : SCM_FALSE; }
PRIM(port_p)        { (void)c; (void)argc; return scm_is_port(argv[0]) ? SCM_TRUE : SCM_FALSE; }
PRIM(current_input_port)  { (void)c; (void)argc; (void)argv; return PORT_STDIN; }
PRIM(current_output_port) { (void)c; (void)argc; (void)argv; return PORT_STDOUT; }
PRIM(read_char) {
    FILE *fp = stdin;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    int ch = fgetc(fp);
    return ch == EOF ? SCM_EOFV : scm_make_char(c, (uint32_t)ch);
}
PRIM(peek_char) {
    FILE *fp = stdin;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    int ch = fgetc(fp);
    if (ch == EOF) return SCM_EOFV;
    ungetc(ch, fp);
    return scm_make_char(c, (uint32_t)ch);
}
PRIM(char_ready_p) { (void)c; (void)argc; (void)argv; return SCM_TRUE; }   // simplified
PRIM(write_to_port) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    scm_display(fp, argv[0], true);
    return SCM_UNSPEC;
}
PRIM(display_to_port) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    scm_display(fp, argv[0], false);
    return SCM_UNSPEC;
}
PRIM(newline_to_port) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    fputc('\n', fp);
    return SCM_UNSPEC;
}
PRIM(write_char_to_port) {
    (void)c;
    FILE *fp = stdout;
    if (argc >= 2 && scm_is_port(argv[1])) fp = SCM_PTR(argv[1])->port.fp;
    fputc((int)SCM_PTR(argv[0])->ch, fp);
    return SCM_UNSPEC;
}
PRIM(read_from_port) {
    FILE *fp = stdin;
    if (argc >= 1 && scm_is_port(argv[0])) fp = SCM_PTR(argv[0])->port.fp;
    return scm_read(c, fp);
}
// Re-route stdin / stdout for the duration of `thunk`'s execution.  We
// save the original fd via dup() and restore via dup2() so the redirection
// is reverted on return — `freopen` would have left it permanently.
#include <fcntl.h>
#include <unistd.h>

PRIM(with_input_from_file) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "with-input-from-file: not a string");
    if (!scm_is_proc(argv[1]))   scm_error(c, "with-input-from-file: not a procedure");
    int new_fd = open(SCM_PTR(argv[0])->str.chars, O_RDONLY);
    if (new_fd < 0) scm_error(c, "with-input-from-file: cannot open '%s'", SCM_PTR(argv[0])->str.chars);
    int saved = dup(fileno(stdin));
    dup2(new_fd, fileno(stdin)); close(new_fd);
    VALUE r = scm_apply(c, argv[1], 0, NULL);
    dup2(saved, fileno(stdin)); close(saved);
    clearerr(stdin);
    return r;
}
PRIM(with_output_to_file) {
    (void)argc;
    if (!scm_is_string(argv[0])) scm_error(c, "with-output-to-file: not a string");
    if (!scm_is_proc(argv[1]))   scm_error(c, "with-output-to-file: not a procedure");
    fflush(stdout);
    int new_fd = open(SCM_PTR(argv[0])->str.chars, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (new_fd < 0) scm_error(c, "with-output-to-file: cannot open '%s'", SCM_PTR(argv[0])->str.chars);
    int saved = dup(fileno(stdout));
    dup2(new_fd, fileno(stdout)); close(new_fd);
    VALUE r = scm_apply(c, argv[1], 0, NULL);
    fflush(stdout);
    dup2(saved, fileno(stdout)); close(saved);
    clearerr(stdout);
    return r;
}

// --- Multiple values --------------------------------------------------------

PRIM(values_p) {
    (void)c;
    if (argc == 1) return argv[0];
    return scm_make_mvalues(c, argc, argv);
}

PRIM(call_with_values_p) {
    (void)argc;
    /* Park producer + consumer + r across the inner scm_apply calls so a
     * moving GC can relocate them.  argv[] is the caller's frame slots
     * (rooted) but C-local copies + the scm_apply(producer) intermediate
     * result are not. */
    if (!scm_is_proc(argv[0])) scm_error(c, "call-with-values: producer not a procedure");
    if (!scm_is_proc(argv[1])) scm_error(c, "call-with-values: consumer not a procedure");
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);
    sp[0] = argv[1];                       /* consumer */
    sp[1] = scm_apply(c, argv[0], 0, NULL); /* producer result */
    VALUE r;
    if (scm_is_mvalues(sp[1])) {
        struct sobj *m = SCM_PTR(sp[1]);
        r = scm_apply(c, sp[0], (int)m->mv.len, m->mv.items);
    } else {
        /* single-value path: hand the value to scm_apply as argv (it
         * parks via build_frame_for before any inner alloc). */
        r = scm_apply(c, sp[0], 1, &sp[1]);
    }
    SP_POP(c, sp);
    return r;
}

// --- Complex numbers --------------------------------------------------------

PRIM(make_rectangular) {
    (void)argc; (void)c;
    return scm_simplify_complex(c, scm_get_double(argv[0]), scm_get_double(argv[1]));
}
PRIM(make_polar) {
    (void)argc; (void)c;
    double m = scm_get_double(argv[0]);
    double a = scm_get_double(argv[1]);
    return scm_simplify_complex(c, m * cos(a), m * sin(a));
}
PRIM(real_part) {
    (void)argc;
    if (scm_is_complex(argv[0])) return scm_make_double(c, SCM_PTR(argv[0])->cpx.re);
    if (scm_is_real(argv[0])) return argv[0];
    scm_error(c, "real-part: not a number");
}
PRIM(imag_part) {
    (void)argc;
    if (scm_is_complex(argv[0])) return scm_make_double(c, SCM_PTR(argv[0])->cpx.im);
    if (scm_is_real(argv[0])) return SCM_FIX(0);
    scm_error(c, "imag-part: not a number");
}
PRIM(magnitude) {
    (void)argc;
    if (scm_is_complex(argv[0])) {
        double r = SCM_PTR(argv[0])->cpx.re, i = SCM_PTR(argv[0])->cpx.im;
        return scm_make_double(c, sqrt(r*r + i*i));
    }
    return prim_abs(c, argc, argv);
}
PRIM(angle) {
    (void)argc;
    if (scm_is_complex(argv[0])) {
        double r = SCM_PTR(argv[0])->cpx.re, i = SCM_PTR(argv[0])->cpx.im;
        return scm_make_double(c, atan2(i, r));
    }
    if (cmp2(c, argv[0], SCM_FIX(0)) >= 0) return scm_make_double(c, 0.0);
    return scm_make_double(c, M_PI);
}

// --- Rational accessors -----------------------------------------------------

PRIM(numerator_p) {
    (void)argc;
    if (SCM_IS_FIXNUM(argv[0]) || scm_is_bignum(argv[0])) return argv[0];
    if (scm_is_rational(argv[0])) return scm_normalize_int(c, mpq_numref(SCM_PTR(argv[0])->mpq));
    scm_error(c, "numerator: not a rational");
}
PRIM(denominator_p) {
    (void)argc;
    if (SCM_IS_FIXNUM(argv[0]) || scm_is_bignum(argv[0])) return SCM_FIX(1);
    if (scm_is_rational(argv[0])) return scm_normalize_int(c, mpq_denref(SCM_PTR(argv[0])->mpq));
    scm_error(c, "denominator: not a rational");
}

// Simple math primitives.
PRIM(sqrt_p) { (void)c; (void)argc; return scm_make_double(c, sqrt(scm_get_double(argv[0]))); }
PRIM(expt_p) {
    (void)argc;
    // exact integer base + non-negative exact integer exponent → exact via mpz_pow_ui
    if (scm_is_exact(argv[0]) && scm_is_integer_value(argv[0]) &&
        scm_is_exact(argv[1]) && scm_is_integer_value(argv[1]) &&
        cmp2(c, argv[1], SCM_FIX(0)) >= 0)
    {
        mpz_t base, r;
        to_mpz(argv[0], base); mpz_init(r);
        unsigned long e;
        if (SCM_IS_FIXNUM(argv[1])) e = (unsigned long)SCM_FIXVAL(argv[1]);
        else if (mpz_fits_ulong_p(SCM_PTR(argv[1])->mpz)) e = mpz_get_ui(SCM_PTR(argv[1])->mpz);
        else { mpz_clear(base); mpz_clear(r); scm_error(c, "expt: exponent too large"); }
        mpz_pow_ui(r, base, e);
        VALUE rv = scm_normalize_int(c, r);
        mpz_clear(base); mpz_clear(r);
        return rv;
    }
    return scm_make_double(c, pow(scm_get_double(argv[0]), scm_get_double(argv[1])));
}
PRIM(floor_p)    { (void)c; (void)argc; return scm_make_double(c, floor(scm_get_double(argv[0]))); }
PRIM(ceiling_p)  { (void)c; (void)argc; return scm_make_double(c, ceil(scm_get_double(argv[0]))); }
PRIM(truncate_p) { (void)c; (void)argc; return scm_make_double(c, trunc(scm_get_double(argv[0]))); }
PRIM(round_p)    { (void)c; (void)argc; return scm_make_double(c, round(scm_get_double(argv[0]))); }
PRIM(log_p)      { (void)c; (void)argc; return scm_make_double(c, log(scm_get_double(argv[0]))); }
PRIM(exp_p)      { (void)c; (void)argc; return scm_make_double(c, exp(scm_get_double(argv[0]))); }
PRIM(sin_p)      { (void)c; (void)argc; return scm_make_double(c, sin(scm_get_double(argv[0]))); }
PRIM(cos_p)      { (void)c; (void)argc; return scm_make_double(c, cos(scm_get_double(argv[0]))); }
PRIM(tan_p)      { (void)c; (void)argc; return scm_make_double(c, tan(scm_get_double(argv[0]))); }
PRIM(atan_p)     { (void)c; (void)argc; return scm_make_double(c, atan(scm_get_double(argv[0]))); }

// car/cdr compositions: caar, cadr, ..., caddr.  Generated via macros.
#define CADR_OP(x) cdr(x)
#define CAAR_OP(x) car(x)
PRIM(caar) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(argv[0])->pair.car)->pair.car; }
PRIM(cadr_p) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.car; }
PRIM(cdar) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(argv[0])->pair.car)->pair.cdr; }
PRIM(cddr) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.cdr; }
PRIM(caddr_p) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.cdr)->pair.car; }
PRIM(cdddr_p) { (void)c; (void)argc; return SCM_PTR(SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.cdr)->pair.cdr; }
PRIM(cadddr_p){ (void)c; (void)argc; return SCM_PTR(SCM_PTR(SCM_PTR(SCM_PTR(argv[0])->pair.cdr)->pair.cdr)->pair.cdr)->pair.car; }

// ---------------------------------------------------------------------------
// Primitive table.
// ---------------------------------------------------------------------------

struct prim_entry PRIM_TABLE[] = {
    { "+", prim_plus, 0, -1 },
    { "-", prim_minus, 1, -1 },
    { "*", prim_mul, 0, -1 },
    { "/", prim_div, 1, -1 },
    { "=", prim_num_eq, 2, -1 },
    { "<", prim_num_lt, 2, -1 },
    { ">", prim_num_gt, 2, -1 },
    { "<=", prim_num_le, 2, -1 },
    { ">=", prim_num_ge, 2, -1 },
    { "modulo", prim_modulo, 2, 2 },
    { "remainder", prim_remainder, 2, 2 },
    { "quotient", prim_quotient, 2, 2 },
    { "abs", prim_abs, 1, 1 },
    { "min", prim_min, 1, -1 },
    { "max", prim_max, 1, -1 },
    { "zero?", prim_zero_p, 1, 1 },
    { "positive?", prim_positive_p, 1, 1 },
    { "negative?", prim_negative_p, 1, 1 },
    { "odd?", prim_odd_p, 1, 1 },
    { "even?", prim_even_p, 1, 1 },
    { "number?", prim_number_p, 1, 1 },
    { "integer?", prim_integer_p, 1, 1 },
    { "real?", prim_real_p, 1, 1 },
    { "rational?", prim_rational_p, 1, 1 },
    { "complex?", prim_complex_p, 1, 1 },
    { "exact?", prim_exact_p, 1, 1 },
    { "inexact?", prim_inexact_p, 1, 1 },
    { "exact->inexact", prim_exact_to_inexact, 1, 1 },
    { "inexact->exact", prim_inexact_to_exact, 1, 1 },
    { "expt", prim_expt_p, 2, 2 },
    { "gcd", prim_gcd_p, 0, -1 },
    { "lcm", prim_lcm_p, 0, -1 },
    { "numerator", prim_numerator_p, 1, 1 },
    { "denominator", prim_denominator_p, 1, 1 },
    { "make-rectangular", prim_make_rectangular, 2, 2 },
    { "make-polar", prim_make_polar, 2, 2 },
    { "real-part", prim_real_part, 1, 1 },
    { "imag-part", prim_imag_part, 1, 1 },
    { "magnitude", prim_magnitude, 1, 1 },
    { "angle", prim_angle, 1, 1 },
    { "values", prim_values_p, 0, -1 },
    { "call-with-values", prim_call_with_values_p, 2, 2 },
    { "|make-promise|", prim_make_promise, 1, 1 },
    { "force", prim_force_p, 1, 1 },
    { "promise?", prim_promise_p, 1, 1 },
    { "open-input-file", prim_open_input_file, 1, 1 },
    { "open-output-file", prim_open_output_file, 1, 1 },
    { "close-input-port", prim_close_input_port, 1, 1 },
    { "close-output-port", prim_close_output_port, 1, 1 },
    { "close-port", prim_close_input_port, 1, 1 },
    { "input-port?", prim_input_port_p, 1, 1 },
    { "output-port?", prim_output_port_p, 1, 1 },
    { "port?", prim_port_p, 1, 1 },
    { "current-input-port", prim_current_input_port, 0, 0 },
    { "current-output-port", prim_current_output_port, 0, 0 },
    { "read-char", prim_read_char, 0, 1 },
    { "peek-char", prim_peek_char, 0, 1 },
    { "char-ready?", prim_char_ready_p, 0, 1 },
    { "with-input-from-file", prim_with_input_from_file, 2, 2 },
    { "with-output-to-file", prim_with_output_to_file, 2, 2 },
    { "sqrt", prim_sqrt_p, 1, 1 },
    { "floor", prim_floor_p, 1, 1 },
    { "ceiling", prim_ceiling_p, 1, 1 },
    { "truncate", prim_truncate_p, 1, 1 },
    { "round", prim_round_p, 1, 1 },
    { "log", prim_log_p, 1, 1 },
    { "exp", prim_exp_p, 1, 1 },
    { "sin", prim_sin_p, 1, 1 },
    { "cos", prim_cos_p, 1, 1 },
    { "tan", prim_tan_p, 1, 1 },
    { "atan", prim_atan_p, 1, 1 },
    { "cons", prim_cons, 2, 2 },
    { "car", prim_car, 1, 1 },
    { "cdr", prim_cdr, 1, 1 },
    { "set-car!", prim_set_car, 2, 2 },
    { "set-cdr!", prim_set_cdr, 2, 2 },
    { "pair?", prim_pair_p, 1, 1 },
    { "null?", prim_null_p, 1, 1 },
    { "list", prim_list, 0, -1 },
    { "list?", prim_list_p, 1, 1 },
    { "length", prim_length, 1, 1 },
    { "reverse", prim_reverse, 1, 1 },
    { "append", prim_append, 0, -1 },
    { "list-ref", prim_list_ref, 2, 2 },
    { "list-tail", prim_list_tail, 2, 2 },
    { "caar", prim_caar, 1, 1 },
    { "cadr", prim_cadr_p, 1, 1 },
    { "cdar", prim_cdar, 1, 1 },
    { "cddr", prim_cddr, 1, 1 },
    { "caddr", prim_caddr_p, 1, 1 },
    { "cdddr", prim_cdddr_p, 1, 1 },
    { "cadddr", prim_cadddr_p, 1, 1 },
    { "eq?", prim_eq_p, 2, 2 },
    { "eqv?", prim_eqv_p, 2, 2 },
    { "equal?", prim_equal_p, 2, 2 },
    { "memq", prim_memq, 2, 2 },
    { "memv", prim_memv, 2, 2 },
    { "member", prim_member, 2, 2 },
    { "assq", prim_assq, 2, 2 },
    { "assv", prim_assv, 2, 2 },
    { "assoc", prim_assoc, 2, 2 },
    { "boolean?", prim_boolean_p, 1, 1 },
    { "not", prim_not_p, 1, 1 },
    { "symbol?", prim_symbol_p, 1, 1 },
    { "procedure?", prim_procedure_p, 1, 1 },
    { "string?", prim_string_p, 1, 1 },
    { "char?", prim_char_p, 1, 1 },
    { "vector?", prim_vector_p, 1, 1 },
    { "symbol->string", prim_symbol_to_string, 1, 1 },
    { "string->symbol", prim_string_to_symbol, 1, 1 },
    { "number->string", prim_number_to_string, 1, 2 },
    { "string->number", prim_string_to_number, 1, 2 },
    { "string-length", prim_string_length, 1, 1 },
    { "string-ref", prim_string_ref, 2, 2 },
    { "string=?", prim_string_eq, 2, -1 },
    { "string<?", prim_string_lt, 2, -1 },
    { "string>?", prim_string_gt, 2, -1 },
    { "string<=?", prim_string_le, 2, -1 },
    { "string>=?", prim_string_ge, 2, -1 },
    { "string-ci=?", prim_string_ci_eq, 2, -1 },
    { "char-ci=?", prim_char_ci_eq, 2, -1 },
    { "string-copy", prim_string_copy, 1, 1 },
    { "string-set!", prim_string_set, 3, 3 },
    { "make-string", prim_make_string, 1, 2 },
    { "string-append", prim_string_append, 0, -1 },
    { "substring", prim_substring, 3, 3 },
    { "string->list", prim_string_to_list, 1, 1 },
    { "list->string", prim_list_to_string, 1, 1 },
    { "string", prim_string_form, 0, -1 },
    { "char=?", prim_char_eq, 2, -1 },
    { "char<?", prim_char_lt, 2, -1 },
    { "char<=?", prim_char_le, 2, -1 },
    { "char>?", prim_char_gt, 2, -1 },
    { "char>=?", prim_char_ge, 2, -1 },
    { "char->integer", prim_char_to_integer, 1, 1 },
    { "integer->char", prim_integer_to_char, 1, 1 },
    { "char-alphabetic?", prim_char_alphabetic_p, 1, 1 },
    { "char-numeric?", prim_char_numeric_p, 1, 1 },
    { "char-whitespace?", prim_char_whitespace_p, 1, 1 },
    { "char-upper-case?", prim_char_upper_p, 1, 1 },
    { "char-lower-case?", prim_char_lower_p, 1, 1 },
    { "char-upcase", prim_char_upcase, 1, 1 },
    { "char-downcase", prim_char_downcase, 1, 1 },
    { "make-vector", prim_make_vector, 1, 2 },
    { "vector", prim_vector_form, 0, -1 },
    { "vector-length", prim_vector_length, 1, 1 },
    { "vector-ref", prim_vector_ref, 2, 2 },
    { "vector-set!", prim_vector_set, 3, 3 },
    { "vector-fill!", prim_vector_fill, 2, 2 },
    { "vector->list", prim_vector_to_list, 1, 1 },
    { "list->vector", prim_list_to_vector, 1, 1 },
    { "display", prim_display, 1, 2 },
    { "write", prim_write, 1, 2 },
    { "newline", prim_newline, 0, 1 },
    { "write-char", prim_write_char, 1, 2 },
    { "read", prim_read_from_port, 0, 1 },
    { "eof-object?", prim_eof_p, 1, 1 },
    { "error", prim_error_p, 1, -1 },
    { "apply", prim_apply_p, 2, -1 },
    { "map", prim_map_p, 2, -1 },
    { "for-each", prim_for_each_p, 2, -1 },
    { "current-time", prim_time_now, 0, 0 },
    { "exit", prim_exit_p, 0, 1 },
    { "gensym", prim_gensym, 0, 0 },
    { NULL, NULL, 0, 0 }
};

VALUE PRIM_PLUS_VAL, PRIM_MINUS_VAL, PRIM_MUL_VAL;
VALUE PRIM_NUM_LT_VAL, PRIM_NUM_LE_VAL, PRIM_NUM_GT_VAL, PRIM_NUM_GE_VAL, PRIM_NUM_EQ_VAL;
VALUE PRIM_NULL_P_VAL, PRIM_PAIR_P_VAL, PRIM_CAR_VAL, PRIM_CDR_VAL, PRIM_NOT_VAL;
VALUE PRIM_VECTOR_REF_VAL, PRIM_VECTOR_SET_VAL;
VALUE PRIM_CONS_VAL, PRIM_EQ_P_VAL, PRIM_EQV_P_VAL;
