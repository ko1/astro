#ifndef ANLOX_CONTEXT_H
#define ANLOX_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <setjmp.h>
#include <alloca.h>

// =====================================================================
// anlox — a Lox interpreter on the ASTro framework.
//
// Lox is the teaching language of Robert Nystrom's *Crafting Interpreters*
// (https://craftinginterpreters.com/).  This is a tree-walking interpreter
// (the book's "jlox") on ASTro: a dynamically-typed language with nil,
// booleans, double-precision numbers, strings, first-class closures, and
// single-inheritance classes with dynamic dispatch.
//
// Part of the **An\*** series ("ASTro Nutshell" — bite-sized pedagogical
// languages; cf. anpy = ChocoPy, ancaml = MinCaml).
//
// Value representation (a tagged word, `VALUE`):
//   - v == NIL / FALSE / TRUE : the three small constants (2 / 4 / 6)
//   - otherwise (8-aligned)   : a heap object pointer (struct lox_obj *)
// Lox has no integers — every number is a double, boxed on the heap
// (LOX_NUM).  Boxing per number is a known cost; see docs/perf.md.
// =====================================================================

typedef intptr_t VALUE;

#define LIKELY(e)   __builtin_expect((e), 1)
#define UNLIKELY(e) __builtin_expect((e), 0)

#define LOX_NIL    ((VALUE)2)
#define LOX_FALSE  ((VALUE)4)
#define LOX_TRUE   ((VALUE)6)

#define LOX_BOOL(b)   ((b) ? LOX_TRUE : LOX_FALSE)
#define LOX_IS_PTR(v) ((((VALUE)(v)) & 7) == 0 && (v) != 0)
#define LOX_PTR(v)    ((struct lox_obj *)(uintptr_t)(v))
#define LOX_OBJ(p)    ((VALUE)(uintptr_t)(p))

struct Node;
struct CTX;

enum lox_obj_type {
    LOX_NUM,        // boxed double
    LOX_STR,        // interned-by-value string
    LOX_CLOSURE,    // function: (fundef, captured env, is_init)
    LOX_CLASS,      // class object
    LOX_INSTANCE,   // class instance (dynamic fields)
    LOX_NATIVE,     // native function (clock, ...)
};

typedef VALUE (*lox_native_fn)(struct CTX *c, int argc, VALUE *argv);

// Lexical environment frame: a flat slot vector + parent pointer.  The
// resolver (parse.c) maps every local to a (depth, slot) coordinate, so
// lookup is a pointer-chase + index — no name hashing at runtime.
struct lox_frame {
    struct lox_frame *parent;
    int nslots;
    VALUE slots[];
};

// Static descriptor of a function/method body (built by the parser).
struct lox_fundef {
    const char *name;        // for stack traces / display
    int arity;
    int nslots;              // function scope size: params + body top-level locals
    struct Node *body;       // a node_stmts; reached via runtime dispatch (own SD entry)
    bool is_init;            // class initializer → returns `this`
};

// A simple chained hash map (name -> VALUE): globals and instance fields
// and class method tables.
struct lox_entry { const char *key; VALUE val; struct lox_entry *next; };
struct lox_table { struct lox_entry **buckets; int nbuckets; int count; };

struct lox_obj {
    int type;
    union {
        double num;                                         // LOX_NUM
        struct { char *chars; int len; } str;               // LOX_STR
        struct {
            struct lox_fundef *fn;
            struct lox_frame *env;
        } closure;                                          // LOX_CLOSURE
        struct {
            const char *name;
            struct lox_obj *superclass;                     // NULL if none
            struct lox_table methods;                       // name -> closure VALUE
        } klass;                                            // LOX_CLASS
        struct {
            struct lox_obj *klass;
            struct lox_table fields;                        // name -> VALUE
        } instance;                                         // LOX_INSTANCE
        struct {
            const char *name;
            lox_native_fn fn;
            int arity;
        } native;                                           // LOX_NATIVE
    } u;
};

typedef struct CTX {
    struct lox_frame *env;        // current local frame chain
    struct lox_table *globals;    // late-bound global variables

    // `return` unwinding: set by node_return, observed by block/while/for and
    // caught at the call boundary (lox_call).
    bool  returning;
    VALUE retval;

    // Runtime-error unwinding (longjmp back to the driver).
    jmp_buf err_jmp;
    int     err_active;
} CTX;

struct anlox_option {
    bool no_compiled_code;   // --plain
    bool aot_compile;        // --aot-compile
    bool pg_compile;         // --pg-compile
    bool record_all;
    bool quiet;
    bool verbose;
    bool dump_ast;
};
extern struct anlox_option OPTION;

#endif // ANLOX_CONTEXT_H
