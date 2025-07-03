// This file is auto-generated from node.def.
// kinds
static const struct NodeKind kind_node_num = {
    .default_dispatcher_name = "DISPATCH_node_num",
    .default_dispatcher = DISPATCH_node_num,
    .hash_func = HASH_node_num,
    .specializer = SPECIALIZE_node_num,
    .dumper = DUMP_node_num,
};

static const struct NodeKind kind_node_add = {
    .default_dispatcher_name = "DISPATCH_node_add",
    .default_dispatcher = DISPATCH_node_add,
    .hash_func = HASH_node_add,
    .specializer = SPECIALIZE_node_add,
    .dumper = DUMP_node_add,
};

static const struct NodeKind kind_node_mul = {
    .default_dispatcher_name = "DISPATCH_node_mul",
    .default_dispatcher = DISPATCH_node_mul,
    .hash_func = HASH_node_mul,
    .specializer = SPECIALIZE_node_mul,
    .dumper = DUMP_node_mul,
};


// allocators

NODE *
ALLOC_node_num(int32_t num) {
    NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct node_num_struct));
    _n->head.dispatcher = DISPATCH_node_num;
    _n->head.dispatcher_name = "DISPATCH_node_num";
    _n->head.kind = &kind_node_num;
    _n->head.parent = NULL;
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
ALLOC_node_mul(NODE * lv, NODE * rv) {
    NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct node_mul_struct));
    _n->head.dispatcher = DISPATCH_node_mul;
    _n->head.dispatcher_name = "DISPATCH_node_mul";
    _n->head.kind = &kind_node_mul;
    _n->head.parent = NULL;
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

