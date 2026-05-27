#ifndef NODE_H
#define NODE_H 1

#include "context.h"
/* gc.h は ARO_STORE 等の static inline 定義を提供。 AOT'd SD_*.c が
 * node.h 経由で取り込まないと "undefined symbol: ARO_STORE" で dlopen
 * 失敗 → 全 SD load skip (= silent AOT fallback to plain dispatch)。
 * baruby_precise iter 59 と同じ fix。 */
#include "precise_gc/gc.h"

typedef struct Node NODE;
// 3-arg dispatcher: sp threads register-resident through the call chain,
// `c->sp` is only synced at GC safepoints by alloc helpers in main.c.
typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n, VALUE *sp);
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
EVAL(CTX *c, NODE *n, VALUE *sp)
{
    return (*n->head.dispatcher)(c, n, sp);
}

// Application primitives provided by main.c.
VALUE scm_apply(CTX *c, VALUE fn, int argc, VALUE *argv);
VALUE scm_callcc(CTX *c, VALUE fn);
// Slow path for `scm_apply_tail` — when the inline fast path below
// can't apply.
VALUE scm_apply_tail_slow(CTX *c, VALUE fn, int argc, VALUE *argv, uint32_t is_tail);
// `--pg-compile` flag — when set, the inline trampoline + scm_apply
// closure paths bump `body->head.dispatch_cnt`.  Off by default so
// the read-modify-write doesn't dominate tight tail loops.
extern bool ASCHEME_PROFILING;

// Both the tail and non-tail closure-leaf paths are inlined here —
// SD `.so` files see the same body as the host interpreter, and gcc
// folds away `is_tail` at the call site (it's a parse-time constant).
// The PLT call to `scm_apply_tail_slow` only fires for non-leaf
// closures, has_rest closures, primitives, and continuations.
static inline __attribute__((always_inline)) VALUE
scm_apply_tail(CTX *c, VALUE fn, int argc, VALUE *argv, uint32_t is_tail)
{
    // Profile mode: route every closure call through the out-of-line
    // path so `scm_apply` / `scm_apply_tail_slow` increment the
    // body->head.dispatch_cnt counter that --pg-compile reads.  The
    // inline fast paths below skip the increment to keep tight loops
    // tight in non-profile runs; this branch is taken only under
    // --pg-compile.
    if (UNLIKELY(ASCHEME_PROFILING))
        return scm_apply_tail_slow(c, fn, argc, argv, is_tail);
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
            ARO_STORE_BULK(c, c->env, c->env->slots, argv, (size_t)cl->closure.nparams);
            c->next_body = cl->closure.body;
            c->next_env = c->env;
            /* Signal frame_sp refresh to the trampoline when the next body
             * is no_capture (= uses lref_sp).  argv already lives in
             * c->env->slots (just copied above), so the trampoline can
             * mirror them onto sp[]. */
            c->next_no_capture = cl->closure.no_capture ? 1u : 0u;
            c->next_nparams = (uint16_t)cl->closure.nparams;
            c->tail_call_pending = 1;
            return 0;     // bogus; trampoline ignores
        }
    }
    // Non-tail leaf-closure call — alloca frame + run trampoline inline.
    // Same shape as scm_apply's closure-leaf path but visible to gcc
    // at the call site, so the SD chain folds through without a PLT hop.
    //
    // Disabled under a precise GC backend (BARUBY_GC != NONE): the GC root
    // visitor traverses c->env and treats any non-NULL frame pointer as a
    // GC heap address — alloca produces stack addresses that look like
    // valid sframe payloads, so the visitor either marks random stack
    // bytes (non-moving GC) or tries to forward them (moving GC), both of
    // which corrupt the heap.  Fall through to scm_apply_tail_slow which
    // routes through build_frame_for (heap-allocated, OBJ_FRAME-tagged).
    /* alloca path requires writing through ARO_GC_EDGE-qualified slots
     * without a real GC heap context; the audit build can't model that,
     * so we route through the slow path under -DARO_GC_WB_AUDIT. */
#if BARUBY_GC == BARUBY_GC_NONE && !defined(ARO_GC_WB_AUDIT)
    if (!is_tail && LIKELY(scm_is_closure(fn))) {
        struct sobj *cl = SCM_PTR(fn);
        if (LIKELY(!cl->closure.has_rest &&
                    cl->closure.leaf &&
                    argc == cl->closure.nparams)) {
            int total = cl->closure.nparams;
            struct sframe *new_env = (struct sframe *)alloca(
                sizeof(struct sframe) + sizeof(VALUE) * (total ? total : 1));
            /* alloca'd frame under GC=NONE — ARO_GC_HAS_WB is undefined so
             * ARO_STORE folds to a plain store (= holder ignored). */
            ARO_STORE(c, new_env, &new_env->parent, (VALUE)cl->closure.env);
            new_env->nslots = total;
            for (int i = 0; i < total; i++) ARO_STORE(c, new_env, &new_env->slots[i], argv[i]);
            struct sframe *saved = c->env;
            VALUE *saved_frame_sp = c->frame_sp;
            NODE *body = cl->closure.body;
            CTX_SET_ENV(c, new_env);
            /* For no_capture body, lref_sp reads c->frame_sp[sp_offset].
             * Anchor frame_sp at new_env->slots + nparams so the offsets
             * (= idx - nparams) land on the right slots.  Safe on GC=none
             * since alloca-stack memory doesn't move. */
            if (cl->closure.no_capture) {
                c->frame_sp = new_env->slots + total;
            }
            if (UNLIKELY(ASCHEME_PROFILING)) body->head.dispatch_cnt++;
            for (;;) {
                VALUE v = EVAL(c, body, c->sp);
                if (!c->tail_call_pending) {
                    CTX_SET_ENV(c, saved);
                    c->frame_sp = saved_frame_sp;
                    return v;
                }
                c->tail_call_pending = 0;
                body = c->next_body;
                // Frame-reuse leaves next_env == current env; skip the
                // bump so the lref level cache stays warm across tight
                // tail-call loops.
                if (c->next_env != c->env) CTX_SET_ENV(c, c->next_env);
                if (c->next_no_capture) {
                    c->frame_sp = c->env->slots + c->next_nparams;
                }
                if (UNLIKELY(ASCHEME_PROFILING)) body->head.dispatch_cnt++;
            }
        }
    }
#endif
    return scm_apply_tail_slow(c, fn, argc, argv, is_tail);
}

// Numeric tower binary ops, called from the specialized arith nodes when
// the fixnum fast-path misses.  Defined in main.c.
VALUE add2(CTX *c, VALUE a, VALUE b);
VALUE sub2(CTX *c, VALUE a, VALUE b);
VALUE mul2(CTX *c, VALUE a, VALUE b);
int   cmp2(CTX *c, VALUE a, VALUE b);

#endif // NODE_H
