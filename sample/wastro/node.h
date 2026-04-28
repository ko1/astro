#ifndef NODE_H
#define NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n);
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
        bool has_hash_opt;     // tracked by code_store for PGC; v0 unused
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;      // Hopt — same as hash_value when PGC inactive

    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;                  // source line for diagnostics; v0 unused
};

// v0: PGC (profile-guided compilation) is not enabled, so the
// "optimized" hash is the same as the structural hash.  The code
// store falls through to AOT (SD_<Horg>) lookups in this mode.
#define HOPT(n) HASH(n)
#define HORG(n) HASH(n)

#include "node_head.h"

// Module-global tables populated by the parser.  node.def references
// WASTRO_FUNCS via an `extern` declaration inside each node_call_N.
extern struct wastro_function WASTRO_FUNCS[];
extern uint32_t WASTRO_FUNC_CNT;

// Function table for call_indirect.  -1 means uninitialized.
// Single funcref table only (wasm 1.0 limit).
extern int32_t *WASTRO_TABLE;
extern uint32_t WASTRO_TABLE_SIZE;

// Type signatures (from `(type $sig ...)`), used by call_indirect for
// runtime structural type checks.
extern struct wastro_type_sig WASTRO_TYPES[];
extern uint32_t WASTRO_TYPE_CNT;

// Parser-side helpers.
NODE *wastro_load_module(const char *path);
int wastro_find_export(const char *name);

// EVAL is inline so that specialized dispatchers (in code_store/all.so)
// don't pay a PLT call back into the host binary on every node
// dispatch — the dispatcher pointer is read and called directly.
static inline RESULT
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

// UNWRAP unwraps a RESULT: returns its `value` for normal flow, or
// early-returns the whole RESULT (propagating `br_depth`) when a
// branch is in flight.  Used inside every multi-operand NODE_DEF.
#define UNWRAP(r) ({ \
    RESULT _r = (r); \
    if (__builtin_expect(_r.br_depth != 0, 0)) return _r; \
    _r.value; \
})

#endif
