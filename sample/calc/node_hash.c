// This file is auto-generated from node.def.
// hash functions

static node_hash_t
HASH_node_num(NODE *n)
{
    node_hash_t h = hash_cstr("node_num");
    h = hash_merge(h, hash_uint32((uint32_t)n->u.node_num.num));
    return h;
}

static node_hash_t
HASH_node_add(NODE *n)
{
    node_hash_t h = hash_cstr("node_add");
    h = hash_merge(h, hash_node(n->u.node_add.lv));
    h = hash_merge(h, hash_node(n->u.node_add.rv));
    return h;
}

static node_hash_t
HASH_node_sub(NODE *n)
{
    node_hash_t h = hash_cstr("node_sub");
    h = hash_merge(h, hash_node(n->u.node_sub.lv));
    h = hash_merge(h, hash_node(n->u.node_sub.rv));
    return h;
}

static node_hash_t
HASH_node_mul(NODE *n)
{
    node_hash_t h = hash_cstr("node_mul");
    h = hash_merge(h, hash_node(n->u.node_mul.lv));
    h = hash_merge(h, hash_node(n->u.node_mul.rv));
    return h;
}

static node_hash_t
HASH_node_div(NODE *n)
{
    node_hash_t h = hash_cstr("node_div");
    h = hash_merge(h, hash_node(n->u.node_div.lv));
    h = hash_merge(h, hash_node(n->u.node_div.rv));
    return h;
}

static node_hash_t
HASH_node_mod(NODE *n)
{
    node_hash_t h = hash_cstr("node_mod");
    h = hash_merge(h, hash_node(n->u.node_mod.lv));
    h = hash_merge(h, hash_node(n->u.node_mod.rv));
    return h;
}

