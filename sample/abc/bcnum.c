// Arbitrary-precision decimal arithmetic for abc (see bcnum.h).
#include <ctype.h>
#include <gc.h>
#include "bcnum.h"

// --- allocation -------------------------------------------------------
// A heap bcnum is scanned GC memory (it holds the mpz limb pointer); the
// GMP limbs themselves are GC_malloc_atomic (set up in node.c::INIT).
// Scale-0 small integers are kept as tagged immediates and never reach
// the allocator (see bcnum.h).

static bcnum *
heap_new(void)
{
    bcnum *const r = (bcnum *)GC_MALLOC(sizeof(bcnum));
    mpz_init(r->m);
    r->scale = 0;
    return r;
}

// Demote a freshly computed heap value to an immediate when it is a
// scale-0 integer in fixnum range.  Keeping the invariant "every small
// scale-0 integer is an immediate" maximises fast-path hits downstream.
static bcnum *
finalize(bcnum *r)
{
    if (r->scale == 0 && mpz_fits_slong_p(r->m)) {
        long v = mpz_get_si(r->m);
        if (bc_fits_fix(v)) return bc_mkfix(v);
    }
    return r;
}

// Materialise any value (possibly an immediate) as a heap bcnum.
static bcnum *
to_heap(const bcnum *v)
{
    if (!BC_IS_FIX(v)) return (bcnum *)v;
    bcnum *r = heap_new();
    mpz_set_si(r->m, BC_FIX_VAL(v));
    return r;
}

bcnum *bc_alloc(void) { return bc_mkfix(0); }

bcnum *
bc_from_long(long v)
{
    if (bc_fits_fix(v)) return bc_mkfix(v);
    bcnum *const r = heap_new();
    mpz_set_si(r->m, v);
    return r;
}

bcnum *
bc_copy(const bcnum *const a)
{
    if (BC_IS_FIX(a)) return (bcnum *)a;     // immutable immediate: share
    bcnum *const r = heap_new();
    mpz_set(r->m, a->m);
    r->scale = a->scale;
    return r;
}

// --- predicates -------------------------------------------------------

int  bc_sign(const bcnum *const a) { if (BC_IS_FIX(a)) { long v = BC_FIX_VAL(a); return (v > 0) - (v < 0); } return mpz_sgn(a->m); }
bool bc_is_zero(const bcnum *const a) { return BC_IS_FIX(a) ? BC_FIX_VAL(a) == 0 : mpz_sgn(a->m) == 0; }
bool bc_truthy(const bcnum *const a) { return !bc_is_zero(a); }
long bc_scale(const bcnum *const a) { return BC_IS_FIX(a) ? 0 : a->scale; }

// 10^k as an mpz (k >= 0).
static void
bc_pow10(mpz_t out, unsigned long k)
{
    mpz_ui_pow_ui(out, 10, k);
}

// --- rescale (truncating toward zero) ---------------------------------

bcnum *
bc_rescale(const bcnum *const a, long newscale)
{
    if (newscale < 0) newscale = 0;
    const long as = bc_scale(a);
    if (newscale == as) return (bcnum *)a;   // immutable: no copy needed

    bcnum *const h = to_heap(a);
    bcnum *const r = heap_new();
    r->scale = newscale;
    if (newscale > as) {
        mpz_t f; mpz_init(f);
        bc_pow10(f, (unsigned long)(newscale - as));
        mpz_mul(r->m, h->m, f);
        mpz_clear(f);
    }
    else {
        mpz_t f; mpz_init(f);
        bc_pow10(f, (unsigned long)(as - newscale));
        mpz_tdiv_q(r->m, h->m, f);   // truncate toward zero
        mpz_clear(f);
    }
    return r;
}

bcnum *
bc_neg(const bcnum *const a)
{
    if (BC_IS_FIX(a)) {
        const long v = BC_FIX_VAL(a);
        if (bc_fits_fix(-v)) return bc_mkfix(-v);
    }
    bcnum *const h = to_heap(a);
    bcnum *const r = heap_new();
    mpz_neg(r->m, h->m);
    r->scale = h->scale;
    return r;
}

// --- add / sub : scale = max(scale a, scale b) ------------------------

