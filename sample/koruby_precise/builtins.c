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
RESULT _allocator_disallowed(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
    const char *cn = (!SPECIAL_CONST_P(self) &&
                      (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE))
        ? korb_id_name(((struct korb_class *)self)->name) : "?";
    return korb_raise(c, (struct korb_class *)eT,
               "allocator undefined for %s", cn);
}
RESULT _new_disallowed(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE eN = korb_const_get(KORB_VM(c)->object_class, korb_intern("NoMethodError"));
    const char *cn = (!SPECIAL_CONST_P(self) &&
                      (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE))
        ? korb_id_name(((struct korb_class *)self)->name) : "?";
    return korb_raise(c, (struct korb_class *)eN,
               "undefined method 'new' for %s", cn);
}

#define DEF(klass, name, fn, argc) \
    korb_class_add_method_cfunc((klass), korb_intern(name), (fn), (argc))
/* DEF_R — register a method using the new sp-based RESULT-returning ABI.
 * Phase 2-4 transition; will become the default once the sweep finishes. */
#define DEF_R(klass, name, fn, argc) \
    korb_class_add_method_cfunc_r((klass), korb_intern(name), (fn), (argc))
#define DEF_PRIV(klass, name, fn, argc) do {                              \
    korb_class_add_method_cfunc((klass), korb_intern(name), (fn), (argc)); \
    struct korb_method *_m = korb_class_find_method((klass), korb_intern(name)); \
    if (_m) _m->visibility = KORB_VIS_PRIVATE;                            \
} while (0)
#define DEF_R_PRIV(klass, name, fn, argc) do {                            \
    korb_class_add_method_cfunc_r((klass), korb_intern(name), (fn), (argc)); \
    struct korb_method *_m = korb_class_find_method((klass), korb_intern(name)); \
    if (_m) _m->visibility = KORB_VIS_PRIVATE;                            \
} while (0)

void korb_init_builtins(CTX *c) {
    (void)c;  /* used by sp-passing alloc helpers below */
    /* BasicObject methods — must come first since CRuby's BasicObject
     * has its own __id__, __send__, ==, !=, !, equal?, instance_eval,
     * instance_exec.  Without these on cBasic itself, `Class.new(BasicObject)`
     * subclasses get nothing. */
    {
        struct korb_class *cBasic = (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("BasicObject"));
        if (cBasic) {
            extern RESULT kernel_object_id(CTX *c, int argc, VALUE *sp);
            extern RESULT kernel_eq(CTX *c, int argc, VALUE *sp);
            extern RESULT kernel_neq(CTX *c, int argc, VALUE *sp);
            extern RESULT kernel_not(CTX *c, int argc, VALUE *sp);
            DEF_R(cBasic, "__id__", kernel_object_id, 0);
            DEF_R(cBasic, "__send__", obj_send, -1);
            DEF_R(cBasic, "==", kernel_eq, 1);
            DEF_R(cBasic, "!=", kernel_neq, 1);
            DEF_R(cBasic, "!", kernel_not, 0);
            DEF_R(cBasic, "equal?", kernel_eq, 1);
            DEF_R(cBasic, "instance_eval", obj_instance_eval, -1);
            DEF_R(cBasic, "instance_exec", obj_instance_exec, -1);
        }
    }
    /* Object methods */
    /* Module functions: private instance method on Object PLUS public
     * module method on Kernel.metaclass.  Makes Kernel.puts etc work.
     *
     * korb_singleton_class_of creates (or returns existing) the singleton
     * metaclass and installs it as kernel_module->basic.klass.  After
     * that, every read via the cKerMeta macro picks up the current addr
     * (visit_roots keeps kernel_module up-to-date; visit_class_edges via
     * scan_edges updates basic.klass).  Avoids the stale-C-local pattern
     * where allocs further down the init would move the singleton class
     * but the local cKerMeta would still hold the pre-GC address. */
    if (KORB_VM(c)->kernel_module) {
        (void)korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->kernel_module);
    }
