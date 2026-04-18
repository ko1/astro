#include <ruby.h>
#include <string.h>
#include "node.h"
#include "context.h"
#include "builtin/builtin.h"
#include "astro_code_store.h"

// Sentinel entry for root frames — avoids NULL checks on frame->entry.
// stack_limit = 0 so GC marks nothing for this frame.
struct abruby_entry abruby_empty_entry = {NULL, NULL, NULL, 0};

struct abruby_option OPTION = {
    .no_compiled_code = true,
    .record_all = false,
};

static VALUE rb_cAbRuby;
static VALUE rb_cAbRubyNode;

// Built-in abruby classes (klass field = ab_tmpl_class_class, set in init)

// Template class structs.
// obj_type = struct's own type (CLASS or MODULE) — used by GC mark/free dispatch.
// instance_obj_type = type of heap objects this class creates — used by abruby_new_object.
static struct abruby_class ab_tmpl_kernel_module_body = { .obj_type = ABRUBY_OBJ_MODULE,  .instance_obj_type = ABRUBY_OBJ_GENERIC };
static struct abruby_class ab_tmpl_object_class_body;
static struct abruby_class ab_tmpl_module_class_body  = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_CLASS,         .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_class_class_body   = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_CLASS,         .super = &ab_tmpl_module_class_body };
static struct abruby_class ab_tmpl_object_class_body  = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_GENERIC };
static struct abruby_class ab_tmpl_float_class_body   = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_FLOAT,         .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_array_class_body   = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_ARRAY,         .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_hash_class_body    = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_HASH,          .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_integer_class_body = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_BIGNUM,        .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_string_class_body  = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_STRING,        .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_symbol_class_body  = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_SYMBOL,        .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_range_class_body   = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_RANGE,         .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_regexp_class_body  = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_REGEXP,        .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_rational_class_body = { .obj_type = ABRUBY_OBJ_CLASS,  .instance_obj_type = ABRUBY_OBJ_RATIONAL,      .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_complex_class_body  = { .obj_type = ABRUBY_OBJ_CLASS,  .instance_obj_type = ABRUBY_OBJ_COMPLEX,       .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_true_class_body    = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_GENERIC,       .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_false_class_body   = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_GENERIC,       .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_nil_class_body     = { .obj_type = ABRUBY_OBJ_CLASS,   .instance_obj_type = ABRUBY_OBJ_GENERIC,       .super = &ab_tmpl_object_class_body };
static struct abruby_class ab_tmpl_runtime_error_class_body = { .obj_type = ABRUBY_OBJ_CLASS, .instance_obj_type = ABRUBY_OBJ_EXCEPTION, .super = &ab_tmpl_object_class_body };

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

    switch (h->obj_type) {

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
    case ABRUBY_OBJ_SYMBOL:
        rb_gc_mark(((const struct abruby_symbol *)ptr)->rb_sym);
        break;
    case ABRUBY_OBJ_EXCEPTION: {
        const struct abruby_exception *exc = (const struct abruby_exception *)ptr;
        rb_gc_mark(exc->message);
        rb_gc_mark(exc->backtrace);
        break;
    }
    case ABRUBY_OBJ_BOUND_METHOD: {
        const struct abruby_bound_method *bm = (const struct abruby_bound_method *)ptr;
        if (!RB_SPECIAL_CONST_P(bm->recv)) rb_gc_mark(bm->recv);
        break;
    }
    case ABRUBY_OBJ_FIBER: {
        // Class structs now use obj_type=CLASS, so FIBER here means a fiber instance.
        extern void abruby_fiber_mark(struct abruby_fiber *f);
        abruby_fiber_mark((struct abruby_fiber *)ptr);
        break;
    }
    case ABRUBY_OBJ_PROC: {
        // Class structs now use obj_type=CLASS, so PROC here means a proc instance.
        const struct abruby_proc *p = (const struct abruby_proc *)ptr;
        if (!RB_SPECIAL_CONST_P(p->captured_self)) rb_gc_mark(p->captured_self);
        if (p->env) {
            for (uint32_t i = 0; i < p->env_size; i++) {
                VALUE v = p->env[i];
                if (!RB_SPECIAL_CONST_P(v)) rb_gc_mark(v);
            }
        }
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
        unsigned int n = obj->ivar_cnt;
        unsigned int inline_n = n < ABRUBY_OBJECT_INLINE_IVARS ? n : ABRUBY_OBJECT_INLINE_IVARS;
        for (unsigned int i = 0; i < inline_n; i++) {
            VALUE v = obj->ivars[i];
            // Skip the rb_gc_mark call overhead for immediates (nil, Fixnum, etc.).
            if (!RB_SPECIAL_CONST_P(v)) rb_gc_mark(v);
        }
        if (n > ABRUBY_OBJECT_INLINE_IVARS && obj->extra_ivars) {
            unsigned int extra = n - ABRUBY_OBJECT_INLINE_IVARS;
            for (unsigned int i = 0; i < extra; i++) {
                VALUE v = obj->extra_ivars[i];
                if (!RB_SPECIAL_CONST_P(v)) rb_gc_mark(v);
            }
        }
        break;
    }
    }
}

// Custom free: for class/module T_DATAs, free the nested methods/constants
// tables before freeing the struct. For generic objects, free the ivars table.
// Other types (bignum, float, string, ...) just free the struct.
// Templates (ab_tmpl_*_class_body) are static memory and never wrapped as
// T_DATA, so they are never passed to this function.
void
abruby_data_free(void *ptr)
{
    if (!ptr) return;
    const struct abruby_header *h = (const struct abruby_header *)ptr;
    switch (h->obj_type) {
    case ABRUBY_OBJ_CLASS:
    case ABRUBY_OBJ_MODULE: {
        struct abruby_class *cls = (struct abruby_class *)ptr;
        ab_id_table_free(&cls->methods);
        ab_id_table_free(&cls->constants);
        if (cls->shape_ids_by_cnt) ruby_xfree(cls->shape_ids_by_cnt);
        // Do NOT free class structs — other objects reference them via
        // klass pointers, and GC sweep order is arbitrary.  The leak
        // is bounded (one set of ~20 classes per abm instance).
        return;
    }
    case ABRUBY_OBJ_GENERIC: {
        struct abruby_object *obj = (struct abruby_object *)ptr;
        if (obj->extra_ivars) ruby_xfree(obj->extra_ivars);
        break;
    }
    case ABRUBY_OBJ_FIBER: {
        // Class structs now use obj_type=CLASS, so FIBER here is always a fiber instance.
        extern void abruby_fiber_free(struct abruby_fiber *f);
        abruby_fiber_free((struct abruby_fiber *)ptr);
        break;
    }
    case ABRUBY_OBJ_PROC: {
        // Class structs now use obj_type=CLASS, so PROC here is always a proc instance.
        struct abruby_proc *p = (struct abruby_proc *)ptr;
        if (p->env) ruby_xfree(p->env);
        break;
    }
    default:
        break;
    }
    ruby_xfree(ptr);
}

const rb_data_type_t abruby_data_type = {
    "AbRuby::Data",
    { abruby_data_mark, abruby_data_free, NULL },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

// Object creation
//
// All wrap functions use TypedData_Make_Struct which allocates the Ruby
// T_DATA object and the C struct in one step.  This avoids the GC hazard
// that exists with separate calloc + TypedData_Wrap_Struct: between the
// two calls, inner CRuby VALUEs stored in the calloc'd struct are
// invisible to the GC and can be collected if GC triggers.

// Shape table: shape_id identifies a (class, ivar_cnt) pair.  Lives on abm
// and caches back-refs in each class's shape_ids_by_cnt.
uint32_t
abruby_shape_for(struct abruby_machine *abm,
                 struct abruby_class *klass, uint32_t ivar_cnt)
{
    // Fast path: class-local cache hit.
    if (ivar_cnt < klass->shape_ids_by_cnt_capa) {
        uint32_t id = klass->shape_ids_by_cnt[ivar_cnt];
        if (id != 0) return id;
    } else {
        // Grow class-local cache.
        uint32_t new_capa = klass->shape_ids_by_cnt_capa == 0 ? 4
                                                              : klass->shape_ids_by_cnt_capa * 2;
        while (new_capa <= ivar_cnt) new_capa *= 2;
        klass->shape_ids_by_cnt =
            (uint32_t *)ruby_xrealloc(klass->shape_ids_by_cnt,
                                      new_capa * sizeof(uint32_t));
        for (uint32_t i = klass->shape_ids_by_cnt_capa; i < new_capa; i++) {
            klass->shape_ids_by_cnt[i] = 0;
        }
        klass->shape_ids_by_cnt_capa = new_capa;
    }

    // Allocate a fresh shape id.  shape_count starts at 1 (0 reserved).
    if (abm->shape_count == 0) abm->shape_count = 1;
    uint32_t id = abm->shape_count;
    if (id > ABRUBY_SHAPE_MAX) {
        // Impossibly large program — fall back to "no shape" and let
        // everything go through the slow path.
        return 0;
    }
    if (id >= abm->shape_capa) {
        uint32_t new_capa = abm->shape_capa == 0 ? 64 : abm->shape_capa * 2;
        while (new_capa <= id) new_capa *= 2;
        abm->shapes = (struct abruby_shape *)ruby_xrealloc(
            abm->shapes, new_capa * sizeof(struct abruby_shape));
        abm->shape_capa = new_capa;
    }
    abm->shapes[id].klass = klass;
    abm->shapes[id].ivar_cnt = ivar_cnt;
    klass->shape_ids_by_cnt[ivar_cnt] = id;
    abm->shape_count = id + 1;
    return id;
}

VALUE
abruby_new_object(CTX *c, struct abruby_class *klass)
{
    struct abruby_object *obj;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_object, &abruby_data_type, obj);
    obj->klass = klass;
    obj->obj_type = klass->instance_obj_type;
    unsigned int n = klass->ivar_shape.cnt;
    obj->ivar_cnt = n;
    // Initialize inline slots (mark reads them).
    unsigned int inline_n = n < ABRUBY_OBJECT_INLINE_IVARS ? n : ABRUBY_OBJECT_INLINE_IVARS;
    for (unsigned int i = 0; i < inline_n; i++) obj->ivars[i] = Qnil;
    if (n > ABRUBY_OBJECT_INLINE_IVARS) {
        unsigned int extra = n - ABRUBY_OBJECT_INLINE_IVARS;
        obj->extra_ivars = (VALUE *)ruby_xmalloc2(extra, sizeof(VALUE));
        for (unsigned int i = 0; i < extra; i++) obj->extra_ivars[i] = Qnil;
    }
    abruby_shape_id_write(wrapper, abruby_shape_for(c->abm, klass, n));
    return wrapper;
}

// Stamp an initial (class, ivar_cnt=0) shape_id on a freshly-allocated
// abruby heap object.  Every abruby heap value carries shape_id >= 1 so
// the ivar_cache fast path can compare against 0 (uninitialised ic) and
// know it is a cold miss without an extra != 0 check.
static inline void
abruby_heap_init_shape(CTX *c, VALUE wrapper, struct abruby_class *klass)
{
    abruby_shape_id_write(wrapper, abruby_shape_for(c->abm, klass, 0));
}

VALUE
abruby_bignum_new(CTX *c, VALUE rb_bignum)
{
    struct abruby_bignum *b;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_bignum, &abruby_data_type, b);
    b->klass = c->abm->integer_class;
    b->obj_type = c->abm->integer_class->instance_obj_type;
    b->rb_bignum = rb_bignum;
    abruby_heap_init_shape(c, wrapper, b->klass);
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
    f->obj_type = c->abm->float_class->instance_obj_type;
    f->rb_float = rb_float;
    abruby_heap_init_shape(c, wrapper, f->klass);
    return wrapper;
}

