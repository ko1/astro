#ifndef LUASTRO_NODE_H
#define LUASTRO_NODE_H 1

#include <limits.h>
#include "context.h"

typedef struct Node NODE;
// Match the ASTroGen-generated typedef for our 3-arg dispatcher
// (CTX *c, NODE *n, LuaValue *frame).
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n, LuaValue *frame);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
RESULT EVAL(CTX *c, NODE *n, LuaValue *frame);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

#define DISPATCHER_NAME(n) ((n)->head.flags.no_inline) ? (#n "->head.dispatcher") : ((n)->head.dispatcher_name)

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
        bool kind_swapped;        // set by swap_dispatcher (profile signal)
        bool has_hash_opt;        // set when hash_opt has been computed
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;         // profile-aware hash (when applicable)
    int         line;             // source line (for PGC keying); 0 if unknown

    const char *dispatcher_name;

    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
};

// HORG / HOPT: structural / profile-aware hashes.  Only HORG is
// implemented; HOPT aliases to HORG since we don't run a separate
// profile hash in v1.
node_hash_t HORG(NODE *n);
#define ASTRO_NODEHEAD_PARENT 1
#define ASTRO_NODEHEAD_JIT_STATUS 1
#define ASTRO_NODEHEAD_DISPATCH_CNT 1

#include "node_head.h"

// Swap a node's dispatcher to a different kind (used for profile-driven
// type specialization, e.g. node_arith_add → node_int_add).  Updates the
// parent link via REPLACER_ when relevant; for now we just rewrite the
// dispatcher slot in place.
static inline void
swap_dispatcher(NODE *n, const struct NodeKind *new_kind)
{
    n->head.dispatcher       = new_kind->default_dispatcher;
    n->head.dispatcher_name  = new_kind->default_dispatcher_name;
    n->head.kind             = new_kind;
    n->head.flags.kind_swapped = true;
}

// Helper to record a dispatch_cnt sample (for profile-guided heuristics).
static inline void
node_count_dispatch(NODE *n)
{
    if (n->head.dispatch_cnt < UINT_MAX) n->head.dispatch_cnt++;
}

#endif // LUASTRO_NODE_H
