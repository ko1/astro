#ifndef KORUBY_OBJECT_H
#define KORUBY_OBJECT_H 1

#include "context.h"

/* heap object structures (CRuby-inspired) */

struct ko_object {
    struct RBasic basic;
    uint32_t ivar_cnt;
    uint32_t ivar_capa;
    VALUE *ivars;
};

struct ko_string {
    struct RBasic basic;
    char *ptr;
    long len;
    long capa;
};

struct ko_array {
    struct RBasic basic;
    VALUE *ptr;
    long len;
    long capa;
};

struct ko_hash_entry {
    VALUE key;
    VALUE value;
    uint64_t hash;
    struct ko_hash_entry *next; /* insertion order chain */
};

struct ko_hash {
    struct RBasic basic;
    struct ko_hash_entry **buckets;
    uint32_t bucket_cnt;
    uint32_t size;
    struct ko_hash_entry *first;  /* insertion order */
    struct ko_hash_entry *last;
    VALUE default_value;
};

struct ko_range {
    struct RBasic basic;
    VALUE begin;
    VALUE end;
    bool exclude_end;
};

struct ko_bignum {
    struct RBasic basic;
    void *mpz; /* mpz_t actually (mpz_struct[1]) */
};

struct ko_float {
    struct RBasic basic;
    double value;
};

struct ko_method {
    enum {
        KO_METHOD_AST,
        KO_METHOD_CFUNC,
    } type;
    ID name;
    struct ko_class *defining_class;
    union {
        struct {
            struct Node *body;
            uint32_t required_params_cnt;
            uint32_t locals_cnt;
        } ast;
        struct {
            VALUE (*func)(CTX *c, VALUE self, int argc, VALUE *argv);
            int argc; /* -1 for varargs */
        } cfunc;
    } u;
};

struct ko_method_table_entry {
    ID name;
    struct ko_method *method;
    struct ko_method_table_entry *next;
};

struct ko_method_table {
    struct ko_method_table_entry **buckets;
    uint32_t bucket_cnt;
    uint32_t size;
};

struct ko_const_entry {
    ID name;
    VALUE value;
    struct ko_const_entry *next;
};

struct ko_class {
    struct RBasic basic;       /* flags = T_CLASS or T_MODULE */
    enum ko_type instance_type; /* type of instances of this class */
    ID name;
    struct ko_class *super;
    struct ko_method_table methods;
    struct ko_const_entry *constants;
    /* ivar shape: name -> slot (linear table) */
    ID *ivar_names;
    uint32_t ivar_count;
    uint32_t ivar_capa;
};

struct ko_proc {
    struct RBasic basic;
    struct Node *body;
    VALUE *env;             /* shared/captured locals */
    uint32_t env_size;      /* slots covered by env (absolute high-water of body) */
    uint32_t params_cnt;
    uint32_t param_base;    /* absolute slot where block's params begin */
    VALUE self;
    bool is_lambda;
};

/* global VM */
struct ko_vm {
    state_serial_t method_serial;

    /* core classes */
    struct ko_class *object_class;
    struct ko_class *class_class;
    struct ko_class *module_class;
    struct ko_class *integer_class;
    struct ko_class *float_class;
    struct ko_class *string_class;
    struct ko_class *array_class;
    struct ko_class *hash_class;
    struct ko_class *symbol_class;
    struct ko_class *true_class;
    struct ko_class *false_class;
    struct ko_class *nil_class;
    struct ko_class *proc_class;
    struct ko_class *range_class;
    struct ko_class *kernel_module;
    struct ko_class *comparable_module;
    struct ko_class *enumerable_module;
    struct ko_class *numeric_class;

    /* globals */
    struct ko_method_table globals;

    /* topframe class (for top-level def, top-level constants) */
    struct ko_class *main_obj_class; /* the singleton-of-main-obj */
    VALUE main_obj;
};

extern struct ko_vm *ko_vm;

/* ---------- API ---------- */

void ko_runtime_init(void);

/* memory */
void *ko_xmalloc(size_t size);
void *ko_xmalloc_atomic(size_t size); /* no-pointer mem (e.g., string char buffer) */
void *ko_xcalloc(size_t n, size_t sz);
void *ko_xrealloc(void *p, size_t newsize);
void  ko_xfree(void *p);

/* ID */
ID ko_intern(const char *str);
ID ko_intern_n(const char *str, long len);
const char *ko_id_name(ID id);

/* class system */
VALUE ko_class_of(VALUE v);
struct ko_class *ko_class_of_class(VALUE v); /* returns C struct */
struct ko_class *ko_class_new(ID name, struct ko_class *super, enum ko_type instance_type);
struct ko_class *ko_module_new(ID name);
void ko_class_add_method_ast(struct ko_class *klass, ID name, struct Node *body, uint32_t params_cnt, uint32_t locals_cnt);
void ko_class_add_method_cfunc(struct ko_class *klass, ID name, VALUE (*func)(CTX *, VALUE, int, VALUE *), int argc);
struct ko_method *ko_class_find_method(const struct ko_class *klass, ID name);

