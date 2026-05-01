#ifndef JSTRO_CONTEXT_H
#define JSTRO_CONTEXT_H

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>

// ====================================================================
// JsValue — tagged 64-bit value covering every ECMAScript Language Value.
// ====================================================================
//
// Per ES2023 §6.1, the JS Language Type set is:
//   Undefined, Null, Boolean, String, Symbol, Number, BigInt, Object.
//
// jstro implements a strict subset: Undefined, Null, Boolean, String,
// Number, Object.  (Symbol/BigInt are out of scope — see README.md.)
//
// Value layout (low 3 bits dispatch):
//
//   ...000  pointer to GCObject (heap) — must be non-zero
//   ...001  SMI: bit 63..1 = signed 31..63-bit integer (sign-extending shift)
//   ...010  flonum (CRuby-style shifted double)
//   ...100  singleton constants (low 3 bits = 100)
//
// Singleton encodings:
//   0x00  undefined  (calloc-zeroed memory naturally reads as undefined)
//   0x04  null
//   0x14  false
//   0x24  true
//   0x34  "the hole" — internal-use sentinel for sparse arrays / TDZ
//
// Note: 0x00 (undefined) does NOT have low-3-bits == 100; we test it
// explicitly.  This keeps `JV_IS_PTR(v)` simple: aligned + non-zero.
// ====================================================================

typedef uint64_t JsValue;

#define JV_UNDEFINED   ((JsValue)0x00)
#define JV_NULL        ((JsValue)0x04)
#define JV_FALSE       ((JsValue)0x14)
#define JV_TRUE        ((JsValue)0x24)
#define JV_HOLE        ((JsValue)0x34)

#define JV_FLONUM_TAG  ((JsValue)0x02)
#define JV_FLONUM_ZERO ((JsValue)0x8000000000000002ULL)

// Predicates — bit-level only.
#define JV_IS_UNDEFINED(v) ((v) == JV_UNDEFINED)
#define JV_IS_NULL(v)      ((v) == JV_NULL)
#define JV_IS_NULLISH(v)   (((v) | 0x04) == JV_NULL)        // undefined or null
#define JV_IS_FALSE(v)     ((v) == JV_FALSE)
#define JV_IS_TRUE(v)      ((v) == JV_TRUE)
#define JV_IS_BOOL(v)      ((v) == JV_FALSE || (v) == JV_TRUE)
#define JV_IS_SMI(v)       (((v) & 1) != 0)
#define JV_IS_FLONUM(v)    (((v) & 3) == 2)
#define JV_IS_NUM(v)       (JV_IS_SMI(v) || JV_IS_FLONUM(v) || JV_IS_HEAP_OF(v, JS_TFLOAT))
#define JV_IS_PTR(v)       ((((v) & 7) == 0) && ((v) != 0))
#define JV_IS_HOLE(v)      ((v) == JV_HOLE)

// Object subtype byte at offset 0 (GCHead.type).
#define jv_heap_type(v)    (*(const uint8_t *)(uintptr_t)(v))
#define JV_IS_HEAP_OF(v, T)  (JV_IS_PTR(v) && jv_heap_type(v) == (T))

#define JV_IS_STR(v)       JV_IS_HEAP_OF(v, JS_TSTRING)
#define JV_IS_OBJ(v)       (JV_IS_PTR(v) && (jv_heap_type(v) >= JS_TOBJECT))
#define JV_IS_ARRAY(v)     JV_IS_HEAP_OF(v, JS_TARRAY)
#define JV_IS_FUNC(v)      JV_IS_HEAP_OF(v, JS_TFUNCTION)
#define JV_IS_CFUNC(v)     JV_IS_HEAP_OF(v, JS_TCFUNCTION)
#define JV_IS_FLOAT_BOX(v) JV_IS_HEAP_OF(v, JS_TFLOAT)

// Constructors.
#define JV_INT(x)          ((JsValue)(((uint64_t)(int64_t)(x) << 1) | 1))
#define JV_BOOL(x)         ((x) ? JV_TRUE : JV_FALSE)
static inline JsValue jv_from_double(double d);
#define JV_DBL(x)          jv_from_double((double)(x))
#define JV_STR(p)          ((JsValue)(uintptr_t)(p))
#define JV_OBJ(p)          ((JsValue)(uintptr_t)(p))
#define JV_FUNC(p)         ((JsValue)(uintptr_t)(p))

