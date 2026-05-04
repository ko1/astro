#ifndef AFORTH_NODE_H
#define AFORTH_NODE_H 1

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

/* No PG support yet — HOPT == HORG == HASH.  Required by runtime/astro_code_store.c. */
#define HORG(n) HASH(n)
#define HOPT(n) HASH(n)

NODE *code_repo_find(node_hash_t h);
void code_repo_add(const char *name, NODE *body, bool force);

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

/* --- Stack helpers used by node.def --- */

static inline void aforth_push(CTX * restrict c, VALUE v) { *c->dsp++ = v; }
static inline VALUE aforth_pop(CTX * restrict c) { return *--c->dsp; }
static inline VALUE aforth_top(const CTX * restrict c) { return c->dsp[-1]; }

static inline void aforth_rpush(CTX * restrict c, VALUE v) { *c->rsp++ = v; }
static inline VALUE aforth_rpop(CTX * restrict c) { return *--c->rsp; }
static inline VALUE aforth_rtop(const CTX * restrict c) { return c->rsp[-1]; }

/* --- Public entry points used by main.c --- */

NODE *aforth_parse_file(const char *src);
void aforth_run(CTX *c, NODE *toplevel);
void aforth_aot_compile_all(NODE *toplevel);
CTX *aforth_ctx_new(void);
void aforth_ctx_free(CTX *c);

/* code_repo (definition table) — also used by main.c for AOT compile walk. */
struct aforth_code_entry {
    const char *name;
    NODE *body;
};
struct aforth_code_repo {
    struct aforth_code_entry *entries;
    uint32_t size;
    uint32_t capa;
};
extern struct aforth_code_repo aforth_code_repo;

/* aforth_word_table[word_id] = body NODE *.  Filled by parser at ; time. */
extern NODE **aforth_word_table;
extern uint32_t aforth_word_count;

#endif /* AFORTH_NODE_H */
