#ifndef LUASTRO_CONTEXT_H
#define LUASTRO_CONTEXT_H

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>

// =====================================================================
// LuaValue — tagged union covering all Lua types.
// =====================================================================
//
// Lua 5.4 distinguishes integer and float subtypes of "number"; we keep
// them as separate tags so specialized arithmetic nodes can dispatch
// without a runtime branch in the fast path.

typedef enum {
    LUA_TNIL    = 0,
    LUA_TBOOL,
    LUA_TINT,
    LUA_TFLOAT,        // (heap-boxed) — LV_IS_FLOAT also covers inline flonum
    LUA_TSTRING,
    LUA_TTABLE,
    LUA_TFUNC,         // Lua closure
    LUA_TCFUNC,        // C function (host)
    LUA_TTHREAD,       // coroutine
    LUA_TBOXED,        // LuaBox cell for a captured local
} lua_type_t;

struct LuaString;
struct LuaTable;
struct LuaClosure;
struct LuaCFunction;
struct LuaValue;
struct Node;

typedef struct LuaString    LuaString;
typedef struct LuaTable     LuaTable;
typedef struct LuaClosure   LuaClosure;
typedef struct LuaCFunction LuaCFunction;

// =====================================================================
// LuaValue — tagged 8-byte word.  CRuby-inspired layout:
//
//   bits 2..0:
//      000 = pointer to heap object (LuaString / LuaTable / LuaClosure /
//            LuaCFunction / LuaCoroutine / LuaBox).  The object's
//            GCHead.type byte distinguishes them.  Pointers must be
//            ≥ 8-byte aligned (we calloc, so they always are).
//      001 = fixnum: bits 63..1 are the signed 63-bit integer value.
//      010 = flonum: CRuby-style bit-rotated double.  Most doubles in
//            "normal" computational range fit inline; out-of-range
//            (denormals, NaN, Inf, very tiny) heap-box as LUA_TFLOAT.
//   Singletons (whole-word patterns):
//      0x00 = nil       (so calloc-zeroed memory is naturally nil)
//      0x14 = false
//      0x24 = true
//   Special: 0x8000000000000002 represents +0.0 (which would otherwise
//   rotate-encode to all-zero, colliding with the nil singleton).
//
// Heap object type (after LV_IS_PTR check) is read from GCHead.type.
// =====================================================================

typedef uint64_t LuaValue;

#define LV_NIL_BITS       ((LuaValue)0x00)
#define LV_FALSE_BITS     ((LuaValue)0x14)
#define LV_TRUE_BITS      ((LuaValue)0x24)
#define LV_FLONUM_TAG     ((LuaValue)0x02)
#define LV_FLONUM_ZERO    ((LuaValue)0x8000000000000002ULL)

// Predicates — bit-level only (no type-tag derivation).  A pointer is
// any value that is 8-byte aligned and non-zero (zero would be nil).
#define LV_IS_PTR(v)      ((((v) & 7) == 0) && ((v) != 0))
#define LV_IS_INT(v)      (((v) & 1) != 0)
#define LV_IS_FLONUM(v)   (((v) & 3) == 2)
#define LV_IS_NIL(v)      ((v) == LV_NIL_BITS)
#define LV_IS_FALSE(v)    ((v) == LV_FALSE_BITS)
#define LV_IS_TRUE(v)     ((v) == LV_TRUE_BITS)
#define LV_IS_BOOL(v)     ((v) == LV_FALSE_BITS || (v) == LV_TRUE_BITS)
#define LV_IS_NUM(v)      (LV_IS_INT(v) || LV_IS_FLONUM(v) || LV_IS_HEAP_OF(v, LUA_TFLOAT))
// Heap-object subtype: GCHead.type is at offset 0 of every heap object,
// so a single byte load through the pointer gives us the type without
// needing a full struct definition here.
#define lv_heap_type(v)     (*(const uint8_t *)(uintptr_t)(v))
#define LV_IS_HEAP_OF(v, T) (LV_IS_PTR(v) && lv_heap_type(v) == (T))

