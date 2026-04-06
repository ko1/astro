#ifndef ABRUBY_BUILTIN_H
#define ABRUBY_BUILTIN_H

#include "../node.h"
#include "../context.h"

// shorthand macros for inner CRuby values
#define RSTR(v) abruby_str_rstr(v)
#define RARY(v) (((struct abruby_array *)RTYPEDDATA_GET_DATA(v))->rb_ary)
#define RHSH(v) (((struct abruby_hash *)RTYPEDDATA_GET_DATA(v))->rb_hash)

// shared helpers (defined in abruby.c)
VALUE ab_inspect_rstr(CTX *c, VALUE v);
void abruby_class_add_cfunc(struct abruby_class *klass, const char *name,
                            abruby_cfunc_t func, unsigned int params_cnt);

// Constant registration (declared in node.h, defined in abruby.c)

// helpers (defined in abruby.c)
VALUE abruby_range_new(VALUE begin, VALUE end, int exclude_end);
VALUE abruby_regexp_new(VALUE rb_regexp);

// Init functions
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
void Init_abruby_true(void);
void Init_abruby_false(void);
void Init_abruby_nil(void);

#endif
