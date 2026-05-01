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
        bool has_hash_opt;        // set when hash_opt has been computed
        bool kind_swapped;        // set by swap_dispatcher (profile signal)
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;         // profile-aware hash (aliases to hash_value today)
    int         line;             // source line (for PGC keying); 0 if unknown

    const char *dispatcher_name;

    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
};

// HORG: structural hash (canonical name).  Specialised variants
// declared with `@canonical=BASE` share BASE's HORG so swap_dispatcher
// keeps the same SD lookup key.
// HOPT: profile-aware hash that uses the *actual* current kind name —
// reflects post-swap state.  jstro PGC bake names SDs by HOPT.
node_hash_t HORG(NODE *n);
node_hash_t HOPT(NODE *n);
node_hash_t hash_node_opt(NODE *n);

#define ASTRO_NODEHEAD_PARENT 1
#define ASTRO_NODEHEAD_JIT_STATUS 1
#define ASTRO_NODEHEAD_DISPATCH_CNT 1

#include "node_head.h"

// Swap the node's dispatcher to a different kind (used for profile-
// driven type specialisation: node_le observes SMI×SMI, swaps to
// kind_node_smi_le_ii).  No-op once a baked SD has patched the
// dispatcher (the SD already encodes the post-swap behaviour, so a
// further runtime swap would just clobber the SD pointer with the
// host-binary default and lose specialisation).
static inline void
swap_dispatcher(NODE *n, const struct NodeKind *new_kind)
{
    if (n->head.flags.is_specialized || n->head.kind == new_kind) return;
    n->head.dispatcher       = new_kind->default_dispatcher;
    n->head.dispatcher_name  = new_kind->default_dispatcher_name;
    n->head.kind             = new_kind;
    n->head.flags.kind_swapped = true;
}

NODE *code_repo_find(node_hash_t h);
NODE *code_repo_find_by_name(const char *name);
void  code_repo_add(const char *name, NODE *body, bool force_add);

#endif
