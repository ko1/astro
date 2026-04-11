#include <ruby.h>
#include "node.h"
#include "context.h"
#include "builtin/builtin.h"
#include "astro_code_store.h"

// (abruby_vm_global removed: method_serial moved to abruby_machine)

struct abruby_option OPTION = {
    .no_compiled_code = true,
    .record_all = false,
    .quiet = false,
};

static VALUE rb_cAbRuby;
static VALUE rb_cAbRubyNode;

// Built-in abruby classes (klass field = ab_tmpl_class_class, set in init)

static struct abruby_class ab_tmpl_kernel_module_body = { .obj_type = ABRUBY_OBJ_MODULE };
static struct abruby_class ab_tmpl_module_class_body  = { .obj_type = ABRUBY_OBJ_CLASS };
static struct abruby_class ab_tmpl_class_class_body   = { .obj_type = ABRUBY_OBJ_CLASS, .super = &ab_tmpl_module_class_body };
static struct abruby_class ab_tmpl_object_class_body  = { .obj_type = ABRUBY_OBJ_GENERIC };
static struct abruby_class ab_tmpl_float_class_body   = { .obj_type = ABRUBY_OBJ_FLOAT,     .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_array_class_body   = { .obj_type = ABRUBY_OBJ_ARRAY,     .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_hash_class_body    = { .obj_type = ABRUBY_OBJ_HASH,      .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_integer_class_body = { .obj_type = ABRUBY_OBJ_BIGNUM,    .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_string_class_body  = { .obj_type = ABRUBY_OBJ_STRING,    .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_symbol_class_body  = { .obj_type = ABRUBY_OBJ_GENERIC,   .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_range_class_body   = { .obj_type = ABRUBY_OBJ_RANGE,     .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_regexp_class_body  = { .obj_type = ABRUBY_OBJ_REGEXP,    .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_rational_class_body = { .obj_type = ABRUBY_OBJ_RATIONAL, .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_complex_class_body  = { .obj_type = ABRUBY_OBJ_COMPLEX,  .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_true_class_body    = { .obj_type = ABRUBY_OBJ_GENERIC,   .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_false_class_body   = { .obj_type = ABRUBY_OBJ_GENERIC,   .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_nil_class_body     = { .obj_type = ABRUBY_OBJ_GENERIC,   .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_runtime_error_class_body = { .obj_type = ABRUBY_OBJ_EXCEPTION, .super = &ab_tmpl_object_class_body };

// Template class pointers.
//
// These are templates populated by Init_abruby_* (method/constant registration).
// They are cloned per abruby_machine instance in init_instance_classes().
// Only Init_abruby_* functions in builtin/*.c should reference these directly.
// Runtime code must use c->abm->xxx_class for per-instance resolution.
struct abruby_class *const ab_tmpl_float_class   = &ab_tmpl_float_class_body;
struct abruby_class *const ab_tmpl_array_class   = &ab_tmpl_array_class_body;
struct abruby_class *const ab_tmpl_hash_class    = &ab_tmpl_hash_class_body;
struct abruby_class *const ab_tmpl_kernel_module = &ab_tmpl_kernel_module_body;
struct abruby_class *const ab_tmpl_module_class  = &ab_tmpl_module_class_body;
struct abruby_class *const ab_tmpl_class_class   = &ab_tmpl_class_class_body;
struct abruby_class *const ab_tmpl_object_class  = &ab_tmpl_object_class_body;
struct abruby_class *const ab_tmpl_integer_class = &ab_tmpl_integer_class_body;
struct abruby_class *const ab_tmpl_string_class  = &ab_tmpl_string_class_body;
struct abruby_class *const ab_tmpl_symbol_class  = &ab_tmpl_symbol_class_body;
struct abruby_class *const ab_tmpl_range_class   = &ab_tmpl_range_class_body;
struct abruby_class *const ab_tmpl_regexp_class  = &ab_tmpl_regexp_class_body;
struct abruby_class *const ab_tmpl_rational_class = &ab_tmpl_rational_class_body;
struct abruby_class *const ab_tmpl_complex_class  = &ab_tmpl_complex_class_body;
struct abruby_class *const ab_tmpl_true_class    = &ab_tmpl_true_class_body;
struct abruby_class *const ab_tmpl_false_class   = &ab_tmpl_false_class_body;
struct abruby_class *const ab_tmpl_nil_class     = &ab_tmpl_nil_class_body;
struct abruby_class *const ab_tmpl_runtime_error_class = &ab_tmpl_runtime_error_class_body;

// Unified T_DATA type for all abruby heap objects

static void abruby_data_mark(void *ptr) {
    const struct abruby_header *h = (const struct abruby_header *)ptr;
    if (!h->klass) return;

    switch (h->klass->obj_type) {
    case ABRUBY_OBJ_BIGNUM:
        rb_gc_mark(((const struct abruby_bignum *)ptr)->rb_bignum);
        break;
    case ABRUBY_OBJ_FLOAT:
        rb_gc_mark(((const struct abruby_float *)ptr)->rb_float);
        break;
    case ABRUBY_OBJ_STRING:
        rb_gc_mark(((const struct abruby_string *)ptr)->rb_str);
        break;
    case ABRUBY_OBJ_ARRAY:
        rb_gc_mark(((const struct abruby_array *)ptr)->rb_ary);
        break;
    case ABRUBY_OBJ_HASH:
        rb_gc_mark(((const struct abruby_hash *)ptr)->rb_hash);
        break;
    case ABRUBY_OBJ_RANGE: {
        const struct abruby_range *r = (const struct abruby_range *)ptr;
        rb_gc_mark(r->begin);
        rb_gc_mark(r->end);
        break;
    }
    case ABRUBY_OBJ_REGEXP:
        rb_gc_mark(((const struct abruby_regexp *)ptr)->rb_regexp);
        break;
    case ABRUBY_OBJ_RATIONAL:
        rb_gc_mark(((const struct abruby_rational *)ptr)->rb_rational);
        break;
    case ABRUBY_OBJ_COMPLEX:
        rb_gc_mark(((const struct abruby_complex *)ptr)->rb_complex);
        break;
    case ABRUBY_OBJ_EXCEPTION: {
        const struct abruby_exception *exc = (const struct abruby_exception *)ptr;
        rb_gc_mark(exc->message);
        rb_gc_mark(exc->backtrace);
        break;
    }
    case ABRUBY_OBJ_CLASS:
    case ABRUBY_OBJ_MODULE: {
        const struct abruby_class *cls = (const struct abruby_class *)ptr;
        ab_id_table_foreach(&cls->methods, _mk, _mv, {
            const struct abruby_method *m = (const struct abruby_method *)_mv;
            if (m->type == ABRUBY_METHOD_AST && m->u.ast.body && m->u.ast.body->head.rb_wrapper) {
                rb_gc_mark(m->u.ast.body->head.rb_wrapper);
            }
        });
        ab_id_table_foreach(&cls->constants, _ck, _cv, {
            rb_gc_mark(_cv);
        });
        break;
    }
    case ABRUBY_OBJ_GENERIC:
    default: {
        const struct abruby_object *obj = (const struct abruby_object *)ptr;
        ab_id_table_foreach(&obj->ivars, _ik, _iv, {
            rb_gc_mark(_iv);
        });
        break;
    }
    }
}

// Custom free: for class/module T_DATAs, free the nested methods/constants
// tables before freeing the struct. For generic objects, free the ivars table.
// Other types (bignum, float, string, ...) just free the struct.
// Templates (ab_tmpl_*_class_body) are static memory and never wrapped as
// T_DATA, so they are never passed to this function.
static void
abruby_data_free(void *ptr)
{
    if (!ptr) return;
    const struct abruby_header *h = (const struct abruby_header *)ptr;
    if (h->klass) {
        switch (h->klass->obj_type) {
        case ABRUBY_OBJ_CLASS:
        case ABRUBY_OBJ_MODULE: {
            struct abruby_class *cls = (struct abruby_class *)ptr;
            ab_id_table_free(&cls->methods);
            ab_id_table_free(&cls->constants);
            break;
        }
        case ABRUBY_OBJ_GENERIC: {
            struct abruby_object *obj = (struct abruby_object *)ptr;
            ab_id_table_free(&obj->ivars);
            break;
        }
        default:
            break;
        }
    }
    ruby_xfree(ptr);
}

const rb_data_type_t abruby_data_type = {
    "AbRuby::Data",
    { abruby_data_mark, abruby_data_free, NULL },
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
abruby_bignum_new(CTX *c, VALUE rb_bignum)
{
    struct abruby_bignum *b;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_bignum, &abruby_data_type, b);
    b->klass = c->abm->integer_class;
    b->rb_bignum = rb_bignum;
    return wrapper;
}

VALUE
abruby_float_new_wrap(CTX *c, VALUE rb_float)
{
    // Pass Flonum through as an immediate; AB_CLASS_OF_IMM resolves it to
    // float_class. Only heap T_FLOAT (out-of-Flonum-range values like NaN,
    // some infinities) needs the T_DATA wrapper.
    if (RB_FLONUM_P(rb_float)) return rb_float;
    struct abruby_float *f;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_float, &abruby_data_type, f);
    f->klass = c->abm->float_class;
    f->rb_float = rb_float;
    return wrapper;
}

// String helpers

VALUE
abruby_str_new(CTX *c, VALUE rb_str)
{
    struct abruby_string *s;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_string, &abruby_data_type, s);
    s->klass = c->abm->string_class;
    s->rb_str = rb_str;
    return wrapper;
}

VALUE
abruby_str_new_cstr(CTX *c, const char *str)
{
    return abruby_str_new(c, rb_str_new_cstr(str));
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
abruby_ary_new(CTX *c, VALUE rb_ary)
{
    struct abruby_array *a;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_array, &abruby_data_type, a);
    a->klass = c->abm->array_class;
    a->rb_ary = rb_ary;
    return wrapper;
}

VALUE
abruby_hash_new_wrap(CTX *c, VALUE rb_hash)
{
    struct abruby_hash *h;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_hash, &abruby_data_type, h);
    h->klass = c->abm->hash_class;
    h->rb_hash = rb_hash;
    return wrapper;
}

VALUE
abruby_range_new(CTX *c, VALUE begin, VALUE end, bool exclude_end)
{
    struct abruby_range *r;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_range, &abruby_data_type, r);
    r->klass = c->abm->range_class;
    r->begin = begin;
    r->end = end;
    r->exclude_end = exclude_end;
    return wrapper;
}

VALUE
abruby_regexp_new(CTX *c, VALUE rb_regexp)
{
    struct abruby_regexp *r;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_regexp, &abruby_data_type, r);
    r->klass = c->abm->regexp_class;
    r->rb_regexp = rb_regexp;
    return wrapper;
}

VALUE
abruby_rational_new(CTX *c, VALUE rb_rational)
{
    struct abruby_rational *r;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_rational, &abruby_data_type, r);
    r->klass = c->abm->rational_class;
    r->rb_rational = rb_rational;
    return wrapper;
}

VALUE
abruby_complex_new(CTX *c, VALUE rb_complex)
{
    struct abruby_complex *cx;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_complex, &abruby_data_type, cx);
    cx->klass = c->abm->complex_class;
    cx->rb_complex = rb_complex;
    return wrapper;
}

// Class wrapper.
//
// Per-instance built-in classes are kept alive via vm_mark (which marks their
// rb_wrapper). User-defined classes are kept alive via their parent class's
// constants table (marked by vm_mark or by abruby_data_mark of the parent).
// Templates (ab_tmpl_*) are never wrapped — they are not exposed to Ruby.
VALUE
abruby_wrap_class(struct abruby_class *klass)
{
    if (klass->rb_wrapper) return klass->rb_wrapper;
    VALUE obj = TypedData_Wrap_Struct(rb_cAbRubyNode, &abruby_data_type, klass);
    klass->rb_wrapper = obj;
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
abruby_class_set_const(struct abruby_class *klass, ID name, VALUE val)
{
    ab_id_table_insert(&klass->constants, name, val);
}

// Call an abruby method (cfunc or AST) from C code.
// Uses a generous frame offset to avoid clobbering caller's locals.
RESULT
abruby_call_method(CTX *c, VALUE recv, const struct abruby_method *method,
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
    const struct abruby_method *ins = abruby_find_method(AB_CLASS_OF(c, v), rb_intern("inspect"));
    RESULT r = abruby_call_method(c, v, ins, 0, NULL);
    return RSTR(r.value);
}

void
abruby_class_add_cfunc(struct abruby_class *klass, ID name,
                       abruby_cfunc_t func, unsigned int params_cnt)
{
    struct abruby_method *m = ruby_xcalloc(1, sizeof(struct abruby_method));
    m->name = name;
    m->type = ABRUBY_METHOD_CFUNC;
    m->u.cfunc.func = func;
    m->u.cfunc.params_cnt = params_cnt;
    ab_id_table_insert(&klass->methods, name, (VALUE)m);
    // method_serial not bumped during init (no machine exists yet;
    // caches start at serial=0, machine starts at serial=1, so first access misses)
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
abruby_ivar_get(VALUE self, ID name)
{
    ab_verify(self);
    const struct abruby_object *obj =
        (const struct abruby_object *)RTYPEDDATA_GET_DATA(self);
    VALUE v;
    if (ab_id_table_lookup(&obj->ivars, name, &v)) return v;
    return Qnil;
}

void
abruby_ivar_set(VALUE self, ID name, VALUE val)
{
    ab_verify(self);
    if (!RB_TYPE_P(self, T_DATA)) {
        rb_raise(rb_eRuntimeError, "can't set instance variable on non-object");
    }
    struct abruby_object *obj;
    TypedData_Get_Struct(self, struct abruby_object, &abruby_data_type, obj);
    ab_id_table_insert(&obj->ivars, name, val);
}

// Per-instance VM state (struct abruby_machine defined in context.h)

static void
vm_mark(void *ptr)
{
    const struct abruby_machine *vm = (const struct abruby_machine *)ptr;
    if (vm->current_fiber) {
        rb_gc_mark(vm->current_fiber->ctx.self);
        rb_gc_mark_locations(vm->current_fiber->ctx.stack, vm->current_fiber->ctx.stack + ABRUBY_STACK_SIZE);
    }
    rb_gc_mark(vm->rb_self);
    rb_gc_mark(vm->current_file);
    rb_gc_mark(vm->loaded_files);
    // Mark main_class method bodies and constants
    // (main_class is embedded in VM, not wrapped as T_DATA)
    const struct abruby_class *mc = &vm->main_class_body;
    ab_id_table_foreach(&mc->methods, _k, _v, {
        const struct abruby_method *m = (const struct abruby_method *)_v;
        if (m->type == ABRUBY_METHOD_AST && m->u.ast.body && m->u.ast.body->head.rb_wrapper) {
            rb_gc_mark(m->u.ast.body->head.rb_wrapper);
        }
    });
    ab_id_table_foreach(&mc->constants, _k2, _v2, {
        rb_gc_mark(_v2);
    });
    // Mark gvars
    ab_id_table_foreach(&vm->gvars, _k3, _v3, {
        rb_gc_mark(_v3);
    });
    // Mark per-instance built-in class wrappers. When the vm is collected,
    // these wrappers become unreachable and get freed via abruby_data_free.
    if (vm->kernel_module       && vm->kernel_module->rb_wrapper)        rb_gc_mark(vm->kernel_module->rb_wrapper);
    if (vm->module_class        && vm->module_class->rb_wrapper)         rb_gc_mark(vm->module_class->rb_wrapper);
    if (vm->class_class         && vm->class_class->rb_wrapper)          rb_gc_mark(vm->class_class->rb_wrapper);
    if (vm->object_class        && vm->object_class->rb_wrapper)         rb_gc_mark(vm->object_class->rb_wrapper);
    if (vm->integer_class       && vm->integer_class->rb_wrapper)        rb_gc_mark(vm->integer_class->rb_wrapper);
    if (vm->float_class         && vm->float_class->rb_wrapper)          rb_gc_mark(vm->float_class->rb_wrapper);
    if (vm->string_class        && vm->string_class->rb_wrapper)         rb_gc_mark(vm->string_class->rb_wrapper);
    if (vm->symbol_class        && vm->symbol_class->rb_wrapper)         rb_gc_mark(vm->symbol_class->rb_wrapper);
    if (vm->array_class         && vm->array_class->rb_wrapper)          rb_gc_mark(vm->array_class->rb_wrapper);
    if (vm->hash_class          && vm->hash_class->rb_wrapper)           rb_gc_mark(vm->hash_class->rb_wrapper);
    if (vm->range_class         && vm->range_class->rb_wrapper)          rb_gc_mark(vm->range_class->rb_wrapper);
    if (vm->regexp_class        && vm->regexp_class->rb_wrapper)         rb_gc_mark(vm->regexp_class->rb_wrapper);
    if (vm->rational_class      && vm->rational_class->rb_wrapper)       rb_gc_mark(vm->rational_class->rb_wrapper);
    if (vm->complex_class       && vm->complex_class->rb_wrapper)        rb_gc_mark(vm->complex_class->rb_wrapper);
    if (vm->true_class          && vm->true_class->rb_wrapper)           rb_gc_mark(vm->true_class->rb_wrapper);
    if (vm->false_class         && vm->false_class->rb_wrapper)          rb_gc_mark(vm->false_class->rb_wrapper);
    if (vm->nil_class           && vm->nil_class->rb_wrapper)            rb_gc_mark(vm->nil_class->rb_wrapper);
    if (vm->runtime_error_class && vm->runtime_error_class->rb_wrapper)  rb_gc_mark(vm->runtime_error_class->rb_wrapper);
}

static void
vm_free(void *ptr)
{
    struct abruby_machine *vm = (struct abruby_machine *)ptr;
    // Free embedded main_class_body's id_tables (main_class_body itself is
    // embedded in vm struct, not separately allocated).
    ab_id_table_free(&vm->main_class_body.methods);
    ab_id_table_free(&vm->main_class_body.constants);
    ab_id_table_free(&vm->gvars);
    // Per-instance built-in classes are T_DATA-wrapped; their structs and
    // inner tables are freed by abruby_data_free when their wrapper is GC'd.
    if (vm->current_fiber) ruby_xfree(vm->current_fiber);
    ruby_xfree(vm);
}

static size_t
vm_memsize(const void *ptr)
{
    return sizeof(struct abruby_machine) + sizeof(struct abruby_fiber);
}

static const rb_data_type_t abruby_machine_type = {
    "AbRuby",
    { vm_mark, vm_free, vm_memsize },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

// Create an exception object with backtrace captured from the current frame chain
VALUE
abruby_exception_new(CTX *c, const struct abruby_frame *start_frame, VALUE message)
{
    // Build backtrace Array by walking the frame list.
    // With caller_node scheme: each frame's caller_node stores the call site
    // in the PARENT method.  Pair f->caller_node (line) with f->prev (method name).
    VALUE bt_ary = rb_ary_new();
    for (const struct abruby_frame *f = start_frame; f && f->prev; f = f->prev) {
        const struct abruby_frame *parent = f->prev;
        const char *name;
        const char *file;
        int32_t line = f->caller_node ? f->caller_node->head.line : 0;

        if (parent->method) {
            name = rb_id2name(parent->method->name);
            file = (parent->method->type == ABRUBY_METHOD_AST && parent->method->u.ast.source_file)
                ? parent->method->u.ast.source_file : "(abruby)";
        } else {
            name = parent->prev ? "<top (required)>" : "<main>";
            file = parent->source_file ? parent->source_file : "(abruby)";
        }

        rb_ary_push(bt_ary, rb_sprintf("%s:%d:in `%s'", file, line, name));
    }

    // Create exception object
    struct abruby_exception *exc;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_exception,
                                          &abruby_data_type, exc);
    exc->klass = c->abm->runtime_error_class;
    exc->message = message;
    exc->backtrace = bt_ary;
    return wrapper;
}

// Clone a template class: copy obj_type, name, methods (not constants).
static struct abruby_class *
clone_class(const struct abruby_class *tmpl)
{
    struct abruby_class *c = ruby_xcalloc(1, sizeof(struct abruby_class));
    c->obj_type = tmpl->obj_type;
    c->name = tmpl->name;
    ab_id_table_clone(&c->methods, &tmpl->methods);
    // constants are NOT cloned (they reference template wrappers)
    return c;
}

// Create per-instance copies of all built-in classes from templates.
static void
init_instance_classes(struct abruby_machine *vm)
{
    // Clone all 18 template classes
    vm->kernel_module      = clone_class(ab_tmpl_kernel_module);
    vm->module_class       = clone_class(ab_tmpl_module_class);
    vm->class_class        = clone_class(ab_tmpl_class_class);
    vm->object_class       = clone_class(ab_tmpl_object_class);
    vm->integer_class      = clone_class(ab_tmpl_integer_class);
    vm->float_class        = clone_class(ab_tmpl_float_class);
    vm->string_class       = clone_class(ab_tmpl_string_class);
    vm->symbol_class       = clone_class(ab_tmpl_symbol_class);
    vm->array_class        = clone_class(ab_tmpl_array_class);
    vm->hash_class         = clone_class(ab_tmpl_hash_class);
    vm->range_class        = clone_class(ab_tmpl_range_class);
    vm->regexp_class       = clone_class(ab_tmpl_regexp_class);
    vm->rational_class     = clone_class(ab_tmpl_rational_class);
    vm->complex_class      = clone_class(ab_tmpl_complex_class);
    vm->true_class         = clone_class(ab_tmpl_true_class);
    vm->false_class        = clone_class(ab_tmpl_false_class);
    vm->nil_class          = clone_class(ab_tmpl_nil_class);
    vm->runtime_error_class = clone_class(ab_tmpl_runtime_error_class);

    // Fix up klass pointers (metaclass)
    vm->kernel_module->klass       = vm->module_class;
    vm->module_class->klass        = vm->class_class;
    vm->class_class->klass         = vm->class_class;
    vm->object_class->klass        = vm->class_class;
    vm->integer_class->klass       = vm->class_class;
    vm->float_class->klass         = vm->class_class;
    vm->string_class->klass        = vm->class_class;
    vm->symbol_class->klass        = vm->class_class;
    vm->array_class->klass         = vm->class_class;
    vm->hash_class->klass          = vm->class_class;
    vm->range_class->klass         = vm->class_class;
    vm->regexp_class->klass        = vm->class_class;
    vm->rational_class->klass      = vm->class_class;
    vm->complex_class->klass       = vm->class_class;
    vm->true_class->klass          = vm->class_class;
    vm->false_class->klass         = vm->class_class;
    vm->nil_class->klass           = vm->class_class;
    vm->runtime_error_class->klass = vm->class_class;

    // Fix up super pointers (inheritance chain)
    vm->class_class->super         = vm->module_class;
    vm->object_class->super        = vm->kernel_module;  // Object includes Kernel
    vm->integer_class->super       = vm->object_class;
    vm->float_class->super         = vm->object_class;
    vm->string_class->super        = vm->object_class;
    vm->symbol_class->super        = vm->object_class;
    vm->array_class->super         = vm->object_class;
    vm->hash_class->super          = vm->object_class;
    vm->range_class->super         = vm->object_class;
    vm->regexp_class->super        = vm->object_class;
    vm->rational_class->super      = vm->object_class;
    vm->complex_class->super       = vm->object_class;
    vm->true_class->super          = vm->object_class;
    vm->false_class->super         = vm->object_class;
    vm->nil_class->super           = vm->object_class;
    vm->runtime_error_class->super = vm->object_class;
    vm->kernel_module->super       = NULL;
    vm->module_class->super        = NULL;

    // Wrap each per-instance class as a VALUE (for constant table, Ruby-level access)
    abruby_wrap_class(vm->kernel_module);
    abruby_wrap_class(vm->module_class);
    abruby_wrap_class(vm->class_class);
    abruby_wrap_class(vm->object_class);
    abruby_wrap_class(vm->integer_class);
    abruby_wrap_class(vm->float_class);
    abruby_wrap_class(vm->string_class);
    abruby_wrap_class(vm->symbol_class);
    abruby_wrap_class(vm->array_class);
    abruby_wrap_class(vm->hash_class);
    abruby_wrap_class(vm->range_class);
    abruby_wrap_class(vm->regexp_class);
    abruby_wrap_class(vm->rational_class);
    abruby_wrap_class(vm->complex_class);
    abruby_wrap_class(vm->true_class);
    abruby_wrap_class(vm->false_class);
    abruby_wrap_class(vm->nil_class);
    abruby_wrap_class(vm->runtime_error_class);

    // Register class name constants on per-instance Object
    struct abruby_class *obj = vm->object_class;
    abruby_class_set_const(obj, rb_intern("Object"),       abruby_wrap_class(vm->object_class));
    abruby_class_set_const(obj, rb_intern("Class"),        abruby_wrap_class(vm->class_class));
    abruby_class_set_const(obj, rb_intern("Module"),       abruby_wrap_class(vm->module_class));
    abruby_class_set_const(obj, rb_intern("Kernel"),       abruby_wrap_class(vm->kernel_module));
    abruby_class_set_const(obj, rb_intern("Integer"),      abruby_wrap_class(vm->integer_class));
    abruby_class_set_const(obj, rb_intern("Float"),        abruby_wrap_class(vm->float_class));
    abruby_class_set_const(obj, rb_intern("String"),       abruby_wrap_class(vm->string_class));
    abruby_class_set_const(obj, rb_intern("Symbol"),       abruby_wrap_class(vm->symbol_class));
    abruby_class_set_const(obj, rb_intern("Array"),        abruby_wrap_class(vm->array_class));
    abruby_class_set_const(obj, rb_intern("Hash"),         abruby_wrap_class(vm->hash_class));
    abruby_class_set_const(obj, rb_intern("Range"),        abruby_wrap_class(vm->range_class));
    abruby_class_set_const(obj, rb_intern("Regexp"),       abruby_wrap_class(vm->regexp_class));
    abruby_class_set_const(obj, rb_intern("Rational"),     abruby_wrap_class(vm->rational_class));
    abruby_class_set_const(obj, rb_intern("Complex"),      abruby_wrap_class(vm->complex_class));
    abruby_class_set_const(obj, rb_intern("TrueClass"),    abruby_wrap_class(vm->true_class));
    abruby_class_set_const(obj, rb_intern("FalseClass"),   abruby_wrap_class(vm->false_class));
    abruby_class_set_const(obj, rb_intern("NilClass"),     abruby_wrap_class(vm->nil_class));
    abruby_class_set_const(obj, rb_intern("RuntimeError"), abruby_wrap_class(vm->runtime_error_class));

    // Float constants. abruby_float_new_wrap passes Flonum through and only
    // wraps heap T_FLOAT in T_DATA, so it works for both representations.
    abruby_class_set_const(vm->float_class, rb_intern("INFINITY"),
        abruby_float_new_wrap(&vm->current_fiber->ctx, rb_float_new(HUGE_VAL)));
    abruby_class_set_const(vm->float_class, rb_intern("NAN"),
        abruby_float_new_wrap(&vm->current_fiber->ctx, rb_float_new(nan(""))));
}

static struct abruby_machine *
create_vm(void)
{
    struct abruby_machine *vm = ruby_xcalloc(1, sizeof(struct abruby_machine));
    vm->method_serial = 1;
    vm->current_fiber = ruby_xcalloc(1, sizeof(struct abruby_fiber));
    // Wire ctx.abm early so init_instance_classes can use abruby_float_new_wrap
    // (which reads c->abm->float_class) when creating Float constants.
    vm->current_fiber->ctx.abm = vm;

    // Create per-instance built-in classes (must be before main_class_body setup)
    init_instance_classes(vm);

    // Per-instance main class (inherits from Object)
    vm->main_class_body.klass = vm->class_class;
    vm->main_class_body.name = rb_intern("main");
    vm->main_class_body.super = vm->object_class;

    vm->current_fiber->ctx.fp = vm->current_fiber->ctx.stack;
    vm->current_fiber->ctx.self = abruby_new_object(&vm->main_class_body);
    vm->current_fiber->ctx.current_class = NULL;
    vm->id_cache.op_plus = rb_intern("+");
    vm->id_cache.op_minus = rb_intern("-");
    vm->id_cache.op_mul = rb_intern("*");
    vm->id_cache.op_div = rb_intern("/");
    vm->id_cache.op_lt = rb_intern("<");
    vm->id_cache.op_le = rb_intern("<=");
    vm->id_cache.op_gt = rb_intern(">");
    vm->id_cache.op_ge = rb_intern(">=");
    vm->id_cache.op_eq = rb_intern("==");
    vm->id_cache.op_mod = rb_intern("%");
    vm->id_cache.method_missing = rb_intern("method_missing");
    vm->current_fiber->ctx.ids = &vm->id_cache;
    vm->current_fiber->ctx.abm = vm;
    vm->rb_self = Qnil;
    vm->current_file = Qnil;
    vm->loaded_files = rb_ary_new();

    for (int i = 0; i < ABRUBY_STACK_SIZE; i++) {
        vm->current_fiber->ctx.stack[i] = Qnil;
    }

    return vm;
}

// init_builtin_consts removed: constants are now registered per-instance in init_instance_classes()

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
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_def(cname, unwrap_node(body), FIX2UINT(params_cnt), FIX2UINT(locals_cnt)));
}


// ivar nodes

// Global variable nodes

static VALUE
rb_alloc_node_gvar_get(VALUE self, VALUE name)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_gvar_get(cname));
}

static VALUE
rb_alloc_node_gvar_set(VALUE self, VALUE name, VALUE value)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_gvar_set(cname, unwrap_node(value)));
}

static VALUE
rb_alloc_node_ivar_get(VALUE self, VALUE name)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_ivar_get(cname));
}

