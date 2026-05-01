#ifndef PASCALAST_CONTEXT_H
#define PASCALAST_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <alloca.h>
#include <setjmp.h>

// Boehm-Demers-Weiser conservative GC.  Used for any heap allocation
// that the user can leak into a long-running program: strings, file
// objects, pointer-target records.  Long-term-lived parser tables
// keep using malloc — the overhead of putting them under GC isn't
// worth it.
extern void *GC_malloc(size_t);
extern void *GC_malloc_atomic(size_t);   // pointer-free buffers (strings, etc)
extern void *GC_realloc(void *, size_t);
extern void  GC_free(void *);
extern void  GC_init(void);
extern void  GC_gcollect(void);
extern size_t GC_get_heap_size(void);
extern size_t GC_get_total_bytes(void);

typedef int64_t VALUE;

// Static type tags carried alongside parsed expressions and stored in
// per-symbol metadata.  They never appear at run time — every value is
// stored as a 64-bit cell whose interpretation is determined by the
// node kind that produced it.
enum pascal_type {
    PT_INT   = 0,
    PT_BOOL  = 1,
    PT_REAL  = 2,
    PT_ARRAY = 3,    // only appears in `parse_type` return; never on values
    PT_SET   = 5,    // 64-bit bitset (skip 4 — used for PT_RECORD in main.c)
    PT_STR   = 6,    // string — slot stores `char *`, malloc'd, NUL-terminated
    PT_POINTER = 7,  // pointer — slot stores a heap address (or 0 for nil)
    PT_PROC    = 8,  // procedure value — slot stores a proc index (cast int64)
    PT_FILE    = 9,  // text file handle — slot stores `struct pascal_file *`
};

// Inline cache embedded in each procedure-call node.  Populated
// lazily on the first dispatch by reading c->procs[pidx]; subsequent
// calls skip the proc-table lookup.  Pascal procs are not
// redefinable, so the cache never has to invalidate.
struct pcall_cache {
    struct Node *body;
    int   nslots;
    int   return_slot;
    int   lexical_depth;
    bool  is_function;
};

// Heap object backing a `text` file variable.  Allocated on first
// `assign(f, 'name')`, opened by `reset` / `rewrite`, closed by
// `close`.  We keep the filename around so `reset` after `close`
// reopens cleanly.
struct pascal_file {
    FILE *fp;
    char  name[256];
};

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

// Sizing knobs.  These cap the language to roughly the same envelope as
// Standard Pascal source written by hand: 512 globals, 1M-deep call
// stack, 256 sub-programs.  Bumping any of these is just a recompile.
#define PASCAL_MAX_GLOBALS    512
#define PASCAL_STACK_SIZE     (1 << 20)
#define PASCAL_MAX_PROCS      256
#define PASCAL_MAX_FRAME_SLOTS 64
#define PASCAL_MAX_CALL_ARGS  4096

struct Node;

#define PASCAL_MAX_PARAMS 16
#define PASCAL_MAX_DEPTH  8         // nesting depth limit (display vector size)

struct pascal_proc {
    const char *name;
    struct Node *body;
    int nparams;
    int nslots;       // params + locals (+ 1 for func return)
    bool is_function;
    int return_slot;  // valid iff is_function
    int  lexical_depth; // 0 = main, 1 = top-level proc, 2 = nested in top, …
    bool param_by_ref[PASCAL_MAX_PARAMS];
    bool param_is_array[PASCAL_MAX_PARAMS]; // var-array parameter
    int32_t param_arr_lo[PASCAL_MAX_PARAMS];// array param's declared lower bound
    char  param_type[PASCAL_MAX_PARAMS];   // PT_INT / PT_BOOL / PT_REAL — used for arg promotion
    char  return_type;                      // valid iff is_function
};

typedef struct CTX_struct {
    // Globals.  array_size[i] != 0 marks a global as an integer array;
    // arrays[i] holds the heap buffer (total length = array_size[i] *
    // (array_size2[i] ?: 1)).  array_size2[i] == 0 means a 1D array.
    VALUE     globals[PASCAL_MAX_GLOBALS];
    int64_t  *arrays[PASCAL_MAX_GLOBALS];
    int32_t   array_lo[PASCAL_MAX_GLOBALS];
    int32_t   array_size[PASCAL_MAX_GLOBALS];
    int32_t   array_lo2[PASCAL_MAX_GLOBALS];
    int32_t   array_size2[PASCAL_MAX_GLOBALS];

    // Call stack: linear VALUE[] indexed by fp + slot.  Each procedure
    // call bumps fp to a fresh region of length proc->nslots.
    VALUE  *stack;
    int     fp;
    int     sp;       // first free slot above current frame

    // Proc table.
    struct pascal_proc procs[PASCAL_MAX_PROCS];
    int    nprocs;

    // Non-local control flow.
    //   loop_action: 0 = normal, 1 = break, 2 = continue
    //   exit_pending: set by `exit` to bail out of the current procedure
    // Both flags are checked at sequence boundaries and at each loop
    // iteration, then cleared by the consumer.
    int loop_action;
    int exit_pending;

    // Display vector for nested procedures.  display[d] is the fp of
    // the most-recent active call frame at lexical depth d.  A
    // non-local variable at (depth, idx) reads
    // c->stack[c->display[depth] + idx].  Each pascal_call saves and
    // restores display[callee_depth] so recursion and sibling calls
    // work without heap-allocated activation records.
    int display[PASCAL_MAX_DEPTH];

    // Exception handling.  `exc_top` is the head of a setjmp-handler
    // stack threaded on the C call stack.  `exc_msg` holds the most
    // recently raised string (Free Pascal's `Exception.Message`).
    struct exc_handler *exc_top;
    const char         *exc_msg;
} CTX;

struct exc_handler {
    jmp_buf buf;
    struct exc_handler *prev;
    int saved_fp;
    int saved_sp;
    int saved_display[PASCAL_MAX_DEPTH];
};

struct pascalast_option {
    bool quiet;
    bool dump_ast;
    bool no_run;
    bool record_all;     // referenced by generated allocators; we never set it
    bool no_compiled_code;  // disable AOT swap-in (run pure interpreter even if SD code is linked)
    bool compile_only;      // -c: emit node_specialized.c and exit
};

extern struct pascalast_option OPTION;

// Runtime helpers (defined in main.c).
VALUE pascal_call(CTX *c, uint32_t pidx, uint32_t argc, VALUE *av);
int64_t pascal_aref(CTX *c, uint32_t arr_idx, int64_t i);
void pascal_aset(CTX *c, uint32_t arr_idx, int64_t i, int64_t v);
__attribute__((noreturn)) void pascal_error(const char *fmt, ...);
__attribute__((noreturn)) void pascal_error_at(int line, const char *fmt, ...);

#endif
