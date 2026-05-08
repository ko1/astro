// arjsv — JSON Schema validator as a CRuby extension built on ASTro.
//
// Ruby surface:
//   Arjsv.schema(hash)       => Arjsv::Schema
//   schema.valid?(data)      => true / false
//   schema.compile!          => self  (AOT specialise the AST)
//
// AST construction lives on the Ruby side (lib/arjsv.rb) and calls back
// through the `_alloc_*` module functions defined here.  Each NODE is wrapped
// as a T_DATA (arjsv_node_type) so child references via NODE * keep parent
// trees alive through ordinary GC marking.  A Schema holds the root NODE
// wrapper plus a Ruby Array of constants used by enum / const lookups.

#include <ruby.h>
#include <ruby/version.h>
#include "node.h"
#include "context.h"
#include "astro_code_store.h"

struct arjsv_option OPTION = {
    .quiet = true,
    .no_compiled_code = false,
    .disasm = false,
    .record_all = false,
};

static VALUE rb_mArjsv;
static VALUE rb_cArjsvSchema;
static VALUE rb_cArjsvNode;

// ---- NODE wrapper --------------------------------------------------------

void
arjsv_node_mark(void *ptr)
{
    NODE *n = (NODE *)ptr;
    if (n == NULL) return;
    if (n->head.kind && n->head.kind->marker) {
        n->head.kind->marker(n);
    }
}

static void
arjsv_node_free(void *ptr)
{
    free(ptr);
}

