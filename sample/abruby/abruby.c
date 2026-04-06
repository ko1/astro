#include <ruby.h>
#include "node.h"
#include "context.h"
#include "builtin/builtin.h"

struct abruby_option OPTION = {
    .no_compiled_code = true,
    .record_all = false,
    .quiet = false,
};

static VALUE rb_cAbRuby;
static VALUE rb_cAbRubyNode;

// Built-in abruby classes (klass field = ab_class_class, set in init)

static struct abruby_class ab_module_class_body  = { .name = "Module" };
static struct abruby_class ab_class_class_body   = { .name = "Class", .super = &ab_module_class_body };
static struct abruby_class ab_object_class_body  = { .name = "Object" };
static struct abruby_class ab_array_class_body   = { .name = "Array", .super = &ab_object_class_body };
static struct abruby_class ab_hash_class_body    = { .name = "Hash",  .super = &ab_object_class_body };
static struct abruby_class ab_integer_class_body = { .name = "Integer", .super = &ab_object_class_body };
static struct abruby_class ab_string_class_body  = { .name = "String",  .super = &ab_object_class_body };
static struct abruby_class ab_true_class_body    = { .name = "TrueClass",  .super = &ab_object_class_body };
static struct abruby_class ab_false_class_body   = { .name = "FalseClass", .super = &ab_object_class_body };
static struct abruby_class ab_nil_class_body     = { .name = "NilClass",   .super = &ab_object_class_body };

struct abruby_class *ab_array_class   = &ab_array_class_body;
struct abruby_class *ab_hash_class    = &ab_hash_class_body;
struct abruby_class *ab_module_class  = &ab_module_class_body;
struct abruby_class *ab_class_class   = &ab_class_class_body;
struct abruby_class *ab_object_class  = &ab_object_class_body;
struct abruby_class *ab_integer_class = &ab_integer_class_body;
struct abruby_class *ab_string_class  = &ab_string_class_body;
struct abruby_class *ab_true_class    = &ab_true_class_body;
struct abruby_class *ab_false_class   = &ab_false_class_body;
struct abruby_class *ab_nil_class     = &ab_nil_class_body;

// Unified T_DATA type for all abruby heap objects

static void abruby_data_mark(void *ptr) {
    struct abruby_header *h = (struct abruby_header *)ptr;
    if (h->klass == ab_string_class) {
        rb_gc_mark(((struct abruby_string *)ptr)->rb_str);
    }
    else if (h->klass == ab_array_class) {
        rb_gc_mark(((struct abruby_array *)ptr)->rb_ary);
    }
    else if (h->klass == ab_hash_class) {
        rb_gc_mark(((struct abruby_hash *)ptr)->rb_hash);
    }
    else if (h->klass != ab_class_class) {
        // user object: mark ivars
        struct abruby_object *obj = (struct abruby_object *)ptr;
        for (unsigned int i = 0; i < obj->ivar_cnt; i++) {
            rb_gc_mark(obj->ivars[i].value);
        }
    }
}