// Constructors.
#define LUAV_NIL          LV_NIL_BITS
#define LUAV_BOOL(x)      ((x) ? LV_TRUE_BITS : LV_FALSE_BITS)
#define LUAV_INT(x)       ((LuaValue)(((uint64_t)(int64_t)(x) << 1) | 1))
static inline LuaValue luav_from_double(double d);
#define LUAV_FLOAT(x)     luav_from_double((double)(x))
#define LUAV_STR(x)       ((LuaValue)(uintptr_t)(x))
#define LUAV_TABLE(x)     ((LuaValue)(uintptr_t)(x))
#define LUAV_FUNC(x)      ((LuaValue)(uintptr_t)(x))
#define LUAV_CFUNC(x)     ((LuaValue)(uintptr_t)(x))
#define LUAV_THREAD(x)    ((LuaValue)(uintptr_t)(x))

// Accessors.
#define LV_AS_INT(v)      ((int64_t)(v) >> 1)               // arithmetic shift sign-extends
#define LV_AS_BOOL(v)     ((v) == LV_TRUE_BITS)
static inline double luav_to_double(LuaValue v);
#define LV_AS_FLOAT(v)    luav_to_double(v)
#define LV_AS_STR(v)      ((struct LuaString    *)(uintptr_t)(v))
#define LV_AS_TBL(v)      ((struct LuaTable     *)(uintptr_t)(v))
#define LV_AS_FN(v)       ((struct LuaClosure   *)(uintptr_t)(v))
#define LV_AS_CF(v)       ((struct LuaCFunction *)(uintptr_t)(v))
#define LV_AS_PTR(v)      ((void               *)(uintptr_t)(v))

// Truthiness — Lua: nil and false are false, everything else true
// (including 0, 0.0, "", and empty tables).
#define LV_TRUTHY(v)      (!(LV_IS_NIL(v) || LV_IS_FALSE(v)))

// Predicate: callable (Lua closure or C function — table __call lookup
// happens at the call site, not via the predicate).
#define LV_IS_STR(v)      LV_IS_HEAP_OF(v, LUA_TSTRING)
#define LV_IS_TBL(v)      LV_IS_HEAP_OF(v, LUA_TTABLE)
#define LV_IS_FN(v)       LV_IS_HEAP_OF(v, LUA_TFUNC)
#define LV_IS_CF(v)       LV_IS_HEAP_OF(v, LUA_TCFUNC)
#define LV_IS_THREAD(v)   LV_IS_HEAP_OF(v, LUA_TTHREAD)
#define LV_IS_BOX(v)      LV_IS_HEAP_OF(v, LUA_TBOXED)
#define LV_IS_CALL(v)     (LV_IS_PTR(v) && (lv_heap_type(v) == LUA_TFUNC || lv_heap_type(v) == LUA_TCFUNC))

// Float predicate: either inline flonum or heap-boxed out-of-range double.
#define LV_IS_FLOAT(v)    (LV_IS_FLONUM(v) || LV_IS_HEAP_OF(v, LUA_TFLOAT))

// GC header — every heap-allocated GC-tracked object starts with this
// so the collector can walk the all-objects list and mark/sweep.
//
// `type` deliberately lives at offset 0 so that LV_IS_TBL / LV_IS_STR
// etc. can read the heap object's type with a single byte load from
// the pointer, without going through GCHead.next first.
struct GCHead {
    uint8_t        type;       // mirrors lua_type_t for the object
    uint8_t        mark;       // 0 = white (sweep), 1 = grey/black (live)
    uint8_t        weak_mode;  // for tables: 0/1/2/3 = none/k/v/kv
    uint8_t        _pad;
    struct GCHead *next;
};

// LuaBox: heap cell wrapping a single LuaValue, used as the storage
// for captured locals.  A captured slot in `frame[]` always stores a
// pointer to a LuaBox; node_box_get / node_box_set know that
// statically (parser-time captured-local rewrite).
struct LuaBox {
    struct GCHead gc;
    LuaValue      value;
};

// LuaHeapDouble: heap-boxed double for values that don't fit the inline
// flonum encoding (denormals, very large/small magnitudes).
struct LuaHeapDouble {
    struct GCHead gc;
    double        value;
};

