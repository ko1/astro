/*
 * linearity.c — static linearity / escape analysis on the AST.
 *
 * The pure-functional `acc + [x]` pattern (canonical in jq's `reduce
 * SRC as $i (INIT; UPDATE)` and similar) is O(N²) under naive
 * copying semantics — every iteration allocates a fresh `acc + [x]`
 * array and copies all of `acc`'s items.  jq itself works around
 * this with runtime refcount: when `acc`'s refcount is 1 at `+`
 * time, it mutates in place.
 *
 * We don't have a refcount (Cheney copying GC), so we do the same
 * work statically: prove at compile time that the LHS of an `+` is
 * the unique reference to its array.  A site that passes the proof
 * gets its `node_add` rewritten to `node_add_inplace`, which calls
 * the array-mutating variant of `+`.
 *
 * Approach — dot-scope linearity counting.
 *
 *   The "current input" `.` is implicit and threaded through the
 *   filter.  Within a *dot-scope* (a contiguous AST region where the
 *   same dot is in effect — boundaries are pipe RHS, reduce/foreach
 *   UPDATE/EXTRACT, function bodies, and the `body` argument of
 *   higher-order builtins like `map(f)`) we count syntactic reads of
 *   that dot.  If a dot-scope reads dot exactly once and that single
 *   read is the LHS of an `+`, then at the moment `+` runs the dot
 *   value has no other live reference within the scope — safe to
 *   mutate.
 *
 *   `as $x | body` complicates this: when src=`.` (the variable
 *   captures dot identity) every reference to $x in body is also a
 *   reference to dot.  We track such "dot-aliased" var ids and count
 *   their uses too.  Conservatively, an `as` whose src is anything
 *   other than `node_identity` does NOT introduce dot-aliasing — the
 *   bound value is a derived value (e.g. `.x`, a sub-tree) which has
 *   its own identity, and even though the sub-tree shares storage
 *   with dot, our mutation is append-only on the array buffer:
 *   pre-existing children pointers are unaffected.
 *
 *   Constructs we can't model (user-def calls, complex builtins,
 *   assignments, object literals with computed keys/vals) "poison"
 *   the scope to UNKNOWN — no `+` in such a scope is rewritten.
 *
 * Two-pass per scope:
 *   pass 1 — `dot_count`: walk the scope subtree, accumulate dot
 *            uses + recursively analyze sub-scopes.
 *   pass 2 — `mark_adds`: if pass 1 yielded exactly 1 dot use AND no
 *            poisoning, walk again (NOT into sub-scopes) and rewrite
 *            every `node_add(node_identity, _)` to node_add_inplace.
 *
 * The pass also descends into all reachable user-def bodies via the
 * already-built nuq_func_def table so def bodies get the same
 * treatment.
 */
#include "context.h"
#include "node.h"
#include <limits.h>

extern const struct NodeKind kind_node_identity;
extern const struct NodeKind kind_node_int;
extern const struct NodeKind kind_node_lit;
extern const struct NodeKind kind_node_str;
extern const struct NodeKind kind_node_null;
extern const struct NodeKind kind_node_true;
extern const struct NodeKind kind_node_false;
extern const struct NodeKind kind_node_var;
extern const struct NodeKind kind_node_break;
extern const struct NodeKind kind_node_field;
extern const struct NodeKind kind_node_field_opt;
extern const struct NodeKind kind_node_index;
extern const struct NodeKind kind_node_index_opt;
extern const struct NodeKind kind_node_iter;
extern const struct NodeKind kind_node_iter_opt;
extern const struct NodeKind kind_node_slice;
extern const struct NodeKind kind_node_slice_opt;
extern const struct NodeKind kind_node_pipe;
extern const struct NodeKind kind_node_comma;
extern const struct NodeKind kind_node_add;
extern const struct NodeKind kind_node_add_inplace;
extern const struct NodeKind kind_node_sub;
extern const struct NodeKind kind_node_mul;
extern const struct NodeKind kind_node_div;
extern const struct NodeKind kind_node_mod;
extern const struct NodeKind kind_node_neg;
extern const struct NodeKind kind_node_eq;
extern const struct NodeKind kind_node_ne;
extern const struct NodeKind kind_node_lt;
extern const struct NodeKind kind_node_le;
extern const struct NodeKind kind_node_gt;
extern const struct NodeKind kind_node_ge;
extern const struct NodeKind kind_node_and;
extern const struct NodeKind kind_node_or;
extern const struct NodeKind kind_node_not;
extern const struct NodeKind kind_node_alt;
extern const struct NodeKind kind_node_array;
extern const struct NodeKind kind_node_array_empty;
extern const struct NodeKind kind_node_emit_count;
extern const struct NodeKind kind_node_emit_fold_add;
extern const struct NodeKind kind_node_if;
extern const struct NodeKind kind_node_try;
extern const struct NodeKind kind_node_as;
extern const struct NodeKind kind_node_label;
extern const struct NodeKind kind_node_empty;
extern const struct NodeKind kind_node_error0;
extern const struct NodeKind kind_node_error1;
extern const struct NodeKind kind_node_reduce;
extern const struct NodeKind kind_node_foreach;
extern const struct NodeKind kind_node_b_select;
extern const struct NodeKind kind_node_defs;

