#ifndef NUQ_NODE_H
#define NUQ_NODE_H 1

#include "context.h"

typedef struct Node NODE;

/*
 * EMIT — return type of every NODE_DEF dispatcher.
 *
 * Instead of allocating a fresh `nuq_array` per filter call (which is
 * the hot path of every emit), each NODE_DEF appends its emits to the
 * per-CTX pool (`c->pool[]`), and returns a slice descriptor:
 *   - items: pointer into c->pool[]
 *   - count: number of emitted values
 *
 * This avoids `GC_malloc` per emit-array (~64 bytes for the nuq_obj
 * header) — for emit-heavy workloads (pyramid, try-catch, fan-out)
 * this is the dominant cost.  The pool itself grows monotonically;
 * callers release slots by writing back `c->pool_top = saved_top`
 * after consuming the child's EMIT.
 */
typedef struct {
    VALUE   *items;
    uint32_t count;
    uint32_t flags;     /* reserved; 0 in normal flow */
} EMIT;

typedef EMIT (*node_dispatcher_func_t)(CTX *c, NODE *n);
typedef uint64_t node_hash_t;

void INIT(void);
node_hash_t HASH(NODE *n);
EMIT EVAL(CTX *c, NODE *n);
void DUMP(FILE *fp, NODE *n, bool oneline);
NODE *OPTIMIZE(NODE *n);
void SPECIALIZE(FILE *fp, NODE *n);

/* Pool helpers — emit a single VALUE into the pool, growing if needed. */
static inline __attribute__((always_inline)) void
nuq_pool_push(CTX *c, VALUE v)
{
    if (UNLIKELY(c->pool_top == c->pool_capa)) {
        size_t nc = c->pool_capa ? c->pool_capa * 2 : 256;
        c->pool = (VALUE *)GC_realloc(c->pool, nc * sizeof(VALUE));
        c->pool_capa = nc;
    }
    c->pool[c->pool_top++] = v;
}

/* Build an EMIT for the slot range [top0, c->pool_top). */
static inline __attribute__((always_inline)) EMIT
nuq_emit_slice(CTX *c, size_t top0)
{
    EMIT e = { c->pool + top0, (uint32_t)(c->pool_top - top0), 0 };
    return e;
}

/* Empty emit (no values). */
#define EMIT_EMPTY ((EMIT){ NULL, 0, 0 })

/* Single-value emit — uses pool. */
static inline __attribute__((always_inline)) EMIT
nuq_emit_one(CTX *c, VALUE v)
{
    size_t top0 = c->pool_top;
    nuq_pool_push(c, v);
    return nuq_emit_slice(c, top0);
}

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

/* runtime.c helpers — EMIT-returning */
EMIT nuq_slice_eval (CTX *c, struct Node *startn, struct Node *stopn, uint32_t flags, bool optional);
EMIT nuq_object_eval(CTX *c, uint32_t entries_id);
EMIT nuq_error_eval (CTX *c, struct Node *expr);
EMIT nuq_user_call  (CTX *c, uint32_t name_id, uint32_t arity, uint32_t args_id);
EMIT nuq_defs_eval  (CTX *c, uint32_t defs_id, struct Node *body);
EMIT nuq_reduce_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update);
EMIT nuq_foreach_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update, struct Node *extract);
EMIT nuq_interp_eval(CTX *c, uint32_t parts_id);
EMIT nuq_format_eval(CTX *c, uint32_t fmt_id, struct Node *body);
EMIT nuq_map_values_eval(CTX *c, struct Node *body);
EMIT nuq_with_entries_eval(CTX *c, struct Node *body);
EMIT nuq_range2_eval(CTX *c, struct Node *from, struct Node *to);
EMIT nuq_range3_eval(CTX *c, struct Node *from, struct Node *to, struct Node *step);
EMIT nuq_has_eval(CTX *c, struct Node *key);
EMIT nuq_in_eval(CTX *c, struct Node *container);
EMIT nuq_contains_eval(CTX *c, struct Node *rhs);
EMIT nuq_split_eval(CTX *c, struct Node *sep);
EMIT nuq_join_eval(CTX *c, struct Node *sep);
EMIT nuq_startswith_eval(CTX *c, struct Node *prefix);
EMIT nuq_endswith_eval(CTX *c, struct Node *suffix);
EMIT nuq_sort_by_eval(CTX *c, struct Node *body);
EMIT nuq_group_by_eval(CTX *c, struct Node *body);
EMIT nuq_unique_by_eval(CTX *c, struct Node *body);
EMIT nuq_min_by_eval(CTX *c, struct Node *body);
EMIT nuq_max_by_eval(CTX *c, struct Node *body);
EMIT nuq_indices_eval(CTX *c, struct Node *pat);
EMIT nuq_index1_eval(CTX *c, struct Node *pat);
EMIT nuq_test_eval(CTX *c, struct Node *pat);
EMIT nuq_getpath_eval(CTX *c, struct Node *path);
EMIT nuq_limit_eval(CTX *c, struct Node *cnt, struct Node *body);
EMIT nuq_nth_eval(CTX *c, struct Node *idx, struct Node *body);

/* recurse / paths into pool */
void nuq_recurse_collect_pool(CTX *c, VALUE v);
void nuq_paths_collect_pool(CTX *c, VALUE v);

/* Cartesian binop fan-out: emits |L| × |R| values into the pool. */
static inline __attribute__((always_inline)) EMIT
nuq_binop_apply_inline(CTX *c, EMIT l, EMIT rv, int op)
{
    size_t top0 = c->pool_top;
    for (uint32_t i = 0; i < l.count; i++) {
        for (uint32_t j = 0; j < rv.count; j++) {
            VALUE a = l.items[i], b = rv.items[j], v;
            switch (op) {
              case NUQ_OP_ADD_K: v = nuq_op_add(a, b); break;
              case NUQ_OP_SUB_K: v = nuq_op_sub(a, b); break;
              case NUQ_OP_MUL_K: v = nuq_op_mul(a, b); break;
              case NUQ_OP_DIV_K: v = nuq_op_div(a, b); break;
              case NUQ_OP_MOD_K: v = nuq_op_mod(a, b); break;
              default:           v = NUQ_NULL; break;
            }
            nuq_pool_push(c, v);
        }
    }
    return nuq_emit_slice(c, top0);
}

/* Cartesian comparison fan-out: emits |L| × |R| booleans. */
static inline __attribute__((always_inline)) EMIT
nuq_cmpop_apply_inline(CTX *c, EMIT l, EMIT rv, int op)
{
    size_t top0 = c->pool_top;
    for (uint32_t i = 0; i < l.count; i++) {
        for (uint32_t j = 0; j < rv.count; j++) {
            VALUE a = l.items[i], b = rv.items[j];
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
            nuq_pool_push(c, t ? NUQ_TRUE : NUQ_FALSE);
        }
    }
    return nuq_emit_slice(c, top0);
}

#endif /* NUQ_NODE_H */
