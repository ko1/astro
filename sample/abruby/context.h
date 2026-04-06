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
    bool record_all;
    bool quiet;
};

extern struct abruby_option OPTION;

// abruby class system

typedef struct CTX_struct CTX;
typedef VALUE (*abruby_cfunc_t)(CTX *c, VALUE self, unsigned int argc, VALUE *argv);

enum abruby_method_type {
    ABRUBY_METHOD_AST,
    ABRUBY_METHOD_CFUNC,
};

struct abruby_method {
    const char *name;
    enum abruby_method_type type;
    union {
        struct {
            struct Node *body;
            unsigned int params_cnt;
            unsigned int locals_cnt;
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
 * 全ての abruby VALUE は以下のいずれかでなければならない:
 *   1. CRuby immediate: Fixnum, Symbol, true, false, nil
 *   2. T_DATA (abruby_data_type) with abruby_header at offset 0
 *
 * CRuby の T_BIGNUM, T_FLOAT, T_STRING 等を直接使ってはならない。
 * Bignum は abruby_bignum、Float は abruby_float で wrap する。
 * これにより AB_CLASS_OF() は immediate チェック後、T_DATA の先頭から
 * klass を読むだけでクラスを特定できる。
 */

// Common layout: ALL abruby T_DATA objects have klass at offset 0
struct abruby_header {
    struct abruby_class *klass;
};

#define ABRUBY_CONST_CAPA 64

struct abruby_class {
    struct abruby_class *klass;  // offset 0: always ab_class_class
    const char *name;
    struct abruby_class *super;
    struct abruby_method methods[ABRUBY_METHOD_CAPA];
    unsigned int method_cnt;
    VALUE rb_wrapper;
    // constants
    struct {
        const char *name;
        VALUE value;
    } constants[ABRUBY_CONST_CAPA];
    unsigned int const_cnt;
};

#define ABRUBY_IVAR_MAX 32

struct abruby_object {
    struct abruby_class *klass;  // offset 0
    unsigned int ivar_cnt;
    struct {
        const char *name;
        VALUE value;
    } ivars[ABRUBY_IVAR_MAX];
};

// Bignum/Float は CRuby の T_BIGNUM/T_FLOAT を直接使わず、
// abruby_header 付きの T_DATA で wrap する。
// これにより AB_CLASS_OF() の統一的なクラス解決が可能になる。

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
    int exclude_end;             // 1 for ..., 0 for ..
};

struct abruby_regexp {
    struct abruby_class *klass;  // offset 0
    VALUE rb_regexp;             // inner CRuby Regexp
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
 * AB_CLASS_OF(obj): abruby VALUE からクラスを取得する。
 *
 * immediate の場合は固定クラスポインタを返す。
 * T_DATA の場合は先頭の abruby_header.klass を直接読む。
 * abruby VALUE invariant により、この2パターンで全てカバーできる。
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

struct CTX_struct {
    VALUE *env;
    VALUE *fp;
    VALUE self;
    struct abruby_class *current_class; // set during class body eval
    struct abruby_class *main_class;    // per-instance, inherits from Object
};

#define LIKELY(expr) __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

#endif
