/* koruby's ruby.h — the CRuby C API as seen by a C extension compiled for
 * koruby (docs/c_ext_api_design.md §9).  Not CRuby's header: a VALUE here is
 * an immediate (same bits as koruby's korb_value) or a HANDLE — the address of
 * a cell the GC keeps pointing at the (moving) object.  Everything that looks
 * inside an object is a function call into koruby (builtins/cext.c). */
#ifndef KORUBY_RUBY_H
#define KORUBY_RUBY_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KORUBY 1
#define RUBY                  /* ruby/defines.h: extensions test it to rename clashing symbols */
#define HAVE_PROTOTYPES 1
#define HAVE_STDARG_PROTOTYPES 1
#define _(args) args          /* ruby/backward/2/stdarg.h */
#define ANYARGS ...
#include <inttypes.h>
#define PRIdVALUE PRIdPTR
#define PRIiVALUE PRIiPTR
#define PRIoVALUE PRIoPTR
#define PRIuVALUE PRIuPTR
#define PRIxVALUE PRIxPTR
#define PRIXVALUE PRIXPTR
#define RUBY_API_VERSION_MAJOR 4
#define RUBY_API_VERSION_MINOR 1
#define RUBY_API_VERSION_CODE (RUBY_API_VERSION_MAJOR * 10000 + RUBY_API_VERSION_MINOR * 100)
#define HAVE_RB_EXT_RESOLVE_SYMBOL 1
#define HAVE_RB_EXT_RACTOR_SAFE 1

typedef uintptr_t VALUE;
typedef intptr_t  SIGNED_VALUE;
typedef uintptr_t ID;

#define RUBY_EXTERN extern
#define RUBY_FUNC_EXPORTED __attribute__((visibility("default")))
#define NORETURN(x) __attribute__((noreturn)) x
#define DEPRECATED(x) x
#define RUBY_ATTR_NORETURN __attribute__((noreturn))
#define PRINTF_ARGS(decl, a, b) decl __attribute__((format(printf, a, b)))
#define RB_LIKELY(x)   __builtin_expect(!!(x), 1)
#define RB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#ifndef LIKELY
#  define LIKELY(x)   RB_LIKELY(x)
#  define UNLIKELY(x) RB_UNLIKELY(x)
#endif
#define UNREACHABLE __builtin_unreachable()
#define UNREACHABLE_RETURN(v) __builtin_unreachable()
#define RB_GNUC_EXTENSION __extension__
#define RBIMPL_WARNING_PUSH()  _Pragma("GCC diagnostic push")
#define RBIMPL_WARNING_POP()   _Pragma("GCC diagnostic pop")
#define RBIMPL_WARNING_IGNORED(flag)
#define RBIMPL_HAS_WARNING(flag) 0
#define RB_GC_GUARD(v) (*__extension__({ volatile VALUE *rb_gc_guarded_ptr = &(v); rb_gc_guarded_ptr; }))
#define RBIMPL_ATTR_MAYBE_UNUSED() __attribute__((unused))
#define MAYBE_UNUSED(x) __attribute__((unused)) x
#define RUBY_ALIGNOF _Alignof
#define ALWAYS_INLINE(x) __attribute__((always_inline)) x
#define NOINLINE(x) __attribute__((noinline)) x

/* ---- immediates: koruby's tagging (context.h) --------------------------- */
#define Qnil    ((VALUE)0)
#define Qfalse  ((VALUE)0x04)
#define Qtrue   ((VALUE)0x14)
#define Qundef  ((VALUE)0x24)
#define RUBY_Qnil   Qnil
#define RUBY_Qfalse Qfalse
#define RUBY_Qtrue  Qtrue
#define RUBY_Qundef Qundef

#define RB_NIL_P(v)          ((VALUE)(v) == Qnil)
#define NIL_P(v)             RB_NIL_P(v)
#define RB_TEST(v)           (((VALUE)(v) & ~(VALUE)4) != 0)
#define RTEST(v)             RB_TEST(v)
#define RB_UNDEF_P(v)        ((VALUE)(v) == Qundef)
#define UNDEF_P(v)           RB_UNDEF_P(v)
#define RB_FIXNUM_P(v)       (((VALUE)(v) & 1) != 0)
#define FIXNUM_P(v)          RB_FIXNUM_P(v)
#define RB_FLONUM_P(v)       (((VALUE)(v) & 3) == 2)
#define FLONUM_P(v)          RB_FLONUM_P(v)
#define RB_STATIC_SYM_P(v)   (((VALUE)(v) & 0xF) == 0xC)
#define STATIC_SYM_P(v)      RB_STATIC_SYM_P(v)
#define RB_SPECIAL_CONST_P(v) (((VALUE)(v) & 7) != 0 || (VALUE)(v) == 0)
#define SPECIAL_CONST_P(v)   RB_SPECIAL_CONST_P(v)
#define IMMEDIATE_P(v)       (((VALUE)(v) & 7) != 0)

