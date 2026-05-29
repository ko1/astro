#ifndef BCNUM_H
#define BCNUM_H 1

#include "context.h"

// ---------------------------------------------------------------------
// Arbitrary-precision decimal (bc number) arithmetic.
//
// Every operation allocates and returns a fresh bcnum (values are
// immutable once handed to the evaluator).  Scale-propagation follows
// the POSIX bc rules, parameterised by the working scale `S` (== c->scale):
//
//   a + b, a - b : scale = max(scale a, scale b)
//   a * b        : scale = min(scale a + scale b, max(S, scale a, scale b))
//   a / b        : scale = S
//   a % b        : a - (a/b)*b ; scale = max(S + scale b, scale a)
//   a ^ b        : b truncated to integer; if b>=0 scale = min(scale a * b,
//                  max(S, scale a)); if b<0 scale = S
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Tagged value representation.
//
// A VALUE (== bcnum *) is either:
//   - an *immediate fixnum*: LSB == 1, holding a scale-0 integer in the
//     low 62 bits (value = ((intptr_t)v) >> 1).  No heap, no GMP, no GC.
//   - a *heap bcnum pointer*: LSB == 0 (8-byte aligned), used for any
//     value with scale > 0 or magnitude outside the fixnum range.
//
// Immediates store no pointers, so the GC simply ignores the odd words
// it finds in scanned memory.  Scale-0 small integers — the bulk of the
// work in integer-heavy bc programs — never touch the allocator.
// ---------------------------------------------------------------------
#define BC_IS_FIX(v)   (((intptr_t)(v)) & 1)
#define BC_FIX_VAL(v)  (((intptr_t)(v)) >> 1)
#define BC_FIX_MAX     (((intptr_t)1 << 62) - 1)
#define BC_FIX_MIN     (-((intptr_t)1 << 62))

static inline bool bc_fits_fix(long x) { return x >= BC_FIX_MIN && x <= BC_FIX_MAX; }
static inline bcnum *bc_mkfix(long x)  { return (bcnum *)(((intptr_t)x << 1) | 1); }

bcnum *bc_alloc(void);                 // the value 0 (an immediate)
bcnum *bc_from_long(long v);           // immediate when in range, else heap
bcnum *bc_copy(const bcnum *a);

int  bc_sign(const bcnum *a);          // -1 / 0 / 1
bool bc_is_zero(const bcnum *a);
bool bc_truthy(const bcnum *a);        // nonzero
long bc_scale(const bcnum *a);         // scale (0 for an immediate)

// Truncate/extend a to a given scale (truncation is toward zero).
bcnum *bc_rescale(const bcnum *a, long newscale);

bcnum *bc_neg(const bcnum *a);
bcnum *bc_add(const bcnum *a, const bcnum *b);
bcnum *bc_sub(const bcnum *a, const bcnum *b);
bcnum *bc_mul(const bcnum *a, const bcnum *b, long S);
bcnum *bc_div(CTX *c, const bcnum *a, const bcnum *b, long S);   // raises on /0
bcnum *bc_mod(CTX *c, const bcnum *a, const bcnum *b, long S);   // raises on %0
bcnum *bc_pow(CTX *c, const bcnum *a, const bcnum *b, long S);

int  bc_cmp(const bcnum *a, const bcnum *b);   // -1 / 0 / 1

// Builtins.
long   bc_length(const bcnum *a);      // total significant digits
bcnum *bc_sqrt(CTX *c, const bcnum *a, long S);

// Convert to a host long (truncating to integer); used for ibase/obase/
// scale assignment, array indices, and exponents.
long bc_to_long(const bcnum *a);

// Parse a numeric literal written in input base `ibase` (2..16).
bcnum *bc_parse_literal(const char *text, long ibase);

// Render `a` to stream `fp` in output base `obase`, honouring bc's
// 70-column line wrapping via *col (the running output column).
void bc_print(FILE *fp, const bcnum *a, long obase, int *col);

// Raw character output with bc-style line wrapping (used for strings too).
void bc_out_char(FILE *fp, int ch, int *col);
void bc_out_str(FILE *fp, const char *s, int *col);
void bc_print_string(FILE *fp, const char *s, int *col);   // with escape processing

// Signal a runtime error (longjmp back to the REPL / exit in batch mode).
void bc_runtime_error(CTX *c, const char *fmt, ...);

#endif // BCNUM_H