// Accessors.
#define JV_AS_SMI(v)       ((int64_t)(v) >> 1)               // arithmetic shift
#define JV_AS_BOOL(v)      ((v) == JV_TRUE)
static inline double jv_to_inline_double(JsValue v);
#define JV_AS_DBL(v)       jv_to_inline_double(v)
#define JV_AS_STR(v)       ((struct JsString  *)(uintptr_t)(v))
#define JV_AS_OBJ(v)       ((struct JsObject  *)(uintptr_t)(v))
#define JV_AS_ARRAY(v)     ((struct JsArray   *)(uintptr_t)(v))
#define JV_AS_FUNC(v)      ((struct JsFunction*)(uintptr_t)(v))
#define JV_AS_CFUNC(v)     ((struct JsCFunction*)(uintptr_t)(v))
#define JV_AS_PTR(v)       ((void *)(uintptr_t)(v))

// ECMAScript truthiness — ToBoolean (§7.1.5):
//   undefined, null, false, +0, -0, NaN, ""    → false
//   everything else (incl. {}, [], Number objects, etc.) → true
static inline bool jv_to_bool(JsValue v);

// Heap object types.  GCHead.type is at offset 0; placing the most
// frequently tested types first lets predicate checks generate tight
// compares.
typedef enum {
    JS_TSTRING    = 1,
    JS_TFLOAT     = 2,        // heap-boxed double (denormals/Inf/NaN)
    JS_TBOX       = 3,        // captured-local cell
    JS_TOBJECT    = 16,       // plain object
    JS_TARRAY     = 17,
    JS_TFUNCTION  = 18,       // JS function (closure)
    JS_TCFUNCTION = 19,       // host C function
    JS_TERROR     = 20,       // Error / TypeError / etc.
    JS_TREGEXP    = 21,       // future
    JS_TMAP       = 32,       // Map
    JS_TSET       = 33,       // Set
    JS_TMAPITER   = 34,       // Map/Set iterator
    JS_TACCESSOR  = 35,       // {get, set} stored in a property slot
    JS_TSYMBOL    = 36,       // Symbol (unique value with optional description)
    JS_TBIGINT    = 37,       // BigInt (currently int64-backed, no overflow)
    JS_TREGEX     = 38,       // RegExp
    JS_TPROXY     = 39,       // Proxy: { target, handler }
    JS_TGENERATOR = 40,       // Generator (function*) — ucontext-based
    JS_TPROMISE   = 41,       // Promise (sync-resolving)
} js_obj_type_t;

// Per-object flag bits stored in GCHead.flags.
#define JS_OBJ_FROZEN     0x01
#define JS_OBJ_SEALED     0x02
#define JS_OBJ_NOT_EXTENS 0x04

struct GCHead {
    uint8_t        type;      // js_obj_type_t
    uint8_t        mark;      // GC mark byte
    uint8_t        flags;     // per-type flag byte (e.g. extensible)
    uint8_t        _pad;
    struct GCHead *next;
};

// Boxed (captured) local cell.
struct JsBox {
    struct GCHead gc;
    JsValue       value;
};

// Heap-boxed double.
struct JsHeapDouble {
    struct GCHead gc;
    double        value;
};

// Inline cache for property access.  Shape-based: shape token + slot id.
//
// `pre_shape` is used by member_set to recognise the constructor
// pattern `this.x = x; this.y = y; ...` where the object's shape
// transitions on each set.  When `o->shape == pre_shape`, we know
// taking the cached transition lands at slot `slot` of the post-state
// (`shape`).  pre_shape == 0 for member_get sites (no transition).
struct JsPropIC {
    uintptr_t shape;       // JsObject.shape at last successful resolve (post-state)
    uint32_t  slot;        // resolved slot index
    uint32_t  _pad;
    uintptr_t pre_shape;   // pre-state for constructor-style sets
};

// Inline cache for call sites.
struct JsCallIC {
    struct JsFunction *cached_fn;
    struct Node       *cached_body;
    uint32_t           cached_nlocals;
    uint32_t           _pad;
};