#define RUBY_FIXNUM_MAX (LONG_MAX >> 1)
#define RUBY_FIXNUM_MIN (LONG_MIN >> 1)
#define FIXNUM_MAX RUBY_FIXNUM_MAX
#define FIXNUM_MIN RUBY_FIXNUM_MIN
#define RB_INT2FIX(i)   ((VALUE)(((uintptr_t)(intptr_t)(i) << 1) | 1))
#define INT2FIX(i)      RB_INT2FIX(i)
#define LONG2FIX(i)     RB_INT2FIX(i)
#define RB_FIX2LONG(v)  ((long)((intptr_t)(v) >> 1))
#define FIX2LONG(v)     RB_FIX2LONG(v)
#define FIX2INT(v)      ((int)rb_fix2int(v))
#define FIX2ULONG(v)    ((unsigned long)FIX2LONG(v))
#define POSFIXABLE(f)   ((f) < RUBY_FIXNUM_MAX + 1)
#define NEGFIXABLE(f)   ((f) >= RUBY_FIXNUM_MIN)
#define FIXABLE(f)      (POSFIXABLE(f) && NEGFIXABLE(f))

/* ---- types -------------------------------------------------------------- */
enum ruby_value_type {
    RUBY_T_NONE = 0x00, RUBY_T_OBJECT = 0x01, RUBY_T_CLASS = 0x02, RUBY_T_MODULE = 0x03,
    RUBY_T_FLOAT = 0x04, RUBY_T_STRING = 0x05, RUBY_T_REGEXP = 0x06, RUBY_T_ARRAY = 0x07,
    RUBY_T_HASH = 0x08, RUBY_T_STRUCT = 0x09, RUBY_T_BIGNUM = 0x0a, RUBY_T_FILE = 0x0b,
    RUBY_T_DATA = 0x0c, RUBY_T_MATCH = 0x0d, RUBY_T_COMPLEX = 0x0e, RUBY_T_RATIONAL = 0x0f,
    RUBY_T_NIL = 0x11, RUBY_T_TRUE = 0x12, RUBY_T_FALSE = 0x13, RUBY_T_SYMBOL = 0x14,
    RUBY_T_FIXNUM = 0x15, RUBY_T_UNDEF = 0x16, RUBY_T_MASK = 0x1f
};
#define T_NONE RUBY_T_NONE
#define T_OBJECT RUBY_T_OBJECT
#define T_CLASS RUBY_T_CLASS
#define T_MODULE RUBY_T_MODULE
#define T_FLOAT RUBY_T_FLOAT
#define T_STRING RUBY_T_STRING
#define T_REGEXP RUBY_T_REGEXP
#define T_ARRAY RUBY_T_ARRAY
#define T_HASH RUBY_T_HASH
#define T_STRUCT RUBY_T_STRUCT
#define T_BIGNUM RUBY_T_BIGNUM
#define T_FILE RUBY_T_FILE
#define T_DATA RUBY_T_DATA
#define T_MATCH RUBY_T_MATCH
#define T_COMPLEX RUBY_T_COMPLEX
#define T_RATIONAL RUBY_T_RATIONAL
#define T_NIL RUBY_T_NIL
#define T_TRUE RUBY_T_TRUE
#define T_FALSE RUBY_T_FALSE
#define T_SYMBOL RUBY_T_SYMBOL
#define T_FIXNUM RUBY_T_FIXNUM
#define T_UNDEF RUBY_T_UNDEF
#define T_MASK RUBY_T_MASK

int rb_type(VALUE v);
#define TYPE(v)            rb_type((VALUE)(v))
#define RB_TYPE(v)         rb_type((VALUE)(v))
#define RB_TYPE_P(v, t)    (rb_type((VALUE)(v)) == (int)(t))
#define BUILTIN_TYPE(v)    rb_type((VALUE)(v))
#define RB_BUILTIN_TYPE(v) rb_type((VALUE)(v))
#define RB_SYMBOL_P(v)     RB_TYPE_P(v, T_SYMBOL)
#define SYMBOL_P(v)        RB_SYMBOL_P(v)
#define RB_FLOAT_TYPE_P(v) RB_TYPE_P(v, T_FLOAT)
#define RB_INTEGER_TYPE_P(v) (FIXNUM_P(v) || RB_TYPE_P(v, T_BIGNUM))
#define RB_BIGNUM_TYPE_P(v) RB_TYPE_P(v, T_BIGNUM)
void rb_check_type(VALUE v, int t);
#define Check_Type(v, t) rb_check_type((VALUE)(v), (t))

/* ---- data --------------------------------------------------------------- */
typedef void (*RUBY_DATA_FUNC)(void *);
typedef struct rb_data_type_struct rb_data_type_t;
struct rb_data_type_struct {
    const char *wrap_struct_name;
    struct {
        RUBY_DATA_FUNC dmark;
        RUBY_DATA_FUNC dfree;
        size_t (*dsize)(const void *);
        RUBY_DATA_FUNC dcompact;
        void *reserved[1];
    } function;
    const rb_data_type_t *parent;
    void *data;
    VALUE flags;
};
#define RUBY_TYPED_FREE_IMMEDIATELY 1
#define RUBY_TYPED_FROZEN_SHAREABLE 0x100
#define RUBY_TYPED_WB_PROTECTED 0x20
#define RUBY_TYPED_EMBEDDABLE 2
#define RUBY_TYPED_DECL_MARKING 0x4000
#define RUBY_DEFAULT_FREE ((RUBY_DATA_FUNC)-1)
#define RUBY_NEVER_FREE   ((RUBY_DATA_FUNC)0)
#define RUBY_TYPED_DEFAULT_FREE RUBY_DEFAULT_FREE
#define RUBY_TYPED_NEVER_FREE   RUBY_NEVER_FREE
#define RUBY_UNTYPED_DATA_WARNING 0

