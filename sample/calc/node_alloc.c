// This file is auto-generated from node.def.

// kinds
const struct NodeKind kind_node_num = {
    .default_dispatcher_name = "DISPATCH_node_num",
    .default_dispatcher = DISPATCH_node_num,
    .hash_func = HASH_node_num,
    .specializer = SPECIALIZE_node_num,
    .dumper = DUMP_node_num,
    .replacer = REPLACER_node_num,
};

const struct NodeKind kind_node_add = {
    .default_dispatcher_name = "DISPATCH_node_add",
    .default_dispatcher = DISPATCH_node_add,
    .hash_func = HASH_node_add,
    .specializer = SPECIALIZE_node_add,
    .dumper = DUMP_node_add,
    .replacer = REPLACER_node_add,
};

const struct NodeKind kind_node_sub = {
    .default_dispatcher_name = "DISPATCH_node_sub",
    .default_dispatcher = DISPATCH_node_sub,
    .hash_func = HASH_node_sub,
    .specializer = SPECIALIZE_node_sub,
    .dumper = DUMP_node_sub,
    .replacer = REPLACER_node_sub,
};

const struct NodeKind kind_node_mul = {
    .default_dispatcher_name = "DISPATCH_node_mul",
    .default_dispatcher = DISPATCH_node_mul,
    .hash_func = HASH_node_mul,
    .specializer = SPECIALIZE_node_mul,
    .dumper = DUMP_node_mul,
    .replacer = REPLACER_node_mul,
};

const struct NodeKind kind_node_div = {
    .default_dispatcher_name = "DISPATCH_node_div",
    .default_dispatcher = DISPATCH_node_div,
    .hash_func = HASH_node_div,
    .specializer = SPECIALIZE_node_div,
    .dumper = DUMP_node_div,
    .replacer = REPLACER_node_div,
};

const struct NodeKind kind_node_mod = {
    .default_dispatcher_name = "DISPATCH_node_mod",
    .default_dispatcher = DISPATCH_node_mod,
    .hash_func = HASH_node_mod,
    .specializer = SPECIALIZE_node_mod,
    .dumper = DUMP_node_mod,
    .replacer = REPLACER_node_mod,
};

// allocators

NODE *
ALLOC_node_num(int32_t num) {
    NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct node_num_struct));
    _n->head.dispatcher = DISPATCH_node_num;
    _n->head.dispatcher_name = "DISPATCH_node_num";
    _n->head.kind = &kind_node_num;
    _n->head.parent = NULL;
    _n->head.jit_status = JIT_STATUS_Unknown;
    _n->head.dispatch_cnt = 0;
    _n->head.flags.has_hash_value = false;
    _n->head.flags.is_specialized = false;
    _n->head.flags.is_specializing = false;
    _n->head.flags.is_dumping = false;
    _n->head.flags.no_inline = false;
    _n->u.node_num.num = num;


    OPTIMIZE(_n);
    if (OPTION.record_all) code_repo_add(NULL, _n, false);
    return _n;
}

NODE *
ALLOC_node_add(NODE * lv, NODE * rv) {
    NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct node_add_struct));
    _n->head.dispatcher = DISPATCH_node_add;
    _n->head.dispatcher_name = "DISPATCH_node_add";
    _n->head.kind = &kind_node_add;
    _n->head.parent = NULL;
    _n->head.jit_status = JIT_STATUS_Unknown;
    _n->head.dispatch_cnt = 0;
    _n->head.flags.has_hash_value = false;
    _n->head.flags.is_specialized = false;
    _n->head.flags.is_specializing = false;
    _n->head.flags.is_dumping = false;
    _n->head.flags.no_inline = false;
    _n->u.node_add.lv = lv;
    _n->u.node_add.rv = rv;
    if (_n->u.node_add.lv) {_n->u.node_add.lv->head.parent = _n;}
    if (_n->u.node_add.rv) {_n->u.node_add.rv->head.parent = _n;}

    OPTIMIZE(_n);
    if (OPTION.record_all) code_repo_add(NULL, _n, false);
    return _n;
}