static bcnum *
addsub(const bcnum *const a, const bcnum *const b, bool sub)
{
    if (BC_IS_FIX(a) && BC_IS_FIX(b)) {        // fast path: native add/sub
        long r;
        const bool of = sub ? __builtin_sub_overflow(BC_FIX_VAL(a), BC_FIX_VAL(b), &r)
                            : __builtin_add_overflow(BC_FIX_VAL(a), BC_FIX_VAL(b), &r);
        if (!of && bc_fits_fix(r)) return bc_mkfix(r);
    }
    const long s = bc_scale(a) > bc_scale(b) ? bc_scale(a) : bc_scale(b);
    bcnum *const ax = to_heap(bc_rescale(a, s));
    bcnum *const bx = to_heap(bc_rescale(b, s));
    bcnum *const r = heap_new();
    r->scale = s;
    if (sub) mpz_sub(r->m, ax->m, bx->m);
    else     mpz_add(r->m, ax->m, bx->m);
    return finalize(r);
}

bcnum *bc_add(const bcnum *const a, const bcnum *const b) { return addsub(a, b, false); }
bcnum *bc_sub(const bcnum *const a, const bcnum *const b) { return addsub(a, b, true); }

// --- mul : scale = min(sa+sb, max(S, sa, sb)) -------------------------

bcnum *
bc_mul(const bcnum *const a, const bcnum *const b, long S)
{
    if (BC_IS_FIX(a) && BC_IS_FIX(b)) {        // fast path: scale 0, exact
        long r;
        if (!__builtin_mul_overflow(BC_FIX_VAL(a), BC_FIX_VAL(b), &r) && bc_fits_fix(r))
            return bc_mkfix(r);
    }
    const long sa = bc_scale(a), sb = bc_scale(b);
    const long raw = sa + sb;
    long target = sa > sb ? sa : sb;
    if (S > target) target = S;
    if (raw < target) target = raw;

    bcnum *r = heap_new();
    r->scale = raw;
    mpz_mul(r->m, to_heap(a)->m, to_heap(b)->m);
    if (target < raw) r = bc_rescale(r, target);   // truncate product
    return finalize(r);
}

// --- div : scale = S, truncate toward zero ----------------------------

bcnum *
bc_div(CTX *const c, const bcnum *const a, const bcnum *const b, long S)
{
    if (bc_is_zero(b)) { bc_runtime_error(c, "Divide by zero"); return bc_alloc(); }
    if (S < 0) S = 0;

    if (BC_IS_FIX(a) && BC_IS_FIX(b) && S == 0) {   // fast path: integer trunc
        const long va = BC_FIX_VAL(a), vb = BC_FIX_VAL(b);
        if (!(va == BC_FIX_MIN && vb == -1))        // avoid overflow corner
            return bc_mkfix(va / vb);               // C truncates toward zero
    }

    const bcnum *const ah = to_heap(a);
    const bcnum *const bh = to_heap(b);
    // result = trunc( a/b * 10^S ) = trunc( ma * 10^(S + sb - sa) / mb )
    const long e = S + bh->scale - ah->scale;
    mpz_t num; mpz_init_set(num, ah->m);
    if (e >= 0) {
        mpz_t f; mpz_init(f); bc_pow10(f, (unsigned long)e);
        mpz_mul(num, num, f);
        mpz_clear(f);
    }
    else {
        mpz_t f; mpz_init(f); bc_pow10(f, (unsigned long)(-e));
        mpz_tdiv_q(num, num, f);
        mpz_clear(f);
    }
    bcnum *const r = heap_new();
    r->scale = S;
    mpz_tdiv_q(r->m, num, bh->m);   // truncate toward zero
    mpz_clear(num);
    return finalize(r);
}

// --- mod : a - (a/b)*b, scale = max(S + sb, sa) -----------------------

bcnum *
bc_mod(CTX *const c, const bcnum *const a, const bcnum *const b, long S)
{
    if (bc_is_zero(b)) { bc_runtime_error(c, "Modulo by zero"); return bc_alloc(); }

    if (BC_IS_FIX(a) && BC_IS_FIX(b) && S == 0) {   // fast path: C % matches bc at S=0
        const long va = BC_FIX_VAL(a), vb = BC_FIX_VAL(b);
        if (!(va == BC_FIX_MIN && vb == -1))
            return bc_mkfix(va % vb);
    }

    bcnum *const q = bc_div(c, a, b, S);     // truncated quotient at scale S
    const bcnum *const bh = to_heap(b);
    // exact q*b
    bcnum *const qb = heap_new();
    qb->scale = bc_scale(q) + bh->scale;
    mpz_mul(qb->m, to_heap(q)->m, bh->m);
    return bc_sub(a, qb);                     // scale = max(sa, S+sb)
}

// --- pow : integer exponent ------------------------------------------

