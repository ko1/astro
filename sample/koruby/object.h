#ifndef KORUBY_OBJECT_H
#define KORUBY_OBJECT_H 1

#include "context.h"

/* heap object structures (CRuby-inspired) */

struct korb_object {
    struct RBasic basic;
    uint32_t ivar_cnt;
    uint32_t ivar_capa;
    VALUE *ivars;
};

struct korb_string {
    struct RBasic basic;
    char *ptr;
    long len;
    long capa;
};

struct korb_array {
    struct RBasic basic;
    VALUE *ptr;
    long len;
    long capa;
};

struct korb_hash_entry {
    VALUE key;
    VALUE value;
    uint64_t hash;
    struct korb_hash_entry *next; /* insertion order chain */
};

struct korb_hash {
    struct RBasic basic;
    struct korb_hash_entry **buckets;
    uint32_t bucket_cnt;
    uint32_t size;
    struct korb_hash_entry *first;  /* insertion order */
    struct korb_hash_entry *last;
    VALUE default_value;
    bool compare_by_identity;       /* keys compared by object identity */
};

struct korb_range {
    struct RBasic basic;
    VALUE begin;
    VALUE end;
    bool exclude_end;
};

struct korb_bignum {
    struct RBasic basic;
    void *mpz; /* mpz_t actually (mpz_struct[1]) */
};

struct korb_float {
    struct RBasic basic;
    double value;
};

struct korb_method {
    enum {
        KORB_METHOD_AST,
        KORB_METHOD_CFUNC,
    } type;
    ID name;
    struct korb_class *defining_class;
    struct korb_cref *def_cref;   /* lexical cref captured at def-time */
    union {
        struct {
            struct Node *body;
            uint32_t required_params_cnt;  /* mandatory pre params */
            uint32_t total_params_cnt;     /* required + optional + rest(0/1) */
            uint32_t locals_cnt;
            int rest_slot;                 /* -1 if no *rest */
        } ast;
        struct {
            VALUE (*func)(CTX *c, VALUE self, int argc, VALUE *argv);
            int argc; /* -1 for varargs */
        } cfunc;
    } u;
};

struct korb_method_table_entry {
    ID name;
    struct korb_method *method;
    struct korb_method_table_entry *next;
};

struct korb_method_table {
    struct korb_method_table_entry **buckets;
    uint32_t bucket_cnt;
    uint32_t size;
};

struct korb_const_entry {
    ID name;
    VALUE value;
    struct korb_const_entry *next;
};

struct korb_class {
    struct RBasic basic;       /* flags = T_CLASS or T_MODULE */
    enum korb_type instance_type; /* type of instances of this class */
    ID name;
    struct korb_class *super;
    struct korb_method_table methods;
    struct korb_const_entry *constants;
    /* ivar shape: name -> slot (linear table) */
    ID *ivar_names;
    uint32_t ivar_count;
    uint32_t ivar_capa;
};

struct korb_proc {
    struct RBasic basic;
    struct Node *body;
    VALUE *env;             /* shared/captured locals */
    uint32_t env_size;      /* slots covered by env (absolute high-water of body) */
    uint32_t params_cnt;
    uint32_t param_base;    /* absolute slot where block's params begin */
    VALUE self;
    bool is_lambda;
};

/* Method object: a bound (receiver, method) pair, callable via #call/#[] */
struct korb_method_obj {
    struct RBasic basic;
    VALUE receiver;
    ID name;
};

/* global VM */
struct korb_vm {
    state_serial_t method_serial;

    /* core classes */
    struct korb_class *object_class;
    struct korb_class *class_class;
    struct korb_class *module_class;
    struct korb_class *integer_class;
    struct korb_class *float_class;
    struct korb_class *string_class;
    struct korb_class *array_class;
    struct korb_class *hash_class;
    struct korb_class *symbol_class;
    struct korb_class *true_class;
    struct korb_class *false_class;
    struct korb_class *nil_class;
    struct korb_class *proc_class;
    struct korb_class *range_class;
    struct korb_class *kernel_module;
    struct korb_class *comparable_module;
    struct korb_class *enumerable_module;
    struct korb_class *numeric_class;
    struct korb_class *fiber_class;
    struct korb_class *method_class;