const rb_data_type_t abruby_data_type = {
    "AbRuby::Data",
    { abruby_data_mark, NULL, NULL },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

// Object creation

VALUE
abruby_new_object(struct abruby_class *klass)
{
    struct abruby_object *obj = calloc(1, sizeof(struct abruby_object));
    obj->klass = klass;
    return TypedData_Wrap_Struct(rb_cAbRubyNode, &abruby_data_type, obj);
}

// String helpers

VALUE
abruby_str_new(VALUE rb_str)
{
    struct abruby_string *s = calloc(1, sizeof(struct abruby_string));
    s->klass = ab_string_class;
    s->rb_str = rb_str;
    return TypedData_Wrap_Struct(rb_cAbRubyNode, &abruby_data_type, s);
}

VALUE
abruby_str_new_cstr(const char *str)
{
    return abruby_str_new(rb_str_new_cstr(str));
}

VALUE
abruby_str_rstr(VALUE ab_str)
{
    ab_verify(ab_str);
    return ((struct abruby_string *)RTYPEDDATA_GET_DATA(ab_str))->rb_str;
}

// shorthand macros
#define RSTR(v) abruby_str_rstr(v)
#define RARY(v) (((struct abruby_array *)RTYPEDDATA_GET_DATA(v))->rb_ary)
#define RHSH(v) (((struct abruby_hash *)RTYPEDDATA_GET_DATA(v))->rb_hash)

VALUE
abruby_ary_new(VALUE rb_ary)
{
    struct abruby_array *a = calloc(1, sizeof(struct abruby_array));
    a->klass = ab_array_class;
    a->rb_ary = rb_ary;
    return TypedData_Wrap_Struct(rb_cAbRubyNode, &abruby_data_type, a);
}

VALUE
abruby_hash_new_wrap(VALUE rb_hash)
{
    struct abruby_hash *h = calloc(1, sizeof(struct abruby_hash));
    h->klass = ab_hash_class;
    h->rb_hash = rb_hash;
    return TypedData_Wrap_Struct(rb_cAbRubyNode, &abruby_data_type, h);
}

// Class wrapper

VALUE
abruby_wrap_class(struct abruby_class *klass)
{
    if (klass->rb_wrapper) return klass->rb_wrapper;
    VALUE obj = TypedData_Wrap_Struct(rb_cAbRubyNode, &abruby_data_type, klass);
    klass->rb_wrapper = obj;
    rb_gc_register_mark_object(obj);
    return obj;
}

struct abruby_class *
abruby_unwrap_class(VALUE obj)
{
    ab_verify(obj);
    return (struct abruby_class *)RTYPEDDATA_GET_DATA(obj);
}

// === Shared helpers and builtin infrastructure ===

VALUE
ab_inspect_rstr(CTX *c, VALUE v) {
    struct abruby_method *ins = abruby_find_method(AB_CLASS_OF(v), "inspect");
    VALUE ab_str;
    if (ins->type == ABRUBY_METHOD_CFUNC) {
        ab_str = ins->u.cfunc.func(c, v, 0, NULL);
    } else {
        // AST inspect: call in a separate frame
        VALUE *save_fp = c->fp;
        VALUE save_self = c->self;
        c->fp = save_fp + 16; // generous offset
        c->self = v;
        ab_str = EVAL(c, ins->u.ast.body);
        c->fp = save_fp;
        c->self = save_self;
    }
    return RSTR(ab_str);
}

void
abruby_class_add_cfunc(struct abruby_class *klass, const char *name,
                       abruby_cfunc_t func, unsigned int params_cnt)
{
    struct abruby_method *m = &klass->methods[klass->method_cnt++];
    m->name = name;
    m->type = ABRUBY_METHOD_CFUNC;
    m->u.cfunc.func = func;
    m->u.cfunc.params_cnt = params_cnt;
}

static void
init_builtin_methods(void)
{
    Init_abruby_class();
    Init_abruby_object();
    Init_abruby_integer();
    Init_abruby_string();
    Init_abruby_array();
    Init_abruby_hash();
    Init_abruby_true();
    Init_abruby_false();
    Init_abruby_nil();
}

// ivar helpers

VALUE
abruby_ivar_get(VALUE self, const char *name)
{
    ab_verify(self);
    struct abruby_object *obj;
    TypedData_Get_Struct(self, struct abruby_object, &abruby_data_type, obj);
    for (unsigned int i = 0; i < obj->ivar_cnt; i++) {
        if (strcmp(obj->ivars[i].name, name) == 0) {
            return obj->ivars[i].value;
        }
    }
    return Qnil;
}

void
abruby_ivar_set(VALUE self, const char *name, VALUE val)
{
    ab_verify(self);
    if (!RB_TYPE_P(self, T_DATA)) {
        rb_raise(rb_eRuntimeError, "can't set instance variable on non-object");
    }
    struct abruby_object *obj;
    TypedData_Get_Struct(self, struct abruby_object, &abruby_data_type, obj);
    for (unsigned int i = 0; i < obj->ivar_cnt; i++) {
        if (strcmp(obj->ivars[i].name, name) == 0) {
            obj->ivars[i].value = val;
            return;
        }
    }
    if (obj->ivar_cnt >= ABRUBY_IVAR_MAX) {
        rb_raise(rb_eRuntimeError, "too many instance variables");
    }
    obj->ivars[obj->ivar_cnt].name = name;
    obj->ivars[obj->ivar_cnt].value = val;
    obj->ivar_cnt++;
}

// CTX

#define STACK_SIZE  10000

static CTX global_ctx;
static VALUE global_stack[STACK_SIZE];
static VALUE rb_ctx_wrapper;

static void
ctx_mark(void *ptr)
{
    (void)ptr;
    rb_gc_mark_locations(global_stack, global_stack + STACK_SIZE);
}

static const rb_data_type_t abruby_ctx_type = {
    "AbRuby::CTX",
    { ctx_mark, NULL, NULL },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static void
init_ctx(void)
{
    global_ctx.env = global_stack;
    global_ctx.fp = global_stack;
    global_ctx.self = Qnil;
    global_ctx.current_class = NULL;
    global_ctx.class_cnt = 0;

    for (int i = 0; i < STACK_SIZE; i++) {
        global_stack[i] = Qnil;
    }

    rb_ctx_wrapper = TypedData_Wrap_Struct(rb_cObject, &abruby_ctx_type, &global_ctx);
    rb_gc_register_mark_object(rb_ctx_wrapper);
}

// NODE wrapper (T_DATA)

const rb_data_type_t abruby_node_type = {
    "AbRuby::Node",
    { abruby_node_mark, RUBY_DEFAULT_FREE, NULL },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE
wrap_node(NODE *n)
{
    if (n == NULL) return Qnil;
    if (n->head.rb_wrapper) return n->head.rb_wrapper;

    VALUE obj = TypedData_Wrap_Struct(rb_cAbRubyNode, &abruby_node_type, n);
    n->head.rb_wrapper = obj;
    return obj;
}

static NODE *
unwrap_node(VALUE obj)
{
    if (NIL_P(obj)) return NULL;
    NODE *n;
    TypedData_Get_Struct(obj, NODE, &abruby_node_type, n);
    return n;
}

// ALLOC wrappers exposed to Ruby

static VALUE
rb_alloc_node_num(VALUE self, VALUE num)
{
    return wrap_node(ALLOC_node_num(FIX2INT(num)));
}

static VALUE
rb_alloc_node_bignum(VALUE self, VALUE str)
{
    const char *cstr = strdup(StringValueCStr(str));
    return wrap_node(ALLOC_node_bignum(cstr));
}

static VALUE
rb_alloc_node_str(VALUE self, VALUE str)
{
    const char *cstr = strdup(StringValueCStr(str));
    return wrap_node(ALLOC_node_str(cstr));
}

static VALUE
rb_alloc_node_true(VALUE self)
{
    return wrap_node(ALLOC_node_true());
}

static VALUE
rb_alloc_node_false(VALUE self)
{
    return wrap_node(ALLOC_node_false());
}

static VALUE
rb_alloc_node_nil(VALUE self)
{
    return wrap_node(ALLOC_node_nil());
}

static VALUE
rb_alloc_node_self(VALUE self)
{
    return wrap_node(ALLOC_node_self());
}

static VALUE
rb_alloc_node_lget(VALUE self, VALUE index)
{
    return wrap_node(ALLOC_node_lget(FIX2UINT(index)));
}

static VALUE
rb_alloc_node_lset(VALUE self, VALUE index, VALUE rhs)
{
    return wrap_node(ALLOC_node_lset(FIX2UINT(index), unwrap_node(rhs)));
}

static VALUE
rb_alloc_node_scope(VALUE self, VALUE envsize, VALUE body)
{
    return wrap_node(ALLOC_node_scope(FIX2UINT(envsize), unwrap_node(body)));
}

static VALUE
rb_alloc_node_seq(VALUE self, VALUE head, VALUE tail)
{
    return wrap_node(ALLOC_node_seq(unwrap_node(head), unwrap_node(tail)));
}

static VALUE
rb_alloc_node_if(VALUE self, VALUE cond, VALUE then_node, VALUE else_node)
{
    return wrap_node(ALLOC_node_if(unwrap_node(cond), unwrap_node(then_node), unwrap_node(else_node)));
}

static VALUE
rb_alloc_node_while(VALUE self, VALUE cond, VALUE body)
{
    return wrap_node(ALLOC_node_while(unwrap_node(cond), unwrap_node(body)));
}


static VALUE
rb_alloc_node_def(VALUE self, VALUE name, VALUE body, VALUE params_cnt, VALUE locals_cnt)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_def(cname, unwrap_node(body), FIX2UINT(params_cnt), FIX2UINT(locals_cnt)));
}


// ivar nodes

static VALUE
rb_alloc_node_ivar_get(VALUE self, VALUE name)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_ivar_get(cname));
}

