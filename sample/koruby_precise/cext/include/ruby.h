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
/* what CRuby's headers pull in; extensions (and mkmf's probes: printf) rely on it */
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>
#include <alloca.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#if defined(__has_include)
#  if __has_include(<ruby/koruby_platform.h>)
#    include <ruby/koruby_platform.h>   /* HAVE_* from CRuby's config.h (make root) */
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KORUBY 1
/* Marks a shared object as built with this header: koruby's require refuses a
 * .so without it (a CRuby-built extension would run CRuby's ABI, even libruby). */
__attribute__((weak, used, visibility("default"))) const int koruby_cext_abi_1 = 1;
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
/* ruby/config.h (x86_64 LP64) */
#define SIZEOF_INT 4
#define SIZEOF_SHORT 2
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOIDP 8
#define SIZEOF_SIZE_T 8
#define SIZEOF_TIME_T 8
#define SIZEOF_OFF_T 8
#define SIZEOF_VALUE 8
#define SIZEOF_INT64_T 8
#define SIZEOF_UINT64_T 8
#define HAVE_LONG_LONG 1
#define HAVE_INT64_T 1
#define HAVE_UINT64_T 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_RUBY_RACTOR_H 1

typedef uintptr_t VALUE;
typedef intptr_t  SIGNED_VALUE;
typedef uintptr_t ID;
typedef uintptr_t st_data_t;
typedef uintptr_t st_index_t;

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
#define RUBY_ASSERT(expr) ((void)0)
#define RUBY_ASSERT_ALWAYS(expr) ((void)0)
#define ASSUME(x) ((void)0)
#define RB_GC_GUARD(v) (*__extension__({ volatile VALUE *rb_gc_guarded_ptr = &(v); rb_gc_guarded_ptr; }))
#define RBIMPL_ATTR_MAYBE_UNUSED() __attribute__((unused))
#define MAYBE_UNUSED(x) __attribute__((unused)) x
#define RUBY_ALIGNOF _Alignof
#define ALWAYS_INLINE(x) __attribute__((always_inline)) inline x   /* CRuby: RBIMPL_ATTR_FORCEINLINE() x */
#define NOINLINE(x) __attribute__((noinline)) x
#define LONG_LONG long long
#define PUREFUNC(x)  __attribute__((pure)) x
#define CONSTFUNC(x) __attribute__((const)) x
#define PRI_SIZE_PREFIX "z"
#define PRIdSIZE PRI_SIZE_PREFIX "d"
#define PRIiSIZE PRI_SIZE_PREFIX "i"
#define PRIoSIZE PRI_SIZE_PREFIX "o"
#define PRIuSIZE PRI_SIZE_PREFIX "u"
#define PRIxSIZE PRI_SIZE_PREFIX "x"
#define PRIXSIZE PRI_SIZE_PREFIX "X"

/* ---- immediates: CRuby's values (koruby swaps nil / false at the boundary) -- */
#define Qfalse  ((VALUE)0x00)
#define Qnil    ((VALUE)0x04)
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
#define RTYPEDDATA_DATA(v)       (*rb_data_ptr_ref(v))   /* an lvalue, as in CRuby */
#define RTYPEDDATA_GET_DATA(v)   rb_data_get_ptr(v)
#define RTYPEDDATA_TYPE(v)       rb_typeddata_type(v)
#define RTYPEDDATA_P(v)          (rb_typeddata_type(v) != NULL)
#define RB_TYPEDDATA_P(v)        (RB_TYPE_P(v, T_DATA) && RTYPEDDATA_P(v))
#define DATA_PTR(v)              (*rb_data_ptr_ref(v))
struct RTypedData { void *data; };                /* only ->data: RTYPEDDATA(obj)->data = p */
#define RTYPEDDATA(v)            ((struct RTypedData *)(void *)rb_data_ptr_ref(v))
void **rb_data_ptr_ref(VALUE obj);

#define TypedData_Wrap_Struct(klass, data_type, sval) rb_data_typed_object_wrap((klass), (sval), (data_type))
#define TypedData_Make_Struct0(result, klass, type, size, data_type, sval) \
    VALUE result = rb_data_typed_object_zalloc((klass), (size), (data_type)); \
    (sval) = (type *)rb_data_get_ptr(result);                                  \
    (void)(sval)
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
/* ALLOCV: CRuby hands back GC-managed scratch; here plain malloc freed by ALLOCV_END */
void *rb_alloc_tmp_buffer(volatile VALUE *store, long len);
void  rb_free_tmp_buffer(volatile VALUE *store);
#define ALLOCV(v, n)        rb_alloc_tmp_buffer(&(v), (long)(n))
#define ALLOCV_N(type, v, n) ((type *)ALLOCV((v), sizeof(type) * (size_t)(n)))
#define ALLOCV_END(v)       rb_free_tmp_buffer(&(v))
#define MEMZERO(p, type, n)    memset((p), 0, sizeof(type) * (size_t)(n))
#define MEMCPY(p1, p2, type, n)  memcpy((p1), (p2), sizeof(type) * (size_t)(n))
#define MEMMOVE(p1, p2, type, n) memmove((p1), (p2), sizeof(type) * (size_t)(n))
#define MEMCMP(p1, p2, type, n)  memcmp((p1), (p2), sizeof(type) * (size_t)(n))

