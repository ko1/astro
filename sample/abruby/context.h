#ifndef ABRUBY_CONTEXT_H
#define ABRUBY_CONTEXT_H 1

#include <ruby.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include "ab_id_table.h"

// VALUE is defined by ruby.h

#ifndef ABRUBY_DEBUG
#define ABRUBY_DEBUG 0
#endif

#if ABRUBY_DEBUG
#define ABRUBY_ASSERT(expr) do { \
    if (!(expr)) { \
        rb_bug("ABRUBY_ASSERT failed: %s (%s:%d)", #expr, __FILE__, __LINE__); \
    } \
} while (0)
#else
#define ABRUBY_ASSERT(expr) ((void)0)
#endif


struct abruby_option {
    bool no_compiled_code;
    bool compiled_only;  // set dispatcher to NULL in ALLOC (crash if uncompiled node is called)
    bool record_all;
    bool quiet;
    bool verbose;
};

extern struct abruby_option OPTION;

// RESULT: two-value return type for non-local exit support.
// Fits in two registers (rax + rdx), no memory access needed.
// Partial evaluation eliminates state checks via constant propagation.
//
// State values are bit flags so that "catch state X and demote to NORMAL" at a
// boundary (method return catch, loop break catch) can be done with a single
// AND instruction instead of a branching check:
//
//   r.state &= ~RESULT_RETURN;  // at method boundary: RETURN -> NORMAL, others unchanged
//   r.state &= ~RESULT_BREAK;   // at while boundary:  BREAK  -> NORMAL, others unchanged
//
// NORMAL is 0 so the common "is state normal?" test is still a plain zero check.

enum result_state {
    RESULT_NORMAL = 0,
    RESULT_RETURN = 1,  // bit 0
    RESULT_RAISE  = 2,  // bit 1
    RESULT_BREAK  = 4,  // bit 2
};

typedef struct {
    VALUE value;
    enum result_state state;
} RESULT;

#define RESULT_OK(v) ((RESULT){(v), RESULT_NORMAL})

// abruby class system

typedef struct CTX_struct CTX;
typedef RESULT (*abruby_cfunc_t)(CTX *c, VALUE self, unsigned int argc, VALUE *argv);

enum abruby_method_type {
    ABRUBY_METHOD_AST,
    ABRUBY_METHOD_CFUNC,
    ABRUBY_METHOD_IVAR_GETTER,  // body was { @name } — inlined in dispatch hot path
    ABRUBY_METHOD_IVAR_SETTER,  // body was { @name = arg } — inlined
};

struct abruby_method {
    ID name;
    enum abruby_method_type type;
    // Class where this method was defined (for super chain resolution).
    // Set by abruby_class_add_method. Preserved across per-instance clones so
    // that super dispatch works correctly without storing klass in every frame.
    const struct abruby_class *defining_class;
    union {
        struct {
            struct Node *body;
            unsigned int params_cnt;
            unsigned int locals_cnt;
            const char *source_file; // file where method was defined
        } ast;
        struct {
            abruby_cfunc_t func;
            unsigned int params_cnt;
        } cfunc;
        struct {
            ID ivar_name;  // e.g. @x — looked up in receiver's klass->ivar_shape at call time
        } ivar_accessor;
    } u;
};

// (ABRUBY_METHOD_CAPA removed: methods are now in ab_id_table)

/*
 * abruby VALUE invariant:
 *
 * Every abruby VALUE must be one of:
 *   1. CRuby immediate: Fixnum, Symbol, true, false, nil
 *   2. T_DATA (abruby_data_type) with abruby_header at offset 0
 *
 * Raw CRuby heap types (T_BIGNUM, T_FLOAT, T_STRING, etc.) must NOT be
 * used directly. Bignum is wrapped in abruby_bignum, Float in abruby_float.
 * This allows AB_CLASS_OF() to resolve the class by checking immediates
 * first, then reading klass from the T_DATA header.
 */

