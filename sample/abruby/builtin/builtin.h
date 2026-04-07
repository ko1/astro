#ifndef ABRUBY_BUILTIN_H
#define ABRUBY_BUILTIN_H

#include "../node.h"
#include "../context.h"

// shorthand macros for inner CRuby values
#define RSTR(v) abruby_str_rstr(v)
#define RARY(v) (((struct abruby_array *)RTYPEDDATA_GET_DATA(v))->rb_ary)
#define RHSH(v) (((struct abruby_hash *)RTYPEDDATA_GET_DATA(v))->rb_hash)

// shared helpers (defined in abruby.c)
RESULT abruby_call_method(CTX *c, VALUE recv, struct abruby_method *method,
                          unsigned int argc, VALUE *argv);
VALUE ab_inspect_rstr(CTX *c, VALUE v);
RESULT abruby_require_file(CTX *c, VALUE rb_path);
RESULT abruby_eval_string(CTX *c, VALUE rb_code);
VALUE abruby_current_file(CTX *c);
void abruby_class_add_cfunc(struct abruby_class *klass, const char *name,
                            abruby_cfunc_t func, unsigned int params_cnt);

// Bignum/Float wrap helpers (defined in abruby.c)
VALUE abruby_bignum_new(VALUE rb_bignum);
VALUE abruby_float_new_wrap(VALUE rb_float);
VALUE abruby_range_new(VALUE begin, VALUE end, bool exclude_end);
VALUE abruby_regexp_new(VALUE rb_regexp);
VALUE abruby_rational_new(VALUE rb_rational);
VALUE abruby_complex_new(VALUE rb_complex);

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
    return ((struct abruby_bignum *)RTYPEDDATA_GET_DATA(v))->rb_bignum;
}

// abruby float → inner CRuby float
static inline VALUE
AB_FLOAT_UNWRAP(VALUE v)
{
    return ((struct abruby_float *)RTYPEDDATA_GET_DATA(v))->rb_float;
}

// abruby numeric → inner CRuby numeric (for mixed-type operations)
static inline VALUE
AB_NUM_UNWRAP(VALUE v)
{
    if (FIXNUM_P(v)) return v;
    struct abruby_header *h = (struct abruby_header *)RTYPEDDATA_GET_DATA(v);
    if (h->klass == ab_integer_class)
        return ((struct abruby_bignum *)h)->rb_bignum;
    if (h->klass == ab_float_class)
        return ((struct abruby_float *)h)->rb_float;
    if (h->klass == ab_rational_class)
        return ((struct abruby_rational *)h)->rb_rational;
    if (h->klass == ab_complex_class)
        return ((struct abruby_complex *)h)->rb_complex;
    return v;
}

// CRuby numeric result → abruby value
// Fixnum passes through. T_BIGNUM wraps in abruby_bignum. Float wraps in abruby_float.
static inline VALUE
AB_NUM_WRAP(VALUE v)
{
    if (FIXNUM_P(v)) return v;
    if (RB_TYPE_P(v, T_BIGNUM)) return abruby_bignum_new(v);
    if (RB_FLOAT_TYPE_P(v)) return abruby_float_new_wrap(v);
    if (RB_TYPE_P(v, T_RATIONAL)) return abruby_rational_new(v);
    if (RB_TYPE_P(v, T_COMPLEX)) return abruby_complex_new(v);
    // true/false/nil (e.g. from ==) pass through
    return v;
}

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