/* ---- GC ----------------------------------------------------------------- */
void rb_gc_register_address(VALUE *addr);
void rb_gc_unregister_address(VALUE *addr);
void rb_global_variable(VALUE *addr);
void rb_gc_register_mark_object(VALUE obj);
void rb_gc_mark_locations(const VALUE *start, const VALUE *end);
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
#define NUM2TIMET(v)   ((time_t)rb_num2ll(v))
#define TIMET2NUM(t)   rb_ll2inum((long long)(t))
#define NUM2OFFT(v)    ((off_t)rb_num2ll(v))
#define OFFT2NUM(n)    rb_ll2inum((long long)(n))
#define ST2FIX(h)      LONG2FIX((long)(h))
int   rb_long2int(long n);
VALUE rb_cstr_to_inum(const char *str, int base, int badcheck);
VALUE rb_str_to_inum(VALUE str, int base, int badcheck);
size_t rb_absint_size(VALUE val, int *nlz_bits_ret);
int   rb_integer_pack(VALUE val, void *words, size_t numwords, size_t wordsize, size_t nails, int flags);
VALUE rb_integer_unpack(const void *words, size_t numwords, size_t wordsize, size_t nails, int flags);
#define INTEGER_PACK_MSWORD_FIRST       0x01
#define INTEGER_PACK_LSWORD_FIRST       0x02
#define INTEGER_PACK_MSBYTE_FIRST       0x10
#define INTEGER_PACK_LSBYTE_FIRST       0x20
#define INTEGER_PACK_NATIVE             0x40
#define INTEGER_PACK_NATIVE_BYTE_ORDER  0x40
#define INTEGER_PACK_2COMP              0x80
#define INTEGER_PACK_FORCE_GENERIC_IMPLEMENTATION 0x400
#define INTEGER_PACK_NEGATIVE           0x200
#define INTEGER_PACK_BIG_ENDIAN (INTEGER_PACK_MSWORD_FIRST | INTEGER_PACK_MSBYTE_FIRST)
#define INTEGER_PACK_LITTLE_ENDIAN (INTEGER_PACK_LSWORD_FIRST | INTEGER_PACK_LSBYTE_FIRST)

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
void  rb_str_modify(VALUE str);
void  rb_str_modify_expand(VALUE str, long expand);
size_t rb_str_capacity(VALUE str);
VALUE rb_str_locktmp(VALUE str);
VALUE rb_str_unlocktmp(VALUE str);
VALUE rb_str_resurrect(VALUE str);
VALUE rb_str_new_frozen(VALUE str);
VALUE rb_str_drop_bytes(VALUE str, long len);
VALUE rb_String(VALUE obj);
VALUE rb_convert_type(VALUE val, int type, const char *tname, const char *method);
VALUE rb_check_convert_type(VALUE val, int type, const char *tname, const char *method);
st_index_t rb_memhash(const void *ptr, long len);
#define rb_str_new4 rb_str_new_frozen
#define rb_str_new5 rb_str_new
#define RSTRING(s) (s)
/* encodings: an rb_encoding is an opaque handle (koruby's encoding index + 1) */
typedef struct rb_encoding_kx rb_encoding;
rb_encoding *rb_utf8_encoding(void);
rb_encoding *rb_ascii8bit_encoding(void);
rb_encoding *rb_usascii_encoding(void);
int   rb_utf8_encindex(void);
int   rb_ascii8bit_encindex(void);
int   rb_usascii_encindex(void);
VALUE rb_enc_associate_index(VALUE obj, int idx);
VALUE rb_enc_associate(VALUE obj, rb_encoding *enc);
int   rb_enc_get_index(VALUE obj);
VALUE rb_enc_str_new(const char *ptr, long len, rb_encoding *enc);
VALUE rb_enc_sprintf(rb_encoding *enc, const char *fmt, ...);
VALUE rb_external_str_new(const char *ptr, long len);
rb_encoding *rb_enc_from_index(int idx);
int   rb_enc_to_index(rb_encoding *enc);
int   rb_enc_find_index(const char *name);
rb_encoding *rb_enc_find(const char *name);
VALUE rb_enc_from_encoding(rb_encoding *enc);
rb_encoding *rb_to_encoding(VALUE obj);
rb_encoding *rb_find_encoding(VALUE obj);
int   rb_to_encoding_index(VALUE obj);
const char *rb_enc_name(rb_encoding *enc);
int   rb_enc_mbminlen(rb_encoding *enc);
int   rb_enc_mbmaxlen(rb_encoding *enc);
int   rb_enc_asciicompat(rb_encoding *enc);
int   rb_enc_mbclen(const char *p, const char *e, rb_encoding *enc);
int   rb_enc_precise_mbclen(const char *p, const char *e, rb_encoding *enc);
int   rb_enc_fast_mbclen(const char *p, const char *e, rb_encoding *enc);
#define MBCLEN_CHARFOUND_P(ret)   (0 < (ret))
#define MBCLEN_CHARFOUND_LEN(ret) (ret)
#define MBCLEN_INVALID_P(ret)     ((ret) == -1)
#define MBCLEN_NEEDMORE_P(ret)    ((ret) < -1)
#define MBCLEN_NEEDMORE_LEN(ret)  (-1 - (ret))
unsigned int rb_enc_codepoint_len(const char *p, const char *e, int *len, rb_encoding *enc);
#define rb_enc_codepoint(p, e, enc) rb_enc_codepoint_len((p), (e), NULL, (enc))
int   rb_enc_codelen(int code, rb_encoding *enc);
int   rb_enc_mbcput(unsigned int c, void *buf, rb_encoding *enc);
#define rb_enc_mbcput(c, buf, enc) rb_enc_mbcput((unsigned int)(c), (buf), (enc))
VALUE rb_enc_uint_chr(unsigned int code, rb_encoding *enc);
char *rb_enc_right_char_head(const char *s, const char *p, const char *e, rb_encoding *enc);
char *rb_enc_left_char_head(const char *s, const char *p, const char *e, rb_encoding *enc);
char *rb_enc_prev_char(const char *s, const char *p, const char *e, rb_encoding *enc);
#define rb_enc_is_newline(p, e, enc) ((p) < (e) && *(p) == '\n')
#define rb_enc_isascii(c, enc) rb_isascii(c)
#define rb_enc_isspace(c, enc) rb_isspace(c)
#define rb_enc_isprint(c, enc) rb_isprint(c)
#define rb_enc_isalpha(c, enc) rb_isalpha(c)
#define rb_enc_isdigit(c, enc) rb_isdigit(c)
#define rb_enc_isalnum(c, enc) rb_isalnum(c)
#define rb_enc_isupper(c, enc) rb_isupper(c)
#define rb_enc_islower(c, enc) rb_islower(c)
#define rb_enc_toupper(c, enc) rb_toupper(c)
#define rb_enc_tolower(c, enc) rb_tolower(c)
rb_encoding *rb_default_external_encoding(void);
rb_encoding *rb_default_internal_encoding(void);
rb_encoding *rb_locale_encoding(void);
rb_encoding *rb_filesystem_encoding(void);
VALUE rb_enc_default_external(void);
VALUE rb_enc_default_internal(void);
void  rb_enc_copy(VALUE dst, VALUE src);
void  rb_enc_set_index(VALUE obj, int idx);
rb_encoding *rb_enc_check(VALUE str1, VALUE str2);
rb_encoding *rb_enc_compatible(VALUE str1, VALUE str2);
VALUE rb_enc_str_new_cstr(const char *ptr, rb_encoding *enc);
VALUE rb_enc_interned_str(const char *ptr, long len, rb_encoding *enc);
VALUE rb_enc_interned_str_cstr(const char *ptr, rb_encoding *enc);
VALUE rb_interned_str(const char *ptr, long len);
VALUE rb_interned_str_cstr(const char *ptr);
#define rb_enc_str_new_static(ptr, len, enc) rb_enc_str_new((ptr), (len), (enc))
#define rb_enc_str_new_literal(s, enc) rb_enc_str_new((s), (long)(sizeof(s) - 1), (enc))
VALUE rb_enc_str_buf_cat(VALUE str, const char *ptr, long len, rb_encoding *enc);
VALUE rb_external_str_new_with_enc(const char *ptr, long len, rb_encoding *enc);
int   rb_enc_str_asciionly_p(VALUE str);
VALUE rb_obj_encoding(VALUE obj);
VALUE rb_str_encode(VALUE str, VALUE to, int ecflags, VALUE ecopts);
VALUE rb_str_conv_enc(VALUE str, rb_encoding *from, rb_encoding *to);
VALUE rb_str_export_to_enc(VALUE str, rb_encoding *enc);
#define rb_enc_raise(enc, klass, ...) rb_raise((klass), __VA_ARGS__)
/* coderange: computed on each call (koruby keeps no cache), so SET / CLEAR do nothing */
#define RUBY_ENC_CODERANGE_UNKNOWN 0
#define RUBY_ENC_CODERANGE_7BIT    0x100000
#define RUBY_ENC_CODERANGE_VALID   0x200000
#define RUBY_ENC_CODERANGE_BROKEN  0x300000
#define ENC_CODERANGE_UNKNOWN RUBY_ENC_CODERANGE_UNKNOWN
#define ENC_CODERANGE_7BIT    RUBY_ENC_CODERANGE_7BIT
#define ENC_CODERANGE_VALID   RUBY_ENC_CODERANGE_VALID
#define ENC_CODERANGE_BROKEN  RUBY_ENC_CODERANGE_BROKEN
int   rb_enc_str_coderange(VALUE str);
#define RB_ENC_CODERANGE(obj) rb_enc_str_coderange(obj)
#define ENC_CODERANGE(obj) rb_enc_str_coderange(obj)
#define ENC_CODERANGE_ASCIIONLY(obj) (rb_enc_str_coderange(obj) == ENC_CODERANGE_7BIT)
#define RB_ENC_CODERANGE_CLEAN_P(cr) ((cr) == ENC_CODERANGE_7BIT || (cr) == ENC_CODERANGE_VALID)
#define ENC_CODERANGE_CLEAN_P(cr) RB_ENC_CODERANGE_CLEAN_P(cr)
#define ENC_CODERANGE_SET(obj, cr) ((void)(obj), (void)(cr))
#define RB_ENC_CODERANGE_SET(obj, cr) ENC_CODERANGE_SET(obj, cr)
#define ENC_CODERANGE_CLEAR(obj) ((void)(obj))
#define ENCODING_GET(obj) rb_enc_get_index(obj)
#define ENCODING_GET_INLINED(obj) rb_enc_get_index(obj)
#define RB_ENCODING_GET(obj) rb_enc_get_index(obj)
#define RB_ENCODING_GET_INLINED(obj) rb_enc_get_index(obj)
#define ENCODING_SET(obj, i) rb_enc_set_index((obj), (i))
#define RB_ENCODING_SET(obj, i) rb_enc_set_index((obj), (i))
#define ENCODING_SET_INLINED(obj, i) rb_enc_set_index((obj), (i))
#define ENCODING_IS_ASCII8BIT(obj) (rb_enc_get_index(obj) == rb_ascii8bit_encindex())
#define ENCODING_MAXNAMELEN 42
#define ENC_CODERANGE_AND(a, b) ((a) == ENC_CODERANGE_7BIT ? (b) : (a) != ENC_CODERANGE_VALID ? ENC_CODERANGE_UNKNOWN : (b) == ENC_CODERANGE_7BIT ? ENC_CODERANGE_VALID : (b))

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

