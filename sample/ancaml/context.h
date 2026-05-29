#ifndef ANCAML_CONTEXT_H
#define ANCAML_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <setjmp.h>
#include <alloca.h>

// =====================================================================
// ancaml — a MinCaml interpreter on the ASTro framework.
//
// MinCaml (https://esumii.github.io/min-caml/) is a tiny, monomorphic
// ML subset designed for an educational optimizing compiler course at
// Tohoku University.  It has exactly five value kinds — unit, bool, int,
// float, plus heap objects (closures, tuples, arrays) — no strings, no
// lists, no variants, no polymorphism.  Every program is a single
// expression.
//
// Runtime value representation (a tagged 64-bit word, `VALUE`):
//   - LSB == 1                : immediate int (value = v >> 1, 63-bit)
//   - v == UNIT/FALSE/TRUE    : the three small constants (2 / 4 / 6)
//   - otherwise (8-aligned)   : a heap object pointer (struct ac_obj *)
// Immediates / constants carry no pointers, so the GC ignores the odd
// and 2/4/6 words it scans; heap pointers are 8-aligned and traced.
// =====================================================================

typedef int64_t VALUE;

#define LIKELY(e)   __builtin_expect((e), 1)
#define UNLIKELY(e) __builtin_expect((e), 0)

#define AC_UNIT   ((VALUE)2)
#define AC_FALSE  ((VALUE)4)
#define AC_TRUE   ((VALUE)6)

#define AC_INT(n)      (((VALUE)(int64_t)(n) << 1) | 1)
#define AC_INT_VAL(v)  ((int64_t)(v) >> 1)
#define AC_IS_INT(v)   (((VALUE)(v)) & 1)
#define AC_IS_PTR(v)   ((((VALUE)(v)) & 7) == 0 && (v) != 0)
#define AC_BOOL(b)     ((b) ? AC_TRUE : AC_FALSE)
#define AC_PTR(v)      ((struct ac_obj *)(uintptr_t)(v))
#define AC_OBJ(p)      ((VALUE)(uintptr_t)(p))

struct Node;
struct CTX;

enum ac_obj_type {
    AC_FLOAT,      // boxed double
    AC_CLOSURE,    // (body node, captured env, nparams)
    AC_TUPLE,      // immutable n-tuple
    AC_ARRAY,      // mutable Array.t
    AC_PRIM,       // external function (print_int, sqrt, ...)
};

typedef VALUE (*ac_prim_fn)(struct CTX *c, int argc, VALUE *argv);

// Lexical environment frame: a flat slot vector + parent pointer.  The
// parser resolves every variable to a (depth, index) coordinate, so
// lookup is a pointer-chase + array index — no name hashing at runtime.
struct ac_frame {
    struct ac_frame *parent;
    int nslots;
    VALUE slots[];
};

struct ac_obj {
    int type;
    union {
        double dbl;                                            // AC_FLOAT
        struct {
            struct Node *body;
            struct ac_frame *env;
            int nparams;
            int is_leaf;        // body creates no closures → its frame may be alloca'd
        } closure;                                             // AC_CLOSURE
        struct { int n; VALUE *items; } tup;                   // AC_TUPLE
        struct { int n; VALUE *items; } arr;                   // AC_ARRAY
        struct { const char *name; ac_prim_fn fn; int arity; } prim;  // AC_PRIM
    } u;
};

#define AC_TC_ARGMAX 64

typedef struct CTX {
    struct ac_frame *env;       // current lexical frame

    // Tail-call trampoline.  A `node_tail_app*` in tail position fills these
    // and returns instead of recursing; ac_apply's loop catches the flag and
    // re-enters with (tc_fn, tc_argv) — so tail recursion runs in O(1) C stack.
    int    tail_pending;
    VALUE  tc_fn;
    int    tc_argc;
    VALUE  tc_argv[AC_TC_ARGMAX];

    // Runtime-error unwinding (longjmp back to the REPL/driver).
    jmp_buf err_jmp;
    int     err_active;
    char    err_msg[256];
} CTX;

struct ancaml_option {
    bool no_compiled_code;   // --plain
    bool aot_compile;        // --aot-compile
    bool pg_compile;         // --pg-compile
    bool record_all;
    bool quiet;
    bool verbose;
    bool dump_ast;
    bool no_typecheck;       // --no-typecheck (debug)
    bool dump_types;         // --dump-types
};
extern struct ancaml_option OPTION;

#endif // ANCAML_CONTEXT_H