// LuaString — interned, hashed string.
struct LuaString {
    struct GCHead gc;
    uint64_t hash;
    uint32_t len;
    char     data[];
};

// LuaTable — hybrid array + hash, optional metatable.
struct LuaTabEntry { LuaValue key; LuaValue val; };
struct LuaTable {
    struct GCHead       gc;
    LuaValue           *array;
    uint32_t            arr_cnt, arr_cap;
    struct LuaTabEntry *hash;
    uint32_t            hash_cap, hash_cnt;
    struct LuaTable    *metatable;
};

// LuaClosure — Lua function with upvalue cells.
struct LuaClosure {
    struct GCHead gc;
    struct Node *body;
    uint32_t nparams, nlocals, nupvals;
    bool     is_vararg;
    const char *name;
    LuaValue **upvals;
};

// =====================================================================
// Inline-flonum encode / decode.
// =====================================================================
//
// CRuby-inspired but uses a shift-based scheme rather than rotation, so
// the round-trip is provable by inspection.
//
// IEEE 754 double bit layout (63..0):
//   [63] sign  [62..52] biased exponent  [51..0] mantissa
//
// We accept doubles whose top 3 exponent bits (bits 60..62 of the
// double) are 011 ("b62=3", magnitudes ≈ 2^-255..1) or 100 ("b62=4",
// magnitudes ≈ 2..2^256).  Together this covers the common
// computational range.  Out-of-range doubles (denormals, ±0, ±Inf,
// NaN, magnitudes > 2^256) heap-box as `LuaHeapDouble`.
//
// Encoded layout (64 bits):
//   bit  0      : 0  ─┐ tag = 10  (LV_IS_FLONUM == ((v & 3) == 2))
//   bit  1      : 1  ─┘
//   bit  2      : sign  (orig bit 63)
//   bit  3      : disc  (1 = b62=3, 0 = b62=4)
//   bits 4..63  : low60 of orig (orig bits 0..59)
//
// `disc` together with the gate gives us the missing 4 high bits of
// orig (bits 60..63): {disc, b62-fixed-bits}.  No mantissa precision
// loss — every bit of the original double is preserved exactly.
//
// `luav_box_double` / `luav_unbox_double` handle the heap fallback
// (out of line in lua_runtime.c).

LuaValue luav_box_double(double d);
double   luav_unbox_double(LuaValue v);

static inline LuaValue
luav_from_double(double d)
{
    union { double d; uint64_t u; } t;
    t.d = d;
    int b62 = (int)((t.u >> 60) & 7);
    if (__builtin_expect(b62 == 3 || b62 == 4, 1)) {
        uint64_t sign  = (t.u >> 63) & 1;
        uint64_t disc  = (uint64_t)(b62 == 3);
        uint64_t low60 = t.u & ((1ULL << 60) - 1);
        return (LuaValue)((low60 << 4) | (disc << 3) | (sign << 2) | 2);
    }
    return luav_box_double(d);
}

static inline double
luav_to_double(LuaValue v)
{
    if (__builtin_expect(((v) & 3) == 2, 1)) {       // LV_IS_FLONUM inline
        uint64_t low60 = (uint64_t)v >> 4;
        uint64_t disc  = ((uint64_t)v >> 3) & 1;
        uint64_t sign  = ((uint64_t)v >> 2) & 1;
        uint64_t high4 = disc ? 0x3 : 0x4;
        union { double d; uint64_t u; } t;
        t.u = (sign << 63) | (high4 << 60) | low60;
        return t.d;
    }
    return luav_unbox_double(v);
}

// GC API — declared after CTX typedef below.

// =====================================================================
// VALUE — the dispatcher payload.  We carry a full LuaValue by value;
// it's 16 bytes which fits in rax+rdx under SysV x86_64.
// =====================================================================
typedef LuaValue VALUE;

// Branch-state convention.  Same idea as wastro:
//   br == 0          → normal flow, RESULT.value is the expression value
//   br == BR_BREAK   → propagating a `break`
//   br == BR_RETURN  → propagating a `return`
//   br == BR_CONTINUE→ propagating a `continue` (goto ::continue::)
//   br == BR_GOTO    → propagating a goto; the target label lives in c->goto_target
#define LUA_BR_NORMAL    0u
#define LUA_BR_BREAK     1u
#define LUA_BR_RETURN    2u
#define LUA_BR_CONTINUE  3u
#define LUA_BR_GOTO      4u

