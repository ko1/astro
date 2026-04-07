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

static struct abruby_class ab_kernel_module_body = { .name = "Kernel" };
static struct abruby_class ab_module_class_body  = { .name = "Module" };
static struct abruby_class ab_class_class_body   = { .name = "Class", .super = &ab_module_class_body };
static struct abruby_class ab_object_class_body  = { .name = "Object" };
static struct abruby_class ab_float_class_body   = { .name = "Float",   .super = &ab_object_class_body };
static struct abruby_class ab_array_class_body   = { .name = "Array", .super = &ab_object_class_body };
static struct abruby_class ab_hash_class_body    = { .name = "Hash",  .super = &ab_object_class_body };
static struct abruby_class ab_integer_class_body = { .name = "Integer", .super = &ab_object_class_body };
static struct abruby_class ab_string_class_body  = { .name = "String",  .super = &ab_object_class_body };
static struct abruby_class ab_symbol_class_body  = { .name = "Symbol",  .super = &ab_object_class_body };
static struct abruby_class ab_range_class_body   = { .name = "Range",   .super = &ab_object_class_body };
static struct abruby_class ab_regexp_class_body  = { .name = "Regexp",  .super = &ab_object_class_body };
static struct abruby_class ab_rational_class_body = { .name = "Rational", .super = &ab_object_class_body };
static struct abruby_class ab_complex_class_body  = { .name = "Complex",  .super = &ab_object_class_body };
static struct abruby_class ab_true_class_body    = { .name = "TrueClass",  .super = &ab_object_class_body };
static struct abruby_class ab_false_class_body   = { .name = "FalseClass", .super = &ab_object_class_body };
static struct abruby_class ab_nil_class_body     = { .name = "NilClass",   .super = &ab_object_class_body };
static struct abruby_class ab_runtime_error_class_body = { .name = "RuntimeError", .super = &ab_object_class_body };

struct abruby_class *ab_float_class   = &ab_float_class_body;
struct abruby_class *ab_array_class   = &ab_array_class_body;
struct abruby_class *ab_hash_class    = &ab_hash_class_body;
struct abruby_class *ab_kernel_module = &ab_kernel_module_body;
struct abruby_class *ab_module_class  = &ab_module_class_body;
struct abruby_class *ab_class_class   = &ab_class_class_body;
struct abruby_class *ab_object_class  = &ab_object_class_body;
struct abruby_class *ab_integer_class = &ab_integer_class_body;
struct abruby_class *ab_string_class  = &ab_string_class_body;
struct abruby_class *ab_symbol_class  = &ab_symbol_class_body;
struct abruby_class *ab_range_class   = &ab_range_class_body;
struct abruby_class *ab_regexp_class  = &ab_regexp_class_body;
struct abruby_class *ab_rational_class = &ab_rational_class_body;
struct abruby_class *ab_complex_class  = &ab_complex_class_body;
struct abruby_class *ab_true_class    = &ab_true_class_body;
struct abruby_class *ab_false_class   = &ab_false_class_body;
struct abruby_class *ab_nil_class     = &ab_nil_class_body;
struct abruby_class *ab_runtime_error_class = &ab_runtime_error_class_body;

// Unified T_DATA type for all abruby heap objects

