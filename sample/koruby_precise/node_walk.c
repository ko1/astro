// This file is auto-generated from node.def.
// walker functions — visit each non-NULL NODE * child of a node.
#define WALK_node_num NULL

#define WALK_node_int_lit NULL

#define WALK_node_bignum_lit NULL

#define WALK_node_float_lit NULL

#define WALK_node_str_lit NULL

#define WALK_node_frozen_str_lit NULL

#define WALK_node_chilled_str_lit NULL

#define WALK_node_sym_lit NULL

#define WALK_node_true NULL

#define WALK_node_false NULL

#define WALK_node_nil NULL

#define WALK_node_self NULL

#define WALK_node_lvar_get NULL

static void
WALK_node_lvar_set(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_lvar_set.rhs) fn(n->u.node_lvar_set.rhs, ctx);
}

#define WALK_node_ivar_get NULL

static void
WALK_node_ivar_set(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_ivar_set.rhs) fn(n->u.node_ivar_set.rhs, ctx);
}

#define WALK_node_gvar_get NULL

static void
WALK_node_gvar_set(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_gvar_set.rhs) fn(n->u.node_gvar_set.rhs, ctx);
}

#define WALK_node_gvar_defined_p NULL

static void
WALK_node_const_path_defined(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_const_path_defined.parent) fn(n->u.node_const_path_defined.parent, ctx);
}

#define WALK_node_super_defined_p NULL

#define WALK_node_const_defined_lex NULL

#define WALK_node_last_line_get NULL

static void
WALK_node_last_line_set(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_last_line_set.rhs) fn(n->u.node_last_line_set.rhs, ctx);
}

#define WALK_node_last_match_get NULL

static void
WALK_node_last_match_set(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_last_match_set.rhs) fn(n->u.node_last_match_set.rhs, ctx);
}

#define WALK_node_cvar_get NULL

static void
WALK_node_cvar_set(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_cvar_set.rhs) fn(n->u.node_cvar_set.rhs, ctx);
}

#define WALK_node_const_get NULL

static void
WALK_node_const_set(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_const_set.rhs) fn(n->u.node_const_set.rhs, ctx);
}

static void
WALK_node_const_path_get(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_const_path_get.parent) fn(n->u.node_const_path_get.parent, ctx);
}

static void
WALK_node_scope(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_scope.body) fn(n->u.node_scope.body, ctx);
}

static void
WALK_node_seq(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_seq.head) fn(n->u.node_seq.head, ctx);
    if (n->u.node_seq.tail) fn(n->u.node_seq.tail, ctx);
}

static void
WALK_node_if(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_if.cond) fn(n->u.node_if.cond, ctx);
    if (n->u.node_if.then_node) fn(n->u.node_if.then_node, ctx);
    if (n->u.node_if.else_node) fn(n->u.node_if.else_node, ctx);
}

static void
WALK_node_while(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_while.cond) fn(n->u.node_while.cond, ctx);
    if (n->u.node_while.body) fn(n->u.node_while.body, ctx);
}

static void
WALK_node_until(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_until.cond) fn(n->u.node_until.cond, ctx);
    if (n->u.node_until.body) fn(n->u.node_until.body, ctx);
}

static void
WALK_node_return(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_return.value) fn(n->u.node_return.value, ctx);
}

static void
WALK_node_break(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_break.value) fn(n->u.node_break.value, ctx);
}

static void
WALK_node_next(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_next.value) fn(n->u.node_next.value, ctx);
}

static void
WALK_node_not(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_not.expr) fn(n->u.node_not.expr, ctx);
}

static void
WALK_node_and(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_and.lhs) fn(n->u.node_and.lhs, ctx);
    if (n->u.node_and.rhs) fn(n->u.node_and.rhs, ctx);
}

static void
WALK_node_or(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_or.lhs) fn(n->u.node_or.lhs, ctx);
    if (n->u.node_or.rhs) fn(n->u.node_or.rhs, ctx);
}

#define WALK_node_ary_new NULL

#define WALK_node_hash_new NULL

static void
WALK_node_range_new(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_range_new.begin) fn(n->u.node_range_new.begin, ctx);
    if (n->u.node_range_new.end) fn(n->u.node_range_new.end, ctx);
}

#define WALK_node_str_concat NULL

static void
WALK_node_def(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_def.body) fn(n->u.node_def.body, ctx);
}

