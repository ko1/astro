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

struct Node;

struct function_entry {
    const char *name;
    struct Node *body;
    unsigned int params_cnt;
    unsigned int locals_cnt;
    bool needs_setjmp;            // body still contains `return` after lifting
};

// Note: the previous design here had a `struct callcache` (per-call-site
// inline cache holding `serial / body / dispatcher / needs_setjmp`) and
// a matching `struct func_addr_cache`.  Both were inherited from the
// dynamic-dispatch sample languages (abruby etc.) where lookups happen
// at run time.  In castro the call target is statically known at parse
// time, so the IR now carries a stable `function_entry *` index instead
// (parse.rb assigns each function definition a slot in `c->func_set[]`)
// and these caches are gone.

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

    unsigned int func_set_cnt;
    unsigned int func_set_capa;
    struct function_entry *func_set;

    // return: setjmp/longjmp at each function-call boundary
    jmp_buf *return_buf;
    VALUE return_value;

    // loop control: setjmp/longjmp at the enclosing loop boundary, but
    // only paid by loops whose body contains the corresponding statement
    // (parse.rb routes such loops to node_while_brk / node_for_brk /
    // node_do_while_brk; loops without break/continue stay on the cheap
    // pointer-load path).
    jmp_buf *break_buf;
    jmp_buf *continue_buf;

    // goto support: when a function uses goto, parse.rb lowers the
    // entire body to a label-dispatch loop.  `goto_target` carries the
    // next label index; node_goto sets it then longjmps back to the
    // dispatch loop via `goto_buf`.
    jmp_buf *goto_buf;
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