// Object type tag — used by GC mark to determine marking strategy
// without comparing class pointers (which are per-instance).
enum abruby_obj_type {
    ABRUBY_OBJ_GENERIC,    // abruby_object (ivars)
    ABRUBY_OBJ_STRING,
    ABRUBY_OBJ_ARRAY,
    ABRUBY_OBJ_HASH,
    ABRUBY_OBJ_RANGE,
    ABRUBY_OBJ_REGEXP,
    ABRUBY_OBJ_BIGNUM,
    ABRUBY_OBJ_FLOAT,
    ABRUBY_OBJ_RATIONAL,
    ABRUBY_OBJ_COMPLEX,
    ABRUBY_OBJ_CLASS,
    ABRUBY_OBJ_MODULE,
    ABRUBY_OBJ_EXCEPTION,
};

// Common layout: ALL abruby T_DATA objects have klass at offset 0
struct abruby_header {
    struct abruby_class *klass;
};

struct abruby_class {
    struct abruby_class *klass;  // offset 0: metaclass (per-instance class_class or module_class)
    enum abruby_obj_type obj_type; // what kind of instances this class creates
    ID name;
    struct abruby_class *super;
    struct ab_id_table methods;    // key=method_name, val=(VALUE)(struct abruby_method*)
    VALUE rb_wrapper;
    struct ab_id_table constants;  // key=const_name, val=const_value
    // Shape: ivar_name -> slot index in abruby_object::ivars (as LONG2FIX(slot)).
    // First seen names are assigned sequential slots. The IC caches (klass, slot)
    // and reads/writes obj->ivars[slot] directly with no per-object table lookup.
    struct ab_id_table ivar_shape;
};

// Direct-indexed instance variables.
// Slots are assigned by the class shape (see abruby_class::ivar_shape) and
// accessed as obj->ivars[slot] with no per-object hash table.
#define ABRUBY_OBJECT_IVAR_CAPA 8

struct abruby_object {
    struct abruby_class *klass;                    // offset 0
    uint32_t ivar_cnt;                              // number of live ivars (for mark)
    VALUE ivars[ABRUBY_OBJECT_IVAR_CAPA];           // Qnil-initialized
};

// Bignum/Float are wrapped in T_DATA with abruby_header, not raw CRuby
// T_BIGNUM/T_FLOAT.  This ensures uniform class resolution via AB_CLASS_OF().

struct abruby_bignum {
    struct abruby_class *klass;  // offset 0: per-instance integer_class
    VALUE rb_bignum;             // inner CRuby Bignum (T_BIGNUM)
};

struct abruby_float {
    struct abruby_class *klass;  // offset 0: per-instance float_class
    VALUE rb_float;              // inner CRuby Float (Flonum or T_FLOAT)
};

struct abruby_string {
    struct abruby_class *klass;  // offset 0: per-instance string_class
    VALUE rb_str;                // inner CRuby String
};

struct abruby_array {
    struct abruby_class *klass;  // offset 0
    VALUE rb_ary;
};

struct abruby_hash {
    struct abruby_class *klass;  // offset 0
    VALUE rb_hash;
};

struct abruby_range {
    struct abruby_class *klass;  // offset 0
    VALUE begin;
    VALUE end;
    bool exclude_end;            // true for ..., false for ..
};

struct abruby_regexp {
    struct abruby_class *klass;  // offset 0
    VALUE rb_regexp;             // inner CRuby Regexp
};

struct abruby_rational {
    struct abruby_class *klass;  // offset 0: per-instance rational_class
    VALUE rb_rational;           // inner CRuby Rational
};

struct abruby_complex {
    struct abruby_class *klass;  // offset 0: per-instance complex_class
    VALUE rb_complex;            // inner CRuby Complex
};

// Built-in abruby classes are per-instance (stored in abruby_machine).
// Access via c->abm->integer_class etc. at runtime.

extern const rb_data_type_t abruby_data_type;
extern const rb_data_type_t abruby_node_type;