// RESULT — for v1, just an 8-byte LuaValue.  Branch state lives in the
// LUASTRO_BR / LUASTRO_BR_VAL globals (set by RESULT_RETURN_ / _BREAK_).
// An earlier draft folded br into the return struct (rax+rdx) but the
// per-call zero-init cost outweighed the saved global write in
// micro-benchmarks; revisit later.
typedef LuaValue RESULT;

// Branch-state globals — set by node_break/node_return/node_goto in EVAL
// bodies, read by every loop / sequence node.  Defined in lua_runtime.c.
extern uint32_t LUASTRO_BR;
extern LuaValue LUASTRO_BR_VAL;

#define RESULT_OK(v)        ((RESULT)(v))
#define RESULT_BREAK_()     (LUASTRO_BR = LUA_BR_BREAK,    LUASTRO_BR_VAL = LUAV_NIL, (RESULT)LUAV_NIL)
#define RESULT_RETURN_(v)   (LUASTRO_BR = LUA_BR_RETURN,   LUASTRO_BR_VAL = (v),       (RESULT)(v))
#define RESULT_CONTINUE_()  (LUASTRO_BR = LUA_BR_CONTINUE, LUASTRO_BR_VAL = LUAV_NIL, (RESULT)LUAV_NIL)

// =====================================================================
// CTX — execution context.
// =====================================================================

#define LUASTRO_STACK_SIZE     (4 * 1024 * 1024)
#define LUASTRO_MAX_RETS       64

struct lua_call_info {
    LuaValue results[LUASTRO_MAX_RETS];
    uint32_t result_cnt;
};

typedef struct CTX_struct {
    LuaValue *stack;          // base of stack
    LuaValue *stack_end;
    LuaValue *sp;             // first free slot

    // Globals — single shared table, indexed by string.
    struct LuaTable *globals;

    // String intern pool.
    struct LuaStrPool *strpool;

    // Multi-return scratch (set by node_call when callee returns multiple
    // values; consumed by `local a,b = f()` / unpack-style sites).
    struct lua_call_info ret_info;

    // Variadic args for the current function (`...`).
    LuaValue *varargs;
    uint32_t  varargs_cnt;

    // Error / pcall machinery — chain of jmp_bufs.  `pcall` pushes one
    // and longjmps on `error()`.
    struct lua_pcall_frame *pcall_top;

    // Last error message (set by lua_raise; consumed by pcall).
    LuaValue last_error;

    // Goto target name when br == LUA_BR_GOTO.  Lifetime: the label
    // string is owned by the AST (interned).
    struct LuaString *goto_target;

#if LUASTRO_DEBUG_EVAL
    int rec_cnt;
#endif
} CTX;

struct lua_pcall_frame {
    jmp_buf jb;
    struct lua_pcall_frame *prev;
    LuaValue *sp_save;
};

// Public runtime APIs (defined in lua_runtime.c) ----------------------

// Strings
struct LuaString *lua_str_intern(const char *cstr);
struct LuaString *lua_str_intern_n(const char *bytes, size_t len);
const char       *lua_str_data(const struct LuaString *s);
size_t            lua_str_len(const struct LuaString *s);
struct LuaString *lua_str_concat(struct LuaString *a, struct LuaString *b);
bool              lua_str_eq(const struct LuaString *a, const struct LuaString *b);
uint64_t          lua_str_hash(const struct LuaString *s);

// Tables
struct LuaTable *lua_table_new(uint32_t nseq, uint32_t nhash);
LuaValue         lua_table_geti(struct LuaTable *t, int64_t i);
LuaValue         lua_table_get (struct LuaTable *t, LuaValue key);
LuaValue         lua_table_get_str(struct LuaTable *t, struct LuaString *key);
void             lua_table_seti(struct LuaTable *t, int64_t i, LuaValue v);
void             lua_table_set (struct LuaTable *t, LuaValue key, LuaValue v);
void             lua_table_set_str(struct LuaTable *t, struct LuaString *key, LuaValue v);
int64_t          lua_table_len(struct LuaTable *t);
struct LuaTable *lua_table_metatable(struct LuaTable *t);
void             lua_table_set_metatable(struct LuaTable *t, struct LuaTable *mt);
bool             lua_table_next(struct LuaTable *t, LuaValue *kio, LuaValue *vout);

