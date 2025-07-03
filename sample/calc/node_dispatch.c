// This file is auto-generated from node.def.
// dispatchers

static VALUE
DISPATCH_node_num(CTX *c,  NODE *n)
{
    dispatch_info(c, n, 0);
    VALUE v = EVAL_node_num(c, n, n->u.node_num.num);
    dispatch_info(c, n, 1);

    return v;
}

static VALUE
DISPATCH_node_add(CTX *c,  NODE *n)
{
    dispatch_info(c, n, 0);
    VALUE v = EVAL_node_add(c, n, n->u.node_add.lv, n->u.node_add.lv->head.dispatcher, n->u.node_add.rv, n->u.node_add.rv->head.dispatcher);
    dispatch_info(c, n, 1);

    return v;
}

static VALUE
DISPATCH_node_mul(CTX *c,  NODE *n)
{
    dispatch_info(c, n, 0);
    VALUE v = EVAL_node_mul(c, n, n->u.node_mul.lv, n->u.node_mul.lv->head.dispatcher, n->u.node_mul.rv, n->u.node_mul.rv->head.dispatcher);
    dispatch_info(c, n, 1);

    return v;
}

