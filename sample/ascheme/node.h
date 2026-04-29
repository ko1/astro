#ifndef NODE_H
#define NODE_H 1

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

struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;     // PGC bookkeeping (unused; v0)
        bool is_specialized;
        bool is_specializing;
        bool is_dumping;
        bool no_inline;
    } flags;

    const struct NodeKind *kind;
    struct Node *parent;

    node_hash_t hash_value;
    node_hash_t hash_opt;       // mirrors hash_value while PGC is off

    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;
};

// PGC is not enabled — the "optimized" hash is the same as the structural
// one, and the code store falls through to AOT (SD_<Horg>) lookups.
#define HOPT(n) HASH(n)
#define HORG(n) HASH(n)

#include "node_head.h"

// Inline so specialized dispatchers don't pay a PLT call back into the host
// binary on every node dispatch — same trick wastro uses.
static inline VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

// Application primitives provided by main.c.
VALUE scm_apply(CTX *c, VALUE fn, int argc, VALUE *argv);
VALUE scm_callcc(CTX *c, VALUE fn);
// Slow path for `scm_apply_tail` — when the inline fast path below
// can't apply.
VALUE scm_apply_tail_slow(CTX *c, VALUE fn, int argc, VALUE *argv, uint32_t is_tail);

// Both the tail and non-tail closure-leaf paths are inlined here —
// SD `.so` files see the same body as the host interpreter, and gcc
// folds away `is_tail` at the call site (it's a parse-time constant).
// The PLT call to `scm_apply_tail_slow` only fires for non-leaf
// closures, has_rest closures, primitives, and continuations.
static inline __attribute__((always_inline)) VALUE
scm_apply_tail(CTX *c, VALUE fn, int argc, VALUE *argv, uint32_t is_tail)
{
    // Tail-call fast path — frame reuse on self-tail-call to a leaf.
    if (is_tail && LIKELY(scm_is_closure(fn))) {
        struct sobj *cl = SCM_PTR(fn);
        int total = cl->closure.nparams + (cl->closure.has_rest ? 1 : 0);
        if (LIKELY(!cl->closure.has_rest &&
                    cl->closure.leaf &&
                    c->env != NULL &&
                    c->env->parent == cl->closure.env &&
                    c->env->nslots == total &&
                    argc == cl->closure.nparams)) {
            for (int i = 0; i < cl->closure.nparams; i++) c->env->slots[i] = argv[i];
            c->next_body = cl->closure.body;
            c->next_env = c->env;
            c->tail_call_pending = 1;
            return 0;     // bogus; trampoline ignores
        }
    }
    // Non-tail leaf-closure call — alloca frame + run trampoline inline.
    // Same shape as scm_apply's closure-leaf path but visible to gcc
    // at the call site, so the SD chain folds through without a PLT hop.
    if (!is_tail && LIKELY(scm_is_closure(fn))) {
        struct sobj *cl = SCM_PTR(fn);
        if (LIKELY(!cl->closure.has_rest &&
                    cl->closure.leaf &&
                    argc == cl->closure.nparams)) {
            int total = cl->closure.nparams;
            struct sframe *new_env = (struct sframe *)alloca(
                sizeof(struct sframe) + sizeof(VALUE) * (total ? total : 1));
            new_env->parent = cl->closure.env;
            new_env->nslots = total;
            for (int i = 0; i < total; i++) new_env->slots[i] = argv[i];
            struct sframe *saved = c->env;
            NODE *body = cl->closure.body;
            c->env = new_env;
            for (;;) {
                VALUE v = EVAL(c, body);
                if (!c->tail_call_pending) { c->env = saved; return v; }
                c->tail_call_pending = 0;
                body  = c->next_body;
                c->env = c->next_env;
            }
        }
    }
    return scm_apply_tail_slow(c, fn, argc, argv, is_tail);
}

// Numeric tower binary ops, called from the specialized arith nodes when
// the fixnum fast-path misses.  Defined in main.c.
VALUE add2(CTX *c, VALUE a, VALUE b);
VALUE sub2(CTX *c, VALUE a, VALUE b);
VALUE mul2(CTX *c, VALUE a, VALUE b);
int   cmp2(CTX *c, VALUE a, VALUE b);

#endif // NODE_H
