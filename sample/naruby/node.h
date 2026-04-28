#ifndef NODE_H
#define NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n, void *frame);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);
char *SPECIALIZED_SRC(NODE *n);

void clear_hash(NODE *n);

NODE *code_repo_find(node_hash_t h);
NODE *code_repo_find_by_name(const char *name);
void code_repo_add(const char *name, NODE *body, bool force_add);

// Enable optional NodeHead fields used by naruby
#define ASTRO_NODEHEAD_PARENT
#define ASTRO_NODEHEAD_JIT_STATUS
#define ASTRO_NODEHEAD_DISPATCH_CNT

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool is_specialized;
        bool is_specializing; // to prohibit recursive specializing
        bool is_dumping;      // to prohibit recursive dumping
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;

    const char *dispatcher_name;

    // use in exec
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
        JIT_STATUS_Querying,
        JIT_STATUS_NotFound,
        JIT_STATUS_Compiling,
        JIT_STATUS_Compiled,
    } jit_status;

    unsigned int dispatch_cnt;
};

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

#include "node_head.h"
#include "bf.h"

static inline VALUE
EVAL(CTX *c, NODE *n, void *frame)
{
    return (*n->head.dispatcher)(c, n, frame);
}

#endif // NODE_H
