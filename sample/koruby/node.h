#ifndef KORUBY_NODE_H
#define KORUBY_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

void clear_hash(NODE *n);

NODE *code_repo_find(node_hash_t h);
void code_repo_add(const char *name, NODE *body, bool force);

#define DISPATCHER_NAME(n) \
    ((n)->head.flags.no_inline ? (#n "->head.dispatcher") : (n)->head.dispatcher_name)

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;
    const struct NodeKind *kind;
    struct Node *parent;
    node_hash_t hash_value;
    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;
    enum jit_status {
        JIT_STATUS_Unknown,
        JIT_STATUS_Compiling,
        JIT_STATUS_Compiled,
    } jit_status;
    unsigned int dispatch_cnt;

    /* line number for backtrace */
    int line;
    const char *source_file;
};

#include "node_head.h"

static inline VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

/* helper: rewrite a child slot in a parent's union, used for type-spec rewrites */
void node_replace(NODE *parent, NODE *old, NODE *new_node);

/* swap an existing node's dispatcher to a different node type without realloc */
void korb_swap_dispatcher(NODE *n, const struct NodeKind *new_kind);

#endif /* KORUBY_NODE_H */