NODE *
ALLOC_node_sub(NODE * lv, NODE * rv) {
    NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct node_sub_struct));
    _n->head.dispatcher = DISPATCH_node_sub;
    _n->head.dispatcher_name = "DISPATCH_node_sub";
    _n->head.kind = &kind_node_sub;
    _n->head.parent = NULL;
    _n->head.jit_status = JIT_STATUS_Unknown;
    _n->head.dispatch_cnt = 0;
    _n->head.flags.has_hash_value = false;
    _n->head.flags.is_specialized = false;
    _n->head.flags.is_specializing = false;
    _n->head.flags.is_dumping = false;
    _n->head.flags.no_inline = false;
    _n->u.node_sub.lv = lv;
    _n->u.node_sub.rv = rv;
    if (_n->u.node_sub.lv) {_n->u.node_sub.lv->head.parent = _n;}
    if (_n->u.node_sub.rv) {_n->u.node_sub.rv->head.parent = _n;}

    OPTIMIZE(_n);
    if (OPTION.record_all) code_repo_add(NULL, _n, false);
    return _n;
}

NODE *
ALLOC_node_mul(NODE * lv, NODE * rv) {
    NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct node_mul_struct));
    _n->head.dispatcher = DISPATCH_node_mul;
    _n->head.dispatcher_name = "DISPATCH_node_mul";
    _n->head.kind = &kind_node_mul;
    _n->head.parent = NULL;
    _n->head.jit_status = JIT_STATUS_Unknown;
    _n->head.dispatch_cnt = 0;
    _n->head.flags.has_hash_value = false;
    _n->head.flags.is_specialized = false;
    _n->head.flags.is_specializing = false;
    _n->head.flags.is_dumping = false;
    _n->head.flags.no_inline = false;
    _n->u.node_mul.lv = lv;
    _n->u.node_mul.rv = rv;
    if (_n->u.node_mul.lv) {_n->u.node_mul.lv->head.parent = _n;}
    if (_n->u.node_mul.rv) {_n->u.node_mul.rv->head.parent = _n;}

    OPTIMIZE(_n);
    if (OPTION.record_all) code_repo_add(NULL, _n, false);
    return _n;
}

NODE *
ALLOC_node_div(NODE * lv, NODE * rv) {
    NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct node_div_struct));
    _n->head.dispatcher = DISPATCH_node_div;
    _n->head.dispatcher_name = "DISPATCH_node_div";
    _n->head.kind = &kind_node_div;
    _n->head.parent = NULL;
    _n->head.jit_status = JIT_STATUS_Unknown;
    _n->head.dispatch_cnt = 0;
    _n->head.flags.has_hash_value = false;
    _n->head.flags.is_specialized = false;
    _n->head.flags.is_specializing = false;
    _n->head.flags.is_dumping = false;
    _n->head.flags.no_inline = false;
    _n->u.node_div.lv = lv;
    _n->u.node_div.rv = rv;
    if (_n->u.node_div.lv) {_n->u.node_div.lv->head.parent = _n;}
    if (_n->u.node_div.rv) {_n->u.node_div.rv->head.parent = _n;}

    OPTIMIZE(_n);
    if (OPTION.record_all) code_repo_add(NULL, _n, false);
    return _n;
}

NODE *
ALLOC_node_mod(NODE * lv, NODE * rv) {
    NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct node_mod_struct));
    _n->head.dispatcher = DISPATCH_node_mod;
    _n->head.dispatcher_name = "DISPATCH_node_mod";
    _n->head.kind = &kind_node_mod;
    _n->head.parent = NULL;
    _n->head.jit_status = JIT_STATUS_Unknown;
    _n->head.dispatch_cnt = 0;
    _n->head.flags.has_hash_value = false;
    _n->head.flags.is_specialized = false;
    _n->head.flags.is_specializing = false;
    _n->head.flags.is_dumping = false;
    _n->head.flags.no_inline = false;
    _n->u.node_mod.lv = lv;
    _n->u.node_mod.rv = rv;
    if (_n->u.node_mod.lv) {_n->u.node_mod.lv->head.parent = _n;}
    if (_n->u.node_mod.rv) {_n->u.node_mod.rv->head.parent = _n;}

    OPTIMIZE(_n);
    if (OPTION.record_all) code_repo_add(NULL, _n, false);
    return _n;
}

