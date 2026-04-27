#ifndef CONTEXT_H
#define CONTEXT_H

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// VALUE is the result type returned by every node dispatcher.
// We carry the raw 64-bit value of any wasm numeric type (i32, i64,
// f32, f64) — the type is statically known from the wasm validation,
// so each operator reinterprets at use sites without runtime tagging.
//
// Reinterpret happens via `memcpy` (not a pointer cast) to stay clear
// of strict-aliasing.  After PE / gcc -O2 the memcpys collapse to a
// single mov in the specialized SD_ functions, so there is no
// runtime cost in the optimized path.
typedef uint64_t VALUE;

#include <string.h>  // memcpy
#include <math.h>    // fabsf, fabs, sqrtf, sqrt, copysignf, copysign, ...

static inline int32_t  AS_I32(VALUE v) { return (int32_t)(uint32_t)v; }
static inline int64_t  AS_I64(VALUE v) { return (int64_t)v; }
static inline uint32_t AS_U32(VALUE v) { return (uint32_t)v; }
static inline uint64_t AS_U64(VALUE v) { return v; }
static inline float    AS_F32(VALUE v) { float f; uint32_t b = (uint32_t)v; memcpy(&f, &b, 4); return f; }
static inline double   AS_F64(VALUE v) { double d; uint64_t b = v; memcpy(&d, &b, 8); return d; }

static inline VALUE FROM_I32(int32_t  x) { return (uint32_t)x; }
static inline VALUE FROM_I64(int64_t  x) { return (uint64_t)x; }
static inline VALUE FROM_U32(uint32_t x) { return (uint64_t)x; }
static inline VALUE FROM_U64(uint64_t x) { return x; }
static inline VALUE FROM_F32(float    x) { uint32_t b; memcpy(&b, &x, 4); return (uint64_t)b; }
static inline VALUE FROM_F64(double   x) { uint64_t b; memcpy(&b, &x, 8); return b; }
static inline VALUE FROM_BOOL(int     x) { return x ? 1 : 0; }

#define WASTRO_STACK_SIZE   (64 * 1024)
#define WASTRO_MAX_FUNCS    256
#define WASTRO_PAGE_SIZE    (64 * 1024)   // wasm linear memory page
#define WASTRO_MAX_GLOBALS  256

// Print msg and exit (1).  Used for traps (div-by-zero, OOB memory, etc.).
__attribute__((noreturn))
static inline void
wastro_trap(const char *msg)
{
    fprintf(stderr, "wastro: trap: %s\n", msg);
    exit(1);
}

// Branch / return state.
// `br_depth == 0` means "no branch in flight" — EVAL falls through normally.
// `br_depth > 0` means "branching out N labels" — block/loop decrement on
// the way out and consume when the count reaches 0.  WASTRO_BR_RETURN is a
// special sentinel that means "return from function" — it is consumed by
// node_call_N, never by block/loop.
#define WASTRO_BR_RETURN   ((uint32_t)0xFFFFFFFFu)

typedef struct CTX_struct {
    VALUE stack[WASTRO_STACK_SIZE];
    VALUE *fp;             // base of the current function's locals
    VALUE *sp;             // first free slot — next frame's `fp`
    uint32_t br_depth;     // 0 = none; UINT_MAX = return; otherwise br count
    VALUE br_value;        // value carried by a typed br / return

    // Linear memory.  NULL if the module has no (memory ...) declaration.
    // Single linear memory only (wasm 1.0 limitation).
    uint8_t *memory;
    uint32_t memory_pages;     // current size in 64KB pages
    uint32_t memory_max_pages; // max growth limit (0xFFFFFFFF if unspecified)
} CTX;

// Wasm value types — only the four numeric types in v0.5.
typedef enum {
    WT_VOID = 0,
    WT_I32,
    WT_I64,
    WT_F32,
    WT_F64,
} wtype_t;

#define WASTRO_MAX_PARAMS 8

struct CTX_struct;
typedef VALUE (*wastro_host_fn_t)(struct CTX_struct *c, VALUE *args, uint32_t argc);

struct wastro_function {
    const char *name;     // optional symbolic name ("$fib"), NULL if unnamed
    int exported;         // 1 if exported, else 0
    const char *export_name; // export name, or NULL
    uint32_t param_cnt;
    wtype_t  param_types[WASTRO_MAX_PARAMS];
    wtype_t  result_type; // WT_VOID if no result
    uint32_t local_cnt;   // total = params + extra body locals
    wtype_t  local_types[64];
    struct Node *body;

    // Imports
    int is_import;
    wastro_host_fn_t host_fn;
};

struct wastro_option {
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;
    bool quiet;
};

extern struct wastro_option OPTION;

#endif