#define DOT_UNKNOWN INT_MAX

static int g_marks = 0;

/* Track up to N "dot-aliased" var ids active in current scope.  Most
 * scopes have 0 — only `node_identity as $x` adds one.  Stack-bounded
 * since we push/pop on entering/leaving as. */
#define MAX_DOT_ALIASES 16
struct ScopeCtx {
    int aliased_var_ids[MAX_DOT_ALIASES];
    int alias_top;
    int dot_uses;     /* accumulator for current scope */
    bool unknown;     /* poison flag */
};

static int  dot_count(struct Node *n, struct ScopeCtx *s);
static void mark_adds(struct Node *n);
static void analyze_scope(struct Node *root);

static bool
is_aliased_var(const struct ScopeCtx *s, int var_id)
{
    for (int i = 0; i < s->alias_top; i++)
        if (s->aliased_var_ids[i] == var_id) return true;
    return false;
}

/* Add to alias set for the duration of body; returns prev top so
 * caller can restore after body analysis. */
static int
alias_push(struct ScopeCtx *s, int var_id)
{
    int prev = s->alias_top;
    if (s->alias_top < MAX_DOT_ALIASES)
        s->aliased_var_ids[s->alias_top++] = var_id;
    else
        s->unknown = true;     /* too many — bail */
    return prev;
}

static void
alias_restore(struct ScopeCtx *s, int prev_top)
{
    s->alias_top = prev_top;
}

/* Compute dot use contribution of `n` to its enclosing scope.
 * Recurses into sub-scopes (which are analyzed independently and
 * contribute 0 to outer scope's count). */