/* constants */
void ko_const_set(struct ko_class *klass, ID name, VALUE value);
VALUE ko_const_get(struct ko_class *klass, ID name);
bool ko_const_has(struct ko_class *klass, ID name);

/* objects */
VALUE ko_object_new(struct ko_class *klass);
VALUE ko_ivar_get(VALUE obj, ID name);
void  ko_ivar_set(VALUE obj, ID name, VALUE value);

/* string */
VALUE ko_str_new(const char *p, long len);
VALUE ko_str_new_cstr(const char *cstr);
VALUE ko_str_dup(VALUE s);
VALUE ko_str_concat(VALUE a, VALUE b);
VALUE ko_str_inspect(VALUE s);
const char *ko_str_cstr(VALUE s); /* terminates */
long  ko_str_len(VALUE s);

/* array */
VALUE ko_ary_new_capa(long capa);
VALUE ko_ary_new(void);
VALUE ko_ary_new_from_values(long n, const VALUE *vals);
void  ko_ary_push(VALUE ary, VALUE v);
VALUE ko_ary_pop(VALUE ary);
VALUE ko_ary_aref(VALUE ary, long i);
void  ko_ary_aset(VALUE ary, long i, VALUE v);
long  ko_ary_len(VALUE ary);

/* hash */
VALUE ko_hash_new(void);
VALUE ko_hash_aref(VALUE h, VALUE key);
VALUE ko_hash_aset(VALUE h, VALUE key, VALUE val);
long  ko_hash_size(VALUE h);

/* symbol */
VALUE ko_id2sym(ID id);
ID    ko_sym2id(VALUE sym);
VALUE ko_str_to_sym(VALUE str);

/* float / bignum */
VALUE ko_float_new(double d);
double ko_num2dbl(VALUE v);
VALUE ko_bignum_new_str(const char *str, int base);
VALUE ko_bignum_new_long(long v);
VALUE ko_int_plus(VALUE a, VALUE b);
VALUE ko_int_minus(VALUE a, VALUE b);
VALUE ko_int_mul(VALUE a, VALUE b);
VALUE ko_int_div(VALUE a, VALUE b);
VALUE ko_int_mod(VALUE a, VALUE b);
VALUE ko_int_lshift(VALUE a, VALUE b);
VALUE ko_int_rshift(VALUE a, VALUE b);
VALUE ko_int_and(VALUE a, VALUE b);
VALUE ko_int_or(VALUE a, VALUE b);
VALUE ko_int_xor(VALUE a, VALUE b);
int   ko_int_cmp(VALUE a, VALUE b);
bool  ko_int_eq(VALUE a, VALUE b);

/* equality / inspect */
bool  ko_eq(VALUE a, VALUE b);
bool  ko_eql(VALUE a, VALUE b);
uint64_t ko_hash_value(VALUE v);
VALUE ko_inspect(VALUE v);
VALUE ko_to_s(VALUE v);
void  ko_p(VALUE v); /* writes to stdout with newline */

/* errors / exceptions */
VALUE ko_exc_new(struct ko_class *klass, const char *msg);
void  ko_raise(CTX *c, struct ko_class *klass, const char *fmt, ...);

/* method dispatch helper */
VALUE ko_funcall(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv);
VALUE ko_funcall_with_block(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv, VALUE block);
VALUE ko_dispatch_call(CTX *c, struct Node *callsite, VALUE recv, ID name, uint32_t argc, uint32_t arg_index, struct ko_proc *block, struct method_cache *mc);
VALUE ko_dispatch_binop(CTX *c, VALUE recv, ID name, int argc, VALUE *argv);
VALUE ko_yield(CTX *c, uint32_t argc, VALUE *argv);

/* gvar */
VALUE ko_gvar_get(ID name);
void  ko_gvar_set(ID name, VALUE v);

/* const lookup along current scope (uses CTX->current_class) */
VALUE ko_const_lookup(CTX *c, ID name);

/* range */
VALUE ko_range_new(VALUE begin, VALUE end, bool exclude_end);

/* proc */
VALUE ko_proc_new(struct Node *body, VALUE *fp, uint32_t env_size, uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda);

/* Builtins init */
void ko_init_builtins(void);


/* booleans */
#define KO_BOOL(b) ((b) ? Qtrue : Qfalse)

/* object FLAGS access */
#define FL_USER_SHIFT 12
#define FL_USER(n)    ((VALUE)1 << (FL_USER_SHIFT + (n)))

/* well-known IDs */
extern ID id_initialize, id_to_s, id_inspect, id_call, id_each, id_new;
extern ID id_op_plus, id_op_minus, id_op_mul, id_op_div, id_op_mod;
extern ID id_op_eq, id_op_neq, id_op_lt, id_op_le, id_op_gt, id_op_ge;
extern ID id_op_aref, id_op_aset, id_op_lshift, id_op_rshift, id_op_and, id_op_or, id_op_xor;

#endif /* KORUBY_OBJECT_H */
