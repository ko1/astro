#ifndef ASTROGRE_NODE_H
#define ASTROGRE_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

/* Dispatch a NODE through its `head.dispatcher` pointer.  Defined as a
 * macro so the call sites stay grep-friendly without going through a
 * function call.  Used at runtime-indirect dispatch sites where the
 * NODE pointer comes from a CTX field, a stack frame, or a runtime
 * selection (so ASTroGen's specialiser cannot constant-fold the
 * dispatcher value).  For chain dispatch sites where the NODE is a
 * NODE_DEF parameter, prefer `EVAL_ARG(c, X)` — that macro is emitted
 * by ASTroGen into node_eval.c and does receive the dispatcher value
 * as a constant arg, which the specialiser can fold into a direct
 * call to the inlined SD. */
#define EVAL(c, n)   ((*(n)->head.dispatcher)((c), (n)))

#define DISPATCHER_NAME(n) ((n)->head.flags.no_inline ? (#n "->head.dispatcher") : (n)->head.dispatcher_name)

/* No PG support yet — HOPT == HORG == HASH. */
#define HORG(n) HASH(n)
#define HOPT(n) HASH(n)

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;          /* PGC tracking; not used for v1 */
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;           /* same as hash_value while PGC is inactive */

    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;                       /* source line for diagnostics; v1 unused */
};

#include "node_head.h"

#endif /* ASTROGRE_NODE_H */
