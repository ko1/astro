#ifndef NODE_H
#define NODE_H 1

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

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

/* No PG support — HOPT == HORG == HASH.  Required by runtime/astro_code_store.c. */
#define HORG(n) HASH(n)
#define HOPT(n) HASH(n)

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;        /* PGC bookkeeping (unused; v0) */
        bool is_specialized;
        bool is_specializing;     /* prevent recursive specializing */
        bool is_dumping;          /* prevent recursive dumping */
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;         /* mirrors hash_value while PGC is off */

    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;                     /* source line for diagnostics */
};

#include "node_head.h"

// --- abc runtime support (implemented in node.c) ---------------------
#include "bcnum.h"

VALUE bc_var_get(CTX *c, const char *name);
VALUE bc_var_set(CTX *c, const char *name, VALUE val);
VALUE bc_arr_get(CTX *c, const char *name, long idx);
VALUE bc_arr_set(CTX *c, const char *name, long idx, VALUE val);
VALUE bc_call(CTX *c, const char *name, NODE *args);

// Function/variable registry (parser-facing).
struct bc_func {
    const char *name;
    int nparams;
    const char **params;
    int *param_isarray;
    int nautos;
    const char **autos;
    int *auto_isarray;
    NODE *body;
};
void bc_register_func(CTX *c, struct bc_func *f);
struct bc_func *bc_lookup_func(CTX *c, const char *name);

CTX *bc_make_context(void);
void bc_aot_specialize(CTX *c, NODE **stmts, int n);

#endif // NODE_H
