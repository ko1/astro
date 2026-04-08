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
DISPATCH_node_sub(CTX *c,  NODE *n)
{
    dispatch_info(c, n, 0);
    VALUE v = EVAL_node_sub(c, n, n->u.node_sub.lv, n->u.node_sub.lv->head.dispatcher, n->u.node_sub.rv, n->u.node_sub.rv->head.dispatcher);
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

static VALUE
DISPATCH_node_div(CTX *c,  NODE *n)
{
    dispatch_info(c, n, 0);
    VALUE v = EVAL_node_div(c, n, n->u.node_div.lv, n->u.node_div.lv->head.dispatcher, n->u.node_div.rv, n->u.node_div.rv->head.dispatcher);
    dispatch_info(c, n, 1);

    return v;
}

static VALUE
DISPATCH_node_mod(CTX *c,  NODE *n)
{
    dispatch_info(c, n, 0);
    VALUE v = EVAL_node_mod(c, n, n->u.node_mod.lv, n->u.node_mod.lv->head.dispatcher, n->u.node_mod.rv, n->u.node_mod.rv->head.dispatcher);
    dispatch_info(c, n, 1);

    return v;
}

