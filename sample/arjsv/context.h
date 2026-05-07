#ifndef ARJSV_CONTEXT_H
#define ARJSV_CONTEXT_H 1

#include <ruby.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

// Validator runtime context.
//
//   data       — JSON value currently being validated (Ruby VALUE: Hash, Array,
//                String, Integer, Float, true/false, nil).  Property and items
//                nodes save/restore around recursion.
//   root_data  — top-level data (reserved for $ref; Tier 3).
//   consts     — pointer into the schema-level constants Array's storage,
//                used for enum / const equality checks.  Stable for the
//                lifetime of a validate call.
typedef struct CTX_struct {
    VALUE data;
    VALUE root_data;
    const VALUE *consts;
    int one_of_count;       // matches counter for `oneOf` walk
    int one_of_active;      // > 0 inside a `oneOf` evaluation
} CTX;

struct arjsv_option {
    bool quiet;
    bool no_compiled_code;
    bool disasm;
    bool record_all;       // referenced by ASTroGen-generated ALLOC_*; unused here
};

extern struct arjsv_option OPTION;

// JSON type bitmask for node_type_check.  Float values that are
// integer-valued pass `INTEGER`.
#define ARJSV_T_NULL     (1u << 0)
#define ARJSV_T_BOOLEAN  (1u << 1)
#define ARJSV_T_INTEGER  (1u << 2)
#define ARJSV_T_NUMBER   (1u << 3)
#define ARJSV_T_STRING   (1u << 4)
#define ARJSV_T_ARRAY    (1u << 5)
#define ARJSV_T_OBJECT   (1u << 6)

// Hash helper for `double` operand: bit-pattern reinterpretation.
static inline uint64_t
arjsv_double_bits(double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

#endif // ARJSV_CONTEXT_H