VALUE rb_data_typed_object_wrap(VALUE klass, void *datap, const rb_data_type_t *type);
VALUE rb_data_typed_object_zalloc(VALUE klass, size_t size, const rb_data_type_t *type);
VALUE rb_data_object_wrap(VALUE klass, void *datap, RUBY_DATA_FUNC dmark, RUBY_DATA_FUNC dfree);
VALUE rb_data_object_zalloc(VALUE klass, size_t size, RUBY_DATA_FUNC dmark, RUBY_DATA_FUNC dfree);
void *rb_check_typeddata(VALUE obj, const rb_data_type_t *type);
int   rb_typeddata_is_kind_of(VALUE obj, const rb_data_type_t *type);
int   rb_typeddata_inherited_p(const rb_data_type_t *child, const rb_data_type_t *parent);
void *rb_data_get_ptr(VALUE obj);
void  rb_data_set_ptr(VALUE obj, void *ptr);
const rb_data_type_t *rb_typeddata_type(VALUE obj);
int   rb_data_p(VALUE obj);   /* koruby: T_DATA with or without a type */
#define RTYPEDDATA_DATA(v)       rb_data_get_ptr(v)
#define RTYPEDDATA_GET_DATA(v)   rb_data_get_ptr(v)
#define RTYPEDDATA_TYPE(v)       rb_typeddata_type(v)
#define RTYPEDDATA_P(v)          (rb_typeddata_type(v) != NULL)
#define RB_TYPEDDATA_P(v)        (RB_TYPE_P(v, T_DATA) && RTYPEDDATA_P(v))
#define DATA_PTR(v)              (*rb_data_ptr_ref(v))
void **rb_data_ptr_ref(VALUE obj);

#define TypedData_Wrap_Struct(klass, data_type, sval) rb_data_typed_object_wrap((klass), (sval), (data_type))
#define TypedData_Make_Struct0(result, klass, type, size, data_type, sval) \
    VALUE result = rb_data_typed_object_zalloc((klass), (size), (data_type)); \
    (sval) = (type *)rb_data_get_ptr(result)
#define TypedData_Make_Struct(klass, type, data_type, sval) \
    (__extension__({ TypedData_Make_Struct0(rb_data_tmp_, (klass), type, sizeof(type), (data_type), (sval)); rb_data_tmp_; }))
#define TypedData_Get_Struct(obj, type, data_type, sval) \
    ((sval) = (type *)rb_check_typeddata((obj), (data_type)))
#define Data_Wrap_Struct(klass, mark, free, sval) \
    rb_data_object_wrap((klass), (void *)(sval), (RUBY_DATA_FUNC)(mark), (RUBY_DATA_FUNC)(free))
#define Data_Make_Struct(klass, type, mark, free, sval) \
    (__extension__({ VALUE rb_data_tmp_ = rb_data_object_zalloc((klass), sizeof(type), (RUBY_DATA_FUNC)(mark), (RUBY_DATA_FUNC)(free)); \
                     (sval) = (type *)rb_data_get_ptr(rb_data_tmp_); rb_data_tmp_; }))
#define Data_Get_Struct(obj, type, sval) ((sval) = (type *)rb_data_get_ptr(obj))

/* ---- memory ------------------------------------------------------------- */
void *ruby_xmalloc(size_t n);
void *ruby_xmalloc2(size_t n, size_t size);
void *ruby_xcalloc(size_t n, size_t size);
void *ruby_xrealloc(void *p, size_t n);
void *ruby_xrealloc2(void *p, size_t n, size_t size);
void  ruby_xfree(void *p);
#define xmalloc  ruby_xmalloc
#define xmalloc2 ruby_xmalloc2
#define xcalloc  ruby_xcalloc
#define xrealloc ruby_xrealloc
#define xrealloc2 ruby_xrealloc2
#define xfree    ruby_xfree
#define ALLOC(type)        ((type *)ruby_xmalloc(sizeof(type)))
#define ALLOC_N(type, n)   ((type *)ruby_xmalloc2((n), sizeof(type)))
#define ZALLOC(type)       ((type *)ruby_xcalloc(1, sizeof(type)))
#define ZALLOC_N(type, n)  ((type *)ruby_xcalloc((n), sizeof(type)))
#define REALLOC_N(var, type, n) ((var) = (type *)ruby_xrealloc2((void *)(var), (n), sizeof(type)))
#define ALLOCA_N(type, n)  ((type *)__builtin_alloca(sizeof(type) * (n)))
#define MEMZERO(p, type, n)    memset((p), 0, sizeof(type) * (size_t)(n))
#define MEMCPY(p1, p2, type, n)  memcpy((p1), (p2), sizeof(type) * (size_t)(n))
#define MEMMOVE(p1, p2, type, n) memmove((p1), (p2), sizeof(type) * (size_t)(n))
#define MEMCMP(p1, p2, type, n)  memcmp((p1), (p2), sizeof(type) * (size_t)(n))

