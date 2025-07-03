// (node_num 1)
static VALUE
SD_ef2d3a2c467c98a6(CTX *c,  NODE *n)
{
    dispatch_info(c, n, false);
    VALUE v = EVAL_node_num(c, n, 
        1
    );
    dispatch_info(c, n, true);
    return v;
}

// (node_num 2)
static VALUE
SD_69455740963f43ed(CTX *c,  NODE *n)
{
    dispatch_info(c, n, false);
    VALUE v = EVAL_node_num(c, n, 
        2
    );
    dispatch_info(c, n, true);
    return v;
}

// (node_num 3)
static VALUE
SD_46d0ffe733329604(CTX *c,  NODE *n)
{
    dispatch_info(c, n, false);
    VALUE v = EVAL_node_num(c, n, 
        3
    );
    dispatch_info(c, n, true);
    return v;
}

// (node_mul (node_num 2) (node_num 3))
static VALUE SD_69455740963f43ed(CTX *c, NODE *n);static VALUE SD_46d0ffe733329604(CTX *c, NODE *n);static VALUE
SD_fa5c4f2645bc412(CTX *c,  NODE *n)
{
    dispatch_info(c, n, false);
    VALUE v = EVAL_node_mul(c, n, 
        n->u.node_mul.lv,
        SD_69455740963f43ed,
        n->u.node_mul.rv,
        SD_46d0ffe733329604
    );
    dispatch_info(c, n, true);
    return v;
}

// (node_add (node_num 1) (node_mul (node_num 2) (node_num 3)))
static VALUE SD_ef2d3a2c467c98a6(CTX *c, NODE *n);static VALUE SD_fa5c4f2645bc412(CTX *c, NODE *n);static VALUE
SD_dfb75fdabb0d5ef6(CTX *c,  NODE *n)
{
    dispatch_info(c, n, false);
    VALUE v = EVAL_node_add(c, n, 
        n->u.node_add.lv,
        SD_ef2d3a2c467c98a6,
        n->u.node_add.rv,
        SD_fa5c4f2645bc412
    );
    dispatch_info(c, n, true);
    return v;
}

struct specialized_code sc_entries[] = {
    { // main
     .hash = 0xdfb75fdabb0d5ef6LL,
     .dispatcher_name = "SD_dfb75fdabb0d5ef6",
     .dispatcher      = SD_dfb75fdabb0d5ef6,
    },
};

#define NODE_SPECIALIZED_INCLUDED 1
