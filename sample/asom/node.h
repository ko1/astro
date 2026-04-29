#ifndef ASOM_NODE_H
#define ASOM_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
VALUE EVAL(CTX *c, NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

#define DISPATCHER_NAME(n) \
    ((n)->head.flags.no_inline ? (#n "->head.dispatcher") : (n)->head.dispatcher_name)

NODE *code_repo_find(node_hash_t h);
void  code_repo_add(const char *name, NODE *body, bool force);

struct NodeHead {
    // cold zone — only touched during HASH/SPECIALIZE/load paths.
    node_hash_t hash_value;       // Horg cache
    node_hash_t hash_opt;         // Hopt cache (profile-aware)
    const char *dispatcher_name;

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
    int32_t line;                 // source line (0 = unknown)
    unsigned int dispatch_cnt;    // bumped per asom_invoke for PG threshold

    // hot zone — touched on every EVAL.
    node_dispatcher_func_t dispatcher;
};

#include "node_head.h"

// HORG/HOPT used by runtime/astro_code_store.c — Horg is the structural
// hash; we don't yet split out profile information, so HOPT == HORG.
node_hash_t HORG(NODE *n);
node_hash_t HOPT(NODE *n);

// Swap a node's dispatcher to a sibling kind (e.g., node_send1 →
// node_send1_intplus on type-feedback). Skips the swap if the node has
// already been baked into SD code (is_specialized): the SD dispatcher
// was generated for the *current* kind, and overwriting it would discard
// the optimization. The kind is also updated for HASH/DUMP consistency.
// Specialized variants share the canonical kind name in HASH so this
// rewrite doesn't invalidate the structural hash used by the code store.
void swap_dispatcher(NODE *n, const struct NodeKind *target_kind);

// asom runtime helpers must be visible inside specialised SD_*.c files
// (which `#include "node.h"`) so generated dispatchers can call e.g.
// asom_send / asom_super_send without an implicit-declaration warning.
#include "asom_runtime.h"

#endif // ASOM_NODE_H
