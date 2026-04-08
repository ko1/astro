// This file is auto-generated from node.def.
#define EVAL_ARG(c, n) (*n##_dispatcher)(c, n)


static VALUE
EVAL_node_num(CTX *c,  NODE *n, int32_t num)
{
    return num;
}

static VALUE
EVAL_node_add(CTX *c,  NODE *n, NODE * lv, node_dispatcher_func_t lv_dispatcher, NODE * rv, node_dispatcher_func_t rv_dispatcher)
{
    return EVAL_ARG(c, lv) + EVAL_ARG(c, rv);
}

static VALUE
EVAL_node_sub(CTX *c,  NODE *n, NODE * lv, node_dispatcher_func_t lv_dispatcher, NODE * rv, node_dispatcher_func_t rv_dispatcher)
{
    return EVAL_ARG(c, lv) - EVAL_ARG(c, rv);
}

static VALUE
EVAL_node_mul(CTX *c,  NODE *n, NODE * lv, node_dispatcher_func_t lv_dispatcher, NODE * rv, node_dispatcher_func_t rv_dispatcher)
{
    return EVAL_ARG(c, lv) * EVAL_ARG(c, rv);
}

static VALUE
EVAL_node_div(CTX *c,  NODE *n, NODE * lv, node_dispatcher_func_t lv_dispatcher, NODE * rv, node_dispatcher_func_t rv_dispatcher)
{
    return EVAL_ARG(c, lv) / EVAL_ARG(c, rv);
}

static VALUE
EVAL_node_mod(CTX *c,  NODE *n, NODE * lv, node_dispatcher_func_t lv_dispatcher, NODE * rv, node_dispatcher_func_t rv_dispatcher)
{
    return EVAL_ARG(c, lv) % EVAL_ARG(c, rv);
}