static inline void
ab_verify(VALUE obj)
{
    if (ABRUBY_DEBUG) {
        // immediate values are always valid
        if (FIXNUM_P(obj) || SYMBOL_P(obj) || RB_FLONUM_P(obj) ||
            obj == Qtrue || obj == Qfalse || obj == Qnil) {
            return;
        }
        // Everything else must be T_DATA with abruby_data_type and non-NULL klass.
        // Raw CRuby T_BIGNUM, T_FLOAT, T_STRING etc. are NOT valid abruby values.
        if (!RB_TYPE_P(obj, T_DATA)) {
            rb_bug("ab_verify: expected immediate or T_DATA, got type %d (%s:%d)", rb_type(obj), __FILE__, __LINE__);
        }
        if (!RTYPEDDATA_P(obj)) {
            rb_bug("ab_verify: T_DATA is not TypedData (%s:%d)", __FILE__, __LINE__);
        }
        if (RTYPEDDATA_TYPE(obj) != &abruby_data_type) {
            rb_bug("ab_verify: wrong data type '%s', expected abruby_data_type (%s:%d)",
                   RTYPEDDATA_TYPE(obj)->wrap_struct_name, __FILE__, __LINE__);
        }
        const struct abruby_class *klass =
            ((const struct abruby_header *)RTYPEDDATA_GET_DATA(obj))->klass;
        if (klass == NULL) {
            rb_bug("ab_verify: klass is NULL (%s:%d)", __FILE__, __LINE__);
        }
    }
}

// Type check by obj_type (instance-independent, works with per-instance classes).
// Equivalent to AB_CLASS_OF(obj)->obj_type == type, but doesn't need a CTX.
// Hardcodes the immediate→obj_type mapping (it's fixed across all machines).
static inline bool
ab_obj_type_p(VALUE obj, enum abruby_obj_type type)
{
    if (FIXNUM_P(obj))           return type == ABRUBY_OBJ_BIGNUM;  // integer_class.obj_type
    if (RB_FLONUM_P(obj))        return type == ABRUBY_OBJ_FLOAT;
    if (RB_SPECIAL_CONST_P(obj)) return type == ABRUBY_OBJ_GENERIC; // symbol/true/false/nil
    const struct abruby_header *h = (const struct abruby_header *)RTYPEDDATA_GET_DATA(obj);
    return h->klass && h->klass->obj_type == type;
}

// AB_CLASS_OF / AB_CLASS_OF_IMM are defined below after struct CTX_struct / abruby_machine.
// AB_CLASS_P removed: use ab_obj_type_p() or AB_CLASS_OF(c, obj) == c->abm->xxx_class

// Global variables are stored in an ab_id_table in abruby_machine.
// (struct abruby_gvar_table removed)

// call frame for backtrace support (32 bytes)
// method != NULL: normal method frame
// method == NULL: <main>/<top (required)>
struct abruby_frame {
    struct abruby_frame *prev;
    const struct abruby_method *method;  // super walks from method->defining_class->super
    union {
        const struct Node *caller_node;  // method frame: call site in the caller (set at push time)
        const char *source_file;         // <main>/<top>: set at push time
    };
};

// Cached interned IDs for operator methods and common names
struct abruby_id_cache {
    ID op_plus, op_minus, op_mul, op_div;
    ID op_lt, op_le, op_gt, op_ge;
    ID op_eq, op_mod;
    ID method_missing;
};

struct abruby_machine;  // forward declaration

#define ABRUBY_STACK_SIZE 10000

struct CTX_struct {
    struct abruby_machine *abm;          // per-instance machine (owner)
    VALUE stack[ABRUBY_STACK_SIZE];      // VALUE stack (locals + args)
    VALUE *fp;                           // frame pointer into stack
    VALUE self;
    struct abruby_class *current_class;  // set during class body eval
    struct abruby_frame *current_frame;  // head of call frame linked list
    const struct abruby_id_cache *ids;   // cached rb_intern results
};