// String: interned, immutable.  data[] holds UTF-8 bytes; len is
// byte-length (we treat .length per spec as approximate for ASCII —
// accurate UTF-16 length tracking is in a follow-up).
struct JsString {
    struct GCHead gc;
    uint32_t hash;
    uint32_t len;
    char     data[];
};
typedef struct JsString JsString;

// Property descriptor (slot of an object).  We store inline names+values
// in a "shape" — like V8 hidden classes.  Two objects with the same
// property-name sequence share a Shape, so property lookup IC keys on
// shape pointer.
struct JsShape {
    struct GCHead    gc;
    uint32_t         nslots;
    uint32_t         capa;
    struct JsString **names;        // length = nslots
    // Transition table: name → next-shape.  Linear probe (ok for ≤8;
    // upgrades to hash above that).
    uint32_t         ntrans;
    uint32_t         tcap;
    struct JsShapeTrans {
        struct JsString *name;
        struct JsShape  *to;
    } *trans;
    struct JsShape  *parent;        // shape we transitioned from
};

// Plain object (Object.prototype-derived by default).
//
// Small objects (shape->nslots ≤ JS_INLINE_SLOTS) keep their values in
// `inline_slots` and `slots` aliases at `&inline_slots[0]`.  Once the
// object grows past the inline capacity, we malloc a separate buffer
// and `slots` points there instead.  Saves one malloc + one free per
// small object (most JS objects are small) — measurable on
// binary_trees / object-heavy workloads.
#define JS_INLINE_SLOTS 4
struct JsObject {
    struct GCHead    gc;
    struct JsShape  *shape;
    JsValue         *slots;         // points to inline_slots OR a malloc'd buffer
    uint32_t         slot_capa;     // slots[] allocation size
    struct JsObject *proto;         // [[Prototype]]
    JsValue          inline_slots[JS_INLINE_SLOTS];
};

// Array: dense backing + sparse fallback.  We try to keep `dense`
// populated; fall back to a JsObject-style hash on holes / non-int keys.
struct JsArray {
    struct GCHead    gc;
    JsValue         *dense;
    uint32_t         length;
    uint32_t         dense_capa;
    struct JsObject *fallback;      // for non-int keys / past-length
    struct JsObject *proto;
};

// JS function (closure).
struct JsFunction {
    struct GCHead    gc;
    struct Node     *body;
    uint32_t         nparams;       // declared param count (last is rest if is_vararg)
    uint32_t         nlocals;
    uint32_t         nupvals;
    uint8_t          is_arrow;
    uint8_t          is_strict;
    uint8_t          is_vararg;     // last param collects remaining args as array
    uint8_t          _pad;
    const char      *name;          // owned by AST or interned
    JsValue        **upvals;        // length = nupvals; each is a JsBox*
    struct JsObject *home_proto;    // for `new` — F.prototype
    struct JsObject *bound_this;    // arrow's captured this (NULL if not arrow)
    struct JsObject *own_props;     // arbitrary own properties (e.g. static methods)
};

// Host C function.
struct JsCFunction;
struct CTX_struct;
typedef JsValue (*js_cfunc_ptr_t)(struct CTX_struct *c, JsValue thisv,
                                  JsValue *args, uint32_t argc);
struct JsCFunction {
    struct GCHead   gc;
    const char     *name;
    js_cfunc_ptr_t  fn;
    uint32_t        nparams;        // for hint only (varargs always allowed)
    uint32_t        _pad;
    struct JsObject *own_props;     // arbitrary own properties (e.g. statics)
};

// Branch-state convention (for break/continue/return/throw propagation).
#define JS_BR_NORMAL    0u
#define JS_BR_BREAK     1u
#define JS_BR_CONTINUE  2u
#define JS_BR_RETURN    3u
#define JS_BR_THROW     4u

extern uint32_t JSTRO_BR;
extern JsValue  JSTRO_BR_VAL;
extern const char *JSTRO_BR_LABEL;   // for labeled break/continue

#define RESULT JsValue
#define VALUE  JsValue

