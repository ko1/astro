#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "context.h"
#include "object.h"
#include "node.h"
/* node_eval.c bodies may use ARO_ROOT_SCOPE_* (= for stale C-local
 * protection in @noinline node bodies that EVAL_ARG into user code).
 * Pull gc.h before node_eval.c is included below. */
#include "precise_gc/gc.h"

/* Hash helpers + HASH / DUMP / hash_node / alloc_dispatcher_name come
 * from runtime/astro_node.c.  We forward-declare node_allocate (the one
 * host hook the shared runtime expects), then include it. */
static __attribute__((noinline)) NODE *node_allocate(size_t size);

#include "../../runtime/astro_node.c"

void clear_hash(NODE *n) {
    while (n) {
        n->head.flags.has_hash_value = false;
        n = n->head.parent;
    }
}

/* node allocation via Boehm GC.  Forward-declared above the
 * runtime/astro_node.c include so the runtime helpers can reach it. */
extern size_t korb_node_cnt;
size_t korb_node_cnt = 0;

static __attribute__((noinline)) NODE *
node_allocate(size_t size) {
    NODE *n = (NODE *)korb_xmalloc(size);
    if (!n) { perror("node_allocate"); exit(1); }
    korb_node_cnt++;
    return n;
}

/* code repo */
struct code_repo {
    uint32_t size, capa;
    struct code_entry {
        const char *name;
        NODE *body;
    } *entries;
};
struct code_repo code_repo;

NODE *code_repo_find(node_hash_t h) {
    if (!h) return NULL;
    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (HASH(code_repo.entries[i].body) == h) return code_repo.entries[i].body;
    }
    return NULL;
}

void code_repo_add(const char *name, NODE *body, bool force) {
    if (!body) return;
    if (!force && code_repo_find(HASH(body))) return;
    if (code_repo.size >= code_repo.capa) {
        code_repo.capa = code_repo.capa ? code_repo.capa * 2 : 8;
        code_repo.entries = korb_xrealloc(code_repo.entries, code_repo.capa * sizeof(*code_repo.entries));
    }
    code_repo.entries[code_repo.size].name = name;
    code_repo.entries[code_repo.size].body = body;
    code_repo.size++;
}

/* alloc_dispatcher_name, astro_fprintf_cstr, astro_fprint_cstr come
 * from runtime/astro_node.c. */

/* OPTIMIZE / SPECIALIZE */

/* Code store: AOT lookup goes through the shared runtime/.  Pulled in
 * after astro_node.c so the runtime can use hash_merge / hash_node /
 * alloc_dispatcher_name etc. that astro_node.c provides. */
#include "../../runtime/astro_code_store.h"

NODE *OPTIMIZE(NODE *n) {
    if (!n) return n;
    if (OPTION.no_compiled_code) return n;
    /* Look up in the runtime code store (dlopen'd all.so).  AOT-only —
     * pass file=NULL so PGC lookup is skipped. */
    astro_cs_load(n, NULL);
    return n;
}

/* SPECIALIZE comes from runtime/astro_code_store.c (included below). */

void node_replace(NODE *parent, NODE *old, NODE *new_node) {
    if (!parent || !parent->head.kind->replacer) return;
    parent->head.kind->replacer(parent, old, new_node);
    clear_hash(parent);
    if (new_node) new_node->head.parent = parent;
}

void korb_swap_dispatcher(NODE *n, const struct NodeKind *new_kind) {
    n->head.kind = new_kind;
    n->head.dispatcher = new_kind->default_dispatcher;
    n->head.dispatcher_name = new_kind->default_dispatcher_name;
    n->head.flags.has_hash_value = false;
    if (n->head.parent) clear_hash(n->head.parent);
}

/* UNWRAP is provided by context.h (RESULT extraction with state
 * propagation).  Phase 8d (2026-05-29): koruby's DISPATCH / EVAL now
 * return RESULT — same convention as baruby / castro. */

/* include generated files */
#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_walk.c"
#if defined(__has_include) && __has_include("node_emit_ast.c")
#include "node_emit_ast.c"
#endif
#include "node_alloc.c"

/* ---------------- sp_offset baking walker ----------------
 *
 * After parse, traverse the AST and patch node_lvar_get / node_lvar_set
 * `index` fields: convert from positive frame index to negative
 * `sp_offset` (= index - scope_size).  The lvar body then dispatches
 * on the sign — negative → `sp[sp_offset]` (= sp-relative, GC-safe),
 * non-negative → fallback `c->current_frame->fp[index]` (= old behavior, used when
 * scope_size is unknown).
 *
 * Scope-introducing nodes (def/block/scope) reset scope_size to their
 * own locals_cnt/env_size before recursing into `body`.  Other children
 * (e.g. node_def_full has none besides body; node_obj_singleton_def has
 * recv_expr which uses the outer scope) are walked with the outer
 * scope_size — handled per-kind below.
 *
 * Top-level (= no enclosing method) starts with scope_size = 0; the
 * outermost `node_scope` wrapper bumps it to the program's envsize. */
