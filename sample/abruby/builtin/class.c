#include "builtin.h"

// Class#new (only on Class, not Module)
static RESULT ab_class_new(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    VALUE obj = abruby_new_object(c, klass);
    const struct abruby_method *init = abruby_find_method(klass, c->ids->initialize);
    if (init) {
        // Push frame for initialize (needed for super to find the class).
        // super walks method->defining_class->super, not frame.klass.
        // All fields must be initialized — abruby_exception_new walks the
        // frame chain and reads self/fp/entry, so stale stack data here
        // surfaces as a segfault during backtrace formatting.
        struct abruby_frame frame;
        frame.prev = c->current_frame;
        frame.caller_node = c->current_frame ? c->current_frame->caller_node : NULL;
        frame.block = NULL;
        frame.self = c->current_frame ? c->current_frame->self : Qnil;
        frame.fp = c->current_frame ? c->current_frame->fp : c->stack;
        { extern struct abruby_entry abruby_empty_entry;
          frame.entry = c->current_frame ? c->current_frame->entry : &abruby_empty_entry; }
        c->current_frame = &frame;

        RESULT r = RESULT_OK(Qnil);
        switch (init->type) {
          case ABRUBY_METHOD_CFUNC:
            r = init->u.cfunc.func(c, obj, argc, argv);
            break;
          case ABRUBY_METHOD_AST: {
            struct abruby_frame init_frame;
            init_frame.prev = c->current_frame;
            init_frame.caller_node = NULL;
            init_frame.block = NULL;
            init_frame.self = obj;
            init_frame.fp = argv;
            init_frame.entry = &init->entry;
            c->current_frame = &init_frame;
            ((struct abruby_entry *)&init->entry)->dispatch_count++;
            r = EVAL(c, init->u.ast.body);
            c->current_frame = init_frame.prev;
            break;
          }
          case ABRUBY_METHOD_IVAR_SETTER:
            // def initialize(v); @name = v; end — collapsed to ivar setter.
            // Write argv[0] into the named slot on the new obj (via its own shape).
            abruby_ivar_set(c, obj, init->u.ivar_accessor.ivar_name, argv[0]);
            break;
          case ABRUBY_METHOD_IVAR_GETTER:
            // def initialize; @name; end — no side-effect for a constructor.
            break;
        }

        c->current_frame = frame.prev;
        if (r.state != RESULT_NORMAL) return r;
    }
    return RESULT_OK(obj);
}

// Module#inspect (inherited by Class)
static RESULT ab_module_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new_cstr(c, rb_id2name(abruby_unwrap_class(self)->name)));
}

// Module#include — insert module into super chain of current_class
static RESULT ab_module_include(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    // self is the class doing the include (current_class wrapped)
    // argv[0] is the module to include
    struct abruby_class *klass = abruby_unwrap_class(self);
    const struct abruby_class *mod = abruby_unwrap_class(argv[0]);

    // Create a proxy that copies module's method table pointer
    // Insert between klass and klass->super
    // Simple approach: create a proxy class that delegates to the module
    struct abruby_class *proxy = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
    proxy->klass = c->abm->module_class;
    proxy->obj_type = ABRUBY_OBJ_MODULE;
    proxy->name = mod->name;
    proxy->super = klass->super;
    // Copy method table from module to proxy
    ab_id_table_foreach(&mod->methods, _k, _v, {
        ab_id_table_insert(&proxy->methods, _k, _v);
    });

    klass->super = proxy;
    // If including into a class with type-specialized operators,
    // the module may shadow built-in operators.
    if (klass == c->abm->integer_class || klass == c->abm->float_class ||
        klass == c->abm->array_class   || klass == c->abm->hash_class) {
        c->abm->basic_op_redefined = 1;
    }
    c->abm->method_serial++;
    return RESULT_OK(Qnil);
}

// Module#const_get(name) — get constant by name (String or Symbol)
static RESULT ab_module_const_get(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const struct abruby_class *klass = abruby_unwrap_class(self);
    ID name_id;
    if (ab_obj_type_p(argv[0], ABRUBY_OBJ_SYMBOL)) {
        name_id = SYM2ID(ab_sym_unwrap(argv[0]));
    } else {
        name_id = rb_intern_str(RSTR(argv[0]));
    }
    VALUE v;
    if (ab_id_table_lookup(&klass->constants, name_id, &v)) {
        return RESULT_OK(v);
    }
    VALUE exc = abruby_exception_new(c, c->current_frame,
        abruby_str_new_cstr(c, "uninitialized constant"));
    return (RESULT){exc, RESULT_RAISE};
}

// Module#const_set(name, value) — set constant by name
static RESULT ab_module_const_set(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    ID name_id;
    if (ab_obj_type_p(argv[0], ABRUBY_OBJ_SYMBOL)) {
        name_id = SYM2ID(ab_sym_unwrap(argv[0]));
    } else {
        name_id = rb_intern_str(RSTR(argv[0]));
    }
    abruby_class_set_const(klass, name_id, argv[1]);
    return RESULT_OK(argv[1]);
}