#define RESULT_OK(v)        ((RESULT)(v))
#define RESULT_BREAK_(lbl)  (JSTRO_BR = JS_BR_BREAK,    JSTRO_BR_LABEL = (lbl), JSTRO_BR_VAL = JV_UNDEFINED, (RESULT)JV_UNDEFINED)
#define RESULT_CONT_(lbl)   (JSTRO_BR = JS_BR_CONTINUE, JSTRO_BR_LABEL = (lbl), JSTRO_BR_VAL = JV_UNDEFINED, (RESULT)JV_UNDEFINED)
#define RESULT_RETURN_(v)   (JSTRO_BR = JS_BR_RETURN,   JSTRO_BR_LABEL = NULL,  JSTRO_BR_VAL = (v),          (RESULT)(v))
#define RESULT_THROW_(v)    (JSTRO_BR = JS_BR_THROW,    JSTRO_BR_LABEL = NULL,  JSTRO_BR_VAL = (v),          (RESULT)(v))

// =====================================================================
// Inline-flonum encode / decode (CRuby-style, copied from luastro).
// =====================================================================
JsValue jv_box_double(double d);

static inline __attribute__((always_inline)) JsValue
jv_from_double(double d)
{
    union { double d; uint64_t u; } t;
    t.d = d;
    int b62 = (int)((t.u >> 60) & 7);
    if (__builtin_expect(b62 == 3 || b62 == 4, 1)) {
        uint64_t sign  = (t.u >> 63) & 1;
        uint64_t disc  = (uint64_t)(b62 == 3);
        uint64_t low60 = t.u & ((1ULL << 60) - 1);
        return (JsValue)((low60 << 4) | (disc << 3) | (sign << 2) | 2);
    }
    return jv_box_double(d);
}

static inline __attribute__((always_inline)) double
jv_to_inline_double(JsValue v)
{
    if (__builtin_expect(((v) & 3) == 2, 1)) {
        uint64_t low60 = (uint64_t)v >> 4;
        uint64_t disc  = ((uint64_t)v >> 3) & 1;
        uint64_t sign  = ((uint64_t)v >> 2) & 1;
        uint64_t high4 = disc ? 0x3 : 0x4;
        union { double d; uint64_t u; } t;
        t.u = (sign << 63) | (high4 << 60) | low60;
        return t.d;
    }
    return ((struct JsHeapDouble *)(uintptr_t)v)->value;
}

// =====================================================================
// CTX — execution context.
// =====================================================================

#define JSTRO_STACK_SIZE     (4 * 1024 * 1024 / sizeof(JsValue))

struct js_throw_frame {
    jmp_buf jb;
    struct js_throw_frame *prev;
    JsValue *frame_save;
    JsValue *sp_save;
    struct js_frame_link *frame_stack_save;  // for GC root unwind on throw
};

// Linked list of live alloca'd frames so the mark-and-sweep GC can find
// roots that aren't reachable through CTX.  Push on call entry, pop on
// return; throw-frames snapshot the head so GC stays consistent across
// longjmp.
struct js_frame_link {
    JsValue              *frame;     // alloca'd local slots
    uint32_t              nlocals;
    JsValue              *args;      // caller-provided args
    uint32_t              argc;
    struct js_frame_link *prev;
};

typedef struct CTX_struct {
    JsValue        *stack;
    JsValue        *stack_end;
    JsValue        *sp;

    struct JsObject *globals;
    struct JsString *intern_table_keys;
    struct JsObject *string_proto;
    struct JsObject *array_proto;
    struct JsObject *function_proto;
    struct JsObject *object_proto;
    struct JsObject *number_proto;
    struct JsObject *boolean_proto;
    struct JsObject *error_proto;
    struct JsObject *map_proto;
    struct JsObject *set_proto;
    struct JsObject *mapiter_proto;
    struct JsObject *regex_proto;

    JsValue         this_val;       // current `this`

    struct JsString **intern_keys;  // string intern table
    struct JsString **intern_buckets;
    uint32_t         intern_cap;
    uint32_t         intern_cnt;

    struct JsShape  *root_shape;    // empty-shape singleton

    struct js_throw_frame *throw_top;
    JsValue          last_error;

    JsValue        **cur_upvals;    // active closure's upvals
    JsValue         *cur_args;      // arguments seen by current call
    uint32_t         cur_argc;
    JsValue          new_target;    // constructor passed to `new`, else undefined
    struct js_frame_link *frame_stack;  // GC roots: stack of alloca'd frames

    JsValue          last_thrown;   // for throw inside generated SD code

    // GC tracking — single linked list of all live heap objects.
    struct GCHead   *all_objects;
    size_t           bytes_allocated;
    size_t           gc_threshold;
    int              gc_disabled;     // counter; >0 = no GC
} CTX;