static void
WALK_node_class_def(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_class_def.super_expr) fn(n->u.node_class_def.super_expr, ctx);
    if (n->u.node_class_def.body) fn(n->u.node_class_def.body, ctx);
}

static void
WALK_node_class_reopen(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_class_reopen.body) fn(n->u.node_class_reopen.body, ctx);
}

static void
WALK_node_module_def(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_module_def.body) fn(n->u.node_module_def.body, ctx);
}

#define WALK_node_func_call NULL

static void
WALK_node_method_call(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_method_call.recv) fn(n->u.node_method_call.recv, ctx);
}

static void
WALK_node_case_eqq_call(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_case_eqq_call.recv) fn(n->u.node_case_eqq_call.recv, ctx);
}

static void
WALK_node_method_call_block(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_method_call_block.recv) fn(n->u.node_method_call_block.recv, ctx);
    if (n->u.node_method_call_block.blk) fn(n->u.node_method_call_block.blk, ctx);
}

static void
WALK_node_func_call_block(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_func_call_block.blk) fn(n->u.node_func_call_block.blk, ctx);
}

static void
WALK_node_block_literal(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_block_literal.body) fn(n->u.node_block_literal.body, ctx);
}

static void
WALK_node_block_literal_rest(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_block_literal_rest.body) fn(n->u.node_block_literal_rest.body, ctx);
}

static void
WALK_node_block_literal_kw(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_block_literal_kw.body) fn(n->u.node_block_literal_kw.body, ctx);
}

static void
WALK_node_proc_set_opt_cnt(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_proc_set_opt_cnt.blk_node) fn(n->u.node_proc_set_opt_cnt.blk_node, ctx);
}

static void
WALK_node_proc_set_block_slot(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_proc_set_block_slot.blk_node) fn(n->u.node_proc_set_block_slot.blk_node, ctx);
}

static void
WALK_node_proc_set_post_cnt(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_proc_set_post_cnt.blk_node) fn(n->u.node_proc_set_post_cnt.blk_node, ctx);
}

static void
WALK_node_proc_set_implicit_rest(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_proc_set_implicit_rest.blk_node) fn(n->u.node_proc_set_implicit_rest.blk_node, ctx);
}

static void
WALK_node_proc_make_lambda(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_proc_make_lambda.blk_node) fn(n->u.node_proc_make_lambda.blk_node, ctx);
}

static void
WALK_node_hash_mark_kwargs(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_hash_mark_kwargs.hash_expr) fn(n->u.node_hash_mark_kwargs.hash_expr, ctx);
}

#define WALK_node_yield NULL

#define WALK_node_yield_splat NULL

static void
WALK_node_plus(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_plus.lhs) fn(n->u.node_plus.lhs, ctx);
    if (n->u.node_plus.rhs) fn(n->u.node_plus.rhs, ctx);
}

static void
WALK_node_minus(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_minus.lhs) fn(n->u.node_minus.lhs, ctx);
    if (n->u.node_minus.rhs) fn(n->u.node_minus.rhs, ctx);
}

static void
WALK_node_mul(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_mul.lhs) fn(n->u.node_mul.lhs, ctx);
    if (n->u.node_mul.rhs) fn(n->u.node_mul.rhs, ctx);
}

static void
WALK_node_div(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_div.lhs) fn(n->u.node_div.lhs, ctx);
    if (n->u.node_div.rhs) fn(n->u.node_div.rhs, ctx);
}

static void
WALK_node_mod(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_mod.lhs) fn(n->u.node_mod.lhs, ctx);
    if (n->u.node_mod.rhs) fn(n->u.node_mod.rhs, ctx);
}

static void
WALK_node_uminus(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_uminus.operand) fn(n->u.node_uminus.operand, ctx);
}

static void
WALK_node_bit_and(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_bit_and.lhs) fn(n->u.node_bit_and.lhs, ctx);
    if (n->u.node_bit_and.rhs) fn(n->u.node_bit_and.rhs, ctx);
}

static void
WALK_node_bit_or(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_bit_or.lhs) fn(n->u.node_bit_or.lhs, ctx);
    if (n->u.node_bit_or.rhs) fn(n->u.node_bit_or.rhs, ctx);
}

static void
WALK_node_bit_xor(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_bit_xor.lhs) fn(n->u.node_bit_xor.lhs, ctx);
    if (n->u.node_bit_xor.rhs) fn(n->u.node_bit_xor.rhs, ctx);
}