    /* globals */
    struct korb_method_table globals;

    /* topframe class (for top-level def, top-level constants) */
    struct korb_class *main_obj_class; /* the singleton-of-main-obj */
    VALUE main_obj;
};

extern struct korb_vm *korb_vm;

/* ---------- API ---------- */

void korb_runtime_init(void);

/* memory */
void *korb_xmalloc(size_t size);
void *korb_xmalloc_atomic(size_t size); /* no-pointer mem (e.g., string char buffer) */
void *korb_xcalloc(size_t n, size_t sz);
void *korb_xrealloc(void *p, size_t newsize);
void  korb_xfree(void *p);

/* ID */
ID korb_intern(const char *str);
ID korb_intern_n(const char *str, long len);
const char *korb_id_name(ID id);

/* class system */
VALUE korb_class_of(VALUE v);
struct korb_class *korb_class_of_class(VALUE v); /* returns C struct */
struct korb_class *korb_class_new(ID name, struct korb_class *super, enum korb_type instance_type);
struct korb_class *korb_module_new(ID name);
void korb_class_add_method_ast(struct korb_class *klass, ID name, struct Node *body, uint32_t params_cnt, uint32_t locals_cnt);
void korb_class_add_method_ast_full(struct korb_class *klass, ID name, struct Node *body,
                                    uint32_t required_params, uint32_t total_params,
                                    int rest_slot, uint32_t locals_cnt);
void korb_class_add_method_ast_full_cref(struct korb_class *klass, ID name, struct Node *body,
                                          uint32_t required_params, uint32_t total_params,
                                          int rest_slot, uint32_t locals_cnt,
                                          struct korb_cref *def_cref);
struct korb_cref *korb_cref_dup(struct korb_cref *src);
void korb_class_add_method_cfunc(struct korb_class *klass, ID name, VALUE (*func)(CTX *, VALUE, int, VALUE *), int argc);
struct korb_method *korb_class_find_method(const struct korb_class *klass, ID name);
void korb_module_include(struct korb_class *klass, struct korb_class *mod);
struct korb_class *korb_singleton_class_of(struct korb_class *klass);

/* constants */
void korb_const_set(struct korb_class *klass, ID name, VALUE value);
VALUE korb_const_get(struct korb_class *klass, ID name);
bool korb_const_has(struct korb_class *klass, ID name);

/* objects */
VALUE korb_object_new(struct korb_class *klass);
VALUE korb_ivar_get(VALUE obj, ID name);
void  korb_ivar_set(VALUE obj, ID name, VALUE value);

/* string */
VALUE korb_str_new(const char *p, long len);
VALUE korb_str_new_cstr(const char *cstr);
VALUE korb_str_dup(VALUE s);
VALUE korb_str_concat(VALUE a, VALUE b);
VALUE korb_str_inspect(VALUE s);
const char *korb_str_cstr(VALUE s); /* terminates */
long  korb_str_len(VALUE s);

/* array */
VALUE korb_ary_new_capa(long capa);
VALUE korb_ary_new(void);
VALUE korb_ary_new_from_values(long n, const VALUE *vals);
void  korb_ary_push(VALUE ary, VALUE v);
VALUE korb_ary_pop(VALUE ary);
VALUE korb_ary_aref(VALUE ary, long i);
void  korb_ary_aset(VALUE ary, long i, VALUE v);
long  korb_ary_len(VALUE ary);

/* hash */
VALUE korb_hash_new(void);
VALUE korb_hash_aref(VALUE h, VALUE key);
VALUE korb_hash_aset(VALUE h, VALUE key, VALUE val);
long  korb_hash_size(VALUE h);

/* symbol */
VALUE korb_id2sym(ID id);
ID    korb_sym2id(VALUE sym);
VALUE korb_str_to_sym(VALUE str);