// Struct.new(:a, :b, ...) — create a new class with attr_accessors for
// each field and a `new(*args)` constructor that assigns its arguments
// to the corresponding ivars.  abruby's Struct support is minimal: the
// returned class has no Struct-specific methods (==, members, etc.),
// just enough to satisfy `MethodDef = Struct.new(:params, :body)` style
// load-time const initializers in optcarrot.  Instances do work for
// basic assignment / read but not for the full Struct API.
RESULT ab_struct_new(CTX *c, VALUE self, unsigned int argc, VALUE *argv);
RESULT ab_struct_new(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self;
    // Create a fresh class inheriting from Object.
    struct abruby_class *klass = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
    klass->klass = c->abm->class_class;
    klass->obj_type = ABRUBY_OBJ_CLASS;             // this is a class struct
    klass->instance_obj_type = ABRUBY_OBJ_GENERIC; // instances are user objects
    klass->name = rb_intern("Struct");
    klass->super = c->abm->object_class;
    // Register an ivar accessor for each field.  abruby internally
    // recognises the simple body shapes used by `def x; @x; end` /
    // `def x=(v); @x = v; end`, but we don't have AST nodes here at
    // builtin init time, so just call abruby_class_add_method.  The
    // ivar shape will be created lazily on first instance access.
    for (unsigned int i = 0; i < argc; i++) {
        ID field = ab_obj_type_p(argv[i], ABRUBY_OBJ_SYMBOL) ? SYM2ID(ab_sym_unwrap(argv[i])) : rb_intern_str(RSTR(argv[i]));
        // Build a synthetic getter cfunc via attr-accessor body would
        // require AST nodes; instead, register a tiny IVAR_GETTER
        // entry directly.
        struct abruby_method *getter = ruby_xcalloc(1, sizeof(struct abruby_method));
        getter->name = field;
        getter->type = ABRUBY_METHOD_IVAR_GETTER;
        getter->defining_class = klass;
        getter->u.ivar_accessor.ivar_name = field;
        ab_id_table_insert(&klass->methods, field, (VALUE)getter);

        // Setter: name with `=` suffix.
        const char *fname = rb_id2name(field);
        char *setname = (char *)alloca(strlen(fname) + 2);
        strcpy(setname, fname);
        strcat(setname, "=");
        ID setter_id = rb_intern(setname);
        struct abruby_method *setter = ruby_xcalloc(1, sizeof(struct abruby_method));
        setter->name = setter_id;
        setter->type = ABRUBY_METHOD_IVAR_SETTER;
        setter->defining_class = klass;
        setter->u.ivar_accessor.ivar_name = field;
        ab_id_table_insert(&klass->methods, setter_id, (VALUE)setter);
    }
    c->abm->method_serial++;
    return RESULT_OK(abruby_wrap_class(klass));
}

// File.* — minimal facade over CRuby's rb_cFile.  Just enough for
// optcarrot-bench: join, binread, dirname, basename, extname, expand_path,
// exist?, readable?.  Receiver is the abruby File class object.

static VALUE arg_to_rstr(VALUE v) {
    if (ab_obj_type_p(v, ABRUBY_OBJ_STRING)) return RSTR(v);
    return v;  // already a CRuby string maybe
}

RESULT ab_file_join(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self;
    VALUE args = rb_ary_new_capa(argc);
    for (unsigned int i = 0; i < argc; i++) rb_ary_push(args, arg_to_rstr(argv[i]));
    VALUE r = rb_funcallv(rb_cFile, rb_intern("join"), 1, &args);
    return RESULT_OK(abruby_str_new(c, r));
}
RESULT ab_file_binread(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc;
    VALUE r = rb_funcall(rb_cFile, rb_intern("binread"), 1, arg_to_rstr(argv[0]));
    return RESULT_OK(abruby_str_new(c, r));
}
RESULT ab_file_dirname(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc;
    return RESULT_OK(abruby_str_new(c, rb_funcall(rb_cFile, rb_intern("dirname"), 1, arg_to_rstr(argv[0]))));
}
RESULT ab_file_basename(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc;
    return RESULT_OK(abruby_str_new(c, rb_funcall(rb_cFile, rb_intern("basename"), 1, arg_to_rstr(argv[0]))));
}
RESULT ab_file_extname(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc;
    return RESULT_OK(abruby_str_new(c, rb_funcall(rb_cFile, rb_intern("extname"), 1, arg_to_rstr(argv[0]))));
}
RESULT ab_file_expand_path(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self;
    VALUE r;
    if (argc >= 2) {
        r = rb_funcall(rb_cFile, rb_intern("expand_path"), 2,
                       arg_to_rstr(argv[0]), arg_to_rstr(argv[1]));
    } else {
        r = rb_funcall(rb_cFile, rb_intern("expand_path"), 1, arg_to_rstr(argv[0]));
    }
    return RESULT_OK(abruby_str_new(c, r));
}
RESULT ab_file_exist_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)self; (void)argc;
    VALUE r = rb_funcall(rb_cFile, rb_intern("exist?"), 1, arg_to_rstr(argv[0]));
    return RESULT_OK(r);
}
RESULT ab_file_readable_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)self; (void)argc;
    VALUE r = rb_funcall(rb_cFile, rb_intern("readable?"), 1, arg_to_rstr(argv[0]));
    return RESULT_OK(r);
}
RESULT ab_file_read(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc;
    VALUE r = rb_funcall(rb_cFile, rb_intern("read"), 1, arg_to_rstr(argv[0]));
    return RESULT_OK(abruby_str_new(c, r));
}