bcnum *
bc_pow(CTX *const c, const bcnum *const a, const bcnum *const b, long S)
{
    // exponent truncated to integer
    bcnum *const bi = bc_rescale(b, 0);
    if (!BC_IS_FIX(bi) && !mpz_fits_slong_p(bi->m)) {
        bc_runtime_error(c, "exponent too large");
        return bc_alloc();
    }
    long e = bc_to_long(bi);

    if (e == 0) return bc_from_long(1);

    const unsigned long ae = (unsigned long)(e < 0 ? -e : e);
    const bcnum *const ah = to_heap(a);

    // exact a^ae : mant^ae, scale = sa*ae
    bcnum *p = heap_new();
    mpz_pow_ui(p->m, ah->m, ae);
    p->scale = ah->scale * (long)ae;

    if (e > 0) {
        // scale = min(sa*e, max(S, sa))
        long target = ah->scale > S ? ah->scale : S;
        if (p->scale < target) target = p->scale;
        if (target < p->scale) p = bc_rescale(p, target);
        return finalize(p);
    }
    else {
        // a^e = 1 / a^ae, computed at scale S
        if (bc_is_zero(a)) { bc_runtime_error(c, "Divide by zero"); return bc_alloc(); }
        bcnum *const one = bc_from_long(1);
        return bc_div(c, one, p, S);
    }
}

// --- comparison -------------------------------------------------------