#define cKerMeta ((KORB_VM(c)->kernel_module                                      \
                   ? (struct korb_class *)KORB_VM(c)->kernel_module->basic.klass  \
                   : NULL))
    DEF_R_PRIV(KORB_VM(c)->object_class, "p", kernel_p, -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "puts", kernel_puts, -1);
    if (cKerMeta) {
        DEF_R(cKerMeta, "p", kernel_p, -1);
        DEF_R(cKerMeta, "puts", kernel_puts, -1);
    }
    /* internal helpers used by `**obj` hash splat lowering. */
    {
        RESULT kernel_kwsplat_to_hash(CTX *c, int argc, VALUE *sp);
        RESULT kernel_kwsplat_to_hash_lenient(CTX *c, int argc, VALUE *sp);
        RESULT kernel_to_block_arg(CTX *c, int argc, VALUE *sp);
        RESULT kernel_rescue_splat_match(CTX *c, int argc, VALUE *sp);
        RESULT kernel_case_splat_match(CTX *c, int argc, VALUE *sp);
        RESULT kernel_pattern_decon_check(CTX *c, int argc, VALUE *sp);
        RESULT kernel_pattern_decon_keys_check(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->object_class, "__kwsplat_to_hash", kernel_kwsplat_to_hash, 1);
        DEF_R(KORB_VM(c)->object_class, "__kwsplat_to_hash_lenient", kernel_kwsplat_to_hash_lenient, 1);
        DEF_R(KORB_VM(c)->object_class, "__to_block_arg", kernel_to_block_arg, 1);
        DEF_R(KORB_VM(c)->object_class, "__rescue_splat_match", kernel_rescue_splat_match, 2);
        DEF_R(KORB_VM(c)->object_class, "__rescue_class_check", kernel_rescue_class_check, 1);
        DEF_R(KORB_VM(c)->object_class, "__case_splat_match", kernel_case_splat_match, 2);
        DEF_R(KORB_VM(c)->object_class, "__case_splat_any",   kernel_case_splat_any,   1);
        DEF_R(KORB_VM(c)->object_class, "__pattern_decon_check", kernel_pattern_decon_check, 1);
        DEF_R(KORB_VM(c)->object_class, "__pattern_decon_keys_check", kernel_pattern_decon_keys_check, 1);
    }
    DEF_R_PRIV(KORB_VM(c)->object_class, "print", kernel_print, -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "raise", kernel_raise, -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "fail", kernel_raise, -1);  /* alias */
    /* Also register on Kernel module so `Kernel.private_instance_methods`
     * reports them (CRuby convention).  Module include propagates to
     * Object instances. */
    if (KORB_VM(c)->kernel_module) {
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "print", kernel_print, -1);
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "raise", kernel_raise, -1);
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "fail", kernel_raise, -1);
    }
    if (cKerMeta) {
        DEF_R(cKerMeta, "print", kernel_print, -1);
        DEF_R(cKerMeta, "raise", kernel_raise, -1);
        DEF_R(cKerMeta, "fail", kernel_raise, -1);
    }
    DEF_R(KORB_VM(c)->object_class, "inspect", kernel_inspect, 0);
    DEF_R(KORB_VM(c)->object_class, "to_s", kernel_to_s, 0);
    DEF_R(KORB_VM(c)->object_class, "class", kernel_class, 0);
    DEF_R(KORB_VM(c)->object_class, "==", kernel_eq, 1);
    DEF_R(KORB_VM(c)->object_class, "!=", kernel_neq, 1);
    {
        extern RESULT kernel_not_match(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->object_class, "!~", kernel_not_match, 1);
    }
    DEF_R(KORB_VM(c)->object_class, "!", kernel_not, 0);
    DEF_R(KORB_VM(c)->object_class, "nil?", kernel_nil_p, 0);
    DEF_R(KORB_VM(c)->object_class, "object_id", kernel_object_id, 0);
    DEF_R(KORB_VM(c)->object_class, "__id__", kernel_object_id, 0);
    DEF_R(KORB_VM(c)->object_class, "equal?", kernel_eq, 1);  /* same as == for now */
    DEF_R(KORB_VM(c)->object_class, "freeze", kernel_freeze, 0);
    DEF_R(KORB_VM(c)->object_class, "frozen?", kernel_frozen_p, 0);
    DEF_R(KORB_VM(c)->object_class, "respond_to?", kernel_respond_to_p, 1);
    DEF_R(KORB_VM(c)->object_class, "is_a?", kernel_is_a_p, 1);
    DEF_R(KORB_VM(c)->object_class, "kind_of?", kernel_is_a_p, 1);
    /* Default Object#respond_to_missing? — always returns false.  CRuby
     * has this as a private instance method on Kernel; user classes
     * override it to participate in respond_to? lookup. */
    {
        RESULT _rtm_default(CTX *c, int argc, VALUE *sp) {
            (void)c; (void)argc; (void)sp;
            return RESULT_OK(Qfalse);
        }
        DEF_R_PRIV(KORB_VM(c)->object_class, "respond_to_missing?", _rtm_default, 2);
        if (KORB_VM(c)->kernel_module) {
            DEF_R_PRIV(KORB_VM(c)->kernel_module, "respond_to_missing?", _rtm_default, 2);
        }
    }
    /* Kernel module copies for `Kernel.{public,private}_instance_methods`
     * introspection. */
    if (KORB_VM(c)->kernel_module) {
        DEF_R(KORB_VM(c)->kernel_module, "respond_to?", kernel_respond_to_p, 1);
        DEF_R(KORB_VM(c)->kernel_module, "is_a?", kernel_is_a_p, 1);
        DEF_R(KORB_VM(c)->kernel_module, "kind_of?", kernel_is_a_p, 1);
    }
    DEF_R(KORB_VM(c)->object_class, "methods", obj_methods, -1);
    {
        RESULT obj_public_methods(CTX *c, int argc, VALUE *sp);
        RESULT obj_private_methods(CTX *c, int argc, VALUE *sp);
        RESULT obj_protected_methods(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->object_class, "public_methods",    obj_public_methods,    -1);
        DEF_R(KORB_VM(c)->object_class, "private_methods",   obj_private_methods,   -1);
        DEF_R(KORB_VM(c)->object_class, "protected_methods", obj_protected_methods, -1);
    }
    DEF_R(KORB_VM(c)->object_class, "singleton_methods", obj_singleton_methods, -1);
    {
        RESULT obj_singleton_class(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->object_class, "singleton_class", obj_singleton_class, 0);
    }
    DEF_R(KORB_VM(c)->object_class, "define_singleton_method", obj_define_singleton_method, -1);
    DEF_R(KORB_VM(c)->object_class, "block_given?", kernel_block_given, 0);
    DEF_R_PRIV(KORB_VM(c)->object_class, "throw",        kernel_throw,      -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "catch",        kernel_catch,      -1);
    if (KORB_VM(c)->kernel_module) {
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "throw", kernel_throw, -1);
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "catch", kernel_catch, -1);
        /* module_function semantics — also accessible as Kernel.throw / .catch. */
        if (cKerMeta) {
            DEF_R(cKerMeta, "throw", kernel_throw, -1);
            DEF_R(cKerMeta, "catch", kernel_catch, -1);
        }
    }
    DEF_R_PRIV(KORB_VM(c)->object_class, "require_relative", kernel_require_relative, 1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "require", kernel_require, 1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "__dir__", kernel_dir, 0);
    DEF_R_PRIV(KORB_VM(c)->object_class, "load", kernel_load, -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "abort", kernel_abort, -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "exit", kernel_exit, -1);
    {
        RESULT kernel_exit_bang(CTX *c, int argc, VALUE *sp);
        RESULT kernel_abort(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->object_class, "exit!", kernel_exit_bang, -1);
        DEF_R(KORB_VM(c)->object_class, "abort", kernel_abort,     -1);
    }
    DEF_R(KORB_VM(c)->object_class, "sleep", kernel_sleep, -1);
    DEF_R(KORB_VM(c)->object_class, "at_exit", kernel_at_exit, 0);
    DEF_R(KORB_VM(c)->object_class, "rand",    kernel_rand,   -1);
    DEF_R(KORB_VM(c)->object_class, "srand",   kernel_srand,  -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "Integer", kernel_integer, -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "Float",   kernel_float,    1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "String",  kernel_string,   1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "Array",   kernel_array,    1);
    /* Spec checks Kernel.private_instance_methods — also register there. */
    if (KORB_VM(c)->kernel_module) {
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "Integer", kernel_integer, -1);
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "Float",   kernel_float,    1);
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "String",  kernel_string,   1);
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "Array",   kernel_array,    1);
        /* module_function semantics: also accessible as public Kernel.X via
         * the singleton class.  CRuby uses module_function for these. */
        if (cKerMeta) {
            DEF_R(cKerMeta, "Integer", kernel_integer, -1);
            DEF_R(cKerMeta, "Float",   kernel_float,    1);
            DEF_R(cKerMeta, "String",  kernel_string,   1);
            DEF_R(cKerMeta, "Array",   kernel_array,    1);
        }
    }

    /* Integer */
    DEF_R(KORB_VM(c)->integer_class, "+", int_plus, 1);
    DEF_R(KORB_VM(c)->integer_class, "-", int_minus, 1);
    DEF_R(KORB_VM(c)->integer_class, "*", int_mul, 1);
    DEF_R(KORB_VM(c)->integer_class, "/", int_div, 1);
    DEF_R(KORB_VM(c)->integer_class, "%", int_mod, 1);
    DEF_R(KORB_VM(c)->integer_class, "<<", int_lshift, 1);
    DEF_R(KORB_VM(c)->integer_class, ">>", int_rshift, 1);
    DEF_R(KORB_VM(c)->integer_class, "&", int_and, 1);
    DEF_R(KORB_VM(c)->integer_class, "|", int_or, 1);
    DEF_R(KORB_VM(c)->integer_class, "^", int_xor, 1);
    DEF_R(KORB_VM(c)->integer_class, "<", int_lt, 1);
    DEF_R(KORB_VM(c)->integer_class, "<=", int_le, 1);
    DEF_R(KORB_VM(c)->integer_class, ">", int_gt, 1);
    DEF_R(KORB_VM(c)->integer_class, ">=", int_ge, 1);
    DEF_R(KORB_VM(c)->integer_class, "==", int_eq, 1);
    DEF_R(KORB_VM(c)->integer_class, "<=>", int_cmp, 1);
    DEF_R(KORB_VM(c)->integer_class, "-@", int_uminus, 0);
    DEF_R(KORB_VM(c)->integer_class, "+@", int_uplus,  0);
    DEF_R(KORB_VM(c)->integer_class, "to_s", int_to_s, -1);
    DEF_R(KORB_VM(c)->integer_class, "to_i", int_to_i, 0);
    DEF_R(KORB_VM(c)->integer_class, "to_int", int_to_i, 0);
    DEF_R(KORB_VM(c)->integer_class, "to_f", int_to_f, 0);
    DEF_R(KORB_VM(c)->integer_class, "zero?", int_zero_p, 0);
    DEF_R(KORB_VM(c)->integer_class, "even?", int_even_p, 0);
    DEF_R(KORB_VM(c)->integer_class, "odd?",  int_odd_p,  0);
    DEF_R(KORB_VM(c)->integer_class, "positive?", int_positive_p, 0);
    DEF_R(KORB_VM(c)->integer_class, "negative?", int_negative_p, 0);
    DEF_R(KORB_VM(c)->integer_class, "times", int_times, 0);
    DEF_R(KORB_VM(c)->integer_class, "succ", int_succ, 0);
    DEF_R(KORB_VM(c)->integer_class, "next", int_succ, 0);
    DEF_R(KORB_VM(c)->integer_class, "pred", int_pred, 0);

    /* Helpers used to forbid `.allocate` / `.new` on classes whose
     * instances are immediates (Float, Symbol, NilClass, TrueClass,
     * FalseClass).  CRuby raises TypeError from .allocate and
     * NoMethodError from .new.  `_allocator_disallowed` and
     * `_new_disallowed` are defined above. */
    /* Float */
    {
        struct korb_class *cFltMeta = korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->float_class);
        DEF_R(cFltMeta, "allocate", _allocator_disallowed, -1);
        DEF_R(cFltMeta, "new",      _new_disallowed,       -1);
    }
    korb_const_set(KORB_VM(c)->float_class, korb_intern("INFINITY"), korb_float_new(c, c->sp_top, 1.0/0.0));
    korb_const_set(KORB_VM(c)->float_class, korb_intern("NAN"),      korb_float_new(c, c->sp_top, 0.0/0.0));
    korb_const_set(KORB_VM(c)->float_class, korb_intern("MAX"),      korb_float_new(c, c->sp_top, 1.7976931348623157e+308));
    korb_const_set(KORB_VM(c)->float_class, korb_intern("MIN"),      korb_float_new(c, c->sp_top, 2.2250738585072014e-308));
    korb_const_set(KORB_VM(c)->float_class, korb_intern("EPSILON"),  korb_float_new(c, c->sp_top, 2.220446049250313e-16));
    DEF_R(KORB_VM(c)->float_class, "+", flt_plus, 1);
    DEF_R(KORB_VM(c)->float_class, "-", flt_minus, 1);
    DEF_R(KORB_VM(c)->float_class, "*", flt_mul, 1);
    DEF_R(KORB_VM(c)->float_class, "%", flt_mod, 1);
    DEF_R(KORB_VM(c)->float_class, "modulo", flt_mod, 1);
    DEF_R(KORB_VM(c)->float_class, "divmod", flt_divmod, 1);
    DEF_R(KORB_VM(c)->float_class, "/", flt_div, 1);
    /* Float#fdiv / quo — both behave like /. */
    DEF_R(KORB_VM(c)->float_class, "fdiv", flt_div, 1);
    DEF_R(KORB_VM(c)->float_class, "quo",  flt_div, 1);
    DEF_R(KORB_VM(c)->float_class, "to_s", flt_to_s, 0);
    extern RESULT flt_to_i(CTX *c, int argc, VALUE *sp);
    DEF_R(KORB_VM(c)->float_class, "to_i",   flt_to_i, 0);
    DEF_R(KORB_VM(c)->float_class, "to_int", flt_to_i, 0);
    DEF_R(KORB_VM(c)->float_class, "step", flt_step, -1);
    DEF_R(KORB_VM(c)->float_class, "nan?",      flt_nan_p,      0);
    DEF_R(KORB_VM(c)->float_class, "infinite?", flt_infinite_p, 0);
    DEF_R(KORB_VM(c)->float_class, "finite?",   flt_finite_p,   0);
    DEF_R(KORB_VM(c)->float_class, "zero?",     flt_zero_p,     0);
    DEF_R(KORB_VM(c)->float_class, "positive?", flt_positive_p, 0);
    DEF_R(KORB_VM(c)->float_class, "negative?", flt_negative_p, 0);
    /* Float#next_float / prev_float — IEEE 754 successor / predecessor. */
    {
        #include <math.h>
        RESULT _flt_next(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            return RESULT_OK(korb_float_new(c, c->sp_top, nextafter(korb_num2dbl(self), 1.0/0.0)));
        }
        RESULT _flt_prev(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            return RESULT_OK(korb_float_new(c, c->sp_top, nextafter(korb_num2dbl(self), -1.0/0.0)));
        }
        DEF_R(KORB_VM(c)->float_class, "next_float", _flt_next, 0);
        DEF_R(KORB_VM(c)->float_class, "prev_float", _flt_prev, 0);
    }

    /* String */
    /* String#encoding stub — return Encoding::UTF_8.  */
    {
        RESULT _str_encoding(CTX *c, int argc, VALUE *sp);
        RESULT _str_force_encoding(CTX *c, int argc, VALUE *sp);
        RESULT _str_b(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->string_class, "encoding",       _str_encoding, 0);
        DEF_R(KORB_VM(c)->string_class, "force_encoding", _str_force_encoding, -1);
        DEF_R(KORB_VM(c)->string_class, "encode",         _str_force_encoding, -1);
        DEF_R(KORB_VM(c)->string_class, "encode!",        _str_force_encoding, -1);
        DEF_R(KORB_VM(c)->string_class, "b",              _str_b, 0);
        /* valid_encoding? / ascii_only? / unicode_normalized? — koruby is
         * UTF-8 only and we don't track invalid byte sequences, so always
         * return true for valid_encoding?.  ascii_only? checks bytes. */
        {
            RESULT _valid_encoding_p(CTX *c, int argc, VALUE *sp) {
                c->sp_top = sp;
                return RESULT_OK(Qtrue);
            }
            RESULT _unicode_normalized_p(CTX *c, int argc, VALUE *sp) {
                c->sp_top = sp;
                return RESULT_OK(Qtrue);
            }
            RESULT _ascii_only_p(CTX *c, int argc, VALUE *sp) {
                c->sp_top = sp;
                VALUE self = sp[-argc - 1];
                if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_STRING) return RESULT_OK(Qfalse);
                struct korb_string *s = (struct korb_string *)self;
                for (long i = 0; i < s->len; i++) {
                    if ((unsigned char)s->ptr[i] >= 0x80) return RESULT_OK(Qfalse);
                }
                return RESULT_OK(Qtrue);
            }
            DEF_R(KORB_VM(c)->string_class, "valid_encoding?",      _valid_encoding_p,      0);
            DEF_R(KORB_VM(c)->string_class, "unicode_normalized?",  _unicode_normalized_p, -1);
            DEF_R(KORB_VM(c)->string_class, "ascii_only?",          _ascii_only_p,          0);
            /* scrub / scrub!: no encoding-error tracking, so just return self. */
            RESULT _str_scrub(CTX *c, int argc, VALUE *sp) {
                c->sp_top = sp;
                VALUE self = sp[-argc - 1];
                return RESULT_OK(self);
            }
            DEF_R(KORB_VM(c)->string_class, "scrub", _str_scrub, -1);
            DEF_R(KORB_VM(c)->string_class, "scrub!", _str_scrub, -1);
        }
    }
    /* Encoding#name / #to_s. */
    {
        VALUE cEnc_v = korb_const_get(KORB_VM(c)->object_class, korb_intern("Encoding"));
        if (!UNDEF_P(cEnc_v) && !SPECIAL_CONST_P(cEnc_v) &&
            BUILTIN_TYPE(cEnc_v) == T_CLASS) {
            struct korb_class *cEnc = (struct korb_class *)cEnc_v;
            RESULT _enc_name(CTX *c, int argc, VALUE *sp);
            RESULT _enc_default_external(CTX *c, int argc, VALUE *sp);
            RESULT _enc_default_internal(CTX *c, int argc, VALUE *sp);
            RESULT _enc_find(CTX *c, int argc, VALUE *sp);
            DEF_R(cEnc, "name",    _enc_name, 0);
            DEF_R(cEnc, "to_s",    _enc_name, 0);
            DEF_R(cEnc, "inspect", _enc_name, 0);
            /* Class methods */
            struct korb_class *cEncMeta = (struct korb_class *)cEnc->basic.klass;
            if (cEncMeta) {
                DEF_R(cEncMeta, "default_external",  _enc_default_external, 0);
                DEF_R(cEncMeta, "default_internal",  _enc_default_internal, 0);
                DEF_R(cEncMeta, "find",              _enc_find, 1);
                /* Setters are no-ops (koruby uses UTF-8 internally). */
                RESULT _enc_setter_noop(CTX *c, int argc, VALUE *sp) {
                    c->sp_top = sp;
                    VALUE *argv = sp - argc;
                    return RESULT_OK(argc > 0 ? argv[0] : Qnil);
                }
                DEF_R(cEncMeta, "default_external=", _enc_setter_noop, 1);
                DEF_R(cEncMeta, "default_internal=", _enc_setter_noop, 1);
            }
        }
    }
    DEF_R(KORB_VM(c)->string_class, "+", str_plus, 1);
    DEF_R(KORB_VM(c)->string_class, "<<", str_lshift, -1);
    DEF_R(KORB_VM(c)->string_class, "concat", str_concat, 1);
    DEF_R(KORB_VM(c)->string_class, "size", str_size, 0);
    DEF_R(KORB_VM(c)->string_class, "length", str_size, 0);
    DEF_R(KORB_VM(c)->string_class, "==", str_eq, 1);
    DEF_R(KORB_VM(c)->string_class, "<=>", str_cmp, 1);
    DEF_R(KORB_VM(c)->string_class, "<",   str_lt, 1);
    DEF_R(KORB_VM(c)->string_class, "<=",  str_le, 1);
    DEF_R(KORB_VM(c)->string_class, ">",   str_gt, 1);
    DEF_R(KORB_VM(c)->string_class, ">=",  str_ge, 1);
    DEF_R(KORB_VM(c)->string_class, "to_s", str_to_s, 0);
    DEF_R(KORB_VM(c)->string_class, "to_str", str_to_s, 0);
    DEF_R(KORB_VM(c)->string_class, "to_sym", str_to_sym, 0);
    DEF_R(KORB_VM(c)->string_class, "__chilled?", str_chilled_p, 0);

    /* Array */
    DEF_R(KORB_VM(c)->array_class, "size", ary_size, 0);
    DEF_R(KORB_VM(c)->array_class, "length", ary_size, 0);
    DEF_R(KORB_VM(c)->array_class, "[]", ary_aref, -1);
    DEF_R(KORB_VM(c)->array_class, "[]=", ary_aset, -1);
    DEF_R(KORB_VM(c)->array_class, "push", ary_push, -1);
    DEF_R(KORB_VM(c)->array_class, "append", ary_push, -1);
    DEF_R(KORB_VM(c)->array_class, "<<", ary_lshift, 1);
    DEF_R(KORB_VM(c)->array_class, "pop", ary_pop, -1);
    DEF_R(KORB_VM(c)->array_class, "first", ary_first_n, -1);
    DEF_R(KORB_VM(c)->array_class, "last",  ary_last_n,  -1);
    DEF_R(KORB_VM(c)->array_class, "each", ary_each, 0);
    DEF_R(KORB_VM(c)->array_class, "each_with_index", ary_each_with_index, 0);
    DEF_R(KORB_VM(c)->array_class, "map", ary_map, 0);
    DEF_R(KORB_VM(c)->array_class, "collect", ary_map, 0);
    DEF_R(KORB_VM(c)->array_class, "select", ary_select, 0);
    DEF_R(KORB_VM(c)->array_class, "filter", ary_select, 0);
    DEF_R(KORB_VM(c)->array_class, "reduce", ary_reduce, -1);
    DEF_R(KORB_VM(c)->array_class, "inject", ary_reduce, -1);
    DEF_R(KORB_VM(c)->array_class, "join", ary_join, -1);
    DEF_R(KORB_VM(c)->array_class, "inspect", ary_inspect, 0);
    DEF_R(KORB_VM(c)->array_class, "to_s", ary_inspect, 0);
    DEF_R(KORB_VM(c)->array_class, "==", ary_eq, 1);   /* Phase 3 PoC: new sp/RESULT ABI */
    DEF_R(KORB_VM(c)->array_class, "dup", ary_dup, 0);
    DEF_R(KORB_VM(c)->array_class, "to_h", ary_to_h, 0);

    /* Hash */
    DEF_R(KORB_VM(c)->hash_class, "[]", hash_aref, 1);
    DEF_R(KORB_VM(c)->hash_class, "[]=", hash_aset, 2);
    DEF_R(KORB_VM(c)->hash_class, "size", hash_size, 0);
    DEF_R(KORB_VM(c)->hash_class, "length", hash_size, 0);
    DEF_R(KORB_VM(c)->hash_class, "each", hash_each, 0);

    /* Range */
    {
        /* Class method Range.new — register on Range's metaclass. */
        struct korb_class *cRngMeta = korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->range_class);
        korb_class_add_method_cfunc_r(cRngMeta, korb_intern("new"), rng_class_new, -1);
    }
    DEF_R(KORB_VM(c)->range_class, "each", rng_each, 0);
    DEF_R(KORB_VM(c)->range_class, "first", rng_first, -1);
    DEF_R(KORB_VM(c)->range_class, "last",  rng_last,  -1);
    DEF_R(KORB_VM(c)->range_class, "begin", rng_begin, 0);
    DEF_R(KORB_VM(c)->range_class, "min",   rng_min,   -1);
    DEF_R(KORB_VM(c)->range_class, "end",   rng_end,    0);
    DEF_R(KORB_VM(c)->range_class, "max",   rng_max,   -1);
    DEF_R(KORB_VM(c)->range_class, "to_a", rng_to_a, 0);
    {
        RESULT rng_hash(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->range_class, "hash", rng_hash, 0);
    }

    /* Class */
    DEF_R(KORB_VM(c)->class_class, "new", class_new, -1);
    {
        RESULT class_allocate(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->class_class, "allocate", class_allocate, 0);
    }
    DEF_R(KORB_VM(c)->class_class, "name", class_name, 0);

    /* Module — applies to both Class and Module */
    DEF_R(KORB_VM(c)->module_class, "name", class_name, 0);
    DEF_R(KORB_VM(c)->module_class, "attr_reader",   module_attr_reader,   -1);
    /* Module#attr — historical:
     *   attr :foo            → reader only
     *   attr :foo, true      → reader + writer (deprecated form)
     *   attr :foo, :bar, ... → readers for each (when args are all symbols) */
    {
        RESULT _module_attr(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            RESULT module_attr_reader(CTX *, int, VALUE *);
            RESULT module_attr_writer(CTX *, int, VALUE *);
            if (argc == 2 && (argv[1] == Qtrue || argv[1] == Qfalse)) {
                bool writable = (argv[1] == Qtrue);
                /* call module_attr_reader with [self, argv[0]] staged */
                sp[0] = self;
                sp[1] = argv[0];
                VALUE r = UNWRAP(module_attr_reader(c, 1, sp + 2));
                if (writable) {
                    /* module_attr_reader is a GC point; the self / argv[0]
                     * C-locals are stale.  Re-read from the (scanned) receiver
                     * and arg slots before re-staging (IDIOM A). */
                    sp[0] = sp[-argc - 1];
                    sp[1] = (sp - argc)[0];
                    VALUE w = UNWRAP(module_attr_writer(c, 1, sp + 2));
                    /* CRuby returns [reader_sym, writer_sym] in this form. */
                    /* Park r, w, and the result array across the pushes
                     * (push can grow/move the result handle). */
                    sp[0] = r; sp[1] = w;
                    sp[2] = korb_ary_new_capa(c, sp + 3, 2);
                    if (BUILTIN_TYPE(sp[0]) == T_ARRAY && ((struct korb_array *)sp[0])->len > 0) {
                        korb_ary_push(c, sp + 3, sp[2], korb_ary_items((struct korb_array *)sp[0])[0]);
                    }
                    if (BUILTIN_TYPE(sp[1]) == T_ARRAY && ((struct korb_array *)sp[1])->len > 0) {
                        korb_ary_push(c, sp + 3, sp[2], korb_ary_items((struct korb_array *)sp[1])[0]);
                    }
                    return RESULT_OK(sp[2]);
                }
                return RESULT_OK(r);
            }
            return module_attr_reader(c, argc, sp);
        }
        DEF_R(KORB_VM(c)->module_class, "attr", _module_attr, -1);
    }
    DEF_R(KORB_VM(c)->module_class, "attr_writer",   module_attr_writer,   -1);
    DEF_R(KORB_VM(c)->module_class, "attr_accessor", module_attr_accessor, -1);
    DEF_R(KORB_VM(c)->module_class, "include",       module_include,       -1);
    /* Toplevel `include M` — main / Object forwards to Object#include.
     * `extend self` and similar; expose on Object too as a private
     * method so any context (including main / class bodies of plain
     * objects) can call it. */
    DEF_R(KORB_VM(c)->object_class, "include",       module_include,       -1);
    DEF_R(KORB_VM(c)->object_class, "private",       module_private,       -1);
    DEF_R(KORB_VM(c)->object_class, "public",        module_public,        -1);
    DEF_R(KORB_VM(c)->module_class, "private",       module_private,       -1);
    DEF_R(KORB_VM(c)->module_class, "public",        module_public,        -1);
    DEF_R(KORB_VM(c)->module_class, "protected",     module_protected,     -1);
    DEF_R(KORB_VM(c)->module_class, "module_function", module_module_function, -1);
    DEF_R(KORB_VM(c)->module_class, "define_method", module_define_method, -1);
    DEF_R(KORB_VM(c)->module_class, "alias_method",  module_alias_method,  -1);
    DEF_R(KORB_VM(c)->module_class, "undef_method",  module_undef_method,  -1);
    DEF_R(KORB_VM(c)->module_class, "remove_method", module_remove_method, -1);
    DEF_R(KORB_VM(c)->module_class, "const_get",     module_const_get,     -1);
    DEF_R(KORB_VM(c)->module_class, "const_set",     module_const_set,     -1);
    DEF_R(KORB_VM(c)->module_class, "const_defined?", module_const_defined_p, -1);
    DEF_R(KORB_VM(c)->module_class, "remove_const",  module_remove_const,  -1);
    DEF_R(KORB_VM(c)->module_class, "remove_class_variable", module_remove_class_variable, -1);
    {
        RESULT mod_class_variable_get(CTX *c, int argc, VALUE *sp);
        RESULT mod_class_variable_set(CTX *c, int argc, VALUE *sp);
        RESULT mod_class_variable_defined_p(CTX *c, int argc, VALUE *sp);
        RESULT mod_class_variables(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->module_class, "private_class_method",   module_private_class_method, -1);
    DEF_R(KORB_VM(c)->module_class, "public_class_method",    module_public_class_method,  -1);
    DEF_R(KORB_VM(c)->module_class, "private_constant",       module_private_constant,     -1);
    DEF_R(KORB_VM(c)->module_class, "public_constant",        module_public_constant,      -1);
    DEF_R(KORB_VM(c)->module_class, "class_variable_get",     mod_class_variable_get,     -1);
        DEF_R(KORB_VM(c)->module_class, "class_variable_set",     mod_class_variable_set,     -1);
        DEF_R(KORB_VM(c)->module_class, "class_variable_defined?", mod_class_variable_defined_p, -1);
        DEF_R(KORB_VM(c)->module_class, "class_variables",        mod_class_variables,        -1);
    }
    DEF_R(KORB_VM(c)->module_class, "===",           class_eqq,            1);
    /* Class < Module — Class instances inherit Module's methods.
     * No need to mirror module_* onto KORB_VM(c)->class_class. */

    /* Comparable instance methods */
    DEF_R(KORB_VM(c)->comparable_module, "<",          cmp_lt,       1);
    DEF_R(KORB_VM(c)->comparable_module, "<=",         cmp_le,       1);
    DEF_R(KORB_VM(c)->comparable_module, ">",          cmp_gt,       1);
    DEF_R(KORB_VM(c)->comparable_module, ">=",         cmp_ge,       1);
    DEF_R(KORB_VM(c)->comparable_module, "==",         cmp_eq,       1);
    DEF_R(KORB_VM(c)->comparable_module, "between?",   cmp_between, -1);
    DEF_R(KORB_VM(c)->comparable_module, "clamp",      cmp_clamp,   -1);

    /* extra Object methods */
    /* Object dup / clone / instance_variables */
    DEF_R(KORB_VM(c)->object_class, "dup",                obj_dup,                   0);
    DEF_R(KORB_VM(c)->object_class, "clone",              obj_clone,                 0);
    DEF_R(KORB_VM(c)->object_class, "instance_variables", obj_instance_variables,    0);
    DEF_R(KORB_VM(c)->object_class, "instance_variable_defined?", obj_ivar_defined_p, 1);
    DEF_R(KORB_VM(c)->object_class, "remove_instance_variable", obj_remove_instance_variable, 1);
    /* Kernel#__method__, caller, eval, loop, lambda, proc */
    DEF_R(KORB_VM(c)->object_class, "__method__",         kernel_method_name,        0);
    DEF_R(KORB_VM(c)->object_class, "__callee__",         kernel_method_name,        0);
    DEF_R_PRIV(KORB_VM(c)->object_class, "global_variables",   kernel_global_variables,   0);
    if (KORB_VM(c)->kernel_module) {
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "global_variables", kernel_global_variables, 0);
        /* module_function semantics: also expose as a class method on
         * Kernel so `Kernel.global_variables` works (CRuby compat). */
        struct korb_class *kKerMeta = korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->kernel_module);
        DEF_R(kKerMeta, "global_variables", kernel_global_variables, 0);
    }
    DEF_R(KORB_VM(c)->object_class, "caller",             kernel_caller,            -1);
    DEF_R(KORB_VM(c)->object_class, "__capture_lvars__",  kernel_capture_lvars,      0);
    DEF_R(KORB_VM(c)->object_class, "local_variables",    kernel_local_variables,    0);
    DEF_R(KORB_VM(c)->object_class, "eval",               kernel_eval_stub,         -1);
    DEF_R(KORB_VM(c)->object_class, "loop",               kernel_loop,               0);
    DEF_R(KORB_VM(c)->object_class, "initialize",         kernel_initialize_default, -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "lambda",        kernel_lambda,             0);
    DEF_R_PRIV(KORB_VM(c)->object_class, "proc",          kernel_proc,               0);
    /* Mirror to Kernel module so Kernel.private_method_defined? sees them. */
    if (KORB_VM(c)->kernel_module) {
        struct korb_class *kmod = KORB_VM(c)->kernel_module;
        korb_class_add_method_cfunc_r(kmod, korb_intern("lambda"), kernel_lambda, 0);
        korb_class_add_method_cfunc_r(kmod, korb_intern("proc"),   kernel_proc,   0);
        korb_class_add_method_cfunc_r(kmod, korb_intern("eval"),   kernel_eval_stub, -1);
        struct korb_method *m;
        if ((m = korb_class_find_method(kmod, korb_intern("lambda")))) m->visibility = KORB_VIS_PRIVATE;
        if ((m = korb_class_find_method(kmod, korb_intern("proc"))))   m->visibility = KORB_VIS_PRIVATE;
        if ((m = korb_class_find_method(kmod, korb_intern("eval"))))   m->visibility = KORB_VIS_PRIVATE;
    }
    /* Range#exclude_end? */
    DEF_R(KORB_VM(c)->range_class, "exclude_end?",       rng_exclude_end_p,         0);
    /* Class ancestors / Module#prepend */
    DEF_R(KORB_VM(c)->module_class, "ancestors",          class_ancestors,           0);
    DEF_R(KORB_VM(c)->module_class, "prepend",            module_prepend,           -1);
    {
        RESULT module_include_p(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            if (argc < 1) return RESULT_OK(Qfalse);
            if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qfalse);
            if (SPECIAL_CONST_P(argv[0]) ||
                (BUILTIN_TYPE(argv[0]) != T_MODULE && BUILTIN_TYPE(argv[0]) != T_CLASS)) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "wrong argument type %s (expected Module)",
                           SPECIAL_CONST_P(argv[0]) ? "?" :
                               korb_id_name(korb_class_of_class(argv[0])->name));
            }
            extern bool korb_module_has_ancestor(struct korb_class *, struct korb_class *);
            return RESULT_OK(KORB_BOOL(korb_module_has_ancestor((struct korb_class *)self,
                                                                 (struct korb_class *)argv[0])));
        }
        DEF_R(KORB_VM(c)->module_class, "include?",     module_include_p,         1);
    }
    DEF_R(KORB_VM(c)->object_class, "extend",             obj_extend,               -1);
    DEF_R(KORB_VM(c)->object_class, "send",                  obj_send,                 -1);
    DEF_R(KORB_VM(c)->object_class, "__send__",              obj_send,                 -1);
    DEF_R(KORB_VM(c)->object_class, "public_send",           obj_public_send,          -1);
    DEF_R(KORB_VM(c)->object_class, "instance_variable_get", obj_instance_variable_get, 1);
    DEF_R(KORB_VM(c)->object_class, "instance_variable_set", obj_instance_variable_set, 2);
    DEF_R(KORB_VM(c)->object_class, "method",                obj_method,                1);
    DEF_R(KORB_VM(c)->object_class, "instance_of?",          obj_instance_of_p,         1);
    DEF_R(KORB_VM(c)->object_class, "===",                   obj_eqq,                   1);
    {
        RESULT obj_tap(CTX *c, int argc, VALUE *sp);
        RESULT obj_then(CTX *c, int argc, VALUE *sp);
        RESULT obj_itself(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->object_class, "tap",        obj_tap,    0);
        DEF_R(KORB_VM(c)->object_class, "then",       obj_then,   0);
        DEF_R(KORB_VM(c)->object_class, "yield_self", obj_then,   0);
        DEF_R(KORB_VM(c)->object_class, "itself",     obj_itself, 0);
    }
    DEF_R_PRIV(KORB_VM(c)->object_class, "format",            kernel_format,            -1);
    DEF_R_PRIV(KORB_VM(c)->object_class, "sprintf",           kernel_format,            -1);
    if (KORB_VM(c)->kernel_module) {
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "format",  kernel_format, -1);
        DEF_R_PRIV(KORB_VM(c)->kernel_module, "sprintf", kernel_format, -1);
    }
    /* Also expose as public methods on Kernel.singleton_class so the
     * `Kernel.format(...)` form works. */
    if (cKerMeta) {
        DEF_R(cKerMeta, "format",  kernel_format, -1);
        DEF_R(cKerMeta, "sprintf", kernel_format, -1);
    }
    DEF_R(KORB_VM(c)->object_class, "printf",                kernel_printf,            -1);

    /* extra Integer */
    DEF_R(KORB_VM(c)->integer_class, "chr",   int_chr, 0);
    DEF_R(KORB_VM(c)->integer_class, "===",   int_eqq, 1);
    DEF_R(KORB_VM(c)->integer_class, "floor", int_floor, -1);
    DEF_R(KORB_VM(c)->integer_class, "ceil",  int_floor, -1);
    DEF_R(KORB_VM(c)->integer_class, "round",    int_round, -1);
    DEF_R(KORB_VM(c)->integer_class, "floor",    int_floor, -1);
    DEF_R(KORB_VM(c)->integer_class, "ceil",     int_ceil,  -1);
    DEF_R(KORB_VM(c)->integer_class, "truncate", int_truncate, -1);
    DEF_R(KORB_VM(c)->integer_class, "abs",   int_abs, 0);
    DEF_R(KORB_VM(c)->integer_class, "[]",    int_aref, -1);
    DEF_R(KORB_VM(c)->integer_class, "bit_length", int_bit_length, 0);
    DEF_R(KORB_VM(c)->integer_class, "divmod", int_divmod, 1);
    DEF_R(KORB_VM(c)->integer_class, "**",    int_pow, 1);
    {
        RESULT int_invert(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->integer_class, "~", int_invert, 0);
    }
    DEF_R(KORB_VM(c)->integer_class, "step",  int_step, -1);
    DEF_R(KORB_VM(c)->integer_class, "upto",  int_upto, 1);
    DEF_R(KORB_VM(c)->integer_class, "downto", int_downto, 1);
    DEF_R(KORB_VM(c)->integer_class, "div",   int_method_div, 1);
    DEF_R(KORB_VM(c)->integer_class, "fdiv",       int_fdiv,       1);
    /* Integer#quo — same as fdiv (returns Float).  Rational#quo would
     * return Rational, but koruby doesn't have Rational implementation. */
    DEF_R(KORB_VM(c)->integer_class, "quo",        int_fdiv,       1);
    DEF_R(KORB_VM(c)->integer_class, "remainder",  int_remainder,  1);
    DEF_R(KORB_VM(c)->integer_class, "modulo",     int_mod,        1);
    {
        RESULT int_abs(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->integer_class, "magnitude", int_abs, 0);
    }
    DEF_R(KORB_VM(c)->integer_class, "size",  int_size, 0);
    DEF_R(KORB_VM(c)->integer_class, "coerce", int_coerce, 1);
    DEF_R(KORB_VM(c)->integer_class, "abs2",   int_abs2,   0);
    DEF_R(KORB_VM(c)->float_class, "coerce", flt_coerce, 1);
    DEF_R(KORB_VM(c)->float_class, "abs2",   flt_abs2,   0);

    /* extra Float */
    DEF_R(KORB_VM(c)->float_class, "floor", flt_floor, -1);
    DEF_R(KORB_VM(c)->float_class, "===",   flt_eqq, 1);
    DEF_R(KORB_VM(c)->float_class, "**",    flt_pow, 1);
    DEF_R(KORB_VM(c)->float_class, "<",     flt_lt, 1);
    DEF_R(KORB_VM(c)->float_class, "<=",    flt_le, 1);
    DEF_R(KORB_VM(c)->float_class, ">",     flt_gt, 1);
    DEF_R(KORB_VM(c)->float_class, ">=",    flt_ge, 1);
    DEF_R(KORB_VM(c)->float_class, "<=>",   flt_cmp, 1);
    DEF_R(KORB_VM(c)->float_class, "==",    flt_eqq, 1);
    DEF_R(KORB_VM(c)->float_class, "to_i",  flt_to_i, 0);
    DEF_R(KORB_VM(c)->float_class, "to_f",  flt_to_f, 0);
    DEF_R(KORB_VM(c)->float_class, "-@",    flt_uminus, 0);
    DEF_R(KORB_VM(c)->float_class, "+@",    flt_uplus, 0);
    DEF_R(KORB_VM(c)->float_class, "abs",   flt_abs, 0);
    DEF_R(KORB_VM(c)->float_class, "magnitude", flt_abs, 0);
    DEF_R(KORB_VM(c)->float_class, "ceil",     flt_ceil,    -1);
    DEF_R(KORB_VM(c)->float_class, "round",    flt_round,   -1);
    DEF_R(KORB_VM(c)->float_class, "truncate", flt_truncate, 0);

    /* extra String */
    DEF_R(KORB_VM(c)->string_class, "split",       str_split,       -1);
    DEF_R(KORB_VM(c)->string_class, "chomp",       str_chomp,       -1);
    DEF_R(KORB_VM(c)->string_class, "chomp!",      str_chomp_bang,  -1);
    DEF_R(KORB_VM(c)->string_class, "strip",       str_strip,        0);
    DEF_R(KORB_VM(c)->string_class, "strip!",      str_strip_bang,   0);
    DEF_R(KORB_VM(c)->string_class, "lstrip",      str_lstrip,       0);
    DEF_R(KORB_VM(c)->string_class, "lstrip!",     str_lstrip_bang,  0);
    DEF_R(KORB_VM(c)->string_class, "rstrip",      str_rstrip,       0);
    DEF_R(KORB_VM(c)->string_class, "rstrip!",     str_rstrip_bang,  0);
    DEF_R(KORB_VM(c)->string_class, "to_i",        str_to_i,        -1);
    DEF_R(KORB_VM(c)->string_class, "to_f",        str_to_f,         0);
    DEF_R(KORB_VM(c)->string_class, "[]",          str_aref,        -1);
    DEF_R(KORB_VM(c)->string_class, "[]=",         str_aset,        -1);
    DEF_R(KORB_VM(c)->string_class, "index",       str_index,       -1);
    DEF_R(KORB_VM(c)->string_class, "rindex",      str_rindex,      -1);
    DEF_R(KORB_VM(c)->string_class, "chars",       str_chars,        0);
    DEF_R(KORB_VM(c)->string_class, "bytes",       str_bytes,        0);
    DEF_R(KORB_VM(c)->string_class, "each_char",   str_each_char,    0);
    DEF_R(KORB_VM(c)->string_class, "each_line",   str_each_line,    0);
    DEF_R(KORB_VM(c)->string_class, "start_with?", str_start_with,  -1);
    DEF_R(KORB_VM(c)->string_class, "end_with?",   str_end_with,    -1);
    DEF_R(KORB_VM(c)->string_class, "include?",    str_include,     -1);
    DEF_R(KORB_VM(c)->string_class, "replace",     str_replace,      1);
    DEF_R(KORB_VM(c)->string_class, "reverse",     str_reverse,      0);
    DEF_R(KORB_VM(c)->string_class, "reverse!",    str_reverse_bang, 0);
    DEF_R(KORB_VM(c)->string_class, "upcase",      str_upcase,       0);
    DEF_R(KORB_VM(c)->string_class, "upcase!",     str_upcase_bang,  0);
    DEF_R(KORB_VM(c)->string_class, "downcase",    str_downcase,     0);
    DEF_R(KORB_VM(c)->string_class, "downcase!",   str_downcase_bang, 0);
    DEF_R(KORB_VM(c)->string_class, "empty?",      str_empty_p,      0);
    DEF_R(KORB_VM(c)->string_class, "*",           str_mul,          1);
    DEF_R(KORB_VM(c)->string_class, "hash",        str_hash,         0);
    DEF_R(KORB_VM(c)->string_class, "===",         str_eqq,          1);
    DEF_R(KORB_VM(c)->string_class, "gsub",        str_gsub,        -1);
    DEF_R(KORB_VM(c)->string_class, "gsub!",       str_gsub_bang,   -1);
    DEF_R(KORB_VM(c)->string_class, "sub",         str_sub,         -1);
    DEF_R(KORB_VM(c)->string_class, "sub!",        str_sub_bang,    -1);
    DEF_R(KORB_VM(c)->string_class, "tr",          str_tr,          -1);
    DEF_R(KORB_VM(c)->string_class, "tr!",         str_tr_bang,     -1);
    DEF_R(KORB_VM(c)->string_class, "tr_s",        str_tr_s,        -1);
    DEF_R(KORB_VM(c)->string_class, "tr_s!",       str_tr_s_bang,   -1);
    DEF_R(KORB_VM(c)->string_class, "%",           str_percent,     -1);
    DEF_R(KORB_VM(c)->string_class, "bytesize",    str_bytesize,     0);
    DEF_R(KORB_VM(c)->string_class, "inspect",     kernel_inspect,   0);
    DEF_R(KORB_VM(c)->string_class, "dup",         obj_dup,          0);
    DEF_R(KORB_VM(c)->string_class, "=~",          str_match_op, 1);
    DEF_R(KORB_VM(c)->string_class, "match?",      str_match_p, -1);
    DEF_R(KORB_VM(c)->string_class, "match",       str_match, -1);
    DEF_R(KORB_VM(c)->string_class, "scan",        str_scan, 1);
    DEF_R(KORB_VM(c)->string_class, "sum",         str_sum, -1);
    DEF_R(KORB_VM(c)->string_class, "unpack",      str_unpack, -1);
    /* String#unpack1 — first element of #unpack.  Delegates by calling
     * unpack and returning element 0 (or nil if empty). */
    {
        RESULT _str_unpack1(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            /* Stage self + args at sp for str_unpack. */
            sp[0] = self;
            for (int i = 0; i < argc; i++) sp[1 + i] = argv[i];
            VALUE r = UNWRAP(str_unpack(c, argc, sp + 1 + argc));
            if (SPECIAL_CONST_P(r) || BUILTIN_TYPE(r) != T_ARRAY) return RESULT_OK(Qnil);
            struct korb_array *a = (struct korb_array *)r;
            if (a->len == 0) return RESULT_OK(Qnil);
            return RESULT_OK(korb_ary_items(a)[0]);
        }
        DEF_R(KORB_VM(c)->string_class, "unpack1",  _str_unpack1, -1);
    }
    DEF_R(KORB_VM(c)->string_class, "center",      str_center, -1);
    DEF_R(KORB_VM(c)->string_class, "ljust",       str_ljust,  -1);
    DEF_R(KORB_VM(c)->string_class, "rjust",       str_rjust,  -1);
    DEF_R(KORB_VM(c)->string_class, "chop",        str_chop,    0);
    DEF_R(KORB_VM(c)->string_class, "chop!",       str_chop_bang, 0);
    DEF_R(KORB_VM(c)->string_class, "count",       str_count_chars, -1);
    DEF_R(KORB_VM(c)->string_class, "delete",      str_delete_chars, -1);
    DEF_R(KORB_VM(c)->string_class, "squeeze",     str_squeeze, -1);
    DEF_R(KORB_VM(c)->string_class, "swapcase",     str_swapcase,        0);
    DEF_R(KORB_VM(c)->string_class, "swapcase!",    str_swapcase_bang,   0);
    DEF_R(KORB_VM(c)->string_class, "capitalize",   str_capitalize,      0);
    DEF_R(KORB_VM(c)->string_class, "capitalize!",  str_capitalize_bang, 0);
    DEF_R(KORB_VM(c)->string_class, "lines",       str_lines,   -1);
    DEF_R(KORB_VM(c)->string_class, "partition",   str_partition, 1);
    DEF_R(KORB_VM(c)->string_class, "rpartition",  str_rpartition, 1);
    DEF_R(KORB_VM(c)->string_class, "succ",        str_succ,    0);
    DEF_R(KORB_VM(c)->string_class, "next",        str_succ,    0);
    DEF_R(KORB_VM(c)->string_class, "each_byte",   str_each_byte, 0);
    DEF_R(KORB_VM(c)->string_class, "ord",         str_ord,     0);
    DEF_R(KORB_VM(c)->string_class, "eql?",        str_eql,     1);
    DEF_R(KORB_VM(c)->string_class, "clone",       str_clone,   0);
    DEF_R(KORB_VM(c)->string_class, "intern",      str_to_sym,  0);

    /* extra Array */
    DEF_R(KORB_VM(c)->array_class, "sort",       ary_sort,       -1);
    DEF_R(KORB_VM(c)->array_class, "sort_by",    ary_sort_by,     0);
    DEF_R(KORB_VM(c)->array_class, "zip",        ary_zip,        -1);
    DEF_R(KORB_VM(c)->array_class, "flatten",    ary_flatten,    -1);
    DEF_R(KORB_VM(c)->array_class, "compact",    ary_compact,     0);
    DEF_R(KORB_VM(c)->array_class, "uniq",       ary_uniq,       -1);
    DEF_R(KORB_VM(c)->array_class, "include?",   ary_include,     1);
    DEF_R(KORB_VM(c)->array_class, "any?",       ary_any_p,      -1);
    DEF_R(KORB_VM(c)->array_class, "all?",       ary_all_p,      -1);
    DEF_R(KORB_VM(c)->array_class, "none?",      ary_none_p,     -1);
    DEF_R(KORB_VM(c)->array_class, "min",        ary_min,        -1);
    DEF_R(KORB_VM(c)->array_class, "max",        ary_max,        -1);
    DEF_R(KORB_VM(c)->array_class, "sum",        ary_sum,        -1);
    DEF_R(KORB_VM(c)->array_class, "each_slice", ary_each_slice,  1);
    DEF_R(KORB_VM(c)->array_class, "step",       ary_step,       -1);
    DEF_R(KORB_VM(c)->array_class, "===",        ary_eqq,         1);
    DEF_R(KORB_VM(c)->array_class, "pack",       ary_pack,       -1);
    DEF_R(KORB_VM(c)->array_class, "concat",     ary_concat,     -1);
    DEF_R(KORB_VM(c)->array_class, "-",          ary_minus,       1);
    {
        RESULT ary_plus(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->array_class, "+",          ary_plus,        1);
    }
    DEF_R(KORB_VM(c)->array_class, "index",      ary_index,      -1);
    DEF_R(KORB_VM(c)->array_class, "find_index", ary_index,      -1);
    DEF_R(KORB_VM(c)->array_class, "reverse",      ary_reverse,      0);
    DEF_R(KORB_VM(c)->array_class, "reverse_each", ary_reverse_each, 0);
    DEF_R(KORB_VM(c)->array_class, "clear",      ary_clear,       0);
    DEF_R(KORB_VM(c)->array_class, "unshift",    ary_unshift,    -1);
    DEF_R(KORB_VM(c)->array_class, "prepend",    ary_unshift,    -1);
    DEF_R(KORB_VM(c)->array_class, "shift",      ary_shift,      -1);
    DEF_R(KORB_VM(c)->array_class, "each_with_object", ary_each_with_object, 1);
    DEF_R(KORB_VM(c)->array_class, "transpose", ary_transpose, 0);
    DEF_R(KORB_VM(c)->array_class, "count",     ary_count, -1);
    DEF_R(KORB_VM(c)->array_class, "drop",      ary_drop,   1);
    DEF_R(KORB_VM(c)->array_class, "take",      ary_take,   1);
    DEF_R(KORB_VM(c)->array_class, "fill",      ary_fill,  -1);
    DEF_R(KORB_VM(c)->array_class, "sample",    ary_sample, -1);
    DEF_R(KORB_VM(c)->array_class, "empty?",    ary_empty_p, 0);
    DEF_R(KORB_VM(c)->array_class, "find",      ary_find, 0);
    DEF_R(KORB_VM(c)->array_class, "detect",    ary_find, 0);
    DEF_R(KORB_VM(c)->array_class, "min_by",    ary_min_by, 0);
    DEF_R(KORB_VM(c)->array_class, "max_by",    ary_max_by, 0);
    DEF_R(KORB_VM(c)->array_class, "*",         ary_mul, 1);
    DEF_R(KORB_VM(c)->array_class, "uniq!",     ary_uniq, -1);
    DEF_R(KORB_VM(c)->array_class, "sort!",     ary_sort_bang, -1);
    DEF_R(KORB_VM(c)->array_class, "compact!",  ary_compact_bang, 0);
    DEF_R(KORB_VM(c)->array_class, "reverse!",  ary_reverse_bang, 0);
    DEF_R(KORB_VM(c)->array_class, "rotate!",   ary_rotate_bang, -1);
    DEF_R(KORB_VM(c)->array_class, "rotate",    ary_rotate, -1);
    DEF_R(KORB_VM(c)->array_class, "flatten!",  ary_flatten_bang, -1);
    DEF_R(KORB_VM(c)->array_class, "freeze",    kernel_freeze, 0);
    DEF_R(KORB_VM(c)->array_class, "frozen?",   kernel_frozen_p, 0);
    {
        RESULT ary_hash_content(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->array_class, "hash",      ary_hash_content, 0);
    }
    DEF_R(KORB_VM(c)->array_class, "slice!",    ary_slice_bang, -1);
    DEF_R(KORB_VM(c)->array_class, "slice",     ary_slice,      -1);
    DEF_R(KORB_VM(c)->array_class, "flat_map",       ary_flat_map, 0);
    DEF_R(KORB_VM(c)->array_class, "collect_concat", ary_flat_map, 0);
    DEF_R(KORB_VM(c)->array_class, "dig",            ary_dig,      -1);
    DEF_R(KORB_VM(c)->array_class, "take_while",     ary_take_while, 0);
    DEF_R(KORB_VM(c)->array_class, "drop_while",     ary_drop_while, 0);
    DEF_R(KORB_VM(c)->array_class, "shuffle",        ary_shuffle,    0);
    DEF_R(KORB_VM(c)->array_class, "bsearch",        ary_bsearch,    -1);
    DEF_R(KORB_VM(c)->array_class, "one?",           ary_one_p,     -1);
    /* String additions */
    DEF_R(KORB_VM(c)->string_class, "hex",           str_hex,           0);
    DEF_R(KORB_VM(c)->string_class, "oct",           str_oct,           0);
    DEF_R(KORB_VM(c)->string_class, "prepend",       str_prepend,      -1);
    DEF_R(KORB_VM(c)->string_class, "insert",        str_insert,        2);
    DEF_R(KORB_VM(c)->string_class, "delete_prefix",  str_delete_prefix,  1);
    DEF_R(KORB_VM(c)->string_class, "delete_prefix!", str_delete_prefix_bang, 1);
    DEF_R(KORB_VM(c)->string_class, "delete_suffix",  str_delete_suffix,  1);
    DEF_R(KORB_VM(c)->string_class, "delete_suffix!", str_delete_suffix_bang, 1);
    DEF_R(KORB_VM(c)->string_class, "byteslice",     str_byteslice,    -1);
    DEF_R(KORB_VM(c)->string_class, "append_as_bytes", str_append_as_bytes, -1);
    DEF_R(KORB_VM(c)->string_class, "setbyte",       str_setbyte,       2);
    DEF_R(KORB_VM(c)->string_class, "getbyte",       str_getbyte,       1);
    /* Numeric eql? — type-strict */
    DEF_R(KORB_VM(c)->integer_class, "eql?",          int_eql,           1);
    DEF_R(KORB_VM(c)->float_class, "eql?",          flt_eql,           1);
    DEF_R(KORB_VM(c)->array_class, "each_cons",      ary_each_cons,  1);
    DEF_R(KORB_VM(c)->array_class, "minmax_by",      ary_minmax_by,  0);
    DEF_R(KORB_VM(c)->array_class, "assoc",       ary_assoc,       1);
    DEF_R(KORB_VM(c)->array_class, "rassoc",      ary_rassoc,      1);
    DEF_R(KORB_VM(c)->array_class, "at",          ary_at,          1);
    DEF_R(KORB_VM(c)->array_class, "to_a",        ary_to_a,        0);
    DEF_R(KORB_VM(c)->array_class, "to_ary",      ary_self,        0);
    DEF_R(KORB_VM(c)->array_class, "deconstruct", ary_self,        0);
    DEF_R(KORB_VM(c)->array_class, "fetch",       ary_fetch,       -1);
    DEF_R(KORB_VM(c)->array_class, "fetch_values", ary_fetch_values, -1);
    DEF_R(KORB_VM(c)->array_class, "delete",      ary_delete,      1);
    DEF_R(KORB_VM(c)->array_class, "delete_at",   ary_delete_at,   1);
    DEF_R(KORB_VM(c)->array_class, "delete_if",   ary_delete_if,   0);
    DEF_R(KORB_VM(c)->array_class, "reject",      ary_reject,      0);
    DEF_R(KORB_VM(c)->array_class, "reject!",     ary_reject_bang, 0);
    DEF_R(KORB_VM(c)->array_class, "insert",      ary_insert,     -1);
    DEF_R(KORB_VM(c)->array_class, "replace",     ary_replace,     1);
    DEF_R(KORB_VM(c)->array_class, "each_index",  ary_each_index,  0);
    DEF_R(KORB_VM(c)->array_class, "clone",       ary_clone,       0);
    DEF_R(KORB_VM(c)->array_class, "eql?",        ary_eql,         1);
    DEF_R(KORB_VM(c)->array_class, "<=>",         ary_cmp,         1);
    DEF_R(KORB_VM(c)->array_class, "cycle",       ary_cycle,      -1);
    DEF_R(KORB_VM(c)->array_class, "combination", ary_combination, 1);
    DEF_R(KORB_VM(c)->array_class, "permutation", ary_permutation, -1);
    DEF_R(KORB_VM(c)->array_class, "repeated_combination", ary_repeated_combination, -1);
    DEF_R(KORB_VM(c)->array_class, "repeated_permutation", ary_repeated_permutation, -1);
    DEF_R(KORB_VM(c)->array_class, "product",     ary_product,    -1);
    {
        /* Override Class.new on Array's metaclass so Array.new(n, default)
         * and Array.new(n) { ... } actually build an array of the right
         * size — Class#new's generic path uses korb_object_new which
         * doesn't size a T_ARRAY correctly. */
        struct korb_class *cAryMeta = korb_class_new(c, c->sp_top, korb_intern("ArrayMeta"),
                                                      KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cAryMeta, korb_intern("new"), ary_class_new, -1);
        /* Array#initialize — populate (or replace contents of) an already
         * allocated Array.  Subclasses can override this. */
        extern RESULT ary_initialize(CTX *c, int argc, VALUE *sp);
        DEF_R_PRIV(KORB_VM(c)->array_class, "initialize", ary_initialize, -1);
        /* Array[] — class method that returns an Array literal of args. */
        RESULT ary_class_brackets(CTX *c, int argc, VALUE *sp);
        korb_class_add_method_cfunc_r(cAryMeta, korb_intern("[]"), ary_class_brackets, -1);
        KORB_VM(c)->array_class->basic.klass = (VALUE)cAryMeta;
        /* Array.try_convert(obj) — obj.to_ary if obj responds and returns
         * Array, else nil.  Raises TypeError if #to_ary returns non-Array. */
        korb_class_add_method_cfunc_r(cAryMeta, korb_intern("try_convert"),
            ({
                RESULT _try(CTX *c, int argc, VALUE *sp) {
                    c->sp_top = sp;
                    VALUE *argv = sp - argc;
                    if (argc < 1) return RESULT_OK(Qnil);
                    VALUE o = argv[0];
                    if (!SPECIAL_CONST_P(o) && BUILTIN_TYPE(o) == T_ARRAY) return RESULT_OK(o);
                    VALUE klass_v = (VALUE)korb_class_of_class(o);
                    if (!klass_v || !korb_class_find_method((struct korb_class *)klass_v,
                                                             korb_intern("to_ary"))) {
                        return RESULT_OK(Qnil);
                    }
                    RESULT _rr = korb_funcall(c, o, korb_intern("to_ary"), 0, NULL);
                    if (_rr.state == KORB_RAISE) return RESULT_OK(Qnil);
                    if (_rr.state != KORB_NORMAL) return _rr;
                    VALUE r = _rr.value;
                    if (NIL_P(r)) return RESULT_OK(Qnil);
                    if (SPECIAL_CONST_P(r) || BUILTIN_TYPE(r) != T_ARRAY) {
                        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                        const char *src_n = korb_id_name(korb_class_of_class(o)->name);
                        const char *got_n = SPECIAL_CONST_P(r) ? "(special)"
                                                                : korb_id_name(korb_class_of_class(r)->name);
                        return korb_raise(c, (struct korb_class *)eT,
                                   "can't convert %s to Array (%s#to_ary gives %s)",
                                   src_n, src_n, got_n);
                    }
                    return RESULT_OK(r);
                }
                _try;
            }), -1);
    }

    /* extra Hash */
    DEF_R(KORB_VM(c)->hash_class, "keys",       hash_keys,       0);
    DEF_R(KORB_VM(c)->hash_class, "values",     hash_values,     0);
    DEF_R(KORB_VM(c)->hash_class, "each_value", hash_each_value, 0);
    DEF_R(KORB_VM(c)->hash_class, "each_key",   hash_each_key,   0);
    DEF_R(KORB_VM(c)->hash_class, "each_pair",  hash_each,       0);
    DEF_R(KORB_VM(c)->hash_class, "key?",       hash_key_p,      1);
    DEF_R(KORB_VM(c)->hash_class, "has_key?",   hash_key_p,      1);
    DEF_R(KORB_VM(c)->hash_class, "include?",   hash_key_p,      1);
    /* Hash#key(val) — return first key whose value == val, or nil. */
    {
        RESULT _hash_key(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            if (argc < 1) return RESULT_OK(Qnil);
            if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_HASH) return RESULT_OK(Qnil);
            struct korb_hash *h = (struct korb_hash *)self;
            VALUE target = argv[0];
            for (struct korb_hash_entry *e = h->first; e; e = e->next) {
                /* Use korb_eq for value compare. */
                if (e->value == target) return RESULT_OK(e->key);
                if (!SPECIAL_CONST_P(e->value) && !SPECIAL_CONST_P(target)) {
                    VALUE r = UNWRAP(korb_funcall(c, e->value, korb_intern("=="), 1, &target));
                    if (RTEST(r)) return RESULT_OK(e->key);
                }
            }
            return RESULT_OK(Qnil);
        }
        DEF_R(KORB_VM(c)->hash_class, "key", _hash_key, 1);
    }
    DEF_R(KORB_VM(c)->hash_class, "merge",      hash_merge,     -1);
    DEF_R(KORB_VM(c)->hash_class, "merge!",     hash_merge_bang,-1);
    DEF_R(KORB_VM(c)->hash_class, "invert",     hash_invert,     0);
    DEF_R(KORB_VM(c)->hash_class, "to_a",       hash_to_a,       0);
    DEF_R(KORB_VM(c)->hash_class, "delete",     hash_delete,    -1);
    DEF_R(KORB_VM(c)->hash_class, "fetch",      hash_fetch,     -1);
    DEF_R(KORB_VM(c)->hash_class, "__korb_required_kwarg__", hash_required_kwarg, 1);
    DEF_R(KORB_VM(c)->hash_class, "__korb_required_kwargs_check__", hash_required_kwargs_check, 1);
    DEF_R(KORB_VM(c)->hash_class, "__korb_kwargs_validate__", hash_kwargs_validate, 1);
    DEF_R(KORB_VM(c)->hash_class, "compare_by_identity",  hash_compare_by_identity, 0);
    DEF_R(KORB_VM(c)->hash_class, "compare_by_identity?", hash_compare_by_identity_p, 0);
    DEF_R(KORB_VM(c)->hash_class, "clear",       hash_clear,        0);
    DEF_R(KORB_VM(c)->hash_class, "delete_if",   hash_delete_if,    0);
    DEF_R(KORB_VM(c)->hash_class, "keep_if",     hash_keep_if,      0);
    DEF_R(KORB_VM(c)->hash_class, "compact",     hash_compact,      0);
    DEF_R(KORB_VM(c)->hash_class, "compact!",    hash_compact_bang, 0);
    DEF_R(KORB_VM(c)->hash_class, "values_at",   hash_values_at,   -1);
    DEF_R(KORB_VM(c)->hash_class, "fetch_values",hash_fetch_values,-1);
    DEF_R(KORB_VM(c)->hash_class, "member?",     hash_key_p,        1);
    DEF_R(KORB_VM(c)->hash_class, "reject",      hash_reject,       0);
    {
        RESULT hash_reject_bang(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->hash_class, "reject!",     hash_reject_bang,  0);
    }
    DEF_R(KORB_VM(c)->hash_class, "replace",     hash_replace,      1);
    DEF_R(KORB_VM(c)->hash_class, "shift",       hash_shift,        0);
    DEF_R(KORB_VM(c)->hash_class, "store",       hash_aset,         2);
    DEF_R(KORB_VM(c)->hash_class, "update",      hash_merge_bang,  -1);
    DEF_R(KORB_VM(c)->hash_class, "slice",       hash_slice,       -1);
    DEF_R(KORB_VM(c)->hash_class, "except",      hash_except,      -1);
    DEF_R(KORB_VM(c)->hash_class, "count",       hash_count,       -1);
    DEF_R(KORB_VM(c)->hash_class, "min_by",      hash_min_by,       0);
    DEF_R(KORB_VM(c)->hash_class, "max_by",      hash_max_by,       0);
    DEF_R(KORB_VM(c)->hash_class, "sort",        hash_sort,         0);
    DEF_R(KORB_VM(c)->hash_class, "deconstruct_keys", hash_deconstruct_keys, 1);
    DEF_R(KORB_VM(c)->hash_class, "dig",              hash_dig,              -1);
    DEF_R(KORB_VM(c)->hash_class, "has_value?",       hash_has_value_p,       1);
    DEF_R(KORB_VM(c)->hash_class, "value?",           hash_has_value_p,       1);
    DEF_R(KORB_VM(c)->hash_class, "group_by",         hash_group_by,          0);
    DEF_R(KORB_VM(c)->hash_class, "sort_by",          hash_sort_by,           0);
    DEF_R(KORB_VM(c)->hash_class, "filter_map",       hash_filter_map,        0);
    DEF_R(KORB_VM(c)->hash_class, "sum",              hash_sum,              -1);
    DEF_R(KORB_VM(c)->hash_class, "each_with_object", hash_each_with_object,  1);
    DEF_R(KORB_VM(c)->hash_class, "take",             hash_take,              1);
    DEF_R(KORB_VM(c)->hash_class, "flat_map",         hash_flat_map,          0);
    DEF_R(KORB_VM(c)->hash_class, "collect_concat",   hash_flat_map,          0);
    DEF_R(KORB_VM(c)->hash_class, "default",      hash_default_get,      0);
    DEF_R(KORB_VM(c)->hash_class, "default=",     hash_default_set,      1);
    DEF_R(KORB_VM(c)->hash_class, "default_proc", hash_default_proc_get, 0);
    DEF_R(KORB_VM(c)->hash_class, "default_proc=", hash_default_proc_set, 1);
    {
        /* Override Class.new on Hash's metaclass so Hash.new(default) and
         * Hash.new { ... } actually create a real hash with the default. */
        struct korb_class *cHshMeta = korb_class_new(c, c->sp_top, korb_intern("HashMeta"),
                                                      KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cHshMeta, korb_intern("new"), hash_class_new, -1);
        /* Hash#initialize — subclasses can override; called by Hash.new
         * after the empty allocation. */
        DEF_R_PRIV(KORB_VM(c)->hash_class, "initialize", hash_initialize, -1);
        korb_class_add_method_cfunc_r(cHshMeta, korb_intern("[]"),  hash_class_aref, -1);
        KORB_VM(c)->hash_class->basic.klass = (VALUE)cHshMeta;
        /* Hash.try_convert(obj) — obj.to_hash if responding and returns
         * Hash, else nil.  Raises TypeError if #to_hash returns non-Hash. */
        korb_class_add_method_cfunc_r(cHshMeta, korb_intern("try_convert"),
            ({
                RESULT _try(CTX *c, int argc, VALUE *sp) {
                    c->sp_top = sp;
                    VALUE *argv = sp - argc;
                    if (argc < 1) return RESULT_OK(Qnil);
                    VALUE o = argv[0];
                    if (!SPECIAL_CONST_P(o) && BUILTIN_TYPE(o) == T_HASH) return RESULT_OK(o);
                    if (SPECIAL_CONST_P(o)) return RESULT_OK(Qnil);
                    RESULT _rr = korb_funcall(c, o, korb_intern("to_hash"), 0, NULL);
                    if (_rr.state == KORB_RAISE) {
                        VALUE bang = _rr.value;
                        VALUE eNo = korb_const_get(KORB_VM(c)->object_class, korb_intern("NoMethodError"));
                        if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) &&
                            BUILTIN_TYPE(eNo) == T_CLASS) {
                            struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                            for (struct korb_class *kk = bk; kk; kk = kk->super) {
                                if (kk == (struct korb_class *)eNo) return RESULT_OK(Qnil);
                            }
                        }
                        return _rr;
                    }
                    if (_rr.state != KORB_NORMAL) return _rr;
                    VALUE r = _rr.value;
                    if (NIL_P(r)) return RESULT_OK(Qnil);
                    if (SPECIAL_CONST_P(r) || BUILTIN_TYPE(r) != T_HASH) {
                        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                        return korb_raise(c, (struct korb_class *)eT,
                                   "can't convert %s to Hash (%s#to_hash gives %s)",
                                   korb_id_name(korb_class_of_class(o)->name),
                                   korb_id_name(korb_class_of_class(o)->name),
                                   korb_id_name(korb_class_of_class(r)->name));
                    }
                    return RESULT_OK(r);
                }
                _try;
            }), -1);
    }
    /* String.new(s = "") — initialize from optional string. */
    {
        RESULT str_class_new(CTX *c, int argc, VALUE *sp);
        struct korb_class *cStrMeta = korb_class_new(c, c->sp_top, korb_intern("StringMeta"),
                                                      KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cStrMeta, korb_intern("new"), str_class_new, -1);
        /* String#initialize — subclasses can override; called by String.new
         * after the empty allocation. */
        extern RESULT str_initialize(CTX *c, int argc, VALUE *sp);
        DEF_R_PRIV(KORB_VM(c)->string_class, "initialize", str_initialize, -1);
        KORB_VM(c)->string_class->basic.klass = (VALUE)cStrMeta;
        /* String.try_convert(obj) — obj.to_str if responding and returns
         * String, else nil.  Raises TypeError if #to_str returns non-String. */
        korb_class_add_method_cfunc_r(cStrMeta, korb_intern("try_convert"),
            ({
                RESULT _try(CTX *c, int argc, VALUE *sp) {
                    c->sp_top = sp;
                    VALUE *argv = sp - argc;
                    if (argc < 1) return RESULT_OK(Qnil);
                    VALUE o = argv[0];
                    if (!SPECIAL_CONST_P(o) && BUILTIN_TYPE(o) == T_STRING) return RESULT_OK(o);
                    if (SPECIAL_CONST_P(o)) return RESULT_OK(Qnil);
                    RESULT _rr = korb_funcall(c, o, korb_intern("to_str"), 0, NULL);
                    if (_rr.state == KORB_RAISE) {
                        VALUE bang = _rr.value;
                        VALUE eNo = korb_const_get(KORB_VM(c)->object_class, korb_intern("NoMethodError"));
                        if (!SPECIAL_CONST_P(bang) && !SPECIAL_CONST_P(eNo) &&
                            BUILTIN_TYPE(eNo) == T_CLASS) {
                            struct korb_class *bk = (struct korb_class *)((struct RBasic *)bang)->klass;
                            for (struct korb_class *kk = bk; kk; kk = kk->super) {
                                if (kk == (struct korb_class *)eNo) return RESULT_OK(Qnil);
                            }
                        }
                        return _rr;
                    }
                    if (_rr.state != KORB_NORMAL) return _rr;
                    VALUE r = _rr.value;
                    if (NIL_P(r)) return RESULT_OK(Qnil);
                    if (SPECIAL_CONST_P(r) || BUILTIN_TYPE(r) != T_STRING) {
                        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                        return korb_raise(c, (struct korb_class *)eT,
                                   "can't convert %s to String (%s#to_str gives %s)",
                                   korb_id_name(korb_class_of_class(o)->name),
                                   korb_id_name(korb_class_of_class(o)->name),
                                   korb_id_name(korb_class_of_class(r)->name));
                    }
                    return RESULT_OK(r);
                }
                _try;
            }), -1);
    }
    DEF_R(KORB_VM(c)->hash_class, "===",        hash_eqq,        1);
    DEF_R(KORB_VM(c)->hash_class, "==",         hash_eq,         1);
    DEF_R(KORB_VM(c)->hash_class, "dup",        hash_dup,        0);
    DEF_R(KORB_VM(c)->hash_class, "clone",      hash_clone,      0);
    DEF_R(KORB_VM(c)->hash_class, "empty?",     hash_empty_p,    0);
    DEF_R(KORB_VM(c)->hash_class, "map",        hash_map,        0);
    DEF_R(KORB_VM(c)->hash_class, "collect",    hash_map,        0);
    DEF_R(KORB_VM(c)->hash_class, "select",     hash_select,     0);
    DEF_R(KORB_VM(c)->hash_class, "filter",     hash_select,     0);
    DEF_R(KORB_VM(c)->hash_class, "partition",  hash_partition,  0);
    DEF_R(KORB_VM(c)->hash_class, "tally",      hash_tally,      0);
    DEF_R(KORB_VM(c)->hash_class, "filter",     hash_select,     0);
    DEF_R(KORB_VM(c)->hash_class, "reduce",     hash_reduce,    -1);
    DEF_R(KORB_VM(c)->hash_class, "inject",     hash_reduce,    -1);

    /* extra Range */
    DEF_R(KORB_VM(c)->range_class, "step",     rng_step,    -1);
    DEF_R(KORB_VM(c)->range_class, "zip",      rng_zip,     -1);
    DEF_R(KORB_VM(c)->range_class, "each_with_index", rng_each_with_index, 0);
    DEF_R(KORB_VM(c)->range_class, "size",     rng_size,     0);
    DEF_R(KORB_VM(c)->range_class, "length",   rng_size,     0);
    DEF_R(KORB_VM(c)->range_class, "include?", rng_include, -1);
    DEF_R(KORB_VM(c)->range_class, "===",      rng_include, -1);
    DEF_R(KORB_VM(c)->range_class, "map",      rng_map,      0);
    DEF_R(KORB_VM(c)->range_class, "collect",  rng_map,      0);
    DEF_R(KORB_VM(c)->range_class, "select",   rng_select,   0);
    DEF_R(KORB_VM(c)->range_class, "filter",   rng_select,   0);
    DEF_R(KORB_VM(c)->range_class, "reduce",   rng_reduce,  -1);
    DEF_R(KORB_VM(c)->range_class, "inject",   rng_reduce,  -1);
    DEF_R(KORB_VM(c)->range_class, "all?",     rng_all_p,    0);
    DEF_R(KORB_VM(c)->range_class, "any?",     rng_any_p,    0);
    DEF_R(KORB_VM(c)->range_class, "count",    rng_count,    0);

    /* extra Symbol additions later (KORB_VM(c)->symbol_class defined further down) */

    /* Struct.new */
    /* Create Struct class object */
    struct korb_class *cStruct = korb_class_new(c, c->sp_top, korb_intern("Struct"), KORB_VM(c)->object_class, T_OBJECT);
    korb_const_set(KORB_VM(c)->object_class, korb_intern("Struct"), (VALUE)cStruct);
    /* Struct.new is a class-level cfunc — install on Class so any class can call .new */
    /* But only Struct itself should have this constructor.  We add it on the class itself's
     * method table; calling Struct.new dispatches to class_of(Struct) which is Class.
     * Workaround: install on the Struct class's "self class" which is Class — but that
     * makes ALL classes have struct_class_new.  Instead, install a static name like
     * "__new_struct__" and use a stub cfunc on Struct that detects Struct === self.
     *
     * Simpler: add Class#new to delegate to self.class_new if class is Struct.
     * Even simpler: replace Class#new with a wrapper that handles Struct specially. */
    /* For our purposes, just add Struct as a singleton-like method to KORB_VM(c)->class_class keyed by the
     * actual class identity check: we install struct_class_new on KORB_VM(c)->class_class under "new_struct"
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
        extern RESULT class_new(CTX *c, int argc, VALUE *sp);
        /* not exposed — we need a wrapper.  Define inline: */
    }
    /* Add struct_class_new under name "new" on Struct.  Since dispatch goes through
     * class_of(Struct) = Class, NOT Struct itself — we need to use a different approach.
     * For now, let users call Struct.new(...) and ensure that lookup goes to Struct's
     * own metaclass.  We create a special ko_class for Struct's metaclass with .new
     * pointing to struct_class_new. */
    {
        struct korb_class *cStructMeta = korb_class_new(c, c->sp_top, korb_intern("StructMeta"), KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cStructMeta, korb_intern("new"), struct_class_new, -1);
        cStruct->basic.klass = (VALUE)cStructMeta;
    }

    /* Data.define — like Struct but immutable.  We implement as a thin
     * shim over struct_class_new and freeze the instance after init. */
    {
        struct korb_class *cData = korb_class_new(c, c->sp_top, korb_intern("Data"), KORB_VM(c)->object_class, T_OBJECT);
        korb_const_set(KORB_VM(c)->object_class, korb_intern("Data"), (VALUE)cData);
        struct korb_class *cDataMeta = korb_class_new(c, c->sp_top, korb_intern("DataMeta"),
                                                       KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cDataMeta, korb_intern("define"),
                                       struct_class_new, -1);
        cData->basic.klass = (VALUE)cDataMeta;
    }

    /* File class */
    struct korb_class *cFile = korb_class_new(c, c->sp_top, korb_intern("File"), KORB_VM(c)->object_class, T_OBJECT);
    korb_const_set(KORB_VM(c)->object_class, korb_intern("File"), (VALUE)cFile);
    {
        struct korb_class *cFileMeta = korb_class_new(c, c->sp_top, korb_intern("FileMeta"), KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("read"), file_read, -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("join"), file_join, -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("exist?"), file_exist_p, -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("exists?"), file_exist_p, -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("directory?"), file_directory_p, -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("file?"),     file_file_p,     -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("size"),      file_size,       -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("unlink"),    file_unlink,     -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("delete"),    file_unlink,     -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("rename"),    file_rename,     -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("chmod"),     file_chmod,      -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("realpath"),  file_realpath,   -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("dirname"), file_dirname, -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("basename"), file_basename, -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("expand_path"), file_expand_path, -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("extname"), file_extname, 1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("binread"), file_binread, 1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("open"), file_open, -1);
        korb_class_add_method_cfunc_r(cFileMeta, korb_intern("write"), file_write, -1);
        cFile->basic.klass = (VALUE)cFileMeta;
    }
    /* Dir / Process classes — stubs so common Ruby idioms don't NPE. */
    {
        struct korb_class *cDir = korb_class_new(c, c->sp_top, korb_intern("Dir"), KORB_VM(c)->object_class, T_OBJECT);
        korb_const_set(KORB_VM(c)->object_class, korb_intern("Dir"), (VALUE)cDir);
        struct korb_class *cDirMeta = korb_class_new(c, c->sp_top, korb_intern("DirMeta"), KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("pwd"),     dir_pwd,     0);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("getwd"),   dir_pwd,     0);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("entries"), dir_entries, 1);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("chdir"),   dir_chdir,  -1);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("glob"),    dir_glob,    1);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("[]"),      dir_glob,    1);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("mkdir"),   dir_mkdir,  -1);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("rmdir"),   dir_rmdir,  -1);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("delete"),  dir_rmdir,  -1);
        korb_class_add_method_cfunc_r(cDirMeta, korb_intern("unlink"),  dir_rmdir,  -1);
        cDir->basic.klass = (VALUE)cDirMeta;
    }
    {
        struct korb_class *cProcess = korb_class_new(c, c->sp_top, korb_intern("Process"), KORB_VM(c)->object_class, T_OBJECT);
        korb_const_set(KORB_VM(c)->object_class, korb_intern("Process"), (VALUE)cProcess);
        struct korb_class *cProcessMeta = korb_class_new(c, c->sp_top, korb_intern("ProcessMeta"), KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cProcessMeta, korb_intern("pid"), process_pid, 0);
        korb_class_add_method_cfunc_r(cProcessMeta, korb_intern("clock_gettime"), proc_clock_gettime_stub, -1);
        korb_class_add_method_cfunc_r(cProcessMeta, korb_intern("spawn"), process_spawn, -1);
        korb_class_add_method_cfunc_r(cProcessMeta, korb_intern("fork"), process_fork, 0);
        korb_class_add_method_cfunc_r(cProcessMeta, korb_intern("wait"), process_wait, -1);
        korb_class_add_method_cfunc_r(cProcessMeta, korb_intern("waitpid"), process_wait, -1);
        korb_class_add_method_cfunc_r(cProcessMeta, korb_intern("kill"), process_kill, -1);
        cProcess->basic.klass = (VALUE)cProcessMeta;
        /* Process::Status — minimal class with attribute readers. */
        struct korb_class *cStatus = korb_class_new(c, c->sp_top, korb_intern("Status"), KORB_VM(c)->object_class, T_OBJECT);
        korb_const_set(cProcess, korb_intern("Status"), (VALUE)cStatus);
        korb_class_add_method_cfunc_r(cStatus, korb_intern("exitstatus"), pstatus_exitstatus, 0);
        korb_class_add_method_cfunc_r(cStatus, korb_intern("pid"), pstatus_pid, 0);
        korb_class_add_method_cfunc_r(cStatus, korb_intern("success?"), pstatus_success_p, 0);
        korb_class_add_method_cfunc_r(cStatus, korb_intern("signaled?"), pstatus_signaled_p, 0);
        korb_class_add_method_cfunc_r(cStatus, korb_intern("termsig"), pstatus_termsig, 0);
        korb_class_add_method_cfunc_r(cStatus, korb_intern("to_i"), pstatus_to_i, 0);
        /* Signal — module with class methods. */
        struct korb_class *cSignal = korb_module_new(c, c->sp_top, korb_intern("Signal"));
        korb_const_set(KORB_VM(c)->object_class, korb_intern("Signal"), (VALUE)cSignal);
        struct korb_class *cSignalMeta = korb_singleton_class_of(c, c->sp_top, cSignal);
        korb_class_add_method_cfunc_r(cSignalMeta, korb_intern("trap"), signal_trap, -1);
        korb_class_add_method_cfunc_r(cSignalMeta, korb_intern("list"), signal_list, 0);
        /* Kernel#system / `cmd` / exec at top-level (Object). */
        DEF_R(KORB_VM(c)->object_class, "system", kernel_system, -1);
        DEF_R(KORB_VM(c)->object_class, "`",      kernel_xstring, 1);
        DEF_R(KORB_VM(c)->object_class, "exec",   kernel_exec, -1);
        DEF_R(KORB_VM(c)->object_class, "fork",   process_fork, 0);
        DEF_R(KORB_VM(c)->object_class, "spawn",  process_spawn, -1);
        DEF_R(KORB_VM(c)->object_class, "trap",   signal_trap, -1);
        /* CLOCK_MONOTONIC constant on Process — sentinel value, used
         * only by clock_gettime which ignores it. */
        korb_const_set(cProcess, korb_intern("CLOCK_MONOTONIC"), INT2FIX(1));
        korb_const_set(cProcess, korb_intern("CLOCK_REALTIME"), INT2FIX(0));
    }

    /* Instance methods on File: it doubles as our IO class for opened
     * files.  Walk-style readers + line iterators + writers. */
    korb_class_add_method_cfunc_r(cFile, korb_intern("close"),     io_close,     0);
    korb_class_add_method_cfunc_r(cFile, korb_intern("read"),      io_read,     -1);
    korb_class_add_method_cfunc_r(cFile, korb_intern("gets"),      io_gets,     -1);
    korb_class_add_method_cfunc_r(cFile, korb_intern("each_line"), io_each_line, 0);
    korb_class_add_method_cfunc_r(cFile, korb_intern("each"),      io_each_line, 0);
    korb_class_add_method_cfunc_r(cFile, korb_intern("puts"),      io_puts,     -1);
    korb_class_add_method_cfunc_r(cFile, korb_intern("write"),     io_write,    -1);
    korb_class_add_method_cfunc_r(cFile, korb_intern("print"),     io_print,    -1);
    korb_class_add_method_cfunc_r(cFile, korb_intern("eof?"),      io_eof_p,     0);

    /* IO / STDOUT / $stdout — these 3 instance objects + their backing class
     * all live across nested alloc-fires-GC calls, so pin them in sp[0..3].
     * Without this, the 2nd korb_object_new moves the 1st stdout_obj but
     * the C local stays stale — the subsequent korb_const_set then writes
     * the stale (= now in some abandoned space) addr into
     * cObject->constants["STDOUT"], and future GCs walking that const slot
     * dereference an obj that no longer lives there → SEGV. */
    {
        VALUE *sp = c->sp_top;
        sp[0] = 0; sp[1] = 0; sp[2] = 0; sp[3] = 0;
        c->sp_top = sp + 4;
        sp[0] = (VALUE)korb_class_new(c, sp + 4, korb_intern("IO"), KORB_VM(c)->object_class, T_OBJECT);
        korb_const_set(KORB_VM(c)->object_class, korb_intern("IO"), sp[0]);
        /* dummy STDOUT/STDERR */
        sp[1] = korb_object_new(c, sp + 4, (struct korb_class *)sp[0]);
        sp[2] = korb_object_new(c, sp + 4, (struct korb_class *)sp[0]);
        g_stderr_obj = sp[2];
        korb_const_set(KORB_VM(c)->object_class, korb_intern("STDOUT"), sp[1]);
        korb_const_set(KORB_VM(c)->object_class, korb_intern("STDERR"), sp[2]);
        /* STDIN — backed by the real stdin FILE*, so STDIN.gets / .read work. */
        sp[3] = korb_object_new(c, sp + 4, (struct korb_class *)sp[0]);
        korb_ivar_set(sp[3], korb_intern("@__fp__"),
                      INT2FIX((long)(uintptr_t)stdin));
        korb_const_set(KORB_VM(c)->object_class, korb_intern("STDIN"), sp[3]);
        c->sp_top = sp;
    }
    /* Outside the scope — fetch from the const entries (= rooted via
     * cObject->constants).  Methods are added below; korb_class_add_method_cfunc
     * is libc-only so the C local stays valid for the chain. */
    struct korb_class *cIO = (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("IO"));
    VALUE stdout_obj = korb_const_get(KORB_VM(c)->object_class, korb_intern("STDOUT"));
    VALUE stderr_obj = korb_const_get(KORB_VM(c)->object_class, korb_intern("STDERR"));
    /* IO#puts / write methods */
    korb_class_add_method_cfunc_r(cIO, korb_intern("puts"), kernel_puts, -1);
    korb_class_add_method_cfunc_r(cIO, korb_intern("print"), kernel_print, -1);
    korb_class_add_method_cfunc_r(cIO, korb_intern("write"), kernel_print, -1);
    korb_class_add_method_cfunc_r(cIO, korb_intern("<<"), kernel_print, 1);
    korb_class_add_method_cfunc_r(cIO, korb_intern("flush"), kernel_inspect, 0);
    korb_class_add_method_cfunc_r(cIO, korb_intern("sync="), kernel_inspect, 1);
    /* IO instances also need gets/read/each_line/eof?/close — share
     * the same impls as File. */
    korb_class_add_method_cfunc_r(cIO, korb_intern("gets"),      io_gets,     -1);
    korb_class_add_method_cfunc_r(cIO, korb_intern("read"),      io_read,     -1);
    korb_class_add_method_cfunc_r(cIO, korb_intern("each_line"), io_each_line, 0);
    korb_class_add_method_cfunc_r(cIO, korb_intern("each"),      io_each_line, 0);
    korb_class_add_method_cfunc_r(cIO, korb_intern("eof?"),      io_eof_p,     0);
    korb_class_add_method_cfunc_r(cIO, korb_intern("close"),     io_close,     0);
    /* IO.pipe / IO.select / IO.popen / IO.copy_stream — class methods on
     * IO's singleton. */
    {
        extern RESULT io_class_pipe(CTX *c, int argc, VALUE *sp);
        extern RESULT io_class_select(CTX *c, int argc, VALUE *sp);
        extern RESULT io_class_popen(CTX *c, int argc, VALUE *sp);
        extern RESULT io_class_copy_stream(CTX *c, int argc, VALUE *sp);
        struct korb_class *cIOMeta = korb_singleton_class_of(c, c->sp_top, cIO);
        korb_class_add_method_cfunc_r(cIOMeta, korb_intern("pipe"),
                                     io_class_pipe, -1);
        korb_class_add_method_cfunc_r(cIOMeta, korb_intern("select"),
                                     io_class_select, -1);
        korb_class_add_method_cfunc_r(cIOMeta, korb_intern("popen"),
                                     io_class_popen, -1);
        korb_class_add_method_cfunc_r(cIOMeta, korb_intern("copy_stream"),
                                     io_class_copy_stream, -1);
    }
    korb_class_add_method_cfunc_r(cIO, korb_intern("tty?"),    io_tty_p,   0);
    korb_class_add_method_cfunc_r(cIO, korb_intern("isatty"),  io_tty_p,   0);
    korb_class_add_method_cfunc_r(cIO, korb_intern("fileno"),  io_fileno,  0);
    korb_class_add_method_cfunc_r(cIO, korb_intern("to_i"),    io_fileno,  0);

    /* gvars */
    korb_gvar_set(korb_intern("$stdout"), stdout_obj);
    korb_gvar_set(korb_intern("$stderr"), stderr_obj);
    /* $stdin: minimal stub — full stdin IO support is out of scope. */
    korb_gvar_set(korb_intern("$stdin"), Qnil);

    /* Symbol */
    {
        struct korb_class *cSymMeta = korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->symbol_class);
        DEF_R(cSymMeta, "allocate", _allocator_disallowed, -1);
        DEF_R(cSymMeta, "new",      _new_disallowed,       -1);
    }
    {
        RESULT obj_itself(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->symbol_class, "to_sym",  obj_itself, 0);
    }
    DEF_R(KORB_VM(c)->symbol_class, "to_s", sym_to_s, 0);
    DEF_R(KORB_VM(c)->symbol_class, "name", sym_to_s, 0);
    DEF_R(KORB_VM(c)->symbol_class, "id2name", sym_to_s, 0);
    DEF_R(KORB_VM(c)->symbol_class, "==", sym_eq, 1);
    DEF_R(KORB_VM(c)->symbol_class, "to_proc", sym_to_proc, 0);
    DEF_R(KORB_VM(c)->symbol_class, "===", sym_eq, 1);
    DEF_R(KORB_VM(c)->symbol_class, "inspect", kernel_inspect, 0);
    DEF_R(KORB_VM(c)->symbol_class, "<=>",     sym_cmp,        1);
    DEF_R(KORB_VM(c)->symbol_class, "succ",    sym_succ,       0);
    DEF_R(KORB_VM(c)->symbol_class, "next",    sym_succ,       0);
    /* Symbol#[] / Symbol#slice — delegate to to_s.[] */
    {
        RESULT _sym_aref(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            VALUE s = UNWRAP(korb_funcall(c, self, korb_intern("to_s"), 0, NULL));
            return korb_funcall(c, s, korb_intern("[]"), argc, argv);
        }
        DEF_R(KORB_VM(c)->symbol_class, "[]",    _sym_aref, -1);
        DEF_R(KORB_VM(c)->symbol_class, "slice", _sym_aref, -1);
    }
    DEF_R(KORB_VM(c)->symbol_class, "size",    sym_length,     0);
    DEF_R(KORB_VM(c)->symbol_class, "length",  sym_length,     0);
    DEF_R(KORB_VM(c)->symbol_class, "empty?",  sym_empty_p,    0);
    DEF_R(KORB_VM(c)->symbol_class, "upcase",     sym_upcase,     0);
    DEF_R(KORB_VM(c)->symbol_class, "downcase",   sym_downcase,   0);
    DEF_R(KORB_VM(c)->symbol_class, "capitalize", sym_capitalize, 0);
    DEF_R(KORB_VM(c)->symbol_class, "swapcase",   sym_swapcase,   0);
    /* Symbol#start_with? / end_with? / encoding — delegate to to_s. */
    {
        RESULT _sym_starts(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            VALUE s = korb_str_new_cstr(c, c->sp_top, korb_id_name(korb_sym2id(self)));
            return korb_funcall(c, s, korb_intern("start_with?"), argc, argv);
        }
        RESULT _sym_ends(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            VALUE s = korb_str_new_cstr(c, c->sp_top, korb_id_name(korb_sym2id(self)));
            return korb_funcall(c, s, korb_intern("end_with?"), argc, argv);
        }
        RESULT _sym_encoding(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            return RESULT_OK(korb_const_get(KORB_VM(c)->object_class, korb_intern("Encoding")));
        }
        DEF_R(KORB_VM(c)->symbol_class, "start_with?", _sym_starts,   -1);
        DEF_R(KORB_VM(c)->symbol_class, "end_with?",   _sym_ends,     -1);
        DEF_R(KORB_VM(c)->symbol_class, "encoding",    _sym_encoding,  0);
    }

    /* Boolean / Nil */
    DEF_R(KORB_VM(c)->true_class, "to_s", true_to_s, 0);
    DEF_R(KORB_VM(c)->false_class, "to_s", false_to_s, 0);
    DEF_R(KORB_VM(c)->nil_class, "to_s", nil_to_s, 0);
    DEF_R(KORB_VM(c)->nil_class, "inspect", nil_inspect, 0);
    /* Boolean &/|/^ — Kernel-style coercion to truthy. */
    DEF_R(KORB_VM(c)->true_class,  "&", true_and,  1);
    DEF_R(KORB_VM(c)->true_class,  "|", true_or,   1);
    DEF_R(KORB_VM(c)->true_class,  "^", true_xor,  1);
    DEF_R(KORB_VM(c)->false_class, "&", false_and, 1);
    DEF_R(KORB_VM(c)->false_class, "|", false_or,  1);
    DEF_R(KORB_VM(c)->false_class, "^", false_xor, 1);
    DEF_R(KORB_VM(c)->nil_class,   "&", nil_and,   1);
    DEF_R(KORB_VM(c)->nil_class,   "|", nil_or,    1);
    DEF_R(KORB_VM(c)->nil_class,   "^", nil_xor,   1);
    DEF_R(KORB_VM(c)->nil_class,   "to_a", nil_to_a, 0);
    DEF_R(KORB_VM(c)->nil_class,   "to_h", nil_to_h, 0);
    DEF_R(KORB_VM(c)->nil_class,   "to_f", nil_to_f, 0);
    DEF_R(KORB_VM(c)->nil_class,   "to_i", nil_to_i, 0);
    DEF_R(KORB_VM(c)->nil_class,   "nil?", nil_nil_p, 0);

    /* Forbid `.allocate` / `.new` on NilClass/TrueClass/FalseClass/
     * Integer/Numeric — their instances are immediates or abstract. */
    {
        struct korb_class *cNilMeta = korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->nil_class);
        DEF_R(cNilMeta, "allocate", _allocator_disallowed, -1);
        DEF_R(cNilMeta, "new",      _new_disallowed,       -1);
        struct korb_class *cTrueMeta = korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->true_class);
        DEF_R(cTrueMeta, "allocate", _allocator_disallowed, -1);
        DEF_R(cTrueMeta, "new",      _new_disallowed,       -1);
        struct korb_class *cFalseMeta = korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->false_class);
        DEF_R(cFalseMeta, "allocate", _allocator_disallowed, -1);
        DEF_R(cFalseMeta, "new",      _new_disallowed,       -1);
        struct korb_class *cIntMeta = korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->integer_class);
        DEF_R(cIntMeta, "allocate", _allocator_disallowed, -1);
        DEF_R(cIntMeta, "new",      _new_disallowed,       -1);
        /* Note: Numeric was previously banned but that broke subclass
         * instantiation (class N < Numeric; end; N.new).  CRuby's
         * Numeric.new fails because Numeric lacks an allocator, but
         * subclasses that define one (or inherit Object's) succeed.
         * Keep Numeric.allocate / .new available; let users handle. */
    }

    /* Proc */
    DEF_R(KORB_VM(c)->proc_class, "call", proc_call, -1);
    DEF_R(KORB_VM(c)->proc_class, "[]", proc_call, -1);
    DEF_R(KORB_VM(c)->proc_class, "yield", proc_call, -1);
    DEF_R(KORB_VM(c)->proc_class, "()", proc_call, -1);
    /* Proc#=== — same as #call.  Used by case/when with a proc value
     * pattern (`case x; when ->(...) { ... }`) and by pattern matching
     * (`case x; in ->(...) { ... }`). */
    DEF_R(KORB_VM(c)->proc_class, "===", proc_call, -1);
    DEF_R(KORB_VM(c)->proc_class, "lambda?", proc_lambda_p, 0);
    DEF_R(KORB_VM(c)->proc_class, "arity", proc_arity, 0);
    {
        RESULT proc_parameters(CTX *c, int argc, VALUE *sp);
        RESULT proc_source_location(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->proc_class, "parameters",      proc_parameters,      0);
        DEF_R(KORB_VM(c)->proc_class, "source_location", proc_source_location, 0);
    }
    DEF_R(KORB_VM(c)->proc_class, "==", proc_eq, 1);
    DEF_R(KORB_VM(c)->proc_class, "eql?", proc_eq, 1);
    {
        struct korb_class *cProcMeta = korb_class_new(c, c->sp_top, korb_intern("ProcMeta"),
                                                      KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cProcMeta, korb_intern("new"), proc_class_new, -1);
        /* Proc.allocate raises TypeError (CRuby) — no proc allocation
         * without a block. */
        {
            RESULT _proc_alloc_raise(CTX *c, int argc, VALUE *sp) {
                c->sp_top = sp;
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "allocator undefined for Proc");
            }
            korb_class_add_method_cfunc_r(cProcMeta, korb_intern("allocate"),
                                          _proc_alloc_raise, 0);
        }
        KORB_VM(c)->proc_class->basic.klass = (VALUE)cProcMeta;
    }
    {
        RESULT obj_itself(CTX *c, int argc, VALUE *sp);
        DEF_R(KORB_VM(c)->proc_class, "to_proc", obj_itself, 0);
    }

    /* Time stub class (Process is set up earlier with proper meta). */

    struct korb_class *cTime = korb_class_new(c, c->sp_top, korb_intern("Time"), KORB_VM(c)->object_class, T_OBJECT);
    korb_const_set(KORB_VM(c)->object_class, korb_intern("Time"), (VALUE)cTime);
    {
        struct korb_class *cTimeMeta = korb_class_new(c, c->sp_top, korb_intern("TimeMeta"),
                                                       KORB_VM(c)->class_class, T_CLASS);
        extern RESULT time_now_stub(CTX *c, int argc, VALUE *sp);
        korb_class_add_method_cfunc_r(cTimeMeta, korb_intern("now"), time_now_stub, 0);
        cTime->basic.klass = (VALUE)cTimeMeta;
    }

    /* Fiber */
    struct korb_class *cFiber = korb_class_new(c, c->sp_top, korb_intern("Fiber"), KORB_VM(c)->object_class, T_DATA);
    korb_const_set(KORB_VM(c)->object_class, korb_intern("Fiber"), (VALUE)cFiber);
    KORB_VM(c)->fiber_class = cFiber;
    {
        /* Fiber.new {|x| ...} */
        extern RESULT korb_fiber_new_cfunc(CTX *c, int argc, VALUE *sp);
        struct korb_class *cFiberMeta = korb_class_new(c, c->sp_top, korb_intern("FiberMeta"),
                                                        KORB_VM(c)->class_class, T_CLASS);
        korb_class_add_method_cfunc_r(cFiberMeta, korb_intern("new"), korb_fiber_new_cfunc, 0);
        /* Fiber.yield */
        extern RESULT korb_fiber_yield_cfunc(CTX *c, int argc, VALUE *sp);
        korb_class_add_method_cfunc_r(cFiberMeta, korb_intern("yield"), korb_fiber_yield_cfunc, -1);
        cFiber->basic.klass = (VALUE)cFiberMeta;
    }
    {
        extern RESULT korb_fiber_resume_cfunc(CTX *c, int argc, VALUE *sp);
        korb_class_add_method_cfunc_r(cFiber, korb_intern("resume"), korb_fiber_resume_cfunc, -1);
    }

    /* Binding class — instances are returned by Kernel#binding.  T_DATA
     * because korb_binding has its own storage layout (fp pointer +
     * names + cref) that doesn't fit T_OBJECT's ivar table. */
    {
        struct korb_class *cBinding = korb_class_new(c, c->sp_top, korb_intern("Binding"), KORB_VM(c)->object_class, T_DATA);
        korb_const_set(KORB_VM(c)->object_class, korb_intern("Binding"), (VALUE)cBinding);
        korb_class_add_method_cfunc_r(cBinding, korb_intern("local_variable_get"),    binding_local_variable_get,       1);
        korb_class_add_method_cfunc_r(cBinding, korb_intern("local_variable_set"),    binding_local_variable_set,       2);
        korb_class_add_method_cfunc_r(cBinding, korb_intern("local_variable_defined?"), binding_local_variable_defined_p, 1);
        korb_class_add_method_cfunc_r(cBinding, korb_intern("local_variables"),       binding_local_variables_cfunc,    0);
        korb_class_add_method_cfunc_r(cBinding, korb_intern("receiver"),              binding_receiver,                 0);
        korb_class_add_method_cfunc_r(cBinding, korb_intern("eval"),                  binding_eval_cfunc,              -1);
        korb_class_add_method_cfunc_r(cBinding, korb_intern("source_location"),       binding_source_location,          0);
        korb_class_add_method_cfunc_r(cBinding, korb_intern("dup"),                   binding_dup_cfunc,                0);
        korb_class_add_method_cfunc_r(cBinding, korb_intern("clone"),                 binding_clone_cfunc,             -1);
        KORB_VM(c)->binding_class = cBinding;
    }
    DEF_R_PRIV(KORB_VM(c)->object_class, "binding", kernel_binding_cfunc, 0);
    /* Proc#binding — return a Binding capturing the proc's env / self / cref. */
    if (KORB_VM(c)->proc_class) {
        DEF_R(KORB_VM(c)->proc_class, "binding", proc_binding_cfunc, 0);
    }
    /* Kernel#binding is module_function-style: PRIVATE instance method
     * (so user code's bare `binding` works) AND PUBLIC class method
     * (so `Kernel.binding` works).  We register on Kernel.metaclass
     * for the class-method side.  Note: do NOT add a regular instance
     * method on Kernel itself — our T_MODULE dispatch checks the
     * module's own method table first, which would find that and
     * raise "private method" for explicit `Kernel.binding` calls. */
    if (cKerMeta) {
        DEF_R(cKerMeta, "binding", kernel_binding_cfunc, 0);
    }
    /* Mirror the visibility info on Kernel itself for
     * Kernel.private_method_defined?(:binding) — the lookup needs
     * the method present on Kernel.  Add but mark private so the
     * T_MODULE dispatch above falls through. */
    if (KORB_VM(c)->kernel_module) {
        korb_class_add_method_cfunc_r(KORB_VM(c)->kernel_module, korb_intern("binding"),
                                       kernel_binding_cfunc, 0);
        struct korb_method *km = korb_class_find_method(KORB_VM(c)->kernel_module, korb_intern("binding"));
        if (km) km->visibility = KORB_VIS_PRIVATE;
    }

    /* Method class — instances are returned by Object#method */
    {
        struct korb_class *cMethod = korb_class_new(c, c->sp_top, korb_intern("Method"), KORB_VM(c)->object_class, T_DATA);
        korb_const_set(KORB_VM(c)->object_class, korb_intern("Method"), (VALUE)cMethod);
        /* UnboundMethod — alias of Method for now (koruby treats them
         * interchangeably).  CRuby specs check for the class name. */
        struct korb_class *cUnboundMethod = korb_class_new(c, c->sp_top,
            korb_intern("UnboundMethod"), KORB_VM(c)->object_class, T_DATA);
        korb_const_set(KORB_VM(c)->object_class, korb_intern("UnboundMethod"),
                        (VALUE)cUnboundMethod);
        (void)cUnboundMethod;
        korb_class_add_method_cfunc_r(cMethod, korb_intern("call"),     method_call,     -1);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("[]"),       method_call,     -1);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("==="),      method_call,     -1);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("()"),       method_call,     -1);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("to_proc"),  method_to_proc,   0);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("arity"),      method_arity,      0);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("name"),       method_name,       0);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("receiver"),   method_receiver,   0);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("owner"),      method_owner,      0);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("bind"),       method_bind,       1);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("unbind"),     method_unbind,     0);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("parameters"),      method_parameters,      0);
        korb_class_add_method_cfunc_r(cMethod, korb_intern("source_location"), method_source_location, 0);
        KORB_VM(c)->method_class = cMethod;
    }
    DEF_R(KORB_VM(c)->object_class, "instance_eval",    obj_instance_eval,       -1);
    DEF_R(KORB_VM(c)->object_class, "instance_exec",    obj_instance_exec,       -1);
    DEF_R(KORB_VM(c)->module_class, "instance_method",  module_instance_method,   1);
    DEF_R(KORB_VM(c)->module_class, "instance_methods", module_instance_methods, -1);
    DEF_R(KORB_VM(c)->module_class, "method_defined?",            module_method_defined_p,           -1);
    DEF_R(KORB_VM(c)->module_class, "public_method_defined?",     module_public_method_defined_p,    -1);
    DEF_R(KORB_VM(c)->module_class, "private_method_defined?",    module_private_method_defined_p,   -1);
    DEF_R(KORB_VM(c)->module_class, "protected_method_defined?",  module_protected_method_defined_p, -1);
    DEF_R(KORB_VM(c)->module_class, "private_instance_methods",   module_private_instance_methods,   -1);
    DEF_R(KORB_VM(c)->module_class, "public_instance_methods",    module_public_instance_methods,    -1);
    DEF_R(KORB_VM(c)->module_class, "protected_instance_methods", module_protected_instance_methods, -1);
    DEF_R(KORB_VM(c)->module_class, "constants",        module_constants,         0);
    DEF_R(KORB_VM(c)->module_class, "class_eval",       module_class_eval,       -1);
    DEF_R(KORB_VM(c)->module_class, "module_eval",      module_class_eval,       -1);
    DEF_R(KORB_VM(c)->module_class, "class_exec",       module_class_exec,       -1);
    DEF_R(KORB_VM(c)->module_class, "module_exec",      module_class_exec,       -1);
    DEF_R(KORB_VM(c)->module_class, "<",                module_lt,                1);
    DEF_R(KORB_VM(c)->module_class, "<=",               module_le,                1);
    DEF_R(KORB_VM(c)->module_class, "<=>",              module_cmp,               1);
    DEF_R(KORB_VM(c)->module_class, ">",                module_gt,                1);
    DEF_R(KORB_VM(c)->module_class, ">=",               module_ge,                1);
    DEF_R(KORB_VM(c)->class_class, "superclass",       class_superclass,         0);

    /* Math module — populated with libm-backed functions and constants. */
    {
        struct korb_class *cMath = korb_module_new(c, c->sp_top, korb_intern("Math"));
        korb_const_set(KORB_VM(c)->object_class, korb_intern("Math"), (VALUE)cMath);
        /* Math::DomainError < StandardError — raised by Math.sqrt(-1) etc. */
        VALUE eStd = korb_const_get(KORB_VM(c)->object_class, korb_intern("StandardError"));
        struct korb_class *cMathDomainError = korb_class_new(c, c->sp_top, korb_intern("DomainError"),
            (eStd && !SPECIAL_CONST_P(eStd) && BUILTIN_TYPE(eStd) == T_CLASS)
                ? (struct korb_class *)eStd : NULL,
            T_OBJECT);
        korb_const_set(cMath, korb_intern("DomainError"), (VALUE)cMathDomainError);
        struct korb_class *cMathMeta = korb_class_new(c, c->sp_top, korb_intern("MathMeta"),
                                                      KORB_VM(c)->module_class, T_MODULE);
        korb_const_set(cMath, korb_intern("PI"), korb_float_new(c, c->sp_top, 3.141592653589793));
        korb_const_set(cMath, korb_intern("E"),  korb_float_new(c, c->sp_top, 2.718281828459045));
        /* Math.fn(...) calls — install on the metaclass so the lookup
         * for `Math.sqrt(2)` (recv = Math) finds them. */
        DEF_R(cMathMeta, "sqrt",  math_sqrt,  1);
        DEF_R(cMathMeta, "sin",   math_sin,   1);
        DEF_R(cMathMeta, "cos",   math_cos,   1);
        DEF_R(cMathMeta, "tan",   math_tan,   1);
        DEF_R(cMathMeta, "asin",  math_asin,  1);
        DEF_R(cMathMeta, "acos",  math_acos,  1);
        DEF_R(cMathMeta, "atan",  math_atan,  1);
        DEF_R(cMathMeta, "atan2", math_atan2, 2);
        DEF_R(cMathMeta, "sinh",  math_sinh,  1);
        DEF_R(cMathMeta, "cosh",  math_cosh,  1);
        DEF_R(cMathMeta, "tanh",  math_tanh,  1);
        DEF_R(cMathMeta, "exp",   math_exp,   1);
        DEF_R(cMathMeta, "log",   math_log,  -1);
        DEF_R(cMathMeta, "log2",  math_log2,  1);
        DEF_R(cMathMeta, "log10", math_log10, 1);
        DEF_R(cMathMeta, "cbrt",  math_cbrt,  1);
        DEF_R(cMathMeta, "hypot", math_hypot, 2);
        DEF_R(cMathMeta, "pow",   math_pow,   2);
        DEF_R(cMathMeta, "asinh", math_asinh, 1);
        DEF_R(cMathMeta, "acosh", math_acosh, 1);
        DEF_R(cMathMeta, "atanh", math_atanh, 1);
        DEF_R(cMathMeta, "erf",   math_erf,   1);
        DEF_R(cMathMeta, "erfc",  math_erfc,  1);
        DEF_R(cMathMeta, "gamma", math_gamma, 1);
        DEF_R(cMathMeta, "ldexp", math_ldexp, 2);
        DEF_R(cMathMeta, "frexp", math_frexp, 1);
        /* Math.lgamma returns [value, sign] using lgamma_r for sign. */
        {
            #include <math.h>
            RESULT _math_lgamma_pair(CTX *c, int argc, VALUE *sp) {
                c->sp_top = sp;
                int sign;
                double d;
                if (FIXNUM_P(sp[-1])) d = (double)FIX2LONG(sp[-1]);
                else d = korb_num2dbl(sp[-1]);
                double v = lgamma_r(d, &sign);
                /* Park pair at sp[0] across float_new + push (handle moves). */
                sp[0] = korb_ary_new_capa(c, sp, 2);
                korb_ary_push(c, sp + 1, sp[0], korb_float_new(c, sp + 1, v));
                korb_ary_push(c, sp + 1, sp[0], INT2FIX((long)sign));
                return RESULT_OK(sp[0]);
            }
            DEF_R(cMathMeta, "lgamma", _math_lgamma_pair, 1);
        }
        cMath->basic.klass = (VALUE)cMathMeta;
    }

    /* Module.new — install on the Module class's singleton so `Module.new {…}` works. */
    {
        struct korb_class *cModMeta = korb_class_new(c, c->sp_top, korb_intern("ModuleMeta"),
                                                     KORB_VM(c)->class_class, T_CLASS);
        DEF_R(cModMeta, "new", module_new_class_func, -1);
        /* Module.nesting — same metaclass. */
        DEF_R(cModMeta, "nesting", module_class_nesting, 0);
        KORB_VM(c)->module_class->basic.klass = (VALUE)cModMeta;
    }

    /* Exception methods — apply to Exception itself + every subclass. */
    {
        VALUE eExc = korb_const_get(KORB_VM(c)->object_class, korb_intern("Exception"));
        if (eExc && !SPECIAL_CONST_P(eExc) &&
            (BUILTIN_TYPE(eExc) == T_CLASS || BUILTIN_TYPE(eExc) == T_MODULE)) {
            struct korb_class *cExc = (struct korb_class *)eExc;
            DEF_R(cExc, "initialize", exc_initialize, -1);
            DEF_R(cExc, "message",   exc_message,   0);
            DEF_R(cExc, "to_s",      exc_to_s,      0);
            DEF_R(cExc, "inspect",   exc_inspect,   0);
            DEF_R(cExc, "backtrace", exc_backtrace, 0);
            DEF_R(cExc, "set_backtrace", exc_set_backtrace, 1);
            DEF_R(cExc, "backtrace_locations", exc_backtrace_locations, 0);
            DEF_R(cExc, "cause",     exc_cause,     0);
            DEF_R(cExc, "full_message", exc_full_message, -1);
            DEF_R(cExc, "detailed_message", exc_detailed_message, -1);
            DEF_R(cExc, "exception", exc_exception, -1);
            /* Exception.exception (class method) — same as Class.new */
            {
                RESULT _exc_class_exception(CTX *c, int argc, VALUE *sp) {
                    c->sp_top = sp;
                    VALUE self = sp[-argc - 1];
                    VALUE *argv = sp - argc;
                    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_CLASS) {
                        return RESULT_OK(Qnil);
                    }
                    /* Delegate to self.new(*argv). */
                    return korb_funcall(c, self, korb_intern("new"), argc, argv);
                }
                struct korb_class *cExcMeta = (struct korb_class *)cExc->basic.klass;
                if (cExcMeta) {
                    DEF_R(cExcMeta, "exception", _exc_class_exception, -1);
                }
            }
        }
        VALUE eNme = korb_const_get(KORB_VM(c)->object_class, korb_intern("NoMethodError"));
        if (eNme && !SPECIAL_CONST_P(eNme) &&
            (BUILTIN_TYPE(eNme) == T_CLASS || BUILTIN_TYPE(eNme) == T_MODULE)) {
            struct korb_class *cNme = (struct korb_class *)eNme;
            DEF_R(cNme, "receiver", nme_receiver, 0);
            DEF_R(cNme, "name",     nme_name,     0);
        }
        VALUE eName = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
        if (eName && !SPECIAL_CONST_P(eName) &&
            (BUILTIN_TYPE(eName) == T_CLASS || BUILTIN_TYPE(eName) == T_MODULE)) {
            struct korb_class *cName = (struct korb_class *)eName;
            DEF_R(cName, "name", nme_name, 0);
        }
        VALUE eSE = korb_const_get(KORB_VM(c)->object_class, korb_intern("SystemExit"));
        if (eSE && !SPECIAL_CONST_P(eSE) &&
            (BUILTIN_TYPE(eSE) == T_CLASS || BUILTIN_TYPE(eSE) == T_MODULE)) {
            struct korb_class *cSE = (struct korb_class *)eSE;
            DEF_R(cSE, "status",   syx_status,    0);
            DEF_R(cSE, "success?", syx_success_p, 0);
        }
    }

    /* Make sure ARGV is at least an empty array; main.c will override */
    korb_const_set(KORB_VM(c)->object_class, korb_intern("ARGV"), korb_ary_new(c, c->sp_top));
    /* ENV: populate from real environment (read-only snapshot). */
    {
        extern char **environ;
        VALUE env = korb_hash_new(c, c->sp_top);
        for (char **p = environ; *p; p++) {
            const char *eq = strchr(*p, '=');
            if (!eq) continue;
            VALUE key = korb_str_new(c, c->sp_top, *p, (size_t)(eq - *p));
            VALUE val = korb_str_new_cstr(c, c->sp_top, eq + 1);
            korb_hash_aset(c, env, key, val);
        }
        korb_const_set(KORB_VM(c)->object_class, korb_intern("ENV"), env);
    }

    /* CRuby treats these as module functions — private instance method
     * on Object/Kernel + public class method on Kernel.singleton_class.
     * Done here at the end so all DEFs have run. */
    {
        struct korb_class *cObj2 = KORB_VM(c)->object_class;
        struct korb_class *cKerMeta2 = KORB_VM(c)->kernel_module
            ? korb_singleton_class_of(c, c->sp_top, KORB_VM(c)->kernel_module) : NULL;
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
                    if (m->u.cfunc.func_r) {
                        korb_class_add_method_cfunc_r(cKerMeta2, nm,
                                                       m->u.cfunc.func_r,
                                                       m->u.cfunc.argc);
                    } else {
                        korb_class_add_method_cfunc(cKerMeta2, nm,
                                                     m->u.cfunc.func,
                                                     m->u.cfunc.argc);
                    }
                }
                /* Copy as PRIVATE to Kernel module so the spec's
                 * Kernel.private_method_defined?(:exec) returns true. */
                if (KORB_VM(c)->kernel_module &&
                    !korb_class_find_method(KORB_VM(c)->kernel_module, nm) &&
                    m->type == KORB_METHOD_CFUNC) {
                    if (m->u.cfunc.func_r) {
                        korb_class_add_method_cfunc_r(KORB_VM(c)->kernel_module, nm,
                                                       m->u.cfunc.func_r,
                                                       m->u.cfunc.argc);
                    } else {
                        korb_class_add_method_cfunc(KORB_VM(c)->kernel_module, nm,
                                                     m->u.cfunc.func,
                                                     m->u.cfunc.argc);
                    }
                    struct korb_method *m2 = korb_class_find_method(KORB_VM(c)->kernel_module, nm);
                    if (m2) m2->visibility = KORB_VIS_PRIVATE;
                }
                m->visibility = KORB_VIS_PRIVATE;
            }
            if (KORB_VM(c)->kernel_module) {
                m = korb_class_find_method(KORB_VM(c)->kernel_module, nm);
                if (m) m->visibility = KORB_VIS_PRIVATE;
            }
        }
    }
#undef cKerMeta
}
