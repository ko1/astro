#ifndef CASTRO_CONTEXT_H
#define CASTRO_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>

#define CASTRO_DEBUG 0
#if CASTRO_DEBUG
#define CASTRO_ASSERT(expr) assert(expr)
#else
#define CASTRO_ASSERT(expr) (void)0
#endif

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

// Receiver-typed VALUE: every node returns 8 bytes; the receiving node
// picks the slot it needs (int64 .i or double .d).  Pointer slot .p is
// reserved for arrays/strings in later phases.
typedef union VALUE {
    int64_t  i;
    double   d;
    void    *p;
} VALUE;

// RESULT: 2-register return type for non-local exit support — abruby's
// approach.  Fits in rax:rdx (16 byte = VALUE 8 + state 4 + pad 4) and
// avoids the CTX-field round-trip a setjmp-free fallback would impose.
//
// state bits (low 16): NORMAL=0 lets `if (r.state)` be a plain zero-test.
// Higher states are bit flags so a boundary catch (function-return at
// node_call, break/continue at the loop body) can mask the bit out with
// a single AND, propagating any other states unchanged.
//
// Within an inlined SD chain `state` is a compile-time-constant 0
// almost everywhere, so gcc DCE's the propagation tests.  Only at
// non-inlined boundaries (recursive call, cross-SD call) does the
// branch survive — and there it's a single register cmp+jne that the
// branch predictor handles for free.
#define RESULT_NORMAL   0u
#define RESULT_RETURN   1u  /* node_return — caught at function boundary */
#define RESULT_BREAK    2u  /* node_break  — caught at the enclosing loop  */
#define RESULT_CONTINUE 4u  /* node_continue — caught at the enclosing loop */
#define RESULT_GOTO     8u  /* node_goto — caught at the enclosing function (carries label idx in .value.i) */

typedef struct {
    VALUE        value;
    unsigned int state;
} RESULT;

#define RESULT_OK(v)        ((RESULT){(v),               RESULT_NORMAL})
#define RESULT_OK_I(n)      ((RESULT){(VALUE){.i = (n)}, RESULT_NORMAL})
#define RESULT_OK_D(x)      ((RESULT){(VALUE){.d = (x)}, RESULT_NORMAL})
#define RESULT_OK_P(p)      ((RESULT){(VALUE){.p = (p)}, RESULT_NORMAL})

struct Node;

// Function table: parse.rb gives every function_definition a stable
// 0..N-1 index, and the IR carries that index directly.  At runtime
// the body NODE* lives in `c->func_bodies[idx]`; nothing else is
// needed for evaluation.  Names are kept only for `--dump` output and
// for locating `main` at startup.  (Earlier revisions threaded a
// `function_entry` struct with params_cnt / locals_cnt / needs_setjmp,
// but every one of those fields was either dead after the
// RESULT-state migration or never read in the first place.)
typedef struct CTX_struct {
    VALUE *env;
    VALUE *fp;
    VALUE *env_end;

    // Globals: a flat VALUE region indexed by slot.  parse.rb
    // determines a slot index for each declared global; the runtime
    // allocates `globals_size` slots up-front, fills them with the
    // declared initializers, then leaves them in place for the rest of
    // the program.  Arrays / structs occupy multiple consecutive slots.
    VALUE *globals;
    size_t globals_size;

    unsigned int func_count;
    struct Node **func_bodies;
    char        **func_names;

    // goto support: parse.rb lowers a function with goto into a
    // label-dispatch loop where each segment ends with `node_goto N`.
    // node_goto returns RESULT_GOTO carrying the target label;
    // node_goto_dispatch catches GOTO, stores the new target here,
    // and re-evaluates the body.  The body's switch reads goto_target
    // to pick the right segment.
    int32_t  goto_target;
} CTX;

struct castro_option {
    bool quiet;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;
    bool dump;
};

extern struct castro_option OPTION;

#endif