int
bc_cmp(const bcnum *const a, const bcnum *const b)
{
    if (BC_IS_FIX(a) && BC_IS_FIX(b)) {        // fast path
        const long va = BC_FIX_VAL(a), vb = BC_FIX_VAL(b);
        return (va > vb) - (va < vb);
    }
    const long s = bc_scale(a) > bc_scale(b) ? bc_scale(a) : bc_scale(b);
    bcnum *const ax = to_heap(bc_rescale(a, s));
    bcnum *const bx = to_heap(bc_rescale(b, s));
    const int r = mpz_cmp(ax->m, bx->m);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

// --- length / sqrt / to_long -----------------------------------------

long
bc_length(const bcnum *const a)
{
    if (BC_IS_FIX(a)) {
        long v = BC_FIX_VAL(a);
        if (v == 0) return 1;
        long d = 0;
        for (unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v; u; u /= 10) d++;
        return d;
    }
    long digits;
    if (mpz_sgn(a->m) == 0) digits = 0;
    else {
        mpz_t t; mpz_init(t); mpz_abs(t, a->m);
        digits = (long)mpz_sizeinbase(t, 10);
        // mpz_sizeinbase may overestimate by one for non-power-of-2 bases.
        mpz_t hi; mpz_init(hi); bc_pow10(hi, (unsigned long)(digits - 1));
        if (mpz_cmp(t, hi) < 0) digits--;
        mpz_clear(hi); mpz_clear(t);
    }
    long L = digits > a->scale ? digits : a->scale;
    return L > 0 ? L : 1;
}

bcnum *
bc_sqrt(CTX *const c, const bcnum *const a, long S)
{
    if (bc_sign(a) < 0) { bc_runtime_error(c, "Square root of a negative number"); return bc_alloc(); }
    if (bc_is_zero(a)) return bc_alloc();

    const bcnum *const ah = to_heap(a);
    long rscale = ah->scale > S ? ah->scale : S;   // bc: max(scale, scale(a))
    // r = floor( sqrt( ma * 10^(2*rscale - sa) ) )
    const long e = 2 * rscale - ah->scale;
    mpz_t n; mpz_init_set(n, ah->m);
    if (e >= 0) {
        mpz_t f; mpz_init(f); bc_pow10(f, (unsigned long)e);
        mpz_mul(n, n, f); mpz_clear(f);
    }
    else {
        mpz_t f; mpz_init(f); bc_pow10(f, (unsigned long)(-e));
        mpz_tdiv_q(n, n, f); mpz_clear(f);
    }
    bcnum *const r = heap_new();
    r->scale = rscale;
    mpz_sqrt(r->m, n);     // floor of integer square root
    mpz_clear(n);
    return finalize(r);
}

long
bc_to_long(const bcnum *const a)
{
    if (BC_IS_FIX(a)) return BC_FIX_VAL(a);
    bcnum *const t = bc_rescale(a, 0);
    if (BC_IS_FIX(t)) return BC_FIX_VAL(t);
    if (!mpz_fits_slong_p(t->m)) {
        return mpz_sgn(t->m) < 0 ? LONG_MIN : LONG_MAX;
    }
    return mpz_get_si(t->m);
}

// --- literal parsing --------------------------------------------------

static int
digit_value(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 10;
    return -1;
}

bcnum *
bc_parse_literal(const char *text, long ibase)
{
    // Split integer / fractional digit strings.
    const char *dot = strchr(text, '.');

    bool has_letter = false;
    for (const char *p = text; *p; p++) if (*p >= 'A' && *p <= 'Z') { has_letter = true; break; }

    // Fastest path: short decimal integer -> immediate, no GMP.
    if (ibase == 10 && !has_letter && !dot) {
        size_t len = strlen(text);
        if (len > 0 && len <= 18) {   // <= 18 digits always fits a fixnum
            long v = 0; bool ok = true;
            for (const char *p = text; *p; p++) {
                if (*p < '0' || *p > '9') { ok = false; break; }
                v = v * 10 + (*p - '0');
            }
            if (ok) return bc_mkfix(v);
        }
    }

    bcnum *const r = heap_new();

    if (ibase == 10 && !has_letter) {
        // Exact decimal path: mant = all digits, scale = #frac digits.
        char buf[4096];
        int bi = 0;
        long frac = 0;
        bool seen_dot = false;
        for (const char *p = text; *p && bi < (int)sizeof(buf) - 1; p++) {
            if (*p == '.') { seen_dot = true; continue; }
            if (digit_value(*p) < 0) break;
            buf[bi++] = *p;
            if (seen_dot) frac++;
        }
        buf[bi] = '\0';
        if (bi == 0) { mpz_set_ui(r->m, 0); r->scale = 0; return finalize(r); }
        mpz_set_str(r->m, buf, 10);
        r->scale = frac;
        return finalize(r);
    }

    // General base: accumulate integer part as mant*ibase + digit.
    mpz_t base; mpz_init_set_si(base, ibase);
    mpz_t ip;   mpz_init_set_ui(ip, 0);
    const char *p = text;
    for (; *p && *p != '.'; p++) {
        const int d = digit_value(*p);
        if (d < 0) break;
        mpz_mul(ip, ip, base);
        mpz_add_ui(ip, ip, (unsigned long)d);
    }
    long k = 0;
    mpz_t fp; mpz_init_set_ui(fp, 0);   // fractional digits as integer in base ibase
    if (dot) {
        for (p = dot + 1; *p; p++) {
            const int d = digit_value(*p);
            if (d < 0) break;
            mpz_mul(fp, fp, base);
            mpz_add_ui(fp, fp, (unsigned long)d);
            k++;
        }
    }
    // value = ip + fp/ibase^k ; render to decimal scale k.
    r->scale = k;
    if (k == 0) {
        mpz_set(r->m, ip);
    }
    else {
        mpz_t ten_k; mpz_init(ten_k); bc_pow10(ten_k, (unsigned long)k);
        mpz_t base_k; mpz_init(base_k); mpz_pow_ui(base_k, base, (unsigned long)k);
        // m = ip*10^k + trunc(fp*10^k / ibase^k)
        mpz_t fpart; mpz_init(fpart);
        mpz_mul(fpart, fp, ten_k);
        mpz_tdiv_q(fpart, fpart, base_k);
        mpz_mul(r->m, ip, ten_k);
        mpz_add(r->m, r->m, fpart);
        mpz_clear(ten_k); mpz_clear(base_k); mpz_clear(fpart);
    }
    mpz_clear(base); mpz_clear(ip); mpz_clear(fp);
    return finalize(r);
}

// --- output -----------------------------------------------------------

void
bc_out_char(FILE *const fp, int ch, int *const col)
{
    if (ch == '\n') { fputc('\n', fp); *col = 0; return; }
    if (*col >= BC_LINE_WRAP) { fputc('\\', fp); fputc('\n', fp); *col = 0; }
    fputc(ch, fp);
    (*col)++;
}

void
bc_out_str(FILE *const fp, const char *s, int *const col)
{
    for (; *s; s++) bc_out_char(fp, (unsigned char)*s, col);
}

// `print` strings interpret backslash escapes (bare strings do not).
// GNU bc: \q -> ", the usual C escapes, and any other \X emits nothing.
void
bc_print_string(FILE *const fp, const char *s, int *const col)
{
    for (; *s; s++) {
        if (*s != '\\' || s[1] == '\0') { bc_out_char(fp, (unsigned char)*s, col); continue; }
        const char n = *++s;
        switch (n) {
          case 'n': bc_out_char(fp, '\n', col); break;
          case 't': bc_out_char(fp, '\t', col); break;
          case 'r': bc_out_char(fp, '\r', col); break;
          case 'a': bc_out_char(fp, '\a', col); break;
          case 'b': bc_out_char(fp, '\b', col); break;
          case 'f': bc_out_char(fp, '\f', col); break;
          case 'v': bc_out_char(fp, '\v', col); break;
          case '\\': bc_out_char(fp, '\\', col); break;
          case 'q': bc_out_char(fp, '"', col); break;
          default: /* unknown escape: emit nothing */ break;
        }
    }
}

// Emit one base-`obase` "digit" (0..obase-1).  For bases > 16 bc prints
// each digit as a zero-padded decimal group; groups are space-separated,
// with a space before each group except the first fraction digit.
static void
out_digit(FILE *const fp, unsigned long d, int *const col, bool grouped, int width, bool lead_space)
{
    if (grouped) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%0*lu", lead_space ? " " : "", width, d);
        bc_out_str(fp, buf, col);
    }
    else {
        const char c = d < 10 ? (char)('0' + d) : (char)('A' + (d - 10));
        bc_out_char(fp, c, col);
    }
}