static int
dot_count(struct Node *n, struct ScopeCtx *s)
{
    if (!n || s->unknown) return 0;
    const struct NodeKind *k = n->head.kind;

    /* Direct dot reads. */
    if (k == &kind_node_identity)        return 1;
    if (k == &kind_node_iter)            return 1;
    if (k == &kind_node_iter_opt)        return 1;
    if (k == &kind_node_field)           return 1;
    if (k == &kind_node_field_opt)       return 1;

    /* Dot read PLUS sub-expr in same scope. */
    if (k == &kind_node_index)
        return 1 + dot_count(n->u.node_index.expr, s);
    if (k == &kind_node_index_opt)
        return 1 + dot_count(n->u.node_index_opt.expr, s);
    if (k == &kind_node_slice)
        return 1 + dot_count(n->u.node_slice.startn, s)
                 + dot_count(n->u.node_slice.stopn, s);
    if (k == &kind_node_slice_opt)
        return 1 + dot_count(n->u.node_slice_opt.startn, s)
                 + dot_count(n->u.node_slice_opt.stopn, s);

    /* Vars: 0 unless alias-bound to dot. */
    if (k == &kind_node_var) {
        return is_aliased_var(s, (int)n->u.node_var.var_id) ? 1 : 0;
    }
    if (k == &kind_node_break) return 0;

    /* Pure literals. */
    if (k == &kind_node_int)         return 0;
    if (k == &kind_node_lit)         return 0;
    if (k == &kind_node_str)         return 0;
    if (k == &kind_node_null)        return 0;
    if (k == &kind_node_true)        return 0;
    if (k == &kind_node_false)       return 0;
    if (k == &kind_node_empty)       return 0;
    if (k == &kind_node_error0)      return 0;
    if (k == &kind_node_array_empty) return 0;

    /* Unary: passes scope through. */
    if (k == &kind_node_neg)    return dot_count(n->u.node_neg.expr, s);
    if (k == &kind_node_not)    return 0;  /* `not` is a 0-arg fn that reads `.` once */
    if (k == &kind_node_error1) return dot_count(n->u.node_error1.expr, s);

    /* Binary: lhs + rhs in same scope.  (We don't pre-mark `+` here
     * — the rewrite happens in pass 2 once the scope total is
     * known.) */
    if (k == &kind_node_comma)
        return dot_count(n->u.node_comma.lhs, s)
             + dot_count(n->u.node_comma.rhs, s);
    if (k == &kind_node_add)
        return dot_count(n->u.node_add.lhs, s)
             + dot_count(n->u.node_add.rhs, s);
    if (k == &kind_node_add_inplace)
        return dot_count(n->u.node_add_inplace.lhs, s)
             + dot_count(n->u.node_add_inplace.rhs, s);
    if (k == &kind_node_sub)
        return dot_count(n->u.node_sub.lhs, s) + dot_count(n->u.node_sub.rhs, s);
    if (k == &kind_node_mul)
        return dot_count(n->u.node_mul.lhs, s) + dot_count(n->u.node_mul.rhs, s);
    if (k == &kind_node_div)
        return dot_count(n->u.node_div.lhs, s) + dot_count(n->u.node_div.rhs, s);
    if (k == &kind_node_mod)
        return dot_count(n->u.node_mod.lhs, s) + dot_count(n->u.node_mod.rhs, s);
    if (k == &kind_node_eq)
        return dot_count(n->u.node_eq.lhs, s) + dot_count(n->u.node_eq.rhs, s);
    if (k == &kind_node_ne)
        return dot_count(n->u.node_ne.lhs, s) + dot_count(n->u.node_ne.rhs, s);
    if (k == &kind_node_lt)
        return dot_count(n->u.node_lt.lhs, s) + dot_count(n->u.node_lt.rhs, s);
    if (k == &kind_node_le)
        return dot_count(n->u.node_le.lhs, s) + dot_count(n->u.node_le.rhs, s);
    if (k == &kind_node_gt)
        return dot_count(n->u.node_gt.lhs, s) + dot_count(n->u.node_gt.rhs, s);
    if (k == &kind_node_ge)
        return dot_count(n->u.node_ge.lhs, s) + dot_count(n->u.node_ge.rhs, s);
    if (k == &kind_node_and)
        return dot_count(n->u.node_and.lhs, s) + dot_count(n->u.node_and.rhs, s);
    if (k == &kind_node_or)
        return dot_count(n->u.node_or.lhs, s) + dot_count(n->u.node_or.rhs, s);
    if (k == &kind_node_alt)
        return dot_count(n->u.node_alt.lhs, s) + dot_count(n->u.node_alt.rhs, s);

    /* Pipe: lhs is in current scope, rhs is a NEW scope (input
     * to rhs is lhs's emit, not outer dot).  Analyze rhs as its
     * own scope so any `+` inside gets its own marking decision. */
    if (k == &kind_node_pipe) {
        analyze_scope(n->u.node_pipe.rhs);
        return dot_count(n->u.node_pipe.lhs, s);
    }

    /* Array literal `[body]`: body is in same scope. */
    if (k == &kind_node_array)
        return dot_count(n->u.node_array.body, s);
    if (k == &kind_node_emit_count)
        return dot_count(n->u.node_emit_count.body, s);
    if (k == &kind_node_emit_fold_add)
        return dot_count(n->u.node_emit_fold_add.body, s);

    /* if-then-else: cond / then / else share outer dot scope; for
     * any single execution only one branch runs but as a static
     * upper bound we sum (the linearity check requires at most 1
     * across the whole expression, so summing is safe). */
    if (k == &kind_node_if) {
        return dot_count(n->u.node_if.cond, s)
             + dot_count(n->u.node_if.thn, s)
             + dot_count(n->u.node_if.els, s);
    }

    /* try / catch share outer scope; both potentially execute. */
    if (k == &kind_node_try) {
        return dot_count(n->u.node_try.body, s)
             + dot_count(n->u.node_try.handler, s);
    }

    /* `as $x | body` — src and body share outer dot.  If src is
     * `node_identity`, $x aliases dot in body — push to alias set. */
    if (k == &kind_node_as) {
        int src_uses = dot_count(n->u.node_as.src, s);
        bool src_is_dot = (n->u.node_as.src
                           && n->u.node_as.src->head.kind == &kind_node_identity);
        int prev = s->alias_top;
        if (src_is_dot) prev = alias_push(s, (int)n->u.node_as.var_id);
        int body_uses = dot_count(n->u.node_as.body, s);
        if (src_is_dot) alias_restore(s, prev);
        return src_uses + body_uses;
    }

    if (k == &kind_node_label)
        return dot_count(n->u.node_label.body, s);

    /* reduce SRC as $x (INIT; UPDATE) — src and init are in current
     * scope; update is a NEW scope (sees acc).  $x is bound to a
     * src emit, NOT to dot, so no aliasing. */
    if (k == &kind_node_reduce) {
        analyze_scope(n->u.node_reduce.update);
        return dot_count(n->u.node_reduce.src, s)
             + dot_count(n->u.node_reduce.init, s);
    }
    if (k == &kind_node_foreach) {
        analyze_scope(n->u.node_foreach.update);
        analyze_scope(n->u.node_foreach.extract);
        return dot_count(n->u.node_foreach.src, s)
             + dot_count(n->u.node_foreach.init, s);
    }

    /* select(cond) keeps . unchanged — cond is in same scope. */
    if (k == &kind_node_b_select)
        return dot_count(n->u.node_b_select.body, s);

    /* `def f: ...; ...; BODY` — defs declarations don't consume the
     * current dot (their bodies are entered via node_call which forms
     * its own scope).  Pass through to BODY in current scope. */
    if (k == &kind_node_defs)
        return dot_count(n->u.node_defs.body, s);

    /* Anything else: we don't model it.  Poison the scope. */
    s->unknown = true;
    return DOT_UNKNOWN;
}