// =====================================================================
// Public runtime API.
// =====================================================================

// String intern.
struct JsString *js_str_intern(CTX *c, const char *cstr);
struct JsString *js_str_intern_n(CTX *c, const char *bytes, size_t len);
struct JsString *js_str_concat(CTX *c, struct JsString *a, struct JsString *b);
const char      *js_str_data(struct JsString *s);
size_t           js_str_len(struct JsString *s);
bool             js_str_eq(struct JsString *a, struct JsString *b);

// Object / shape / property.
struct JsObject *js_object_new(CTX *c, struct JsObject *proto);
void             js_object_grow_slots(struct JsObject *o, uint32_t need);
struct JsShape  *js_shape_root(CTX *c);
struct JsShape  *js_shape_transition(CTX *c, struct JsShape *from, struct JsString *name);
void             js_object_set(CTX *c, struct JsObject *o, struct JsString *key, JsValue v);
JsValue          js_object_get(CTX *c, struct JsObject *o, struct JsString *key);
JsValue          js_object_get_with_ic(CTX *c, struct JsObject *o, struct JsString *key, struct JsPropIC *ic);
void             js_object_set_with_ic(CTX *c, struct JsObject *o, struct JsString *key, JsValue v, struct JsPropIC *ic);
JsValue          js_object_get_proto(CTX *c, JsValue v, struct JsString *key);
bool             js_object_has(struct JsObject *o, struct JsString *key);
bool             js_object_delete(struct JsObject *o, struct JsString *key);
int              js_shape_find_slot(struct JsShape *s, struct JsString *name);

// Array.
struct JsArray *js_array_new(CTX *c, uint32_t length);
JsValue         js_array_get(CTX *c, struct JsArray *a, int64_t i);
void            js_array_set(CTX *c, struct JsArray *a, int64_t i, JsValue v);
JsValue         js_array_get_v(CTX *c, struct JsArray *a, JsValue key);
void            js_array_set_v(CTX *c, struct JsArray *a, JsValue key, JsValue v);
void            js_array_set_length(CTX *c, struct JsArray *a, uint32_t len);
void            js_array_push(CTX *c, struct JsArray *a, JsValue v);
JsValue         js_array_pop(CTX *c, struct JsArray *a);

// Function.
struct JsFunction *js_func_new(CTX *c, struct Node *body, uint32_t np,
                               uint32_t nl, uint32_t nu, bool is_arrow,
                               const char *name);
struct JsCFunction *js_cfunc_new(CTX *c, const char *name, js_cfunc_ptr_t fn,
                                 uint32_t nparams);

// Property access on any value (handles primitives via wrappers per spec).
JsValue js_get_member(CTX *c, JsValue obj, struct JsString *name);
JsValue js_get_index(CTX *c, JsValue obj, JsValue key);
void    js_set_member(CTX *c, JsValue obj, struct JsString *name, JsValue v);
void    js_set_index(CTX *c, JsValue obj, JsValue key, JsValue v);

// Calls.
JsValue js_call(CTX *c, JsValue fn, JsValue thisv, JsValue *args, uint32_t argc);
JsValue js_construct(CTX *c, JsValue fn, JsValue *args, uint32_t argc);

// Direct call into a known JsFunction body — bypasses the type-tag check
// in js_call.  Defined in js_runtime.c.
JsValue js_call_func_direct(CTX *c, struct JsFunction *fn, JsValue thisv,
                             JsValue *args, uint32_t argc);

// Errors.
__attribute__((noreturn)) void js_throw(CTX *c, JsValue err);
__attribute__((noreturn)) void js_throw_type_error(CTX *c, const char *msg, ...);
__attribute__((noreturn)) void js_throw_range_error(CTX *c, const char *msg, ...);
__attribute__((noreturn)) void js_throw_syntax_error(CTX *c, const char *msg, ...);
__attribute__((noreturn)) void js_throw_reference_error(CTX *c, const char *msg, ...);
JsValue js_make_error(CTX *c, const char *kind, const char *msg);