/* ---- st_table (CRuby's C-level hash table; insertion-ordered like CRuby's) ---- */
struct st_hash_type { int (*compare)(st_data_t, st_data_t); st_index_t (*hash)(st_data_t); };
struct st_table_entry;
typedef struct st_table {
    const struct st_hash_type *type;
    st_index_t num_entries;                 /* read by extensions */
    st_index_t entries_bound, entries_capa, bins_mask;
    struct st_table_entry *entries;
    st_index_t *bins;                       /* entry index + 1; 0 = empty */
} st_table;
typedef int st_foreach_callback_func(st_data_t key, st_data_t val, st_data_t arg);
typedef int st_update_callback_func(st_data_t *key, st_data_t *value, st_data_t arg, int existing);
#define st_init_table rb_st_init_table
#define st_init_table_with_size rb_st_init_table_with_size
#define st_init_numtable rb_st_init_numtable
#define st_init_numtable_with_size rb_st_init_numtable_with_size
#define st_init_strtable rb_st_init_strtable
#define st_init_strtable_with_size rb_st_init_strtable_with_size
#define st_init_strcasetable rb_st_init_strcasetable
#define st_init_strcasetable_with_size rb_st_init_strcasetable_with_size
#define st_free_table rb_st_free_table
#define st_clear rb_st_clear
#define st_lookup rb_st_lookup
#define st_get_key rb_st_get_key
#define st_insert rb_st_insert
#define st_insert2 rb_st_insert2
#define st_add_direct rb_st_add_direct
#define st_delete rb_st_delete
#define st_update rb_st_update
#define st_foreach rb_st_foreach
#define st_foreach_safe rb_st_foreach_safe
#define st_foreach_check rb_st_foreach_check
#define st_copy rb_st_copy
#define st_memsize rb_st_memsize
#define st_numcmp rb_st_numcmp
#define st_numhash rb_st_numhash
#define st_hash rb_st_hash
#define st_strcasecmp rb_st_locale_insensitive_strcasecmp
#define st_strncasecmp rb_st_locale_insensitive_strncasecmp
st_table *rb_st_init_table(const struct st_hash_type *type);
st_table *rb_st_init_table_with_size(const struct st_hash_type *type, st_index_t size);
st_table *rb_st_init_numtable(void);
st_table *rb_st_init_numtable_with_size(st_index_t size);
st_table *rb_st_init_strtable(void);
st_table *rb_st_init_strtable_with_size(st_index_t size);
st_table *rb_st_init_strcasetable(void);
st_table *rb_st_init_strcasetable_with_size(st_index_t size);
void  rb_st_free_table(st_table *tab);
void  rb_st_clear(st_table *tab);
int   rb_st_lookup(st_table *tab, st_data_t key, st_data_t *value);
int   rb_st_get_key(st_table *tab, st_data_t key, st_data_t *result);
int   rb_st_insert(st_table *tab, st_data_t key, st_data_t value);
int   rb_st_insert2(st_table *tab, st_data_t key, st_data_t value, st_data_t (*func)(st_data_t));
void  rb_st_add_direct(st_table *tab, st_data_t key, st_data_t value);
int   rb_st_delete(st_table *tab, st_data_t *key, st_data_t *value);
int   rb_st_update(st_table *tab, st_data_t key, st_update_callback_func *func, st_data_t arg);
int   rb_st_foreach(st_table *tab, st_foreach_callback_func *func, st_data_t arg);
void  rb_st_foreach_safe(st_table *tab, st_foreach_callback_func *func, st_data_t arg);
int   rb_st_foreach_check(st_table *tab, st_foreach_callback_func *func, st_data_t arg, st_data_t never);
st_table *rb_st_copy(st_table *tab);
size_t rb_st_memsize(const st_table *tab);
int   rb_st_numcmp(st_data_t a, st_data_t b);
st_index_t rb_st_numhash(st_data_t n);
st_index_t rb_st_hash(const void *ptr, size_t len, st_index_t h);
void  rb_mark_tbl(st_table *tab);
void  rb_mark_set(st_table *tab);
void  rb_mark_hash(st_table *tab);
#define rb_mark_tbl_no_pin(tab) rb_mark_tbl(tab)
char *ruby_strdup(const char *str);
VALUE rb_utf8_str_new_static(const char *ptr, long len);
VALUE rb_str_split(VALUE str, const char *sep);
VALUE rb_range_new(VALUE beg, VALUE end, int exclude_end);
int   rb_range_values(VALUE range, VALUE *begp, VALUE *endp, int *exclp);
VALUE rb_range_beg_len(VALUE range, long *begp, long *lenp, long len, int err);
void  rb_hash_set_ifnone(VALUE hash, VALUE ifnone);
#define RHASH_SET_IFNONE(h, v) rb_hash_set_ifnone((h), (v))
VALUE rb_assoc_new(VALUE a, VALUE b);
VALUE rb_ary_aref(int argc, const VALUE *argv, VALUE ary);

