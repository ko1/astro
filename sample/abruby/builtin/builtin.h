#ifndef ABRUBY_BUILTIN_H
#define ABRUBY_BUILTIN_H

#include "../node.h"
#include "../context.h"

// shorthand macros for inner CRuby values
#define RSTR(v) abruby_str_rstr(v)
#define RARY(v) (((struct abruby_array *)RTYPEDDATA_GET_DATA(v))->rb_ary)
#define RHSH(v) (((struct abruby_hash *)RTYPEDDATA_GET_DATA(v))->rb_hash)
#define RSYM(v) (((struct abruby_symbol *)RTYPEDDATA_GET_DATA(v))->rb_sym)

// Unwrap abruby symbol to inner CRuby symbol.
// Handles both static (immediate) symbols and wrapped (dynamic) symbols.
static inline VALUE
ab_sym_unwrap(VALUE v)
{
    if (RB_STATIC_SYM_P(v)) return v;
    return RSYM(v);
}

// Hash key normalisation: unwrap abruby String / Array wrappers to
// their inner CRuby objects so rb_hash_* lookups hit the stored keys.
// Immediates (Fixnum/Symbol/etc.) pass through unchanged.
static inline VALUE
ab_hash_key(VALUE v)
{
    if (RB_SPECIAL_CONST_P(v)) return v;
    if (RB_TYPE_P(v, T_DATA)) {
        const struct abruby_header *h = (const struct abruby_header *)RTYPEDDATA_GET_DATA(v);
        if (h->obj_type == ABRUBY_OBJ_STRING) return ((const struct abruby_string *)h)->rb_str;
        if (h->obj_type == ABRUBY_OBJ_ARRAY)  return ((const struct abruby_array *)h)->rb_ary;
        if (h->obj_type == ABRUBY_OBJ_SYMBOL) return ((const struct abruby_symbol *)h)->rb_sym;
    }
    return v;
}

// Inline equivalent of rb_ary_entry — avoids the PLT jump into libruby
// on hot Array#[] paths.  Mirrors CRuby's internal rb_ary_entry_internal.
static inline __attribute__((always_inline)) VALUE
ab_ary_entry(VALUE ary, long offset)
{
    long len = RARRAY_LEN(ary);
    const VALUE *ptr = RARRAY_CONST_PTR(ary);
    if (len == 0) return Qnil;
    if (offset < 0) {
        offset += len;
        if (offset < 0) return Qnil;
    }
    else if (len <= offset) {
        return Qnil;
    }
    return ptr[offset];
}

// shared helpers (defined in abruby.c)
RESULT abruby_call_method(CTX *c, VALUE recv, const struct abruby_method *method,
                          unsigned int argc, VALUE *argv);
VALUE ab_inspect_rstr(CTX *c, VALUE v);
RESULT abruby_require_file(CTX *c, VALUE rb_path);
RESULT abruby_eval_string(CTX *c, VALUE rb_code);
VALUE abruby_current_file(const CTX *c);
void abruby_class_add_cfunc(struct abruby_class *klass, ID name,
                            abruby_cfunc_t func, unsigned int params_cnt);

// Bignum/Float wrap helpers (defined in abruby.c)
VALUE abruby_bignum_new(CTX *c, VALUE rb_bignum);
VALUE abruby_float_new_wrap(CTX *c, VALUE rb_float);
VALUE abruby_range_new(CTX *c, VALUE begin, VALUE end, bool exclude_end);
VALUE abruby_regexp_new(CTX *c, VALUE rb_regexp);
VALUE abruby_rational_new(CTX *c, VALUE rb_rational);
VALUE abruby_complex_new(CTX *c, VALUE rb_complex);

/*
 * Numeric unwrap/wrap helpers.
 *
 * abruby Bignum/Float are wrapped in T_DATA with abruby_header.
 * Before calling CRuby APIs (rb_funcall, rb_big_*, etc.), extract the
 * inner CRuby value with UNWRAP.  Wrap the result back with WRAP.
 *
 * Fixnum/Symbol/true/false/nil are CRuby immediates and pass through as-is.
 */

// abruby integer → inner CRuby integer (Fixnum or T_BIGNUM)
static inline VALUE
AB_INT_UNWRAP(VALUE v)
{
    if (FIXNUM_P(v)) return v;
    return ((const struct abruby_bignum *)RTYPEDDATA_GET_DATA(v))->rb_bignum;
}

// abruby float → inner CRuby float
static inline VALUE
AB_FLOAT_UNWRAP(VALUE v)
{
    if (RB_FLONUM_P(v)) return v;
    return ((const struct abruby_float *)RTYPEDDATA_GET_DATA(v))->rb_float;
}

// Inline flonum decode (mirrors CRuby internal/numeric.h
// rb_float_flonum_value).  Avoids the external rb_float_value call that
// dominates mandelbrot's inner loop.
static inline __attribute__((always_inline)) double
ab_flonum_value(VALUE v)
{
    if (v != (VALUE)0x8000000000000002) {
        union { double d; VALUE v; } t;
        VALUE b63 = (v >> 63);
        t.v = ((2 - b63) | (v & ~(VALUE)0x03));
        // rotate right by 3 bits
        t.v = (t.v >> 3) | (t.v << 61);
        return t.d;
    }
    return 0.0;
}