// Module#attr_reader(:a, :b, ...) — register IVAR_GETTER methods on self.
// Used for the dynamic form `attr_reader id` where id is a runtime
// variable; the parser falls through to here when args aren't all
// symbol literals.
static struct abruby_method *
make_ivar_getter_method(struct abruby_class *klass, ID name, ID ivar_name)
{
    struct abruby_method *m = (struct abruby_method *)ruby_xcalloc(1, sizeof(struct abruby_method));
    m->name = name;
    m->type = ABRUBY_METHOD_IVAR_GETTER;
    m->defining_class = klass;
    m->u.ivar_accessor.ivar_name = ivar_name;
    return m;
}

static struct abruby_method *
make_ivar_setter_method(struct abruby_class *klass, ID name, ID ivar_name)
{
    struct abruby_method *m = (struct abruby_method *)ruby_xcalloc(1, sizeof(struct abruby_method));
    m->name = name;
    m->type = ABRUBY_METHOD_IVAR_SETTER;
    m->defining_class = klass;
    m->u.ivar_accessor.ivar_name = ivar_name;
    return m;
}

static ID arg_to_id(VALUE arg) {
    if (ab_obj_type_p(arg, ABRUBY_OBJ_SYMBOL)) return SYM2ID(ab_sym_unwrap(arg));
    if (ab_obj_type_p(arg, ABRUBY_OBJ_STRING)) return rb_intern_str(RSTR(arg));
    return 0;
}

static RESULT ab_module_attr_reader(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    for (unsigned int i = 0; i < argc; i++) {
        ID name = arg_to_id(argv[i]);
        if (!name) continue;
        ID ivar = rb_intern_str(rb_str_cat_cstr(rb_str_new_cstr("@"), rb_id2name(name)));
        ab_id_table_insert(&klass->methods, name,
                           (VALUE)make_ivar_getter_method(klass, name, ivar));
    }
    c->abm->method_serial++;
    return RESULT_OK(Qnil);
}

static RESULT ab_module_attr_writer(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    for (unsigned int i = 0; i < argc; i++) {
        ID name = arg_to_id(argv[i]);
        if (!name) continue;
        ID ivar = rb_intern_str(rb_str_cat_cstr(rb_str_new_cstr("@"), rb_id2name(name)));
        ID setter = rb_intern_str(rb_str_cat_cstr(rb_str_new_cstr(rb_id2name(name)), "="));
        ab_id_table_insert(&klass->methods, setter,
                           (VALUE)make_ivar_setter_method(klass, setter, ivar));
    }
    c->abm->method_serial++;
    return RESULT_OK(Qnil);
}

static RESULT ab_module_attr_accessor(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    RESULT r = ab_module_attr_reader(c, self, argc, argv);
    if (r.state != RESULT_NORMAL) return r;
    return ab_module_attr_writer(c, self, argc, argv);
}

// Module#private / #public / #protected — no-op visibility controls.
// abruby does not enforce method visibility, so these are placeholders
// that accept arbitrary arguments and return self (matching Ruby's
// "return self when called with no args, otherwise return first arg").
static RESULT ab_module_visibility_noop(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c;
    if (argc == 0) return RESULT_OK(self);
    return RESULT_OK(argv[0]);
}

// Module#=== — check if argv[0] is_a? self (class matching for case/when)
static RESULT ab_module_case_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const struct abruby_class *check_class = abruby_unwrap_class(self);
    const struct abruby_class *obj_class = AB_CLASS_OF(c, argv[0]);
    while (obj_class) {
        if (obj_class == check_class) return RESULT_OK(Qtrue);
        obj_class = obj_class->super;
    }
    return RESULT_OK(Qfalse);
}

void
Init_abruby_class(void)
{
    // Module (parent of Class)
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("==="),       ab_module_case_eq,   1);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("inspect"),   ab_module_inspect,   0);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("include"),   ab_module_include,   1);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("const_get"), ab_module_const_get, 1);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("const_set"), ab_module_const_set, 2);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("private"),   ab_module_visibility_noop, 0);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("public"),    ab_module_visibility_noop, 0);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("protected"), ab_module_visibility_noop, 0);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("module_function"), ab_module_visibility_noop, 0);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("attr_reader"),   ab_module_attr_reader,   0);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("attr_writer"),   ab_module_attr_writer,   0);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("attr_accessor"), ab_module_attr_accessor, 0);

    // Class (inherits Module, adds new)
    abruby_class_add_cfunc(ab_tmpl_class_class, rb_intern("new"), ab_class_new, -1);
}
