#ifndef ASTR_NODE_H
#define ASTR_NODE_H 1

// stdlib/string headers are needed by generated node_eval.c (e.g. strcmp,
// exit) which becomes part of every SD compile.
#include <stdlib.h>
#include <string.h>

#include "context.h"

struct Node;
bool astro_cs_load(struct Node *n, const char *file);

typedef struct Node NODE;

// 3-arg dispatcher (c, n, fp): the explicit `fp` parameter keeps the
// callee's frame in a register through specialized SD chains, mirroring
// naruby's design.
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n, VALUE *fp);
typedef uint64_t node_hash_t;

void   INIT(void);
node_hash_t HASH(NODE *n);
void   DUMP(FILE *fp, NODE *n, bool oneline);
NODE  *OPTIMIZE(NODE *n);
void   SPECIALIZE(FILE *fp, NODE *n);

NODE  *code_repo_find(node_hash_t h);
NODE  *code_repo_find_by_name(const char *name);
void   code_repo_add(const char *name, NODE *body, bool force_add);
uint32_t code_repo_find_locals_cnt_by_name(const char *name);

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

// No PG support yet — HOPT == HORG == HASH.  Required by
// runtime/astro_code_store.c.
#define HORG(n) HASH(n)
#define HOPT(n) HASH(n)

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;

    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;
};

#include "node_head.h"

static inline RESULT
EVAL(CTX *c, NODE *n, VALUE *fp)
{
    return (*n->head.dispatcher)(c, n, fp);
}

#endif // ASTR_NODE_H