/* ---- objects / classes -------------------------------------------------- */
RUBY_EXTERN VALUE rb_mKernel, rb_mComparable, rb_mEnumerable, rb_mMath, rb_mProcess, rb_mGC;
RUBY_EXTERN VALUE rb_cBasicObject, rb_cObject, rb_cModule, rb_cClass, rb_cString, rb_cSymbol,
                  rb_cArray, rb_cHash, rb_cInteger, rb_cFloat, rb_cNumeric, rb_cNilClass,
                  rb_cTrueClass, rb_cFalseClass, rb_cProc, rb_cMethod, rb_cRange, rb_cIO, rb_cFile,
                  rb_cTime, rb_cStruct, rb_cRational, rb_cComplex, rb_cRegexp, rb_cMatch,
                  rb_cEncoding, rb_cData, rb_cThread, rb_cRandom;
RUBY_EXTERN VALUE rb_eIOTimeoutError, rb_mWaitReadable, rb_mWaitWritable;
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
#define RB_OBJ_FROZEN_RAW(v) OBJ_FROZEN(v)
#define OBJ_FROZEN_RAW(v) OBJ_FROZEN(v)
VALUE rb_obj_dup(VALUE obj);
VALUE rb_obj_clone(VALUE obj);
VALUE rb_obj_id(VALUE obj);
VALUE rb_inspect(VALUE obj);
VALUE rb_obj_as_string(VALUE obj);
VALUE rb_any_to_s(VALUE obj);
VALUE rb_equal(VALUE a, VALUE b);
VALUE rb_eql(VALUE a, VALUE b);
int   rb_respond_to(VALUE obj, ID mid);
void  rb_attr(VALUE klass, ID id, int read, int write, int ex);
VALUE rb_enumeratorize(VALUE obj, VALUE meth, int argc, const VALUE *argv);
#define RETURN_ENUMERATOR(obj, argc, argv) do { if (!rb_block_given_p()) \
    return rb_enumeratorize((obj), ID2SYM(rb_frame_this_func()), (argc), (argv)); } while (0)