/* float / bignum */
VALUE korb_float_new(double d);
double korb_num2dbl(VALUE v);
VALUE korb_bignum_new_str(const char *str, int base);
VALUE korb_bignum_new_long(long v);
VALUE korb_int_plus(VALUE a, VALUE b);
VALUE korb_int_minus(VALUE a, VALUE b);
VALUE korb_int_mul(VALUE a, VALUE b);
VALUE korb_int_div(VALUE a, VALUE b);
VALUE korb_int_mod(VALUE a, VALUE b);
VALUE korb_int_lshift(VALUE a, VALUE b);
VALUE korb_int_rshift(VALUE a, VALUE b);
VALUE korb_int_and(VALUE a, VALUE b);
VALUE korb_int_or(VALUE a, VALUE b);
VALUE korb_int_xor(VALUE a, VALUE b);
int   korb_int_cmp(VALUE a, VALUE b);
bool  korb_int_eq(VALUE a, VALUE b);

/* equality / inspect */
bool  korb_eq(VALUE a, VALUE b);
bool  korb_eql(VALUE a, VALUE b);
uint64_t korb_hash_value(VALUE v);
VALUE korb_inspect(VALUE v);
VALUE korb_to_s(VALUE v);
void  korb_p(VALUE v); /* writes to stdout with newline */

/* errors / exceptions */
VALUE korb_exc_new(struct korb_class *klass, const char *msg);
void  korb_raise(CTX *c, struct korb_class *klass, const char *fmt, ...);

/* method dispatch helper */
VALUE korb_funcall(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv);
VALUE korb_funcall_with_block(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv, VALUE block);
VALUE korb_dispatch_call(CTX *c, struct Node *callsite, VALUE recv, ID name, uint32_t argc, uint32_t arg_index, struct korb_proc *block, struct method_cache *mc);
VALUE korb_dispatch_binop(CTX *c, VALUE recv, ID name, int argc, VALUE *argv);
VALUE korb_yield(CTX *c, uint32_t argc, VALUE *argv);
bool korb_block_given(void);

/* gvar */
VALUE korb_gvar_get(ID name);
void  korb_gvar_set(ID name, VALUE v);

/* const lookup along current scope (uses CTX->current_class) */
VALUE korb_const_lookup(CTX *c, ID name);

/* range */
VALUE korb_range_new(VALUE begin, VALUE end, bool exclude_end);

/* proc */
VALUE korb_proc_new(struct Node *body, VALUE *fp, uint32_t env_size, uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda);

/* Builtins init */
void korb_init_builtins(void);

/* Fiber */
struct korb_fiber;
VALUE korb_fiber_new(struct korb_proc *block);
VALUE korb_fiber_resume(CTX *c, VALUE fib, int argc, VALUE *argv);
VALUE korb_fiber_yield(CTX *c, int argc, VALUE *argv);

/* file load (parse + eval) */
VALUE korb_load_file(CTX *c, const char *path);
VALUE korb_eval_string(CTX *c, const char *src, size_t len, const char *filename);

/* path resolution for require_relative */
char *korb_dirname(const char *path);
char *korb_join_path(const char *dir, const char *name);
bool korb_file_exists(const char *path);
char *korb_resolve_relative(const char *current_file, const char *name);


/* booleans */
#define KORB_BOOL(b) ((b) ? Qtrue : Qfalse)

/* object FLAGS access */
#define FL_USER_SHIFT 12
#define FL_USER(n)    ((VALUE)1 << (FL_USER_SHIFT + (n)))

/* well-known IDs */
extern ID id_initialize, id_to_s, id_inspect, id_call, id_each, id_new;
extern ID id_op_plus, id_op_minus, id_op_mul, id_op_div, id_op_mod;
extern ID id_op_eq, id_op_neq, id_op_lt, id_op_le, id_op_gt, id_op_ge;
extern ID id_op_aref, id_op_aset, id_op_lshift, id_op_rshift, id_op_and, id_op_or, id_op_xor;

#endif /* KORUBY_OBJECT_H */
