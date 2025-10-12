#ifndef CONTEXT_H
#define CONTEXT_H

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef int64_t VALUE;

typedef struct {
    int dummy;
} CTX;

struct calc_option {
    // language
    bool static_lang;

    // exec mode
    bool compile_only;
    bool pg_mode;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;

    // misc
    bool quiet;
};

extern struct calc_option OPTION;

#endif