// Closures
struct LuaClosure *lua_closure_new(struct Node *body, uint32_t nparams,
                                   uint32_t nlocals, uint32_t nupvals,
                                   bool is_vararg, const char *name);
struct LuaClosure *lua_closure_with_upvals(struct LuaClosure *proto, LuaValue **upvals);
LuaValue          *lua_closure_upval (struct LuaClosure *cl, uint32_t i);

// C functions
typedef RESULT (*lua_cfunc_ptr_t)(CTX *c, LuaValue *args, uint32_t argc);
struct LuaCFunction {
    struct GCHead   gc;
    const char     *name;
    lua_cfunc_ptr_t fn;
};
struct LuaCFunction *lua_cfunc_new(const char *name, lua_cfunc_ptr_t fn);

// Conversions
bool     lua_to_int(LuaValue v, int64_t *out);
bool     lua_to_float(LuaValue v, double *out);
bool     lua_to_bool(LuaValue v);
LuaValue lua_tostring(CTX *c, LuaValue v);
const char *lua_type_name(LuaValue v);

// Errors / pcall
__attribute__((noreturn)) void lua_raise(CTX *c, LuaValue err);
__attribute__((noreturn)) void lua_raisef(CTX *c, const char *fmt, ...);
RESULT lua_pcall(CTX *c, LuaValue fn, LuaValue *args, uint32_t argc);

// Equality / comparison (with metamethods if applicable)
bool     lua_eq(CTX *c, LuaValue a, LuaValue b);
bool     lua_lt(CTX *c, LuaValue a, LuaValue b);
bool     lua_le(CTX *c, LuaValue a, LuaValue b);

// Function call (general-purpose; node_call_* fast paths inline this)
RESULT   lua_call(CTX *c, LuaValue fn, LuaValue *args, uint32_t argc);

// Length operator (#) — strings and tables, with __len metamethod.
int64_t  lua_len(CTX *c, LuaValue v);

// Variable-arity arithmetic / compare with metatable handling.  Used
// when both operands are not the same primitive type.
LuaValue lua_arith(CTX *c, int op, LuaValue a, LuaValue b);
LuaValue lua_unm  (CTX *c, LuaValue a);
LuaValue lua_concat(CTX *c, LuaValue a, LuaValue b);

// Arithmetic op codes (used by lua_arith).
enum {
    LUA_OP_ADD = 0, LUA_OP_SUB, LUA_OP_MUL, LUA_OP_DIV,
    LUA_OP_FLOORDIV, LUA_OP_MOD, LUA_OP_POW,
    LUA_OP_BAND, LUA_OP_BOR, LUA_OP_BXOR, LUA_OP_BNOT, LUA_OP_SHL, LUA_OP_SHR,
};

// Setup
void luastro_init_globals(CTX *c);
CTX *luastro_create_context(void);

// GC
void   luastro_gc_register(void *obj, uint8_t type);
void   luastro_gc_collect(CTX *c);
size_t luastro_gc_total(void);

// Pretty-print a value (for debugging / `print`).
void lua_print_value(FILE *fp, LuaValue v);

// =====================================================================
// CLI option flags (mirrors abruby/naruby/wastro pattern)
// =====================================================================
struct luastro_option {
    bool quiet;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;
    bool compile_first;    // -c / --aot-compile-first: bake SDs, then run with them active
    bool aot_only;         // --aot-compile: bake SDs and exit (no run)
    bool pg_mode;          // -p / --pg-compile: run first, then bake using profile
    bool dump_ast;         // --dump
    bool verbose;
};

extern struct luastro_option OPTION;

// =====================================================================
// Convenience NODE forward decl (the full struct comes from node_head.h
// which is generated).  Keeps this header standalone-includeable.
// =====================================================================
struct Node;

#endif // LUASTRO_CONTEXT_H