/* ---- GC ----------------------------------------------------------------- */
void rb_gc_register_address(VALUE *addr);
void rb_gc_unregister_address(VALUE *addr);
void rb_global_variable(VALUE *addr);
void rb_gc_register_mark_object(VALUE obj);
void rb_gc_mark(VALUE obj);
void rb_gc_mark_movable(VALUE obj);
void rb_gc_mark_maybe(VALUE obj);
VALUE rb_gc_location(VALUE obj);
void rb_gc(void);
VALUE rb_gc_start(void);
#define RB_OBJ_WRITE(old, slot, young) (*(slot) = (young))
#define RB_OBJ_WRITTEN(old, oldv, young) ((void)0)

/* ---- symbols / IDs ------------------------------------------------------ */
ID    rb_intern(const char *name);
ID    rb_intern2(const char *name, long len);
ID    rb_intern3(const char *name, long len, void *enc);
/* a literal name is interned once per call site (CRuby's ruby.h does the same) */
#define rb_intern(str) (__builtin_constant_p(str) \
    ? __extension__({ static ID rb_intern_id_cache_; \
                      if (!rb_intern_id_cache_) rb_intern_id_cache_ = rb_intern2((str), (long)strlen(str)); \
                      rb_intern_id_cache_; }) \
    : (rb_intern)(str))
#define rb_intern_const(s) rb_intern(s)
#define CONST_ID(var, str) do { static ID the_id_; if (!the_id_) the_id_ = rb_intern(str); (var) = the_id_; } while (0)
const char *rb_id2name(ID id);
VALUE rb_id2str(ID id);
VALUE rb_id2sym(ID id);
ID    rb_sym2id(VALUE sym);
VALUE rb_sym2str(VALUE sym);
ID    rb_to_id(VALUE name);
VALUE rb_to_symbol(VALUE name);
ID    rb_check_id(volatile VALUE *namep);
#define ID2SYM(id) rb_id2sym(id)
#define SYM2ID(s)  rb_sym2id(s)
#define RB_ID2SYM  rb_id2sym
#define RB_SYM2ID  rb_sym2id

/* ---- numeric ------------------------------------------------------------ */
long   rb_num2long(VALUE v);
unsigned long rb_num2ulong(VALUE v);
long long rb_num2ll(VALUE v);
unsigned long long rb_num2ull(VALUE v);
long   rb_num2int(VALUE v);
long   rb_fix2int(VALUE v);
unsigned long rb_num2uint(VALUE v);
double rb_num2dbl(VALUE v);
VALUE  rb_int2inum(intptr_t n);
VALUE  rb_uint2inum(uintptr_t n);
VALUE  rb_ll2inum(long long n);
VALUE  rb_ull2inum(unsigned long long n);
VALUE  rb_float_new(double d);
double rb_float_value(VALUE v);
VALUE  rb_Integer(VALUE v);
VALUE  rb_Float(VALUE v);
#define NUM2LONG(v)  rb_num2long(v)
#define NUM2ULONG(v) rb_num2ulong(v)
#define NUM2INT(v)   ((int)rb_num2int(v))
#define NUM2UINT(v)  ((unsigned int)rb_num2uint(v))
#define NUM2LL(v)    rb_num2ll(v)
#define NUM2ULL(v)   rb_num2ull(v)
#define NUM2SIZET(v) ((size_t)rb_num2ulong(v))
#define NUM2SSIZET(v) ((ssize_t)rb_num2long(v))
#define NUM2DBL(v)   rb_num2dbl(v)
#define NUM2CHR(v)   ((char)rb_num2int(v))
#define LONG2NUM(n)  rb_int2inum((intptr_t)(n))
#define ULONG2NUM(n) rb_uint2inum((uintptr_t)(n))
#define INT2NUM(n)   rb_int2inum((intptr_t)(n))
#define UINT2NUM(n)  rb_uint2inum((uintptr_t)(n))
#define LL2NUM(n)    rb_ll2inum(n)
#define ULL2NUM(n)   rb_ull2inum(n)
#define SIZET2NUM(n) rb_uint2inum((uintptr_t)(n))
#define SSIZET2NUM(n) rb_int2inum((intptr_t)(n))
#define DBL2NUM(d)   rb_float_new(d)
#define RFLOAT_VALUE(v) rb_float_value(v)
#define CHR2FIX(c)   INT2FIX((long)((c) & 0xff))