// String helpers

VALUE
abruby_str_new(CTX *c, VALUE rb_str)
{
    struct abruby_string *s;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_string, &abruby_data_type, s);
    s->klass = c->abm->string_class;
    s->obj_type = c->abm->string_class->instance_obj_type;
    s->rb_str = rb_str;
    abruby_heap_init_shape(c, wrapper, s->klass);
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
    return ((struct abruby_string *)ABRUBY_DATA_PTR(ab_str))->rb_str;
}

// Symbol wrapper — wraps a CRuby Symbol in T_DATA.  Static (immediate)
// symbols pass through as-is, like Fixnum.  Only heap (dynamic) symbols
// need wrapping so that AB_CLASS_OF can skip the T_SYMBOL check.
VALUE
abruby_sym_new(CTX *c, VALUE rb_sym)
{
    if (RB_STATIC_SYM_P(rb_sym)) return rb_sym;
    struct abruby_symbol *s;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_symbol, &abruby_data_type, s);
    s->klass = c->abm->symbol_class;
    s->obj_type = ABRUBY_OBJ_SYMBOL;
    s->rb_sym = rb_sym;
    abruby_heap_init_shape(c, wrapper, s->klass);
    return wrapper;
}

// Convert a stack-allocated abruby_block (or one passed via the
// current frame) into a heap Proc.  The captured locals are *copied*
// out of the original captured_fp into a fresh heap env so the Proc
// remains valid after the enclosing method returns.
VALUE
abruby_block_to_proc(CTX *c, const struct abruby_block *blk, bool is_lambda)
{
    struct abruby_proc *p;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_proc,
                                          &abruby_data_type, p);
    p->klass        = c->abm->proc_class;
    p->obj_type     = c->abm->proc_class->instance_obj_type;
    p->body         = blk->body;
    p->env_size     = blk->env_size;
    p->params_cnt   = blk->params_cnt;
    p->param_base   = blk->param_base;
    p->captured_self = blk->captured_self;
    p->cref         = blk->cref;
    p->is_lambda    = is_lambda;
    if (p->env_size > 0) {
        p->env = (VALUE *)ruby_xcalloc(p->env_size, sizeof(VALUE));
        if (blk->captured_fp) {
            for (uint32_t i = 0; i < p->env_size; i++) {
                p->env[i] = blk->captured_fp[i];
            }
        }
    }
    abruby_heap_init_shape(c, wrapper, p->klass);
    return wrapper;
}

// Wrap an abruby_fiber struct in a T_DATA so it survives GC and can
// be passed around as a Ruby VALUE.  The fiber struct's `klass` (via
// the abruby_header at offset 0) gets fiber_class so the dispatch /
// mark / free paths know what to do.
VALUE
abruby_fiber_wrap(struct abruby_machine *abm, struct abruby_fiber *f)
{
    if (f->rb_wrapper != Qnil && f->rb_wrapper != 0) return f->rb_wrapper;
    f->klass = abm->fiber_class;
    f->obj_type = abm->fiber_class->instance_obj_type;
    VALUE wrapper = TypedData_Wrap_Struct(rb_cAbRubyNode, &abruby_data_type, f);
    f->rb_wrapper = wrapper;
    abruby_shape_id_write(wrapper, abruby_shape_for(abm, f->klass, 0));
    return wrapper;
}

VALUE
abruby_bound_method_new(CTX *c, VALUE recv, ID name)
{
    struct abruby_bound_method *bm;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_bound_method,
                                          &abruby_data_type, bm);
    bm->klass = c->abm->method_class;
    bm->obj_type = c->abm->method_class->instance_obj_type;
    bm->recv = recv;
    bm->method_name = name;
    abruby_heap_init_shape(c, wrapper, bm->klass);
    return wrapper;
}

// shorthand macros
#define RSTR(v) abruby_str_rstr(v)
#define RARY(v) (((struct abruby_array *)ABRUBY_DATA_PTR(v))->rb_ary)
#define RHSH(v) (((struct abruby_hash *)ABRUBY_DATA_PTR(v))->rb_hash)
#define RSYM(v) (((struct abruby_symbol *)ABRUBY_DATA_PTR(v))->rb_sym)

VALUE
abruby_ary_new(CTX *c, VALUE rb_ary)
{
    struct abruby_array *a;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_array, &abruby_data_type, a);
    a->klass = c->abm->array_class;
    a->obj_type = c->abm->array_class->instance_obj_type;
    a->rb_ary = rb_ary;
    abruby_heap_init_shape(c, wrapper, a->klass);
    return wrapper;
}

VALUE
abruby_hash_new_wrap(CTX *c, VALUE rb_hash)
{
    struct abruby_hash *h;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_hash, &abruby_data_type, h);
    h->klass = c->abm->hash_class;
    h->obj_type = c->abm->hash_class->instance_obj_type;
    h->rb_hash = rb_hash;
    abruby_heap_init_shape(c, wrapper, h->klass);
    return wrapper;
}

VALUE
abruby_range_new(CTX *c, VALUE begin, VALUE end, bool exclude_end)
{
    struct abruby_range *r;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_range, &abruby_data_type, r);
    r->klass = c->abm->range_class;
    r->obj_type = c->abm->range_class->instance_obj_type;
    r->begin = begin;
    r->end = end;
    r->exclude_end = exclude_end;
    abruby_heap_init_shape(c, wrapper, r->klass);
    return wrapper;
}

VALUE
abruby_regexp_new(CTX *c, VALUE rb_regexp)
{
    struct abruby_regexp *r;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_regexp, &abruby_data_type, r);
    r->klass = c->abm->regexp_class;
    r->obj_type = c->abm->regexp_class->instance_obj_type;
    r->rb_regexp = rb_regexp;
    abruby_heap_init_shape(c, wrapper, r->klass);
    return wrapper;
}

VALUE
abruby_rational_new(CTX *c, VALUE rb_rational)
{
    struct abruby_rational *r;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_rational, &abruby_data_type, r);
    r->klass = c->abm->rational_class;
    r->obj_type = c->abm->rational_class->instance_obj_type;
    r->rb_rational = rb_rational;
    abruby_heap_init_shape(c, wrapper, r->klass);
    return wrapper;
}

VALUE
abruby_complex_new(CTX *c, VALUE rb_complex)
{
    struct abruby_complex *cx;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_complex, &abruby_data_type, cx);
    cx->klass = c->abm->complex_class;
    cx->obj_type = c->abm->complex_class->instance_obj_type;
    cx->rb_complex = rb_complex;
    abruby_heap_init_shape(c, wrapper, cx->klass);
    return wrapper;
}

// Class wrapper.
//
// Per-instance built-in classes are kept alive via abm_mark (which marks their
// rb_wrapper). User-defined classes are kept alive via their parent class's
// constants table (marked by abm_mark or by abruby_data_mark of the parent).
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
    return (struct abruby_class *)ABRUBY_DATA_PTR(obj);
}

// === Shared helpers and builtin infrastructure ===

void
abruby_class_set_const(struct abruby_class *klass, ID name, VALUE val)
{
    ab_id_table_insert(&klass->constants, name, val);
    if (klass->rb_wrapper) RB_OBJ_WRITTEN(klass->rb_wrapper, Qundef, val);
}

