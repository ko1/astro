/* koruby builtin methods */

#include <stdio.h>
#include <string.h>
#include <math.h>
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
#define DEF(klass, name, fn, argc) \
    korb_class_add_method_cfunc((klass), korb_intern(name), (fn), (argc))

void korb_init_builtins(void) {
    /* Object methods */
    struct korb_class *cObj = korb_vm->object_class;
    DEF(cObj, "p", kernel_p, -1);
    DEF(cObj, "puts", kernel_puts, -1);
    DEF(cObj, "print", kernel_print, -1);
    DEF(cObj, "raise", kernel_raise, -1);
    DEF(cObj, "inspect", kernel_inspect, 0);
    DEF(cObj, "to_s", kernel_to_s, 0);
    DEF(cObj, "class", kernel_class, 0);
    DEF(cObj, "==", kernel_eq, 1);
    DEF(cObj, "!=", kernel_neq, 1);
    DEF(cObj, "!", kernel_not, 0);
    DEF(cObj, "nil?", kernel_nil_p, 0);
    DEF(cObj, "object_id", kernel_object_id, 0);
    DEF(cObj, "equal?", kernel_eq, 1);  /* same as == for now */
    DEF(cObj, "freeze", kernel_freeze, 0);
    DEF(cObj, "frozen?", kernel_frozen_p, 0);
    DEF(cObj, "respond_to?", kernel_respond_to_p, 1);
    DEF(cObj, "is_a?", kernel_is_a_p, 1);
    DEF(cObj, "kind_of?", kernel_is_a_p, 1);
    DEF(cObj, "block_given?", kernel_block_given, 0);
    DEF(cObj, "require_relative", kernel_require_relative, 1);
    DEF(cObj, "require", kernel_require, 1);
    DEF(cObj, "__dir__", kernel_dir, 0);
    DEF(cObj, "load", kernel_load, -1);
    DEF(cObj, "abort", kernel_abort, -1);
    DEF(cObj, "exit", kernel_exit, -1);
    DEF(cObj, "Integer", kernel_integer, -1);
    DEF(cObj, "Float", kernel_float, 1);
    DEF(cObj, "String", kernel_string, 1);
    DEF(cObj, "Array", kernel_array, 1);

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

    /* Float */
    struct korb_class *cFlt = korb_vm->float_class;
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

    /* String */
    struct korb_class *cStr = korb_vm->string_class;
    DEF(cStr, "+", str_plus, 1);
    DEF(cStr, "<<", str_concat, 1);
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

    /* Array */
    struct korb_class *cAry = korb_vm->array_class;
    DEF(cAry, "size", ary_size, 0);
    DEF(cAry, "length", ary_size, 0);
    DEF(cAry, "[]", ary_aref, -1);
    DEF(cAry, "[]=", ary_aset, -1);
    DEF(cAry, "push", ary_push, -1);
    DEF(cAry, "<<", ary_lshift, 1);
    DEF(cAry, "pop", ary_pop, 0);
    DEF(cAry, "first", ary_first, 0);
    DEF(cAry, "last", ary_last, 0);
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

    /* Hash */
    struct korb_class *cHsh = korb_vm->hash_class;
    DEF(cHsh, "[]", hash_aref, 1);
    DEF(cHsh, "[]=", hash_aset, 2);
    DEF(cHsh, "size", hash_size, 0);
    DEF(cHsh, "length", hash_size, 0);
    DEF(cHsh, "each", hash_each, 0);

    /* Range */
    struct korb_class *cRng = korb_vm->range_class;
    DEF(cRng, "each", rng_each, 0);
    DEF(cRng, "first", rng_first, 0);
    DEF(cRng, "last", rng_last, 0);
    DEF(cRng, "to_a", rng_to_a, 0);

    /* Class */
    struct korb_class *cCls = korb_vm->class_class;
    DEF(cCls, "new", class_new, -1);
    DEF(cCls, "name", class_name, 0);

    /* Module — applies to both Class and Module */
    struct korb_class *cMod = korb_vm->module_class;
    DEF(cMod, "attr_reader",   module_attr_reader,   -1);
    DEF(cMod, "attr_writer",   module_attr_writer,   -1);
    DEF(cMod, "attr_accessor", module_attr_accessor, -1);
    DEF(cMod, "include",       module_include,       -1);
    DEF(cMod, "private",       module_private,       -1);
    DEF(cMod, "public",        module_public,        -1);
    DEF(cMod, "protected",     module_protected,     -1);
    DEF(cMod, "module_function", module_module_function, -1);
    DEF(cMod, "define_method", module_define_method, -1);
    DEF(cMod, "alias_method",  module_alias_method,  -1);
    DEF(cMod, "const_get",     module_const_get,     -1);
    DEF(cMod, "const_set",     module_const_set,     -1);
    DEF(cMod, "const_defined?", module_const_defined_p, -1);
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
    DEF(cObj, "clone",              obj_dup,                   0);
    DEF(cObj, "instance_variables", obj_instance_variables,    0);
    DEF(cObj, "instance_variable_defined?", obj_ivar_defined_p, 1);
    /* Kernel#__method__, caller, eval, loop, lambda, proc */
    DEF(cObj, "__method__",         kernel_method_name,        0);
    DEF(cObj, "__callee__",         kernel_method_name,        0);
    DEF(cObj, "caller",             kernel_caller,            -1);
    DEF(cObj, "eval",               kernel_eval_stub,         -1);
    DEF(cObj, "loop",               kernel_loop,               0);
    DEF(cObj, "lambda",             kernel_lambda,             0);
    DEF(cObj, "proc",               kernel_proc,               0);
    /* Range#exclude_end? */
    DEF(cRng, "exclude_end?",       rng_exclude_end_p,         0);
    /* Class ancestors / Module#prepend */
    DEF(cMod, "ancestors",          class_ancestors,           0);
    DEF(cMod, "prepend",            module_prepend,           -1);
    DEF(cObj, "extend",             obj_extend,               -1);
    DEF(cObj, "send",                  obj_send,                 -1);
    DEF(cObj, "__send__",              obj_send,                 -1);
    DEF(cObj, "public_send",           obj_send,                 -1);
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
    DEF(cObj, "format",                kernel_format,            -1);
    DEF(cObj, "sprintf",               kernel_format,            -1);
    DEF(cObj, "printf",                kernel_printf,            -1);

    /* extra Integer */
    DEF(cInt, "chr",   int_chr, 0);
    DEF(cInt, "===",   int_eqq, 1);
    DEF(cInt, "floor", int_floor, -1);
    DEF(cInt, "ceil",  int_floor, -1);
    DEF(cInt, "round", int_floor, -1);
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
    DEF(cFlt, "abs",   flt_abs, 0);
    DEF(cFlt, "ceil",     flt_ceil,    -1);
    DEF(cFlt, "round",    flt_round,   -1);
    DEF(cFlt, "truncate", flt_truncate, 0);

    /* extra String */
    DEF(cStr, "split",       str_split,       -1);
    DEF(cStr, "chomp",       str_chomp,       -1);
    DEF(cStr, "strip",       str_strip,        0);
    DEF(cStr, "to_i",        str_to_i,        -1);
    DEF(cStr, "to_f",        str_to_f,         0);
    DEF(cStr, "[]",          str_aref,        -1);
    DEF(cStr, "[]=",         str_aset,        -1);
    DEF(cStr, "index",       str_index,       -1);
    DEF(cStr, "rindex",      str_rindex,      -1);
    DEF(cStr, "chars",       str_chars,        0);
    DEF(cStr, "bytes",       str_bytes,        0);
    DEF(cStr, "each_char",   str_each_char,    0);
    DEF(cStr, "each_line",   str_split,       -1); /* approximate */
    DEF(cStr, "start_with?", str_start_with,  -1);
    DEF(cStr, "end_with?",   str_end_with,    -1);
    DEF(cStr, "include?",    str_include,     -1);
    DEF(cStr, "replace",     str_replace,      1);
    DEF(cStr, "reverse",     str_reverse,      0);
    DEF(cStr, "upcase",      str_upcase,       0);
    DEF(cStr, "downcase",    str_downcase,     0);
    DEF(cStr, "empty?",      str_empty_p,      0);
    DEF(cStr, "*",           str_mul,          1);
    DEF(cStr, "hash",        str_hash,         0);
    DEF(cStr, "===",         str_eqq,          1);
    DEF(cStr, "gsub",        str_gsub,        -1);
    DEF(cStr, "sub",         str_sub,         -1);
    DEF(cStr, "tr",          str_tr,          -1);
    DEF(cStr, "tr_s",        str_tr,          -1);
    DEF(cStr, "%",           str_percent,     -1);
    DEF(cStr, "inspect",     kernel_inspect,   0);
    DEF(cStr, "dup",         obj_dup,          0);
    DEF(cStr, "=~",          str_match_op, 1);
    DEF(cStr, "match?",      str_match_p, -1);
    DEF(cStr, "match",       str_match, -1);
    DEF(cStr, "scan",        str_scan, 1);
    DEF(cStr, "sum",         str_sum, -1);
    DEF(cStr, "unpack",      str_unpack, -1);

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
    DEF(cAry, "min",        ary_min,        -1);
    DEF(cAry, "max",        ary_max,        -1);
    DEF(cAry, "sum",        ary_sum,        -1);
    DEF(cAry, "each_slice", ary_each_slice,  1);
    DEF(cAry, "step",       ary_step,       -1);
    DEF(cAry, "===",        ary_eqq,         1);
    DEF(cAry, "pack",       ary_pack,       -1);
    DEF(cAry, "concat",     ary_concat,     -1);
    DEF(cAry, "-",          ary_minus,       1);
    DEF(cAry, "+",          ary_concat,     -1);
    DEF(cAry, "index",      ary_index,      -1);
    DEF(cAry, "find_index", ary_index,      -1);
    DEF(cAry, "reverse",    ary_reverse,     0);
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
    DEF(cAry, "sort!",     ary_sort, -1);
    DEF(cAry, "compact!",  ary_compact, 0);
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
    DEF(cAry, "flat_map",  ary_map, 0);   /* simplified: same as map for shallow */
    DEF(cAry, "collect_concat", ary_map, 0);
    DEF(cAry, "assoc",       ary_assoc,       1);
    DEF(cAry, "rassoc",      ary_rassoc,      1);
    DEF(cAry, "at",          ary_at,          1);
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
        cAry->basic.klass = (VALUE)cAryMeta;
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
    DEF(cHsh, "reject!",     hash_delete_if,    0);
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
    DEF(cHsh, "default",      hash_default_get,      0);
    DEF(cHsh, "default=",     hash_default_set,      1);
    DEF(cHsh, "default_proc", hash_default_proc_get, 0);
    {
        /* Override Class.new on Hash's metaclass so Hash.new(default) and
         * Hash.new { ... } actually create a real hash with the default. */
        struct korb_class *cHshMeta = korb_class_new(korb_intern("HashMeta"),
                                                      korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cHshMeta, korb_intern("new"), hash_class_new, -1);
        cHsh->basic.klass = (VALUE)cHshMeta;
    }
    DEF(cHsh, "===",        hash_eqq,        1);
    DEF(cHsh, "dup",        hash_dup,        0);
    DEF(cHsh, "clone",      hash_dup,        0);
    DEF(cHsh, "empty?",     hash_empty_p,    0);
    DEF(cHsh, "map",        hash_map,        0);
    DEF(cHsh, "collect",    hash_map,        0);
    DEF(cHsh, "select",     hash_select,     0);
    DEF(cHsh, "filter",     hash_select,     0);
    DEF(cHsh, "reduce",     hash_reduce,    -1);
    DEF(cHsh, "inject",     hash_reduce,    -1);

    /* extra Range */
    DEF(cRng, "step",     rng_step,    -1);
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

    /* File class */
    struct korb_class *cFile = korb_class_new(korb_intern("File"), korb_vm->object_class, T_OBJECT);
    korb_const_set(korb_vm->object_class, korb_intern("File"), (VALUE)cFile);
    {
        struct korb_class *cFileMeta = korb_class_new(korb_intern("FileMeta"), korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("read"), file_read, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("join"), file_join, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("exist?"), file_exist_p, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("exists?"), file_exist_p, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("dirname"), file_dirname, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("basename"), file_basename, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("expand_path"), file_expand_path, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("extname"), file_extname, 1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("binread"), file_binread, 1);
        cFile->basic.klass = (VALUE)cFileMeta;
    }

    /* IO / STDOUT / $stdout */
    struct korb_class *cIO = korb_class_new(korb_intern("IO"), korb_vm->object_class, T_OBJECT);
    korb_const_set(korb_vm->object_class, korb_intern("IO"), (VALUE)cIO);
    /* dummy STDOUT/STDERR */
    VALUE stdout_obj = korb_object_new(cIO);
    VALUE stderr_obj = korb_object_new(cIO);
    g_stderr_obj = stderr_obj;
    korb_const_set(korb_vm->object_class, korb_intern("STDOUT"), stdout_obj);
    korb_const_set(korb_vm->object_class, korb_intern("STDERR"), stderr_obj);
    korb_const_set(korb_vm->object_class, korb_intern("STDIN"), korb_object_new(cIO));
    /* IO#puts / write methods */
    korb_class_add_method_cfunc(cIO, korb_intern("puts"), kernel_puts, -1);
    korb_class_add_method_cfunc(cIO, korb_intern("print"), kernel_print, -1);
    korb_class_add_method_cfunc(cIO, korb_intern("write"), kernel_print, -1);
    korb_class_add_method_cfunc(cIO, korb_intern("flush"), kernel_inspect, 0);
    korb_class_add_method_cfunc(cIO, korb_intern("sync="), kernel_inspect, 1);

    /* gvars */
    korb_gvar_set(korb_intern("$stdout"), stdout_obj);
    korb_gvar_set(korb_intern("$stderr"), stderr_obj);

    /* Symbol */
    struct korb_class *cSym = korb_vm->symbol_class;
    DEF(cSym, "to_s", sym_to_s, 0);
    DEF(cSym, "==", sym_eq, 1);
    DEF(cSym, "to_proc", sym_to_proc, 0);
    DEF(cSym, "===", sym_eq, 1);
    DEF(cSym, "inspect", kernel_inspect, 0);
    DEF(cSym, "<=>",     sym_cmp,        1);
    DEF(cSym, "size",    sym_length,     0);
    DEF(cSym, "length",  sym_length,     0);
    DEF(cSym, "empty?",  sym_empty_p,    0);
    DEF(cSym, "upcase",  sym_upcase,     0);
    DEF(cSym, "downcase",sym_downcase,   0);

    /* Boolean / Nil */
    DEF(korb_vm->true_class, "to_s", true_to_s, 0);
    DEF(korb_vm->false_class, "to_s", false_to_s, 0);
    DEF(korb_vm->nil_class, "to_s", nil_to_s, 0);
    DEF(korb_vm->nil_class, "inspect", nil_inspect, 0);

    /* Proc */
    struct korb_class *cPrc = korb_vm->proc_class;
    DEF(cPrc, "call", proc_call, -1);
    DEF(cPrc, "[]", proc_call, -1);
    {
        VALUE obj_itself(CTX *c, VALUE self, int argc, VALUE *argv);
        DEF(cPrc, "to_proc", obj_itself, 0);
    }

    /* Process / Time stub modules */
    struct korb_class *cProcess = korb_module_new(korb_intern("Process"));
    korb_const_set(korb_vm->object_class, korb_intern("Process"), (VALUE)cProcess);
    {
        /* Process.clock_gettime, etc. — stubs returning 0.0 */
        extern VALUE proc_clock_gettime_stub(CTX *c, VALUE self, int argc, VALUE *argv);
        korb_class_add_method_cfunc(cProcess, korb_intern("clock_gettime"), proc_clock_gettime_stub, -1);
        korb_const_set(cProcess, korb_intern("CLOCK_MONOTONIC"), INT2FIX(1));
    }

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

    /* Method class — instances are returned by Object#method */
    {
        struct korb_class *cMethod = korb_class_new(korb_intern("Method"), korb_vm->object_class, T_DATA);
        korb_const_set(korb_vm->object_class, korb_intern("Method"), (VALUE)cMethod);
        korb_class_add_method_cfunc(cMethod, korb_intern("call"),     method_call,     -1);
        korb_class_add_method_cfunc(cMethod, korb_intern("[]"),       method_call,     -1);
        korb_class_add_method_cfunc(cMethod, korb_intern("to_proc"),  method_to_proc,   0);
        korb_class_add_method_cfunc(cMethod, korb_intern("arity"),    method_arity,     0);
        korb_class_add_method_cfunc(cMethod, korb_intern("name"),     method_name,      0);
        korb_class_add_method_cfunc(cMethod, korb_intern("receiver"), method_receiver,  0);
        korb_class_add_method_cfunc(cMethod, korb_intern("owner"),    method_owner,     0);
        korb_vm->method_class = cMethod;
    }

    /* Math module — populated with libm-backed functions and constants. */
    {
        struct korb_class *cMath = korb_module_new(korb_intern("Math"));
        korb_const_set(korb_vm->object_class, korb_intern("Math"), (VALUE)cMath);
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
}