static void abruby_data_mark(void *ptr) {
    struct abruby_header *h = (struct abruby_header *)ptr;
    if (h->klass == ab_integer_class) {
        rb_gc_mark(((struct abruby_bignum *)ptr)->rb_bignum);
    }
    else if (h->klass == ab_float_class) {
        rb_gc_mark(((struct abruby_float *)ptr)->rb_float);
    }
    else if (h->klass == ab_string_class) {
        rb_gc_mark(((struct abruby_string *)ptr)->rb_str);
    }
    else if (h->klass == ab_array_class) {
        rb_gc_mark(((struct abruby_array *)ptr)->rb_ary);
    }
    else if (h->klass == ab_hash_class) {
        rb_gc_mark(((struct abruby_hash *)ptr)->rb_hash);
    }
    else if (h->klass == ab_range_class) {
        struct abruby_range *r = (struct abruby_range *)ptr;
        rb_gc_mark(r->begin);
        rb_gc_mark(r->end);
    }
    else if (h->klass == ab_regexp_class) {
        rb_gc_mark(((struct abruby_regexp *)ptr)->rb_regexp);
    }
    else if (h->klass == ab_rational_class) {
        rb_gc_mark(((struct abruby_rational *)ptr)->rb_rational);
    }
    else if (h->klass == ab_complex_class) {
        rb_gc_mark(((struct abruby_complex *)ptr)->rb_complex);
    }
    else if (h->klass == ab_runtime_error_class) {
        struct abruby_exception *exc = (struct abruby_exception *)ptr;
        rb_gc_mark(exc->message);
        rb_gc_mark(exc->backtrace);
    }
    else if (h->klass == ab_class_class || h->klass == ab_module_class) {
        struct abruby_class *cls = (struct abruby_class *)ptr;
        // Mark AST method bodies so GC doesn't collect them
        for (unsigned int i = 0; i < cls->method_cnt; i++) {
            if (cls->methods[i].type == ABRUBY_METHOD_AST) {
                NODE *body = cls->methods[i].u.ast.body;
                if (body && body->head.rb_wrapper) {
                    rb_gc_mark(body->head.rb_wrapper);
                }
            }
        }
        // Mark constant values
        for (unsigned int i = 0; i < cls->const_cnt; i++) {
            rb_gc_mark(cls->constants[i].value);
        }
    }
    else {
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
//
// All wrap functions use TypedData_Make_Struct which allocates the Ruby
// T_DATA object and the C struct in one step.  This avoids the GC hazard
// that exists with separate calloc + TypedData_Wrap_Struct: between the
// two calls, inner CRuby VALUEs stored in the calloc'd struct are
// invisible to the GC and can be collected if GC triggers.

VALUE
abruby_new_object(struct abruby_class *klass)
{
    struct abruby_object *obj;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_object, &abruby_data_type, obj);
    obj->klass = klass;
    return wrapper;
}

VALUE
abruby_bignum_new(VALUE rb_bignum)
{
    struct abruby_bignum *b;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_bignum, &abruby_data_type, b);
    b->klass = ab_integer_class;
    b->rb_bignum = rb_bignum;
    return wrapper;
}

VALUE
abruby_float_new_wrap(VALUE rb_float)
{
    struct abruby_float *f;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_float, &abruby_data_type, f);
    f->klass = ab_float_class;
    f->rb_float = rb_float;
    return wrapper;
}

// String helpers

VALUE
abruby_str_new(VALUE rb_str)
{
    struct abruby_string *s;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_string, &abruby_data_type, s);
    s->klass = ab_string_class;
    s->rb_str = rb_str;
    return wrapper;
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
    struct abruby_array *a;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_array, &abruby_data_type, a);
    a->klass = ab_array_class;
    a->rb_ary = rb_ary;
    return wrapper;
}

VALUE
abruby_hash_new_wrap(VALUE rb_hash)
{
    struct abruby_hash *h;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_hash, &abruby_data_type, h);
    h->klass = ab_hash_class;
    h->rb_hash = rb_hash;
    return wrapper;
}

VALUE
abruby_range_new(VALUE begin, VALUE end, bool exclude_end)
{
    struct abruby_range *r;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_range, &abruby_data_type, r);
    r->klass = ab_range_class;
    r->begin = begin;
    r->end = end;
    r->exclude_end = exclude_end;
    return wrapper;
}

VALUE
abruby_regexp_new(VALUE rb_regexp)
{
    struct abruby_regexp *r;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_regexp, &abruby_data_type, r);
    r->klass = ab_regexp_class;
    r->rb_regexp = rb_regexp;
    return wrapper;
}

VALUE
abruby_rational_new(VALUE rb_rational)
{
    struct abruby_rational *r;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_rational, &abruby_data_type, r);
    r->klass = ab_rational_class;
    r->rb_rational = rb_rational;
    return wrapper;
}

VALUE
abruby_complex_new(VALUE rb_complex)
{
    struct abruby_complex *cx;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_complex, &abruby_data_type, cx);
    cx->klass = ab_complex_class;
    cx->rb_complex = rb_complex;
    return wrapper;
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

void
abruby_class_set_const(struct abruby_class *klass, const char *name, VALUE val)
{
    // Check for existing, update
    for (unsigned int i = 0; i < klass->const_cnt; i++) {
        if (strcmp(klass->constants[i].name, name) == 0) {
            klass->constants[i].value = val;
            return;
        }
    }
    klass->constants[klass->const_cnt].name = name;
    klass->constants[klass->const_cnt].value = val;
    klass->const_cnt++;
}