// Fiber: owns a CTX (execution context with its own stack).
// Currently only main fiber exists; future: multiple fibers with switching.
struct abruby_fiber {
    CTX ctx;                             // execution context (stack included)
};

struct abruby_machine {
    uint32_t method_serial;              // method version (for inline cache invalidation)
    struct abruby_fiber *current_fiber;  // currently running fiber
    struct abruby_class main_class_body; // per-instance Object subclass (temporary)
    struct ab_id_table gvars;            // global variables
    struct abruby_id_cache id_cache;     // cached rb_intern results
    VALUE rb_self;                       // Ruby-level AbRuby instance
    VALUE current_file;                  // current file path
    VALUE loaded_files;                  // loaded file paths
    // Per-instance class pointers (each instance has its own class hierarchy)
    struct abruby_class *object_class;
    struct abruby_class *integer_class;
    struct abruby_class *float_class;
    struct abruby_class *string_class;
    struct abruby_class *symbol_class;
    struct abruby_class *array_class;
    struct abruby_class *hash_class;
    struct abruby_class *range_class;
    struct abruby_class *regexp_class;
    struct abruby_class *rational_class;
    struct abruby_class *complex_class;
    struct abruby_class *true_class;
    struct abruby_class *false_class;
    struct abruby_class *nil_class;
    struct abruby_class *kernel_module;
    struct abruby_class *module_class;
    struct abruby_class *class_class;
    struct abruby_class *runtime_error_class;
};

// exception object
struct abruby_exception {
    struct abruby_class *klass;  // offset 0: per-instance runtime_error_class
    VALUE message;               // abruby VALUE (usually abruby string)
    VALUE backtrace;             // Ruby Array of Strings, or Qnil
};

// inline method cache per call site
struct method_cache {
    const struct abruby_class *klass;    // read-only after fill
    const struct abruby_method *method;  // read-only after fill
    uint32_t serial;
    uint32_t ivar_slot;                 // for IVAR_GETTER/SETTER: cached slot in receiver's shape
    struct Node *body;                  // cached method->u.ast.body (NULL for CFUNC)
    RESULT (*dispatcher)(struct CTX_struct *, struct Node *); // cached body->head.dispatcher
};

// inline ivar cache per node_ivar_get / node_ivar_set site.
// Guard: klass matches obj's klass. If so, slot is authoritative (the class
// shape guarantees every instance of klass has this ivar at this slot).
struct ivar_cache {
    const struct abruby_class *klass;  // NULL means not yet filled
    unsigned int slot;                 // direct index into obj->ivars
};


#define LIKELY(expr) __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

// Class resolution helpers — defined after abruby_machine so that c->abm->xxx_class
// can be dereferenced inline.

static __attribute__((unused)) struct abruby_class *
AB_CLASS_OF_IMM(const CTX *c, VALUE obj)
{
    if (FIXNUM_P(obj))       return c->abm->integer_class;
    else if (RB_FLONUM_P(obj)) return c->abm->float_class;
    else if (SYMBOL_P(obj))  return c->abm->symbol_class;
    else if (obj == Qtrue)   return c->abm->true_class;
    else if (obj == Qfalse)  return c->abm->false_class;
    else                     return c->abm->nil_class;
}

/*
 * AB_CLASS_OF(c, obj): resolve the abruby class of a VALUE.
 *
 * Heap objects (T_DATA) are checked first since they are the most common
 * receivers in node_method_call — Fixnum arithmetic goes through
 * type-specialized nodes (fixnum_plus, etc.) and rarely reaches here.
 * The abruby VALUE invariant guarantees obj is either a CRuby immediate
 * or T_DATA with abruby_header at offset 0.
 */
static inline struct abruby_class *
AB_CLASS_OF(const CTX *c, VALUE obj)
{
    ab_verify(obj);

    if (RB_LIKELY(!RB_SPECIAL_CONST_P(obj))) {
        return ((struct abruby_header *)RTYPEDDATA_GET_DATA(obj))->klass;
    }
    else {
        return AB_CLASS_OF_IMM(c, obj);
    }
}

#endif
