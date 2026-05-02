#ifndef NODE_H
#define NODE_H 1

#include "context.h"

typedef struct Node NODE;
// 3-arg dispatcher: VALUE *fp is the current local-variable frame,
// register-passed.  Using fp parameter (instead of c->fp memory traffic)
// is what lets specialized SD chains keep the frame in a register
// across recursive calls — see castro/docs/runtime.md for the pattern.
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n, VALUE *fp);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);
char *SPECIALIZED_SRC(NODE *n);

void clear_hash(NODE *n);

NODE *code_repo_find(node_hash_t h);
NODE *code_repo_find_by_name(const char *name);
void code_repo_add(const char *name, NODE *body, bool force_add);
void code_repo_add2(const char *name, NODE *body, bool force_add, uint32_t locals_cnt);
uint32_t code_repo_find_locals_cnt_by_body(NODE *body);
uint32_t code_repo_find_locals_cnt_by_name(const char *name);

// Enable optional NodeHead fields used by naruby
#define ASTRO_NODEHEAD_PARENT
#define ASTRO_NODEHEAD_JIT_STATUS
#define ASTRO_NODEHEAD_DISPATCH_CNT

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;       // tracked by code_store for PGC; v0 unused
        bool is_specialized;
        bool is_specializing; // to prohibit recursive specializing
        bool is_dumping;      // to prohibit recursive dumping
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;        // same as hash_value when PGC inactive

    const char *dispatcher_name;

    // use in exec
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
        JIT_STATUS_Querying,
        JIT_STATUS_NotFound,
        JIT_STATUS_Compiling,
        JIT_STATUS_Compiled,
    } jit_status;

    unsigned int dispatch_cnt;
    int line;                    // source line for diagnostics; v0 unused
};

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

// naruby has no profile-guided hash split; HOPT == HORG == HASH.
// Defined as macros so runtime/astro_node.c's alloc_dispatcher_name_hash
// (which references HOPT under astro_cs_use_hopt_name) compiles cleanly.
#define HOPT(n) HASH(n)
#define HORG(n) HASH(n)

#include "node_head.h"
#include "bf.h"

static inline RESULT
EVAL(CTX *c, NODE *n, VALUE *fp)
{
    return (*n->head.dispatcher)(c, n, fp);
}

// Cold-path slowpaths for `node_call2` / `node_pg_call_<N>`.  Defined
// in node_slowpath.c (compiled only into the main `naruby` binary).
// The generated AOT/PG code_store/*.so files reference these as
// unresolved externs and resolve them via -rdynamic at dlopen — so a
// single copy lives in the main binary instead of being duplicated
// into every .so.
struct callcache;
RESULT node_pg_call_slowpath(CTX *c, NODE *n, VALUE *fp,
                             const char *name, uint32_t params_cnt,
                             uint32_t arg_index, struct callcache *cc,
                             NODE **sp_body_p);
RESULT node_pg_call_n_slowpath(CTX *c, NODE *n, const VALUE *args,
                               uint32_t argc, const char *name,
                               struct callcache *cc, NODE **sp_body_p);

#endif // NODE_H