// Invoke the block attached to the lexically-enclosing method from a cfunc
// body.  Used by builtin iterators (Integer#times, Array#each, ...).
// Contract (mirrors node_yield):
//   - Reads abruby_context_frame(c)->block; raises LocalJumpError if NULL.
//   - Transfers argv[0..argc) into the block's captured fp at param_base,
//     pad-with-nil for missing, drop-extra semantics (proc-style).
//   - Swaps fp/self/current_block/current_block_frame to the captured env,
//     runs the block body, then restores.  current_frame is NOT swapped —
//     it stays as the physical call chain so non-local return skip counting
//     works.  current_block_frame is set to c->current_frame so that
//     abruby_in_block(c) returns true inside the block body but naturally
//     flips to false the moment a callee method pushes a new frame.
//   - Demotes RESULT_NEXT to NORMAL (next returns from the block).
//   - Leaves BREAK / RAISE / RETURN intact; the cfunc must return these
//     states unchanged so dispatch_method_frame_with_block demotes them.
RESULT
abruby_yield(CTX *c, unsigned int argc, VALUE *argv)
{
    const struct abruby_block *blk = abruby_context_frame(c)->block;
    if (UNLIKELY(blk == NULL)) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "no block given (yield)"));
        return (RESULT){exc, RESULT_RAISE};
    }

    // Proc-style arg binding, mirroring node_yield:
    //   - pad missing with nil, drop extras
    //   - n_params >= 2 && argc == 1 && arg0 is Array → auto-splat
    // See node.def node_yield for the full spec discussion.
    unsigned int n_params = blk->params_cnt;
    VALUE * restrict dst = blk->captured_fp + blk->param_base;
    // Fast path: single-param block with single arg (most common in
    // each/map/select).  No auto-splat needed; direct store.
    if (LIKELY(n_params == 1 && argc == 1)) {
        dst[0] = argv[0];
    }
    else if (n_params >= 2 && argc == 1 &&
            AB_CLASS_OF(c, argv[0]) == c->abm->array_class) {
        VALUE rb_ary = RARY(argv[0]);
        long len = RARRAY_LEN(rb_ary);
        unsigned int n_copy = ((unsigned long)len < n_params) ? (unsigned int)len : n_params;
        for (unsigned int i = 0; i < n_copy; i++) {
            dst[i] = RARRAY_AREF(rb_ary, i);
        }
        for (unsigned int i = n_copy; i < n_params; i++) {
            dst[i] = Qnil;
        }
    }
    else {
        unsigned int n_copy = argc < n_params ? argc : n_params;
        for (unsigned int i = 0; i < n_copy; i++) {
            dst[i] = argv[i];
        }
        for (unsigned int i = n_copy; i < n_params; i++) {
            dst[i] = Qnil;
        }
    }

    VALUE *save_fp = c->current_frame->fp;
    VALUE save_self = c->current_frame->self;
    const struct abruby_entry *save_entry = c->current_frame->entry;
    const struct abruby_block *save_current_block = c->current_block;
    const struct abruby_frame *save_current_block_frame = c->current_block_frame;

    // Copy captured closure env to a fresh area past the callee's frame
    // so block-body temporaries don't collide with callee locals.
    // Install a synthetic entry whose cref is the block's captured cref
    // so const lookups inside the block body walk the lexical scope chain
    // that was active when the block literal was created (not the cfunc's
    // own NULL entry).
    struct abruby_entry blk_entry = {blk->cref, NULL, NULL, blk->env_size};
    c->current_frame->fp = blk->captured_fp;
    c->current_frame->self = blk->captured_self;
    c->current_frame->entry = &blk_entry;
    c->current_block = blk;
    c->current_block_frame = c->current_frame;


    RESULT r = EVAL(c, blk->body);

    c->current_frame->fp = save_fp;
    c->current_frame->self = save_self;
    c->current_frame->entry = save_entry;
    c->current_block = save_current_block;
    c->current_block_frame = save_current_block_frame;

    r.state &= ~(unsigned)RESULT_NEXT;
    return r;
}

// Call an abruby method (cfunc or AST) from C code.
// Uses a generous frame offset to avoid clobbering caller's locals.
RESULT
abruby_call_method(CTX *c, VALUE recv, const struct abruby_method *method,
                   unsigned int argc, VALUE *argv)
{
    if (method->type == ABRUBY_METHOD_CFUNC) {
        return method->u.cfunc.func(c, recv, argc, argv);
    } else if (method->type == ABRUBY_METHOD_IVAR_GETTER) {
        VALUE save_self = c->current_frame->self;
        c->current_frame->self = recv;
        ID ivar_name = method->u.ivar_accessor.ivar_name;
        VALUE v = abruby_ivar_get(recv, ivar_name);
        c->current_frame->self = save_self;
        return RESULT_OK(v);
    } else if (method->type == ABRUBY_METHOD_IVAR_SETTER) {
        VALUE save_self = c->current_frame->self;
        c->current_frame->self = recv;
        ID ivar_name = method->u.ivar_accessor.ivar_name;
        VALUE v = (argc >= 1) ? argv[0] : Qnil;
        abruby_ivar_set(c, recv, ivar_name, v);
        c->current_frame->self = save_self;
        return RESULT_OK(v);
    } else {
        unsigned int gap = method->u.ast.locals_cnt;
        if (gap < 16) gap = 16;
        VALUE *new_fp = c->current_frame->fp + gap;
        for (unsigned int i = 0; i < argc; i++) {
            new_fp[i] = argv[i];
        }
        struct abruby_frame frame;
        frame.prev = c->current_frame;
        frame.caller_node = NULL;
        frame.block = NULL;
        frame.self = recv;
        frame.fp = new_fp;
        frame.entry = &method->entry;
        c->current_frame = &frame;
        RESULT r = EVAL(c, method->u.ast.body);
        c->current_frame = frame.prev;
        // Catch RETURN at this C-boundary method call.  This frame now
        // supports super lookup.  The historic wildcard-catch for non-local
        // matching; treat it as an implicit wildcard catch (the historic
        // behavior) and strip both RETURN and any residual skip-count bits.
        if (r.state & RESULT_RETURN) {
            r.state &= ~(unsigned)RESULT_RETURN_CATCH_MASK;
        }
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
    m->defining_class = klass;
    m->entry.method = m;  // self-reference so frame->entry->method works
    m->u.cfunc.func = func;
    m->u.cfunc.params_cnt = params_cnt;
    ab_id_table_insert(&klass->methods, name, (VALUE)m);
    // method_serial not bumped during init (no machine exists yet;
    // caches start at serial=0, machine starts at serial=1, so first access misses)
}

void
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

// Ensure obj has at least `new_cnt` ivar slots (growing inline + extra as needed).
void
abruby_object_grow_ivars(struct abruby_object *obj, unsigned int new_cnt)
{
    if (new_cnt <= obj->ivar_cnt) return;
    // Initialize newly-live inline slots.
    unsigned int inline_end = new_cnt < ABRUBY_OBJECT_INLINE_IVARS ? new_cnt : ABRUBY_OBJECT_INLINE_IVARS;
    for (unsigned int i = obj->ivar_cnt; i < inline_end; i++) obj->ivars[i] = Qnil;
    // Grow/allocate extra_ivars if needed.
    if (new_cnt > ABRUBY_OBJECT_INLINE_IVARS) {
        unsigned int new_extra = new_cnt - ABRUBY_OBJECT_INLINE_IVARS;
        unsigned int old_extra = obj->ivar_cnt > ABRUBY_OBJECT_INLINE_IVARS
            ? obj->ivar_cnt - ABRUBY_OBJECT_INLINE_IVARS : 0;
        if (new_extra > old_extra) {
            obj->extra_ivars = (VALUE *)ruby_xrealloc2(obj->extra_ivars, new_extra, sizeof(VALUE));
            for (unsigned int i = old_extra; i < new_extra; i++) obj->extra_ivars[i] = Qnil;
        }
    }
    obj->ivar_cnt = new_cnt;
}

static inline VALUE *
abruby_object_ivar_slot(struct abruby_object *obj, unsigned int slot)
{
    if (slot < ABRUBY_OBJECT_INLINE_IVARS) return &obj->ivars[slot];
    return &obj->extra_ivars[slot - ABRUBY_OBJECT_INLINE_IVARS];
}

VALUE
abruby_ivar_get(VALUE self, ID name)
{
    ab_verify(self);
    const struct abruby_object *obj =
        (const struct abruby_object *)ABRUBY_DATA_PTR(self);
    VALUE slot_fix;
    if (ab_id_table_lookup(&obj->klass->ivar_shape, name, &slot_fix)) {
        unsigned int slot = (unsigned int)FIX2ULONG(slot_fix);
        if (slot < obj->ivar_cnt) return abruby_object_ivar_read(obj, slot);
    }
    return Qnil;
}

void
abruby_ivar_set(CTX *c, VALUE self, ID name, VALUE val)
{
    ab_verify(self);
    if (!RB_TYPE_P(self, T_DATA)) {
        rb_raise(rb_eRuntimeError, "can't set instance variable on non-object");
    }
    struct abruby_object *obj;
    TypedData_Get_Struct(self, struct abruby_object, &abruby_data_type, obj);
    struct abruby_class *klass = (struct abruby_class *)obj->klass;
    VALUE slot_fix;
    unsigned int slot;
    if (ab_id_table_lookup(&klass->ivar_shape, name, &slot_fix)) {
        slot = (unsigned int)FIX2ULONG(slot_fix);
    } else {
        slot = klass->ivar_shape.cnt;
        ab_id_table_insert(&klass->ivar_shape, name, LONG2FIX((long)slot));
    }
    uint32_t old_cnt = obj->ivar_cnt;
    abruby_object_grow_ivars(obj, slot + 1);
    RB_OBJ_WRITE(self, abruby_object_ivar_slot(obj, slot), val);
    if (obj->ivar_cnt != old_cnt) {
        abruby_shape_id_write(self, abruby_shape_for(c->abm, klass, obj->ivar_cnt));
    }
}

// Per-instance abruby_machine state (struct defined in context.h)

// Walk the frame chain and mark each frame's VALUE slots.
// Each frame's entry->stack_limit tells GC how many slots from fp to scan.
// Slots are zero-filled at scope/prologue entry so stale reads are safe.
void
abm_mark_fiber_stack(const CTX *ctx)
{
    const VALUE *stack_lo = ctx->stack;
    const VALUE *stack_hi = ctx->stack + ABRUBY_STACK_SIZE;
    for (const struct abruby_frame *f = ctx->current_frame; f; f = f->prev) {
        if (!RB_SPECIAL_CONST_P(f->self)) rb_gc_mark(f->self);
        if (f->entry->stack_limit == 0 || !f->fp) continue;
        VALUE *base = f->fp;
        VALUE *top = f->fp + f->entry->stack_limit;
        if (base < stack_lo || base >= stack_hi) continue;
        if (top > stack_hi) top = (VALUE *)stack_hi;
        if (top > base) rb_gc_mark_locations(base, top);
    }
}

void
abm_mark(void *ptr)
{
    if (!ptr) return;
    const struct abruby_machine *abm = (const struct abruby_machine *)ptr;
    if (abm->current_fiber) {
        abm_mark_fiber_stack(&abm->current_fiber->ctx);
        if (!RB_SPECIAL_CONST_P(abm->current_fiber->rb_wrapper))
            rb_gc_mark(abm->current_fiber->rb_wrapper);
        for (struct abruby_fiber *f = abm->current_fiber->resumer; f != NULL; f = f->resumer) {
            if (!RB_SPECIAL_CONST_P(f->rb_wrapper)) rb_gc_mark(f->rb_wrapper);
            abm_mark_fiber_stack(&f->ctx);
        }
    }
    // Suspended non-main fibers are kept alive via their VALUE
    // wrapper; abruby_fiber_mark walks their per-fiber state.
    // We additionally need to mark the *root* fiber's wrapper if any.
    rb_gc_mark(abm->rb_self);
    rb_gc_mark(abm->current_file);
    rb_gc_mark(abm->loaded_files);
    rb_gc_mark(abm->loaded_asts);
    // Mark main_class method bodies and constants
    // (main_class is embedded in abm, not wrapped as T_DATA)
    const struct abruby_class *mc = &abm->main_class_body;
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
    ab_id_table_foreach(&abm->gvars, _k3, _v3, {
        rb_gc_mark(_v3);
    });
    // Mark per-instance built-in class wrappers. When the abm is collected,
    // these wrappers become unreachable and get freed via abruby_data_free.
    if (abm->kernel_module       && abm->kernel_module->rb_wrapper)        rb_gc_mark(abm->kernel_module->rb_wrapper);
    if (abm->module_class        && abm->module_class->rb_wrapper)         rb_gc_mark(abm->module_class->rb_wrapper);
    if (abm->class_class         && abm->class_class->rb_wrapper)          rb_gc_mark(abm->class_class->rb_wrapper);
    if (abm->object_class        && abm->object_class->rb_wrapper)         rb_gc_mark(abm->object_class->rb_wrapper);
    if (abm->integer_class       && abm->integer_class->rb_wrapper)        rb_gc_mark(abm->integer_class->rb_wrapper);
    if (abm->float_class         && abm->float_class->rb_wrapper)          rb_gc_mark(abm->float_class->rb_wrapper);
    if (abm->string_class        && abm->string_class->rb_wrapper)         rb_gc_mark(abm->string_class->rb_wrapper);
    if (abm->symbol_class        && abm->symbol_class->rb_wrapper)         rb_gc_mark(abm->symbol_class->rb_wrapper);
    if (abm->array_class         && abm->array_class->rb_wrapper)          rb_gc_mark(abm->array_class->rb_wrapper);
    if (abm->hash_class          && abm->hash_class->rb_wrapper)           rb_gc_mark(abm->hash_class->rb_wrapper);
    if (abm->range_class         && abm->range_class->rb_wrapper)          rb_gc_mark(abm->range_class->rb_wrapper);
    if (abm->regexp_class        && abm->regexp_class->rb_wrapper)         rb_gc_mark(abm->regexp_class->rb_wrapper);
    if (abm->rational_class      && abm->rational_class->rb_wrapper)       rb_gc_mark(abm->rational_class->rb_wrapper);
    if (abm->complex_class       && abm->complex_class->rb_wrapper)        rb_gc_mark(abm->complex_class->rb_wrapper);
    if (abm->true_class          && abm->true_class->rb_wrapper)           rb_gc_mark(abm->true_class->rb_wrapper);
    if (abm->false_class         && abm->false_class->rb_wrapper)          rb_gc_mark(abm->false_class->rb_wrapper);
    if (abm->nil_class           && abm->nil_class->rb_wrapper)            rb_gc_mark(abm->nil_class->rb_wrapper);
    if (abm->runtime_error_class && abm->runtime_error_class->rb_wrapper)  rb_gc_mark(abm->runtime_error_class->rb_wrapper);
}

#if ABRUBY_PROFILE
struct dc_entry { ID id; char name[64]; unsigned long count; };
static struct dc_entry dc_table[4096];
static unsigned int dc_n = 0;

void abruby_dispatch_count_inc(ID name) {
    for (unsigned int i = 0; i < dc_n; i++) {
        if (dc_table[i].id == name) { dc_table[i].count++; return; }
    }
    if (dc_n < 4096) {
        dc_table[dc_n].id = name;
        const char *s = rb_id2name(name);
        if (s) { strncpy(dc_table[dc_n].name, s, 63); dc_table[dc_n].name[63] = '\0'; }
        else { dc_table[dc_n].name[0] = '\0'; }
        dc_table[dc_n].count = 1;
        dc_n++;
    }
}

static int dc_cmp(const void *a, const void *b) {
    long diff = (long)((const struct dc_entry *)b)->count - (long)((const struct dc_entry *)a)->count;
    return diff > 0 ? 1 : diff < 0 ? -1 : 0;
}

void abruby_dispatch_count_dump(void) {
    if (dc_n == 0) return;
    qsort(dc_table, dc_n, sizeof(dc_table[0]), dc_cmp);
    fprintf(stderr, "\n=== dispatch counts (top 30) ===\n");
    for (unsigned int i = 0; i < dc_n && i < 30; i++) {
        fprintf(stderr, "%12lu  %s\n", dc_table[i].count, dc_table[i].name);
    }
}
#endif

void
abm_free(void *ptr)
{
    if (!ptr) return;
    struct abruby_machine *abm = (struct abruby_machine *)ptr;
    // Free embedded main_class_body's id_tables (main_class_body itself is
    // embedded in abm struct, not separately allocated).
    ab_id_table_free(&abm->main_class_body.methods);
    ab_id_table_free(&abm->main_class_body.constants);
    if (abm->main_class_body.shape_ids_by_cnt) {
        ruby_xfree(abm->main_class_body.shape_ids_by_cnt);
    }
    ab_id_table_free(&abm->gvars);
    if (abm->shapes) ruby_xfree(abm->shapes);
    // Per-instance built-in classes are T_DATA-wrapped; their structs and
    // inner tables are freed by abruby_data_free when their wrapper is GC'd.
    if (abm->current_fiber) {
        ruby_xfree(abm->current_fiber);
    }
    ruby_xfree(abm);
}

static size_t
abm_memsize(const void *ptr)
{
    return sizeof(struct abruby_machine) + sizeof(struct abruby_fiber);
}

static const rb_data_type_t abruby_machine_type = {
    "AbRuby",
    { abm_mark, abm_free, abm_memsize },
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

        const struct abruby_method *pm = parent->entry ? parent->entry->method : NULL;
        if (pm) {
            name = rb_id2name(pm->name);
        } else {
            name = parent->prev ? "<top (required)>" : "<main>";
        }
        file = (parent->entry && parent->entry->source_file)
            ? parent->entry->source_file : "(abruby)";

        rb_ary_push(bt_ary, rb_sprintf("%s:%d:in `%s'", file, line, name));
    }

    // Create exception object
    struct abruby_exception *exc;
    VALUE wrapper = TypedData_Make_Struct(rb_cAbRubyNode, struct abruby_exception,
                                          &abruby_data_type, exc);
    exc->klass = c->abm->runtime_error_class;
    exc->obj_type = c->abm->runtime_error_class->instance_obj_type;
    exc->message = message;
    exc->backtrace = bt_ary;
    abruby_heap_init_shape(c, wrapper, exc->klass);
    return wrapper;
}