#define RETURN_SIZED_ENUMERATOR(obj, argc, argv, size_fn) RETURN_ENUMERATOR((obj), (argc), (argv))
ID    rb_frame_this_func(void);
int   rb_during_gc(void);
VALUE rb_ruby_verbose_get(void);
#define ruby_verbose rb_ruby_verbose_get()
VALUE rb_ruby_debug_get(void);
#define ruby_debug rb_ruby_debug_get()
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
VALUE rb_call_super_kw(int argc, const VALUE *argv, int kw_splat);
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
typedef enum { RB_WARN_CATEGORY_NONE, RB_WARN_CATEGORY_DEPRECATED, RB_WARN_CATEGORY_EXPERIMENTAL,
               RB_WARN_CATEGORY_PERFORMANCE, RB_WARN_CATEGORY_STRICT_UNUSED_BLOCK } rb_warning_category_t;
#define RB_WARN_CATEGORY_DEPRECATED RB_WARN_CATEGORY_DEPRECATED   /* extconf's have_macro probe */
void  rb_category_warn(rb_warning_category_t category, const char *fmt, ...);
int   rb_opts_exception_p(VALUE opts, int default_value);
VALUE rb_exc_new(VALUE klass, const char *ptr, long len);
VALUE rb_exc_new_cstr(VALUE klass, const char *ptr);
VALUE rb_exc_new_str(VALUE klass, VALUE str);
VALUE rb_syserr_new(int err, const char *msg);
NORETURN(void rb_eof_error(void));
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