static void
WALK_node_lshift(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_lshift.lhs) fn(n->u.node_lshift.lhs, ctx);
    if (n->u.node_lshift.rhs) fn(n->u.node_lshift.rhs, ctx);
}

static void
WALK_node_rshift(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_rshift.lhs) fn(n->u.node_rshift.lhs, ctx);
    if (n->u.node_rshift.rhs) fn(n->u.node_rshift.rhs, ctx);
}

static void
WALK_node_lt(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_lt.lhs) fn(n->u.node_lt.lhs, ctx);
    if (n->u.node_lt.rhs) fn(n->u.node_lt.rhs, ctx);
}

static void
WALK_node_le(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_le.lhs) fn(n->u.node_le.lhs, ctx);
    if (n->u.node_le.rhs) fn(n->u.node_le.rhs, ctx);
}

static void
WALK_node_gt(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_gt.lhs) fn(n->u.node_gt.lhs, ctx);
    if (n->u.node_gt.rhs) fn(n->u.node_gt.rhs, ctx);
}

static void
WALK_node_ge(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_ge.lhs) fn(n->u.node_ge.lhs, ctx);
    if (n->u.node_ge.rhs) fn(n->u.node_ge.rhs, ctx);
}

static void
WALK_node_eq(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_eq.lhs) fn(n->u.node_eq.lhs, ctx);
    if (n->u.node_eq.rhs) fn(n->u.node_eq.rhs, ctx);
}

static void
WALK_node_neq(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_neq.lhs) fn(n->u.node_neq.lhs, ctx);
    if (n->u.node_neq.rhs) fn(n->u.node_neq.rhs, ctx);
}

static void
WALK_node_aref(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_aref.recv) fn(n->u.node_aref.recv, ctx);
    if (n->u.node_aref.idx) fn(n->u.node_aref.idx, ctx);
}

static void
WALK_node_aset(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_aset.recv) fn(n->u.node_aset.recv, ctx);
    if (n->u.node_aset.idx) fn(n->u.node_aset.idx, ctx);
    if (n->u.node_aset.val) fn(n->u.node_aset.val, ctx);
}

static void
WALK_node_aset_splat(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_aset_splat.recv) fn(n->u.node_aset_splat.recv, ctx);
    if (n->u.node_aset_splat.splat) fn(n->u.node_aset_splat.splat, ctx);
    if (n->u.node_aset_splat.val) fn(n->u.node_aset_splat.val, ctx);
}

static void
WALK_node_rescue(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_rescue.body) fn(n->u.node_rescue.body, ctx);
    if (n->u.node_rescue.rescue_body) fn(n->u.node_rescue.rescue_body, ctx);
}

#define WALK_node_retry NULL

static void
WALK_node_rescue_else(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_rescue_else.body) fn(n->u.node_rescue_else.body, ctx);
    if (n->u.node_rescue_else.rescue_body) fn(n->u.node_rescue_else.rescue_body, ctx);
    if (n->u.node_rescue_else.else_body) fn(n->u.node_rescue_else.else_body, ctx);
}

#define WALK_node_redo NULL

static void
WALK_node_raise(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_raise.exc_expr) fn(n->u.node_raise.exc_expr, ctx);
}

static void
WALK_node_to_ary_for_mlhs(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_to_ary_for_mlhs.expr) fn(n->u.node_to_ary_for_mlhs.expr, ctx);
}

static void
WALK_node_splat_to_ary(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_splat_to_ary.expr) fn(n->u.node_splat_to_ary.expr, ctx);
}

static void
WALK_node_ary_aget(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_ary_aget.ary_node) fn(n->u.node_ary_aget.ary_node, ctx);
}

static void
WALK_node_ary_aget_neg(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_ary_aget_neg.ary_node) fn(n->u.node_ary_aget_neg.ary_node, ctx);
}

static void
WALK_node_ary_aget_right(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_ary_aget_right.ary_node) fn(n->u.node_ary_aget_right.ary_node, ctx);
}

static void
WALK_node_ary_slice_middle(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_ary_slice_middle.ary_node) fn(n->u.node_ary_slice_middle.ary_node, ctx);
}

static void
WALK_node_str_to_sym(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_str_to_sym.str_node) fn(n->u.node_str_to_sym.str_node, ctx);
}

