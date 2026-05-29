#ifndef CONTEXT_H
#define CONTEXT_H 1

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// =====================================================================
// AnPy — a ChocoPy interpreter on the ASTro framework.
//
// ChocoPy (chocopy.org) is a statically-typed dialect of Python 3.6:
// explicit type annotations, single-inheritance classes, int/bool/str/
// lists/None, global+nested functions with global/nonlocal.  Every valid
// ChocoPy program is also valid Python 3.6 with the same observable
// semantics, which is what the differential test harness exploits.
//
// Runtime value representation (a tagged word, `VALUE`):
//   - LSB == 1            : immediate int  (value = (intptr_t)v >> 1)
//   - v == NONE/FALSE/TRUE : the three small constants (2 / 4 / 6)
//   - otherwise (8-aligned, != 0): a heap object pointer (anpy_obj *)
// Immediates carry no pointers, so the GC ignores the odd / 2 / 4 / 6
// words it scans; heap pointers are 8-aligned and traced normally.
// =====================================================================

typedef intptr_t VALUE;

#define ANPY_NONE   ((VALUE)2)
#define ANPY_FALSE  ((VALUE)4)
#define ANPY_TRUE   ((VALUE)6)

#define IS_INT(v)   (((VALUE)(v)) & 1)
#define IS_PTR(v)   ((((VALUE)(v)) & 7) == 0 && (v) != 0)
#define IS_NONE(v)  ((v) == ANPY_NONE)
#define IS_BOOL(v)  ((v) == ANPY_FALSE || (v) == ANPY_TRUE)

#define INT2VAL(i)  ((VALUE)(((intptr_t)(i) << 1) | 1))
#define VAL2INT(v)  (((intptr_t)(v)) >> 1)
#define BOOL2VAL(b) ((b) ? ANPY_TRUE : ANPY_FALSE)
#define VAL2BOOL(v) ((v) == ANPY_TRUE)

// Heap object kinds.
enum anpy_kind {
    K_STR,      // immutable string
    K_LIST,     // mutable fixed-length list
    K_OBJ,      // instance of a user-defined class
    K_FUNC,     // a function/method binding (closure) — never user-visible
    K_CLASS,    // a class object (constructor) — never user-visible
};

// Common heap header (every heap object starts with this).
typedef struct anpy_obj {
    enum anpy_kind kind;
} anpy_obj;

typedef struct {
    anpy_obj hdr;
    int32_t len;
    char data[];          // NUL-terminated for convenience
} anpy_str;

typedef struct {
    anpy_obj hdr;
    int32_t len;
    VALUE *elems;
} anpy_list;

struct anpy_class;        // defined in node.h

typedef struct {
    anpy_obj hdr;
    struct anpy_class *cls;
    VALUE attrs[];        // one slot per attribute (by slot index)
} anpy_inst;

struct anpy_func;         // closure: defined in node.h
struct anpy_env;          // lexical environment frame: defined in node.h

// A function/closure binding stored in a cell.
typedef struct {
    anpy_obj hdr;
    struct anpy_func *fn;   // static descriptor (params/locals/body)
    struct anpy_env *env;   // captured defining environment (NULL for top level)
    VALUE bound_self;       // for bound methods (ANPY_NONE if not bound)
} anpy_closure;

typedef struct {
    anpy_obj hdr;
    struct anpy_class *cls;
} anpy_classval;

typedef struct CTX {
    struct anpy_env *global;   // global environment frame
    struct anpy_env *env;      // current environment frame
    bool returning;            // a return statement fired
    VALUE retval;              // value carried by return
} CTX;

struct anpy_option {
    bool no_compiled_code;   // --plain
    bool aot_compile;        // --aot-compile
    bool pg_compile;         // --pg-compile
    bool record_all;
    bool quiet;
    bool verbose;
    bool dump_ast;
    bool no_typecheck;       // --no-typecheck : skip static checking (debug)
    bool disasm;
};
extern struct anpy_option OPTION;

#endif // CONTEXT_H
