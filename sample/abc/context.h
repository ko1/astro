#ifndef CONTEXT_H
#define CONTEXT_H 1

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <gmp.h>

// =====================================================================
// abc — a `bc` (POSIX/GNU) compatible arbitrary-precision calculator
// built on the ASTro framework.
//
// A bc number is an arbitrary-precision *decimal fixed-point* value:
//   value == mant * 10^(-scale)
// where `mant` is a signed GMP integer carrying the sign, and `scale`
// (>= 0) is the number of fractional decimal digits.  Trailing zeros
// are significant (bc keeps them): 1.50 is stored as mant=150, scale=2
// and prints back as "1.50".
// =====================================================================

typedef struct bcnum {
    mpz_t m;       // signed mantissa (the limbs are GC-managed; see node.c)
    long scale;    // number of fractional decimal digits, >= 0
} bcnum;

// The single runtime value type threaded through every EVAL is a
// pointer to a (GC-allocated) bcnum.  Strings only ever appear inside
// `print` items and are handled there, never as a first-class VALUE.
typedef bcnum *VALUE;

// Non-local control flow for return / break / continue.  The tree
// walker sets c->flow and unwinds; loops and the call boundary catch it.
enum bc_flow {
    FLOW_NORMAL = 0,
    FLOW_BREAK,
    FLOW_CONTINUE,
    FLOW_RETURN,
};

struct bc_func;     // defined in node.c
struct bc_symtab;   // defined in node.c

typedef struct CTX {
    long ibase;          // input base  (2..16)
    long obase;          // output base (2..BC_OBASE_MAX)
    long scale;          // working scale for / % ^ sqrt etc.
    VALUE last;          // value of the most recent printed expression ("last" / ".")

    enum bc_flow flow;   // pending non-local control transfer
    VALUE retval;        // value carried by a `return`

    int out_col;         // current output column (for bc's 70-col line wrap)

    struct bc_symtab *vars;   // global scalar + array variables + functions

    bool interactive;    // REPL: print runtime errors but keep going
} CTX;

// bc's default output line length is 70; numbers/strings longer than
// that are wrapped with a trailing backslash-newline.  bc emits the
// backslash once 68 visible characters have accumulated on the line.
#define BC_LINE_WRAP 68

// Largest output base we support without arbitrary widening.
#define BC_OBASE_MAX 65535

struct abc_option {
    // exec mode (mapped from the framework's universal flags)
    bool no_compiled_code;   // --plain : pure interpreter
    bool aot_compile;        // --aot-compile
    bool pg_compile;         // --pg-compile
    bool record_all;         // record every allocated node (framework hook)

    // misc
    bool quiet;              // -q : suppress the startup banner
    bool verbose;            // -v
    bool warn;               // -w : POSIX-incompatibility warnings (accepted, mostly no-op)
    bool math_lib;           // -l : load the math library (a,c,e,j,l,s) + scale=20
    bool disasm;             // --disasm
};

extern struct abc_option OPTION;

#endif // CONTEXT_H
