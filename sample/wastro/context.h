#ifndef CONTEXT_H
#define CONTEXT_H

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>     // mprotect / PROT_* — used by node_memory_grow

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

// A wasm local slot.  All four numeric types share an 8-byte
// (uint64_t-aligned) cell so that an array of slots is a uniform
// memory layout regardless of which types the function declares; the
// per-local wasm type chooses which union member is read/written.
//
// The frame pointer (third dispatcher parameter, `union wastro_slot
// *frame`) points at the start of such an array allocated by the
// caller's call_N (or by wastro_invoke at the entry boundary).  Each
// `local.get_<type>` / `local.set_<type>` op accesses `frame[i].<type>`
// directly — gcc sees a typed access and can SROA each slot at its
// real C type when the SD chain inlines.
union wastro_slot {
    int32_t  i32;
    int64_t  i64;
    float    f32;
    double   f64;
    uint64_t raw;   // type-erased view, used by call_N to write args
                    // before the callee specializes the read.
};

#define WASTRO_STACK_SIZE   (64 * 1024)
#define WASTRO_MAX_FUNCS    1024
#define WASTRO_PAGE_SIZE    (64 * 1024)   // wasm linear memory page
#define WASTRO_MAX_GLOBALS  1024

// Trap.  Default handler prints msg and exits.  The spec-test harness
// installs a longjmp handler instead so that asserts can recover.
__attribute__((noreturn))
extern void wastro_trap(const char *msg);

// Branch state convention.
// `br_depth == 0` means "normal flow" — RESULT.value carries the operator's
// produced value.
// `br_depth >  0` means "branching out N labels" — block/loop decrement on
// the way out and consume when the count reaches 0.  RESULT.value carries
// the value carried by `(br N value)` (or 0 for `(br N)`).
// `br_depth == WASTRO_BR_RETURN` is a sentinel for `return` / `unreachable`
// trap propagation — consumed only at the function boundary.
#define WASTRO_BR_RETURN   ((uint32_t)0xFFFFFFFFu)

// RESULT — every node dispatcher returns one of these.  At 12 bytes
// (8-byte VALUE + 4-byte br_depth, padded to 16 by alignment) it fits
// in the rax+rdx pair under SysV x86_64, so branch state never hits
// memory in interpreter / specialized code.  Idiomatic abruby pattern.
typedef struct {
    VALUE    value;
    uint32_t br_depth;
} RESULT;

#define RESULT_OK(v)             ((RESULT){(v), 0})
#define RESULT_BR(v, depth)      ((RESULT){(v), (depth) + 1})
#define RESULT_RETURN_RES(v)     ((RESULT){(v), WASTRO_BR_RETURN})

typedef struct CTX_struct {
    VALUE stack[WASTRO_STACK_SIZE];
    VALUE *fp;             // base of the current function's locals
    VALUE *sp;             // first free slot — next frame's `fp`

    // Linear memory.  NULL if the module has no (memory ...) declaration.
    // Single linear memory only (wasm 1.0 limitation).
    uint8_t *memory;
    uint32_t memory_pages;     // current size in 64KB pages
    uint32_t memory_max_pages; // max growth limit (0xFFFFFFFF if unspecified)
    uint64_t memory_size_bytes;// cached `memory_pages * WASTRO_PAGE_SIZE`,
                               // used by every load/store bounds check —
                               // re-derived only on memory.grow so the hot
                               // path is one compare instead of a load+shift.
} CTX;

// Wasm value types — only the four numeric types in v0.5.
// WT_POLY is the polymorphic-stack type emitted by always-branching
// instructions (br, br_table, return, unreachable).  It satisfies any
// expected type at parse-time.
typedef enum {
    WT_VOID = 0,
    WT_I32,
    WT_I64,
    WT_F32,
    WT_F64,
    WT_POLY,
} wtype_t;

// Sanity cap on per-function param/local counts.  Storage is dynamic
// (param_types / local_types are heap-allocated and sized to actual
// count); this constant is only used to reject pathological inputs.
#define WASTRO_MAX_PARAMS 1024

struct CTX_struct;
typedef VALUE (*wastro_host_fn_t)(struct CTX_struct *c, VALUE *args, uint32_t argc);

// A wasm function type signature (used by (type ...) declarations and
// by call_indirect for runtime structural type checks).
//
// `param_types` is a heap allocation of exactly `param_cnt` entries
// (NULL when param_cnt == 0); freed when the module is reset.
struct wastro_type_sig {
    uint32_t param_cnt;
    wtype_t *param_types;
    wtype_t  result_type;
};

// `param_types` and `local_types` are heap allocations sized to
// `param_cnt` / `local_cnt` respectively (NULL when count == 0),
// freed when the module is reset.  The parser allocates them via
// `wastro_function_set_params` / `wastro_function_set_locals`.
struct wastro_function {
    const char *name;     // optional symbolic name ("$fib"), NULL if unnamed
    int exported;         // 1 if exported, else 0
    const char *export_name; // export name, or NULL
    uint32_t param_cnt;
    wtype_t *param_types;
    wtype_t  result_type; // WT_VOID if no result
    uint32_t local_cnt;   // total = params + extra body locals
    wtype_t *local_types;
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
