#ifndef ANLOX_NODE_H
#define ANLOX_NODE_H 1

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

// --- value model (value.c) -------------------------------------------

struct lox_obj *lox_alloc(int type);
VALUE lox_number(double d);
VALUE lox_string(const char *s, int len);          // copies s
VALUE lox_string_take(char *s, int len);            // takes ownership
VALUE lox_closure(struct lox_fundef *fn, struct lox_frame *env);
VALUE lox_class(const char *name, struct lox_obj *superclass);
VALUE lox_instance(struct lox_obj *klass);
VALUE lox_native(const char *name, lox_native_fn fn, int arity);

struct lox_frame *lox_new_frame(struct lox_frame *parent, int nslots);

// hash tables (globals / fields / method tables)
void  lox_table_init(struct lox_table *t);
bool  lox_table_get(struct lox_table *t, const char *key, VALUE *out);
void  lox_table_set(struct lox_table *t, const char *key, VALUE v);

// globals (late-bound, definable/redefinable at runtime)
void  lox_global_define(CTX *c, const char *name, VALUE v);
VALUE lox_global_get(CTX *c, const char *name);            // runtime error if undefined
void  lox_global_set(CTX *c, const char *name, VALUE v);   // runtime error if undefined

// operations
bool   lox_truthy(VALUE v);                 // only nil and false are falsey
bool   lox_equals(VALUE a, VALUE b);        // Lox ==
VALUE  lox_add(CTX *c, VALUE a, VALUE b);   // number add or string concat
double lox_as_num(CTX *c, VALUE v, const char *what);  // runtime-checked
VALUE  lox_call(CTX *c, VALUE callee, int argc, VALUE *argv);
VALUE  lox_get_property(CTX *c, VALUE obj, const char *name);
void   lox_set_property(CTX *c, VALUE obj, const char *name, VALUE v);
VALUE  lox_make_class(CTX *c, const char *name, VALUE superclass, uint32_t methods_idx, uint32_t cnt);
VALUE  lox_super_get(CTX *c, uint32_t super_depth, const char *method);

void  lox_print(FILE *fp, VALUE v);         // `print` statement formatting

void  lox_register_natives(CTX *c);

__attribute__((noreturn, format(printf, 2, 3)))
void lox_runtime_error(CTX *c, const char *fmt, ...);

CTX *lox_make_context(void);

// --- side-tables for variable-arity nodes (filled by parser) ---------
extern NODE **LOX_CALL_ARGS;     // node_call argument nodes
extern NODE **LOX_BLOCK_STMTS;   // node_block / node_stmts statement nodes
extern struct lox_fundef **LOX_FUNDEFS;   // node_closure function descriptors
extern uint32_t *LOX_CLASS_METHODS;       // node_classexpr method fundef indices

// AOT: top-level + every function body + variable-arity children are
// reached via a runtime dispatcher read; each is its own code-store entry.
extern NODE **lox_entries;
extern int    lox_n_entries;
void lox_aot_specialize(NODE *root);

#endif // ANLOX_NODE_H
