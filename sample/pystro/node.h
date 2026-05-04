#ifndef PYSTRO_NODE_H
#define PYSTRO_NODE_H 1

#include "context.h"

typedef struct Node NODE;
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

#define DISPATCHER_NAME(n) \
    ((n)->head.flags.no_inline ? (#n "->head.dispatcher") : (n)->head.dispatcher_name)

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
    node_hash_t hash_opt;       // mirrors hash_value (PGC unused for v0)

    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;                   // source line (1-based) for error messages
};

#define HOPT(n) HASH(n)
#define HORG(n) HASH(n)

#include "node_head.h"

// Inline so generated SDs don't pay a PLT call back into the host
// binary on every node dispatch.
static inline VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

// Slow-path apply: handles bound methods, classes, builtins, and the
// "wrong type" error case.  The closure fast path is inlined below to
// fold into SD bodies.
extern VALUE py_apply_slow(CTX *c, VALUE fn, int argc, VALUE *argv);

// Inline closure-call fast path.  ascheme's `scm_apply_tail` did the
// same: visible to SD code so the per-call frame setup folds into the
// caller's SD without a PLT hop into runtime.c.  Cold cases (builtin /
// bound method / class / wrong type) fall through to py_apply_slow.
//
// `leaf` funcs (no nested def/class in body — set by the parser) get
// their frame on the C stack via alloca: zero GC pressure per call.
// Boehm conservatively scans the C stack so VALUE slots stored there
// keep their referents alive.  Frames don't escape (pystro has no
// closure capture of locals), so the alloca lifetime exactly matches
// the call.
static inline __attribute__((always_inline)) VALUE
py_apply(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    if (LIKELY(PY_IS_PTR(fn) && PY_PTR(fn)->type == PY_T_FUNC)) {
        struct pyobj *f = PY_PTR(fn);
        if (LIKELY(argc == f->func.nparams)) {
            int total = f->func.nlocals;
            struct pyframe *new_env;
            if (LIKELY(f->func.leaf)) {
                new_env = (struct pyframe *)alloca(
                    sizeof(struct pyframe) + sizeof(VALUE) * (total ? total : 1));
            } else {
                new_env = (struct pyframe *)GC_malloc(
                    sizeof(struct pyframe) + sizeof(VALUE) * (total ? total : 1));
            }
            new_env->parent = f->func.env;
            new_env->nslots = total;
            for (int i = 0; i < argc; i++) new_env->slots[i] = argv[i];
            for (int i = argc; i < total; i++) new_env->slots[i] = PY_NONE;

            struct pyframe *saved = c->env;
            c->env = new_env;
            EVAL(c, f->func.body);
            c->env = saved;

            if (c->state == PY_STATE_RETURN) {
                VALUE r = c->state_value;
                c->state = PY_STATE_NORMAL;
                c->state_value = PY_NONE;
                return r;
            }
            if (UNLIKELY(c->state == PY_STATE_RAISE)) return PY_NONE;
            return PY_NONE;
        }
    }
    return py_apply_slow(c, fn, argc, argv);
}

// Variadic call sites (node_call) look up arg sub-trees through this
// table — populated by the parser, indexed by `args_idx`.  ASTroGen
// can't bake a static dispatcher for these (the index is a runtime
// value), so the loop walks `arg->head.dispatcher` directly.
extern NODE **PYSTRO_CALL_ARGS;
extern size_t PYSTRO_CALL_ARGS_LEN;
extern size_t PYSTRO_CALL_ARGS_CAP;
size_t pystro_call_args_reserve(NODE **args, size_t n);

#endif // PYSTRO_NODE_H
