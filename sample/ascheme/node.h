#ifndef NODE_H
#define NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;     // PGC bookkeeping (unused; v0)
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;       // mirrors hash_value while PGC is off

    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;
};

// PGC is not enabled — the "optimized" hash is the same as the structural
// one, and the code store falls through to AOT (SD_<Horg>) lookups.
#define HOPT(n) HASH(n)
#define HORG(n) HASH(n)

#include "node_head.h"

// Inline so specialized dispatchers don't pay a PLT call back into the host
// binary on every node dispatch — same trick wastro uses.
static inline VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

// Application primitives provided by main.c.
VALUE scm_apply_tail(CTX *c, VALUE fn, int argc, VALUE *argv, uint32_t is_tail);
VALUE scm_callcc(CTX *c, VALUE fn);

// Numeric tower binary ops, called from the specialized arith nodes when
// the fixnum fast-path misses.  Defined in main.c.
VALUE add2(CTX *c, VALUE a, VALUE b);
VALUE sub2(CTX *c, VALUE a, VALUE b);
VALUE mul2(CTX *c, VALUE a, VALUE b);
int   cmp2(CTX *c, VALUE a, VALUE b);

#endif // NODE_H
