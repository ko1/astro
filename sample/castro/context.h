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

typedef uint64_t state_serial_t;

struct Node;

struct function_entry {
    const char *name;
    struct Node *body;
    unsigned int params_cnt;
    unsigned int locals_cnt;
};

struct callcache {
    state_serial_t serial;
    struct Node *body;
};

typedef struct CTX_struct {
    VALUE *env;
    VALUE *fp;
    VALUE *env_end;

    unsigned int func_set_cnt;
    unsigned int func_set_capa;
    struct function_entry *func_set;
    state_serial_t serial;

    // return: setjmp/longjmp at each function-call boundary
    jmp_buf *return_buf;
    VALUE return_value;
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