const rb_data_type_t arjsv_node_type = {
    "Arjsv::Node",
    { arjsv_node_mark, arjsv_node_free, NULL, },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE
arjsv_wrap_node(NODE *n)
{
    if (n == NULL) return Qnil;
    if (n->head.rb_wrapper) return n->head.rb_wrapper;
    VALUE w = TypedData_Wrap_Struct(rb_cArjsvNode, &arjsv_node_type, n);
    n->head.rb_wrapper = w;
    return w;
}

NODE *
arjsv_unwrap_node(VALUE v)
{
    if (NIL_P(v)) return NULL;
    NODE *n;
    TypedData_Get_Struct(v, NODE, &arjsv_node_type, n);
    return n;
}

// ---- Schema --------------------------------------------------------------

struct arjsv_schema {
    NODE *root;
    VALUE root_wrapper;  // keeps root NODE alive (also part of `entries`)
    VALUE consts;        // Ruby Array of constant VALUEs (enum / const / fstring keys / regex / $defs targets)
    VALUE entries;       // Ruby Array of NODE wrappers — root + secondary entries (e.g. $defs targets, schemas reached via callbacks)
    bool compiled;
};

static void
schema_mark(void *ptr)
{
    struct arjsv_schema *s = (struct arjsv_schema *)ptr;
    rb_gc_mark(s->root_wrapper);
    rb_gc_mark(s->consts);
    rb_gc_mark(s->entries);
}

static void
schema_free(void *ptr)
{
    free(ptr);
}

static size_t
schema_size(const void *ptr)
{
    (void)ptr;
    return sizeof(struct arjsv_schema);
}

static const rb_data_type_t arjsv_schema_type = {
    "Arjsv::Schema",
    { schema_mark, schema_free, schema_size, },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY,
};

static struct arjsv_schema *
schema_get(VALUE self)
{
    struct arjsv_schema *s;
    TypedData_Get_Struct(self, struct arjsv_schema, &arjsv_schema_type, s);
    return s;
}

// Public: build a Schema from (root_node, consts, entries).  `entries`
// includes the root plus any secondary entries (e.g. $defs targets,
// schemas reached through callbacks like additionalProperties) that need
// independent astro_cs_compile registration.
static VALUE
rb_arjsv_schema_new(VALUE klass, VALUE root_node_v, VALUE consts_v, VALUE entries_v)
{
    NODE *root = arjsv_unwrap_node(root_node_v);
    if (root == NULL) {
        rb_raise(rb_eArgError, "Arjsv::Schema.new: root node is nil");
    }
    Check_Type(consts_v, T_ARRAY);
    Check_Type(entries_v, T_ARRAY);

    struct arjsv_schema *s;
    VALUE obj = TypedData_Make_Struct(klass, struct arjsv_schema,
                                      &arjsv_schema_type, s);
    s->root = root;
    s->root_wrapper = root_node_v;
    s->consts = rb_ary_freeze(rb_ary_dup(consts_v));
    s->entries = rb_ary_freeze(rb_ary_dup(entries_v));
    s->compiled = false;
    return obj;
}

// Public: validate `data` against the schema.  Returns true / false.
static VALUE
rb_arjsv_schema_valid_p(VALUE self, VALUE data)
{
    struct arjsv_schema *s = schema_get(self);
    CTX ctx;
    ctx.data = data;
    ctx.root_data = data;
    ctx.consts = RARRAY_LEN(s->consts) > 0 ? RARRAY_CONST_PTR(s->consts) : NULL;
    ctx.one_of_count = 0;
    ctx.one_of_active = 0;
    int ok = EVAL(&ctx, s->root);
    return ok ? Qtrue : Qfalse;
}

// Public: AOT specialise the schema's AST.  Idempotent.  Subsequent
// `valid?` calls hit the specialised dispatcher.
//
// `cflags` is appended to the cc invocation that builds code_store/all.so.
// SD source includes <ruby.h> (via context.h), so the Ruby caller must pass
// at least the Ruby header search paths — see lib/arjsv.rb#compile!.
static VALUE
rb_arjsv_schema_compile_bang(int argc, VALUE *argv, VALUE self)
{
    struct arjsv_schema *s = schema_get(self);
    VALUE cflags_val;
    rb_scan_args(argc, argv, "01", &cflags_val);
    const char *cflags = NIL_P(cflags_val) ? NULL : StringValueCStr(cflags_val);

    if (s->compiled) return self;
    if (OPTION.no_compiled_code) {
        s->compiled = true;
        return self;
    }
    long n = RARRAY_LEN(s->entries);
    bool any_to_compile = false;
    for (long i = 0; i < n; i++) {
        NODE *e = arjsv_unwrap_node(RARRAY_AREF(s->entries, i));
        if (e && !e->head.flags.is_specialized) {
            astro_cs_compile(e, NULL);
            any_to_compile = true;
        }
    }
    if (any_to_compile) {
        astro_cs_build(cflags);
        astro_cs_reload();
        for (long i = 0; i < n; i++) {
            NODE *e = arjsv_unwrap_node(RARRAY_AREF(s->entries, i));
            if (e) astro_cs_load(e, NULL);
        }
    }
    s->compiled = true;
    return self;
}

// Diagnostic: dump the schema's AST to stderr (one-line form).
static VALUE
rb_arjsv_schema_dump(VALUE self)
{
    struct arjsv_schema *s = schema_get(self);
    DUMP(stderr, s->root, true);
    fprintf(stderr, "\n");
    return self;
}

// ---- ALLOC wrappers ------------------------------------------------------

static VALUE
rb_alloc_validate_root(VALUE self, VALUE body)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_validate_root(arjsv_unwrap_node(body)));
}

static VALUE
rb_alloc_pass(VALUE self)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_pass());
}

static VALUE
rb_alloc_fail(VALUE self)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_fail());
}

static VALUE
rb_alloc_seq(VALUE self, VALUE head, VALUE tail)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_seq(arjsv_unwrap_node(head),
                                          arjsv_unwrap_node(tail)));
}

static VALUE
rb_alloc_type_check(VALUE self, VALUE mask)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_type_check(NUM2UINT(mask)));
}

// strdup the key so the NODE owns a stable pointer regardless of the
// caller-side String's GC lifetime.  Currently we never free it (NODE
// lifetime == Schema lifetime, leak is bounded by schema count).
static const char *
arjsv_dup_key(VALUE key_str)
{
    Check_Type(key_str, T_STRING);
    return strdup(StringValueCStr(key_str));
}

// `key` provides the content-hashed C-string for SD specialization;
// `key_idx` indexes into the schema's consts array where a frozen Ruby
// String (the same content) is parked for runtime hash lookup.
static VALUE
rb_alloc_required(VALUE self, VALUE key, VALUE key_idx, VALUE next)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_required(arjsv_dup_key(key),
                                               NUM2UINT(key_idx),
                                               arjsv_unwrap_node(next)));
}