// Clone a template class: copy obj_type, instance_obj_type, name, methods (not constants).
static struct abruby_class *
clone_class(const struct abruby_class *tmpl)
{
    struct abruby_class *c = ruby_xcalloc(1, sizeof(struct abruby_class));
    c->obj_type = tmpl->obj_type;               // own struct type (CLASS or MODULE)
    c->instance_obj_type = tmpl->instance_obj_type; // type of instances this class creates
    c->name = tmpl->name;
    ab_id_table_clone(&c->methods, &tmpl->methods);
    // constants are NOT cloned (they reference template wrappers)
    return c;
}

// Create per-instance copies of all built-in classes from templates.
void
init_instance_classes(struct abruby_machine *abm)
{
    // Clone all 18 template classes
    abm->kernel_module      = clone_class(ab_tmpl_kernel_module);
    abm->module_class       = clone_class(ab_tmpl_module_class);
    abm->class_class        = clone_class(ab_tmpl_class_class);
    abm->object_class       = clone_class(ab_tmpl_object_class);
    abm->integer_class      = clone_class(ab_tmpl_integer_class);
    abm->float_class        = clone_class(ab_tmpl_float_class);
    abm->string_class       = clone_class(ab_tmpl_string_class);
    abm->symbol_class       = clone_class(ab_tmpl_symbol_class);
    abm->array_class        = clone_class(ab_tmpl_array_class);
    abm->hash_class         = clone_class(ab_tmpl_hash_class);
    abm->range_class        = clone_class(ab_tmpl_range_class);
    abm->regexp_class       = clone_class(ab_tmpl_regexp_class);
    abm->rational_class     = clone_class(ab_tmpl_rational_class);
    abm->complex_class      = clone_class(ab_tmpl_complex_class);
    abm->true_class         = clone_class(ab_tmpl_true_class);
    abm->false_class        = clone_class(ab_tmpl_false_class);
    abm->nil_class          = clone_class(ab_tmpl_nil_class);
    abm->runtime_error_class = clone_class(ab_tmpl_runtime_error_class);

    // Fix up klass pointers (metaclass)
    abm->kernel_module->klass       = abm->module_class;
    abm->module_class->klass        = abm->class_class;
    abm->class_class->klass         = abm->class_class;
    abm->object_class->klass        = abm->class_class;
    abm->integer_class->klass       = abm->class_class;
    abm->float_class->klass         = abm->class_class;
    abm->string_class->klass        = abm->class_class;
    abm->symbol_class->klass        = abm->class_class;
    abm->array_class->klass         = abm->class_class;
    abm->hash_class->klass          = abm->class_class;
    abm->range_class->klass         = abm->class_class;
    abm->regexp_class->klass        = abm->class_class;
    abm->rational_class->klass      = abm->class_class;
    abm->complex_class->klass       = abm->class_class;
    abm->true_class->klass          = abm->class_class;
    abm->false_class->klass         = abm->class_class;
    abm->nil_class->klass           = abm->class_class;
    abm->runtime_error_class->klass = abm->class_class;

    // Fix up super pointers (inheritance chain)
    abm->class_class->super         = abm->module_class;
    abm->object_class->super        = abm->kernel_module;  // Object includes Kernel
    abm->integer_class->super       = abm->object_class;
    abm->float_class->super         = abm->object_class;
    abm->string_class->super        = abm->object_class;
    abm->symbol_class->super        = abm->object_class;
    abm->array_class->super         = abm->object_class;
    abm->hash_class->super          = abm->object_class;
    abm->range_class->super         = abm->object_class;
    abm->regexp_class->super        = abm->object_class;
    abm->rational_class->super      = abm->object_class;
    abm->complex_class->super       = abm->object_class;
    abm->true_class->super          = abm->object_class;
    abm->false_class->super         = abm->object_class;
    abm->nil_class->super           = abm->object_class;
    abm->runtime_error_class->super = abm->object_class;
    abm->kernel_module->super       = NULL;
    // Module inherits from Object so Class (and class-body self) can see
    // Kernel methods (`p`, `require_relative`, etc.) via the super chain:
    //   Class -> Module -> Object -> Kernel -> nil
    abm->module_class->super        = abm->object_class;

    // Wrap each per-instance class as a VALUE (for constant table, Ruby-level access)
    abruby_wrap_class(abm->kernel_module);
    abruby_wrap_class(abm->module_class);
    abruby_wrap_class(abm->class_class);
    abruby_wrap_class(abm->object_class);
    abruby_wrap_class(abm->integer_class);
    abruby_wrap_class(abm->float_class);
    abruby_wrap_class(abm->string_class);
    abruby_wrap_class(abm->symbol_class);
    abruby_wrap_class(abm->array_class);
    abruby_wrap_class(abm->hash_class);
    abruby_wrap_class(abm->range_class);
    abruby_wrap_class(abm->regexp_class);
    abruby_wrap_class(abm->rational_class);
    abruby_wrap_class(abm->complex_class);
    abruby_wrap_class(abm->true_class);
    abruby_wrap_class(abm->false_class);
    abruby_wrap_class(abm->nil_class);
    abruby_wrap_class(abm->runtime_error_class);

    // Register class name constants on per-instance Object
    struct abruby_class *obj = abm->object_class;
    abruby_class_set_const(obj, rb_intern("Object"),       abruby_wrap_class(abm->object_class));
    abruby_class_set_const(obj, rb_intern("Class"),        abruby_wrap_class(abm->class_class));
    abruby_class_set_const(obj, rb_intern("Module"),       abruby_wrap_class(abm->module_class));
    abruby_class_set_const(obj, rb_intern("Kernel"),       abruby_wrap_class(abm->kernel_module));
    abruby_class_set_const(obj, rb_intern("Integer"),      abruby_wrap_class(abm->integer_class));
    abruby_class_set_const(obj, rb_intern("Float"),        abruby_wrap_class(abm->float_class));
    abruby_class_set_const(obj, rb_intern("String"),       abruby_wrap_class(abm->string_class));
    abruby_class_set_const(obj, rb_intern("Symbol"),       abruby_wrap_class(abm->symbol_class));
    abruby_class_set_const(obj, rb_intern("Array"),        abruby_wrap_class(abm->array_class));
    abruby_class_set_const(obj, rb_intern("Hash"),         abruby_wrap_class(abm->hash_class));
    abruby_class_set_const(obj, rb_intern("Range"),        abruby_wrap_class(abm->range_class));
    abruby_class_set_const(obj, rb_intern("Regexp"),       abruby_wrap_class(abm->regexp_class));
    abruby_class_set_const(obj, rb_intern("Rational"),     abruby_wrap_class(abm->rational_class));
    abruby_class_set_const(obj, rb_intern("Complex"),      abruby_wrap_class(abm->complex_class));
    abruby_class_set_const(obj, rb_intern("TrueClass"),    abruby_wrap_class(abm->true_class));
    abruby_class_set_const(obj, rb_intern("FalseClass"),   abruby_wrap_class(abm->false_class));
    abruby_class_set_const(obj, rb_intern("NilClass"),     abruby_wrap_class(abm->nil_class));
    abruby_class_set_const(obj, rb_intern("RuntimeError"), abruby_wrap_class(abm->runtime_error_class));
    // No exception class hierarchy in abruby — alias common parent /
    // sibling classes to the single RuntimeError so user code like
    // `class X < StandardError` or `raise NotImplementedError, "msg"` parses
    // and runs.  They all share the same cfunc-level behavior (a simple
    // exception object with a message).
    abruby_class_set_const(obj, rb_intern("Exception"),        abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("StandardError"),    abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("NotImplementedError"), abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("ArgumentError"),    abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("TypeError"),        abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("NameError"),        abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("NoMethodError"),    abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("IndexError"),       abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("KeyError"),         abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("RangeError"),       abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("ZeroDivisionError"),abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("FloatDomainError"), abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("IOError"),          abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("LoadError"),        abruby_wrap_class(abm->runtime_error_class));
    abruby_class_set_const(obj, rb_intern("StopIteration"),    abruby_wrap_class(abm->runtime_error_class));

    // File: a class object whose own methods table holds the file
    // utilities (`File.join`, `File.binread`, etc.).  We use the same
    // self-referential klass trick as for Struct so that lookups via
    // AB_CLASS_OF(File) hit the methods we install here.
    {
        struct abruby_class *file_klass = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
        file_klass->klass = file_klass;  // self-reference: methods on self
        file_klass->obj_type = ABRUBY_OBJ_CLASS;
        file_klass->name = rb_intern("File");
        file_klass->super = abm->object_class;
        extern RESULT ab_file_join(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_file_binread(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_file_dirname(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_file_basename(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_file_extname(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_file_expand_path(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_file_exist_p(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_file_readable_p(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_file_read(CTX *, VALUE, unsigned int, VALUE *);
        struct {
            const char *name;
            abruby_cfunc_t fn;
        } entries[] = {
            {"join",        ab_file_join},
            {"binread",     ab_file_binread},
            {"dirname",     ab_file_dirname},
            {"basename",    ab_file_basename},
            {"extname",     ab_file_extname},
            {"expand_path", ab_file_expand_path},
            {"exist?",      ab_file_exist_p},
            {"readable?",   ab_file_readable_p},
            {"read",        ab_file_read},
        };
        for (size_t i = 0; i < sizeof(entries)/sizeof(entries[0]); i++) {
            struct abruby_method *m = ruby_xcalloc(1, sizeof(struct abruby_method));
            m->name = rb_intern(entries[i].name);
            m->type = ABRUBY_METHOD_CFUNC;
            m->defining_class = file_klass;
            m->u.cfunc.func = entries[i].fn;
            ab_id_table_insert(&file_klass->methods, m->name, (VALUE)m);
        }
        abruby_class_set_const(obj, rb_intern("File"), abruby_wrap_class(file_klass));
    }

    // Fiber: per-instance class with `new` / `resume` / `yield` (class
    // method) / `alive?` cfuncs.  Self-referential klass for the same
    // reason as Proc / Struct / File.
    {
        struct abruby_class *fb_klass = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
        fb_klass->klass = fb_klass;
        fb_klass->obj_type = ABRUBY_OBJ_CLASS;          // this is a class struct
        fb_klass->instance_obj_type = ABRUBY_OBJ_FIBER; // instances are abruby_fiber structs
        fb_klass->name = rb_intern("Fiber");
        fb_klass->super = abm->object_class;
        extern RESULT ab_fiber_new(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_fiber_resume(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_fiber_yield(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_fiber_alive_p(CTX *, VALUE, unsigned int, VALUE *);
        struct {
            const char *name;
            abruby_cfunc_t fn;
        } entries[] = {
            {"new",      ab_fiber_new},
            {"resume",   ab_fiber_resume},
            {"yield",    ab_fiber_yield},
            {"alive?",   ab_fiber_alive_p},
        };
        for (size_t i = 0; i < sizeof(entries)/sizeof(entries[0]); i++) {
            struct abruby_method *mm = ruby_xcalloc(1, sizeof(struct abruby_method));
            mm->name = rb_intern(entries[i].name);
            mm->type = ABRUBY_METHOD_CFUNC;
            mm->defining_class = fb_klass;
            mm->u.cfunc.func = entries[i].fn;
            ab_id_table_insert(&fb_klass->methods, mm->name, (VALUE)mm);
        }
        abm->fiber_class = fb_klass;
        abruby_class_set_const(obj, rb_intern("Fiber"), abruby_wrap_class(fb_klass));
    }

    // Proc: a class object whose own methods table holds the
    // Proc#call / Proc#[] cfuncs.  Instances are abruby_proc T_DATAs.
    {
        struct abruby_class *p_klass = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
        p_klass->klass = p_klass;        // self-referential so `Proc.new` finds our `new`
        p_klass->obj_type = ABRUBY_OBJ_CLASS;           // this is a class struct
        p_klass->instance_obj_type = ABRUBY_OBJ_PROC;  // instances are abruby_proc structs
        p_klass->name = rb_intern("Proc");
        p_klass->super = abm->object_class;
        extern RESULT ab_proc_new(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_proc_call(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_proc_lambda_p(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_proc_arity(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_proc_to_proc(CTX *, VALUE, unsigned int, VALUE *);
        struct {
            const char *name;
            abruby_cfunc_t fn;
        } entries[] = {
            {"new",       ab_proc_new},
            {"call",      ab_proc_call},
            {"[]",        ab_proc_call},
            {"()",        ab_proc_call},
            {"===",       ab_proc_call},  // Proc#=== invokes call (case/when match)
            {"yield",     ab_proc_call},
            {"lambda?",   ab_proc_lambda_p},
            {"arity",     ab_proc_arity},
            {"to_proc",   ab_proc_to_proc},
        };
        for (size_t i = 0; i < sizeof(entries)/sizeof(entries[0]); i++) {
            struct abruby_method *mm = ruby_xcalloc(1, sizeof(struct abruby_method));
            mm->name = rb_intern(entries[i].name);
            mm->type = ABRUBY_METHOD_CFUNC;
            mm->defining_class = p_klass;
            mm->u.cfunc.func = entries[i].fn;
            mm->u.cfunc.params_cnt = (unsigned int)-1;  // variadic
            ab_id_table_insert(&p_klass->methods, mm->name, (VALUE)mm);
        }
        abm->proc_class = p_klass;
        abruby_class_set_const(obj, rb_intern("Proc"), abruby_wrap_class(p_klass));
    }

    // Method: a class object whose own methods table holds `call` /
    // `[]` cfuncs that dispatch the bound method.  Instances are
    // abruby_bound_method T_DATAs (recv, method_name).
    {
        struct abruby_class *m_klass = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
        m_klass->klass = abm->class_class;
        m_klass->obj_type = ABRUBY_OBJ_CLASS;                    // this is a class struct
        m_klass->instance_obj_type = ABRUBY_OBJ_BOUND_METHOD;   // instances are abruby_bound_method structs
        m_klass->name = rb_intern("Method");
        m_klass->super = abm->object_class;
        extern RESULT ab_method_call(CTX *, VALUE, unsigned int, VALUE *);
        struct {
            const char *name;
            abruby_cfunc_t fn;
        } entries[] = {
            {"call", ab_method_call},
            {"[]",   ab_method_call},
            {"()",   ab_method_call},
        };
        for (size_t i = 0; i < sizeof(entries)/sizeof(entries[0]); i++) {
            struct abruby_method *mm = ruby_xcalloc(1, sizeof(struct abruby_method));
            mm->name = rb_intern(entries[i].name);
            mm->type = ABRUBY_METHOD_CFUNC;
            mm->defining_class = m_klass;
            mm->u.cfunc.func = entries[i].fn;
            ab_id_table_insert(&m_klass->methods, mm->name, (VALUE)mm);
        }
        abm->method_class = m_klass;
        abruby_class_set_const(obj, rb_intern("Method"), abruby_wrap_class(m_klass));
    }

    // GC: thin facade so user code can call GC.disable / GC.enable.
    // Delegates to CRuby's GC.
    {
        struct abruby_class *gc_klass = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
        gc_klass->klass = gc_klass;
        gc_klass->obj_type = ABRUBY_OBJ_CLASS;
        gc_klass->name = rb_intern("GC");
        gc_klass->super = abm->object_class;
        extern RESULT ab_gc_disable(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_gc_enable(CTX *, VALUE, unsigned int, VALUE *);
        extern RESULT ab_gc_start(CTX *, VALUE, unsigned int, VALUE *);
        struct {
            const char *name;
            abruby_cfunc_t fn;
        } gc_entries[] = {
            {"disable", ab_gc_disable},
            {"enable",  ab_gc_enable},
            {"start",   ab_gc_start},
        };
        for (size_t i = 0; i < sizeof(gc_entries)/sizeof(gc_entries[0]); i++) {
            struct abruby_method *m = ruby_xcalloc(1, sizeof(struct abruby_method));
            m->name = rb_intern(gc_entries[i].name);
            m->type = ABRUBY_METHOD_CFUNC;
            m->defining_class = gc_klass;
            m->u.cfunc.func = gc_entries[i].fn;
            ab_id_table_insert(&gc_klass->methods, m->name, (VALUE)m);
        }
        abruby_class_set_const(obj, rb_intern("GC"), abruby_wrap_class(gc_klass));
    }

    // Process: minimal stub class with `clock_gettime` and the
    // CLOCK_MONOTONIC constant.  Optcarrot uses these for FPS timing.
    {
        struct abruby_class *proc_klass = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
        proc_klass->klass = proc_klass;
        proc_klass->obj_type = ABRUBY_OBJ_CLASS;
        proc_klass->name = rb_intern("Process");
        proc_klass->super = abm->object_class;
        extern RESULT ab_process_clock_gettime(CTX *, VALUE, unsigned int, VALUE *);
        struct abruby_method *m_clock = ruby_xcalloc(1, sizeof(struct abruby_method));
        m_clock->name = rb_intern("clock_gettime");
        m_clock->type = ABRUBY_METHOD_CFUNC;
        m_clock->defining_class = proc_klass;
        m_clock->u.cfunc.func = ab_process_clock_gettime;
        ab_id_table_insert(&proc_klass->methods, m_clock->name, (VALUE)m_clock);
        // Process::CLOCK_MONOTONIC — symbol stub.  abruby's
        // ab_process_clock_gettime ignores the argument anyway.
        abruby_class_set_const(proc_klass, rb_intern("CLOCK_MONOTONIC"),
                               ID2SYM(rb_intern("CLOCK_MONOTONIC")));
        abruby_class_set_const(obj, rb_intern("Process"), abruby_wrap_class(proc_klass));
    }

    // ARGV: mirror CRuby's ARGV so scripts can read command-line args.
    {
        VALUE cruby_argv = rb_const_get(rb_cObject, rb_intern("ARGV"));
        long len = RARRAY_LEN(cruby_argv);
        VALUE ab_ary = rb_ary_new_capa(len);
        CTX *c0 = &abm->current_fiber->ctx;
        for (long i = 0; i < len; i++) {
            VALUE s = RARRAY_AREF(cruby_argv, i);
            rb_ary_push(ab_ary, RB_TYPE_P(s, T_STRING) ? abruby_str_new(c0, s) : s);
        }
        abruby_class_set_const(obj, rb_intern("ARGV"), abruby_ary_new(c0, ab_ary));
    }

    // ENV: empty abruby Hash stub.  abruby has no environment access; the
    // bench only does `ENV["OPTCARROT_DUMMY_RACTOR"]` to optionally enable
    // a Ractor stub, which we don't support either.  Returning nil from
    // `ENV[...]` is sufficient.
    abruby_class_set_const(obj, rb_intern("ENV"),
        abruby_hash_new_wrap(&abm->current_fiber->ctx, rb_hash_new()));

    // Struct: a class with a `new` cfunc that returns a freshly minted
    // user class with the requested ivar accessors.  See ab_struct_new
    // in builtin/class.c.
    {
        struct abruby_class *struct_klass = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
        // self-referential klass so `new` looked up on Struct goes through
        // struct_klass->methods (where ab_struct_new is registered) rather
        // than the inherited Class#new.
        struct_klass->klass = struct_klass;
        struct_klass->obj_type = ABRUBY_OBJ_CLASS;
        struct_klass->name = rb_intern("Struct");
        struct_klass->super = abm->object_class;
        // Override `new` so `Struct.new(...)` builds a new class.
        extern RESULT ab_struct_new(CTX *c, VALUE self, unsigned int argc, VALUE *argv);
        struct abruby_method *m = ruby_xcalloc(1, sizeof(struct abruby_method));
        m->name = rb_intern("new");
        m->type = ABRUBY_METHOD_CFUNC;
        m->defining_class = struct_klass;
        m->u.cfunc.func = ab_struct_new;
        m->u.cfunc.params_cnt = 0;
        ab_id_table_insert(&struct_klass->methods, m->name, (VALUE)m);
        abruby_class_set_const(obj, rb_intern("Struct"), abruby_wrap_class(struct_klass));
    }

    // Float constants. abruby_float_new_wrap passes Flonum through and only
    // wraps heap T_FLOAT in T_DATA, so it works for both representations.
    abruby_class_set_const(abm->float_class, rb_intern("INFINITY"),
        abruby_float_new_wrap(&abm->current_fiber->ctx, rb_float_new(HUGE_VAL)));
    abruby_class_set_const(abm->float_class, rb_intern("NAN"),
        abruby_float_new_wrap(&abm->current_fiber->ctx, rb_float_new(nan(""))));
}

void
init_abm(struct abruby_machine *abm)
{
    abm->method_serial = 1;
    abm->current_fiber = ruby_xcalloc(1, sizeof(struct abruby_fiber));
    abm->current_fiber->is_main = true;
    abm->current_fiber->state = ABRUBY_FIBER_RUNNING;
    abm->current_fiber->abm = abm;
    abm->current_fiber->crb_fiber = Qnil;
    abm->current_fiber->rb_wrapper = Qnil;
    // Wire ctx.abm early so init_instance_classes can use abruby_float_new_wrap
    // (which reads c->abm->float_class) when creating Float constants.
    abm->current_fiber->ctx.abm = abm;
    /* sp removed */
    // Set up root frame for this fiber
    memset(&abm->current_fiber->root_frame, 0, sizeof(struct abruby_frame));
    abm->current_fiber->root_frame.fp = abm->current_fiber->ctx.stack;
    abm->current_fiber->root_frame.entry = &abruby_empty_entry;
    abm->current_fiber->ctx.current_frame = &abm->current_fiber->root_frame;

    // Create per-instance built-in classes (must be before main_class_body setup)
    init_instance_classes(abm);

    // Now that fiber_class exists, tag the main fiber so abruby_header
    // dispatches via OBJ_FIBER on it (rare; only if it's ever wrapped).
    abm->current_fiber->klass = abm->fiber_class;
    abm->current_fiber->obj_type = abm->fiber_class->instance_obj_type;
    abm->root_fiber = abm->current_fiber;

    // Per-instance main class (inherits from Object)
    abm->main_class_body.klass = abm->class_class;
    abm->main_class_body.obj_type = ABRUBY_OBJ_CLASS;
    abm->main_class_body.instance_obj_type = ABRUBY_OBJ_GENERIC;
    abm->main_class_body.name = rb_intern("main");
    abm->main_class_body.super = abm->object_class;

    abm->current_fiber->root_frame.fp = abm->current_fiber->ctx.stack;
    /* sp removed */
    abm->current_fiber->root_frame.self = abruby_new_object(&abm->current_fiber->ctx, &abm->main_class_body);

    abm->current_fiber->ctx.current_class = NULL;
    abm->current_fiber->root_frame.entry = &abruby_empty_entry;
    abm->id_cache.op_plus = rb_intern("+");
    abm->id_cache.op_minus = rb_intern("-");
    abm->id_cache.op_mul = rb_intern("*");
    abm->id_cache.op_div = rb_intern("/");
    abm->id_cache.op_lt = rb_intern("<");
    abm->id_cache.op_le = rb_intern("<=");
    abm->id_cache.op_gt = rb_intern(">");
    abm->id_cache.op_ge = rb_intern(">=");
    abm->id_cache.op_eq = rb_intern("==");
    abm->id_cache.op_mod = rb_intern("%");
    abm->id_cache.op_aref = rb_intern("[]");
    abm->id_cache.op_aset = rb_intern("[]=");
    abm->id_cache.op_ltlt = rb_intern("<<");
    abm->id_cache.op_and = rb_intern("&");
    abm->id_cache.op_or = rb_intern("|");
    abm->id_cache.op_xor = rb_intern("^");
    abm->id_cache.op_gtgt = rb_intern(">>");
    abm->id_cache.method_missing = rb_intern("method_missing");
    abm->id_cache.initialize = rb_intern("initialize");
    abm->current_fiber->ctx.ids = &abm->id_cache;
    abm->current_fiber->ctx.abm = abm;
    abm->rb_self = Qnil;
    abm->current_file = Qnil;
    abm->loaded_files = rb_ary_new();
    abm->loaded_asts  = rb_ary_new();

    for (int i = 0; i < ABRUBY_STACK_SIZE; i++) {
        abm->current_fiber->ctx.stack[i] = Qnil;
    }
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

// Wrap any process-stable immediate VALUE (Fixnum, Flonum, Symbol,
// true/false/nil) on a node_literal.  The caller — lib/abruby.rb parser
// — is responsible for ensuring `v` is actually an immediate; we assert
// here under ABRUBY_DEBUG.
static VALUE
rb_alloc_node_literal(VALUE self, VALUE v)
{
#if ABRUBY_DEBUG
    if (!RB_SPECIAL_CONST_P(v)) {
        rb_raise(rb_eArgError,
                 "alloc_node_literal requires an immediate VALUE (got heap object)");
    }
#endif
    return wrap_node(ALLOC_node_literal((uint64_t)v));
}

static VALUE
rb_alloc_node_bignum_new(VALUE self, VALUE str)
{
    const char *cstr = strdup(StringValueCStr(str));
    return wrap_node(ALLOC_node_bignum_new(cstr));
}

static VALUE
rb_alloc_node_float_new(VALUE self, VALUE num)
{
    double val = NUM2DBL(num);
    // Common case: the literal fits in the CRuby flonum encoding, so we
    // can store the pre-computed VALUE directly on the node and let EVAL
    // return it with no arithmetic.  The encoded flonum is a pure
    // function of the double, so the operand is process-stable and the
    // code store serializes it as a plain C integer literal.
    VALUE flo = ab_flonum_encode(val);
    if (LIKELY(flo != 0)) {
        return wrap_node(ALLOC_node_literal((uint64_t)flo));
    }
    // Out-of-range doubles (NaN/Inf/denormals/0x3000_…) keep the
    // heap-wrapper path; node_float_new EVAL pays a rb_float_new call
    // but literals of this kind are rare in real source.
    return wrap_node(ALLOC_node_float_new(val));
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

// Return the tail field of a node_seq (for parser-side opt_pc chain walking).
// Returns nil if the node is not a node_seq.
extern const struct NodeKind kind_node_seq;
static VALUE
rb_node_seq_tail(VALUE self, VALUE node_val)
{
    NODE *n = unwrap_node(node_val);
    if (!n || n->head.kind != &kind_node_seq) return Qnil;
    return wrap_node(n->u.node_seq.tail);
}

static VALUE
rb_alloc_node_not(VALUE self, VALUE expr)
{
    return wrap_node(ALLOC_node_not(unwrap_node(expr)));
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
rb_alloc_node_def(VALUE self, VALUE name, VALUE body,
                  VALUE required_params_cnt, VALUE params_cnt, VALUE post_params_cnt,
                  VALUE rest_index, VALUE locals_cnt)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_def(cname, unwrap_node(body),
                     FIX2UINT(required_params_cnt), FIX2UINT(params_cnt),
                     FIX2UINT(post_params_cnt),
                     FIX2INT(rest_index), FIX2UINT(locals_cnt)));
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
rb_alloc_node_super(VALUE self, VALUE params_cnt, VALUE arg_index)
{
    return wrap_node(ALLOC_node_super(FIX2UINT(params_cnt), FIX2UINT(arg_index)));
}

// === Block support ===

static VALUE
rb_alloc_node_block_literal(VALUE self, VALUE body, VALUE params_cnt, VALUE param_base, VALUE env_size)
{
    return wrap_node(ALLOC_node_block_literal(unwrap_node(body),
                                              FIX2UINT(params_cnt),
                                              FIX2UINT(param_base),
                                              FIX2UINT(env_size)));
}

static VALUE
rb_alloc_node_block_arg(VALUE self, VALUE expr)
{
    return wrap_node(ALLOC_node_block_arg(unwrap_node(expr)));
}

// === Consolidated call nodes ===

static VALUE
rb_alloc_node_call0(VALUE self, VALUE recv, VALUE name, VALUE arg_index)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_call0(unwrap_node(recv), cname, FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_call1(VALUE self, VALUE recv, VALUE name, VALUE arg0, VALUE arg_index)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_call1(unwrap_node(recv), cname, unwrap_node(arg0),
                                       FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_call2(VALUE self, VALUE recv, VALUE name, VALUE arg0, VALUE arg1, VALUE arg_index)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_call2(unwrap_node(recv), cname, unwrap_node(arg0), unwrap_node(arg1),
                                       FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_call0_b(VALUE self, VALUE recv, VALUE name, VALUE arg_index, VALUE blk)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_call0_b(unwrap_node(recv), cname,
                                         FIX2UINT(arg_index), unwrap_node(blk)));
}

static VALUE
rb_alloc_node_call1_b(VALUE self, VALUE recv, VALUE name, VALUE arg0, VALUE arg_index, VALUE blk)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_call1_b(unwrap_node(recv), cname, unwrap_node(arg0),
                                         FIX2UINT(arg_index), unwrap_node(blk)));
}

static VALUE
rb_alloc_node_call2_b(VALUE self, VALUE recv, VALUE name, VALUE arg0, VALUE arg1, VALUE arg_index, VALUE blk)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_call2_b(unwrap_node(recv), cname, unwrap_node(arg0), unwrap_node(arg1),
                                         FIX2UINT(arg_index), unwrap_node(blk)));
}

static VALUE
rb_alloc_node_call(VALUE self, VALUE recv, VALUE name, VALUE argc, VALUE arg_index, VALUE blk)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_call(unwrap_node(recv), cname,
                                      FIX2UINT(argc), FIX2UINT(arg_index), unwrap_node(blk)));
}

static VALUE
rb_alloc_node_apply_call(VALUE self, VALUE recv, VALUE name, VALUE args_node, VALUE arg_index, VALUE blk)
{
    ID cname = rb_intern_str(name);
    return wrap_node(ALLOC_node_apply_call(unwrap_node(recv), cname, unwrap_node(args_node),
                                            FIX2UINT(arg_index), unwrap_node(blk)));
}

static VALUE
rb_alloc_node_block_param(VALUE self, VALUE lvar_index)
{
    return wrap_node(ALLOC_node_block_param(FIX2UINT(lvar_index)));
}

static VALUE
rb_alloc_node_yield(VALUE self, VALUE argc, VALUE arg_index)
{
    return wrap_node(ALLOC_node_yield(FIX2UINT(argc), FIX2UINT(arg_index)));
}

static VALUE
rb_alloc_node_next(VALUE self, VALUE value)
{
    return wrap_node(ALLOC_node_next(unwrap_node(value)));
}

static VALUE
rb_alloc_node_super_with_block(VALUE self, VALUE params_cnt, VALUE arg_index, VALUE block_literal)
{
    return wrap_node(ALLOC_node_super_with_block(FIX2UINT(params_cnt), FIX2UINT(arg_index),
                                                 unwrap_node(block_literal)));
}


static VALUE
rb_set_node_line(VALUE self, VALUE node_obj, VALUE line)
{
    NODE *n = unwrap_node(node_obj);
    if (n) n->head.line = FIX2INT(line);
    return node_obj;
}

// Binary-op node alloc wrappers (arith / compare / mod).
// All share signature (left, right, arg_index) and only differ in the
// ALLOC_node_* macro they invoke. X-macro keeps declarations and method
// registrations in lock-step.
#define ABRUBY_BINOP_NODES(X) \
    X(plus)        X(minus)       X(mul)         X(div)         \
    X(fixnum_plus) X(fixnum_minus) X(fixnum_mul) X(fixnum_div) \
    X(lt)          X(le)          X(gt)          X(ge)          \
    X(fixnum_lt)   X(fixnum_le)   X(fixnum_gt)   X(fixnum_ge)   \
    X(fixnum_eq)   X(fixnum_neq)  X(fixnum_mod)                 \
    X(array_aref)  X(array_ltlt)                                \
    X(and)         X(fixnum_and)  X(or)          X(fixnum_or)   \
    X(xor)         X(fixnum_xor)  X(gtgt)        X(fixnum_gtgt)

#define ABRUBY_DEFINE_BINOP_WRAPPER(name) \
    static VALUE rb_alloc_node_##name(VALUE self, VALUE left, VALUE right, VALUE arg_index) { \
        return wrap_node(ALLOC_node_##name(unwrap_node(left), unwrap_node(right), FIX2UINT(arg_index))); \
    }
ABRUBY_BINOP_NODES(ABRUBY_DEFINE_BINOP_WRAPPER)
#undef ABRUBY_DEFINE_BINOP_WRAPPER

// node_array_aset: 3-operand + arg_index (recv, idx, value, arg_index).
static VALUE
rb_alloc_node_array_aset(VALUE self, VALUE recv, VALUE idx, VALUE value, VALUE arg_index)
{
    return wrap_node(ALLOC_node_array_aset(unwrap_node(recv), unwrap_node(idx),
                                           unwrap_node(value), FIX2UINT(arg_index)));
}

// Convert abruby value to Ruby value for returning to CRuby world
static VALUE
abruby_to_ruby(VALUE v)
{
    if (RB_TYPE_P(v, T_DATA) && RTYPEDDATA_P(v) &&
        RTYPEDDATA_TYPE(v) == &abruby_data_type) {
        const struct abruby_header *h = (const struct abruby_header *)ABRUBY_DATA_PTR(v);
        if (!h->klass) return v;
        switch (h->obj_type) {
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
        case ABRUBY_OBJ_SYMBOL:
            return ((const struct abruby_symbol *)h)->rb_sym;
        default:
            break;
        }
    }
    return v;
}

// require helper: load and eval a file in the abm context
// Returns Qtrue if loaded, Qfalse if already loaded.
RESULT
abruby_require_file(CTX *c, VALUE rb_path)
{
    struct abruby_machine *abm = c->abm;

    // Resolve to absolute path
    VALUE abs_path = rb_file_expand_path(rb_path, Qnil);
    VALUE abs_str = rb_funcall(abs_path, rb_intern("to_s"), 0);

    // Check if already loaded
    if (RTEST(rb_funcall(abm->loaded_files, rb_intern("include?"), 1, abs_str))) {
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

    // Mark as loaded, and retain the AST for the lifetime of the abm so
    // method bodies (raw NODE pointers stored on abruby_method) stay
    // live across GC cycles.
    rb_ary_push(abm->loaded_files, abs_str);
    rb_ary_push(abm->loaded_asts, ast_obj);

    // Save/restore current_file
    VALUE save_file = abm->current_file;
    abm->current_file = abs_str;

    // Eval AST
    NODE *ast = unwrap_node(ast_obj);
    struct abruby_frame *save_frame = c->current_frame;
    struct abruby_class *save_class = c->current_class;
    struct abruby_entry req_entry = {NULL, RSTRING_PTR(abs_str), NULL, 0};
    struct abruby_frame req_frame;
    req_frame.prev = save_frame;
    req_frame.caller_node = NULL;
    req_frame.block = NULL;
    req_frame.self = abruby_new_object(c, &abm->main_class_body);
    req_frame.fp = c->stack;
    req_frame.entry = &req_entry;
    c->current_frame = &req_frame;
    c->current_class = NULL;
    RESULT r = EVAL(c, ast);
    c->current_frame = save_frame;
    c->current_class = save_class;

    abm->current_file = save_file;

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

    // Retain the AST for the lifetime of the abm (see abruby_require_file).
    rb_ary_push(c->abm->loaded_asts, ast_obj);

    // Eval AST in current context
    NODE *ast = unwrap_node(ast_obj);
    RESULT r = EVAL(c, ast);
    return r;
}

// Get current file path from abm
VALUE
abruby_current_file(const CTX *c)
{
    return c->abm->current_file;
}

// AbRuby#initialize
static VALUE
rb_abruby_initialize(VALUE self)
{
    // Allocate and assign DATA_PTR BEFORE calling init_abm (which may trigger
    // GC via rb_ary_new / TypedData_Make_Struct inside init_instance_classes).
    // With DATA_PTR set early, abm_mark receives a valid pointer instead of NULL.
    struct abruby_machine *abm = ruby_xcalloc(1, sizeof(struct abruby_machine));
    DATA_PTR(self) = abm;
    init_abm(abm);
    abm->rb_self = self;
    return self;
}

static VALUE
rb_abruby_get_current_file(VALUE self)
{
    struct abruby_machine *abm;
    TypedData_Get_Struct(self, struct abruby_machine, &abruby_machine_type, abm);
    return abm->current_file;
}

static VALUE
rb_abruby_set_current_file(VALUE self, VALUE path)
{
    struct abruby_machine *abm;
    TypedData_Get_Struct(self, struct abruby_machine, &abruby_machine_type, abm);
    abm->current_file = NIL_P(path) ? Qnil : rb_file_expand_path(path, Qnil);
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
    struct abruby_machine *abm;
    TypedData_Get_Struct(self, struct abruby_machine, &abruby_machine_type, abm);

    NODE *ast = unwrap_node(ast_obj);
    if (ast == NULL) {
        rb_raise(rb_eRuntimeError, "cannot eval NULL AST");
    }

    // Retain the AST for the lifetime of the abm so method bodies defined
    // inside it stay live even after this eval returns.  (Otherwise GC
    // collects the NODE T_DATA and abruby_method bodies become dangling.)
    rb_ary_push(abm->loaded_asts, ast_obj);

    // Reset stack for each eval.  Clear the full stack so GC frame-walk
    // never sees freed T_DATA pointers from a previous eval.
    memset(abm->current_fiber->ctx.stack, 0,
           ABRUBY_STACK_SIZE * sizeof(VALUE));
    abm->current_fiber->root_frame.fp = abm->current_fiber->ctx.stack;
    abm->current_fiber->ctx.current_frame = &abm->current_fiber->root_frame;
    abm->current_fiber->ctx.current_class = NULL;

    // Push <main> frame so backtrace always has a bottom frame.
    // Inherit self/fp from root frame.
    const char *eval_file = NIL_P(abm->current_file) ? "(abruby)" : RSTRING_PTR(abm->current_file);
    struct abruby_frame *rf = &abm->current_fiber->root_frame;
    struct abruby_entry main_entry = {NULL, eval_file, NULL, 0};
    struct abruby_frame main_frame;
    main_frame.prev = NULL;
    main_frame.caller_node = NULL;
    main_frame.block = NULL;
    main_frame.self = rf->self;
    main_frame.fp = rf->fp;
    main_frame.entry = &main_entry;
    abm->current_fiber->ctx.current_frame = &main_frame;

    RESULT r = EVAL(&abm->current_fiber->ctx, ast);
    abm->current_fiber->ctx.current_frame = &abm->current_fiber->root_frame;
    if (r.state == RESULT_RAISE) {
        VALUE exc_val = r.value;
        // Extract message and backtrace from exception object
        if (RB_TYPE_P(exc_val, T_DATA) && RTYPEDDATA_P(exc_val) &&
            RTYPEDDATA_TYPE(exc_val) == &abruby_data_type) {
            const struct abruby_header *h = (const struct abruby_header *)ABRUBY_DATA_PTR(exc_val);
            if (h->klass && h->obj_type == ABRUBY_OBJ_EXCEPTION) {
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

// AbRuby.cs_load(node, file = nil) → true/false
// file is used for PGC (Hopt) index lookup; pass nil for AOT-only.
static VALUE
rb_astro_cs_load(int argc, VALUE *argv, VALUE self)
{
    VALUE node_val, file_val;
    rb_scan_args(argc, argv, "11", &node_val, &file_val);
    NODE *n = DATA_PTR(node_val);
    const char *file = NIL_P(file_val) ? NULL : StringValueCStr(file_val);
    return astro_cs_load(n, file) ? Qtrue : Qfalse;
}

// AbRuby.cs_compile(node, file = nil)
// file != nil switches to PGC mode (SD_<Hopt>.c + index entry).
static VALUE
rb_astro_cs_compile(int argc, VALUE *argv, VALUE self)
{
    VALUE node_val, file_val;
    rb_scan_args(argc, argv, "11", &node_val, &file_val);
    NODE *n = DATA_PTR(node_val);
    const char *file = NIL_P(file_val) ? NULL : StringValueCStr(file_val);
    astro_cs_compile(n, file);
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

// AbRuby.horg(node) → Integer — structural (Horg) hash of node
static VALUE
rb_astro_horg(VALUE self, VALUE node_val)
{
    NODE *n = DATA_PTR(node_val);
    return ULL2NUM((unsigned long long)HORG(n));
}

// AbRuby.hopt(node) → Integer — profile-aware (Hopt) hash of node
static VALUE
rb_astro_hopt(VALUE self, VALUE node_val)
{
    NODE *n = DATA_PTR(node_val);
    return ULL2NUM((unsigned long long)HOPT(n));
}

// AbRuby.has_profile?(node) → true iff any descendant has a filled
// method_cache (distinguishes real runtime profile from parse-time node
// specialisation).  Used by iabrb --pgc to pick PGSD_ vs SD_.
static VALUE
rb_astro_has_profile_p(VALUE self, VALUE node_val)
{
    NODE *n = DATA_PTR(node_val);
    if (!n || !n->head.kind || !n->head.kind->profile_func) return Qfalse;
    return n->head.kind->profile_func(n) ? Qtrue : Qfalse;
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

    // Expose the build-time ABRUBY_DEBUG flag so Ruby callers (exe/abruby's
    // cs_build helper) can compile SD_*.c with a matching NodeHead layout.
    rb_define_const(rb_cAbRuby, "DEBUG", ABRUBY_DEBUG ? Qtrue : Qfalse);

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
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_literal", rb_alloc_node_literal, 1);
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
    rb_define_singleton_method(rb_cAbRuby, "node_seq_tail", rb_node_seq_tail, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_not", rb_alloc_node_not, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_if", rb_alloc_node_if, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_while", rb_alloc_node_while, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_return", rb_alloc_node_return, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_break", rb_alloc_node_break, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_rescue", rb_alloc_node_rescue, 4);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_def", rb_alloc_node_def, 7);


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
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_super", rb_alloc_node_super, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_block_literal", rb_alloc_node_block_literal, 4);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_block_arg", rb_alloc_node_block_arg, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_call0", rb_alloc_node_call0, 3);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_call1", rb_alloc_node_call1, 4);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_call2", rb_alloc_node_call2, 5);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_call0_b", rb_alloc_node_call0_b, 4);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_call1_b", rb_alloc_node_call1_b, 5);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_call2_b", rb_alloc_node_call2_b, 6);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_call", rb_alloc_node_call, 5);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_apply_call", rb_alloc_node_apply_call, 5);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_block_param", rb_alloc_node_block_param, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_yield", rb_alloc_node_yield, 2);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_next", rb_alloc_node_next, 1);
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_super_with_block", rb_alloc_node_super_with_block, 3);
#define ABRUBY_REGISTER_BINOP_WRAPPER(name) \
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_" #name, rb_alloc_node_##name, 3);
    ABRUBY_BINOP_NODES(ABRUBY_REGISTER_BINOP_WRAPPER)
#undef ABRUBY_REGISTER_BINOP_WRAPPER
    rb_define_singleton_method(rb_cAbRuby, "alloc_node_array_aset", rb_alloc_node_array_aset, 4);
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
    rb_define_singleton_method(rb_cAbRuby, "cs_load", rb_astro_cs_load, -1);
    rb_define_singleton_method(rb_cAbRuby, "cs_compile", rb_astro_cs_compile, -1);
    rb_define_singleton_method(rb_cAbRuby, "cs_build", rb_astro_cs_build, -1);
    rb_define_singleton_method(rb_cAbRuby, "cs_reload", rb_astro_cs_reload, 0);
    rb_define_singleton_method(rb_cAbRuby, "horg", rb_astro_horg, 1);
    rb_define_singleton_method(rb_cAbRuby, "hopt", rb_astro_hopt, 1);
    rb_define_singleton_method(rb_cAbRuby, "has_profile?", rb_astro_has_profile_p, 1);
    rb_define_singleton_method(rb_cAbRuby, "cs_disasm", rb_astro_cs_disasm, 1);

#if ABRUBY_PROFILE
    {
        extern void abruby_dispatch_count_dump(void);
        atexit(abruby_dispatch_count_dump);
    }
#endif
}