/* ---- strings ------------------------------------------------------------ */
VALUE rb_str_new(const char *ptr, long len);
VALUE rb_str_new_cstr(const char *ptr);
VALUE rb_usascii_str_new(const char *ptr, long len);
VALUE rb_usascii_str_new_cstr(const char *ptr);
VALUE rb_utf8_str_new(const char *ptr, long len);
VALUE rb_utf8_str_new_cstr(const char *ptr);
VALUE rb_str_buf_new(long capa);
VALUE rb_str_buf_cat(VALUE str, const char *ptr, long len);
VALUE rb_str_buf_cat2(VALUE str, const char *ptr);
VALUE rb_str_cat(VALUE str, const char *ptr, long len);
VALUE rb_str_cat2(VALUE str, const char *ptr);
VALUE rb_str_cat_cstr(VALUE str, const char *ptr);
VALUE rb_str_buf_append(VALUE str, VALUE str2);
VALUE rb_str_append(VALUE str, VALUE str2);
VALUE rb_str_concat(VALUE str, VALUE obj);
VALUE rb_str_dup(VALUE str);
VALUE rb_str_resize(VALUE str, long len);
void  rb_str_set_len(VALUE str, long len);
VALUE rb_str_freeze(VALUE str);
int   rb_str_cmp(VALUE a, VALUE b);
VALUE rb_str_equal(VALUE a, VALUE b);
VALUE rb_str_plus(VALUE a, VALUE b);
VALUE rb_str_times(VALUE str, VALUE times);
VALUE rb_str_substr(VALUE str, long beg, long len);
VALUE rb_str_intern(VALUE str);
VALUE rb_str_to_str(VALUE obj);
VALUE rb_string_value(volatile VALUE *ptr);
char *rb_string_value_ptr(volatile VALUE *ptr);
char *rb_string_value_cstr(volatile VALUE *ptr);
VALUE rb_check_string_type(VALUE obj);
VALUE rb_obj_as_string(VALUE obj);
VALUE rb_str_inspect(VALUE str);
VALUE rb_sprintf(const char *fmt, ...);
VALUE rb_vsprintf(const char *fmt, va_list ap);
VALUE rb_str_catf(VALUE str, const char *fmt, ...);
char *rb_string_ptr(VALUE str);   /* koruby: RSTRING_PTR (pins the bytes) */
long  rb_string_len(VALUE str);
#define rb_str_new2 rb_str_new_cstr
#define rb_str_new3 rb_str_dup
#define rb_str_new_lit(s) rb_str_new((s), (long)(sizeof(s) - 1))
#define rb_str_new_literal(s) rb_str_new_lit(s)
#define rb_usascii_str_new_lit(s) rb_usascii_str_new((s), (long)(sizeof(s) - 1))
#define RSTRING_PTR(s)  rb_string_ptr(s)
#define RSTRING_LEN(s)  rb_string_len(s)
#define RSTRING_LENINT(s) ((int)rb_string_len(s))
#define RSTRING_END(s)  (rb_string_ptr(s) + rb_string_len(s))
#define RSTRING_GETMEM(s, ptrvar, lenvar) ((ptrvar) = RSTRING_PTR(s), (lenvar) = RSTRING_LEN(s))
#define StringValue(v)     rb_string_value(&(v))
#define StringValuePtr(v)  rb_string_value_ptr(&(v))
#define StringValueCStr(v) rb_string_value_cstr(&(v))
#define PRI_VALUE_PREFIX "l"
#define PRIsVALUE PRI_VALUE_PREFIX "i" "\v"   /* rb_sprintf formats the argument as a VALUE (to_s; "+" = inspect) */

/* ---- arrays / hashes ---------------------------------------------------- */
VALUE rb_ary_new(void);
VALUE rb_ary_new_capa(long capa);
VALUE rb_ary_new_from_args(long n, ...);
VALUE rb_ary_new_from_values(long n, const VALUE *elts);
VALUE rb_ary_push(VALUE ary, VALUE item);
VALUE rb_ary_pop(VALUE ary);
VALUE rb_ary_shift(VALUE ary);
VALUE rb_ary_unshift(VALUE ary, VALUE item);
VALUE rb_ary_entry(VALUE ary, long idx);
void  rb_ary_store(VALUE ary, long idx, VALUE val);
VALUE rb_ary_dup(VALUE ary);
VALUE rb_ary_join(VALUE ary, VALUE sep);
VALUE rb_ary_concat(VALUE x, VALUE y);
VALUE rb_ary_clear(VALUE ary);
VALUE rb_ary_includes(VALUE ary, VALUE item);
VALUE rb_check_array_type(VALUE obj);
VALUE rb_Array(VALUE obj);
long  rb_array_len(VALUE ary);
#define rb_ary_new2 rb_ary_new_capa
#define rb_ary_new3 rb_ary_new_from_args
#define rb_ary_new4 rb_ary_new_from_values
#define RARRAY_LEN(a)      rb_array_len(a)
#define RARRAY_LENINT(a)   ((int)rb_array_len(a))
#define RARRAY_AREF(a, i)  rb_ary_entry((a), (i))
#define rb_ary_aref1(a, i) rb_ary_entry((a), NUM2LONG(i))
VALUE rb_hash_new(void);
VALUE rb_hash_new_capa(long capa);
VALUE rb_hash_aref(VALUE hash, VALUE key);
VALUE rb_hash_aset(VALUE hash, VALUE key, VALUE val);
VALUE rb_hash_lookup(VALUE hash, VALUE key);
VALUE rb_hash_lookup2(VALUE hash, VALUE key, VALUE def);
VALUE rb_hash_fetch(VALUE hash, VALUE key);
VALUE rb_hash_delete(VALUE hash, VALUE key);
VALUE rb_hash_clear(VALUE hash);
VALUE rb_hash_size(VALUE hash);
size_t rb_hash_size_num(VALUE hash);
VALUE rb_hash_keys(VALUE hash);
VALUE rb_check_hash_type(VALUE obj);
enum rb_hash_iter_status { ST_CONTINUE = 0, ST_STOP = 1, ST_DELETE = 2, ST_CHECK = 3 };
void  rb_hash_foreach(VALUE hash, int (*func)(VALUE key, VALUE val, VALUE arg), VALUE arg);
#define RHASH_SIZE(h) rb_hash_size_num(h)