static VALUE
rb_alloc_property(VALUE self, VALUE key, VALUE key_idx, VALUE schema, VALUE next)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_property(arjsv_dup_key(key),
                                               NUM2UINT(key_idx),
                                               arjsv_unwrap_node(schema),
                                               arjsv_unwrap_node(next)));
}

static VALUE
rb_alloc_items_uniform(VALUE self, VALUE schema)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_items_uniform(arjsv_unwrap_node(schema)));
}

static VALUE
rb_alloc_items_tuple(VALUE self, VALUE index, VALUE schema, VALUE next)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_items_tuple(NUM2UINT(index),
                                                  arjsv_unwrap_node(schema),
                                                  arjsv_unwrap_node(next)));
}

static VALUE
rb_alloc_additional_items(VALUE self, VALUE start, VALUE schema)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_additional_items(NUM2UINT(start),
                                                      arjsv_unwrap_node(schema)));
}

static VALUE
rb_alloc_no_additional_items(VALUE self, VALUE prefix_len)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_no_additional_items(NUM2UINT(prefix_len)));
}

static VALUE
rb_alloc_min_items(VALUE self, VALUE n)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_min_items(NUM2INT(n)));
}

static VALUE
rb_alloc_max_items(VALUE self, VALUE n)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_max_items(NUM2INT(n)));
}

static VALUE
rb_alloc_unique_items(VALUE self)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_unique_items());
}

static VALUE
rb_alloc_min_properties(VALUE self, VALUE n)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_min_properties(NUM2INT(n)));
}

static VALUE
rb_alloc_max_properties(VALUE self, VALUE n)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_max_properties(NUM2INT(n)));
}

static VALUE
rb_alloc_multiple_of(VALUE self, VALUE divisor)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_multiple_of(NUM2DBL(divisor)));
}

static VALUE
rb_alloc_pattern(VALUE self, VALUE pattern_str, VALUE consts_idx)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_pattern(arjsv_dup_key(pattern_str),
                                              NUM2UINT(consts_idx)));
}

static VALUE
rb_alloc_format(VALUE self, VALUE name, VALUE checker_idx)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_format(arjsv_dup_key(name),
                                             NUM2UINT(checker_idx)));
}

static VALUE
rb_alloc_not(VALUE self, VALUE schema)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_not(arjsv_unwrap_node(schema)));
}

static VALUE
rb_alloc_if_then_else(VALUE self, VALUE if_s, VALUE then_s, VALUE else_s)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_if_then_else(arjsv_unwrap_node(if_s),
                                                   arjsv_unwrap_node(then_s),
                                                   arjsv_unwrap_node(else_s)));
}

static VALUE
rb_alloc_any_of(VALUE self, VALUE branch, VALUE next)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_any_of(arjsv_unwrap_node(branch),
                                             arjsv_unwrap_node(next)));
}

static VALUE
rb_alloc_one_of(VALUE self, VALUE chain)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_one_of(arjsv_unwrap_node(chain)));
}

static VALUE
rb_alloc_one_of_step(VALUE self, VALUE branch, VALUE next)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_one_of_step(arjsv_unwrap_node(branch),
                                                  arjsv_unwrap_node(next)));
}

static VALUE
rb_alloc_pattern_property(VALUE self, VALUE regex_idx, VALUE schema, VALUE next)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_pattern_property(NUM2UINT(regex_idx),
                                                       arjsv_unwrap_node(schema),
                                                       arjsv_unwrap_node(next)));
}

static VALUE
rb_alloc_additional_properties_schema(VALUE self, VALUE keys_idx, VALUE keys_count,
                                       VALUE pats_idx, VALUE pats_count, VALUE schema)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_additional_properties_schema(
        NUM2UINT(keys_idx), NUM2UINT(keys_count),
        NUM2UINT(pats_idx), NUM2UINT(pats_count),
        arjsv_unwrap_node(schema)));
}

static VALUE
rb_alloc_no_additional_properties(VALUE self, VALUE keys_idx, VALUE keys_count,
                                   VALUE pats_idx, VALUE pats_count)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_no_additional_properties(
        NUM2UINT(keys_idx), NUM2UINT(keys_count),
        NUM2UINT(pats_idx), NUM2UINT(pats_count)));
}