#define WALK_node_super NULL

static void
WALK_node_super_block(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_super_block.blk_expr) fn(n->u.node_super_block.blk_expr, ctx);
}

static void
WALK_node_super_forward_block(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_super_forward_block.blk_expr) fn(n->u.node_super_forward_block.blk_expr, ctx);
}

#define WALK_node_super_forward NULL

static void
WALK_node_ensure(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_ensure.body) fn(n->u.node_ensure.body, ctx);
    if (n->u.node_ensure.ensure_body) fn(n->u.node_ensure.ensure_body, ctx);
}

static void
WALK_node_singleton_def(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_singleton_def.body) fn(n->u.node_singleton_def.body, ctx);
}

static void
WALK_node_singleton_def_post(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_singleton_def_post.body) fn(n->u.node_singleton_def_post.body, ctx);
}

static void
WALK_node_obj_singleton_def(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_obj_singleton_def.recv_expr) fn(n->u.node_obj_singleton_def.recv_expr, ctx);
    if (n->u.node_obj_singleton_def.body) fn(n->u.node_obj_singleton_def.body, ctx);
}

static void
WALK_node_obj_singleton_def_post(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_obj_singleton_def_post.recv_expr) fn(n->u.node_obj_singleton_def_post.recv_expr, ctx);
    if (n->u.node_obj_singleton_def_post.body) fn(n->u.node_obj_singleton_def_post.body, ctx);
}

static void
WALK_node_singleton_class_body(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_singleton_class_body.recv_expr) fn(n->u.node_singleton_class_body.recv_expr, ctx);
    if (n->u.node_singleton_class_body.body) fn(n->u.node_singleton_class_body.body, ctx);
}

static void
WALK_node_default_init(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_default_init.default_expr) fn(n->u.node_default_init.default_expr, ctx);
}

static void
WALK_node_def_full(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_def_full.body) fn(n->u.node_def_full.body, ctx);
}

static void
WALK_node_def_post(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_def_post.body) fn(n->u.node_def_post.body, ctx);
}

#define WALK_node_set_kwh_save_slot NULL

static void
WALK_node_alias_method(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_alias_method.new_name) fn(n->u.node_alias_method.new_name, ctx);
    if (n->u.node_alias_method.old_name) fn(n->u.node_alias_method.old_name, ctx);
}

static void
WALK_node_ary_concat(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_ary_concat.lhs) fn(n->u.node_ary_concat.lhs, ctx);
    if (n->u.node_ary_concat.rhs) fn(n->u.node_ary_concat.rhs, ctx);
}

static void
WALK_node_apply_call(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_apply_call.recv) fn(n->u.node_apply_call.recv, ctx);
    if (n->u.node_apply_call.args_node) fn(n->u.node_apply_call.args_node, ctx);
    if (n->u.node_apply_call.blk) fn(n->u.node_apply_call.blk, ctx);
}

static void
WALK_node_class_def_in_strict(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_class_def_in_strict.parent_expr) fn(n->u.node_class_def_in_strict.parent_expr, ctx);
    if (n->u.node_class_def_in_strict.super_expr) fn(n->u.node_class_def_in_strict.super_expr, ctx);
    if (n->u.node_class_def_in_strict.body) fn(n->u.node_class_def_in_strict.body, ctx);
}

static void
WALK_node_class_def_in(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_class_def_in.parent_expr) fn(n->u.node_class_def_in.parent_expr, ctx);
    if (n->u.node_class_def_in.super_expr) fn(n->u.node_class_def_in.super_expr, ctx);
    if (n->u.node_class_def_in.body) fn(n->u.node_class_def_in.body, ctx);
}

static void
WALK_node_module_def_in(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_module_def_in.parent_expr) fn(n->u.node_module_def_in.parent_expr, ctx);
    if (n->u.node_module_def_in.body) fn(n->u.node_module_def_in.body, ctx);
}

static void
WALK_node_do_while(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_do_while.cond) fn(n->u.node_do_while.cond, ctx);
    if (n->u.node_do_while.body) fn(n->u.node_do_while.body, ctx);
}

static void
WALK_node_do_until(NODE *n, void (*fn)(NODE *, void *), void *ctx)
{
    if (n->u.node_do_until.cond) fn(n->u.node_do_until.cond, ctx);
    if (n->u.node_do_until.body) fn(n->u.node_do_until.body, ctx);
}

