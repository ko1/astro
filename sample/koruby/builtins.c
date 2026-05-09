/* koruby builtin methods */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "korb_gmp.h"
#include "context.h"
#include "object.h"
#include "node.h"

/* Builtin classes — each section split out into its own file under
 * builtins/.  We #include them here so they all share builtins.c's
 * translation unit (one .o, single set of static helpers, no extra
 * link plumbing).  Headers + macros (KORB_BOOL, korb_intern, ...) are
 * already pulled in at the top of this file. */
#include "builtins/kernel.c"
#include "builtins/integer.c"
#include "builtins/float.c"
#include "builtins/string.c"
#include "builtins/array.c"
#include "builtins/hash.c"
#include "builtins/range.c"
#include "builtins/module.c"
#include "builtins/comparable.c"
#include "builtins/object.c"
#include "builtins/symbol.c"
#include "builtins/exception.c"
#include "builtins/math.c"
#include "builtins/file.c"
#include "builtins/boolean.c"
#include "builtins/proc.c"
#include "builtins/binding.c"
VALUE _allocator_disallowed(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
    const char *cn = (!SPECIAL_CONST_P(self) &&
                      (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE))
        ? korb_id_name(((struct korb_class *)self)->name) : "?";
    korb_raise(c, (struct korb_class *)eT,
               "allocator undefined for %s", cn);
    return Qnil;
}
VALUE _new_disallowed(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
    const char *cn = (!SPECIAL_CONST_P(self) &&
                      (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE))
        ? korb_id_name(((struct korb_class *)self)->name) : "?";
    korb_raise(c, (struct korb_class *)eN,
               "undefined method 'new' for %s", cn);
    return Qnil;
}

#define DEF(klass, name, fn, argc) \
    korb_class_add_method_cfunc((klass), korb_intern(name), (fn), (argc))
#define DEF_PRIV(klass, name, fn, argc) do {                              \
    korb_class_add_method_cfunc((klass), korb_intern(name), (fn), (argc)); \
    struct korb_method *_m = korb_class_find_method((klass), korb_intern(name)); \
    if (_m) _m->visibility = KORB_VIS_PRIVATE;                            \
} while (0)