struct bake_ctx { int32_t scope_size; };

static void bake_visit(NODE *n, void *ctx_);

static void
bake_patch_lvar(NODE *n, int32_t scope_size)
{
    int32_t *idx_field;
    if (n->head.kind == &kind_node_lvar_get) {
        idx_field = (int32_t *)&n->u.node_lvar_get.index;
    } else if (n->head.kind == &kind_node_lvar_set) {
        idx_field = (int32_t *)&n->u.node_lvar_set.index;
    } else {
        return;
    }
    int32_t idx = *idx_field;
    if (idx < 0) return; /* already patched */
    int32_t off = idx - scope_size;
    if (off >= 0) return; /* idx >= scope_size: outer-scope ref, keep as fp[] */
    *idx_field = off;
}

static inline void
bake_recurse_body(NODE *body, int32_t new_scope_size)
{
    if (!body) return;
    struct bake_ctx ctx = { .scope_size = new_scope_size };
    bake_visit(body, &ctx);
}

static void
bake_visit(NODE *n, void *ctx_)
{
    if (!n) return;
    struct bake_ctx *ctx = (struct bake_ctx *)ctx_;
    const struct NodeKind *k = n->head.kind;

    /* lvar nodes: patch index → sp_offset. */
    if (k == &kind_node_lvar_get) {
        bake_patch_lvar(n, ctx->scope_size);
        return;
    }
    if (k == &kind_node_lvar_set) {
        bake_patch_lvar(n, ctx->scope_size);
        if (n->u.node_lvar_set.rhs) bake_visit(n->u.node_lvar_set.rhs, ctx);
        return;
    }

    /* Scope boundary: switch scope_size, recurse into `body` only. */
    if (k == &kind_node_scope) {
        bake_recurse_body(n->u.node_scope.body, (int32_t)n->u.node_scope.envsize);
        return;
    }
    if (k == &kind_node_def) {
        bake_recurse_body(n->u.node_def.body, (int32_t)n->u.node_def.locals_cnt);
        return;
    }
    if (k == &kind_node_def_full) {
        bake_recurse_body(n->u.node_def_full.body, (int32_t)n->u.node_def_full.locals_cnt);
        return;
    }
    if (k == &kind_node_def_post) {
        bake_recurse_body(n->u.node_def_post.body, (int32_t)n->u.node_def_post.locals_cnt);
        return;
    }
    if (k == &kind_node_singleton_def) {
        bake_recurse_body(n->u.node_singleton_def.body, (int32_t)n->u.node_singleton_def.locals_cnt);
        return;
    }
    if (k == &kind_node_singleton_def_post) {
        bake_recurse_body(n->u.node_singleton_def_post.body, (int32_t)n->u.node_singleton_def_post.locals_cnt);
        return;
    }
    if (k == &kind_node_obj_singleton_def) {
        bake_visit(n->u.node_obj_singleton_def.recv_expr, ctx);
        bake_recurse_body(n->u.node_obj_singleton_def.body, (int32_t)n->u.node_obj_singleton_def.locals_cnt);
        return;
    }
    if (k == &kind_node_obj_singleton_def_post) {
        bake_visit(n->u.node_obj_singleton_def_post.recv_expr, ctx);
        bake_recurse_body(n->u.node_obj_singleton_def_post.body, (int32_t)n->u.node_obj_singleton_def_post.locals_cnt);
        return;
    }
    if (k == &kind_node_block_literal) {
        bake_recurse_body(n->u.node_block_literal.body, (int32_t)n->u.node_block_literal.env_size);
        return;
    }
    if (k == &kind_node_block_literal_rest) {
        bake_recurse_body(n->u.node_block_literal_rest.body, (int32_t)n->u.node_block_literal_rest.env_size);
        return;
    }
    if (k == &kind_node_block_literal_kw) {
        bake_recurse_body(n->u.node_block_literal_kw.body, (int32_t)n->u.node_block_literal_kw.env_size);
        return;
    }

    /* Default: walk all children with current scope_size. */
    if (k->walker) k->walker(n, bake_visit, ctx);
}

void
koruby_bake_sp_offsets(NODE *root)
{
    struct bake_ctx ctx = { .scope_size = 0 };
    bake_visit(root, &ctx);
}

/* Pulled in last — uses HASH/HORG/HOPT and the static helpers from
 * astro_node.c above. */
#include "../../runtime/astro_code_store.c"

/* Build orchestrator (used by --generate-executable in main.c). */
#include "../../runtime/astro_build.c"

void INIT(void) {
    /* nothing — kept as a stable symbol for exe_main.c and main.c. */
}
