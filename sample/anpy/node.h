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

// =====================================================================
// Runtime structures (implemented in node.c)
// =====================================================================

// A boxed value cell — shared by reference so closures observe mutation.
typedef struct anpy_cell { VALUE v; } anpy_cell;

// Lexical environment frame: a name->cell map plus a parent pointer.
typedef struct anpy_env {
    struct anpy_env *parent;
    struct env_entry { const char *name; anpy_cell *cell; struct env_entry *next; } **buckets;
    int nbuckets;
} anpy_env;

struct Type;   // check.h

// One declaration inside a function body / class body.
struct var_decl { const char *name; NODE *init; int slot; struct Type *type; };

// Static descriptor of a function or method.
typedef struct anpy_func {
    const char *name;
    int nparams;       const char **params;   struct Type **param_types;   struct Type *ret_type;
    int nvars;         struct var_decl *vars;       // local variable defs (name + literal init)
    int nnested;       struct anpy_func **nested;    // nested function defs
    int nglobals;      const char **globals;         // `global x`
    int nnonlocals;    const char **nonlocals;       // `nonlocal x`
    NODE *body;                                       // seq of statements (may be NULL)
    bool is_method;
    struct anpy_class *cls;                           // defining class (methods)
} anpy_func;

// Method-table entry for dynamic dispatch.
struct method_ent { const char *name; anpy_func *fn; };

// Static descriptor of a class.
typedef struct anpy_class {
    const char *name;
    const char *super_name;     // resolved to `super` by finalize
    struct anpy_class *super;
    // declared in THIS class (filled by the parser):
    int own_nattrs;            struct var_decl *own_attrs;
    int own_nmethods;          struct method_ent *own_methods;
    // flattened (filled by anpy_finalize_classes): includes inherited + overrides:
    int nattrs;                struct var_decl *attrs;
    int nmethods;              struct method_ent *methods;
    bool finalized;
    int builtin;               // 0 user, else one of B_OBJECT/B_INT/B_BOOL/B_STR
} anpy_class;

void anpy_finalize_classes(void);   // flatten attrs/methods across the hierarchy

enum { B_NONE = 0, B_OBJECT, B_INT, B_BOOL, B_STR };

// --- value runtime (value.c) -----------------------------------------
#include "value.h"

// --- environment / scope ---------------------------------------------
anpy_env  *env_new(anpy_env *parent);
anpy_cell *env_lookup(anpy_env *e, const char *name);    // search chain; NULL if absent
anpy_cell *env_define(anpy_env *e, const char *name);    // create cell in this frame
void       env_bind(anpy_env *e, const char *name, anpy_cell *cell);  // alias existing cell

// --- registries (filled by parser/checker) ---------------------------
void         anpy_register_class(anpy_class *cls);
anpy_class  *anpy_lookup_class(const char *name);
void         anpy_register_global_func(anpy_func *fn);

// --- function call / object construction / dispatch ------------------
VALUE anpy_call_closure(CTX *c, anpy_closure *clo, VALUE *args, int nargs);
VALUE anpy_construct(CTX *c, anpy_class *cls);
anpy_func *anpy_class_method(anpy_class *cls, const char *name);

// --- eval helpers used by node.def (need NODE) -----------------------
VALUE anpy_make_list(CTX *c, NODE *chain);
VALUE anpy_getattr(CTX *c, VALUE obj, const char *name);
void  anpy_setattr(CTX *c, VALUE obj, const char *name, VALUE v);
VALUE anpy_do_call(CTX *c, const char *name, NODE *args);
VALUE anpy_do_method(CTX *c, VALUE recv, const char *name, NODE *args);
VALUE anpy_do_for(CTX *c, const char *name, VALUE iter, NODE *body);
void  anpy_massign(CTX *c, NODE *targets, VALUE v);

// --- context construction --------------------------------------------
CTX *anpy_make_context(void);
void anpy_install_globals(CTX *c);   // top-level defs into the global env
void anpy_aot_specialize(NODE *body, anpy_func **funcs, int nfuncs);

#endif // NODE_H
