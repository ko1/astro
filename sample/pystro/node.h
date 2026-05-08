#ifndef PYS_NODE_H
#define PYS_NODE_H 1

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
extern VALUE pys_apply_slow(CTX *c, VALUE fn, int argc, VALUE *argv);

// Inline closure-call fast path.  ascheme's `scm_apply_tail` did the
// same: visible to SD code so the per-call frame setup folds into the
// caller's SD without a PLT hop into runtime.c.  Cold cases (builtin /
// bound method / class / wrong type) fall through to pys_apply_slow.
//
// `leaf` funcs (no nested def/class in body — set by the parser) get
// their frame on the C stack via alloca: zero GC pressure per call.
// Boehm conservatively scans the C stack so VALUE slots stored there
// keep their referents alive.  Frames don't escape (pystro has no
// closure capture of locals), so the alloca lifetime exactly matches
// the call.
static inline __attribute__((always_inline)) VALUE
pys_apply(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    // If an argument expression raised, don't invoke the callee with
    // a half-built argv — propagate the raise immediately.
    if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
    if (LIKELY(PYS_IS_PTR(fn) && PYS_PTR(fn)->type == PYS_T_FUNC)) {
        struct pysobj *f = PYS_PTR(fn);
        // Fast path only handles plain "exact arity, no varargs/kwargs,
        // not a generator" — anything fancier routes through
        // pys_apply_slow.
        if (LIKELY(argc == f->func.nparams && argc == f->func.n_pos_named && !f->func.has_varargs && !f->func.has_kwargs && !f->func.is_generator)) {
            int total = f->func.nlocals;
            struct pysframe *new_env;
            if (LIKELY(f->func.leaf)) {
                new_env = (struct pysframe *)alloca(
                    sizeof(struct pysframe) + sizeof(VALUE) * (total ? total : 1));
            } else {
                new_env = (struct pysframe *)GC_malloc(
                    sizeof(struct pysframe) + sizeof(VALUE) * (total ? total : 1));
            }
            new_env->parent = f->func.env;
            new_env->nslots = total;
            for (int i = 0; i < argc; i++) new_env->slots[i] = argv[i];
            for (int i = argc; i < total; i++) new_env->slots[i] = PYS_NONE;

            struct pysframe *saved = c->env;
            VALUE saved_mc = c->method_class;
            struct pysglobals *saved_g = c->globals;
            int saved_call_top = c->call_top;
            if (UNLIKELY(saved_call_top >= c->recursion_limit)) {
                pys_raise_exc(c, c->EXC_RecursionError,
                             "maximum recursion depth exceeded");
            }
            if (saved_call_top < 1024) c->call_stack[c->call_top++] = f->func.name;
            c->env = new_env;
            c->method_class = f->func.defining_class;
            if (f->func.fglobals) c->globals = f->func.fglobals;
            EVAL(c, f->func.body);
            c->env = saved;
            c->method_class = saved_mc;
            c->globals = saved_g;
            c->call_top = saved_call_top;

            if (c->state == PYS_STATE_RETURN) {
                VALUE r = c->state_value;
                c->state = PYS_STATE_NORMAL;
                c->state_value = PYS_NONE;
                return r;
            }
            if (UNLIKELY(c->state == PYS_STATE_RAISE)) return PYS_NONE;
            return PYS_NONE;
        }
    }
    return pys_apply_slow(c, fn, argc, argv);
}

// Variadic call sites (node_call) look up arg sub-trees through this
// table — populated by the parser, indexed by `args_idx`.  ASTroGen
// can't bake a static dispatcher for these (the index is a runtime
// value), so the loop walks `arg->head.dispatcher` directly.
extern NODE **PYS_CALL_ARGS;
extern size_t PYS_CALL_ARGS_LEN;
extern size_t PYS_CALL_ARGS_CAP;
size_t pys_call_args_reserve(NODE **args, size_t n);

// Inline fast paths for the iter protocol.  Kinds 0/2 (list / tuple /
// range) are pure-data, no user code, no alloca — safe to inline at
// SD-baked call sites.  Kind 5 (user iterator) goes via the dedicated
// out-of-line pys_iter_next_user so its alloca'd frames don't
// accumulate on the SD's stack across a tight loop.
extern bool pys_iter_next(CTX *c, struct pys_iter *it, VALUE *out);
extern bool pys_iter_next_user(CTX *c, struct pys_iter *it, VALUE *out);

static inline __attribute__((always_inline)) bool
pys_iter_next_inline(CTX *c, struct pys_iter *it, VALUE *out)
{
    switch (it->kind) {
      case 0:   // list / tuple
        if (it->i >= it->end) return false;
        *out = PYS_PTR(it->container)->list.items[it->i++];
        return true;
      case 2:   // range
        if (it->step > 0 ? it->i >= it->end : it->i <= it->end) return false;
        *out = pys_make_int(it->i);
        it->i += it->step;
        return true;
      case 5:   // user iterator (out-of-line, smaller frame)
        return pys_iter_next_user(c, it, out);
    }
    return pys_iter_next(c, it, out);
}

#endif // PYS_NODE_H