void
bc_print(FILE *const fp, const bcnum *const v, long obase, int *const col)
{
    // bc prints exact zero as "0" regardless of scale / base.
    if (bc_is_zero(v)) { bc_out_char(fp, '0', col); return; }

    // Base-10 immediate: print the integer directly (no GMP).
    if (BC_IS_FIX(v) && obase == 10) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%ld", BC_FIX_VAL(v));
        bc_out_str(fp, buf, col);
        return;
    }

    const bcnum *const a = to_heap(v);   // materialise for the GMP paths
    if (mpz_sgn(a->m) < 0) bc_out_char(fp, '-', col);

    mpz_t am; mpz_init(am); mpz_abs(am, a->m);

    // Split into integer mantissa / fractional mantissa.
    mpz_t scale10; mpz_init(scale10); bc_pow10(scale10, (unsigned long)a->scale);
    mpz_t ip, frac; mpz_init(ip); mpz_init(frac);
    mpz_tdiv_qr(ip, frac, am, scale10);   // ip = integer part, frac = fractional mantissa

    if (obase == 10) {
        // integer part
        if (mpz_sgn(ip) == 0) {
            // no leading zero before the point (bc prints ".333")
        }
        else {
            char *s = mpz_get_str(NULL, 10, ip);
            bc_out_str(fp, s, col);
            // (string from GMP allocator == GC memory; left to GC)
        }
        if (a->scale > 0) {
            bc_out_char(fp, '.', col);
            // fractional mantissa, zero-padded to scale digits
            char *fs = mpz_get_str(NULL, 10, frac);
            long flen = (long)strlen(fs);
            for (long i = flen; i < a->scale; i++) bc_out_char(fp, '0', col);
            bc_out_str(fp, fs, col);
        }
    }
    else {
        const bool grouped = obase > 16;
        int width = 1;
        for (long t = obase - 1; t >= 10; t /= 10) width++;

        // integer part in base obase (no leading zero for a 0 integer part)
        if (mpz_sgn(ip) != 0) {
            // collect digits (little-endian) then emit big-endian
            mpz_t q, rem, base; mpz_init_set(q, ip); mpz_init(rem);
            mpz_init_set_si(base, obase);
            unsigned long *digs = NULL; size_t n = 0, cap = 0;
            while (mpz_sgn(q) > 0) {
                mpz_tdiv_qr(q, rem, q, base);
                if (n == cap) { cap = cap ? cap * 2 : 64; digs = GC_REALLOC(digs, cap * sizeof(*digs)); }
                digs[n++] = mpz_get_ui(rem);
            }
            for (size_t i = n; i-- > 0; ) out_digit(fp, digs[i], col, grouped, width, /*lead_space*/true);
            mpz_clear(q); mpz_clear(rem); mpz_clear(base);
        }

        // fractional part: ndig = smallest k with obase^k >= 10^scale
        if (a->scale > 0) {
            bc_out_char(fp, '.', col);
            mpz_t need; mpz_init(need); bc_pow10(need, (unsigned long)a->scale);
            mpz_t acc, base; mpz_init_set_ui(acc, 1); mpz_init_set_si(base, obase);
            long ndig = 0;
            while (mpz_cmp(acc, need) < 0) { mpz_mul(acc, acc, base); ndig++; }
            if (ndig == 0) ndig = 1;
            // repeatedly: frac *= obase; digit = frac / 10^scale; frac %= 10^scale
            mpz_t digit;  mpz_init(digit);
            for (long i = 0; i < ndig; i++) {
                mpz_mul(frac, frac, base);
                mpz_tdiv_qr(digit, frac, frac, scale10);
                out_digit(fp, mpz_get_ui(digit), col, grouped, width, /*lead_space*/ i != 0);
            }
            mpz_clear(need); mpz_clear(acc); mpz_clear(base); mpz_clear(digit);
        }
    }

    mpz_clear(am); mpz_clear(scale10); mpz_clear(ip); mpz_clear(frac);
}
