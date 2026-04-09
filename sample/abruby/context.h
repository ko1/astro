#ifndef ABRUBY_CONTEXT_H
#define ABRUBY_CONTEXT_H 1

#include <ruby.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

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

enum result_state {
    RESULT_NORMAL,
    RESULT_RETURN,
    RESULT_RAISE,
    RESULT_BREAK,
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
};

struct abruby_method {
    ID name;
    enum abruby_method_type type;
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
    } u;
};

#define ABRUBY_METHOD_CAPA 64

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

// Common layout: ALL abruby T_DATA objects have klass at offset 0
struct abruby_header {
    struct abruby_class *klass;
};

#define ABRUBY_CONST_CAPA 64

struct abruby_class {
    struct abruby_class *klass;  // offset 0: always ab_class_class
    ID name;
    struct abruby_class *super;
    struct abruby_method methods[ABRUBY_METHOD_CAPA];
    unsigned int method_cnt;
    VALUE rb_wrapper;
    // constants
    struct {
        ID name;
        VALUE value;
    } constants[ABRUBY_CONST_CAPA];
    unsigned int const_cnt;
};

#define ABRUBY_IVAR_MAX 32

struct abruby_object {
    struct abruby_class *klass;  // offset 0
    unsigned int ivar_cnt;
    struct {
        ID name;
        VALUE value;
    } ivars[ABRUBY_IVAR_MAX];
};

// Bignum/Float are wrapped in T_DATA with abruby_header, not raw CRuby
// T_BIGNUM/T_FLOAT.  This ensures uniform class resolution via AB_CLASS_OF().

struct abruby_bignum {
    struct abruby_class *klass;  // offset 0: always ab_integer_class
    VALUE rb_bignum;             // inner CRuby Bignum (T_BIGNUM)
};

struct abruby_float {
    struct abruby_class *klass;  // offset 0: always ab_float_class
    VALUE rb_float;              // inner CRuby Float (Flonum or T_FLOAT)
};

struct abruby_string {
    struct abruby_class *klass;  // offset 0: always ab_string_class
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
    struct abruby_class *klass;  // offset 0: always ab_rational_class
    VALUE rb_rational;           // inner CRuby Rational
};

struct abruby_complex {
    struct abruby_class *klass;  // offset 0: always ab_complex_class
    VALUE rb_complex;            // inner CRuby Complex
};

// built-in abruby classes (defined in abruby.c)
extern struct abruby_class *ab_object_class;
extern struct abruby_class *ab_integer_class;
extern struct abruby_class *ab_string_class;
extern struct abruby_class *ab_symbol_class;
extern struct abruby_class *ab_true_class;
extern struct abruby_class *ab_false_class;
extern struct abruby_class *ab_nil_class;
extern struct abruby_class *ab_float_class;
extern struct abruby_class *ab_array_class;
extern struct abruby_class *ab_hash_class;
extern struct abruby_class *ab_range_class;
extern struct abruby_class *ab_regexp_class;
extern struct abruby_class *ab_kernel_module;
extern struct abruby_class *ab_rational_class;
extern struct abruby_class *ab_complex_class;
extern struct abruby_class *ab_module_class;
extern struct abruby_class *ab_class_class;

extern const rb_data_type_t abruby_data_type;
extern const rb_data_type_t abruby_node_type;

static inline void
ab_verify(VALUE obj)
{
    if (ABRUBY_DEBUG) {
        // immediate values are always valid
        if (FIXNUM_P(obj) || SYMBOL_P(obj) ||
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
        struct abruby_class *klass = ((struct abruby_header *)RTYPEDDATA_GET_DATA(obj))->klass;
        if (klass == NULL) {
            rb_bug("ab_verify: klass is NULL (%s:%d)", __FILE__, __LINE__);
        }
    }
}

/*
 * AB_CLASS_OF(obj): resolve the abruby class of a VALUE.
 *
 * Immediates return a fixed class pointer.
 * T_DATA objects read klass directly from the abruby_header at offset 0.
 * The abruby VALUE invariant guarantees these two cases are exhaustive.
 */
static inline struct abruby_class *
AB_CLASS_OF(VALUE obj)
{
    ab_verify(obj);
    if (FIXNUM_P(obj))  return ab_integer_class;
    if (SYMBOL_P(obj))  return ab_symbol_class;
    if (obj == Qtrue)   return ab_true_class;
    if (obj == Qfalse)  return ab_false_class;
    if (obj == Qnil)    return ab_nil_class;
    return ((struct abruby_header *)RTYPEDDATA_GET_DATA(obj))->klass;
}

#define AB_CLASS_P(obj, klass) (AB_CLASS_OF(obj) == (klass))

#define ABRUBY_GVAR_MAX 64

struct abruby_gvar_table {
    unsigned int cnt;
    struct {
        ID name;
        VALUE value;
    } entries[ABRUBY_GVAR_MAX];
};

// call frame for backtrace support (24 bytes)
// method != NULL: normal method frame, node = call site node
// method == NULL: <main>/<top (required)>, source_file = file name
struct abruby_frame {
    struct abruby_frame *prev;
    struct abruby_method *method;
    struct abruby_class *klass;   // receiver's class at call time (for super)
    union {
        struct Node *node;        // method frame: updated by child push
        const char *source_file;  // <main>/<top>: set at push time
    };
};

struct CTX_struct {
    VALUE *env;
    VALUE *fp;
    VALUE self;
    struct abruby_class *current_class; // set during class body eval
    struct abruby_class *main_class;    // per-instance, inherits from Object
    struct abruby_gvar_table *gvars;    // global variables
    struct abruby_frame *current_frame; // head of call frame linked list
};

// exception object
struct abruby_exception {
    struct abruby_class *klass;  // offset 0: ab_runtime_error_class
    VALUE message;               // abruby VALUE (usually abruby string)
    VALUE backtrace;             // Ruby Array of Strings, or Qnil
};

extern struct abruby_class *ab_runtime_error_class;

#define LIKELY(expr) __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

#endif