/* ---- objects / classes -------------------------------------------------- */
RUBY_EXTERN VALUE rb_mKernel, rb_mComparable, rb_mEnumerable, rb_mMath, rb_mProcess, rb_mGC;
RUBY_EXTERN VALUE rb_cBasicObject, rb_cObject, rb_cModule, rb_cClass, rb_cString, rb_cSymbol,
                  rb_cArray, rb_cHash, rb_cInteger, rb_cFloat, rb_cNumeric, rb_cNilClass,
                  rb_cTrueClass, rb_cFalseClass, rb_cProc, rb_cMethod, rb_cRange, rb_cIO, rb_cFile,
                  rb_cTime, rb_cStruct, rb_cRational, rb_cComplex, rb_cRegexp, rb_cMatch,
                  rb_cEncoding, rb_cData, rb_cThread;
RUBY_EXTERN VALUE rb_eException, rb_eStandardError, rb_eRuntimeError, rb_eArgError, rb_eTypeError,
                  rb_eNameError, rb_eNoMethodError, rb_eIndexError, rb_eKeyError, rb_eRangeError,
                  rb_eIOError, rb_eEOFError, rb_eLoadError, rb_eNotImpError, rb_eFrozenError,
                  rb_eZeroDivError, rb_eFloatDomainError, rb_eStopIteration, rb_eSecurityError,
                  rb_eSystemCallError, rb_eNoMemError, rb_eScriptError, rb_eSyntaxError,
                  rb_eEncodingError, rb_eEncCompatError, rb_eInterrupt, rb_eSignal, rb_eFatal,
                  rb_eSystemExit, rb_eLocalJumpError, rb_eMathDomainError, rb_eThreadError;

VALUE rb_define_class(const char *name, VALUE super);
VALUE rb_define_module(const char *name);
VALUE rb_define_class_under(VALUE outer, const char *name, VALUE super);
VALUE rb_define_module_under(VALUE outer, const char *name);
VALUE rb_define_class_id_under(VALUE outer, ID id, VALUE super);
VALUE rb_define_module_id_under(VALUE outer, ID id);
void  rb_include_module(VALUE klass, VALUE module);
void  rb_extend_object(VALUE obj, VALUE module);
void  rb_prepend_module(VALUE klass, VALUE module);
typedef VALUE (*rb_alloc_func_t)(VALUE klass);
void  rb_define_alloc_func(VALUE klass, rb_alloc_func_t func);
void  rb_undef_alloc_func(VALUE klass);
/* The method table takes any C function; arity says how it is called
 * (-1: (int argc, VALUE *argv, VALUE self), -2: (VALUE self, VALUE args), n: (self, a1..an)).
 * The macros cast, as CRuby's do, so a (VALUE, VALUE) function is accepted under C23. */
