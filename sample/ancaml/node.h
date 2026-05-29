#ifndef ANCAML_NODE_H
#define ANCAML_NODE_H 1

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

/* No PG support — HOPT == HORG == HASH. */
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
    enum jit_status { JIT_STATUS_Unknown } jit_status;
    unsigned int dispatch_cnt;
    int line;
};

#include "node_head.h"

static inline VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

NODE *code_repo_find(node_hash_t h);
void  code_repo_add(const char *name, NODE *body, bool force);

// --- runtime value model (value.c) -----------------------------------

struct ac_obj *ac_alloc(int type);
VALUE ac_make_float(double d);
VALUE ac_make_closure(NODE *body, struct ac_frame *env, int nparams, int is_leaf);
VALUE ac_make_tuple(int n, const VALUE *items);     // copies items
VALUE ac_make_array(int n, VALUE init);
VALUE ac_make_prim(const char *name, ac_prim_fn fn, int arity);

struct ac_frame *ac_new_frame(struct ac_frame *parent, int nslots);

double ac_get_float(CTX *c, VALUE v);
bool   ac_structural_eq(CTX *c, VALUE a, VALUE b);
int    ac_compare(CTX *c, VALUE a, VALUE b);         // -1 / 0 / 1 ordering

// Apply any callable (closure or prim) to argc arguments.
VALUE ac_apply(CTX *c, VALUE fn, int argc, VALUE *argv);

// External-function registry (print_int, sqrt, ...).
void  ac_register_externals(void);
VALUE ac_lookup_external(const char *name);          // 0 if absent

__attribute__((noreturn, format(printf, 2, 3)))
void ac_runtime_error(CTX *c, const char *fmt, ...);

void ac_display(FILE *fp, VALUE v);                  // for the REPL/debug

CTX *ac_make_context(void);

// Global side-tables holding the children of variable-arity nodes
// (tuple constructors and >4-ary applications).  Filled by the parser;
// indexed by the node's `*_idx` operand.  See node.def.
extern NODE **AC_CALL_ARGS;     // appn argument nodes
extern NODE **AC_TUPLE_ITEMS;   // tuple element nodes

// AOT: top-level expr + every function body + every variable-arity
// child is reached through a runtime dispatcher read, so each is its
// own code-store entry.  The parser collects them here.
extern NODE **ac_entries;
extern int    ac_n_entries;
void ac_aot_specialize(NODE *root);

// Function bodies — each is a tail-position root for tail-call marking.
extern NODE **ac_tail_roots;
extern int    ac_n_tail_roots;

// Rewrite tail-position applications under `n` into their `node_tail_*`
// variants (in-place kind+dispatcher swap; identical struct layout).
void ac_mark_tail(NODE *n);

#endif // ANCAML_NODE_H