/* ---- IO: enough for extensions that hand a socket's fd to a C library ---- */
typedef struct rb_io { VALUE self; int fd; int mode; } rb_io_t;
rb_io_t *rb_io_get_fptr(VALUE io);
#define GetOpenFile(obj, fp) rb_io_check_closed((fp) = rb_io_get_fptr(obj))   /* fileno raises on a closed IO */
#define RB_IO_POINTER(obj, fp) GetOpenFile((obj), (fp))
void  rb_io_check_readable(rb_io_t *fptr);
void  rb_io_check_writable(rb_io_t *fptr);
void  rb_io_check_closed(rb_io_t *fptr);
void  rb_io_set_nonblock(rb_io_t *fptr);
int   rb_io_descriptor(VALUE io);
enum rb_io_event { RUBY_IO_READABLE = 1, RUBY_IO_PRIORITY = 2, RUBY_IO_WRITABLE = 4 };
#define RUBY_IO_TIMEOUT_DEFAULT Qnil
VALUE rb_io_wait(VALUE io, VALUE events, VALUE timeout);
VALUE rb_io_maybe_wait(int error, VALUE io, VALUE events, VALUE timeout);
VALUE rb_io_timeout(VALUE io);
int   rb_thread_wait_fd(int fd);
int   rb_thread_fd_writable(int fd);

/* ---- ractor-local storage (one ractor: plain per-key slots) ---- */
typedef struct rb_ractor_local_key_struct *rb_ractor_local_key_t;
struct rb_ractor_local_storage_type { void (*mark)(void *ptr); void (*free)(void *ptr); };
rb_ractor_local_key_t rb_ractor_local_storage_ptr_newkey(const struct rb_ractor_local_storage_type *type);
void *rb_ractor_local_storage_ptr(rb_ractor_local_key_t key);
void  rb_ractor_local_storage_ptr_set(rb_ractor_local_key_t key, void *ptr);
rb_ractor_local_key_t rb_ractor_local_storage_value_newkey(void);
VALUE rb_ractor_local_storage_value(rb_ractor_local_key_t key);
void  rb_ractor_local_storage_value_set(rb_ractor_local_key_t key, VALUE val);
#define InitVM(ext) {void InitVM_##ext(void); InitVM_##ext();}

/* ---- numerics ---- */
static inline VALUE rb_special_const_p(VALUE v) { return RB_SPECIAL_CONST_P(v) ? Qtrue : Qfalse; }
#define RB_OBJ_FREEZE(v) rb_obj_freeze(v)
short rb_num2short(VALUE v);
unsigned short rb_num2ushort(VALUE v);
#define NUM2SHORT(v)  rb_num2short(v)
#define NUM2USHORT(v) rb_num2ushort(v)
#define FIX2SHORT(v)  rb_num2short(v)
VALUE rb_to_int(VALUE v);
VALUE rb_num_coerce_bin(VALUE x, VALUE y, ID op);
VALUE rb_num_coerce_cmp(VALUE x, VALUE y, ID op);
VALUE rb_num_coerce_relop(VALUE x, VALUE y, ID op);
NORETURN(void rb_cmperr(VALUE x, VALUE y));
VALUE rb_Rational(VALUE num, VALUE den);
#define rb_Rational1(x) rb_Rational((x), INT2FIX(1))
#define rb_Rational2(x, y) rb_Rational((x), (y))
VALUE rb_rational_new(VALUE num, VALUE den);
#define rb_rational_new1(x) rb_rational_new((x), INT2FIX(1))
#define rb_rational_new2(x, y) rb_rational_new((x), (y))
VALUE rb_rational_num(VALUE rat);
VALUE rb_rational_den(VALUE rat);
VALUE rb_Complex(VALUE real, VALUE imag);
VALUE rb_complex_real(VALUE z);
VALUE rb_complex_imag(VALUE z);
int   rb_big_sign(VALUE b);                  /* 1: >= 0, 0: negative (CRuby's BIGNUM_SIGN) */
#define RBIGNUM_SIGN(b)       rb_big_sign(b)
#define RBIGNUM_POSITIVE_P(b) (rb_big_sign(b) != 0)
#define RBIGNUM_NEGATIVE_P(b) (rb_big_sign(b) == 0)
VALUE rb_big_cmp(VALUE x, VALUE y);
VALUE rb_big2str(VALUE x, int base);