typedef VALUE (*rb_cfunc_anyargs)(void);
#define RUBY_METHOD_FUNC(f) ((rb_cfunc_anyargs)(f))
void  rb_define_method(VALUE klass, const char *name, void *func, int arity);
void  rb_define_private_method(VALUE klass, const char *name, void *func, int arity);
void  rb_define_protected_method(VALUE klass, const char *name, void *func, int arity);
void  rb_define_singleton_method(VALUE obj, const char *name, void *func, int arity);
void  rb_define_module_function(VALUE mod, const char *name, void *func, int arity);
void  rb_define_global_function(const char *name, void *func, int arity);
void  rb_define_method_id(VALUE klass, ID mid, void *func, int arity);
#define rb_define_method(k, n, f, a)           (rb_define_method)((k), (n), (void *)(f), (a))
#define rb_define_private_method(k, n, f, a)   (rb_define_private_method)((k), (n), (void *)(f), (a))
#define rb_define_protected_method(k, n, f, a) (rb_define_protected_method)((k), (n), (void *)(f), (a))
#define rb_define_singleton_method(k, n, f, a) (rb_define_singleton_method)((k), (n), (void *)(f), (a))
#define rb_define_module_function(k, n, f, a)  (rb_define_module_function)((k), (n), (void *)(f), (a))
#define rb_define_global_function(n, f, a)     (rb_define_global_function)((n), (void *)(f), (a))
#define rb_define_method_id(k, m, f, a)        (rb_define_method_id)((k), (m), (void *)(f), (a))
void  rb_undef_method(VALUE klass, const char *name);
void  rb_define_alias(VALUE klass, const char *newname, const char *oldname);
void  rb_alias(VALUE klass, ID newname, ID oldname);
void  rb_define_attr(VALUE klass, const char *name, int read, int write);
void  rb_define_const(VALUE mod, const char *name, VALUE val);
void  rb_define_global_const(const char *name, VALUE val);
VALUE rb_const_get(VALUE mod, ID id);
VALUE rb_const_get_at(VALUE mod, ID id);
int   rb_const_defined(VALUE mod, ID id);
int   rb_const_defined_at(VALUE mod, ID id);
void  rb_const_set(VALUE mod, ID id, VALUE val);
VALUE rb_path2class(const char *path);
VALUE rb_path_to_class(VALUE path);
VALUE rb_class_new_instance(int argc, const VALUE *argv, VALUE klass);
VALUE rb_class_new_instance_kw(int argc, const VALUE *argv, VALUE klass, int kw_splat);
VALUE rb_obj_alloc(VALUE klass);
void  rb_obj_call_init(VALUE obj, int argc, const VALUE *argv);
void  rb_obj_call_init_kw(VALUE obj, int argc, const VALUE *argv, int kw_splat);
VALUE rb_obj_class(VALUE obj);
VALUE rb_class_of(VALUE obj);
#define CLASS_OF(v) rb_class_of((VALUE)(v))
#define RBASIC_CLASS(v) rb_class_of((VALUE)(v))
VALUE rb_singleton_class(VALUE obj);
VALUE rb_class_superclass(VALUE klass);
VALUE rb_class_name(VALUE klass);
VALUE rb_class_path(VALUE klass);
const char *rb_class2name(VALUE klass);
const char *rb_obj_classname(VALUE obj);
VALUE rb_obj_is_kind_of(VALUE obj, VALUE klass);
VALUE rb_obj_is_instance_of(VALUE obj, VALUE klass);
VALUE rb_class_inherited_p(VALUE mod, VALUE arg);
VALUE rb_obj_freeze(VALUE obj);
VALUE rb_obj_frozen_p(VALUE obj);
void  rb_check_frozen(VALUE obj);
#define OBJ_FREEZE(v) rb_obj_freeze(v)
#define OBJ_FROZEN(v) RTEST(rb_obj_frozen_p(v))
#define RB_OBJ_FROZEN(v) OBJ_FROZEN(v)
VALUE rb_obj_dup(VALUE obj);
VALUE rb_obj_clone(VALUE obj);
VALUE rb_obj_id(VALUE obj);
VALUE rb_inspect(VALUE obj);
VALUE rb_obj_as_string(VALUE obj);
VALUE rb_any_to_s(VALUE obj);
VALUE rb_equal(VALUE a, VALUE b);
VALUE rb_eql(VALUE a, VALUE b);
int   rb_respond_to(VALUE obj, ID mid);
VALUE rb_obj_method(VALUE obj, VALUE name);
VALUE rb_ivar_get(VALUE obj, ID name);
VALUE rb_ivar_set(VALUE obj, ID name, VALUE val);
VALUE rb_ivar_defined(VALUE obj, ID name);
VALUE rb_iv_get(VALUE obj, const char *name);
VALUE rb_iv_set(VALUE obj, const char *name, VALUE val);
VALUE rb_attr_get(VALUE obj, ID name);
VALUE rb_cvar_get(VALUE klass, ID name);
void  rb_cvar_set(VALUE klass, ID name, VALUE val);
VALUE rb_gv_get(const char *name);
VALUE rb_gv_set(const char *name, VALUE val);
void  rb_define_variable(const char *name, VALUE *var);

/* ---- calls / blocks ----------------------------------------------------- */
VALUE rb_funcall(VALUE recv, ID mid, int n, ...);
VALUE rb_funcallv(VALUE recv, ID mid, int argc, const VALUE *argv);
VALUE rb_funcallv_kw(VALUE recv, ID mid, int argc, const VALUE *argv, int kw_splat);
VALUE rb_funcallv_public(VALUE recv, ID mid, int argc, const VALUE *argv);
VALUE rb_funcall_passing_block(VALUE recv, ID mid, int argc, const VALUE *argv);
VALUE rb_funcall_with_block(VALUE recv, ID mid, int argc, const VALUE *argv, VALUE procval);
VALUE rb_call_super(int argc, const VALUE *argv);
VALUE rb_current_receiver(void);
#define rb_funcall2 rb_funcallv
#define rb_funcall3 rb_funcallv_public
#define RB_NO_KEYWORDS 0
#define RB_PASS_KEYWORDS 1
#define RB_PASS_CALLED_KEYWORDS rb_keyword_given_p()
int   rb_block_given_p(void);
void  rb_need_block(void);
VALUE rb_block_proc(void);
VALUE rb_yield(VALUE val);
VALUE rb_yield_values(int n, ...);
VALUE rb_yield_values2(int argc, const VALUE *argv);
VALUE rb_yield_splat(VALUE ary);
int   rb_keyword_given_p(void);
typedef VALUE rb_block_call_func(VALUE yielded_arg, VALUE callback_arg, int argc, const VALUE *argv, VALUE blockarg);
typedef rb_block_call_func *rb_block_call_func_t;
#define RB_BLOCK_CALL_FUNC_ARGLIST(yielded_arg, callback_arg) \
    VALUE yielded_arg, VALUE callback_arg, int argc, const VALUE *argv, VALUE blockarg
