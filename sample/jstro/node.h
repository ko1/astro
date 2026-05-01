#ifndef JSTRO_NODE_H
#define JSTRO_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef uint64_t node_hash_t;

typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n, JsValue *frame);

void INIT(void);
node_hash_t HASH(NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

// EVAL is defined as both a function (for external callers) and a macro
// (for hot internal use, expanding to the same code without going through
// the linker's symbol table).
RESULT EVAL_func(CTX *c, NODE *n, JsValue *frame);
#define EVAL(c, n, frame) ((*(n)->head.dispatcher)((c), (n), (frame)))

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
};

#include "node_head.h"

NODE *code_repo_find(node_hash_t h);
NODE *code_repo_find_by_name(const char *name);
void  code_repo_add(const char *name, NODE *body, bool force_add);

#endif