// Conversions per ECMAScript spec.
JsValue          js_to_primitive(CTX *c, JsValue v, const char *hint);
JsValue          js_to_number(CTX *c, JsValue v);
double           js_to_double(CTX *c, JsValue v);
int32_t          js_to_int32(CTX *c, JsValue v);
uint32_t         js_to_uint32(CTX *c, JsValue v);
struct JsString *js_to_string(CTX *c, JsValue v);
struct JsObject *js_to_object(CTX *c, JsValue v);
const char      *js_typeof(JsValue v);
bool             js_strict_eq(JsValue a, JsValue b);
bool             js_loose_eq(CTX *c, JsValue a, JsValue b);
bool             js_lt(CTX *c, JsValue a, JsValue b, bool left_first);
int              js_lt_ts(CTX *c, JsValue a, JsValue b, bool left_first);
JsValue          js_add(CTX *c, JsValue a, JsValue b);
JsValue          js_sub(CTX *c, JsValue a, JsValue b);
JsValue          js_mul(CTX *c, JsValue a, JsValue b);
JsValue          js_div(CTX *c, JsValue a, JsValue b);
JsValue          js_mod(CTX *c, JsValue a, JsValue b);
JsValue          js_pow(CTX *c, JsValue a, JsValue b);
JsValue          js_neg(CTX *c, JsValue v);
JsValue          js_bnot(CTX *c, JsValue v);
JsValue          js_band(CTX *c, JsValue a, JsValue b);
JsValue          js_bor(CTX *c, JsValue a, JsValue b);
JsValue          js_bxor(CTX *c, JsValue a, JsValue b);
JsValue          js_shl(CTX *c, JsValue a, JsValue b);
JsValue          js_sar(CTX *c, JsValue a, JsValue b);
JsValue          js_shr(CTX *c, JsValue a, JsValue b);
JsValue          js_in(CTX *c, JsValue key, JsValue obj);
JsValue          js_instanceof(CTX *c, JsValue v, JsValue ctor);

// Print / debug.
void             js_print_value(CTX *c, FILE *fp, JsValue v);
void             js_console_log(CTX *c, JsValue *args, uint32_t argc);

// truthy
static inline bool jv_to_bool(JsValue v) {
    if (v == JV_UNDEFINED || v == JV_NULL || v == JV_FALSE) return false;
    if (JV_IS_SMI(v)) return JV_AS_SMI(v) != 0;
    if (JV_IS_FLONUM(v)) {
        double d = JV_AS_DBL(v);
        return d != 0.0 && !__builtin_isnan(d);
    }
    if (JV_IS_FLOAT_BOX(v)) {
        double d = JV_AS_DBL(v);
        return d != 0.0 && !__builtin_isnan(d);
    }
    if (JV_IS_STR(v)) return JV_AS_STR(v)->len != 0;
    return true; // any other heap object
}

// Setup.
CTX *js_create_context(void);
void js_init_globals(CTX *c);

// Regex (defined in js_regex.c, included by node.c).
JsValue js_regex_new(CTX *c, struct JsString *source, struct JsString *flags);
JsValue js_regex_test(CTX *c, JsValue rev, struct JsString *s);
JsValue js_regex_exec(CTX *c, JsValue rev, struct JsString *s);
struct JsRegex;
int     js_regex_search(struct JsRegex *re, struct JsString *s, int32_t from, int32_t *out_len);

// GC.
void js_gc_register(CTX *c, void *obj, uint8_t type);
void js_gc_register_size(CTX *c, void *obj, uint8_t type, size_t size);
void js_gc_collect(CTX *c);
void jstro_gc_safepoint(CTX *c);
void *js_gc_alloc(CTX *c, size_t size, uint8_t type);

// =====================================================================
// CLI option flags.
// =====================================================================
struct jstro_option {
    bool quiet;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;
    bool compile_first;
    bool aot_only;
    bool pg_mode;
    bool dump_ast;
    bool verbose;
    bool show_result;
    bool trace_calls;
};

extern struct jstro_option OPTION;

// Forward decl.
struct Node;

#define JSTRO_LIKELY(x)   __builtin_expect(!!(x), 1)
#define JSTRO_UNLIKELY(x) __builtin_expect(!!(x), 0)

#endif // JSTRO_CONTEXT_H