VALUE rb_block_call(VALUE obj, ID mid, int argc, const VALUE *argv, rb_block_call_func_t bl_proc, VALUE data2);
VALUE rb_proc_new(rb_block_call_func_t func, VALUE val);
VALUE rb_proc_call(VALUE proc, VALUE args);
VALUE rb_proc_call_with_block(VALUE proc, int argc, const VALUE *argv, VALUE passed_proc);
VALUE rb_obj_is_proc(VALUE obj);
VALUE rb_each(VALUE obj);
int   rb_scan_args(int argc, const VALUE *argv, const char *fmt, ...);
int   rb_scan_args_kw(int kw_splat, int argc, const VALUE *argv, const char *fmt, ...);
int   rb_get_kwargs(VALUE keyword_hash, const ID *table, int required, int optional, VALUE *values);
VALUE rb_extract_keywords(VALUE *orighash);
void  rb_check_arity(int argc, int min, int max);
#define UNLIMITED_ARGUMENTS (-1)
NORETURN(void rb_error_arity(int argc, int min, int max));
VALUE rb_eval_string(const char *str);
VALUE rb_eval_string_protect(const char *str, int *state);
VALUE rb_require(const char *feature);
VALUE rb_f_require(VALUE self, VALUE fname);
void *rb_ext_resolve_symbol(const char *feature, const char *symbol);
void  rb_ext_ractor_safe(bool flag);
void  rb_provide(const char *feature);

/* ---- exceptions --------------------------------------------------------- */
NORETURN(void rb_raise(VALUE exc_class, const char *fmt, ...)) __attribute__((format(printf, 2, 3)));
NORETURN(void rb_exc_raise(VALUE exc));
NORETURN(void rb_fatal(const char *fmt, ...));
NORETURN(void rb_bug(const char *fmt, ...));
NORETURN(void rb_sys_fail(const char *msg));
NORETURN(void rb_sys_fail_str(VALUE msg));
NORETURN(void rb_syserr_fail(int err, const char *msg));
NORETURN(void rb_notimplement(void));
NORETURN(void rb_memerror(void));
NORETURN(void rb_num_zerodiv(void));
NORETURN(void rb_frozen_error_raise(VALUE frozen_obj, const char *fmt, ...));
NORETURN(void rb_error_frozen_object(VALUE frozen_obj));
NORETURN(void rb_iter_break(void));
NORETURN(void rb_iter_break_value(VALUE val));
NORETURN(void rb_throw(const char *tag, VALUE val));
NORETURN(void rb_throw_obj(VALUE tag, VALUE val));
NORETURN(void rb_jump_tag(int state));
NORETURN(void rb_exit(int status));
void  rb_warn(const char *fmt, ...);
void  rb_warning(const char *fmt, ...);
VALUE rb_exc_new(VALUE klass, const char *ptr, long len);
VALUE rb_exc_new_cstr(VALUE klass, const char *ptr);
VALUE rb_exc_new_str(VALUE klass, VALUE str);
#define rb_exc_new2 rb_exc_new_cstr
#define rb_exc_new3 rb_exc_new_str
VALUE rb_errinfo(void);
void  rb_set_errinfo(VALUE err);
VALUE rb_protect(VALUE (*func)(VALUE), VALUE arg, int *state);
VALUE rb_ensure(VALUE (*b_proc)(VALUE), VALUE data1, VALUE (*e_proc)(VALUE), VALUE data2);
VALUE rb_rescue(VALUE (*b_proc)(VALUE), VALUE data1, VALUE (*r_proc)(VALUE, VALUE), VALUE data2);
VALUE rb_rescue2(VALUE (*b_proc)(VALUE), VALUE data1, VALUE (*r_proc)(VALUE, VALUE), VALUE data2, ...);
VALUE rb_catch(const char *tag, rb_block_call_func_t func, VALUE data);
VALUE rb_catch_obj(VALUE tag, rb_block_call_func_t func, VALUE data);

/* ---- threads (koruby is single-OS-thread: these just call) -------------- */
typedef void *rb_blocking_function_t(void *);
typedef void rb_unblock_function_t(void *);
#define RUBY_UBF_IO ((rb_unblock_function_t *)-1)
#define RUBY_UBF_PROCESS ((rb_unblock_function_t *)-1)
void *rb_thread_call_without_gvl(void *(*func)(void *), void *data1, rb_unblock_function_t *ubf, void *data2);
void *rb_thread_call_with_gvl(void *(*func)(void *), void *data1);
void *rb_nogvl(void *(*func)(void *), void *data1, rb_unblock_function_t *ubf, void *data2, int flags);
void  rb_thread_check_ints(void);
VALUE rb_thread_current(void);
void  rb_thread_schedule(void);

#define rb_intern_str(s) rb_to_id(s)
#define InitVM(ext) {void InitVM_##ext(void); InitVM_##ext();}

#ifdef __cplusplus
}
#endif
#endif