/* ---- structs, hashes, misc objects ---- */
VALUE rb_struct_define(const char *name, ...);
VALUE rb_struct_define_under(VALUE outer, const char *name, ...);
VALUE rb_struct_new(VALUE klass, ...);
VALUE rb_struct_aref(VALUE st, VALUE idx);
VALUE rb_struct_aset(VALUE st, VALUE idx, VALUE val);
VALUE rb_struct_size(VALUE st);
VALUE rb_struct_members(VALUE st);
VALUE rb_struct_s_members(VALUE klass);
VALUE rb_struct_getmember(VALUE st, ID id);
#define RSTRUCT_GET(st, idx) rb_struct_aref((st), INT2NUM(idx))
#define RSTRUCT_SET(st, idx, v) rb_struct_aset((st), INT2NUM(idx), (v))
#define RSTRUCT_LEN(st) NUM2LONG(rb_struct_size(st))
VALUE rb_hash_dup(VALUE hash);
VALUE rb_hash_freeze(VALUE hash);
VALUE rb_ary_freeze(VALUE ary);
VALUE rb_str_replace(VALUE str, VALUE other);
VALUE rb_str_new_shared(VALUE str);
VALUE rb_str_format(int argc, const VALUE *argv, VALUE fmt);
VALUE rb_obj_hide(VALUE obj);
VALUE rb_obj_reveal(VALUE obj, VALUE klass);
void  rb_copy_generic_ivar(VALUE clone, VALUE obj);
VALUE rb_hash(VALUE obj);
int   rb_cmpint(VALUE val, VALUE a, VALUE b);
int   rb_proc_arity(VALUE proc);
VALUE rb_marshal_load(VALUE port);
VALUE rb_marshal_dump(VALUE obj, VALUE port);
VALUE rb_cstr2inum(const char *str, int base);
VALUE rb_str2inum(VALUE str, int base);
double rb_cstr_to_dbl(const char *p, int badcheck);
double rb_str_to_dbl(VALUE str, int badcheck);
VALUE rb_int_positive_pow(long x, unsigned long y);
extern const signed char ruby_digit36_to_number_table[];
unsigned long ruby_scan_digits(const char *str, ssize_t len, int base, size_t *retlen, int *overflow);
long long rb_big2ll(VALUE x);
unsigned long long rb_big2ull(VALUE x);
long  rb_big2long(VALUE x);
unsigned long rb_big2ulong(VALUE x);
double rb_big2dbl(VALUE x);
VALUE rb_io_write(VALUE io, VALUE str);
VALUE rb_io_flush(VALUE io);
#define rb_usascii_str_new2(s) rb_usascii_str_new_cstr(s)
#define STRTOUL(str, endptr, base) strtoul((str), (endptr), (base))
#define ruby_strtoul(str, endptr, base) strtoul((str), (endptr), (base))
#define DECIMAL_SIZE_OF_BITS(n) (((n) * 3010 + 9998) / 9999)
#define RB_EXT_RACTOR_SAFE(f) rb_ext_ractor_safe(f)
#define RB_SCAN_ARGS_PASS_CALLED_KEYWORDS 0
#define RB_SCAN_ARGS_KEYWORDS 1
#define RB_SCAN_ARGS_LAST_HASH_KEYWORDS 3
/* CRuby's config.h: the system types under rb_ names, and their conversions */
typedef pid_t rb_pid_t;
typedef uid_t rb_uid_t;
typedef gid_t rb_gid_t;
typedef time_t rb_time_t;
typedef mode_t rb_mode_t;
typedef off_t rb_off_t;
typedef dev_t rb_dev_t;
#define PIDT2NUM(v) LONG2NUM(v)
#define NUM2PIDT(v) ((pid_t)NUM2LONG(v))
#define UIDT2NUM(v) ULONG2NUM(v)
#define NUM2UIDT(v) ((uid_t)NUM2ULONG(v))
#define GIDT2NUM(v) ULONG2NUM(v)
#define NUM2GIDT(v) ((gid_t)NUM2ULONG(v))
#define MODET2NUM(v) UINT2NUM(v)
#define NUM2MODET(v) ((mode_t)NUM2UINT(v))
#define DEVT2NUM(v) ULONG2NUM(v)
#define NUM2DEVT(v) ((dev_t)NUM2ULONG(v))
VALUE rb_ary_delete(VALUE ary, VALUE item);
VALUE rb_apply(VALUE recv, ID mid, VALUE args);
/* ---- strings, threads ---- */
#define STRINGIZE0(expr) #expr
#define STRINGIZE(expr) STRINGIZE0(expr)
#define rb_strlen_lit(str) (sizeof(str "") - 1)
VALUE rb_ary_subseq(VALUE ary, long beg, long len);
rb_encoding *rb_enc_get(VALUE obj);
int   rb_enc_str_asciicompat_p(VALUE str);
NORETURN(void ruby_malloc_size_overflow(size_t count, size_t elsize));
VALUE rb_str_subseq(VALUE str, long beg, long len);
#define rb_str_tmp_new(len) rb_str_new(NULL, (len))
void  rb_must_asciicompat(VALUE str);
VALUE rb_thread_local_aref(VALUE thread, ID id);
VALUE rb_thread_local_aset(VALUE thread, ID id, VALUE val);

