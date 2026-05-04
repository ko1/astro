#ifndef AFORTH_CONTEXT_H
#define AFORTH_CONTEXT_H

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef int64_t VALUE;

#define AFORTH_DSTACK_SIZE  (1u << 16)   /* data stack cells */
#define AFORTH_RSTACK_SIZE  (1u << 14)   /* return stack cells */
#define AFORTH_DOSTACK_SIZE (1u << 12)   /* DO/LOOP frame stack */
#define AFORTH_VARS_SIZE    (1u << 20)   /* CREATE/VARIABLE storage cells */

/* DO/LOOP frame.  `index` and `limit` are kept on a parallel stack rather
 * than on the return stack so that R> / >R / R@ remain simple. */
struct aforth_do_frame {
    VALUE index;
    VALUE limit;
};

typedef struct {
    /* Data stack: grows up.  `dsp` points to the slot ABOVE the top. */
    VALUE * restrict dsp;
    VALUE *dstack_base;
    VALUE *dstack_end;

    /* Return stack: grows up.  `rsp` points to the slot ABOVE the top. */
    VALUE * restrict rsp;
    VALUE *rstack_base;
    VALUE *rstack_end;

    /* DO/LOOP frame stack (separate from return stack for clarity). */
    struct aforth_do_frame *dop;
    struct aforth_do_frame *dostack_base;
    struct aforth_do_frame *dostack_end;

    /* CREATE/VARIABLE/CONSTANT storage cells.  Indexed by var_id. */
    VALUE *vars;
    uint32_t vars_used;

    /* LEAVE flag for current DO loop.  Bumped by LEAVE; consumed by LOOP. */
    int leave_flag;
} CTX;

struct aforth_option {
    bool quiet;
    bool dump_ast;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool aot_compile;       /* compile every entry then exit (after running once) */
    bool record_all;
};

extern struct aforth_option OPTION;

#endif /* AFORTH_CONTEXT_H */
