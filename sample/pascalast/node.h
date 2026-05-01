#ifndef PASCALAST_NODE_H
#define PASCALAST_NODE_H 1

#include "context.h"

typedef struct Node NODE;
// Dispatcher takes (c, n, fp).  fp is the current call frame's base
// index into c->stack — threaded through the dispatch chain so each
// node can access locals at fp+idx without reloading c->fp from
// memory at every access.
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n, int fp);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
VALUE EVAL(CTX *c, NODE *n, int fp);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

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
    } jit_status;
    unsigned int dispatch_cnt;

    // Source line of the program text this node was built from.
    // Stamped by OPTIMIZE from a parser-side global; used in run-time
    // error messages.
    int line;
};

extern int g_alloc_line;     // updated by the parser (next_token)
extern int pascal_runtime_line; // last node entered via dispatch_info

#include "node_head.h"

#endif
