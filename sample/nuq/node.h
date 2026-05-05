#ifndef NUQ_NODE_H
#define NUQ_NODE_H 1

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

    enum jit_status {
        JIT_STATUS_Unknown,
    } jit_status;
    unsigned int dispatch_cnt;
    int line;
};

#include "node_head.h"

NODE *code_repo_find(node_hash_t h);
void  code_repo_add (const char *name, NODE *body, bool force);

/* ===== inline-friendly helpers used directly from NODE_DEF bodies =====
 *
 * These live in this header so the SD specializer (which compiles
 * generated `EVAL_xxx` functions into per-NODE SDs) can inline them
 * directly into the SD bodies — fold-in candidates with no PLT hop.
 */

/* Cartesian binop fan-out: emits |L| × |R| values, each computed via
 * the appropriate `nuq_op_*` for `op`. */
static inline __attribute__((always_inline)) VALUE
nuq_binop_apply_inline(VALUE l, VALUE rv, int op)
{
    struct nuq_obj *lo = NUQ_PTR(l);
    struct nuq_obj *ro = NUQ_PTR(rv);
    VALUE r = nuq_make_array(lo->arr.len * ro->arr.len);
    for (size_t i = 0; i < lo->arr.len; i++) {
        for (size_t j = 0; j < ro->arr.len; j++) {
            VALUE a = lo->arr.items[i], b = ro->arr.items[j], v;
            switch (op) {
              case NUQ_OP_ADD_K: v = nuq_op_add(a, b); break;
              case NUQ_OP_SUB_K: v = nuq_op_sub(a, b); break;
              case NUQ_OP_MUL_K: v = nuq_op_mul(a, b); break;
              case NUQ_OP_DIV_K: v = nuq_op_div(a, b); break;
              case NUQ_OP_MOD_K: v = nuq_op_mod(a, b); break;
              default:           v = NUQ_NULL; break;
            }
            nuq_array_push(r, v);
        }
    }
    return r;
}

/* Cartesian comparison fan-out: emits |L| × |R| booleans. */
static inline __attribute__((always_inline)) VALUE
nuq_cmpop_apply_inline(VALUE l, VALUE rv, int op)
{
    struct nuq_obj *lo = NUQ_PTR(l);
    struct nuq_obj *ro = NUQ_PTR(rv);
    VALUE r = nuq_make_array(lo->arr.len * ro->arr.len);
    for (size_t i = 0; i < lo->arr.len; i++) {
        for (size_t j = 0; j < ro->arr.len; j++) {
            VALUE a = lo->arr.items[i], b = ro->arr.items[j];
            bool t;
            switch (op) {
              case NUQ_CMP_EQ_K: t = nuq_eq(a, b); break;
              case NUQ_CMP_NE_K: t = !nuq_eq(a, b); break;
              case NUQ_CMP_LT_K: t = nuq_cmp(a, b) <  0; break;
              case NUQ_CMP_LE_K: t = nuq_cmp(a, b) <= 0; break;
              case NUQ_CMP_GT_K: t = nuq_cmp(a, b) >  0; break;
              case NUQ_CMP_GE_K: t = nuq_cmp(a, b) >= 0; break;
              default:           t = false; break;
            }
            nuq_array_push(r, t ? NUQ_TRUE : NUQ_FALSE);
        }
    }
    return r;
}

#endif /* NUQ_NODE_H */
