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
HASH_node_mul(NODE *n)
{
    node_hash_t h = hash_cstr("node_mul");
    h = hash_merge(h, hash_node(n->u.node_mul.lv));
    h = hash_merge(h, hash_node(n->u.node_mul.rv));
    return h;
}