static VALUE
rb_alloc_property_names(VALUE self, VALUE schema)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_property_names(arjsv_unwrap_node(schema)));
}

static VALUE
rb_alloc_ref(VALUE self, VALUE defs_name, VALUE consts_idx)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_ref(arjsv_dup_key(defs_name),
                                          NUM2UINT(consts_idx)));
}

static VALUE
rb_alloc_dependency(VALUE self, VALUE key, VALUE key_idx, VALUE dep_schema, VALUE next)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_dependency(arjsv_dup_key(key),
                                                 NUM2UINT(key_idx),
                                                 arjsv_unwrap_node(dep_schema),
                                                 arjsv_unwrap_node(next)));
}

static VALUE
rb_alloc_contains(VALUE self, VALUE schema)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_contains(arjsv_unwrap_node(schema)));
}

static VALUE
rb_alloc_minimum(VALUE self, VALUE threshold, VALUE exclusive)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_minimum(NUM2DBL(threshold),
                                              RTEST(exclusive) ? 1 : 0));
}

static VALUE
rb_alloc_maximum(VALUE self, VALUE threshold, VALUE exclusive)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_maximum(NUM2DBL(threshold),
                                              RTEST(exclusive) ? 1 : 0));
}

static VALUE
rb_alloc_min_length(VALUE self, VALUE n)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_min_length(NUM2INT(n)));
}

static VALUE
rb_alloc_max_length(VALUE self, VALUE n)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_max_length(NUM2INT(n)));
}

static VALUE
rb_alloc_const(VALUE self, VALUE canonical, VALUE consts_idx)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_const(arjsv_dup_key(canonical),
                                            NUM2UINT(consts_idx)));
}

static VALUE
rb_alloc_enum(VALUE self, VALUE canonical, VALUE consts_idx, VALUE next)
{
    (void)self;
    return arjsv_wrap_node(ALLOC_node_enum(arjsv_dup_key(canonical),
                                           NUM2UINT(consts_idx),
                                           arjsv_unwrap_node(next)));
}

// ---- Init ----------------------------------------------------------------