/* ---- locale-independent ctype (CRuby's ruby/internal/ctype.h) ---- */
static inline int rb_isascii(int c) { return '\0' <= c && c <= '\x7f'; }
static inline int rb_isupper(int c) { return 'A' <= c && c <= 'Z'; }
static inline int rb_islower(int c) { return 'a' <= c && c <= 'z'; }
static inline int rb_isalpha(int c) { return rb_isupper(c) || rb_islower(c); }
static inline int rb_isdigit(int c) { return '0' <= c && c <= '9'; }
static inline int rb_isalnum(int c) { return rb_isalpha(c) || rb_isdigit(c); }
static inline int rb_isxdigit(int c) { return rb_isdigit(c) || ('A' <= c && c <= 'F') || ('a' <= c && c <= 'f'); }
static inline int rb_isblank(int c) { return c == ' ' || c == '\t'; }
static inline int rb_isspace(int c) { return c == ' ' || ('\t' <= c && c <= '\r'); }
static inline int rb_iscntrl(int c) { return ('\0' <= c && c < ' ') || c == '\x7f'; }
static inline int rb_isprint(int c) { return ' ' <= c && c <= '\x7e'; }
static inline int rb_isgraph(int c) { return '!' <= c && c <= '\x7e'; }
static inline int rb_ispunct(int c) { return rb_isgraph(c) && !rb_isalnum(c); }
static inline int rb_tolower(int c) { return rb_isupper(c) ? (c | 0x20) : c; }
static inline int rb_toupper(int c) { return rb_islower(c) ? (c & 0x5f) : c; }
#define ISASCII(c)  rb_isascii(c)
#define ISPRINT(c)  rb_isprint(c)
#define ISGRAPH(c)  rb_isgraph(c)
#define ISSPACE(c)  rb_isspace(c)
#define ISUPPER(c)  rb_isupper(c)
#define ISLOWER(c)  rb_islower(c)
#define ISALNUM(c)  rb_isalnum(c)
#define ISALPHA(c)  rb_isalpha(c)
#define ISDIGIT(c)  rb_isdigit(c)
#define ISXDIGIT(c) rb_isxdigit(c)
#define ISBLANK(c)  rb_isblank(c)
#define ISCNTRL(c)  rb_iscntrl(c)
#define ISPUNCT(c)  rb_ispunct(c)
#define TOUPPER(c)  rb_toupper(c)
#define TOLOWER(c)  rb_tolower(c)
/* ASCII-only, as CRuby's st_locale_insensitive_str(n)casecmp (date does #define strncasecmp STRNCASECMP) */
static inline int
rb_st_locale_insensitive_strncasecmp(const char *s1, const char *s2, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const int a = rb_tolower((unsigned char)s1[i]), b = rb_tolower((unsigned char)s2[i]);
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
    }
    return 0;
}
static inline int rb_st_locale_insensitive_strcasecmp(const char *s1, const char *s2) { return rb_st_locale_insensitive_strncasecmp(s1, s2, (size_t)-1); }
#define st_locale_insensitive_strncasecmp rb_st_locale_insensitive_strncasecmp
#define st_locale_insensitive_strcasecmp rb_st_locale_insensitive_strcasecmp
#define STRNCASECMP(s1, s2, n) rb_st_locale_insensitive_strncasecmp((s1), (s2), (n))
#define STRCASECMP(s1, s2) rb_st_locale_insensitive_strcasecmp((s1), (s2))

#ifdef __cplusplus
}
#endif
#endif
