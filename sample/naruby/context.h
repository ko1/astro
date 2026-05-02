#ifndef NARUBY_CONTEXT_H
#define NARUBY_CONTEXT_H 1

#define NARUBY_DEBUG 1
#if NARUBY_DEBUG
#define NARUBY_ASSERT(expr) assert(expr)
#else
#define NARUBY_ASSERT(expr) 0
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

struct naruby_option {
    // language
    bool static_lang;

    // exec mode
    bool compile_only;
    bool pg_mode;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;
    bool jit;

    // misc
    bool quiet;
};

extern struct naruby_option OPTION;

typedef int64_t VALUE;
typedef uint64_t state_serial_t;

// RESULT: 2-register return type for non-local exit support (`return`).
// Same shape as castro's RESULT — fits in rax:rdx so the function return
// ABI carries both VALUE and a state bit without needing setjmp.
//
// On the fast path (no `return`), `state == RESULT_NORMAL == 0` lets the
// `if (r.state)` test fold to a single branch the predictor handles for
// free.  Within an inlined SD chain `state` is a compile-time-constant 0
// almost everywhere, so gcc DCE's the propagation tests entirely.

#define RESULT_NORMAL 0u
#define RESULT_RETURN 1u   /* node_return — caught at function-call boundary */

typedef struct {
    VALUE        value;
    unsigned int state;
} RESULT;

#define RESULT_OK(v)        ((RESULT){(v), RESULT_NORMAL})
#define RESULT_RETURN_(v)   ((RESULT){(v), RESULT_RETURN})

// UNWRAP: extract VALUE from RESULT, or propagate non-NORMAL state by
// returning from the *caller* function (statement expression).  Use this
// at every internal EVAL_ARG site so e.g. `return` inside a deeply
// nested if/while bubbles up to the enclosing function-call boundary
// without setjmp.  Borrowed from castro/abruby.
#define UNWRAP(r) ({ RESULT _r = (r); if (UNLIKELY(_r.state != RESULT_NORMAL)) return _r; _r.value; })

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

typedef VALUE (*builtin_func_ptr)(void);
typedef VALUE (*builtin_func1_ptr)(VALUE);
typedef VALUE (*builtin_func2_ptr)(VALUE, VALUE);
typedef VALUE (*builtin_func3_ptr)(VALUE, VALUE, VALUE);
typedef VALUE (*builtin_func4_ptr)(VALUE, VALUE, VALUE, VALUE);

typedef struct builtin_func {
    builtin_func_ptr func;
    const char *name;
    const char *func_name;
    bool have_src;
} builtin_func_t;

#ifndef DEBUG_EVAL
#define DEBUG_EVAL 0
#endif

typedef struct CTX_struct {
    VALUE *env;
    VALUE *fp;
    unsigned int func_set_cnt;
    struct function_entry *func_set;
    state_serial_t serial;

#if DEBUG_EVAL
    unsigned int frame_cnt;
    unsigned int rec_cnt;
#endif
} CTX;

#define LIKELY(expr) __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

#endif