void
Init_arjsv(void)
{
    INIT();

    rb_mArjsv = rb_define_module("Arjsv");

    rb_cArjsvNode = rb_define_class_under(rb_mArjsv, "Node", rb_cObject);
    rb_undef_alloc_func(rb_cArjsvNode);

    rb_cArjsvSchema = rb_define_class_under(rb_mArjsv, "Schema", rb_cObject);
    rb_undef_alloc_func(rb_cArjsvSchema);
    rb_define_singleton_method(rb_cArjsvSchema, "_new",
                               rb_arjsv_schema_new, 3);
    rb_define_method(rb_cArjsvSchema, "valid?",
                     rb_arjsv_schema_valid_p, 1);
    rb_define_method(rb_cArjsvSchema, "_compile",
                     rb_arjsv_schema_compile_bang, -1);
    rb_define_method(rb_cArjsvSchema, "_dump",
                     rb_arjsv_schema_dump, 0);

    // Type bitmask constants visible to the Ruby walker.
    rb_define_const(rb_mArjsv, "T_NULL",    UINT2NUM(ARJSV_T_NULL));
    rb_define_const(rb_mArjsv, "T_BOOLEAN", UINT2NUM(ARJSV_T_BOOLEAN));
    rb_define_const(rb_mArjsv, "T_INTEGER", UINT2NUM(ARJSV_T_INTEGER));
    rb_define_const(rb_mArjsv, "T_NUMBER",  UINT2NUM(ARJSV_T_NUMBER));
    rb_define_const(rb_mArjsv, "T_STRING",  UINT2NUM(ARJSV_T_STRING));
    rb_define_const(rb_mArjsv, "T_ARRAY",   UINT2NUM(ARJSV_T_ARRAY));
    rb_define_const(rb_mArjsv, "T_OBJECT",  UINT2NUM(ARJSV_T_OBJECT));

    // ALLOC entry points (private to Ruby walker).
    rb_define_module_function(rb_mArjsv, "_alloc_validate_root", rb_alloc_validate_root, 1);
    rb_define_module_function(rb_mArjsv, "_alloc_pass",          rb_alloc_pass,          0);
    rb_define_module_function(rb_mArjsv, "_alloc_fail",          rb_alloc_fail,          0);
    rb_define_module_function(rb_mArjsv, "_alloc_seq",           rb_alloc_seq,           2);
    rb_define_module_function(rb_mArjsv, "_alloc_type_check",    rb_alloc_type_check,    1);
    rb_define_module_function(rb_mArjsv, "_alloc_required",      rb_alloc_required,      3);
    rb_define_module_function(rb_mArjsv, "_alloc_property",      rb_alloc_property,      4);
    rb_define_module_function(rb_mArjsv, "_alloc_items_uniform", rb_alloc_items_uniform, 1);
    rb_define_module_function(rb_mArjsv, "_alloc_items_tuple",   rb_alloc_items_tuple,   3);
    rb_define_module_function(rb_mArjsv, "_alloc_additional_items", rb_alloc_additional_items, 2);
    rb_define_module_function(rb_mArjsv, "_alloc_no_additional_items", rb_alloc_no_additional_items, 1);
    rb_define_module_function(rb_mArjsv, "_alloc_min_items",     rb_alloc_min_items,     1);
    rb_define_module_function(rb_mArjsv, "_alloc_max_items",     rb_alloc_max_items,     1);
    rb_define_module_function(rb_mArjsv, "_alloc_unique_items",  rb_alloc_unique_items,  0);
    rb_define_module_function(rb_mArjsv, "_alloc_min_properties", rb_alloc_min_properties, 1);
    rb_define_module_function(rb_mArjsv, "_alloc_max_properties", rb_alloc_max_properties, 1);
    rb_define_module_function(rb_mArjsv, "_alloc_multiple_of",   rb_alloc_multiple_of,   1);
    rb_define_module_function(rb_mArjsv, "_alloc_pattern",       rb_alloc_pattern,       2);
    rb_define_module_function(rb_mArjsv, "_alloc_format",        rb_alloc_format,        2);
    rb_define_module_function(rb_mArjsv, "_alloc_not",           rb_alloc_not,           1);
    rb_define_module_function(rb_mArjsv, "_alloc_if_then_else",  rb_alloc_if_then_else,  3);
    rb_define_module_function(rb_mArjsv, "_alloc_any_of",        rb_alloc_any_of,        2);
    rb_define_module_function(rb_mArjsv, "_alloc_one_of",        rb_alloc_one_of,        1);
    rb_define_module_function(rb_mArjsv, "_alloc_one_of_step",   rb_alloc_one_of_step,   2);
    rb_define_module_function(rb_mArjsv, "_alloc_pattern_property", rb_alloc_pattern_property, 3);
    rb_define_module_function(rb_mArjsv, "_alloc_additional_properties_schema",
                              rb_alloc_additional_properties_schema, 5);
    rb_define_module_function(rb_mArjsv, "_alloc_no_additional_properties",
                              rb_alloc_no_additional_properties, 4);
    rb_define_module_function(rb_mArjsv, "_alloc_property_names", rb_alloc_property_names, 1);
    rb_define_module_function(rb_mArjsv, "_alloc_ref",           rb_alloc_ref,           2);
    rb_define_module_function(rb_mArjsv, "_alloc_dependency",    rb_alloc_dependency,    4);
    rb_define_module_function(rb_mArjsv, "_alloc_contains",      rb_alloc_contains,      1);
    rb_define_module_function(rb_mArjsv, "_alloc_minimum",       rb_alloc_minimum,       2);
    rb_define_module_function(rb_mArjsv, "_alloc_maximum",       rb_alloc_maximum,       2);
    rb_define_module_function(rb_mArjsv, "_alloc_min_length",    rb_alloc_min_length,    1);
    rb_define_module_function(rb_mArjsv, "_alloc_max_length",    rb_alloc_max_length,    1);
    rb_define_module_function(rb_mArjsv, "_alloc_const",         rb_alloc_const,         2);
    rb_define_module_function(rb_mArjsv, "_alloc_enum",          rb_alloc_enum,          3);
}