void korb_init_builtins(void) {
    /* BasicObject methods — must come first since CRuby's BasicObject
     * has its own __id__, __send__, ==, !=, !, equal?, instance_eval,
     * instance_exec.  Without these on cBasic itself, `Class.new(BasicObject)`
     * subclasses get nothing. */
    {
        struct korb_class *cBasic = (struct korb_class *)korb_const_get(korb_vm->object_class, korb_intern("BasicObject"));
        if (cBasic) {
            extern VALUE kernel_object_id(CTX *c, VALUE self, int argc, VALUE *argv);
            extern VALUE kernel_eq(CTX *c, VALUE self, int argc, VALUE *argv);
            extern VALUE kernel_neq(CTX *c, VALUE self, int argc, VALUE *argv);
            extern VALUE kernel_not(CTX *c, VALUE self, int argc, VALUE *argv);
            DEF(cBasic, "__id__", kernel_object_id, 0);
            DEF(cBasic, "__send__", obj_send, -1);
            DEF(cBasic, "==", kernel_eq, 1);
            DEF(cBasic, "!=", kernel_neq, 1);
            DEF(cBasic, "!", kernel_not, 0);
            DEF(cBasic, "equal?", kernel_eq, 1);
            DEF(cBasic, "instance_eval", obj_instance_eval, -1);
            DEF(cBasic, "instance_exec", obj_instance_exec, -1);
        }
    }
    /* Object methods */
    struct korb_class *cObj = korb_vm->object_class;
    /* Module functions: private instance method on Object PLUS public
     * module method on Kernel.metaclass.  Makes Kernel.puts etc work. */
    struct korb_class *cKerMeta = korb_vm->kernel_module
        ? korb_singleton_class_of(korb_vm->kernel_module) : NULL;
    DEF_PRIV(cObj, "p", kernel_p, -1);
    DEF_PRIV(cObj, "puts", kernel_puts, -1);
    if (cKerMeta) {
        DEF(cKerMeta, "p", kernel_p, -1);
        DEF(cKerMeta, "puts", kernel_puts, -1);
    }
    /* internal helpers used by `**obj` hash splat lowering. */
    {
        VALUE kernel_kwsplat_to_hash(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE kernel_kwsplat_to_hash_lenient(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE kernel_to_block_arg(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE kernel_rescue_splat_match(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE kernel_case_splat_match(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE kernel_pattern_decon_check(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE kernel_pattern_decon_keys_check(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cObj, "__kwsplat_to_hash", kernel_kwsplat_to_hash, 1);
        DEF(cObj, "__kwsplat_to_hash_lenient", kernel_kwsplat_to_hash_lenient, 1);
        DEF(cObj, "__to_block_arg", kernel_to_block_arg, 1);
        DEF(cObj, "__rescue_splat_match", kernel_rescue_splat_match, 2);
        DEF(cObj, "__rescue_class_check", kernel_rescue_class_check, 1);
        DEF(cObj, "__case_splat_match", kernel_case_splat_match, 2);
        DEF(cObj, "__case_splat_any",   kernel_case_splat_any,   1);
        DEF(cObj, "__pattern_decon_check", kernel_pattern_decon_check, 1);
        DEF(cObj, "__pattern_decon_keys_check", kernel_pattern_decon_keys_check, 1);
    }
    DEF_PRIV(cObj, "print", kernel_print, -1);
    DEF_PRIV(cObj, "raise", kernel_raise, -1);
    DEF_PRIV(cObj, "fail", kernel_raise, -1);  /* alias */
    /* Also register on Kernel module so `Kernel.private_instance_methods`
     * reports them (CRuby convention).  Module include propagates to
     * Object instances. */
    if (korb_vm->kernel_module) {
        DEF_PRIV(korb_vm->kernel_module, "print", kernel_print, -1);
        DEF_PRIV(korb_vm->kernel_module, "raise", kernel_raise, -1);
        DEF_PRIV(korb_vm->kernel_module, "fail", kernel_raise, -1);
    }
    if (cKerMeta) {
        DEF(cKerMeta, "print", kernel_print, -1);
        DEF(cKerMeta, "raise", kernel_raise, -1);
        DEF(cKerMeta, "fail", kernel_raise, -1);
    }
    DEF(cObj, "inspect", kernel_inspect, 0);
    DEF(cObj, "to_s", kernel_to_s, 0);
    DEF(cObj, "class", kernel_class, 0);
    DEF(cObj, "==", kernel_eq, 1);
    DEF(cObj, "!=", kernel_neq, 1);
    {
        extern VALUE kernel_not_match(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cObj, "!~", kernel_not_match, 1);
    }
    DEF(cObj, "!", kernel_not, 0);
    DEF(cObj, "nil?", kernel_nil_p, 0);
    DEF(cObj, "object_id", kernel_object_id, 0);
    DEF(cObj, "__id__", kernel_object_id, 0);
    DEF(cObj, "equal?", kernel_eq, 1);  /* same as == for now */
    DEF(cObj, "freeze", kernel_freeze, 0);
    DEF(cObj, "frozen?", kernel_frozen_p, 0);
    DEF(cObj, "respond_to?", kernel_respond_to_p, 1);
    DEF(cObj, "is_a?", kernel_is_a_p, 1);
    DEF(cObj, "kind_of?", kernel_is_a_p, 1);
    /* Default Object#respond_to_missing? — always returns false.  CRuby
     * has this as a private instance method on Kernel; user classes
     * override it to participate in respond_to? lookup. */
    {
        VALUE _rtm_default(CTX *c, VALUE self, int argc, VALUE *argv) {
            return Qfalse;
        }
        DEF_PRIV(cObj, "respond_to_missing?", _rtm_default, 2);
        if (korb_vm->kernel_module) {
            DEF_PRIV(korb_vm->kernel_module, "respond_to_missing?", _rtm_default, 2);
        }
    }
    /* Kernel module copies for `Kernel.{public,private}_instance_methods`
     * introspection. */
    if (korb_vm->kernel_module) {
        DEF(korb_vm->kernel_module, "respond_to?", kernel_respond_to_p, 1);
        DEF(korb_vm->kernel_module, "is_a?", kernel_is_a_p, 1);
        DEF(korb_vm->kernel_module, "kind_of?", kernel_is_a_p, 1);
    }
    DEF(cObj, "methods", obj_methods, -1);
    {
        VALUE obj_public_methods(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE obj_private_methods(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE obj_protected_methods(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cObj, "public_methods",    obj_public_methods,    -1);
        DEF(cObj, "private_methods",   obj_private_methods,   -1);
        DEF(cObj, "protected_methods", obj_protected_methods, -1);
    }
    DEF(cObj, "singleton_methods", obj_singleton_methods, -1);
    {
        VALUE obj_singleton_class(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cObj, "singleton_class", obj_singleton_class, 0);
    }
    DEF(cObj, "define_singleton_method", obj_define_singleton_method, -1);
    DEF(cObj, "block_given?", kernel_block_given, 0);
    DEF_PRIV(cObj, "throw",        kernel_throw,      -1);
    DEF_PRIV(cObj, "catch",        kernel_catch,      -1);
    if (korb_vm->kernel_module) {
        DEF_PRIV(korb_vm->kernel_module, "throw", kernel_throw, -1);
        DEF_PRIV(korb_vm->kernel_module, "catch", kernel_catch, -1);
    }
    DEF_PRIV(cObj, "require_relative", kernel_require_relative, 1);
    DEF_PRIV(cObj, "require", kernel_require, 1);
    DEF_PRIV(cObj, "__dir__", kernel_dir, 0);
    DEF_PRIV(cObj, "load", kernel_load, -1);
    DEF_PRIV(cObj, "abort", kernel_abort, -1);
    DEF_PRIV(cObj, "exit", kernel_exit, -1);
    {
        VALUE kernel_exit_bang(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE kernel_abort(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cObj, "exit!", kernel_exit_bang, -1);
        DEF(cObj, "abort", kernel_abort,     -1);
    }
    DEF(cObj, "sleep", kernel_sleep, -1);
    DEF(cObj, "at_exit", kernel_at_exit, 0);
    DEF(cObj, "rand",    kernel_rand,   -1);
    DEF(cObj, "srand",   kernel_srand,  -1);
    DEF_PRIV(cObj, "Integer", kernel_integer, -1);
    DEF_PRIV(cObj, "Float",   kernel_float,    1);
    DEF_PRIV(cObj, "String",  kernel_string,   1);
    DEF_PRIV(cObj, "Array",   kernel_array,    1);
    /* Spec checks Kernel.private_instance_methods — also register there. */
    if (korb_vm->kernel_module) {
        DEF_PRIV(korb_vm->kernel_module, "Integer", kernel_integer, -1);
        DEF_PRIV(korb_vm->kernel_module, "Float",   kernel_float,    1);
        DEF_PRIV(korb_vm->kernel_module, "String",  kernel_string,   1);
        DEF_PRIV(korb_vm->kernel_module, "Array",   kernel_array,    1);
    }

    /* Integer */
    struct korb_class *cInt = korb_vm->integer_class;
    DEF(cInt, "+", int_plus, 1);
    DEF(cInt, "-", int_minus, 1);
    DEF(cInt, "*", int_mul, 1);
    DEF(cInt, "/", int_div, 1);
    DEF(cInt, "%", int_mod, 1);
    DEF(cInt, "<<", int_lshift, 1);
    DEF(cInt, ">>", int_rshift, 1);
    DEF(cInt, "&", int_and, 1);
    DEF(cInt, "|", int_or, 1);
    DEF(cInt, "^", int_xor, 1);
    DEF(cInt, "<", int_lt, 1);
    DEF(cInt, "<=", int_le, 1);
    DEF(cInt, ">", int_gt, 1);
    DEF(cInt, ">=", int_ge, 1);
    DEF(cInt, "==", int_eq, 1);
    DEF(cInt, "<=>", int_cmp, 1);
    DEF(cInt, "-@", int_uminus, 0);
    DEF(cInt, "+@", int_uplus,  0);
    DEF(cInt, "to_s", int_to_s, -1);
    DEF(cInt, "to_i", int_to_i, 0);
    DEF(cInt, "to_f", int_to_f, 0);
    DEF(cInt, "zero?", int_zero_p, 0);
    DEF(cInt, "even?", int_even_p, 0);
    DEF(cInt, "odd?",  int_odd_p,  0);
    DEF(cInt, "positive?", int_positive_p, 0);
    DEF(cInt, "negative?", int_negative_p, 0);
    DEF(cInt, "times", int_times, 0);
    DEF(cInt, "succ", int_succ, 0);
    DEF(cInt, "next", int_succ, 0);
    DEF(cInt, "pred", int_pred, 0);

    /* Helpers used to forbid `.allocate` / `.new` on classes whose
     * instances are immediates (Float, Symbol, NilClass, TrueClass,
     * FalseClass).  CRuby raises TypeError from .allocate and
     * NoMethodError from .new.  `_allocator_disallowed` and
     * `_new_disallowed` are defined above. */
    /* Float */
    struct korb_class *cFlt = korb_vm->float_class;
    {
        struct korb_class *cFltMeta = korb_singleton_class_of(cFlt);
        DEF(cFltMeta, "allocate", _allocator_disallowed, -1);
        DEF(cFltMeta, "new",      _new_disallowed,       -1);
    }
    korb_const_set(cFlt, korb_intern("INFINITY"), korb_float_new(1.0/0.0));
    korb_const_set(cFlt, korb_intern("NAN"),      korb_float_new(0.0/0.0));
    korb_const_set(cFlt, korb_intern("MAX"),      korb_float_new(1.7976931348623157e+308));
    korb_const_set(cFlt, korb_intern("MIN"),      korb_float_new(2.2250738585072014e-308));
    korb_const_set(cFlt, korb_intern("EPSILON"),  korb_float_new(2.220446049250313e-16));
    DEF(cFlt, "+", flt_plus, 1);
    DEF(cFlt, "-", flt_minus, 1);
    DEF(cFlt, "*", flt_mul, 1);
    DEF(cFlt, "/", flt_div, 1);
    DEF(cFlt, "to_s", flt_to_s, 0);
    extern VALUE flt_to_i(CTX *c, VALUE self, int argc, VALUE *argv);
    DEF(cFlt, "to_i",   flt_to_i, 0);
    DEF(cFlt, "to_int", flt_to_i, 0);
    DEF(cFlt, "step", flt_step, -1);
    DEF(cFlt, "nan?",      flt_nan_p,      0);
    DEF(cFlt, "infinite?", flt_infinite_p, 0);
    DEF(cFlt, "finite?",   flt_finite_p,   0);
    DEF(cFlt, "zero?",     flt_zero_p,     0);
    DEF(cFlt, "positive?", flt_positive_p, 0);
    DEF(cFlt, "negative?", flt_negative_p, 0);

    /* String */
    struct korb_class *cStr = korb_vm->string_class;
    /* String#encoding stub — return Encoding::UTF_8.  */
    {
        VALUE _str_encoding(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE _str_force_encoding(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE _str_b(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cStr, "encoding",       _str_encoding, 0);
        DEF(cStr, "force_encoding", _str_force_encoding, -1);
        DEF(cStr, "encode",         _str_force_encoding, -1);
        DEF(cStr, "encode!",        _str_force_encoding, -1);
        DEF(cStr, "b",              _str_b, 0);
    }
    /* Encoding#name / #to_s. */
    {
        VALUE cEnc_v = korb_const_get(korb_vm->object_class, korb_intern("Encoding"));
        if (!UNDEF_P(cEnc_v) && !SPECIAL_CONST_P(cEnc_v) &&
            BUILTIN_TYPE(cEnc_v) == T_CLASS) {
            struct korb_class *cEnc = (struct korb_class *)cEnc_v;
            VALUE _enc_name(CTX *c, VALUE self, int argc, VALUE *argv);
            DEF(cEnc, "name",    _enc_name, 0);
            DEF(cEnc, "to_s",    _enc_name, 0);
            DEF(cEnc, "inspect", _enc_name, 0);
        }
    }
    DEF(cStr, "+", str_plus, 1);
    DEF(cStr, "<<", str_lshift, -1);
    DEF(cStr, "concat", str_concat, 1);
    DEF(cStr, "size", str_size, 0);
    DEF(cStr, "length", str_size, 0);
    DEF(cStr, "==", str_eq, 1);
    DEF(cStr, "<=>", str_cmp, 1);
    DEF(cStr, "<",   str_lt, 1);
    DEF(cStr, "<=",  str_le, 1);
    DEF(cStr, ">",   str_gt, 1);
    DEF(cStr, ">=",  str_ge, 1);
    DEF(cStr, "to_s", str_to_s, 0);
    DEF(cStr, "to_sym", str_to_sym, 0);
    DEF(cStr, "__chilled?", str_chilled_p, 0);

    /* Array */
    struct korb_class *cAry = korb_vm->array_class;
    DEF(cAry, "size", ary_size, 0);
    DEF(cAry, "length", ary_size, 0);
    DEF(cAry, "[]", ary_aref, -1);
    DEF(cAry, "[]=", ary_aset, -1);
    DEF(cAry, "push", ary_push, -1);
    DEF(cAry, "<<", ary_lshift, 1);
    DEF(cAry, "pop", ary_pop, -1);
    DEF(cAry, "first", ary_first_n, -1);
    DEF(cAry, "last",  ary_last_n,  -1);
    DEF(cAry, "each", ary_each, 0);
    DEF(cAry, "each_with_index", ary_each_with_index, 0);
    DEF(cAry, "map", ary_map, 0);
    DEF(cAry, "collect", ary_map, 0);
    DEF(cAry, "select", ary_select, 0);
    DEF(cAry, "filter", ary_select, 0);
    DEF(cAry, "reduce", ary_reduce, -1);
    DEF(cAry, "inject", ary_reduce, -1);
    DEF(cAry, "join", ary_join, -1);
    DEF(cAry, "inspect", ary_inspect, 0);
    DEF(cAry, "to_s", ary_inspect, 0);
    DEF(cAry, "==", ary_eq, 1);
    DEF(cAry, "dup", ary_dup, 0);
    DEF(cAry, "to_h", ary_to_h, 0);

    /* Hash */
    struct korb_class *cHsh = korb_vm->hash_class;
    DEF(cHsh, "[]", hash_aref, 1);
    DEF(cHsh, "[]=", hash_aset, 2);
    DEF(cHsh, "size", hash_size, 0);
    DEF(cHsh, "length", hash_size, 0);
    DEF(cHsh, "each", hash_each, 0);

    /* Range */
    struct korb_class *cRng = korb_vm->range_class;
    {
        /* Class method Range.new — register on Range's metaclass. */
        struct korb_class *cRngMeta = korb_singleton_class_of(cRng);
        korb_class_add_method_cfunc(cRngMeta, korb_intern("new"), rng_class_new, -1);
    }
    DEF(cRng, "each", rng_each, 0);
    DEF(cRng, "first", rng_first, -1);
    DEF(cRng, "last",  rng_last,  -1);
    DEF(cRng, "begin", rng_first, -1);
    DEF(cRng, "min",   rng_first, -1);
    DEF(cRng, "end",   rng_last,  -1);
    DEF(cRng, "max",   rng_last,  -1);
    DEF(cRng, "to_a", rng_to_a, 0);
    {
        VALUE rng_hash(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cRng, "hash", rng_hash, 0);
    }

    /* Class */
    struct korb_class *cCls = korb_vm->class_class;
    DEF(cCls, "new", class_new, -1);
    {
        VALUE class_allocate(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cCls, "allocate", class_allocate, 0);
    }
    DEF(cCls, "name", class_name, 0);

    /* Module — applies to both Class and Module */
    struct korb_class *cMod = korb_vm->module_class;
    DEF(cMod, "name", class_name, 0);
    DEF(cMod, "attr_reader",   module_attr_reader,   -1);
    /* Module#attr — historical:
     *   attr :foo            → reader only
     *   attr :foo, true      → reader + writer (deprecated form)
     *   attr :foo, :bar, ... → readers for each (when args are all symbols) */
    {
        VALUE _module_attr(CTX *c, VALUE self, int argc, VALUE *argv) {
            VALUE module_attr_reader(CTX *, VALUE, int, VALUE *);
            VALUE module_attr_writer(CTX *, VALUE, int, VALUE *);
            if (argc == 2 && (argv[1] == Qtrue || argv[1] == Qfalse)) {
                bool writable = (argv[1] == Qtrue);
                VALUE r = module_attr_reader(c, self, 1, argv);
                if (c->state == KORB_RAISE) return Qnil;
                if (writable) {
                    VALUE w = module_attr_writer(c, self, 1, argv);
                    if (c->state == KORB_RAISE) return Qnil;
                    /* CRuby returns [reader_sym, writer_sym] in this form. */
                    VALUE arr = korb_ary_new_capa(2);
                    if (BUILTIN_TYPE(r) == T_ARRAY && ((struct korb_array *)r)->len > 0) {
                        korb_ary_push(arr, ((struct korb_array *)r)->ptr[0]);
                    }
                    if (BUILTIN_TYPE(w) == T_ARRAY && ((struct korb_array *)w)->len > 0) {
                        korb_ary_push(arr, ((struct korb_array *)w)->ptr[0]);
                    }
                    return arr;
                }
                return r;
            }
            return module_attr_reader(c, self, argc, argv);
        }
        DEF(cMod, "attr", _module_attr, -1);
    }
    DEF(cMod, "attr_writer",   module_attr_writer,   -1);
    DEF(cMod, "attr_accessor", module_attr_accessor, -1);
    DEF(cMod, "include",       module_include,       -1);
    /* Toplevel `include M` — main / Object forwards to Object#include.
     * `extend self` and similar; expose on Object too as a private
     * method so any context (including main / class bodies of plain
     * objects) can call it. */
    DEF(cObj, "include",       module_include,       -1);
    DEF(cObj, "private",       module_private,       -1);
    DEF(cObj, "public",        module_public,        -1);
    DEF(cMod, "private",       module_private,       -1);
    DEF(cMod, "public",        module_public,        -1);
    DEF(cMod, "protected",     module_protected,     -1);
    DEF(cMod, "module_function", module_module_function, -1);
    DEF(cMod, "define_method", module_define_method, -1);
    DEF(cMod, "alias_method",  module_alias_method,  -1);
    DEF(cMod, "undef_method",  module_undef_method,  -1);
    DEF(cMod, "remove_method", module_remove_method, -1);
    DEF(cMod, "const_get",     module_const_get,     -1);
    DEF(cMod, "const_set",     module_const_set,     -1);
    DEF(cMod, "const_defined?", module_const_defined_p, -1);
    DEF(cMod, "remove_const",  module_remove_const,  -1);
    DEF(cMod, "remove_class_variable", module_remove_class_variable, -1);
    {
        VALUE mod_class_variable_get(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE mod_class_variable_set(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE mod_class_variable_defined_p(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE mod_class_variables(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cMod, "private_class_method",   module_private_class_method, -1);
    DEF(cMod, "public_class_method",    module_public_class_method,  -1);
    DEF(cMod, "private_constant",       module_private_constant,     -1);
    DEF(cMod, "public_constant",        module_public_constant,      -1);
    DEF(cMod, "class_variable_get",     mod_class_variable_get,     -1);
        DEF(cMod, "class_variable_set",     mod_class_variable_set,     -1);
        DEF(cMod, "class_variable_defined?", mod_class_variable_defined_p, -1);
        DEF(cMod, "class_variables",        mod_class_variables,        -1);
    }
    DEF(cMod, "===",           class_eqq,            1);
    /* Class < Module — Class instances inherit Module's methods.
     * No need to mirror module_* onto cCls. */

    /* Comparable instance methods */
    struct korb_class *cCmp = korb_vm->comparable_module;
    DEF(cCmp, "<",          cmp_lt,       1);
    DEF(cCmp, "<=",         cmp_le,       1);
    DEF(cCmp, ">",          cmp_gt,       1);
    DEF(cCmp, ">=",         cmp_ge,       1);
    DEF(cCmp, "==",         cmp_eq,       1);
    DEF(cCmp, "between?",   cmp_between, -1);
    DEF(cCmp, "clamp",      cmp_clamp,   -1);

    /* extra Object methods */
    /* Object dup / clone / instance_variables */
    DEF(cObj, "dup",                obj_dup,                   0);
    DEF(cObj, "clone",              obj_clone,                 0);
    DEF(cObj, "instance_variables", obj_instance_variables,    0);
    DEF(cObj, "instance_variable_defined?", obj_ivar_defined_p, 1);
    /* Kernel#__method__, caller, eval, loop, lambda, proc */
    DEF(cObj, "__method__",         kernel_method_name,        0);
    DEF(cObj, "__callee__",         kernel_method_name,        0);
    DEF(cObj, "caller",             kernel_caller,            -1);
    DEF(cObj, "__capture_lvars__",  kernel_capture_lvars,      0);
    DEF(cObj, "local_variables",    kernel_local_variables,    0);
    DEF(cObj, "eval",               kernel_eval_stub,         -1);
    DEF(cObj, "loop",               kernel_loop,               0);
    DEF(cObj, "initialize",         kernel_initialize_default, -1);
    DEF_PRIV(cObj, "lambda",        kernel_lambda,             0);
    DEF_PRIV(cObj, "proc",          kernel_proc,               0);
    /* Mirror to Kernel module so Kernel.private_method_defined? sees them. */
    if (korb_vm->kernel_module) {
        struct korb_class *kmod = korb_vm->kernel_module;
        korb_class_add_method_cfunc(kmod, korb_intern("lambda"), kernel_lambda, 0);
        korb_class_add_method_cfunc(kmod, korb_intern("proc"),   kernel_proc,   0);
        korb_class_add_method_cfunc(kmod, korb_intern("eval"),   kernel_eval_stub, -1);
        struct korb_method *m;
        if ((m = korb_class_find_method(kmod, korb_intern("lambda")))) m->visibility = KORB_VIS_PRIVATE;
        if ((m = korb_class_find_method(kmod, korb_intern("proc"))))   m->visibility = KORB_VIS_PRIVATE;
        if ((m = korb_class_find_method(kmod, korb_intern("eval"))))   m->visibility = KORB_VIS_PRIVATE;
    }
    /* Range#exclude_end? */
    DEF(cRng, "exclude_end?",       rng_exclude_end_p,         0);
    /* Class ancestors / Module#prepend */
    DEF(cMod, "ancestors",          class_ancestors,           0);
    DEF(cMod, "prepend",            module_prepend,           -1);
    {
        VALUE module_include_p(CTX *c, VALUE self, int argc, VALUE *argv) {
            if (argc < 1) return Qfalse;
            if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
            if (SPECIAL_CONST_P(argv[0]) ||
                (BUILTIN_TYPE(argv[0]) != T_MODULE && BUILTIN_TYPE(argv[0]) != T_CLASS)) {
                VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                korb_raise(c, (struct korb_class *)eT,
                           "wrong argument type %s (expected Module)",
                           SPECIAL_CONST_P(argv[0]) ? "?" :
                               korb_id_name(korb_class_of_class(argv[0])->name));
                return Qnil;
            }
            extern bool korb_module_has_ancestor(struct korb_class *, struct korb_class *);
            return KORB_BOOL(korb_module_has_ancestor((struct korb_class *)self,
                                                      (struct korb_class *)argv[0]));
        }
        DEF(cMod, "include?",       module_include_p,         1);
    }
    DEF(cObj, "extend",             obj_extend,               -1);
    DEF(cObj, "send",                  obj_send,                 -1);
    DEF(cObj, "__send__",              obj_send,                 -1);
    DEF(cObj, "public_send",           obj_public_send,          -1);
    DEF(cObj, "instance_variable_get", obj_instance_variable_get, 1);
    DEF(cObj, "instance_variable_set", obj_instance_variable_set, 2);
    DEF(cObj, "method",                obj_method,                1);
    DEF(cObj, "instance_of?",          obj_instance_of_p,         1);
    DEF(cObj, "===",                   obj_eqq,                   1);
    {
        VALUE obj_tap(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE obj_then(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE obj_itself(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cObj, "tap",        obj_tap,    0);
        DEF(cObj, "then",       obj_then,   0);
        DEF(cObj, "yield_self", obj_then,   0);
        DEF(cObj, "itself",     obj_itself, 0);
    }
    DEF_PRIV(cObj, "format",            kernel_format,            -1);
    DEF_PRIV(cObj, "sprintf",           kernel_format,            -1);
    if (korb_vm->kernel_module) {
        DEF_PRIV(korb_vm->kernel_module, "format",  kernel_format, -1);
        DEF_PRIV(korb_vm->kernel_module, "sprintf", kernel_format, -1);
    }
    /* Also expose as public methods on Kernel.singleton_class so the
     * `Kernel.format(...)` form works. */
    if (cKerMeta) {
        DEF(cKerMeta, "format",  kernel_format, -1);
        DEF(cKerMeta, "sprintf", kernel_format, -1);
    }
    DEF(cObj, "printf",                kernel_printf,            -1);

    /* extra Integer */
    DEF(cInt, "chr",   int_chr, 0);
    DEF(cInt, "===",   int_eqq, 1);
    DEF(cInt, "floor", int_floor, -1);
    DEF(cInt, "ceil",  int_floor, -1);
    DEF(cInt, "round",    int_round, -1);
    DEF(cInt, "floor",    int_floor, -1);
    DEF(cInt, "ceil",     int_ceil,  -1);
    DEF(cInt, "truncate", int_floor, -1);
    DEF(cInt, "abs",   int_abs, 0);
    DEF(cInt, "[]",    int_aref, -1);
    DEF(cInt, "bit_length", int_bit_length, 0);
    DEF(cInt, "divmod", int_divmod, 1);
    DEF(cInt, "**",    int_pow, 1);
    {
        VALUE int_invert(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cInt, "~", int_invert, 0);
    }
    DEF(cInt, "step",  int_step, -1);
    DEF(cInt, "upto",  int_upto, 1);
    DEF(cInt, "downto", int_downto, 1);
    DEF(cInt, "div",   int_method_div, 1);
    DEF(cInt, "fdiv",       int_fdiv,       1);
    DEF(cInt, "remainder",  int_remainder,  1);
    DEF(cInt, "modulo",     int_mod,        1);
    {
        VALUE int_abs(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cInt, "magnitude", int_abs, 0);
    }
    DEF(cInt, "size",  int_size, 0);
    DEF(cInt, "coerce", int_coerce, 1);
    DEF(cInt, "abs2",   int_abs2,   0);
    DEF(cFlt, "coerce", flt_coerce, 1);
    DEF(cFlt, "abs2",   flt_abs2,   0);

    /* extra Float */
    DEF(cFlt, "floor", flt_floor, -1);
    DEF(cFlt, "===",   flt_eqq, 1);
    DEF(cFlt, "**",    flt_pow, 1);
    DEF(cFlt, "<",     flt_lt, 1);
    DEF(cFlt, "<=",    flt_le, 1);
    DEF(cFlt, ">",     flt_gt, 1);
    DEF(cFlt, ">=",    flt_ge, 1);
    DEF(cFlt, "<=>",   flt_cmp, 1);
    DEF(cFlt, "==",    flt_eqq, 1);
    DEF(cFlt, "to_i",  flt_to_i, 0);
    DEF(cFlt, "to_f",  flt_to_f, 0);
    DEF(cFlt, "-@",    flt_uminus, 0);
    DEF(cFlt, "+@",    flt_uplus, 0);
    DEF(cFlt, "abs",   flt_abs, 0);
    DEF(cFlt, "magnitude", flt_abs, 0);
    DEF(cFlt, "ceil",     flt_ceil,    -1);
    DEF(cFlt, "round",    flt_round,   -1);
    DEF(cFlt, "truncate", flt_truncate, 0);

    /* extra String */
    DEF(cStr, "split",       str_split,       -1);
    DEF(cStr, "chomp",       str_chomp,       -1);
    DEF(cStr, "chomp!",      str_chomp_bang,  -1);
    DEF(cStr, "strip",       str_strip,        0);
    DEF(cStr, "strip!",      str_strip_bang,   0);
    DEF(cStr, "lstrip",      str_lstrip,       0);
    DEF(cStr, "lstrip!",     str_lstrip_bang,  0);
    DEF(cStr, "rstrip",      str_rstrip,       0);
    DEF(cStr, "rstrip!",     str_rstrip_bang,  0);
    DEF(cStr, "to_i",        str_to_i,        -1);
    DEF(cStr, "to_f",        str_to_f,         0);
    DEF(cStr, "[]",          str_aref,        -1);
    DEF(cStr, "[]=",         str_aset,        -1);
    DEF(cStr, "index",       str_index,       -1);
    DEF(cStr, "rindex",      str_rindex,      -1);
    DEF(cStr, "chars",       str_chars,        0);
    DEF(cStr, "bytes",       str_bytes,        0);
    DEF(cStr, "each_char",   str_each_char,    0);
    DEF(cStr, "each_line",   str_each_line,    0);
    DEF(cStr, "start_with?", str_start_with,  -1);
    DEF(cStr, "end_with?",   str_end_with,    -1);
    DEF(cStr, "include?",    str_include,     -1);
    DEF(cStr, "replace",     str_replace,      1);
    DEF(cStr, "reverse",     str_reverse,      0);
    DEF(cStr, "reverse!",    str_reverse_bang, 0);
    DEF(cStr, "upcase",      str_upcase,       0);
    DEF(cStr, "upcase!",     str_upcase_bang,  0);
    DEF(cStr, "downcase",    str_downcase,     0);
    DEF(cStr, "downcase!",   str_downcase_bang, 0);
    DEF(cStr, "empty?",      str_empty_p,      0);
    DEF(cStr, "*",           str_mul,          1);
    DEF(cStr, "hash",        str_hash,         0);
    DEF(cStr, "===",         str_eqq,          1);
    DEF(cStr, "gsub",        str_gsub,        -1);
    DEF(cStr, "gsub!",       str_gsub_bang,   -1);
    DEF(cStr, "sub",         str_sub,         -1);
    DEF(cStr, "sub!",        str_sub_bang,    -1);
    DEF(cStr, "tr",          str_tr,          -1);
    DEF(cStr, "tr!",         str_tr_bang,     -1);
    DEF(cStr, "tr_s",        str_tr_s,        -1);
    DEF(cStr, "tr_s!",       str_tr_s_bang,   -1);
    DEF(cStr, "%",           str_percent,     -1);
    DEF(cStr, "bytesize",    str_bytesize,     0);
    DEF(cStr, "inspect",     kernel_inspect,   0);
    DEF(cStr, "dup",         obj_dup,          0);
    DEF(cStr, "=~",          str_match_op, 1);
    DEF(cStr, "match?",      str_match_p, -1);
    DEF(cStr, "match",       str_match, -1);
    DEF(cStr, "scan",        str_scan, 1);
    DEF(cStr, "sum",         str_sum, -1);
    DEF(cStr, "unpack",      str_unpack, -1);
    DEF(cStr, "center",      str_center, -1);
    DEF(cStr, "ljust",       str_ljust,  -1);
    DEF(cStr, "rjust",       str_rjust,  -1);
    DEF(cStr, "chop",        str_chop,    0);
    DEF(cStr, "chop!",       str_chop_bang, 0);
    DEF(cStr, "count",       str_count_chars, -1);
    DEF(cStr, "delete",      str_delete_chars, -1);
    DEF(cStr, "squeeze",     str_squeeze, -1);
    DEF(cStr, "swapcase",     str_swapcase,        0);
    DEF(cStr, "swapcase!",    str_swapcase_bang,   0);
    DEF(cStr, "capitalize",   str_capitalize,      0);
    DEF(cStr, "capitalize!",  str_capitalize_bang, 0);
    DEF(cStr, "lines",       str_lines,   -1);
    DEF(cStr, "partition",   str_partition, 1);
    DEF(cStr, "rpartition",  str_rpartition, 1);
    DEF(cStr, "succ",        str_succ,    0);
    DEF(cStr, "next",        str_succ,    0);
    DEF(cStr, "each_byte",   str_each_byte, 0);
    DEF(cStr, "ord",         str_ord,     0);
    DEF(cStr, "eql?",        str_eql,     1);
    DEF(cStr, "clone",       str_clone,   0);
    DEF(cStr, "intern",      str_to_sym,  0);

    /* extra Array */
    DEF(cAry, "sort",       ary_sort,       -1);
    DEF(cAry, "sort_by",    ary_sort_by,     0);
    DEF(cAry, "zip",        ary_zip,        -1);
    DEF(cAry, "flatten",    ary_flatten,    -1);
    DEF(cAry, "compact",    ary_compact,     0);
    DEF(cAry, "uniq",       ary_uniq,       -1);
    DEF(cAry, "include?",   ary_include,     1);
    DEF(cAry, "any?",       ary_any_p,      -1);
    DEF(cAry, "all?",       ary_all_p,      -1);
    DEF(cAry, "none?",      ary_none_p,     -1);
    DEF(cAry, "min",        ary_min,        -1);
    DEF(cAry, "max",        ary_max,        -1);
    DEF(cAry, "sum",        ary_sum,        -1);
    DEF(cAry, "each_slice", ary_each_slice,  1);
    DEF(cAry, "step",       ary_step,       -1);
    DEF(cAry, "===",        ary_eqq,         1);
    DEF(cAry, "pack",       ary_pack,       -1);
    DEF(cAry, "concat",     ary_concat,     -1);
    DEF(cAry, "-",          ary_minus,       1);
    {
        VALUE ary_plus(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cAry, "+",          ary_plus,        1);
    }
    DEF(cAry, "index",      ary_index,      -1);
    DEF(cAry, "find_index", ary_index,      -1);
    DEF(cAry, "reverse",      ary_reverse,      0);
    DEF(cAry, "reverse_each", ary_reverse_each, 0);
    DEF(cAry, "clear",      ary_clear,       0);
    DEF(cAry, "unshift",    ary_unshift,    -1);
    DEF(cAry, "prepend",    ary_unshift,    -1);
    DEF(cAry, "shift",      ary_shift,      -1);
    DEF(cAry, "each_with_object", ary_each_with_object, 1);
    DEF(cAry, "transpose", ary_transpose, 0);
    DEF(cAry, "count",     ary_count, -1);
    DEF(cAry, "drop",      ary_drop,   1);
    DEF(cAry, "take",      ary_take,   1);
    DEF(cAry, "fill",      ary_fill,  -1);
    DEF(cAry, "sample",    ary_sample, -1);
    DEF(cAry, "empty?",    ary_empty_p, 0);
    DEF(cAry, "find",      ary_find, 0);
    DEF(cAry, "detect",    ary_find, 0);
    DEF(cAry, "min_by",    ary_min_by, 0);
    DEF(cAry, "max_by",    ary_max_by, 0);
    DEF(cAry, "*",         ary_mul, 1);
    DEF(cAry, "uniq!",     ary_uniq, -1);
    DEF(cAry, "sort!",     ary_sort_bang, -1);
    DEF(cAry, "compact!",  ary_compact_bang, 0);
    DEF(cAry, "reverse!",  ary_reverse_bang, 0);
    DEF(cAry, "rotate!",   ary_rotate_bang, -1);
    DEF(cAry, "rotate",    ary_rotate, -1);
    DEF(cAry, "flatten!",  ary_flatten, -1);
    DEF(cAry, "freeze",    kernel_freeze, 0);
    DEF(cAry, "frozen?",   kernel_frozen_p, 0);
    {
        VALUE ary_hash_content(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cAry, "hash",      ary_hash_content, 0);
    }
    DEF(cAry, "slice!",    ary_slice_bang, -1);
    DEF(cAry, "slice",     ary_slice_bang, -1); /* not quite right but ok */
    DEF(cAry, "flat_map",       ary_flat_map, 0);
    DEF(cAry, "collect_concat", ary_flat_map, 0);
    DEF(cAry, "dig",            ary_dig,      -1);
    DEF(cAry, "take_while",     ary_take_while, 0);
    DEF(cAry, "drop_while",     ary_drop_while, 0);
    DEF(cAry, "shuffle",        ary_shuffle,    0);
    DEF(cAry, "bsearch",        ary_bsearch,    0);
    DEF(cAry, "one?",           ary_one_p,     -1);
    /* String additions */
    DEF(cStr, "hex",           str_hex,           0);
    DEF(cStr, "oct",           str_oct,           0);
    DEF(cStr, "prepend",       str_prepend,      -1);
    DEF(cStr, "insert",        str_insert,        2);
    DEF(cStr, "delete_prefix",  str_delete_prefix,  1);
    DEF(cStr, "delete_prefix!", str_delete_prefix_bang, 1);
    DEF(cStr, "delete_suffix",  str_delete_suffix,  1);
    DEF(cStr, "delete_suffix!", str_delete_suffix_bang, 1);
    DEF(cStr, "byteslice",     str_byteslice,    -1);
    DEF(cStr, "append_as_bytes", str_append_as_bytes, -1);
    DEF(cStr, "setbyte",       str_setbyte,       2);
    DEF(cStr, "getbyte",       str_getbyte,       1);
    /* Numeric eql? — type-strict */
    DEF(cInt, "eql?",          int_eql,           1);
    DEF(cFlt, "eql?",          flt_eql,           1);
    DEF(cAry, "each_cons",      ary_each_cons,  1);
    DEF(cAry, "minmax_by",      ary_minmax_by,  0);
    DEF(cAry, "assoc",       ary_assoc,       1);
    DEF(cAry, "rassoc",      ary_rassoc,      1);
    DEF(cAry, "at",          ary_at,          1);
    DEF(cAry, "to_a",        ary_to_a,        0);
    DEF(cAry, "to_ary",      ary_self,        0);
    DEF(cAry, "deconstruct", ary_self,        0);
    DEF(cAry, "fetch",       ary_fetch,       -1);
    DEF(cAry, "fetch_values", ary_fetch_values, -1);
    DEF(cAry, "delete",      ary_delete,      1);
    DEF(cAry, "delete_at",   ary_delete_at,   1);
    DEF(cAry, "delete_if",   ary_delete_if,   0);
    DEF(cAry, "reject",      ary_reject,      0);
    DEF(cAry, "reject!",     ary_delete_if,   0);
    DEF(cAry, "insert",      ary_insert,     -1);
    DEF(cAry, "replace",     ary_replace,     1);
    DEF(cAry, "each_index",  ary_each_index,  0);
    DEF(cAry, "clone",       ary_clone,       0);
    DEF(cAry, "eql?",        ary_eql,         1);
    DEF(cAry, "<=>",         ary_cmp,         1);
    DEF(cAry, "combination", ary_combination, 1);
    DEF(cAry, "permutation", ary_permutation, -1);
    DEF(cAry, "product",     ary_product,    -1);
    {
        /* Override Class.new on Array's metaclass so Array.new(n, default)
         * and Array.new(n) { ... } actually build an array of the right
         * size — Class#new's generic path uses korb_object_new which
         * doesn't size a T_ARRAY correctly. */
        struct korb_class *cAryMeta = korb_class_new(korb_intern("ArrayMeta"),
                                                      korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cAryMeta, korb_intern("new"), ary_class_new, -1);
        /* Array[] — class method that returns an Array literal of args. */
        VALUE ary_class_brackets(CTX *c, VALUE self, int argc, VALUE *argv);
        korb_class_add_method_cfunc(cAryMeta, korb_intern("[]"), ary_class_brackets, -1);
        cAry->basic.klass = (VALUE)cAryMeta;
        /* Array.try_convert(obj) — obj.to_ary if obj responds and returns
         * Array, else nil.  Raises TypeError if #to_ary returns non-Array. */
        korb_class_add_method_cfunc(cAryMeta, korb_intern("try_convert"),
            ({
                VALUE _try(CTX *c, VALUE self, int argc, VALUE *argv) {
                    if (argc < 1) return Qnil;
                    VALUE o = argv[0];
                    if (!SPECIAL_CONST_P(o) && BUILTIN_TYPE(o) == T_ARRAY) return o;
                    VALUE klass_v = (VALUE)korb_class_of_class(o);
                    if (!klass_v || !korb_class_find_method((struct korb_class *)klass_v,
                                                             korb_intern("to_ary"))) {
                        return Qnil;
                    }
                    VALUE r = korb_funcall(c, o, korb_intern("to_ary"), 0, NULL);
                    if (c->state == KORB_RAISE) return Qnil;
                    if (NIL_P(r)) return Qnil;
                    if (SPECIAL_CONST_P(r) || BUILTIN_TYPE(r) != T_ARRAY) {
                        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                        const char *src_n = korb_id_name(korb_class_of_class(o)->name);
                        const char *got_n = SPECIAL_CONST_P(r) ? "(special)"
                                                                : korb_id_name(korb_class_of_class(r)->name);
                        korb_raise(c, (struct korb_class *)eT,
                                   "can't convert %s to Array (%s#to_ary gives %s)",
                                   src_n, src_n, got_n);
                        return Qnil;
                    }
                    return r;
                }
                _try;
            }), -1);
    }

    /* extra Hash */
    DEF(cHsh, "keys",       hash_keys,       0);
    DEF(cHsh, "values",     hash_values,     0);
    DEF(cHsh, "each_value", hash_each_value, 0);
    DEF(cHsh, "each_key",   hash_each_key,   0);
    DEF(cHsh, "each_pair",  hash_each,       0);
    DEF(cHsh, "key?",       hash_key_p,      1);
    DEF(cHsh, "has_key?",   hash_key_p,      1);
    DEF(cHsh, "include?",   hash_key_p,      1);
    DEF(cHsh, "merge",      hash_merge,     -1);
    DEF(cHsh, "merge!",     hash_merge_bang,-1);
    DEF(cHsh, "invert",     hash_invert,     0);
    DEF(cHsh, "to_a",       hash_to_a,       0);
    DEF(cHsh, "delete",     hash_delete,    -1);
    DEF(cHsh, "fetch",      hash_fetch,     -1);
    DEF(cHsh, "__korb_required_kwarg__", hash_required_kwarg, 1);
    DEF(cHsh, "__korb_required_kwargs_check__", hash_required_kwargs_check, 1);
    DEF(cHsh, "__korb_kwargs_validate__", hash_kwargs_validate, 1);
    DEF(cHsh, "compare_by_identity",  hash_compare_by_identity, 0);
    DEF(cHsh, "compare_by_identity?", hash_compare_by_identity_p, 0);
    DEF(cHsh, "clear",       hash_clear,        0);
    DEF(cHsh, "delete_if",   hash_delete_if,    0);
    DEF(cHsh, "keep_if",     hash_keep_if,      0);
    DEF(cHsh, "compact",     hash_compact,      0);
    DEF(cHsh, "compact!",    hash_compact_bang, 0);
    DEF(cHsh, "values_at",   hash_values_at,   -1);
    DEF(cHsh, "fetch_values",hash_fetch_values,-1);
    DEF(cHsh, "member?",     hash_key_p,        1);
    DEF(cHsh, "reject",      hash_reject,       0);
    {
        VALUE hash_reject_bang(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cHsh, "reject!",     hash_reject_bang,  0);
    }
    DEF(cHsh, "replace",     hash_replace,      1);
    DEF(cHsh, "shift",       hash_shift,        0);
    DEF(cHsh, "store",       hash_aset,         2);
    DEF(cHsh, "update",      hash_merge_bang,  -1);
    DEF(cHsh, "slice",       hash_slice,       -1);
    DEF(cHsh, "except",      hash_except,      -1);
    DEF(cHsh, "count",       hash_count,       -1);
    DEF(cHsh, "min_by",      hash_min_by,       0);
    DEF(cHsh, "max_by",      hash_max_by,       0);
    DEF(cHsh, "sort",        hash_sort,         0);
    DEF(cHsh, "deconstruct_keys", hash_deconstruct_keys, 1);
    DEF(cHsh, "dig",              hash_dig,              -1);
    DEF(cHsh, "has_value?",       hash_has_value_p,       1);
    DEF(cHsh, "value?",           hash_has_value_p,       1);
    DEF(cHsh, "group_by",         hash_group_by,          0);
    DEF(cHsh, "sort_by",          hash_sort_by,           0);
    DEF(cHsh, "filter_map",       hash_filter_map,        0);
    DEF(cHsh, "sum",              hash_sum,              -1);
    DEF(cHsh, "each_with_object", hash_each_with_object,  1);
    DEF(cHsh, "take",             hash_take,              1);
    DEF(cHsh, "flat_map",         hash_flat_map,          0);
    DEF(cHsh, "collect_concat",   hash_flat_map,          0);
    DEF(cHsh, "default",      hash_default_get,      0);
    DEF(cHsh, "default=",     hash_default_set,      1);
    DEF(cHsh, "default_proc", hash_default_proc_get, 0);
    DEF(cHsh, "default_proc=", hash_default_proc_set, 1);
    {
        /* Override Class.new on Hash's metaclass so Hash.new(default) and
         * Hash.new { ... } actually create a real hash with the default. */
        struct korb_class *cHshMeta = korb_class_new(korb_intern("HashMeta"),
                                                      korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cHshMeta, korb_intern("new"), hash_class_new, -1);
        korb_class_add_method_cfunc(cHshMeta, korb_intern("[]"),  hash_class_aref, -1);
        cHsh->basic.klass = (VALUE)cHshMeta;
        /* Hash.try_convert(obj) — obj.to_hash if responding and returns
         * Hash, else nil.  Raises TypeError if #to_hash returns non-Hash. */
        korb_class_add_method_cfunc(cHshMeta, korb_intern("try_convert"),
            ({
                VALUE _try(CTX *c, VALUE self, int argc, VALUE *argv) {
                    if (argc < 1) return Qnil;
                    VALUE o = argv[0];
                    if (!SPECIAL_CONST_P(o) && BUILTIN_TYPE(o) == T_HASH) return o;
                    if (SPECIAL_CONST_P(o)) return Qnil;
                    VALUE r = korb_funcall(c, o, korb_intern("to_hash"), 0, NULL);
                    if (c->state == KORB_RAISE) {
                        VALUE bang = c->state_value;
                        VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
                        if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) &&
                            BUILTIN_TYPE(eNo) == T_CLASS) {
                            struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                            for (struct korb_class *kk = bk; kk; kk = kk->super) {
                                if (kk == (struct korb_class *)eNo) {
                                    c->state = KORB_NORMAL; c->state_value = Qnil;
                                    return Qnil;
                                }
                            }
                        }
                        return Qnil;
                    }
                    if (NIL_P(r)) return Qnil;
                    if (SPECIAL_CONST_P(r) || BUILTIN_TYPE(r) != T_HASH) {
                        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                        korb_raise(c, (struct korb_class *)eT,
                                   "can't convert %s to Hash (%s#to_hash gives %s)",
                                   korb_id_name(korb_class_of_class(o)->name),
                                   korb_id_name(korb_class_of_class(o)->name),
                                   korb_id_name(korb_class_of_class(r)->name));
                        return Qnil;
                    }
                    return r;
                }
                _try;
            }), -1);
    }
    /* String.new(s = "") — initialize from optional string. */
    {
        VALUE str_class_new(CTX *c, VALUE self, int argc, VALUE *argv);
        struct korb_class *cStrMeta = korb_class_new(korb_intern("StringMeta"),
                                                      korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cStrMeta, korb_intern("new"), str_class_new, -1);
        cStr->basic.klass = (VALUE)cStrMeta;
        /* String.try_convert(obj) — obj.to_str if responding and returns
         * String, else nil.  Raises TypeError if #to_str returns non-String. */
        korb_class_add_method_cfunc(cStrMeta, korb_intern("try_convert"),
            ({
                VALUE _try(CTX *c, VALUE self, int argc, VALUE *argv) {
                    if (argc < 1) return Qnil;
                    VALUE o = argv[0];
                    if (!SPECIAL_CONST_P(o) && BUILTIN_TYPE(o) == T_STRING) return o;
                    if (SPECIAL_CONST_P(o)) return Qnil;
                    VALUE r = korb_funcall(c, o, korb_intern("to_str"), 0, NULL);
                    if (c->state == KORB_RAISE) {
                        VALUE bang = c->state_value;
                        VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
                        if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) &&
                            BUILTIN_TYPE(eNo) == T_CLASS) {
                            struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                            for (struct korb_class *kk = bk; kk; kk = kk->super) {
                                if (kk == (struct korb_class *)eNo) {
                                    c->state = KORB_NORMAL; c->state_value = Qnil;
                                    return Qnil;
                                }
                            }
                        }
                        return Qnil;
                    }
                    if (NIL_P(r)) return Qnil;
                    if (SPECIAL_CONST_P(r) || BUILTIN_TYPE(r) != T_STRING) {
                        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                        korb_raise(c, (struct korb_class *)eT,
                                   "can't convert %s to String (%s#to_str gives %s)",
                                   korb_id_name(korb_class_of_class(o)->name),
                                   korb_id_name(korb_class_of_class(o)->name),
                                   korb_id_name(korb_class_of_class(r)->name));
                        return Qnil;
                    }
                    return r;
                }
                _try;
            }), -1);
    }
    DEF(cHsh, "===",        hash_eqq,        1);
    DEF(cHsh, "dup",        hash_dup,        0);
    DEF(cHsh, "clone",      hash_clone,      0);
    DEF(cHsh, "empty?",     hash_empty_p,    0);
    DEF(cHsh, "map",        hash_map,        0);
    DEF(cHsh, "collect",    hash_map,        0);
    DEF(cHsh, "select",     hash_select,     0);
    DEF(cHsh, "filter",     hash_select,     0);
    DEF(cHsh, "partition",  hash_partition,  0);
    DEF(cHsh, "tally",      hash_tally,      0);
    DEF(cHsh, "filter",     hash_select,     0);
    DEF(cHsh, "reduce",     hash_reduce,    -1);
    DEF(cHsh, "inject",     hash_reduce,    -1);

    /* extra Range */
    DEF(cRng, "step",     rng_step,    -1);
    DEF(cRng, "zip",      rng_zip,     -1);
    DEF(cRng, "each_with_index", rng_each_with_index, 0);
    DEF(cRng, "size",     rng_size,     0);
    DEF(cRng, "length",   rng_size,     0);
    DEF(cRng, "include?", rng_include, -1);
    DEF(cRng, "===",      rng_include, -1);
    DEF(cRng, "map",      rng_map,      0);
    DEF(cRng, "collect",  rng_map,      0);
    DEF(cRng, "select",   rng_select,   0);
    DEF(cRng, "filter",   rng_select,   0);
    DEF(cRng, "reduce",   rng_reduce,  -1);
    DEF(cRng, "inject",   rng_reduce,  -1);
    DEF(cRng, "all?",     rng_all_p,    0);
    DEF(cRng, "any?",     rng_any_p,    0);
    DEF(cRng, "count",    rng_count,    0);

    /* extra Symbol additions later (cSym defined further down) */

    /* Struct.new */
    /* Create Struct class object */
    struct korb_class *cStruct = korb_class_new(korb_intern("Struct"), korb_vm->object_class, T_OBJECT);
    korb_const_set(korb_vm->object_class, korb_intern("Struct"), (VALUE)cStruct);
    /* Struct.new is a class-level cfunc — install on Class so any class can call .new */
    /* But only Struct itself should have this constructor.  We add it on the class itself's
     * method table; calling Struct.new dispatches to class_of(Struct) which is Class.
     * Workaround: install on the Struct class's "self class" which is Class — but that
     * makes ALL classes have struct_class_new.  Instead, install a static name like
     * "__new_struct__" and use a stub cfunc on Struct that detects Struct === self.
     *
     * Simpler: add Class#new to delegate to self.class_new if class is Struct.
     * Even simpler: replace Class#new with a wrapper that handles Struct specially. */
    /* For our purposes, just add Struct as a singleton-like method to cCls keyed by the
     * actual class identity check: we install struct_class_new on cCls under "new_struct"
     * and add a method on Struct that calls it. */
    /* ... easier: just inject a method on cStruct at the "class level" via metaclass —
     * but we don't model singleton classes.  Instead, the simplest hack: install a cfunc
     * on Class itself that checks if self == Struct and calls struct_class_new. */
    /* Actually even simpler: make Struct.new == Class.new + struct_class_new logic.
     * Approach: provide a builtin on Class that, when self is Struct, returns a new struct
     * class.  For other classes, falls back to normal new. */
    /* implementing as: replace class_new */
    /* We modify class_new defined above — but it's static.  Instead add a layered method. */
    {
        extern VALUE class_new(CTX *c, VALUE self, int argc, VALUE *argv);
        /* not exposed — we need a wrapper.  Define inline: */
    }
    /* Add struct_class_new under name "new" on Struct.  Since dispatch goes through
     * class_of(Struct) = Class, NOT Struct itself — we need to use a different approach.
     * For now, let users call Struct.new(...) and ensure that lookup goes to Struct's
     * own metaclass.  We create a special ko_class for Struct's metaclass with .new
     * pointing to struct_class_new. */
    {
        struct korb_class *cStructMeta = korb_class_new(korb_intern("StructMeta"), korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cStructMeta, korb_intern("new"), struct_class_new, -1);
        cStruct->basic.klass = (VALUE)cStructMeta;
    }

    /* Data.define — like Struct but immutable.  We implement as a thin
     * shim over struct_class_new and freeze the instance after init. */
    {
        struct korb_class *cData = korb_class_new(korb_intern("Data"), korb_vm->object_class, T_OBJECT);
        korb_const_set(korb_vm->object_class, korb_intern("Data"), (VALUE)cData);
        struct korb_class *cDataMeta = korb_class_new(korb_intern("DataMeta"),
                                                       korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cDataMeta, korb_intern("define"),
                                     struct_class_new, -1);
        cData->basic.klass = (VALUE)cDataMeta;
    }

    /* File class */
    struct korb_class *cFile = korb_class_new(korb_intern("File"), korb_vm->object_class, T_OBJECT);
    korb_const_set(korb_vm->object_class, korb_intern("File"), (VALUE)cFile);
    {
        struct korb_class *cFileMeta = korb_class_new(korb_intern("FileMeta"), korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("read"), file_read, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("join"), file_join, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("exist?"), file_exist_p, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("exists?"), file_exist_p, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("directory?"), file_directory_p, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("file?"),     file_file_p,     -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("size"),      file_size,       -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("unlink"),    file_unlink,     -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("delete"),    file_unlink,     -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("rename"),    file_rename,     -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("chmod"),     file_chmod,      -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("realpath"),  file_realpath,   -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("dirname"), file_dirname, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("basename"), file_basename, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("expand_path"), file_expand_path, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("extname"), file_extname, 1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("binread"), file_binread, 1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("open"), file_open, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("write"), file_write, -1);
        cFile->basic.klass = (VALUE)cFileMeta;
    }
    /* Dir / Process classes — stubs so common Ruby idioms don't NPE. */
    {
        struct korb_class *cDir = korb_class_new(korb_intern("Dir"), korb_vm->object_class, T_OBJECT);
        korb_const_set(korb_vm->object_class, korb_intern("Dir"), (VALUE)cDir);
        struct korb_class *cDirMeta = korb_class_new(korb_intern("DirMeta"), korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("pwd"),     dir_pwd,     0);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("getwd"),   dir_pwd,     0);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("entries"), dir_entries, 1);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("chdir"),   dir_chdir,  -1);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("glob"),    dir_glob,    1);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("[]"),      dir_glob,    1);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("mkdir"),   dir_mkdir,  -1);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("rmdir"),   dir_rmdir,  -1);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("delete"),  dir_rmdir,  -1);
        korb_class_add_method_cfunc(cDirMeta, korb_intern("unlink"),  dir_rmdir,  -1);
        cDir->basic.klass = (VALUE)cDirMeta;
    }
    {
        struct korb_class *cProcess = korb_class_new(korb_intern("Process"), korb_vm->object_class, T_OBJECT);
        korb_const_set(korb_vm->object_class, korb_intern("Process"), (VALUE)cProcess);
        struct korb_class *cProcessMeta = korb_class_new(korb_intern("ProcessMeta"), korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cProcessMeta, korb_intern("pid"), process_pid, 0);
        korb_class_add_method_cfunc(cProcessMeta, korb_intern("clock_gettime"), proc_clock_gettime_stub, -1);
        korb_class_add_method_cfunc(cProcessMeta, korb_intern("spawn"), process_spawn, -1);
        korb_class_add_method_cfunc(cProcessMeta, korb_intern("fork"), process_fork, 0);
        korb_class_add_method_cfunc(cProcessMeta, korb_intern("wait"), process_wait, -1);
        korb_class_add_method_cfunc(cProcessMeta, korb_intern("waitpid"), process_wait, -1);
        korb_class_add_method_cfunc(cProcessMeta, korb_intern("kill"), process_kill, -1);
        cProcess->basic.klass = (VALUE)cProcessMeta;
        /* Process::Status — minimal class with attribute readers. */
        struct korb_class *cStatus = korb_class_new(korb_intern("Status"), korb_vm->object_class, T_OBJECT);
        korb_const_set(cProcess, korb_intern("Status"), (VALUE)cStatus);
        korb_class_add_method_cfunc(cStatus, korb_intern("exitstatus"), pstatus_exitstatus, 0);
        korb_class_add_method_cfunc(cStatus, korb_intern("pid"), pstatus_pid, 0);
        korb_class_add_method_cfunc(cStatus, korb_intern("success?"), pstatus_success_p, 0);
        korb_class_add_method_cfunc(cStatus, korb_intern("signaled?"), pstatus_signaled_p, 0);
        korb_class_add_method_cfunc(cStatus, korb_intern("termsig"), pstatus_termsig, 0);
        korb_class_add_method_cfunc(cStatus, korb_intern("to_i"), pstatus_to_i, 0);
        /* Signal — module with class methods. */
        struct korb_class *cSignal = korb_module_new(korb_intern("Signal"));
        korb_const_set(korb_vm->object_class, korb_intern("Signal"), (VALUE)cSignal);
        struct korb_class *cSignalMeta = korb_singleton_class_of(cSignal);
        korb_class_add_method_cfunc(cSignalMeta, korb_intern("trap"), signal_trap, -1);
        korb_class_add_method_cfunc(cSignalMeta, korb_intern("list"), signal_list, 0);
        /* Kernel#system / `cmd` / exec at top-level (Object). */
        DEF(cObj, "system", kernel_system, -1);
        DEF(cObj, "`",      kernel_xstring, 1);
        DEF(cObj, "exec",   kernel_exec, -1);
        DEF(cObj, "fork",   process_fork, 0);
        DEF(cObj, "spawn",  process_spawn, -1);
        DEF(cObj, "trap",   signal_trap, -1);
        /* CLOCK_MONOTONIC constant on Process — sentinel value, used
         * only by clock_gettime which ignores it. */
        korb_const_set(cProcess, korb_intern("CLOCK_MONOTONIC"), INT2FIX(1));
        korb_const_set(cProcess, korb_intern("CLOCK_REALTIME"), INT2FIX(0));
    }

    /* Instance methods on File: it doubles as our IO class for opened
     * files.  Walk-style readers + line iterators + writers. */
    korb_class_add_method_cfunc(cFile, korb_intern("close"),     io_close,     0);
    korb_class_add_method_cfunc(cFile, korb_intern("read"),      io_read,     -1);
    korb_class_add_method_cfunc(cFile, korb_intern("gets"),      io_gets,     -1);
    korb_class_add_method_cfunc(cFile, korb_intern("each_line"), io_each_line, 0);
    korb_class_add_method_cfunc(cFile, korb_intern("each"),      io_each_line, 0);
    korb_class_add_method_cfunc(cFile, korb_intern("puts"),      io_puts,     -1);
    korb_class_add_method_cfunc(cFile, korb_intern("write"),     io_write,    -1);
    korb_class_add_method_cfunc(cFile, korb_intern("print"),     io_print,    -1);
    korb_class_add_method_cfunc(cFile, korb_intern("eof?"),      io_eof_p,     0);

    /* IO / STDOUT / $stdout */
    struct korb_class *cIO = korb_class_new(korb_intern("IO"), korb_vm->object_class, T_OBJECT);
    korb_const_set(korb_vm->object_class, korb_intern("IO"), (VALUE)cIO);
    /* dummy STDOUT/STDERR */
    VALUE stdout_obj = korb_object_new(cIO);
    VALUE stderr_obj = korb_object_new(cIO);
    g_stderr_obj = stderr_obj;
    korb_const_set(korb_vm->object_class, korb_intern("STDOUT"), stdout_obj);
    korb_const_set(korb_vm->object_class, korb_intern("STDERR"), stderr_obj);
    /* STDIN — backed by the real stdin FILE*, so STDIN.gets / .read work. */
    VALUE stdin_obj = korb_object_new(cIO);
    korb_ivar_set(stdin_obj, korb_intern("@__fp__"),
                  INT2FIX((long)(uintptr_t)stdin));
    korb_const_set(korb_vm->object_class, korb_intern("STDIN"), stdin_obj);
    /* IO#puts / write methods */
    korb_class_add_method_cfunc(cIO, korb_intern("puts"), kernel_puts, -1);
    korb_class_add_method_cfunc(cIO, korb_intern("print"), kernel_print, -1);
    korb_class_add_method_cfunc(cIO, korb_intern("write"), kernel_print, -1);
    korb_class_add_method_cfunc(cIO, korb_intern("flush"), kernel_inspect, 0);
    korb_class_add_method_cfunc(cIO, korb_intern("sync="), kernel_inspect, 1);
    /* IO instances also need gets/read/each_line/eof?/close — share
     * the same impls as File. */
    korb_class_add_method_cfunc(cIO, korb_intern("gets"),      io_gets,     -1);
    korb_class_add_method_cfunc(cIO, korb_intern("read"),      io_read,     -1);
    korb_class_add_method_cfunc(cIO, korb_intern("each_line"), io_each_line, 0);
    korb_class_add_method_cfunc(cIO, korb_intern("each"),      io_each_line, 0);
    korb_class_add_method_cfunc(cIO, korb_intern("eof?"),      io_eof_p,     0);
    korb_class_add_method_cfunc(cIO, korb_intern("close"),     io_close,     0);
    /* IO.pipe / IO.select / IO.popen / IO.copy_stream — class methods on
     * IO's singleton. */
    {
        extern VALUE io_class_pipe(CTX *c, VALUE self, int argc, VALUE *argv);
        extern VALUE io_class_select(CTX *c, VALUE self, int argc, VALUE *argv);
        extern VALUE io_class_popen(CTX *c, VALUE self, int argc, VALUE *argv);
        extern VALUE io_class_copy_stream(CTX *c, VALUE self, int argc, VALUE *argv);
        struct korb_class *cIOMeta = korb_singleton_class_of(cIO);
        korb_class_add_method_cfunc(cIOMeta, korb_intern("pipe"),
                                     io_class_pipe, -1);
        korb_class_add_method_cfunc(cIOMeta, korb_intern("select"),
                                     io_class_select, -1);
        korb_class_add_method_cfunc(cIOMeta, korb_intern("popen"),
                                     io_class_popen, -1);
        korb_class_add_method_cfunc(cIOMeta, korb_intern("copy_stream"),
                                     io_class_copy_stream, -1);
    }
    korb_class_add_method_cfunc(cIO, korb_intern("tty?"),    io_tty_p,   0);
    korb_class_add_method_cfunc(cIO, korb_intern("isatty"),  io_tty_p,   0);
    korb_class_add_method_cfunc(cIO, korb_intern("fileno"),  io_fileno,  0);
    korb_class_add_method_cfunc(cIO, korb_intern("to_i"),    io_fileno,  0);

    /* gvars */
    korb_gvar_set(korb_intern("$stdout"), stdout_obj);
    korb_gvar_set(korb_intern("$stderr"), stderr_obj);

    /* Symbol */
    struct korb_class *cSym = korb_vm->symbol_class;
    {
        struct korb_class *cSymMeta = korb_singleton_class_of(cSym);
        DEF(cSymMeta, "allocate", _allocator_disallowed, -1);
        DEF(cSymMeta, "new",      _new_disallowed,       -1);
    }
    {
        VALUE obj_itself(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cSym, "to_sym",  obj_itself, 0);
    }
    DEF(cSym, "to_s", sym_to_s, 0);
    DEF(cSym, "id2name", sym_to_s, 0);
    DEF(cSym, "==", sym_eq, 1);
    DEF(cSym, "to_proc", sym_to_proc, 0);
    DEF(cSym, "===", sym_eq, 1);
    DEF(cSym, "inspect", kernel_inspect, 0);
    DEF(cSym, "<=>",     sym_cmp,        1);
    DEF(cSym, "succ",    sym_succ,       0);
    DEF(cSym, "next",    sym_succ,       0);
    DEF(cSym, "size",    sym_length,     0);
    DEF(cSym, "length",  sym_length,     0);
    DEF(cSym, "empty?",  sym_empty_p,    0);
    DEF(cSym, "upcase",     sym_upcase,     0);
    DEF(cSym, "downcase",   sym_downcase,   0);
    DEF(cSym, "capitalize", sym_capitalize, 0);
    DEF(cSym, "swapcase",   sym_swapcase,   0);

    /* Boolean / Nil */
    DEF(korb_vm->true_class, "to_s", true_to_s, 0);
    DEF(korb_vm->false_class, "to_s", false_to_s, 0);
    DEF(korb_vm->nil_class, "to_s", nil_to_s, 0);
    DEF(korb_vm->nil_class, "inspect", nil_inspect, 0);
    /* Boolean &/|/^ — Kernel-style coercion to truthy. */
    DEF(korb_vm->true_class,  "&", true_and,  1);
    DEF(korb_vm->true_class,  "|", true_or,   1);
    DEF(korb_vm->true_class,  "^", true_xor,  1);
    DEF(korb_vm->false_class, "&", false_and, 1);
    DEF(korb_vm->false_class, "|", false_or,  1);
    DEF(korb_vm->false_class, "^", false_xor, 1);
    DEF(korb_vm->nil_class,   "&", nil_and,   1);
    DEF(korb_vm->nil_class,   "|", nil_or,    1);
    DEF(korb_vm->nil_class,   "^", nil_xor,   1);
    DEF(korb_vm->nil_class,   "to_a", nil_to_a, 0);
    DEF(korb_vm->nil_class,   "to_h", nil_to_h, 0);
    DEF(korb_vm->nil_class,   "to_f", nil_to_f, 0);
    DEF(korb_vm->nil_class,   "to_i", nil_to_i, 0);
    DEF(korb_vm->nil_class,   "nil?", nil_nil_p, 0);

    /* Proc */
    struct korb_class *cPrc = korb_vm->proc_class;
    DEF(cPrc, "call", proc_call, -1);
    DEF(cPrc, "[]", proc_call, -1);
    /* Proc#=== — same as #call.  Used by case/when with a proc value
     * pattern (`case x; when ->(...) { ... }`) and by pattern matching
     * (`case x; in ->(...) { ... }`). */
    DEF(cPrc, "===", proc_call, -1);
    DEF(cPrc, "lambda?", proc_lambda_p, 0);
    DEF(cPrc, "arity", proc_arity, 0);
    {
        VALUE proc_parameters(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE proc_source_location(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cPrc, "parameters",      proc_parameters,      0);
        DEF(cPrc, "source_location", proc_source_location, 0);
    }
    DEF(cPrc, "==", proc_eq, 1);
    DEF(cPrc, "eql?", proc_eq, 1);
    {
        struct korb_class *cProcMeta = korb_class_new(korb_intern("ProcMeta"),
                                                      korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cProcMeta, korb_intern("new"), proc_class_new, -1);
        /* Proc.allocate raises TypeError (CRuby) — no proc allocation
         * without a block. */
        {
            VALUE _proc_alloc_raise(CTX *c, VALUE self, int argc, VALUE *argv) {
                VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                korb_raise(c, (struct korb_class *)eT,
                           "allocator undefined for Proc");
                return Qnil;
            }
            korb_class_add_method_cfunc(cProcMeta, korb_intern("allocate"),
                                        _proc_alloc_raise, 0);
        }
        cPrc->basic.klass = (VALUE)cProcMeta;
    }
    {
        VALUE obj_itself(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cPrc, "to_proc", obj_itself, 0);
    }

    /* Time stub class (Process is set up earlier with proper meta). */

    struct korb_class *cTime = korb_class_new(korb_intern("Time"), korb_vm->object_class, T_OBJECT);
    korb_const_set(korb_vm->object_class, korb_intern("Time"), (VALUE)cTime);
    {
        struct korb_class *cTimeMeta = korb_class_new(korb_intern("TimeMeta"),
                                                       korb_vm->class_class, T_CLASS);
        extern VALUE time_now_stub(CTX *c, VALUE self, int argc, VALUE *argv);
        korb_class_add_method_cfunc(cTimeMeta, korb_intern("now"), time_now_stub, 0);
        cTime->basic.klass = (VALUE)cTimeMeta;
    }

    /* Fiber */
    struct korb_class *cFiber = korb_class_new(korb_intern("Fiber"), korb_vm->object_class, T_DATA);
    korb_const_set(korb_vm->object_class, korb_intern("Fiber"), (VALUE)cFiber);
    korb_vm->fiber_class = cFiber;
    {
        /* Fiber.new {|x| ...} */
        extern VALUE korb_fiber_new_cfunc(CTX *c, VALUE self, int argc, VALUE *argv);
        struct korb_class *cFiberMeta = korb_class_new(korb_intern("FiberMeta"),
                                                        korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cFiberMeta, korb_intern("new"), korb_fiber_new_cfunc, 0);
        /* Fiber.yield */
        extern VALUE korb_fiber_yield_cfunc(CTX *c, VALUE self, int argc, VALUE *argv);
        korb_class_add_method_cfunc(cFiberMeta, korb_intern("yield"), korb_fiber_yield_cfunc, -1);
        cFiber->basic.klass = (VALUE)cFiberMeta;
    }
    {
        extern VALUE korb_fiber_resume_cfunc(CTX *c, VALUE self, int argc, VALUE *argv);
        korb_class_add_method_cfunc(cFiber, korb_intern("resume"), korb_fiber_resume_cfunc, -1);
    }

    /* Binding class — instances are returned by Kernel#binding.  T_DATA
     * because korb_binding has its own storage layout (fp pointer +
     * names + cref) that doesn't fit T_OBJECT's ivar table. */
    {
        struct korb_class *cBinding = korb_class_new(korb_intern("Binding"), korb_vm->object_class, T_DATA);
        korb_const_set(korb_vm->object_class, korb_intern("Binding"), (VALUE)cBinding);
        korb_class_add_method_cfunc(cBinding, korb_intern("local_variable_get"),    binding_local_variable_get,       1);
        korb_class_add_method_cfunc(cBinding, korb_intern("local_variable_set"),    binding_local_variable_set,       2);
        korb_class_add_method_cfunc(cBinding, korb_intern("local_variable_defined?"), binding_local_variable_defined_p, 1);
        korb_class_add_method_cfunc(cBinding, korb_intern("local_variables"),       binding_local_variables_cfunc,    0);
        korb_class_add_method_cfunc(cBinding, korb_intern("receiver"),              binding_receiver,                 0);
        korb_class_add_method_cfunc(cBinding, korb_intern("eval"),                  binding_eval_cfunc,              -1);
        korb_class_add_method_cfunc(cBinding, korb_intern("source_location"),       binding_source_location,          0);
        korb_class_add_method_cfunc(cBinding, korb_intern("dup"),                   binding_dup_cfunc,                0);
        korb_class_add_method_cfunc(cBinding, korb_intern("clone"),                 binding_clone_cfunc,             -1);
        korb_vm->binding_class = cBinding;
    }
    DEF_PRIV(cObj, "binding", kernel_binding_cfunc, 0);
    /* Proc#binding — return a Binding capturing the proc's env / self / cref. */
    if (korb_vm->proc_class) {
        DEF(korb_vm->proc_class, "binding", proc_binding_cfunc, 0);
    }
    /* Kernel#binding is module_function-style: PRIVATE instance method
     * (so user code's bare `binding` works) AND PUBLIC class method
     * (so `Kernel.binding` works).  We register on Kernel.metaclass
     * for the class-method side.  Note: do NOT add a regular instance
     * method on Kernel itself — our T_MODULE dispatch checks the
     * module's own method table first, which would find that and
     * raise "private method" for explicit `Kernel.binding` calls. */
    if (cKerMeta) {
        DEF(cKerMeta, "binding", kernel_binding_cfunc, 0);
    }
    /* Mirror the visibility info on Kernel itself for
     * Kernel.private_method_defined?(:binding) — the lookup needs
     * the method present on Kernel.  Add but mark private so the
     * T_MODULE dispatch above falls through. */
    if (korb_vm->kernel_module) {
        korb_class_add_method_cfunc(korb_vm->kernel_module, korb_intern("binding"),
                                     kernel_binding_cfunc, 0);
        struct korb_method *km = korb_class_find_method(korb_vm->kernel_module, korb_intern("binding"));
        if (km) km->visibility = KORB_VIS_PRIVATE;
    }

    /* Method class — instances are returned by Object#method */
    {
        struct korb_class *cMethod = korb_class_new(korb_intern("Method"), korb_vm->object_class, T_DATA);
        korb_const_set(korb_vm->object_class, korb_intern("Method"), (VALUE)cMethod);
        korb_class_add_method_cfunc(cMethod, korb_intern("call"),     method_call,     -1);
        korb_class_add_method_cfunc(cMethod, korb_intern("[]"),       method_call,     -1);
        korb_class_add_method_cfunc(cMethod, korb_intern("to_proc"),  method_to_proc,   0);
        korb_class_add_method_cfunc(cMethod, korb_intern("arity"),      method_arity,      0);
        korb_class_add_method_cfunc(cMethod, korb_intern("name"),       method_name,       0);
        korb_class_add_method_cfunc(cMethod, korb_intern("receiver"),   method_receiver,   0);
        korb_class_add_method_cfunc(cMethod, korb_intern("owner"),      method_owner,      0);
        korb_class_add_method_cfunc(cMethod, korb_intern("bind"),       method_bind,       1);
        korb_class_add_method_cfunc(cMethod, korb_intern("unbind"),     method_unbind,     0);
        korb_class_add_method_cfunc(cMethod, korb_intern("parameters"),      method_parameters,      0);
        korb_class_add_method_cfunc(cMethod, korb_intern("source_location"), method_source_location, 0);
        korb_vm->method_class = cMethod;
    }
    DEF(cObj, "instance_eval",    obj_instance_eval,       -1);
    DEF(cObj, "instance_exec",    obj_instance_exec,       -1);
    DEF(cMod, "instance_method",  module_instance_method,   1);
    DEF(cMod, "instance_methods", module_instance_methods, -1);
    DEF(cMod, "method_defined?",            module_method_defined_p,           -1);
    DEF(cMod, "public_method_defined?",     module_public_method_defined_p,    -1);
    DEF(cMod, "private_method_defined?",    module_private_method_defined_p,   -1);
    DEF(cMod, "protected_method_defined?",  module_protected_method_defined_p, -1);
    DEF(cMod, "private_instance_methods",   module_private_instance_methods,   -1);
    DEF(cMod, "public_instance_methods",    module_public_instance_methods,    -1);
    DEF(cMod, "protected_instance_methods", module_protected_instance_methods, -1);
    DEF(cMod, "constants",        module_constants,         0);
    DEF(cMod, "class_eval",       module_class_eval,       -1);
    DEF(cMod, "module_eval",      module_class_eval,       -1);
    DEF(cMod, "class_exec",       module_class_exec,       -1);
    DEF(cMod, "module_exec",      module_class_exec,       -1);
    DEF(cMod, "<",                module_lt,                1);
    DEF(cMod, "<=",               module_le,                1);
    DEF(cMod, "<=>",              module_cmp,               1);
    DEF(cMod, ">",                module_gt,                1);
    DEF(cMod, ">=",               module_ge,                1);
    DEF(cCls, "superclass",       class_superclass,         0);

    /* Math module — populated with libm-backed functions and constants. */
    {
        struct korb_class *cMath = korb_module_new(korb_intern("Math"));
        korb_const_set(korb_vm->object_class, korb_intern("Math"), (VALUE)cMath);
        /* Math::DomainError < StandardError — raised by Math.sqrt(-1) etc. */
        VALUE eStd = korb_const_get(korb_vm->object_class, korb_intern("StandardError"));
        struct korb_class *cMathDomainError = korb_class_new(korb_intern("DomainError"),
            (eStd && !SPECIAL_CONST_P(eStd) && BUILTIN_TYPE(eStd) == T_CLASS)
                ? (struct korb_class *)eStd : NULL,
            T_OBJECT);
        korb_const_set(cMath, korb_intern("DomainError"), (VALUE)cMathDomainError);
        struct korb_class *cMathMeta = korb_class_new(korb_intern("MathMeta"),
                                                      korb_vm->module_class, T_MODULE);
        korb_const_set(cMath, korb_intern("PI"), korb_float_new(3.141592653589793));
        korb_const_set(cMath, korb_intern("E"),  korb_float_new(2.718281828459045));
        /* Math.fn(...) calls — install on the metaclass so the lookup
         * for `Math.sqrt(2)` (recv = Math) finds them. */
        DEF(cMathMeta, "sqrt",  math_sqrt,  1);
        DEF(cMathMeta, "sin",   math_sin,   1);
        DEF(cMathMeta, "cos",   math_cos,   1);
        DEF(cMathMeta, "tan",   math_tan,   1);
        DEF(cMathMeta, "asin",  math_asin,  1);
        DEF(cMathMeta, "acos",  math_acos,  1);
        DEF(cMathMeta, "atan",  math_atan,  1);
        DEF(cMathMeta, "atan2", math_atan2, 2);
        DEF(cMathMeta, "sinh",  math_sinh,  1);
        DEF(cMathMeta, "cosh",  math_cosh,  1);
        DEF(cMathMeta, "tanh",  math_tanh,  1);
        DEF(cMathMeta, "exp",   math_exp,   1);
        DEF(cMathMeta, "log",   math_log,  -1);
        DEF(cMathMeta, "log2",  math_log2,  1);
        DEF(cMathMeta, "log10", math_log10, 1);
        DEF(cMathMeta, "cbrt",  math_cbrt,  1);
        DEF(cMathMeta, "hypot", math_hypot, 2);
        DEF(cMathMeta, "pow",   math_pow,   2);
        cMath->basic.klass = (VALUE)cMathMeta;
    }

    /* Module.new — install on the Module class's singleton so `Module.new {…}` works. */
    {
        struct korb_class *cModMeta = korb_class_new(korb_intern("ModuleMeta"),
                                                     korb_vm->class_class, T_CLASS);
        DEF(cModMeta, "new", module_new_class_func, -1);
        /* Module.nesting — same metaclass. */
        DEF(cModMeta, "nesting", module_class_nesting, 0);
        korb_vm->module_class->basic.klass = (VALUE)cModMeta;
    }

    /* Exception methods — apply to Exception itself + every subclass. */
    {
        VALUE eExc = korb_const_get(korb_vm->object_class, korb_intern("Exception"));
        if (eExc && !SPECIAL_CONST_P(eExc) &&
            (BUILTIN_TYPE(eExc) == T_CLASS || BUILTIN_TYPE(eExc) == T_MODULE)) {
            struct korb_class *cExc = (struct korb_class *)eExc;
            DEF(cExc, "initialize", exc_initialize, -1);
            DEF(cExc, "message",   exc_message,   0);
            DEF(cExc, "to_s",      exc_to_s,      0);
            DEF(cExc, "inspect",   exc_inspect,   0);
            DEF(cExc, "backtrace", exc_backtrace, 0);
            DEF(cExc, "set_backtrace", exc_set_backtrace, 1);
            DEF(cExc, "backtrace_locations", exc_backtrace_locations, 0);
            DEF(cExc, "cause",     exc_cause,     0);
            DEF(cExc, "full_message", exc_full_message, -1);
            DEF(cExc, "detailed_message", exc_detailed_message, -1);
            DEF(cExc, "exception", exc_exception, -1);
        }
        VALUE eNme = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
        if (eNme && !SPECIAL_CONST_P(eNme) &&
            (BUILTIN_TYPE(eNme) == T_CLASS || BUILTIN_TYPE(eNme) == T_MODULE)) {
            struct korb_class *cNme = (struct korb_class *)eNme;
            DEF(cNme, "receiver", nme_receiver, 0);
            DEF(cNme, "name",     nme_name,     0);
        }
        VALUE eName = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        if (eName && !SPECIAL_CONST_P(eName) &&
            (BUILTIN_TYPE(eName) == T_CLASS || BUILTIN_TYPE(eName) == T_MODULE)) {
            struct korb_class *cName = (struct korb_class *)eName;
            DEF(cName, "name", nme_name, 0);
        }
        VALUE eSE = korb_const_get(korb_vm->object_class, korb_intern("SystemExit"));
        if (eSE && !SPECIAL_CONST_P(eSE) &&
            (BUILTIN_TYPE(eSE) == T_CLASS || BUILTIN_TYPE(eSE) == T_MODULE)) {
            struct korb_class *cSE = (struct korb_class *)eSE;
            DEF(cSE, "status",   syx_status,    0);
            DEF(cSE, "success?", syx_success_p, 0);
        }
    }

    /* Make sure ARGV is at least an empty array; main.c will override */
    korb_const_set(korb_vm->object_class, korb_intern("ARGV"), korb_ary_new());
    /* ENV: populate from real environment (read-only snapshot). */
    {
        extern char **environ;
        VALUE env = korb_hash_new();
        for (char **p = environ; *p; p++) {
            const char *eq = strchr(*p, '=');
            if (!eq) continue;
            VALUE key = korb_str_new(*p, (size_t)(eq - *p));
            VALUE val = korb_str_new_cstr(eq + 1);
            korb_hash_aset(env, key, val);
        }
        korb_const_set(korb_vm->object_class, korb_intern("ENV"), env);
    }

    /* CRuby treats these as module functions — private instance method
     * on Object/Kernel + public class method on Kernel.singleton_class.
     * Done here at the end so all DEFs have run. */
    {
        struct korb_class *cObj2 = korb_vm->object_class;
        struct korb_class *cKerMeta2 = korb_vm->kernel_module
            ? korb_singleton_class_of(korb_vm->kernel_module) : NULL;
        const char *names[] = {
            "abort", "exec", "system", "exit", "exit!",
            "load", "gets", "puts", "p", "pp",
            "open", "trap", "`", "throw", "catch",
            "at_exit", "caller", "caller_locations",
            "loop", "sleep", "proc", "lambda",
            "binding", "block_given?", "fork", "spawn",
            "require", "require_relative", "warn",
            NULL
        };
        for (int i = 0; names[i]; i++) {
            ID nm = korb_intern(names[i]);
            struct korb_method *m = korb_class_find_method(cObj2, nm);
            if (m) {
                if (cKerMeta2 && !korb_class_find_method(cKerMeta2, nm) &&
                    m->type == KORB_METHOD_CFUNC) {
                    korb_class_add_method_cfunc(cKerMeta2, nm,
                                                 m->u.cfunc.func,
                                                 m->u.cfunc.argc);
                }
                /* Copy as PRIVATE to Kernel module so the spec's
                 * Kernel.private_method_defined?(:exec) returns true. */
                if (korb_vm->kernel_module &&
                    !korb_class_find_method(korb_vm->kernel_module, nm) &&
                    m->type == KORB_METHOD_CFUNC) {
                    korb_class_add_method_cfunc(korb_vm->kernel_module, nm,
                                                 m->u.cfunc.func,
                                                 m->u.cfunc.argc);
                    struct korb_method *m2 = korb_class_find_method(korb_vm->kernel_module, nm);
                    if (m2) m2->visibility = KORB_VIS_PRIVATE;
                }
                m->visibility = KORB_VIS_PRIVATE;
            }
            if (korb_vm->kernel_module) {
                m = korb_class_find_method(korb_vm->kernel_module, nm);
                if (m) m->visibility = KORB_VIS_PRIVATE;
            }
        }
    }
}