// Call an abruby method (cfunc or AST) from C code.
// Uses a generous frame offset to avoid clobbering caller's locals.
RESULT
abruby_call_method(CTX *c, VALUE recv, struct abruby_method *method,
                   unsigned int argc, VALUE *argv)
{
    if (method->type == ABRUBY_METHOD_CFUNC) {
        return method->u.cfunc.func(c, recv, argc, argv);
    } else {
        VALUE *save_fp = c->fp;
        VALUE save_self = c->self;
        c->fp = save_fp + 16;
        // copy args into frame
        for (unsigned int i = 0; i < argc; i++) {
            c->fp[i] = argv[i];
        }
        c->self = recv;
        RESULT r = EVAL(c, method->u.ast.body);
        c->fp = save_fp;
        c->self = save_self;
        // Catch RETURN at method boundary
        if (r.state == RESULT_RETURN) return RESULT_OK(r.value);
        return r;
    }
}

VALUE
ab_inspect_rstr(CTX *c, VALUE v) {
    struct abruby_method *ins = abruby_find_method(AB_CLASS_OF(v), "inspect");
    RESULT r = abruby_call_method(c, v, ins, 0, NULL);
    return RSTR(r.value);
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
    Init_abruby_kernel();
    Init_abruby_class();
    Init_abruby_object();

    Init_abruby_integer();
    Init_abruby_string();
    Init_abruby_symbol();
    Init_abruby_float();
    Init_abruby_array();
    Init_abruby_hash();
    Init_abruby_range();
    Init_abruby_regexp();
    Init_abruby_rational();
    Init_abruby_complex();
    Init_abruby_true();
    Init_abruby_false();
    Init_abruby_nil();
    Init_abruby_exception();
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

// Per-instance VM state

#define STACK_SIZE 10000

struct abruby_vm {
    CTX ctx;
    VALUE stack[STACK_SIZE];
    struct abruby_class main_class_body;
    struct abruby_gvar_table gvars;
    VALUE rb_self;           // Ruby-level AbRuby instance (for callbacks)
    VALUE current_file;      // current file path (Ruby String or Qnil)
    VALUE loaded_files;      // Ruby Array of loaded file paths
};

static void
vm_mark(void *ptr)
{
    struct abruby_vm *vm = (struct abruby_vm *)ptr;
    rb_gc_mark(vm->ctx.self);
    rb_gc_mark_locations(vm->stack, vm->stack + STACK_SIZE);
    for (unsigned int i = 0; i < vm->gvars.cnt; i++) {
        rb_gc_mark(vm->gvars.entries[i].value);
    }
    rb_gc_mark(vm->rb_self);
    rb_gc_mark(vm->current_file);
    rb_gc_mark(vm->loaded_files);
    // Mark main_class method bodies and constants
    // (main_class is embedded in VM, not wrapped as T_DATA)
    struct abruby_class *mc = &vm->main_class_body;
    for (unsigned int i = 0; i < mc->method_cnt; i++) {
        if (mc->methods[i].type == ABRUBY_METHOD_AST) {
            NODE *body = mc->methods[i].u.ast.body;
            if (body && body->head.rb_wrapper) {
                rb_gc_mark(body->head.rb_wrapper);
            }
        }
    }
    for (unsigned int i = 0; i < mc->const_cnt; i++) {
        rb_gc_mark(mc->constants[i].value);
    }
}

static void
vm_free(void *ptr)
{
    free(ptr);
}

static const rb_data_type_t abruby_vm_type = {
    "AbRuby",
    { vm_mark, vm_free, NULL },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

// CTX is the first member of abruby_vm, so we can cast back
#define VM_FROM_CTX(c) ((struct abruby_vm *)(c))

// Create an exception object with backtrace captured from the current frame chain
VALUE
abruby_exception_new(CTX *c, struct abruby_frame *start_frame, VALUE message)
{
    (void)c;

    // Build backtrace Array by walking the frame list.
    // Derive name/file/line from method and node pointers (cold path).
    VALUE bt_ary = rb_ary_new();
    for (struct abruby_frame *f = start_frame; f; f = f->prev) {
        const char *name;
        const char *file;
        int32_t line;

        if (f->method) {
            // Normal method frame
            name = f->method->name;
            file = (f->method->type == ABRUBY_METHOD_AST && f->method->u.ast.source_file)
                ? f->method->u.ast.source_file : "(abruby)";
            line = f->node ? f->node->head.line : 0;
        } else {
            // <main> or <top (required)>: union holds source_file
            name = f->prev ? "<top (required)>" : "<main>";
            file = f->source_file ? f->source_file : "(abruby)";
            line = 0;
        }

        rb_ary_push(bt_ary, rb_sprintf("%s:%d:in `%s'", file, line, name));
    }

    // Create exception object
    struct abruby_exception *exc;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_exception,
                                          &abruby_data_type, exc);
    exc->klass = ab_runtime_error_class;
    exc->message = message;
    exc->backtrace = bt_ary;
    return wrapper;
}

static struct abruby_vm *
create_vm(void)
{
    struct abruby_vm *vm = ruby_xcalloc(1, sizeof(struct abruby_vm));
    // Per-instance main class (inherits from Object)
    vm->main_class_body.klass = ab_class_class;
    vm->main_class_body.name = "main";
    vm->main_class_body.super = ab_object_class;
    vm->ctx.main_class = &vm->main_class_body;

    vm->ctx.env = vm->stack;
    vm->ctx.fp = vm->stack;
    vm->ctx.self = abruby_new_object(&vm->main_class_body);
    vm->ctx.current_class = NULL;
    vm->ctx.gvars = &vm->gvars;
    vm->rb_self = Qnil;
    vm->current_file = Qnil;
    vm->loaded_files = rb_ary_new();

    for (int i = 0; i < STACK_SIZE; i++) {
        vm->stack[i] = Qnil;
    }

    return vm;
}

static void
init_builtin_consts(void)
{
    abruby_class_set_const(ab_object_class, "Object",     abruby_wrap_class(ab_object_class));
    abruby_class_set_const(ab_object_class, "Class",      abruby_wrap_class(ab_class_class));
    abruby_class_set_const(ab_object_class, "Module",     abruby_wrap_class(ab_module_class));
    abruby_class_set_const(ab_object_class, "Kernel",     abruby_wrap_class(ab_kernel_module));
    abruby_class_set_const(ab_object_class, "Integer",    abruby_wrap_class(ab_integer_class));
    abruby_class_set_const(ab_object_class, "Float",      abruby_wrap_class(ab_float_class));
    abruby_class_set_const(ab_object_class, "String",     abruby_wrap_class(ab_string_class));
    abruby_class_set_const(ab_object_class, "Symbol",     abruby_wrap_class(ab_symbol_class));
    abruby_class_set_const(ab_object_class, "Array",      abruby_wrap_class(ab_array_class));
    abruby_class_set_const(ab_object_class, "Hash",       abruby_wrap_class(ab_hash_class));
    abruby_class_set_const(ab_object_class, "Range",      abruby_wrap_class(ab_range_class));
    abruby_class_set_const(ab_object_class, "Regexp",     abruby_wrap_class(ab_regexp_class));
    abruby_class_set_const(ab_object_class, "Rational",   abruby_wrap_class(ab_rational_class));
    abruby_class_set_const(ab_object_class, "Complex",    abruby_wrap_class(ab_complex_class));
    abruby_class_set_const(ab_object_class, "TrueClass",  abruby_wrap_class(ab_true_class));
    abruby_class_set_const(ab_object_class, "FalseClass", abruby_wrap_class(ab_false_class));
    abruby_class_set_const(ab_object_class, "NilClass",      abruby_wrap_class(ab_nil_class));
    abruby_class_set_const(ab_object_class, "RuntimeError", abruby_wrap_class(ab_runtime_error_class));
}

// NODE wrapper (T_DATA)

const rb_data_type_t abruby_node_type = {
    "AbRuby::Node",
    { abruby_node_mark, RUBY_DEFAULT_FREE, NULL },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

VALUE
abruby_wrap_node(NODE *n)
{
    if (n == NULL) return Qnil;
    if (n->head.rb_wrapper) return n->head.rb_wrapper;

    VALUE obj = TypedData_Wrap_Struct(rb_cAbRubyNode, &abruby_node_type, n);
    n->head.rb_wrapper = obj;
    return obj;
}

static VALUE
wrap_node(NODE *n)
{
    return abruby_wrap_node(n);
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
rb_alloc_node_bignum_new(VALUE self, VALUE str)
{
    const char *cstr = strdup(StringValueCStr(str));
    return wrap_node(ALLOC_node_bignum_new(cstr));
}

static VALUE
rb_alloc_node_float_new(VALUE self, VALUE str)
{
    const char *cstr = strdup(StringValueCStr(str));
    return wrap_node(ALLOC_node_float_new(cstr));
}

static VALUE
rb_alloc_node_str_new(VALUE self, VALUE str)
{
    const char *cstr = strdup(StringValueCStr(str));
    return wrap_node(ALLOC_node_str_new(cstr));
}

static VALUE
rb_alloc_node_str_concat(VALUE self, VALUE argc, VALUE arg_index)
{
    return wrap_node(ALLOC_node_str_concat(FIX2UINT(argc), FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_sym(VALUE self, VALUE str)
{
    const char *cstr = strdup(StringValueCStr(str));
    return wrap_node(ALLOC_node_sym(cstr));
}

static VALUE
rb_alloc_node_range_new(VALUE self, VALUE begin_node, VALUE end_node, VALUE exclude_end)
{
    return wrap_node(ALLOC_node_range_new(unwrap_node(begin_node), unwrap_node(end_node), FIX2UINT(exclude_end)));
}

static VALUE
rb_alloc_node_regexp_new(VALUE self, VALUE source, VALUE flags)
{
    const char *csrc = strdup(StringValueCStr(source));
    const char *cflags = strdup(StringValueCStr(flags));
    return wrap_node(ALLOC_node_regexp_new(csrc, cflags));
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
rb_alloc_node_lvar_get(VALUE self, VALUE index)
{
    return wrap_node(ALLOC_node_lvar_get(FIX2UINT(index)));
}

static VALUE
rb_alloc_node_lvar_set(VALUE self, VALUE index, VALUE rhs)
{
    return wrap_node(ALLOC_node_lvar_set(FIX2UINT(index), unwrap_node(rhs)));
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
rb_alloc_node_return(VALUE self, VALUE value)
{
    return wrap_node(ALLOC_node_return(unwrap_node(value)));
}

static VALUE
rb_alloc_node_break(VALUE self, VALUE value)
{
    return wrap_node(ALLOC_node_break(unwrap_node(value)));
}

static VALUE
rb_alloc_node_rescue(VALUE self, VALUE body, VALUE rescue_body, VALUE ensure_body, VALUE exception_lvar_index)
{
    return wrap_node(ALLOC_node_rescue(unwrap_node(body), unwrap_node(rescue_body),
                                       unwrap_node(ensure_body), FIX2UINT(exception_lvar_index)));
}

static VALUE
rb_alloc_node_def(VALUE self, VALUE name, VALUE body, VALUE params_cnt, VALUE locals_cnt)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_def(cname, unwrap_node(body), FIX2UINT(params_cnt), FIX2UINT(locals_cnt)));
}


// ivar nodes

// Global variable nodes

static VALUE
rb_alloc_node_gvar_get(VALUE self, VALUE name)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_gvar_get(cname));
}

static VALUE
rb_alloc_node_gvar_set(VALUE self, VALUE name, VALUE value)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_gvar_set(cname, unwrap_node(value)));
}

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
rb_alloc_node_ary_new(VALUE self, VALUE argc, VALUE arg_index)
{
    return wrap_node(ALLOC_node_ary_new(FIX2UINT(argc), FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_hash_new(VALUE self, VALUE argc, VALUE arg_index)
{
    return wrap_node(ALLOC_node_hash_new(FIX2UINT(argc), FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_const_set(VALUE self, VALUE name, VALUE value)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_const_set(cname, unwrap_node(value)));
}

static VALUE
rb_alloc_node_const_get(VALUE self, VALUE name)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_const_get(cname));
}

static VALUE
rb_alloc_node_const_path_get(VALUE self, VALUE parent, VALUE name)
{
    const char *cparent = strdup(StringValueCStr(parent));
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_const_path_get(cparent, cname));
}

static VALUE
rb_alloc_node_method_call(VALUE self, VALUE recv, VALUE name, VALUE params_cnt, VALUE arg_index)
{
    const char *cname = strdup(StringValueCStr(name));
    return wrap_node(ALLOC_node_method_call(unwrap_node(recv), cname, FIX2UINT(params_cnt), FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_super(VALUE self, VALUE params_cnt, VALUE arg_index)
{
    return wrap_node(ALLOC_node_super(FIX2UINT(params_cnt), FIX2UINT(arg_index)));
}


static VALUE
rb_set_node_line(VALUE self, VALUE node_obj, VALUE line)
{
    NODE *n = unwrap_node(node_obj);
    if (n) n->head.line = FIX2INT(line);
    return node_obj;
}

// Arithmetic node alloc wrappers
static VALUE rb_alloc_node_plus(VALUE self, VALUE left, VALUE right) {
    return wrap_node(ALLOC_node_plus(unwrap_node(left), unwrap_node(right)));
}
static VALUE rb_alloc_node_minus(VALUE self, VALUE left, VALUE right) {
    return wrap_node(ALLOC_node_minus(unwrap_node(left), unwrap_node(right)));
}
static VALUE rb_alloc_node_mul(VALUE self, VALUE left, VALUE right) {
    return wrap_node(ALLOC_node_mul(unwrap_node(left), unwrap_node(right)));
}
static VALUE rb_alloc_node_div(VALUE self, VALUE left, VALUE right) {
    return wrap_node(ALLOC_node_div(unwrap_node(left), unwrap_node(right)));
}

// Comparison node alloc wrappers
static VALUE rb_alloc_node_lt(VALUE self, VALUE left, VALUE right) {
    return wrap_node(ALLOC_node_lt(unwrap_node(left), unwrap_node(right)));
}
static VALUE rb_alloc_node_le(VALUE self, VALUE left, VALUE right) {
    return wrap_node(ALLOC_node_le(unwrap_node(left), unwrap_node(right)));
}
static VALUE rb_alloc_node_gt(VALUE self, VALUE left, VALUE right) {
    return wrap_node(ALLOC_node_gt(unwrap_node(left), unwrap_node(right)));
}
static VALUE rb_alloc_node_ge(VALUE self, VALUE left, VALUE right) {
    return wrap_node(ALLOC_node_ge(unwrap_node(left), unwrap_node(right)));
}

// dump

// eval

// Convert abruby value to Ruby value for returning to CRuby world
static VALUE
abruby_to_ruby(VALUE v)
{
    // Symbols are CRuby immediates, pass through
    if (SYMBOL_P(v)) return v;

    if (RB_TYPE_P(v, T_DATA) && RTYPEDDATA_P(v) &&
        RTYPEDDATA_TYPE(v) == &abruby_data_type) {
        struct abruby_header *h = (struct abruby_header *)RTYPEDDATA_GET_DATA(v);
        if (h->klass == ab_integer_class) {
            return ((struct abruby_bignum *)h)->rb_bignum;
        }
        if (h->klass == ab_float_class) {
            return ((struct abruby_float *)h)->rb_float;
        }
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
                VALUE val = abruby_to_ruby(rb_hash_aref(hash, k));
                rb_hash_aset(result, k, val);
            }
            return result;
        }
        if (h->klass == ab_range_class) {
            struct abruby_range *r = (struct abruby_range *)h;
            return rb_range_new(abruby_to_ruby(r->begin), abruby_to_ruby(r->end), r->exclude_end);
        }
        if (h->klass == ab_regexp_class) {
            return ((struct abruby_regexp *)h)->rb_regexp;
        }
        if (h->klass == ab_rational_class) {
            return ((struct abruby_rational *)h)->rb_rational;
        }
        if (h->klass == ab_complex_class) {
            return ((struct abruby_complex *)h)->rb_complex;
        }
    }
    return v;
}

// require helper: load and eval a file in the VM's context
// Returns Qtrue if loaded, Qfalse if already loaded.
RESULT
abruby_require_file(CTX *c, VALUE rb_path)
{
    struct abruby_vm *vm = VM_FROM_CTX(c);

    // Resolve to absolute path
    VALUE abs_path = rb_file_expand_path(rb_path, Qnil);
    VALUE abs_str = rb_funcall(abs_path, rb_intern("to_s"), 0);

    // Check if already loaded
    if (RTEST(rb_funcall(vm->loaded_files, rb_intern("include?"), 1, abs_str))) {
        return RESULT_OK(Qfalse);
    }

    // Check file exists
    if (!RTEST(rb_funcall(rb_cFile, rb_intern("exist?"), 1, abs_str))) {
        rb_raise(rb_eLoadError, "cannot load such file -- %s", StringValueCStr(abs_str));
    }

    // Read file
    VALUE code = rb_funcall(rb_cFile, rb_intern("read"), 1, abs_str);

    // Parse via AbRuby::Parser
    VALUE parser = rb_funcall(rb_const_get(rb_cAbRuby, rb_intern("Parser")), rb_intern("new"), 0);
    VALUE ast_obj = rb_funcall(parser, rb_intern("parse"), 1, code);

    // Mark as loaded
    rb_ary_push(vm->loaded_files, abs_str);

    // Save/restore current_file
    VALUE save_file = vm->current_file;
    vm->current_file = abs_str;

    // Eval AST
    NODE *ast = unwrap_node(ast_obj);
    VALUE *save_fp = c->fp;
    struct abruby_frame *save_frame = c->current_frame;
    c->fp = c->env;
    c->current_class = NULL;
    struct abruby_frame req_frame = {save_frame, NULL, NULL, {.source_file = RSTRING_PTR(abs_str)}};
    c->current_frame = &req_frame;
    RESULT r = EVAL(c, ast);
    c->fp = save_fp;
    c->current_frame = save_frame;

    vm->current_file = save_file;

    if (r.state == RESULT_RAISE) return r;
    return RESULT_OK(Qtrue);
}

// eval(code_string) — parse and evaluate a string in the current context
RESULT
abruby_eval_string(CTX *c, VALUE rb_code)
{
    // Parse via AbRuby::Parser
    VALUE parser = rb_funcall(rb_const_get(rb_cAbRuby, rb_intern("Parser")), rb_intern("new"), 0);
    VALUE ast_obj = rb_funcall(parser, rb_intern("parse"), 1, rb_code);

    // Eval AST in current context
    NODE *ast = unwrap_node(ast_obj);
    RESULT r = EVAL(c, ast);
    return r;
}

// Get current file path from VM
VALUE
abruby_current_file(CTX *c)
{
    return VM_FROM_CTX(c)->current_file;
}

// AbRuby#initialize
static VALUE
rb_abruby_initialize(VALUE self)
{
    struct abruby_vm *vm = create_vm();
    DATA_PTR(self) = vm;
    vm->rb_self = self;
    return self;
}

static VALUE
rb_abruby_set_current_file(VALUE self, VALUE path)
{
    struct abruby_vm *vm;
    TypedData_Get_Struct(self, struct abruby_vm, &abruby_vm_type, vm);
    vm->current_file = NIL_P(path) ? Qnil : rb_file_expand_path(path, Qnil);
    return path;
}

// AbRuby allocator
static VALUE
rb_abruby_alloc(VALUE klass)
{
    return TypedData_Wrap_Struct(klass, &abruby_vm_type, NULL);
}

// AbRuby#eval_ast
static VALUE
rb_abruby_eval_ast(VALUE self, VALUE ast_obj)
{
    struct abruby_vm *vm;
    TypedData_Get_Struct(self, struct abruby_vm, &abruby_vm_type, vm);

    NODE *ast = unwrap_node(ast_obj);
    if (ast == NULL) {
        rb_raise(rb_eRuntimeError, "cannot eval NULL AST");
    }

    // reset stack for each eval (classes/methods/self persist)
    vm->ctx.fp = vm->ctx.env;
    vm->ctx.current_class = NULL;

    // Push <main> frame so backtrace always has a bottom frame
    const char *eval_file = NIL_P(vm->current_file) ? "(abruby)" : RSTRING_PTR(vm->current_file);
    struct abruby_frame main_frame = {NULL, NULL, NULL, {.source_file = eval_file}};
    vm->ctx.current_frame = &main_frame;

    RESULT r = EVAL(&vm->ctx, ast);
    vm->ctx.current_frame = NULL;
    if (r.state == RESULT_RAISE) {
        VALUE exc_val = r.value;
        // Extract message and backtrace from exception object
        if (RB_TYPE_P(exc_val, T_DATA) && RTYPEDDATA_P(exc_val) &&
            RTYPEDDATA_TYPE(exc_val) == &abruby_data_type) {
            struct abruby_header *h = (struct abruby_header *)RTYPEDDATA_GET_DATA(exc_val);
            if (h->klass == ab_runtime_error_class) {
                struct abruby_exception *exc = (struct abruby_exception *)h;
                VALUE msg = abruby_to_ruby(exc->message);
                VALUE msg_str = RB_TYPE_P(msg, T_STRING) ? msg : rb_funcall(msg, rb_intern("to_s"), 0);
                VALUE rb_exc = rb_exc_new_str(rb_eRuntimeError, msg_str);
                if (!NIL_P(exc->backtrace)) {
                    rb_funcall(rb_exc, rb_intern("set_backtrace"), 1, exc->backtrace);
                }
                rb_exc_raise(rb_exc);
            }
        }
        // Fallback for non-exception RAISE values
        VALUE msg = abruby_to_ruby(exc_val);
        if (RB_TYPE_P(msg, T_STRING)) {
            rb_raise(rb_eRuntimeError, "%s", StringValueCStr(msg));
        } else {
            VALUE s = rb_funcall(msg, rb_intern("to_s"), 0);
            rb_raise(rb_eRuntimeError, "%s", StringValueCStr(s));
        }
    }
    return abruby_to_ruby(r.value);
}

// AbRuby#dump_ast
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

void
Init_abruby(void)
{
    INIT();
    init_builtin_methods();

    rb_cAbRuby = rb_define_class("AbRuby", rb_cObject);
    rb_define_alloc_func(rb_cAbRuby, rb_abruby_alloc);
    rb_define_method(rb_cAbRuby, "initialize", rb_abruby_initialize, 0);

    rb_cAbRubyNode = rb_define_class_under(rb_cAbRuby, "Node", rb_cObject);
    rb_undef_alloc_func(rb_cAbRubyNode);

    // Set klass field on all built-in classes (common header)
    ab_float_class->klass   = ab_class_class;
    ab_array_class->klass   = ab_class_class;
    ab_hash_class->klass    = ab_class_class;
    ab_kernel_module->klass = ab_module_class;
    ab_module_class->klass  = ab_class_class;
    ab_class_class->klass   = ab_class_class;
    ab_object_class->klass  = ab_class_class;
    ab_integer_class->klass = ab_class_class;
    ab_string_class->klass  = ab_class_class;
    ab_symbol_class->klass  = ab_class_class;
    ab_range_class->klass   = ab_class_class;
    ab_regexp_class->klass  = ab_class_class;
    ab_rational_class->klass = ab_class_class;
    ab_complex_class->klass  = ab_class_class;
    ab_true_class->klass    = ab_class_class;
    ab_false_class->klass   = ab_class_class;
    ab_nil_class->klass     = ab_class_class;
    ab_runtime_error_class->klass = ab_class_class;

    // Wrap built-in classes as VALUE (must be after rb_cAbRubyNode is defined)
    abruby_wrap_class(ab_kernel_module);
    abruby_wrap_class(ab_module_class);
    abruby_wrap_class(ab_class_class);
    abruby_wrap_class(ab_object_class);
    abruby_wrap_class(ab_float_class);
    abruby_wrap_class(ab_array_class);
    abruby_wrap_class(ab_hash_class);
    abruby_wrap_class(ab_integer_class);
    abruby_wrap_class(ab_string_class);
    abruby_wrap_class(ab_symbol_class);
    abruby_wrap_class(ab_range_class);
    abruby_wrap_class(ab_regexp_class);
    abruby_wrap_class(ab_rational_class);
    abruby_wrap_class(ab_complex_class);
    abruby_wrap_class(ab_true_class);
    abruby_wrap_class(ab_false_class);
    abruby_wrap_class(ab_nil_class);
    abruby_wrap_class(ab_runtime_error_class);

    // include Kernel in Object: Object -> Kernel -> nil
    ab_object_class->super = ab_kernel_module;

    init_builtin_consts();

    // ALLOC wrappers
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_num", rb_alloc_node_num, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_bignum_new", rb_alloc_node_bignum_new, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_float_new", rb_alloc_node_float_new, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_str_new", rb_alloc_node_str_new, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_str_concat", rb_alloc_node_str_concat, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_sym", rb_alloc_node_sym, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_range_new", rb_alloc_node_range_new, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_regexp_new", rb_alloc_node_regexp_new, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_true", rb_alloc_node_true, 0);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_false", rb_alloc_node_false, 0);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_nil", rb_alloc_node_nil, 0);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_self", rb_alloc_node_self, 0);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_lvar_get", rb_alloc_node_lvar_get, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_lvar_set", rb_alloc_node_lvar_set, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_scope", rb_alloc_node_scope, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_seq", rb_alloc_node_seq, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_if", rb_alloc_node_if, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_while", rb_alloc_node_while, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_return", rb_alloc_node_return, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_break", rb_alloc_node_break, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_rescue", rb_alloc_node_rescue, 4);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_def", rb_alloc_node_def, 4);


    // ivar
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_gvar_get", rb_alloc_node_gvar_get, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_gvar_set", rb_alloc_node_gvar_set, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_ivar_get", rb_alloc_node_ivar_get, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_ivar_set", rb_alloc_node_ivar_set, 2);

    // Array / Hash
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_ary_new", rb_alloc_node_ary_new, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_hash_new", rb_alloc_node_hash_new, 2);

    // OOP
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_module_def", rb_alloc_node_module_def, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_class_def", rb_alloc_node_class_def, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_const_set", rb_alloc_node_const_set, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_const_get", rb_alloc_node_const_get, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_const_path_get", rb_alloc_node_const_path_get, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_method_call", rb_alloc_node_method_call, 4);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_super", rb_alloc_node_super, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_plus", rb_alloc_node_plus, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_minus", rb_alloc_node_minus, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_mul", rb_alloc_node_mul, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_div", rb_alloc_node_div, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_lt", rb_alloc_node_lt, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_le", rb_alloc_node_le, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_gt", rb_alloc_node_gt, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_ge", rb_alloc_node_ge, 2);
    rb_define_singleton_method(rb_cAbRuby, "set_node_line", rb_set_node_line, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_self", rb_alloc_node_self, 0);

    // eval / dump
    rb_define_method(rb_cAbRuby, "eval_ast", rb_abruby_eval_ast, 1);
    rb_define_method(rb_cAbRuby, "current_file=", rb_abruby_set_current_file, 1);
    rb_define_method(rb_cAbRuby, "dump_ast", rb_abruby_dump_ast, 1);
}