static VALUE
rb_alloc_node_ivar_set(VALUE self, VALUE name, VALUE value)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_ivar_set(cname, unwrap_node(value)));
}

// OOP nodes

static VALUE
rb_alloc_node_module_def(VALUE self, VALUE name, VALUE body)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_module_def(cname, unwrap_node(body)));
}

static VALUE
rb_alloc_node_class_def(VALUE self, VALUE name, VALUE super_expr, VALUE body)
{
    ID cname = rb_intern_str(name);
    NODE *super_node = unwrap_node(super_expr);
    return wrap_node(ALLOC_node_class_def(cname, super_node, unwrap_node(body)));
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
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_const_set(cname, unwrap_node(value)));
}

static VALUE
rb_alloc_node_const_get(VALUE self, VALUE name)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_const_get(cname));
}

static VALUE
rb_alloc_node_const_path_get(VALUE self, VALUE parent, VALUE name)
{
    ID cparent = rb_intern_str(parent);
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_const_path_get(cparent, cname));
}

static VALUE
rb_alloc_node_method_call(VALUE self, VALUE recv, VALUE name, VALUE params_cnt, VALUE arg_index)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_method_call(unwrap_node(recv), cname, FIX2UINT(params_cnt), FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_func_call(VALUE self, VALUE name, VALUE params_cnt, VALUE arg_index)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_func_call(cname, FIX2UINT(params_cnt), FIX2UINT(arg_index)));
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
static VALUE rb_alloc_node_plus(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_plus(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_minus(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_minus(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_plus(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_plus(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_minus(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_minus(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_mul(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_mul(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_div(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_div(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_mul(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_mul(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_div(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_div(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}

// Comparison node alloc wrappers
static VALUE rb_alloc_node_fixnum_lt(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_lt(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_le(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_le(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_gt(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_gt(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_ge(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_ge(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_eq(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_eq(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_neq(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_neq(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_fixnum_mod(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_fixnum_mod(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_lt(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_lt(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_le(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_le(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_gt(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_gt(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
}
static VALUE rb_alloc_node_ge(VALUE self, VALUE left, VALUE right, VALUE arg_index) {
    return wrap_node(ALLOC_node_ge(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index)));
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
        const struct abruby_header *h = (const struct abruby_header *)RTYPEDDATA_GET_DATA(v);
        if (!h->klass) return v;
        switch (h->klass->obj_type) {
        case ABRUBY_OBJ_BIGNUM:
            return ((const struct abruby_bignum *)h)->rb_bignum;
        case ABRUBY_OBJ_FLOAT:
            return ((const struct abruby_float *)h)->rb_float;
        case ABRUBY_OBJ_STRING:
            return ((const struct abruby_string *)h)->rb_str;
        case ABRUBY_OBJ_ARRAY: {
            VALUE ary = ((const struct abruby_array *)h)->rb_ary;
            long len = RARRAY_LEN(ary);
            VALUE result = rb_ary_new_capa(len);
            for (long i = 0; i < len; i++) {
                rb_ary_push(result, abruby_to_ruby(RARRAY_AREF(ary, i)));
            }
            return result;
        }
        case ABRUBY_OBJ_HASH: {
            VALUE hash = ((const struct abruby_hash *)h)->rb_hash;
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
        case ABRUBY_OBJ_RANGE: {
            const struct abruby_range *r = (const struct abruby_range *)h;
            return rb_range_new(abruby_to_ruby(r->begin), abruby_to_ruby(r->end), r->exclude_end);
        }
        case ABRUBY_OBJ_REGEXP:
            return ((const struct abruby_regexp *)h)->rb_regexp;
        case ABRUBY_OBJ_RATIONAL:
            return ((const struct abruby_rational *)h)->rb_rational;
        case ABRUBY_OBJ_COMPLEX:
            return ((const struct abruby_complex *)h)->rb_complex;
        default:
            break;
        }
    }
    return v;
}

// require helper: load and eval a file in the VM's context
// Returns Qtrue if loaded, Qfalse if already loaded.
RESULT
abruby_require_file(CTX *c, VALUE rb_path)
{
    struct abruby_machine *vm = c->abm;

    // Resolve to absolute path
    VALUE abs_path = rb_file_expand_path(rb_path, Qnil);
    VALUE abs_str = rb_funcall(abs_path, rb_intern("to_s"), 0);

    // Check if already loaded
    if (RTEST(rb_funcall(vm->loaded_files, rb_intern("include?"), 1, abs_str))) {
        return RESULT_OK(Qfalse);
    }

    // Check file exists (use file? to exclude directories)
    if (!RTEST(rb_funcall(rb_cFile, rb_intern("file?"), 1, abs_str))) {
        rb_raise(rb_eLoadError, "cannot load such file -- %s", StringValueCStr(abs_str));
    }

    // Read file
    VALUE code = rb_funcall(rb_cFile, rb_intern("read"), 1, abs_str);

    // Parse via AbRuby::Parser
    VALUE parser = rb_funcall(rb_const_get(rb_cAbRuby, rb_intern("Parser")), rb_intern("new"), 0);
    VALUE ast_obj = rb_funcall(parser, rb_intern("parse"), 2, code, abs_str);

    // Mark as loaded
    rb_ary_push(vm->loaded_files, abs_str);

    // Save/restore current_file
    VALUE save_file = vm->current_file;
    vm->current_file = abs_str;

    // Eval AST
    NODE *ast = unwrap_node(ast_obj);
    VALUE *save_fp = c->fp;
    struct abruby_frame *save_frame = c->current_frame;
    c->fp = c->stack;
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
abruby_current_file(const CTX *c)
{
    return c->abm->current_file;
}

// AbRuby#initialize
static VALUE
rb_abruby_initialize(VALUE self)
{
    struct abruby_machine *vm = create_vm();
    DATA_PTR(self) = vm;
    vm->rb_self = self;
    return self;
}

static VALUE
rb_abruby_get_current_file(VALUE self)
{
    struct abruby_machine *vm;
    TypedData_Get_Struct(self, struct abruby_machine, &abruby_machine_type, vm);
    return vm->current_file;
}

static VALUE
rb_abruby_set_current_file(VALUE self, VALUE path)
{
    struct abruby_machine *vm;
    TypedData_Get_Struct(self, struct abruby_machine, &abruby_machine_type, vm);
    vm->current_file = NIL_P(path) ? Qnil : rb_file_expand_path(path, Qnil);
    return path;
}

// AbRuby allocator
static VALUE
rb_abruby_alloc(VALUE klass)
{
    return TypedData_Wrap_Struct(klass, &abruby_machine_type, NULL);
}

// AbRuby#eval_ast
static VALUE
rb_abruby_eval_ast(VALUE self, VALUE ast_obj)
{
    struct abruby_machine *vm;
    TypedData_Get_Struct(self, struct abruby_machine, &abruby_machine_type, vm);

    NODE *ast = unwrap_node(ast_obj);
    if (ast == NULL) {
        rb_raise(rb_eRuntimeError, "cannot eval NULL AST");
    }

    // reset stack for each eval (classes/methods/self persist)
    vm->current_fiber->ctx.fp = vm->current_fiber->ctx.stack;
    vm->current_fiber->ctx.current_class = NULL;

    // Push <main> frame so backtrace always has a bottom frame
    const char *eval_file = NIL_P(vm->current_file) ? "(abruby)" : RSTRING_PTR(vm->current_file);
    struct abruby_frame main_frame = {NULL, NULL, NULL, {.source_file = eval_file}};
    vm->current_fiber->ctx.current_frame = &main_frame;

    RESULT r = EVAL(&vm->current_fiber->ctx, ast);
    vm->current_fiber->ctx.current_frame = NULL;
    if (r.state == RESULT_RAISE) {
        VALUE exc_val = r.value;
        // Extract message and backtrace from exception object
        if (RB_TYPE_P(exc_val, T_DATA) && RTYPEDDATA_P(exc_val) &&
            RTYPEDDATA_TYPE(exc_val) == &abruby_data_type) {
            const struct abruby_header *h = (const struct abruby_header *)RTYPEDDATA_GET_DATA(exc_val);
            if (h->klass && h->klass->obj_type == ABRUBY_OBJ_EXCEPTION) {
                const struct abruby_exception *exc = (const struct abruby_exception *)h;
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

// --- Code Store Ruby API ---

// AbRuby.verbose = bool
static VALUE
rb_astro_set_verbose(VALUE self, VALUE val)
{
    OPTION.verbose = RTEST(val);
    return val;
}

// AbRuby.compiled_only = bool — set before parsing; non-noinline nodes get NULL dispatcher
static VALUE
rb_astro_set_compiled_only(VALUE self, VALUE val)
{
    OPTION.compiled_only = RTEST(val);
    return val;
}

// AbRuby.verify_compiled(node) — check a node's top-level is specialized.
// Child nodes are called directly by the SD function (not via default dispatcher),
// so verifying the top entry node is sufficient.
static VALUE
rb_astro_verify_compiled(VALUE self, VALUE node_val)
{
    NODE *n = DATA_PTR(node_val);
    if (!n->head.flags.no_inline && !n->head.flags.is_specialized) {
        return rb_str_new_cstr(n->head.kind->default_dispatcher_name);
    }
    return Qnil;
}

// AbRuby.cs_init(store_dir, src_dir, version)
static VALUE
rb_astro_cs_init(VALUE self, VALUE store_dir, VALUE src_dir, VALUE version)
{
    astro_cs_init(StringValueCStr(store_dir), StringValueCStr(src_dir),
                  NUM2ULL(version));
    OPTION.no_compiled_code = false;
    return Qnil;
}

// AbRuby.cs_load(node) → true/false
static VALUE
rb_astro_cs_load(VALUE self, VALUE node_val)
{
    NODE *n = DATA_PTR(node_val);
    return astro_cs_load(n) ? Qtrue : Qfalse;
}

// AbRuby.cs_compile(node)
static VALUE
rb_astro_cs_compile(VALUE self, VALUE node_val)
{
    NODE *n = DATA_PTR(node_val);
    astro_cs_compile(n);
    return Qnil;
}

// AbRuby.cs_build(cflags = nil)
static VALUE
rb_astro_cs_build(int argc, VALUE *argv, VALUE self)
{
    VALUE cflags_val;
    rb_scan_args(argc, argv, "01", &cflags_val);
    const char *cflags = NIL_P(cflags_val) ? NULL : StringValueCStr(cflags_val);
    astro_cs_build(cflags);
    return Qnil;
}

// AbRuby.cs_reload
static VALUE
rb_astro_cs_reload(VALUE self)
{
    astro_cs_reload();
    return Qnil;
}

// AbRuby.cs_disasm(node)
static VALUE
rb_astro_cs_disasm(VALUE self, VALUE node_val)
{
    NODE *n = DATA_PTR(node_val);
    astro_cs_disasm(n);
    return Qnil;
}

void
Init_abruby(void)
{
    // Initialize class names (can't use rb_intern in static initializers)
    ab_tmpl_kernel_module_body.name = rb_intern("Kernel");
    ab_tmpl_module_class_body.name = rb_intern("Module");
    ab_tmpl_class_class_body.name = rb_intern("Class");
    ab_tmpl_object_class_body.name = rb_intern("Object");
    ab_tmpl_float_class_body.name = rb_intern("Float");
    ab_tmpl_array_class_body.name = rb_intern("Array");
    ab_tmpl_hash_class_body.name = rb_intern("Hash");
    ab_tmpl_integer_class_body.name = rb_intern("Integer");
    ab_tmpl_string_class_body.name = rb_intern("String");
    ab_tmpl_symbol_class_body.name = rb_intern("Symbol");
    ab_tmpl_range_class_body.name = rb_intern("Range");
    ab_tmpl_regexp_class_body.name = rb_intern("Regexp");
    ab_tmpl_rational_class_body.name = rb_intern("Rational");
    ab_tmpl_complex_class_body.name = rb_intern("Complex");
    ab_tmpl_true_class_body.name = rb_intern("TrueClass");
    ab_tmpl_false_class_body.name = rb_intern("FalseClass");
    ab_tmpl_nil_class_body.name = rb_intern("NilClass");
    ab_tmpl_runtime_error_class_body.name = rb_intern("RuntimeError");

    INIT();
    init_builtin_methods();

    rb_cAbRuby = rb_define_class("AbRuby", rb_cObject);
    rb_define_alloc_func(rb_cAbRuby, rb_abruby_alloc);
    rb_define_method(rb_cAbRuby, "initialize", rb_abruby_initialize, 0);

    rb_cAbRubyNode = rb_define_class_under(rb_cAbRuby, "Node", rb_cObject);
    rb_undef_alloc_func(rb_cAbRubyNode);

    // Set klass field on all built-in classes (common header)
    ab_tmpl_float_class->klass   = ab_tmpl_class_class;
    ab_tmpl_array_class->klass   = ab_tmpl_class_class;
    ab_tmpl_hash_class->klass    = ab_tmpl_class_class;
    ab_tmpl_kernel_module->klass = ab_tmpl_module_class;
    ab_tmpl_module_class->klass  = ab_tmpl_class_class;
    ab_tmpl_class_class->klass   = ab_tmpl_class_class;
    ab_tmpl_object_class->klass  = ab_tmpl_class_class;
    ab_tmpl_integer_class->klass = ab_tmpl_class_class;
    ab_tmpl_string_class->klass  = ab_tmpl_class_class;
    ab_tmpl_symbol_class->klass  = ab_tmpl_class_class;
    ab_tmpl_range_class->klass   = ab_tmpl_class_class;
    ab_tmpl_regexp_class->klass  = ab_tmpl_class_class;
    ab_tmpl_rational_class->klass = ab_tmpl_class_class;
    ab_tmpl_complex_class->klass  = ab_tmpl_class_class;
    ab_tmpl_true_class->klass    = ab_tmpl_class_class;
    ab_tmpl_false_class->klass   = ab_tmpl_class_class;
    ab_tmpl_nil_class->klass     = ab_tmpl_class_class;
    ab_tmpl_runtime_error_class->klass = ab_tmpl_class_class;

    // Templates are NOT wrapped as T_DATA — they are in static memory and
    // would crash abruby_data_free (which calls ruby_xfree). Templates are
    // only accessed by Init_abruby_* for method registration, not by runtime.

    // include Kernel in Object (template): Object -> Kernel -> nil
    ab_tmpl_object_class->super = ab_tmpl_kernel_module;

    // init_builtin_consts moved to init_instance_classes (per-instance)

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
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_func_call", rb_alloc_node_func_call, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_super", rb_alloc_node_super, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_plus", rb_alloc_node_plus, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_minus", rb_alloc_node_minus, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_mul", rb_alloc_node_mul, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_div", rb_alloc_node_div, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_plus", rb_alloc_node_fixnum_plus, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_minus", rb_alloc_node_fixnum_minus, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_mul", rb_alloc_node_fixnum_mul, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_div", rb_alloc_node_fixnum_div, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_lt", rb_alloc_node_lt, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_le", rb_alloc_node_le, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_gt", rb_alloc_node_gt, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_ge", rb_alloc_node_ge, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_eq", rb_alloc_node_fixnum_eq, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_neq", rb_alloc_node_fixnum_neq, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_mod", rb_alloc_node_fixnum_mod, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_lt", rb_alloc_node_fixnum_lt, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_le", rb_alloc_node_fixnum_le, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_gt", rb_alloc_node_fixnum_gt, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_fixnum_ge", rb_alloc_node_fixnum_ge, 3);
    rb_define_singleton_method(rb_cAbRuby, "set_node_line", rb_set_node_line, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_self", rb_alloc_node_self, 0);

    // eval / dump
    rb_define_method(rb_cAbRuby, "eval_ast", rb_abruby_eval_ast, 1);
    rb_define_method(rb_cAbRuby, "current_file",  rb_abruby_get_current_file, 0);
    rb_define_method(rb_cAbRuby, "current_file=", rb_abruby_set_current_file, 1);
    rb_define_method(rb_cAbRuby, "dump_ast", rb_abruby_dump_ast, 1);

    // options
    rb_define_singleton_method(rb_cAbRuby, "verbose=", rb_astro_set_verbose, 1);
    rb_define_singleton_method(rb_cAbRuby, "compiled_only=", rb_astro_set_compiled_only, 1);
    rb_define_singleton_method(rb_cAbRuby, "verify_compiled", rb_astro_verify_compiled, 1);

    // code store
    rb_define_singleton_method(rb_cAbRuby, "cs_init", rb_astro_cs_init, 3);
    rb_define_singleton_method(rb_cAbRuby, "cs_load", rb_astro_cs_load, 1);
    rb_define_singleton_method(rb_cAbRuby, "cs_compile", rb_astro_cs_compile, 1);
    rb_define_singleton_method(rb_cAbRuby, "cs_build", rb_astro_cs_build, -1);
    rb_define_singleton_method(rb_cAbRuby, "cs_reload", rb_astro_cs_reload, 0);
    rb_define_singleton_method(rb_cAbRuby, "cs_disasm", rb_astro_cs_disasm, 1);
}