static VALUE
rb_alloc_node_ivar_set(VALUE self, VALUE name, VALUE value)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_ivar_set(cname, unwrap_node(value)));
}

// OOP nodes

static VALUE
rb_alloc_node_module_def(VALUE self, VALUE name, VALUE body)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_module_def(cname, unwrap_node(body)));
}

static VALUE
rb_alloc_node_class_def(VALUE self, VALUE name, VALUE super_name, VALUE body)
{
    const char *cname = strdup(StringValueCStr(name));
    const char *csup = strdup(StringValueCStr(super_name));
    return wrap_node(ALLOC_node_class_def(cname, csup, unwrap_node(body)));
}

static VALUE
rb_alloc_node_array_new(VALUE self, VALUE argc, VALUE arg_index)
{
    return wrap_node(ALLOC_node_array_new(FIX2UINT(argc), FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_hash_new(VALUE self, VALUE argc, VALUE arg_index)
{
    return wrap_node(ALLOC_node_hash_new(FIX2UINT(argc), FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_const_get(VALUE self, VALUE name)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_const_get(cname));
}

static VALUE
rb_alloc_node_method_call(VALUE self, VALUE recv, VALUE name, VALUE params_cnt, VALUE arg_index)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_method_call(unwrap_node(recv), cname, FIX2UINT(params_cnt), FIX2UINT(arg_index)));
}

// dump

static VALUE
rb_abruby_dump_ast(VALUE self, VALUE ast_obj)
{
    NODE *ast = unwrap_node(ast_obj);
    if (ast == NULL) return rb_str_new_cstr("<NULL>");

    char *buf = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    if (fp == NULL) rb_raise(rb_eRuntimeError, "open_memstream failed");

    DUMP(fp, ast, true);
    fclose(fp);

    VALUE str = rb_str_new(buf, len);
    free(buf);
    return str;
}

// eval

// Convert abruby value to Ruby value for returning to CRuby world
static VALUE
abruby_to_ruby(VALUE v)
{
    if (RB_TYPE_P(v, T_DATA) && RTYPEDDATA_P(v) &&
        RTYPEDDATA_TYPE(v) == &abruby_data_type) {
        struct abruby_header *h = (struct abruby_header *)RTYPEDDATA_GET_DATA(v);
        if (h->klass == ab_string_class) {
            return ((struct abruby_string *)h)->rb_str;
        }
        if (h->klass == ab_array_class) {
            VALUE ary = ((struct abruby_array *)h)->rb_ary;
            long len = RARRAY_LEN(ary);
            VALUE result = rb_ary_new_capa(len);
            for (long i = 0; i < len; i++) {
                rb_ary_push(result, abruby_to_ruby(RARRAY_AREF(ary, i)));
            }
            return result;
        }
        if (h->klass == ab_hash_class) {
            VALUE hash = ((struct abruby_hash *)h)->rb_hash;
            VALUE keys = rb_funcall(hash, rb_intern("keys"), 0);
            long len = RARRAY_LEN(keys);
            VALUE result = rb_hash_new();
            for (long i = 0; i < len; i++) {
                VALUE k = RARRAY_AREF(keys, i);
                VALUE v = abruby_to_ruby(rb_hash_aref(hash, k));
                rb_hash_aset(result, k, v);
            }
            return result;
        }
    }
    return v;
}

static VALUE
rb_abruby_eval_ast(VALUE self, VALUE ast_obj)
{
    NODE *ast = unwrap_node(ast_obj);
    if (ast == NULL) {
        rb_raise(rb_eRuntimeError, "cannot eval NULL AST");
    }

    // reset context for each eval
    global_ctx.fp = global_ctx.env;
    global_ctx.self = Qnil;
    global_ctx.current_class = NULL;
    global_ctx.class_cnt = 0;

    VALUE result = EVAL(&global_ctx, ast);
    return abruby_to_ruby(result);
}

void
Init_abruby(void)
{
    INIT();
    init_builtin_methods();

    rb_cAbRuby = rb_define_class("AbRuby", rb_cObject);
    rb_cAbRubyNode = rb_define_class_under(rb_cAbRuby, "Node", rb_cObject);
    rb_undef_alloc_func(rb_cAbRubyNode);

    // Set klass field on all built-in classes (common header)
    ab_array_class->klass   = ab_class_class;
    ab_hash_class->klass    = ab_class_class;
    ab_module_class->klass  = ab_class_class;
    ab_class_class->klass   = ab_class_class;
    ab_object_class->klass  = ab_class_class;
    ab_integer_class->klass = ab_class_class;
    ab_string_class->klass  = ab_class_class;
    ab_true_class->klass    = ab_class_class;
    ab_false_class->klass   = ab_class_class;
    ab_nil_class->klass     = ab_class_class;

    // Wrap built-in classes as VALUE (must be after rb_cAbRubyNode is defined)
    abruby_wrap_class(ab_module_class);
    abruby_wrap_class(ab_class_class);
    abruby_wrap_class(ab_object_class);
    abruby_wrap_class(ab_array_class);
    abruby_wrap_class(ab_hash_class);
    abruby_wrap_class(ab_integer_class);
    abruby_wrap_class(ab_string_class);
    abruby_wrap_class(ab_true_class);
    abruby_wrap_class(ab_false_class);
    abruby_wrap_class(ab_nil_class);

    init_ctx();

    // ALLOC wrappers
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_num", rb_alloc_node_num, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_bignum", rb_alloc_node_bignum, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_str", rb_alloc_node_str, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_true", rb_alloc_node_true, 0);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_false", rb_alloc_node_false, 0);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_nil", rb_alloc_node_nil, 0);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_self", rb_alloc_node_self, 0);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_lget", rb_alloc_node_lget, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_lset", rb_alloc_node_lset, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_scope", rb_alloc_node_scope, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_seq", rb_alloc_node_seq, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_if", rb_alloc_node_if, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_while", rb_alloc_node_while, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_def", rb_alloc_node_def, 4);


    // ivar
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_ivar_get", rb_alloc_node_ivar_get, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_ivar_set", rb_alloc_node_ivar_set, 2);

    // Array / Hash
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_array_new", rb_alloc_node_array_new, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_hash_new", rb_alloc_node_hash_new, 2);

    // OOP
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_module_def", rb_alloc_node_module_def, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_class_def", rb_alloc_node_class_def, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_const_get", rb_alloc_node_const_get, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_method_call", rb_alloc_node_method_call, 4);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_self", rb_alloc_node_self, 0);

    // eval / dump
    rb_define_singleton_method(rb_cAbRuby, "eval_ast", rb_abruby_eval_ast, 1);
    rb_define_singleton_method(rb_cAbRuby, "dump_ast", rb_abruby_dump_ast, 1);
}