// abruby float → C double, skipping rb_float_value for flonums.
static inline __attribute__((always_inline)) double
AB_FLOAT_TO_DOUBLE(VALUE v)
{
    if (RB_FLONUM_P(v)) return ab_flonum_value(v);
    return RFLOAT_VALUE(((const struct abruby_float *)RTYPEDDATA_GET_DATA(v))->rb_float);
}

// Inline flonum encode (mirrors CRuby internal/numeric.h
// rb_float_new_inline).  Returns either a flonum-tagged VALUE or 0 if
// the value is out of flonum range; callers must handle the fallback.
static inline __attribute__((always_inline)) VALUE
ab_flonum_encode(double d)
{
    union { double d; VALUE v; } t;
    int bits;
    t.d = d;
    bits = (int)((VALUE)(t.v >> 60) & 0x7);
    if (t.v != 0x3000000000000000 && !((bits - 3) & ~0x01)) {
        VALUE rot = (t.v << 3) | (t.v >> 61);
        return (rot & ~(VALUE)0x01) | 0x02;
    }
    if (t.v == (VALUE)0) return 0x8000000000000002;
    return 0;  // out-of-range: caller falls back to rb_float_new
}

// abruby numeric → inner CRuby numeric (for mixed-type operations)
static inline VALUE
AB_NUM_UNWRAP(VALUE v)
{
    if (FIXNUM_P(v)) return v;
    if (RB_FLONUM_P(v)) return v;
    if (RB_SPECIAL_CONST_P(v)) return v;
    const struct abruby_header *h = (const struct abruby_header *)RTYPEDDATA_GET_DATA(v);
    switch (h->obj_type) {
    case ABRUBY_OBJ_BIGNUM:   return ((const struct abruby_bignum *)h)->rb_bignum;
    case ABRUBY_OBJ_FLOAT:    return ((const struct abruby_float *)h)->rb_float;
    case ABRUBY_OBJ_RATIONAL: return ((const struct abruby_rational *)h)->rb_rational;
    case ABRUBY_OBJ_COMPLEX:  return ((const struct abruby_complex *)h)->rb_complex;
    default: return v;
    }
}

// CRuby numeric result → abruby value
// Fixnum passes through. T_BIGNUM wraps in abruby_bignum. Float wraps in abruby_float.
static inline VALUE
AB_NUM_WRAP(CTX *c, VALUE v)
{
    if (FIXNUM_P(v)) return v;
    if (RB_TYPE_P(v, T_BIGNUM)) return abruby_bignum_new(c, v);
    if (RB_FLOAT_TYPE_P(v)) return abruby_float_new_wrap(c, v);
    if (RB_TYPE_P(v, T_RATIONAL)) return abruby_rational_new(c, v);
    if (RB_TYPE_P(v, T_COMPLEX)) return abruby_complex_new(c, v);
    // true/false/nil (e.g. from ==) pass through
    return v;
}

// Template class pointers — Init_abruby_* functions register methods on these.
// Runtime code must NOT use these directly; use c->abm->xxx_class instead.
extern struct abruby_class *const ab_tmpl_object_class;
extern struct abruby_class *const ab_tmpl_integer_class;
extern struct abruby_class *const ab_tmpl_string_class;
extern struct abruby_class *const ab_tmpl_symbol_class;
extern struct abruby_class *const ab_tmpl_true_class;
extern struct abruby_class *const ab_tmpl_false_class;
extern struct abruby_class *const ab_tmpl_nil_class;
extern struct abruby_class *const ab_tmpl_float_class;
extern struct abruby_class *const ab_tmpl_array_class;
extern struct abruby_class *const ab_tmpl_hash_class;
extern struct abruby_class *const ab_tmpl_range_class;
extern struct abruby_class *const ab_tmpl_regexp_class;
extern struct abruby_class *const ab_tmpl_kernel_module;
extern struct abruby_class *const ab_tmpl_rational_class;
extern struct abruby_class *const ab_tmpl_complex_class;
extern struct abruby_class *const ab_tmpl_module_class;
extern struct abruby_class *const ab_tmpl_class_class;
extern struct abruby_class *const ab_tmpl_runtime_error_class;

// Init functions
void Init_abruby_kernel(void);
void Init_abruby_object(void);
void Init_abruby_class(void);
void Init_abruby_integer(void);
void Init_abruby_float(void);
void Init_abruby_string(void);
void Init_abruby_symbol(void);
void Init_abruby_array(void);
void Init_abruby_hash(void);
void Init_abruby_range(void);
void Init_abruby_regexp(void);
void Init_abruby_rational(void);
void Init_abruby_complex(void);
void Init_abruby_true(void);
void Init_abruby_false(void);
void Init_abruby_nil(void);
void Init_abruby_exception(void);

#endif