/* Pass 2: walk subtree (NOT into sub-scopes — those handle their
 * own `+` rewrite).  For each `node_add(node_identity, _)` we visit,
 * rewrite to `node_add_inplace`. */
static void
mark_adds(struct Node *n)
{
    if (!n) return;
    const struct NodeKind *k = n->head.kind;

    if (k == &kind_node_add) {
        struct Node *lhs = n->u.node_add.lhs;
        if (lhs && lhs->head.kind == &kind_node_identity) {
            /* Rewrite kind in place — node_add and node_add_inplace
             * have identical struct layout (lhs/rhs only). */
            n->head.kind = &kind_node_add_inplace;
            n->head.dispatcher = kind_node_add_inplace.default_dispatcher;
            n->head.dispatcher_name = kind_node_add_inplace.default_dispatcher_name;
            g_marks++;
        }
        /* recurse into rhs in same scope.  lhs is identity, no nested + worth visiting. */
        mark_adds(n->u.node_add.rhs);
        return;
    }

    /* Don't descend into scope-changers — those got their own
     * analyze_scope() call. */
    if (k == &kind_node_pipe) { mark_adds(n->u.node_pipe.lhs); return; }
    if (k == &kind_node_reduce) {
        mark_adds(n->u.node_reduce.src);
        mark_adds(n->u.node_reduce.init);
        return;
    }
    if (k == &kind_node_foreach) {
        mark_adds(n->u.node_foreach.src);
        mark_adds(n->u.node_foreach.init);
        return;
    }

    /* Same-scope binary ops. */
    if (k == &kind_node_comma) { mark_adds(n->u.node_comma.lhs); mark_adds(n->u.node_comma.rhs); return; }
    if (k == &kind_node_add_inplace) { mark_adds(n->u.node_add_inplace.rhs); return; }
    if (k == &kind_node_sub) { mark_adds(n->u.node_sub.lhs); mark_adds(n->u.node_sub.rhs); return; }
    if (k == &kind_node_mul) { mark_adds(n->u.node_mul.lhs); mark_adds(n->u.node_mul.rhs); return; }
    if (k == &kind_node_div) { mark_adds(n->u.node_div.lhs); mark_adds(n->u.node_div.rhs); return; }
    if (k == &kind_node_mod) { mark_adds(n->u.node_mod.lhs); mark_adds(n->u.node_mod.rhs); return; }
    if (k == &kind_node_eq)  { mark_adds(n->u.node_eq.lhs); mark_adds(n->u.node_eq.rhs); return; }
    if (k == &kind_node_ne)  { mark_adds(n->u.node_ne.lhs); mark_adds(n->u.node_ne.rhs); return; }
    if (k == &kind_node_lt)  { mark_adds(n->u.node_lt.lhs); mark_adds(n->u.node_lt.rhs); return; }
    if (k == &kind_node_le)  { mark_adds(n->u.node_le.lhs); mark_adds(n->u.node_le.rhs); return; }
    if (k == &kind_node_gt)  { mark_adds(n->u.node_gt.lhs); mark_adds(n->u.node_gt.rhs); return; }
    if (k == &kind_node_ge)  { mark_adds(n->u.node_ge.lhs); mark_adds(n->u.node_ge.rhs); return; }
    if (k == &kind_node_and) { mark_adds(n->u.node_and.lhs); mark_adds(n->u.node_and.rhs); return; }
    if (k == &kind_node_or)  { mark_adds(n->u.node_or.lhs); mark_adds(n->u.node_or.rhs); return; }
    if (k == &kind_node_alt) { mark_adds(n->u.node_alt.lhs); mark_adds(n->u.node_alt.rhs); return; }
    if (k == &kind_node_neg) { mark_adds(n->u.node_neg.expr); return; }
    if (k == &kind_node_index) { mark_adds(n->u.node_index.expr); return; }
    if (k == &kind_node_index_opt) { mark_adds(n->u.node_index_opt.expr); return; }
    if (k == &kind_node_slice) {
        mark_adds(n->u.node_slice.startn);
        mark_adds(n->u.node_slice.stopn);
        return;
    }
    if (k == &kind_node_slice_opt) {
        mark_adds(n->u.node_slice_opt.startn);
        mark_adds(n->u.node_slice_opt.stopn);
        return;
    }
    if (k == &kind_node_array) { mark_adds(n->u.node_array.body); return; }
    if (k == &kind_node_emit_count)    { mark_adds(n->u.node_emit_count.body); return; }
    if (k == &kind_node_emit_fold_add) { mark_adds(n->u.node_emit_fold_add.body); return; }
    if (k == &kind_node_if) {
        mark_adds(n->u.node_if.cond);
        mark_adds(n->u.node_if.thn);
        mark_adds(n->u.node_if.els);
        return;
    }
    if (k == &kind_node_try) {
        mark_adds(n->u.node_try.body);
        mark_adds(n->u.node_try.handler);
        return;
    }
    if (k == &kind_node_as) {
        mark_adds(n->u.node_as.src);
        mark_adds(n->u.node_as.body);
        return;
    }
    if (k == &kind_node_label) { mark_adds(n->u.node_label.body); return; }
    if (k == &kind_node_b_select) { mark_adds(n->u.node_b_select.body); return; }
    if (k == &kind_node_error1) { mark_adds(n->u.node_error1.expr); return; }
    if (k == &kind_node_defs) { mark_adds(n->u.node_defs.body); return; }
    /* leaf or unknown: nothing to mark. */
}

static void
analyze_scope(struct Node *root)
{
    if (!root) return;
    struct ScopeCtx s = {0};
    s.alias_top = 0;
    s.dot_uses = 0;
    s.unknown = false;
    int total = dot_count(root, &s);
    if (getenv("NUQ_LIN_TRACE")) {
        fprintf(stderr, "[lin] scope root kind=%s total=%d unknown=%d\n",
                root->head.kind ? root->head.kind->default_dispatcher_name : "?",
                total, (int)s.unknown);
    }
    if (!s.unknown && total == 1) {
        int before = g_marks;
        mark_adds(root);
        if (getenv("NUQ_LIN_TRACE"))
            fprintf(stderr, "[lin]   marked %d add nodes\n", g_marks - before);
    }
}

/* External entry point — analyze the top-level filter and any
 * already-defined function bodies. */
void
nuq_linearity_analyze(struct Node *root)
{
    analyze_scope(root);
}

/* Apply analysis to a single user-def body (called from
 * nuq_compile_all_def_bodies / runtime def-loading paths). */
void
nuq_linearity_analyze_def_body(struct Node *body)
{
    analyze_scope(body);
}
