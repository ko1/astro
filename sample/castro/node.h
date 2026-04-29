#ifndef CASTRO_NODE_H
#define CASTRO_NODE_H 1

#include "context.h"

typedef struct Node NODE;
// 3-arg dispatcher: castro threads `VALUE *fp` (the active frame
// pointer) through every node EVAL as a register-passed argument so
// node_call doesn't need to round-trip through `c->fp` on each call.
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n, VALUE *fp);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);
void code_repo_add(const char *name, NODE *body, bool force_add);

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;       // tracked by code_store for PGC; v0 unused
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;        // same as hash_value when PGC inactive

    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;                    // source line for diagnostics; v0 unused
};

#define HOPT(n) HASH(n)
#define HORG(n) HASH(n)

#include "node_head.h"

// Inline EVAL so specialized dispatchers in code_store don't pay PLT
// indirection back into the host.  Caller supplies the `fp` to use
// for the evaluated tree (typically `c->env` for the program entry,
// or the caller's local frame for nested evaluation).
static inline VALUE
EVAL(CTX *c, NODE *n, VALUE *fp)
{
    return (*n->head.dispatcher)(c, n, fp);
}

#endif
