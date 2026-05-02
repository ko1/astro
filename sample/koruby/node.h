#ifndef KORUBY_NODE_H
#define KORUBY_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
/* hash_node is defined static in runtime/astro_node.c (single TU only). */
/* runtime/astro_node.c uses HORG / HOPT to choose between
 * structural (Horg) and profile-baked (Hopt) hash names.  koruby is
 * AOT-only — both expand to the structural hash. */
#define HORG(n) hash_node(n)
#define HOPT(n) hash_node(n)
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
        bool has_hash_opt;       /* Hopt loaded from PGC index — runtime use only */
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;
    const struct NodeKind *kind;
    struct Node *parent;
    node_hash_t hash_value;
    node_hash_t hash_opt;        /* PGC-baked hash, when has_hash_opt set */
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

/* The auto-generated SD_*.c files include node.h and node_eval.c.
 * node_eval.c uses korb_* helpers declared in object.h; pull those in
 * here (after struct Node is fully defined so object.h's inline
 * functions can dereference NODE fields). */
#include "object.h"

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
