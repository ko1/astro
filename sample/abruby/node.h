#ifndef ABRUBY_NODE_H
#define ABRUBY_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;
typedef node_hash_t (*node_hash_func_t)(NODE *n);
typedef void (*node_specializer_func_t)(FILE *f, NODE *n, bool public_func);
typedef void (*node_dumper_func_t)(FILE *f, NODE *n, bool online);

void INIT(void);
node_hash_t HASH(NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);
char *SPECIALIZED_SRC(NODE *n);

void clear_hash(NODE *n);

VALUE abruby_ivar_get(VALUE self, const char *name);
void abruby_ivar_set(VALUE self, const char *name, VALUE val);
struct abruby_method *abruby_find_method(struct abruby_class *klass, const char *name);

VALUE abruby_wrap_class(struct abruby_class *klass);
void abruby_class_set_const(struct abruby_class *klass, const char *name, VALUE val);
struct abruby_class *abruby_unwrap_class(VALUE obj);
VALUE abruby_new_object(struct abruby_class *klass);

// String helpers
VALUE abruby_str_new(VALUE rb_str);
VALUE abruby_str_new_cstr(const char *s);
VALUE abruby_str_rstr(VALUE ab_str);

// Array/Hash helpers
VALUE abruby_ary_new(VALUE rb_ary);
VALUE abruby_hash_new_wrap(VALUE rb_hash);
void abruby_node_mark(void *ptr);

void code_repo_add(const char *name, NODE *body, bool force_add);
void node_replace(NODE *old_node, NODE *new_node);
VALUE abruby_wrap_node(NODE *n);

// exception support
VALUE abruby_exception_new(CTX *c, struct abruby_frame *frame, VALUE message);

typedef void (*node_replace_child_func_t)(NODE *parent, NODE *old_child, NODE *new_child);

struct NodeKind {
    const char *default_dispatcher_name;
    node_dispatcher_func_t default_dispatcher;
    node_hash_func_t hash_func;
    node_specializer_func_t specializer;
    node_dumper_func_t dumper;
    node_replace_child_func_t replace_child;
};

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

    // for GC (T_DATA wrapper)
    VALUE rb_wrapper;

    // kept for astrogen compatibility
    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;

    // source location (for backtrace)
    int32_t line;
};

#define DISPATCHER_NAME(n) (n->head.flags.no_inline) ? (#n "->head.dispatcher") : (n->head.dispatcher_name)

#include "node_head.h"

static inline RESULT
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

#endif
