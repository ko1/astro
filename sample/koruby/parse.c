/* prism -> koruby AST converter */
#include "prism.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "context.h"
#include "object.h"
#include "node.h"

/* per-frame parsing context.  Slots are absolute indices into the runtime
 * fp.  Blocks (inner frames) sit on top of their lexical parent and share
 * its fp — so an enclosed block writing to outer's local writes straight
 * into the same fp slot.  slot_base is where this frame's named locals
 * begin in the absolute fp coordinate system. */
struct frame_context {
    pm_constant_id_list_t *locals;
    uint32_t slot_base;   /* absolute slot of locals[0] */
    uint32_t arg_index;   /* next free absolute slot for arg staging */
    uint32_t max_cnt;     /* highest absolute slot ever used */
    bool is_block;        /* true for block frames (share parent fp) */
    /* `def f(...)` forwarding: hidden slots holding the captured *args,
     * **kwargs, and &blk so the body's `f(...)` calls can splat them. */
    int fwd_rest_slot;
    int fwd_kwh_slot;
    int fwd_blk_slot;
    struct frame_context *prev;
};

struct transduce_context {
    struct frame_context *frame;
    pm_parser_t *parser;
    bool verbose;
    int last_line;
};

static NODE *T(struct transduce_context *tc, pm_node_t *n);

static const char *
alloc_cstr(pm_parser_t *parser, pm_constant_id_t cid) {
    pm_constant_t *c = pm_constant_pool_id_to_constant(&parser->constant_pool, cid);
    char *s = korb_xmalloc_atomic(c->length + 1);
    memcpy(s, c->start, c->length);
    s[c->length] = 0;
    return s;
}

static ID
intern_constant(pm_parser_t *parser, pm_constant_id_t cid) {
    pm_constant_t *c = pm_constant_pool_id_to_constant(&parser->constant_pool, cid);
    return korb_intern_n((const char *)c->start, (long)c->length);
}

static struct method_cache *alloc_method_cache(void) {
    return korb_xcalloc(1, sizeof(struct method_cache));
}

static struct ivar_cache *alloc_ivar_cache(void) {
    struct ivar_cache *c = korb_xcalloc(1, sizeof(*c));
    c->slot = -1;
    return c;
}

static void push_frame(struct transduce_context *tc, pm_constant_id_list_t *locals, bool is_block) {
    struct frame_context *f = korb_xmalloc(sizeof(*f));
    f->prev = tc->frame;
    f->locals = locals;
    f->is_block = is_block;
    /* For block frames, slot_base is parent's current arg_index — i.e. just
     * above any previously-staged value.  This sits the block's locals on
     * top of any temporaries the parent had reserved, which is correct for
     * shared-fp closures because parent will not run again until the block
     * returns (we're still inside the parent's expression evaluation). */
    if (is_block && tc->frame) {
        /* Need to position above parent's current max_cnt so we never trample
         * parent's locals/temps. */
        f->slot_base = tc->frame->max_cnt;
    } else {
        f->slot_base = 0;
    }
    f->arg_index = f->slot_base + (locals ? locals->size : 0);
    f->max_cnt = f->arg_index;
    f->fwd_rest_slot = -1;
    f->fwd_kwh_slot = -1;
    f->fwd_blk_slot = -1;
    tc->frame = f;
}

static void pop_frame(struct transduce_context *tc) {
    /* Propagate block's usage up so parent's allocation is big enough. */
    struct frame_context *child = tc->frame;
    struct frame_context *parent = child->prev;
    if (parent && child->is_block) {
        if (child->max_cnt > parent->max_cnt) parent->max_cnt = child->max_cnt;
    }
    tc->frame = parent;
}

static int lvar_index_in(pm_constant_id_list_t *list, pm_constant_id_t cid) {
    for (size_t i = 0; i < list->size; i++) if (list->ids[i] == cid) return (int)i;
    return -1;
}

/* Find lvar by walking enclosing frames.  Returns the absolute slot. */
static int lvar_slot(struct transduce_context *tc, pm_constant_id_t cid, uint32_t depth) {
    struct frame_context *f = tc->frame;
    for (uint32_t i = 0; i < depth && f; i++) f = f->prev;
    if (!f) return -1;
    int idx = lvar_index_in(f->locals, cid);
    if (idx < 0) return -1;
    return f->slot_base + idx;
}

/* Search any enclosing frame for the var; returns absolute slot or -1.
 * Used when prism doesn't tell us the depth. */
static int lvar_slot_any(struct transduce_context *tc, pm_constant_id_t cid) {
    for (struct frame_context *f = tc->frame; f; f = f->prev) {
        int idx = lvar_index_in(f->locals, cid);
        if (idx >= 0) return f->slot_base + idx;
        if (!f->is_block) break; /* method/program frame is opaque to outer */
    }
    return -1;
}

static uint32_t arg_index(struct transduce_context *tc) { return tc->frame->arg_index; }

static uint32_t inc_arg_index(struct transduce_context *tc) {
    uint32_t i = tc->frame->arg_index++;
    if (tc->frame->arg_index > tc->frame->max_cnt) tc->frame->max_cnt = tc->frame->arg_index;
    return i;
}

static void rewind_arg_index(struct transduce_context *tc, uint32_t to) {
    tc->frame->arg_index = to;
}

static bool ceq(struct transduce_context *tc, pm_constant_id_t cid, const char *s) {
    pm_constant_t *c = pm_constant_pool_id_to_constant(&tc->parser->constant_pool, cid);
    size_t len = strlen(s);
    return c->length == len && memcmp(c->start, s, len) == 0;
}

/* binop detection */
static bool is_binop_name(struct transduce_context *tc, pm_constant_id_t name) {
    static const char *ops[] = {"+","-","*","/","%","<","<=",">",">=","==","!=","<<",">>","&","|","^","**", NULL};
    for (int i = 0; ops[i]; i++) if (ceq(tc, name, ops[i])) return true;
    return false;
}

static NODE *alloc_binop(struct transduce_context *tc, pm_constant_id_t name, NODE *l, NODE *r) {
    uint32_t ai = arg_index(tc);
    /* reserve 2 slots for fallback method dispatch */
    inc_arg_index(tc); inc_arg_index(tc);
    rewind_arg_index(tc, ai);
    if (ceq(tc, name, "+"))  return ALLOC_node_plus(l, r, ai);
    if (ceq(tc, name, "-"))  return ALLOC_node_minus(l, r, ai);
    if (ceq(tc, name, "*"))  return ALLOC_node_mul(l, r, ai);
    if (ceq(tc, name, "/"))  return ALLOC_node_div(l, r, ai);
    if (ceq(tc, name, "%"))  return ALLOC_node_mod(l, r, ai);
    if (ceq(tc, name, "<"))  return ALLOC_node_lt(l, r, ai);
    if (ceq(tc, name, "<=")) return ALLOC_node_le(l, r, ai);
    if (ceq(tc, name, ">"))  return ALLOC_node_gt(l, r, ai);
    if (ceq(tc, name, ">=")) return ALLOC_node_ge(l, r, ai);
    if (ceq(tc, name, "==")) return ALLOC_node_eq(l, r, ai);
    if (ceq(tc, name, "!=")) return ALLOC_node_neq(l, r, ai);
    if (ceq(tc, name, "<<")) return ALLOC_node_lshift(l, r, ai);
    if (ceq(tc, name, ">>")) return ALLOC_node_rshift(l, r, ai);
    if (ceq(tc, name, "&"))  return ALLOC_node_bit_and(l, r, ai);
    if (ceq(tc, name, "|"))  return ALLOC_node_bit_or(l, r, ai);
    if (ceq(tc, name, "^"))  return ALLOC_node_bit_xor(l, r, ai);
    if (ceq(tc, name, "**")) {
        /* No specialized node for ** — call as a method on l */
        struct method_cache *mc = alloc_method_cache();
        NODE *seq_arg = ALLOC_node_lvar_set(ai, r);
        return ALLOC_node_seq(seq_arg, ALLOC_node_method_call(l, korb_intern("**"), 1, ai, mc));
    }
    return NULL;
}

/* Build a sequence of statements using node_seq.
 * pm_statements_node has 'body' = pm_node_list_t. */
static NODE *transduce_statements(struct transduce_context *tc, pm_statements_node_t *sn) {
    if (!sn || sn->body.size == 0) return ALLOC_node_nil();
    NODE *result = NULL;
    for (size_t i = 0; i < sn->body.size; i++) {
        NODE *cur = T(tc, sn->body.nodes[i]);
        if (!cur) cur = ALLOC_node_nil();
        if (!result) result = cur;
        else result = ALLOC_node_seq(result, cur);
    }
    return result;
}

static NODE *
build_container(struct transduce_context *tc, pm_node_list_t *items, bool is_array, bool is_hash, bool is_str_concat);

/* Build a single Array NODE that flattens splatted args at runtime.
 * For `[a, *b, c]` form: build [a] + b.to_a + [c]. */
static NODE *
build_args_array_with_splat(struct transduce_context *tc, pm_node_list_t *args)
{
    NODE *result = NULL;
    size_t i = 0;
    while (i < args->size) {
        if (PM_NODE_TYPE_P(args->nodes[i], PM_SPLAT_NODE)) {
            pm_splat_node_t *sn = (pm_splat_node_t *)args->nodes[i];
            NODE *splatted = sn->expression
                ? ALLOC_node_splat_to_ary(T(tc, sn->expression))
                : ALLOC_node_ary_new(0, 0);
            result = result ? ALLOC_node_ary_concat(result, splatted) : splatted;
            i++;
        } else {
            size_t j = i;
            while (j < args->size && !PM_NODE_TYPE_P(args->nodes[j], PM_SPLAT_NODE)) j++;
            pm_node_list_t sub = { 0 };
            sub.size = sub.capacity = j - i;
            sub.nodes = &args->nodes[i];
            NODE *part = build_container(tc, &sub, true, false, false);
            result = result ? ALLOC_node_ary_concat(result, part) : part;
            i = j;
        }
    }
    return result ? result : ALLOC_node_ary_new(0, 0);
}

static NODE *build_call_simple(struct transduce_context *tc, NODE *recv, ID name,
                                pm_node_list_t *args, NODE *block_node, bool is_method);

/* Wrapper: build a call where the block is given as a prism node, so we
 * can construct the block AFTER reserving call arg slots — making the
 * block's param_base sit above the staging area. */
static NODE *
build_call_with_block(struct transduce_context *tc, NODE *recv, ID name,
                       pm_node_list_t *args, pm_node_t *block_pm, bool is_method)
{
    /* `&expr` block-pass — appears in arguments list (rare; usually
     * lands in n->block at the call-node level). */
    if (!block_pm && args) {
        for (uint32_t i = 0; i < args->size; i++) {
            if (args->nodes[i] && PM_NODE_TYPE_P(args->nodes[i], PM_BLOCK_ARGUMENT_NODE)) {
                pm_block_argument_node_t *ba = (pm_block_argument_node_t *)args->nodes[i];
                /* Build a private args list without the block-arg. */
                pm_node_list_t real_args = { .size = 0, .capacity = args->capacity, .nodes = args->nodes };
                /* In-place compaction copy onto a stack-buffer is fine
                 * for small arg counts. */
                pm_node_t *buf[16];
                if (args->size > 16) {
                    /* Fallback: skip rewrite; treat as ordinary call. */
                    return build_call_simple(tc, recv, name, args, NULL, is_method);
                }
                uint32_t bn = 0;
                for (uint32_t j = 0; j < args->size; j++) {
                    if (j == i) continue;
                    buf[bn++] = args->nodes[j];
                }
                real_args.size = bn;
                real_args.nodes = buf;
                /* Build `expr.to_proc` node manually. */
                NODE *expr = ba->expression ? T(tc, ba->expression) : ALLOC_node_nil();
                struct method_cache *mc = alloc_method_cache();
                NODE *to_proc = ALLOC_node_method_call(expr, korb_intern("to_proc"), 0, arg_index(tc), mc);
                return build_call_simple(tc, recv, name, &real_args, to_proc, is_method);
            }
        }
    }
    if (!block_pm) {
        return build_call_simple(tc, recv, name, args, NULL, is_method);
    }
    pm_block_node_t *bn = (pm_block_node_t *)block_pm;
    uint32_t params_cnt = 0;
    int block_rest_slot_pre = -1;     /* set below once frame is pushed */
    pm_constant_id_t block_rest_name = 0;
    if (bn->parameters && PM_NODE_TYPE_P(bn->parameters, PM_BLOCK_PARAMETERS_NODE)) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)bn->parameters;
        if (bp->parameters && PM_NODE_TYPE_P((pm_node_t *)bp->parameters, PM_PARAMETERS_NODE)) {
            pm_parameters_node_t *pn = (pm_parameters_node_t *)bp->parameters;
            params_cnt = (uint32_t)pn->requireds.size;
            if (pn->rest && PM_NODE_TYPE_P(pn->rest, PM_REST_PARAMETER_NODE)) {
                pm_rest_parameter_node_t *rp = (pm_rest_parameter_node_t *)pn->rest;
                if (rp->name) block_rest_name = rp->name;
            }
        }
    }
    /* Reserve slots for recv (if any) + args first, so block's slot_base
     * lands past them. */
    uint32_t arg_cnt = args ? (uint32_t)args->size : 0;
    uint32_t saved_arg_index = arg_index(tc);
    /* Reserve recv slot (only if there are args) and arg slots */
    bool reserve_recv = recv && arg_cnt > 0;
    uint32_t reserve_n = (reserve_recv ? 1 : 0) + arg_cnt;
    for (uint32_t r = 0; r < reserve_n; r++) inc_arg_index(tc);

    /* Now build block — its slot_base = parent.max_cnt = past the
     * reserved staging slots. */
    push_frame(tc, &bn->locals, true);
    uint32_t param_base = tc->frame->slot_base;
    /* Build a destructure prelude for any `(a, b)` style block params.
     * Each MULTI_TARGET param sits in its own param slot at runtime;
     * we read it as an Array and assign each component to the named
     * lvar before the body runs. */
    NODE *destructure_pre = NULL;
    if (bn->parameters && PM_NODE_TYPE_P(bn->parameters, PM_BLOCK_PARAMETERS_NODE)) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)bn->parameters;
        if (bp->parameters && PM_NODE_TYPE_P((pm_node_t *)bp->parameters, PM_PARAMETERS_NODE)) {
            pm_parameters_node_t *pn = (pm_parameters_node_t *)bp->parameters;
            /* Two passes: first snapshot all MULTI_TARGET param slots to
             * fresh temps (so later destructure writes don't clobber
             * not-yet-snapshot params), then expand each one. */
            uint32_t saved_tmp[16] = {0};
            int n_mt = 0;
            for (size_t i = 0; i < pn->requireds.size && n_mt < 16; i++) {
                if (PM_NODE_TYPE_P(pn->requireds.nodes[i], PM_MULTI_TARGET_NODE)) {
                    uint32_t holder_slot = param_base + (uint32_t)i;
                    uint32_t tmp_slot = inc_arg_index(tc);
                    saved_tmp[i] = tmp_slot;
                    NODE *snap = ALLOC_node_lvar_set(tmp_slot, ALLOC_node_lvar_get(holder_slot));
                    destructure_pre = destructure_pre ? ALLOC_node_seq(destructure_pre, snap) : snap;
                    n_mt++;
                }
            }
            for (size_t i = 0; i < pn->requireds.size; i++) {
                pm_node_t *req = pn->requireds.nodes[i];
                if (!PM_NODE_TYPE_P(req, PM_MULTI_TARGET_NODE)) continue;
                pm_multi_target_node_t *mt = (pm_multi_target_node_t *)req;
                uint32_t tmp_slot = saved_tmp[i];
                for (size_t j = 0; j < mt->lefts.size; j++) {
                    pm_node_t *t = mt->lefts.nodes[j];
                    ID name_id = 0;
                    uint32_t name_depth = 0;
                    if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                        pm_local_variable_target_node_t *lt = (pm_local_variable_target_node_t *)t;
                        name_id = lt->name; name_depth = lt->depth;
                    } else if (PM_NODE_TYPE_P(t, PM_REQUIRED_PARAMETER_NODE)) {
                        pm_required_parameter_node_t *rp = (pm_required_parameter_node_t *)t;
                        name_id = rp->name;
                    } else continue;
                    int slot = lvar_slot(tc, name_id, name_depth);
                    if (slot < 0) slot = lvar_slot_any(tc, name_id);
                    if (slot < 0) continue;
                    NODE *get = ALLOC_node_ary_aget(ALLOC_node_lvar_get(tmp_slot), (uint32_t)j);
                    NODE *set = ALLOC_node_lvar_set((uint32_t)slot, get);
                    destructure_pre = ALLOC_node_seq(destructure_pre, set);
                }
            }
        }
    }
    /* Resolve *rest's slot now that the block frame is up. */
    if (block_rest_name) {
        int rs = lvar_slot(tc, block_rest_name, 0);
        if (rs >= 0) block_rest_slot_pre = rs;
    }
    NODE *body = bn->body ? T(tc, bn->body) : ALLOC_node_nil();
    if (destructure_pre) body = ALLOC_node_seq(destructure_pre, body);
    uint32_t env_size = tc->frame->max_cnt;
    pop_frame(tc);
    NODE *block_node;
    if (block_rest_slot_pre >= 0) {
        block_node = ALLOC_node_block_literal_rest(body, params_cnt, param_base,
                                                    env_size, (int32_t)block_rest_slot_pre);
    } else {
        block_node = ALLOC_node_block_literal(body, params_cnt, param_base, env_size);
    }
    /* Register block body so AOT (--aot-compile) emits an SD for it.
     * Without this, the block dispatcher stays at DISPATCH_node_*
     * (interpreter), and the hot work inside `iters.times { ... }` —
     * the `while` loop, ivar set, method call — never gets specialized. */
    code_repo_add("<block>", body, false);

    /* Restore arg_index to original; build_call_simple will re-reserve. */
    rewind_arg_index(tc, saved_arg_index);

    return build_call_simple(tc, recv, name, args, block_node, is_method);
}

/* Build call: receiver is optional (NULL = func_call). args list is pm_arguments_node_t
   children (already known length). args_cnt = number of pre-evaluated args.
   block is optional. */
static NODE *
build_call_simple(struct transduce_context *tc, NODE *recv, ID name,
                  pm_node_list_t *args, NODE *block_node, bool is_method)
{
    uint32_t arg_cnt = args ? (uint32_t)args->size : 0;
    uint32_t call_arg_idx = arg_index(tc);

    /* `f(...)` — arguments contain a single PM_FORWARDING_ARGUMENTS_NODE.
     * Forward via the captured fwd_* slots set when entering `def f(...)`. */
    if (args && arg_cnt == 1 &&
        PM_NODE_TYPE_P(args->nodes[0], PM_FORWARDING_ARGUMENTS_NODE)) {
        struct frame_context *df = tc->frame;
        while (df && df->fwd_rest_slot < 0) df = df->prev;
        if (df) {
            int fr = df->fwd_rest_slot;
            int fk = df->fwd_kwh_slot;
            int fb = df->fwd_blk_slot;
            /* args_array = fwd_rest + [fwd_kwh]   (kwh as last positional) */
            uint32_t ai = inc_arg_index(tc);
            inc_arg_index(tc); rewind_arg_index(tc, ai);
            struct method_cache *mc_one = alloc_method_cache();
            NODE *one = ALLOC_node_method_call(ALLOC_node_lvar_get((uint32_t)fk),
                                                korb_intern("any?"), 0, ai, mc_one);
            (void)one;
            /* Always include kwh; receiver doesn't need to know it's empty. */
            uint32_t ai2 = inc_arg_index(tc);
            inc_arg_index(tc); rewind_arg_index(tc, ai2);
            struct method_cache *mc_push = alloc_method_cache();
            /* args_array = fwd_rest.dup; args_array.push(fwd_kwh) */
            uint32_t saved_slot = inc_arg_index(tc);
            uint32_t ai_dup = inc_arg_index(tc);
            rewind_arg_index(tc, ai_dup);
            struct method_cache *mc_dup = alloc_method_cache();
            NODE *dup_call = ALLOC_node_method_call(ALLOC_node_lvar_get((uint32_t)fr),
                                                    korb_intern("dup"), 0, ai_dup, mc_dup);
            NODE *save_arr = ALLOC_node_lvar_set(saved_slot, dup_call);
            /* Only push kwh if non-empty: forwarding to a method that
             * doesn't take kwargs would otherwise see a stray {}. */
            uint32_t ai_p = inc_arg_index(tc);
            inc_arg_index(tc); rewind_arg_index(tc, ai_p);
            struct method_cache *mc_p = alloc_method_cache();
            uint32_t ai_any = inc_arg_index(tc);
            rewind_arg_index(tc, ai_any);
            struct method_cache *mc_any = alloc_method_cache();
            NODE *non_empty = ALLOC_node_method_call(
                ALLOC_node_lvar_get((uint32_t)fk),
                korb_intern("any?"), 0, ai_any, mc_any);
            NODE *karg = ALLOC_node_lvar_set(ai_p, ALLOC_node_lvar_get((uint32_t)fk));
            NODE *do_push = ALLOC_node_seq(karg,
                ALLOC_node_method_call(ALLOC_node_lvar_get(saved_slot),
                                       korb_intern("push"), 1, ai_p, mc_p));
            NODE *push_kwh = ALLOC_node_if(non_empty, do_push, ALLOC_node_nil());
            NODE *args_array = ALLOC_node_seq(save_arr,
                ALLOC_node_seq(push_kwh, ALLOC_node_lvar_get(saved_slot)));

            /* reserve up to 16 staging slots for apply_call. */
            uint32_t apply_idx = inc_arg_index(tc);
            for (int s = 1; s < 16; s++) inc_arg_index(tc);
            rewind_arg_index(tc, call_arg_idx);
            struct method_cache *mc = alloc_method_cache();
            NODE *blk = ALLOC_node_lvar_get((uint32_t)fb);
            return ALLOC_node_apply_call(recv ? recv : ALLOC_node_self(), name,
                                          args_array, apply_idx, blk,
                                          is_method ? 1 : 0, mc);
        }
    }

    /* Detect splat: if any arg is a splat, use the apply-style call which
     * builds a runtime array of args and copies it into staging slots. */
    bool has_splat = false;
    if (args) {
        for (uint32_t i = 0; i < arg_cnt; i++) {
            if (PM_NODE_TYPE_P(args->nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
        }
    }
    if (has_splat) {
        NODE *args_array = build_args_array_with_splat(tc, args);
        struct method_cache *mc = alloc_method_cache();
        /* reserve up to a sane maximum staging slots; we use 16 for now. */
        for (int s = 0; s < 16; s++) inc_arg_index(tc);
        rewind_arg_index(tc, call_arg_idx);
        NODE *blk = block_node ? block_node : ALLOC_node_nil();
        return ALLOC_node_apply_call(recv ? recv : ALLOC_node_self(), name, args_array,
                                     call_arg_idx, blk, is_method ? 1 : 0, mc);
    }

    /* Non-splat path.
     * IMPORTANT: When the call has args + a receiver expression, the
     * receiver expression may itself stage temporaries that overlap with
     * the arg slots we're about to use.  To avoid clobber, we evaluate
     * recv first into its own slot, then re-read it from that slot at
     * call site. */
    NODE *recv_set = NULL;
    NODE *recv_for_call = recv;
    if (recv && arg_cnt > 0) {
        uint32_t recv_slot = inc_arg_index(tc);
        recv_set = ALLOC_node_lvar_set(recv_slot, recv);
        recv_for_call = ALLOC_node_lvar_get(recv_slot);
        /* Re-read call_arg_idx since we've consumed one slot for recv;
         * args start at the new arg_index. */
        call_arg_idx = arg_index(tc);
    }

    uint32_t *slots = arg_cnt ? korb_xmalloc(sizeof(uint32_t) * arg_cnt) : NULL;
    for (uint32_t i = 0; i < arg_cnt; i++) slots[i] = inc_arg_index(tc);
    NODE *seq = recv_set;
    for (uint32_t i = 0; i < arg_cnt; i++) {
        NODE *arg = T(tc, args->nodes[i]);
        if (!arg) arg = ALLOC_node_nil();
        NODE *set = ALLOC_node_lvar_set(slots[i], arg);
        seq = seq ? ALLOC_node_seq(seq, set) : set;
    }
    NODE *call;
    struct method_cache *mc = alloc_method_cache();
    if (block_node) {
        if (is_method) {
            call = ALLOC_node_method_call_block(recv_for_call, name, arg_cnt, call_arg_idx, block_node, mc);
        } else {
            call = ALLOC_node_func_call_block(name, arg_cnt, call_arg_idx, block_node, mc);
        }
    } else {
        if (is_method) {
            call = ALLOC_node_method_call(recv_for_call, name, arg_cnt, call_arg_idx, mc);
        } else {
            call = ALLOC_node_func_call(name, arg_cnt, call_arg_idx, mc);
        }
    }
    /* Rewind to the original arg_idx so subsequent siblings reuse slots,
     * but keep max_cnt high enough (already done by inc_arg_index). */
    rewind_arg_index(tc, recv_set ? call_arg_idx - 1 : call_arg_idx);
    return seq ? ALLOC_node_seq(seq, call) : call;
}

/* For container literals: pre-evaluate items into successive arg slots.
 * IMPORTANT: reserve the slots BEFORE recursing into T() for the elements,
 * otherwise a nested compound (hash/array literal, method call) will allocate
 * its own staging from the same slot we're about to write — clobbering the
 * pending element at runtime. */
static NODE *
build_container(struct transduce_context *tc, pm_node_list_t *items, bool is_array, bool is_hash, bool is_str_concat)
{
    uint32_t n = (uint32_t)items->size;
    if (is_hash) n = 0;
    uint32_t arg_idx = arg_index(tc);
    NODE *seq = NULL;
    if (is_hash) {
        /* If any element is a `**splat`, build the hash via base + merges
         * instead of a single hash_new.  Treat the simple all-assoc case
         * with the original fast path. */
        bool has_splat = false;
        for (size_t i = 0; i < items->size; i++) {
            if (PM_NODE_TYPE_P(items->nodes[i], PM_ASSOC_SPLAT_NODE)) {
                has_splat = true;
                break;
            }
        }
        if (has_splat) {
            /* Build base = {} then walk: for assoc, base[k]=v; for splat,
             * base.merge!(splat_expr). */
            uint32_t base_slot = inc_arg_index(tc);
            NODE *base_init = ALLOC_node_lvar_set(base_slot,
                                                   ALLOC_node_hash_new(0, base_slot + 1));
            seq = base_init;
            for (size_t i = 0; i < items->size; i++) {
                pm_node_t *it = items->nodes[i];
                if (PM_NODE_TYPE_P(it, PM_ASSOC_NODE)) {
                    pm_assoc_node_t *as = (pm_assoc_node_t *)it;
                    NODE *kn = T(tc, as->key);
                    NODE *vn = T(tc, as->value);
                    uint32_t ai = inc_arg_index(tc);
                    inc_arg_index(tc); inc_arg_index(tc);
                    rewind_arg_index(tc, ai);
                    struct method_cache *mc = alloc_method_cache();
                    NODE *kset = ALLOC_node_lvar_set(ai, kn);
                    NODE *vset = ALLOC_node_lvar_set(ai + 1, vn);
                    NODE *call = ALLOC_node_method_call(ALLOC_node_lvar_get(base_slot),
                                                        korb_intern("[]="), 2, ai, mc);
                    seq = ALLOC_node_seq(seq,
                            ALLOC_node_seq(kset, ALLOC_node_seq(vset, call)));
                } else if (PM_NODE_TYPE_P(it, PM_ASSOC_SPLAT_NODE)) {
                    pm_assoc_splat_node_t *sn = (pm_assoc_splat_node_t *)it;
                    NODE *sval = sn->value ? T(tc, sn->value) : ALLOC_node_hash_new(0, base_slot + 1);
                    uint32_t ai = inc_arg_index(tc);
                    inc_arg_index(tc); rewind_arg_index(tc, ai);
                    struct method_cache *mc = alloc_method_cache();
                    NODE *aset = ALLOC_node_lvar_set(ai, sval);
                    /* hash_merge returns a new hash — re-assign base. */
                    NODE *call = ALLOC_node_method_call(ALLOC_node_lvar_get(base_slot),
                                                        korb_intern("merge"), 1, ai, mc);
                    NODE *update = ALLOC_node_lvar_set(base_slot, call);
                    seq = ALLOC_node_seq(seq, ALLOC_node_seq(aset, update));
                }
            }
            NODE *get = ALLOC_node_lvar_get(base_slot);
            rewind_arg_index(tc, arg_idx);
            return ALLOC_node_seq(seq, get);
        }
        /* First pass: reserve all key/value slot pairs.  This bumps the
         * frame's arg_index high so subsequent T() calls allocate fresh
         * slots that won't overlap with our pending writes. */
        size_t valid = 0;
        for (size_t i = 0; i < items->size; i++) {
            if (PM_NODE_TYPE_P(items->nodes[i], PM_ASSOC_NODE)) valid++;
        }
        uint32_t *slots = korb_xmalloc(sizeof(uint32_t) * valid * 2);
        for (size_t k = 0; k < valid * 2; k++) slots[k] = inc_arg_index(tc);
        size_t si = 0;
        for (size_t i = 0; i < items->size; i++) {
            pm_node_t *node = items->nodes[i];
            if (!PM_NODE_TYPE_P(node, PM_ASSOC_NODE)) continue;
            pm_assoc_node_t *as = (pm_assoc_node_t *)node;
            NODE *kn = T(tc, as->key);
            NODE *vn = T(tc, as->value);
            NODE *ks = ALLOC_node_lvar_set(slots[si++], kn);
            NODE *vs = ALLOC_node_lvar_set(slots[si++], vn);
            NODE *pair = ALLOC_node_seq(ks, vs);
            seq = seq ? ALLOC_node_seq(seq, pair) : pair;
            n += 2;
        }
    } else {
        /* Reserve slots for all elements first */
        uint32_t *slots = korb_xmalloc(sizeof(uint32_t) * n);
        for (uint32_t i = 0; i < n; i++) slots[i] = inc_arg_index(tc);
        for (uint32_t i = 0; i < n; i++) {
            NODE *en = T(tc, items->nodes[i]);
            if (!en) en = ALLOC_node_nil();
            NODE *st = ALLOC_node_lvar_set(slots[i], en);
            seq = seq ? ALLOC_node_seq(seq, st) : st;
        }
    }
    NODE *create;
    if (is_array)      create = ALLOC_node_ary_new(n, arg_idx);
    else if (is_hash)  create = ALLOC_node_hash_new(n, arg_idx);
    else /* str_concat */ create = ALLOC_node_str_concat(n, arg_idx);
    rewind_arg_index(tc, arg_idx);
    return seq ? ALLOC_node_seq(seq, create) : create;
}

static int integer_value_int32(pm_integer_t *integer, intptr_t *out) {
    if (integer->length == 0) {
        intptr_t v = (intptr_t)integer->value;
        if (integer->negative) v = -v;
        *out = v;
        return 1;
    }
    /* large integer: need bignum */
    return 0;
}

static char *integer_to_string(pm_integer_t *integer) {
    /* use prism API */
    pm_buffer_t buf = { 0 };
    pm_buffer_init(&buf);
    pm_integer_string(&buf, integer);
    char *s = korb_xmalloc_atomic(buf.length + 1);
    memcpy(s, buf.value, buf.length);
    s[buf.length] = 0;
    pm_buffer_free(&buf);
    return s;
}

/* Resolve the source line for a prism node by binary-searching the
 * parser's newline-offset table — pm_newline_list_line is internal so
 * we inline it here.  Returns 1-based line, or 0 if no parser. */
static int line_of_node(struct transduce_context *tc, pm_node_t *node) {
    if (!tc || !tc->parser || !node) return 0;
    const pm_newline_list_t *nl = &tc->parser->newline_list;
    if (!nl->offsets || nl->size == 0) return 0;
    size_t off = (size_t)(node->location.start - nl->start);
    long lo = 0, hi = (long)nl->size - 1, best = 0;
    while (lo <= hi) {
        long m = (lo + hi) / 2;
        if (nl->offsets[m] <= off) { best = m; lo = m + 1; }
        else hi = m - 1;
    }
    return (int)(best + 1);
}

static NODE *
T_inner(struct transduce_context *tc, pm_node_t *node);

static NODE *
T(struct transduce_context *tc, pm_node_t *node)
{
    NODE *r = T_inner(tc, node);
    if (r && node) r->head.line = line_of_node(tc, node);
    return r;
}

/* Pattern matching support: lower a prism pattern node to a NODE that
 * EAs to true/false (binding subpattern lvars as a side-effect).
 *
 * subj_slot is the absolute lvar slot holding the value being matched. */
static NODE *build_pattern_check(struct transduce_context *tc, pm_node_t *pat,
                                  uint32_t subj_slot)
{
    if (!pat) return ALLOC_node_true();

    switch (PM_NODE_TYPE(pat)) {
      case PM_LOCAL_VARIABLE_TARGET_NODE: {
          /* `in name` always matches; bind subject to `name`. */
          pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)pat;
          int slot = lvar_slot(tc, t->name, t->depth);
          if (slot < 0) slot = lvar_slot_any(tc, t->name);
          if (slot < 0) return ALLOC_node_true();
          NODE *bind = ALLOC_node_lvar_set((uint32_t)slot,
                                            ALLOC_node_lvar_get(subj_slot));
          return ALLOC_node_seq(bind, ALLOC_node_true());
      }

      case PM_PINNED_EXPRESSION_NODE: {
          pm_pinned_expression_node_t *p = (pm_pinned_expression_node_t *)pat;
          NODE *expr = T(tc, p->expression);
          uint32_t ai = inc_arg_index(tc);
          inc_arg_index(tc);
          rewind_arg_index(tc, ai);
          return ALLOC_node_eq(expr, ALLOC_node_lvar_get(subj_slot), ai);
      }

      case PM_PINNED_VARIABLE_NODE: {
          pm_pinned_variable_node_t *p = (pm_pinned_variable_node_t *)pat;
          NODE *expr = T(tc, (pm_node_t *)p->variable);
          uint32_t ai = inc_arg_index(tc);
          inc_arg_index(tc);
          rewind_arg_index(tc, ai);
          return ALLOC_node_eq(expr, ALLOC_node_lvar_get(subj_slot), ai);
      }

      case PM_ARRAY_PATTERN_NODE: {
          /* in [p1, p2, ..., *rest]
           *   coerced = subj.is_a?(Array) ? subj :
           *             (subj.respond_to?(:deconstruct) ? subj.deconstruct : nil)
           *   coerced.is_a?(Array) && coerced.size matches && p1(coerced[0]) && ... */
          pm_array_pattern_node_t *a = (pm_array_pattern_node_t *)pat;
          uint32_t req_cnt = (uint32_t)a->requireds.size;
          uint32_t post_cnt = (uint32_t)a->posts.size;
          bool has_rest = (a->rest != NULL);

          /* Coerce subj into an array via deconstruct if needed.  We
           * overwrite subj_slot so the rest of the pattern code sees the
           * coerced array.  (subj_slot is local to the case_match, so
           * it's safe to replace here.)  This `coerce_step` node
           * prepends to the returned check expression. */
          NODE *coerce_step;
          {
              uint32_t ai_isa1 = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, ai_isa1);
              struct method_cache *mc1 = alloc_method_cache();
              NODE *isa_arg1 = ALLOC_node_lvar_set(ai_isa1,
                                                    ALLOC_node_const_get(korb_intern("Array")));
              NODE *isa1 = ALLOC_node_seq(isa_arg1,
                  ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                          korb_intern("is_a?"), 1, ai_isa1, mc1));
              uint32_t ai_rt = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, ai_rt);
              struct method_cache *mc_rt = alloc_method_cache();
              NODE *rt_arg = ALLOC_node_lvar_set(ai_rt,
                                                  ALLOC_node_sym_lit(korb_intern("deconstruct")));
              NODE *rt = ALLOC_node_seq(rt_arg,
                  ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                          korb_intern("respond_to?"), 1, ai_rt, mc_rt));
              uint32_t ai_dc = inc_arg_index(tc);
              rewind_arg_index(tc, ai_dc);
              struct method_cache *mc_dc = alloc_method_cache();
              NODE *dc_call = ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                                      korb_intern("deconstruct"), 0, ai_dc, mc_dc);
              NODE *coerced = ALLOC_node_if(isa1,
                                             ALLOC_node_lvar_get(subj_slot),
                                             ALLOC_node_if(rt, dc_call, ALLOC_node_nil()));
              coerce_step = ALLOC_node_lvar_set(subj_slot, coerced);
          }

          /* subj.is_a?(Array) */
          uint32_t ai = inc_arg_index(tc);
          inc_arg_index(tc);
          rewind_arg_index(tc, ai);
          struct method_cache *mc_isa = alloc_method_cache();
          NODE *array_const = ALLOC_node_const_get(korb_intern("Array"));
          NODE *isa_arg = ALLOC_node_lvar_set(ai, array_const);
          NODE *isa_call = ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                                  korb_intern("is_a?"), 1, ai, mc_isa);
          NODE *isa = ALLOC_node_seq(isa_arg, isa_call);

          /* size check */
          uint32_t ai2 = inc_arg_index(tc);
          inc_arg_index(tc);
          rewind_arg_index(tc, ai2);
          struct method_cache *mc_size = alloc_method_cache();
          NODE *size_call = ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                                   korb_intern("size"), 0, ai2, mc_size);
          NODE *size_check;
          if (has_rest) {
              size_check = ALLOC_node_method_call(
                  size_call, korb_intern(">="), 1,
                  (inc_arg_index(tc), inc_arg_index(tc), rewind_arg_index(tc, ai2), ai2),
                  alloc_method_cache());
              /* the args were re-allocated; build properly */
              uint32_t ai3 = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, ai3);
              struct method_cache *mc_ge = alloc_method_cache();
              NODE *expected = ALLOC_node_int_lit(req_cnt + post_cnt);
              NODE *ge_arg = ALLOC_node_lvar_set(ai3, expected);
              size_check = ALLOC_node_seq(ge_arg,
                  ALLOC_node_method_call(size_call, korb_intern(">="), 1, ai3, mc_ge));
          } else {
              uint32_t ai3 = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, ai3);
              struct method_cache *mc_eq = alloc_method_cache();
              NODE *expected = ALLOC_node_int_lit(req_cnt + post_cnt);
              size_check = ALLOC_node_eq(size_call, expected, ai3);
          }

          NODE *combined = ALLOC_node_and(isa, size_check);

          /* element checks */
          for (uint32_t i = 0; i < req_cnt; i++) {
              uint32_t elem_slot = inc_arg_index(tc);
              uint32_t aii = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, aii);
              struct method_cache *mc_idx = alloc_method_cache();
              NODE *idx_set = ALLOC_node_lvar_set(aii, ALLOC_node_int_lit((intptr_t)i));
              NODE *aref = ALLOC_node_seq(idx_set,
                  ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                         korb_intern("[]"), 1, aii, mc_idx));
              NODE *bind_subj = ALLOC_node_lvar_set(elem_slot, aref);
              NODE *sub_check = build_pattern_check(tc, a->requireds.nodes[i], elem_slot);
              combined = ALLOC_node_and(combined, ALLOC_node_seq(bind_subj, sub_check));
          }
          /* posts: indexed from -post_cnt to -1 (i.e. last N) */
          for (uint32_t i = 0; i < post_cnt; i++) {
              uint32_t elem_slot = inc_arg_index(tc);
              uint32_t aii = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, aii);
              struct method_cache *mc_idx = alloc_method_cache();
              intptr_t idx = -(intptr_t)(post_cnt - i);
              NODE *idx_set = ALLOC_node_lvar_set(aii, ALLOC_node_int_lit(idx));
              NODE *aref = ALLOC_node_seq(idx_set,
                  ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                         korb_intern("[]"), 1, aii, mc_idx));
              NODE *bind_subj = ALLOC_node_lvar_set(elem_slot, aref);
              NODE *sub_check = build_pattern_check(tc, a->posts.nodes[i], elem_slot);
              combined = ALLOC_node_and(combined, ALLOC_node_seq(bind_subj, sub_check));
          }
          /* rest binding (if it's a local target with a name) */
          if (has_rest && PM_NODE_TYPE_P(a->rest, PM_SPLAT_NODE)) {
              pm_splat_node_t *sn = (pm_splat_node_t *)a->rest;
              if (sn->expression && PM_NODE_TYPE_P(sn->expression, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                  pm_local_variable_target_node_t *t =
                      (pm_local_variable_target_node_t *)sn->expression;
                  int rslot = lvar_slot(tc, t->name, t->depth);
                  if (rslot < 0) rslot = lvar_slot_any(tc, t->name);
                  if (rslot >= 0) {
                      /* rest = subj[req_cnt, subj.size - req_cnt - post_cnt]
                       *
                       * Build it cleanly: stage offset and length into two
                       * fresh slots, then call subj.[](off, len). */
                      uint32_t off_slot = inc_arg_index(tc);
                      uint32_t len_slot = inc_arg_index(tc);
                      rewind_arg_index(tc, off_slot);

                      /* size = subj.size; len = size - req_cnt - post_cnt */
                      uint32_t ai_sz = inc_arg_index(tc);
                      rewind_arg_index(tc, ai_sz);
                      NODE *size_call = ALLOC_node_method_call(
                          ALLOC_node_lvar_get(subj_slot),
                          korb_intern("size"), 0, ai_sz, alloc_method_cache());
                      uint32_t ai_m1 = inc_arg_index(tc);
                      inc_arg_index(tc); rewind_arg_index(tc, ai_m1);
                      NODE *m1_arg = ALLOC_node_lvar_set(ai_m1,
                          ALLOC_node_int_lit((intptr_t)(req_cnt + post_cnt)));
                      NODE *len_expr = ALLOC_node_seq(m1_arg,
                          ALLOC_node_method_call(size_call, korb_intern("-"),
                                                  1, ai_m1, alloc_method_cache()));
                      NODE *set_off = ALLOC_node_lvar_set(off_slot,
                          ALLOC_node_int_lit((intptr_t)req_cnt));
                      NODE *set_len = ALLOC_node_lvar_set(len_slot, len_expr);

                      /* subj.[](off_slot, len_slot) — args at off_slot, len_slot. */
                      NODE *slice = ALLOC_node_method_call(
                          ALLOC_node_lvar_get(subj_slot),
                          korb_intern("[]"), 2, off_slot, alloc_method_cache());

                      NODE *bind_rest = ALLOC_node_lvar_set((uint32_t)rslot,
                          ALLOC_node_seq(set_off,
                              ALLOC_node_seq(set_len, slice)));
                      combined = ALLOC_node_and(combined,
                          ALLOC_node_seq(bind_rest, ALLOC_node_true()));
                  }
              }
          }
          return ALLOC_node_seq(coerce_step, combined);
      }

      case PM_HASH_PATTERN_NODE: {
          /* in {k: pat, ...}
           *   coerced = subj.is_a?(Hash) ? subj :
           *             (subj.respond_to?(:deconstruct_keys) ? subj.deconstruct_keys(nil) : nil)
           *   coerced.is_a?(Hash) && coerced.has_key?(k) && pat(coerced[k]) && ... */
          pm_hash_pattern_node_t *h = (pm_hash_pattern_node_t *)pat;
          uint32_t cnt = (uint32_t)h->elements.size;

          /* deconstruct_keys coerce step. */
          NODE *coerce_step;
          {
              uint32_t ai_isa1 = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, ai_isa1);
              struct method_cache *mc1 = alloc_method_cache();
              NODE *isa_arg1 = ALLOC_node_lvar_set(ai_isa1,
                                                    ALLOC_node_const_get(korb_intern("Hash")));
              NODE *isa1 = ALLOC_node_seq(isa_arg1,
                  ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                          korb_intern("is_a?"), 1, ai_isa1, mc1));
              uint32_t ai_rt = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, ai_rt);
              struct method_cache *mc_rt = alloc_method_cache();
              NODE *rt_arg = ALLOC_node_lvar_set(ai_rt,
                                                  ALLOC_node_sym_lit(korb_intern("deconstruct_keys")));
              NODE *rt = ALLOC_node_seq(rt_arg,
                  ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                          korb_intern("respond_to?"), 1, ai_rt, mc_rt));
              uint32_t ai_dc = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, ai_dc);
              struct method_cache *mc_dc = alloc_method_cache();
              NODE *nil_arg = ALLOC_node_lvar_set(ai_dc, ALLOC_node_nil());
              NODE *dc_call = ALLOC_node_seq(nil_arg,
                  ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                          korb_intern("deconstruct_keys"), 1, ai_dc, mc_dc));
              NODE *coerced = ALLOC_node_if(isa1,
                                             ALLOC_node_lvar_get(subj_slot),
                                             ALLOC_node_if(rt, dc_call, ALLOC_node_nil()));
              coerce_step = ALLOC_node_lvar_set(subj_slot, coerced);
          }

          uint32_t ai = inc_arg_index(tc);
          inc_arg_index(tc); rewind_arg_index(tc, ai);
          struct method_cache *mc_isa = alloc_method_cache();
          NODE *isa_arg = ALLOC_node_lvar_set(ai, ALLOC_node_const_get(korb_intern("Hash")));
          NODE *isa_call = ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                                  korb_intern("is_a?"), 1, ai, mc_isa);
          NODE *combined = ALLOC_node_seq(isa_arg, isa_call);

          for (uint32_t i = 0; i < cnt; i++) {
              pm_node_t *el = h->elements.nodes[i];
              if (!PM_NODE_TYPE_P(el, PM_ASSOC_NODE)) continue;
              pm_assoc_node_t *as = (pm_assoc_node_t *)el;
              if (!PM_NODE_TYPE_P(as->key, PM_SYMBOL_NODE)) continue;
              pm_symbol_node_t *sym = (pm_symbol_node_t *)as->key;
              const char *kstr = (const char *)pm_string_source(&sym->unescaped);
              size_t klen = pm_string_length(&sym->unescaped);
              ID kid = korb_intern_n(kstr, (long)klen);

              /* has_key?(k) */
              uint32_t aih = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, aih);
              struct method_cache *mc_hk = alloc_method_cache();
              NODE *karg = ALLOC_node_lvar_set(aih, ALLOC_node_sym_lit(kid));
              NODE *hk = ALLOC_node_seq(karg,
                  ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                         korb_intern("has_key?"), 1, aih, mc_hk));
              combined = ALLOC_node_and(combined, hk);

              /* subj[k] */
              uint32_t elem_slot = inc_arg_index(tc);
              uint32_t aih2 = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, aih2);
              struct method_cache *mc_aref = alloc_method_cache();
              NODE *karg2 = ALLOC_node_lvar_set(aih2, ALLOC_node_sym_lit(kid));
              NODE *aref = ALLOC_node_seq(karg2,
                  ALLOC_node_method_call(ALLOC_node_lvar_get(subj_slot),
                                         korb_intern("[]"), 1, aih2, mc_aref));
              NODE *bind_subj = ALLOC_node_lvar_set(elem_slot, aref);

              /* If value side is implicit (`{k:}`), bind subj[k] to lvar named k */
              pm_node_t *val_pat = as->value;
              if (val_pat && PM_NODE_TYPE_P(val_pat, PM_IMPLICIT_NODE)) {
                  /* prism's implicit-value wraps the local target — get the inner */
                  pm_implicit_node_t *imp = (pm_implicit_node_t *)val_pat;
                  val_pat = imp->value;
              }
              NODE *sub = build_pattern_check(tc, val_pat, elem_slot);
              combined = ALLOC_node_and(combined, ALLOC_node_seq(bind_subj, sub));
          }
          return ALLOC_node_seq(coerce_step, combined);
      }

      case PM_ALTERNATION_PATTERN_NODE: {
          /* `pat1 | pat2` — try left, else try right.  Each can bind
           * variables; CRuby actually disallows binding in alternation
           * patterns but we accept it permissively. */
          pm_alternation_pattern_node_t *ap = (pm_alternation_pattern_node_t *)pat;
          NODE *left  = build_pattern_check(tc, ap->left,  subj_slot);
          NODE *right = build_pattern_check(tc, ap->right, subj_slot);
          return ALLOC_node_or(left, right);
      }

      case PM_IF_NODE: {
          /* Pattern guard: `pat if guard` — prism wraps the actual
           * pattern in pm_if_node { predicate = guard, statements = pat }. */
          pm_if_node_t *ifn = (pm_if_node_t *)pat;
          pm_node_t *inner_pat = NULL;
          if (ifn->statements && ((pm_statements_node_t *)ifn->statements)->body.size > 0) {
              inner_pat = ((pm_statements_node_t *)ifn->statements)->body.nodes[0];
          }
          NODE *pat_check = inner_pat ? build_pattern_check(tc, inner_pat, subj_slot)
                                       : ALLOC_node_true();
          NODE *guard = ifn->predicate ? T(tc, ifn->predicate) : ALLOC_node_true();
          return ALLOC_node_and(pat_check, guard);
      }

      case PM_UNLESS_NODE: {
          /* `pat unless guard` — same with negated predicate. */
          pm_unless_node_t *un = (pm_unless_node_t *)pat;
          pm_node_t *inner_pat = NULL;
          if (un->statements && ((pm_statements_node_t *)un->statements)->body.size > 0) {
              inner_pat = ((pm_statements_node_t *)un->statements)->body.nodes[0];
          }
          NODE *pat_check = inner_pat ? build_pattern_check(tc, inner_pat, subj_slot)
                                       : ALLOC_node_true();
          NODE *guard = un->predicate ? T(tc, un->predicate) : ALLOC_node_true();
          return ALLOC_node_and(pat_check, ALLOC_node_if(guard, ALLOC_node_false(), ALLOC_node_true()));
      }

      default: {
          /* Anything else: treat as `pattern_value === subject`. */
          NODE *pv = T(tc, pat);
          uint32_t ai = inc_arg_index(tc);
          inc_arg_index(tc); rewind_arg_index(tc, ai);
          struct method_cache *mc = alloc_method_cache();
          NODE *arg = ALLOC_node_lvar_set(ai, ALLOC_node_lvar_get(subj_slot));
          return ALLOC_node_seq(arg,
              ALLOC_node_method_call(pv, korb_intern("==="), 1, ai, mc));
      }
    }
}

static NODE *
T_inner(struct transduce_context *tc, pm_node_t *node)
{
    if (!node) return NULL;

    /* line tracking omitted (would need pm_newline_list_line_column) */

    switch (PM_NODE_TYPE(node)) {
      case PM_PROGRAM_NODE: {
          pm_program_node_t *n = (pm_program_node_t *)node;
          push_frame(tc, &n->locals, false);
          NODE *body = transduce_statements(tc, n->statements);
          uint32_t mx = tc->frame->max_cnt;
          pop_frame(tc);
          return ALLOC_node_scope(mx, body);
      }
      case PM_STATEMENTS_NODE:
        return transduce_statements(tc, (pm_statements_node_t *)node);

      case PM_PARENTHESES_NODE: {
          pm_parentheses_node_t *n = (pm_parentheses_node_t *)node;
          if (!n->body) return ALLOC_node_nil();
          return T(tc, n->body);
      }
      case PM_NIL_NODE:    return ALLOC_node_nil();
      case PM_TRUE_NODE:   return ALLOC_node_true();
      case PM_FALSE_NODE:  return ALLOC_node_false();
      case PM_SELF_NODE:   return ALLOC_node_self();
      case PM_INTEGER_NODE: {
          pm_integer_node_t *n = (pm_integer_node_t *)node;
          intptr_t v;
          if (integer_value_int32(&n->value, &v) && FIXABLE(v)) {
              return ALLOC_node_int_lit(v);
          }
          char *s = integer_to_string(&n->value);
          if (n->value.negative) {
              /* integer_to_string already includes sign? Actually pm_integer_string uses absolute val + negative flag */
              char *t = korb_xmalloc_atomic(strlen(s) + 2);
              t[0] = '-'; strcpy(t+1, s);
              s = t;
          }
          return ALLOC_node_bignum_lit(s);
      }
      case PM_FLOAT_NODE: {
          pm_float_node_t *n = (pm_float_node_t *)node;
          return ALLOC_node_float_lit(n->value);
      }
      case PM_STRING_NODE: {
          pm_string_node_t *n = (pm_string_node_t *)node;
          long len = (long)pm_string_length(&n->unescaped);
          const char *src = (const char *)pm_string_source(&n->unescaped);
          char *buf = korb_xmalloc_atomic(len + 1);
          memcpy(buf, src, len); buf[len] = 0;
          return ALLOC_node_str_lit(buf, (uint32_t)len);
      }
      case PM_SYMBOL_NODE: {
          pm_symbol_node_t *n = (pm_symbol_node_t *)node;
          long len = (long)pm_string_length(&n->unescaped);
          const char *src = (const char *)pm_string_source(&n->unescaped);
          ID id = korb_intern_n(src, len);
          return ALLOC_node_sym_lit(id);
      }
      case PM_INTERPOLATED_STRING_NODE: {
          pm_interpolated_string_node_t *n = (pm_interpolated_string_node_t *)node;
          return build_container(tc, &n->parts, false, false, true);
      }
      case PM_EMBEDDED_STATEMENTS_NODE: {
          pm_embedded_statements_node_t *n = (pm_embedded_statements_node_t *)node;
          return T(tc, (pm_node_t *)n->statements);
      }

      case PM_LOCAL_VARIABLE_READ_NODE: {
          pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
          int slot = lvar_slot(tc, n->name, n->depth);
          if (slot < 0) slot = lvar_slot_any(tc, n->name);
          if (slot < 0) {
              fprintf(stderr, "lvar not found: %s\n", alloc_cstr(tc->parser, n->name));
              exit(1);
          }
          return ALLOC_node_lvar_get(slot);
      }
      case PM_LOCAL_VARIABLE_WRITE_NODE: {
          pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
          int slot = lvar_slot(tc, n->name, n->depth);
          if (slot < 0) slot = lvar_slot_any(tc, n->name);
          if (slot < 0) {
              fprintf(stderr, "lvar not found (write): %s\n", alloc_cstr(tc->parser, n->name));
              exit(1);
          }
          return ALLOC_node_lvar_set(slot, T(tc, n->value));
      }
      case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
          pm_local_variable_operator_write_node_t *n = (pm_local_variable_operator_write_node_t *)node;
          int slot = lvar_slot(tc, n->name, n->depth);
          if (slot < 0) slot = lvar_slot_any(tc, n->name);
          if (slot < 0) { fprintf(stderr, "lvar not found (op-write)\n"); exit(1); }
          NODE *lhs = ALLOC_node_lvar_get(slot);
          NODE *rhs = T(tc, n->value);
          NODE *combined = alloc_binop(tc, n->binary_operator, lhs, rhs);
          return ALLOC_node_lvar_set(slot, combined);
      }

      case PM_INSTANCE_VARIABLE_READ_NODE: {
          pm_instance_variable_read_node_t *n = (pm_instance_variable_read_node_t *)node;
          return ALLOC_node_ivar_get(intern_constant(tc->parser, n->name));
      }
      case PM_INSTANCE_VARIABLE_WRITE_NODE: {
          pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)node;
          return ALLOC_node_ivar_set(intern_constant(tc->parser, n->name), T(tc, n->value));
      }

      case PM_GLOBAL_VARIABLE_READ_NODE: {
          pm_global_variable_read_node_t *n = (pm_global_variable_read_node_t *)node;
          return ALLOC_node_gvar_get(intern_constant(tc->parser, n->name));
      }
      case PM_GLOBAL_VARIABLE_WRITE_NODE: {
          pm_global_variable_write_node_t *n = (pm_global_variable_write_node_t *)node;
          return ALLOC_node_gvar_set(intern_constant(tc->parser, n->name), T(tc, n->value));
      }

      case PM_CONSTANT_READ_NODE: {
          pm_constant_read_node_t *n = (pm_constant_read_node_t *)node;
          return ALLOC_node_const_get(intern_constant(tc->parser, n->name));
      }
      case PM_CONSTANT_WRITE_NODE: {
          pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
          return ALLOC_node_const_set(intern_constant(tc->parser, n->name), T(tc, n->value));
      }
      case PM_CONSTANT_PATH_NODE: {
          pm_constant_path_node_t *n = (pm_constant_path_node_t *)node;
          NODE *parent = n->parent ? T(tc, n->parent) : ALLOC_node_const_get(korb_intern("Object"));
          return ALLOC_node_const_path_get(parent, intern_constant(tc->parser, n->name));
      }

      case PM_IF_NODE: {
          pm_if_node_t *n = (pm_if_node_t *)node;
          NODE *cond = T(tc, n->predicate);
          NODE *th = transduce_statements(tc, n->statements);
          NODE *el = n->subsequent ? T(tc, n->subsequent) : ALLOC_node_nil();
          return ALLOC_node_if(cond, th, el);
      }
      case PM_UNLESS_NODE: {
          pm_unless_node_t *n = (pm_unless_node_t *)node;
          NODE *cond = T(tc, n->predicate);
          NODE *th = transduce_statements(tc, n->statements);
          NODE *el = n->else_clause ? T(tc, (pm_node_t *)n->else_clause) : ALLOC_node_nil();
          /* swap branches */
          return ALLOC_node_if(cond, el, th);
      }
      case PM_ELSE_NODE: {
          pm_else_node_t *n = (pm_else_node_t *)node;
          return n->statements ? transduce_statements(tc, n->statements) : ALLOC_node_nil();
      }
      case PM_WHILE_NODE: {
          pm_while_node_t *n = (pm_while_node_t *)node;
          NODE *cond = T(tc, n->predicate);
          NODE *body = transduce_statements(tc, n->statements);
          if (n->base.flags & PM_LOOP_FLAGS_BEGIN_MODIFIER) {
              /* `begin; body; end while cond` — run body once unconditionally,
               * then check cond. */
              return ALLOC_node_do_while(cond, body);
          }
          return ALLOC_node_while(cond, body);
      }
      case PM_UNTIL_NODE: {
          pm_until_node_t *n = (pm_until_node_t *)node;
          NODE *cond = T(tc, n->predicate);
          NODE *body = transduce_statements(tc, n->statements);
          if (n->base.flags & PM_LOOP_FLAGS_BEGIN_MODIFIER) {
              return ALLOC_node_do_until(cond, body);
          }
          return ALLOC_node_until(cond, body);
      }
      case PM_BREAK_NODE: {
          pm_break_node_t *n = (pm_break_node_t *)node;
          NODE *v = n->arguments && n->arguments->arguments.size > 0
              ? T(tc, n->arguments->arguments.nodes[0]) : ALLOC_node_nil();
          return ALLOC_node_break(v);
      }
      case PM_NEXT_NODE: {
          pm_next_node_t *n = (pm_next_node_t *)node;
          NODE *v = n->arguments && n->arguments->arguments.size > 0
              ? T(tc, n->arguments->arguments.nodes[0]) : ALLOC_node_nil();
          return ALLOC_node_next(v);
      }
      case PM_RETRY_NODE: {
          return ALLOC_node_retry();
      }
      case PM_REDO_NODE: {
          return ALLOC_node_redo();
      }
      case PM_SINGLETON_CLASS_NODE: {
          /* class << obj; body; end */
          pm_singleton_class_node_t *n = (pm_singleton_class_node_t *)node;
          NODE *recv = T(tc, n->expression);
          push_frame(tc, &n->locals, false);
          NODE *body = n->body ? T(tc, n->body) : ALLOC_node_nil();
          pop_frame(tc);
          return ALLOC_node_singleton_class_body(recv, body);
      }
      case PM_IMAGINARY_NODE: {
          /* `5i` → `Complex(0, 5)`. */
          pm_imaginary_node_t *n = (pm_imaginary_node_t *)node;
          NODE *num = T(tc, n->numeric);
          uint32_t ai = arg_index(tc);
          inc_arg_index(tc); inc_arg_index(tc);
          rewind_arg_index(tc, ai);
          struct method_cache *mc = alloc_method_cache();
          NODE *zero_set = ALLOC_node_lvar_set(ai,     ALLOC_node_int_lit(0));
          NODE *num_set  = ALLOC_node_lvar_set(ai + 1, num);
          NODE *seq = ALLOC_node_seq(zero_set, num_set);
          return ALLOC_node_seq(seq, ALLOC_node_func_call(korb_intern("Complex"), 2, ai, mc));
      }
      case PM_RATIONAL_NODE: {
          /* `3r` → `Rational(3, 1)`.  prism stores numerator/denominator
           * as pm_integer_t — but for the integer literal forms (`Nr`)
           * we extract via numerator field; non-integer forms (e.g.
           * `0.5r`) are rare and we approximate by using the value as
           * numerator and 1 as denominator. */
          pm_rational_node_t *n = (pm_rational_node_t *)node;
          /* pm_integer_t may be a "large" int; use the first word as a
           * proxy.  For typical script values this is fine. */
          long num_val = (long)n->numerator.value;
          /* denominator field is also pm_integer_t; default 1. */
          long den_val = (long)n->denominator.value;
          if (den_val == 0) den_val = 1;
          uint32_t ai = arg_index(tc);
          inc_arg_index(tc); inc_arg_index(tc);
          rewind_arg_index(tc, ai);
          struct method_cache *mc = alloc_method_cache();
          NODE *num_set = ALLOC_node_lvar_set(ai,     ALLOC_node_int_lit(num_val));
          NODE *den_set = ALLOC_node_lvar_set(ai + 1, ALLOC_node_int_lit(den_val));
          NODE *seq = ALLOC_node_seq(num_set, den_set);
          return ALLOC_node_seq(seq, ALLOC_node_func_call(korb_intern("Rational"), 2, ai, mc));
      }
      case PM_SOURCE_LINE_NODE: {
          /* `__LINE__` — line of this token in the source file.
           * pm_newline_list_line is libprism-internal (not exported),
           * so do the binary search in-line.  newline_list.offsets
           * holds source offsets where each line *begins* (after a
           * preceding '\n'); offsets[0] is 0, offsets[i] = start of
           * line i+1.  Find the largest i with offsets[i] <= our
           * cursor offset; the line number is i+1. */
          const pm_newline_list_t *nl = &tc->parser->newline_list;
          size_t cursor_off = (size_t)(node->location.start - nl->start);
          long lo = 0, hi = (long)nl->size - 1, best = 0;
          while (lo <= hi) {
              long m = (lo + hi) / 2;
              if (nl->offsets[m] <= cursor_off) { best = m; lo = m + 1; }
              else hi = m - 1;
          }
          return ALLOC_node_int_lit((intptr_t)(best + 1));
      }
      case PM_SOURCE_FILE_NODE: {
          /* `__FILE__` — the script's path. */
          pm_source_file_node_t *n = (pm_source_file_node_t *)node;
          const char *path = (const char *)pm_string_source(&n->filepath);
          size_t plen = pm_string_length(&n->filepath);
          return ALLOC_node_str_lit(path, (uint32_t)plen);
      }
      case PM_FOR_NODE: {
          /* `for x in coll; body; end` — Ruby semantics: x and any
           * lvars set inside body are *not* scope-gated (visible to
           * the surrounding scope).  Lower to coll.each {|x| body}
           * but with the block's param landing in x's parent-frame
           * slot — body evaluates with the parent fp, so its lvar
           * reads/writes hit the parent slots directly. */
          pm_for_node_t *n = (pm_for_node_t *)node;
          NODE *coll = T(tc, n->collection);
          int x_slot = -1;
          if (n->index && PM_NODE_TYPE_P(n->index, PM_LOCAL_VARIABLE_TARGET_NODE)) {
              pm_local_variable_target_node_t *lt = (pm_local_variable_target_node_t *)n->index;
              x_slot = lvar_slot(tc, lt->name, lt->depth);
              if (x_slot < 0) x_slot = lvar_slot_any(tc, lt->name);
          }
          if (x_slot < 0) x_slot = (int)inc_arg_index(tc);  /* fallback */
          NODE *body = n->statements ? transduce_statements(tc, n->statements) : ALLOC_node_nil();
          uint32_t env_size = tc->frame->max_cnt;
          NODE *block_node = ALLOC_node_block_literal(body, 1, (uint32_t)x_slot, env_size);
          code_repo_add("<for>", body, false);
          struct method_cache *mc = alloc_method_cache();
          return ALLOC_node_method_call_block(coll, korb_intern("each"), 0, arg_index(tc), block_node, mc);
      }
      case PM_PRE_EXECUTION_NODE: {
          /* `BEGIN { stmts }` — koruby is single-pass, so we execute
           * inline (instead of hoisting to the very top of the program).
           * Close enough for tests. */
          pm_pre_execution_node_t *n = (pm_pre_execution_node_t *)node;
          if (n->statements) return transduce_statements(tc, n->statements);
          return ALLOC_node_nil();
      }
      case PM_POST_EXECUTION_NODE: {
          /* `END { stmts }` — Ruby runs these at exit (LIFO).  We don't
           * have an at_exit hook here; treat as no-op (registers but
           * never fires).  Tests that check `END { ... }` doesn't raise
           * pass; tests that observe the side effect do not. */
          (void)node;
          return ALLOC_node_nil();
      }
      case PM_RETURN_NODE: {
          pm_return_node_t *n = (pm_return_node_t *)node;
          NODE *v;
          if (!n->arguments || n->arguments->arguments.size == 0) {
              v = ALLOC_node_nil();
          } else if (n->arguments->arguments.size == 1) {
              v = T(tc, n->arguments->arguments.nodes[0]);
          } else {
              /* return a, b, c → return [a, b, c] */
              v = build_container(tc, &n->arguments->arguments, true, false, false);
          }
          return ALLOC_node_return(v);
      }
      case PM_AND_NODE: {
          pm_and_node_t *n = (pm_and_node_t *)node;
          return ALLOC_node_and(T(tc, n->left), T(tc, n->right));
      }
      case PM_OR_NODE: {
          pm_or_node_t *n = (pm_or_node_t *)node;
          return ALLOC_node_or(T(tc, n->left), T(tc, n->right));
      }

      case PM_ARRAY_NODE: {
          pm_array_node_t *n = (pm_array_node_t *)node;
          /* If there's a splat, build as concatenation: [a, *b, c] → [a] + b.to_a + [c] */
          bool has_splat = false;
          for (size_t i = 0; i < n->elements.size; i++) {
              if (PM_NODE_TYPE_P(n->elements.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
          }
          if (!has_splat) {
              return build_container(tc, &n->elements, true, false, false);
          }
          /* Splat path: build runtime concat chain. */
          NODE *result = NULL;
          /* Group consecutive non-splats into a sub-array literal, then
           * concat splats in between. */
          size_t i = 0;
          while (i < n->elements.size) {
              if (PM_NODE_TYPE_P(n->elements.nodes[i], PM_SPLAT_NODE)) {
                  pm_splat_node_t *sn = (pm_splat_node_t *)n->elements.nodes[i];
                  NODE *splatted = sn->expression
                      ? ALLOC_node_splat_to_ary(T(tc, sn->expression))
                      : ALLOC_node_ary_new(0, 0);
                  result = result ? ALLOC_node_ary_concat(result, splatted) : splatted;
                  i++;
              } else {
                  /* Group consecutive non-splat */
                  size_t j = i;
                  while (j < n->elements.size && !PM_NODE_TYPE_P(n->elements.nodes[j], PM_SPLAT_NODE)) j++;
                  pm_node_list_t sub = { 0 };
                  sub.size = sub.capacity = j - i;
                  sub.nodes = &n->elements.nodes[i];
                  NODE *part = build_container(tc, &sub, true, false, false);
                  result = result ? ALLOC_node_ary_concat(result, part) : part;
                  i = j;
              }
          }
          return result ? result : ALLOC_node_ary_new(0, 0);
      }
      case PM_HASH_NODE: {
          pm_hash_node_t *n = (pm_hash_node_t *)node;
          return build_container(tc, &n->elements, false, true, false);
      }
      case PM_RANGE_NODE: {
          pm_range_node_t *n = (pm_range_node_t *)node;
          NODE *b = n->left ? T(tc, n->left) : ALLOC_node_nil();
          NODE *e = n->right ? T(tc, n->right) : ALLOC_node_nil();
          uint32_t excl = (n->base.flags & PM_RANGE_FLAGS_EXCLUDE_END) ? 1 : 0;
          return ALLOC_node_range_new(b, e, excl);
      }

      case PM_DEF_NODE: {
          pm_def_node_t *n = (pm_def_node_t *)node;
          ID name = intern_constant(tc->parser, n->name);
          uint32_t required_cnt = 0;
          uint32_t total_cnt = 0;
          int rest_slot = -1;
          int block_slot = -1;
          uint32_t kwh_save_slot = (uint32_t)-1;
          push_frame(tc, &n->locals, false);

          NODE *prologue = NULL;  /* default-value initialization */
          if (n->parameters) {
              pm_parameters_node_t *pn = (pm_parameters_node_t *)n->parameters;
              required_cnt = (uint32_t)pn->requireds.size;
              total_cnt = required_cnt;
              /* optionals: build "if Qundef then assign default" chain */
              for (size_t i = 0; i < pn->optionals.size; i++) {
                  pm_optional_parameter_node_t *op = (pm_optional_parameter_node_t *)pn->optionals.nodes[i];
                  int slot = lvar_slot(tc, op->name, 0);
                  if (slot < 0) continue;
                  NODE *def_val = T(tc, op->value);
                  /* if (lvar_get(slot) == Qundef) lvar_set(slot, def_val) */
                  NODE *cur = ALLOC_node_lvar_get(slot);
                  /* Use a special node that compares to Qundef: implement by
                   * building "if cur.equal?(Qundef)..." but we don't have a
                   * direct way to express Qundef in user space.  Use the
                   * dedicated node_default_init. */
                  NODE *init = ALLOC_node_default_init(slot, def_val);
                  prologue = prologue ? ALLOC_node_seq(prologue, init) : init;
                  total_cnt++;
              }
              /* rest (or `def f(...)` forwarding parameter, which prism
               * sometimes places here too). */
              bool fwd_param = false;
              if (pn->rest && PM_NODE_TYPE_P(pn->rest, PM_FORWARDING_PARAMETER_NODE)) {
                  fwd_param = true;
              }
              if (pn->keyword_rest && PM_NODE_TYPE_P(pn->keyword_rest, PM_FORWARDING_PARAMETER_NODE)) {
                  fwd_param = true;
              }
              if (fwd_param) {
                  /* def f(...) — capture into 3 hidden slots for `f(...)` to forward. */
                  uint32_t fr = inc_arg_index(tc);
                  uint32_t fk = inc_arg_index(tc);
                  uint32_t fb = inc_arg_index(tc);
                  rest_slot = (int)fr;
                  total_cnt++;                         /* rest is one param slot */
                  block_slot = (int)fb;
                  tc->frame->fwd_rest_slot = (int)fr;
                  tc->frame->fwd_kwh_slot = (int)fk;
                  tc->frame->fwd_blk_slot = (int)fb;
              } else if (pn->rest) {
                  pm_node_t *rp = pn->rest;
                  if (PM_NODE_TYPE_P(rp, PM_REST_PARAMETER_NODE)) {
                      pm_rest_parameter_node_t *r = (pm_rest_parameter_node_t *)rp;
                      if (r->name) {
                          int slot = lvar_slot(tc, r->name, 0);
                          if (slot >= 0) {
                              rest_slot = slot;
                              total_cnt++;
                          }
                      } else {
                          /* anonymous *rest: locals don't have a name; use a sentinel slot */
                      }
                  }
              }
              /* post-rest required params (def f(a, *r, b, c)). */
              for (size_t i = 0; i < pn->posts.size; i++) {
                  if (PM_NODE_TYPE_P(pn->posts.nodes[i], PM_REQUIRED_PARAMETER_NODE)) {
                      total_cnt++;
                  }
              }
              /* keyword params (`def f(a:, b: 10)`).
               *
               * Lower to: caller's last positional arg is the kwargs hash;
               * body prelude snapshots it and extracts each key.  The
               * positional-only-total stays as-is; we add ONE more total
               * for the kwh slot.  The hash lands at fp[positional_only_total]
               * (collides with whichever local prism placed there — the
               * snapshot dance preserves it).  kwh_save_slot is a fresh
               * slot beyond locals_cnt that holds the hash for extraction. */
              bool has_kwrest = pn->keyword_rest && PM_NODE_TYPE_P(pn->keyword_rest, PM_KEYWORD_REST_PARAMETER_NODE);
              int kwrest_target_slot = -1;
              if (has_kwrest) {
                  pm_keyword_rest_parameter_node_t *kr =
                      (pm_keyword_rest_parameter_node_t *)pn->keyword_rest;
                  if (kr->name) {
                      kwrest_target_slot = lvar_slot(tc, kr->name, 0);
                  }
              }
              if (fwd_param) {
                  /* forward `(...)` also accepts kwargs — use the hidden
                   * fwd_kwh slot as the kwrest target. */
                  has_kwrest = true;
                  kwrest_target_slot = tc->frame->fwd_kwh_slot;
              }
              if (pn->keywords.size > 0 || has_kwrest) {
                  /* Reserve a hidden slot the prologue stashes the peeled
                   * kwargs hash into.  The body prelude only needs to read
                   * from this slot — the prologue handles kwh extraction. */
                  kwh_save_slot = inc_arg_index(tc);
                  /* For forwarding, also expose kwh slot to the call site. */
                  if (fwd_param) {
                      tc->frame->fwd_kwh_slot = (int)kwh_save_slot;
                  }
                  /* For each keyword: extract from kwh_save_slot into the
                   * named local's slot. */
                  for (size_t i = 0; i < pn->keywords.size; i++) {
                      pm_node_t *kp = pn->keywords.nodes[i];
                      if (PM_NODE_TYPE_P(kp, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) {
                          pm_required_keyword_parameter_node_t *rk =
                              (pm_required_keyword_parameter_node_t *)kp;
                          int slot = lvar_slot(tc, rk->name, 0);
                          if (slot < 0) continue;
                          /* slot = kwh_save.fetch(:name) */
                          uint32_t ai = inc_arg_index(tc);
                          inc_arg_index(tc); rewind_arg_index(tc, ai);
                          struct method_cache *mc = alloc_method_cache();
                          NODE *karg = ALLOC_node_lvar_set(ai,
                              ALLOC_node_sym_lit(intern_constant(tc->parser, rk->name)));
                          NODE *fetch = ALLOC_node_seq(karg,
                              ALLOC_node_method_call(ALLOC_node_lvar_get(kwh_save_slot),
                                                     korb_intern("fetch"), 1, ai, mc));
                          NODE *ext = ALLOC_node_lvar_set((uint32_t)slot, fetch);
                          prologue = prologue ? ALLOC_node_seq(prologue, ext) : ext;
                      } else if (PM_NODE_TYPE_P(kp, PM_OPTIONAL_KEYWORD_PARAMETER_NODE)) {
                          pm_optional_keyword_parameter_node_t *ok =
                              (pm_optional_keyword_parameter_node_t *)kp;
                          int slot = lvar_slot(tc, ok->name, 0);
                          if (slot < 0) continue;
                          NODE *def_val = T(tc, ok->value);
                          /* slot = kwh.has_key?(:name) ? kwh[:name] : default */
                          ID kid = intern_constant(tc->parser, ok->name);
                          uint32_t ai = inc_arg_index(tc);
                          inc_arg_index(tc); rewind_arg_index(tc, ai);
                          struct method_cache *mc_hk = alloc_method_cache();
                          NODE *hk_arg = ALLOC_node_lvar_set(ai, ALLOC_node_sym_lit(kid));
                          NODE *hk = ALLOC_node_seq(hk_arg,
                              ALLOC_node_method_call(ALLOC_node_lvar_get(kwh_save_slot),
                                                     korb_intern("has_key?"), 1, ai, mc_hk));
                          uint32_t ai2 = inc_arg_index(tc);
                          inc_arg_index(tc); rewind_arg_index(tc, ai2);
                          struct method_cache *mc_aref = alloc_method_cache();
                          NODE *karg = ALLOC_node_lvar_set(ai2, ALLOC_node_sym_lit(kid));
                          NODE *aref = ALLOC_node_seq(karg,
                              ALLOC_node_method_call(ALLOC_node_lvar_get(kwh_save_slot),
                                                     korb_intern("[]"), 1, ai2, mc_aref));
                          NODE *if_n = ALLOC_node_if(hk, aref, def_val);
                          NODE *set_lv = ALLOC_node_lvar_set((uint32_t)slot, if_n);
                          prologue = prologue ? ALLOC_node_seq(prologue, set_lv) : set_lv;
                      }
                  }
                  /* **kwrest: copy kwh and delete the named keys.  If no
                   * name was given (anonymous **), skip — nothing to bind. */
                  if (kwrest_target_slot >= 0) {
                      /* rest = kwh.dup */
                      uint32_t ai_dup = inc_arg_index(tc);
                      rewind_arg_index(tc, ai_dup);
                      struct method_cache *mc_dup = alloc_method_cache();
                      NODE *dup = ALLOC_node_method_call(ALLOC_node_lvar_get(kwh_save_slot),
                                                        korb_intern("dup"), 0, ai_dup, mc_dup);
                      NODE *bind = ALLOC_node_lvar_set((uint32_t)kwrest_target_slot, dup);
                      prologue = prologue ? ALLOC_node_seq(prologue, bind) : bind;
                      /* For each named kwarg, delete from rest. */
                      for (size_t i = 0; i < pn->keywords.size; i++) {
                          pm_node_t *kp = pn->keywords.nodes[i];
                          ID kid = 0;
                          if (PM_NODE_TYPE_P(kp, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) {
                              kid = intern_constant(tc->parser, ((pm_required_keyword_parameter_node_t *)kp)->name);
                          } else if (PM_NODE_TYPE_P(kp, PM_OPTIONAL_KEYWORD_PARAMETER_NODE)) {
                              kid = intern_constant(tc->parser, ((pm_optional_keyword_parameter_node_t *)kp)->name);
                          } else continue;
                          uint32_t aid = inc_arg_index(tc);
                          inc_arg_index(tc); rewind_arg_index(tc, aid);
                          struct method_cache *mc_del = alloc_method_cache();
                          NODE *karg = ALLOC_node_lvar_set(aid, ALLOC_node_sym_lit(kid));
                          NODE *del = ALLOC_node_seq(karg,
                              ALLOC_node_method_call(ALLOC_node_lvar_get((uint32_t)kwrest_target_slot),
                                                     korb_intern("delete"), 1, aid, mc_del));
                          prologue = prologue ? ALLOC_node_seq(prologue, del) : del;
                      }
                  }
              }
              /* &blk — reify block as Proc into a local slot */
              if (pn->block && PM_NODE_TYPE_P((pm_node_t *)pn->block, PM_BLOCK_PARAMETER_NODE)) {
                  pm_block_parameter_node_t *bp = (pm_block_parameter_node_t *)pn->block;
                  if (bp->name) {
                      int slot = lvar_slot(tc, bp->name, 0);
                      if (slot >= 0) block_slot = slot;
                  }
              }
          }
          NODE *body = n->body ? T(tc, n->body) : ALLOC_node_nil();
          if (prologue) body = ALLOC_node_seq(prologue, body);
          uint32_t locals = tc->frame->max_cnt;
          pop_frame(tc);
          code_repo_add(korb_id_name(name), body, false);
          if (n->receiver) {
              if (PM_NODE_TYPE_P(n->receiver, PM_SELF_NODE)) {
                  return ALLOC_node_singleton_def(name, body, required_cnt, locals);
              }
              /* def obj.foo — install on obj's singleton class. */
              NODE *recv = T(tc, n->receiver);
              return ALLOC_node_obj_singleton_def(recv, name, body, required_cnt, locals);
          }
          /* posts size — params after *rest, e.g. `def f(a, *r, b, c)`. */
          uint32_t post_cnt = 0;
          if (n->parameters) {
              pm_parameters_node_t *pn = (pm_parameters_node_t *)n->parameters;
              post_cnt = (uint32_t)pn->posts.size;
          }
          NODE *def_node;
          if (post_cnt > 0) {
              def_node = ALLOC_node_def_post(name, body, required_cnt, total_cnt,
                                              (int32_t)rest_slot, (int32_t)block_slot, locals, post_cnt);
          } else {
              def_node = ALLOC_node_def_full(name, body, required_cnt, total_cnt,
                                              (int32_t)rest_slot, (int32_t)block_slot, locals);
          }
          if (kwh_save_slot != (uint32_t)-1) {
              def_node = ALLOC_node_seq(def_node,
                                         ALLOC_node_set_kwh_save_slot(name, (int32_t)kwh_save_slot));
          }
          return def_node;
      }

      case PM_CLASS_NODE: {
          pm_class_node_t *n = (pm_class_node_t *)node;
          ID name = intern_constant(tc->parser, n->name);
          NODE *super = n->superclass ? T(tc, n->superclass) : ALLOC_node_const_get(korb_intern("Object"));
          push_frame(tc, &n->locals, false);
          NODE *body = n->body ? T(tc, n->body) : ALLOC_node_nil();
          uint32_t mx = tc->frame->max_cnt;
          pop_frame(tc);
          NODE *body_scope = ALLOC_node_scope(mx, body);
          /* If constant_path is a ConstantPathNode, attach to parent module */
          if (n->constant_path && PM_NODE_TYPE_P(n->constant_path, PM_CONSTANT_PATH_NODE)) {
              pm_constant_path_node_t *cp = (pm_constant_path_node_t *)n->constant_path;
              NODE *parent = cp->parent ? T(tc, cp->parent) : ALLOC_node_const_get(korb_intern("Object"));
              return ALLOC_node_class_def_in(parent, name, super, body_scope);
          }
          return ALLOC_node_class_def(name, super, body_scope);
      }

      case PM_MODULE_NODE: {
          pm_module_node_t *n = (pm_module_node_t *)node;
          ID name = intern_constant(tc->parser, n->name);
          push_frame(tc, &n->locals, false);
          NODE *body = n->body ? T(tc, n->body) : ALLOC_node_nil();
          uint32_t mx = tc->frame->max_cnt;
          pop_frame(tc);
          NODE *body_scope = ALLOC_node_scope(mx, body);
          if (n->constant_path && PM_NODE_TYPE_P(n->constant_path, PM_CONSTANT_PATH_NODE)) {
              pm_constant_path_node_t *cp = (pm_constant_path_node_t *)n->constant_path;
              NODE *parent = cp->parent ? T(tc, cp->parent) : ALLOC_node_const_get(korb_intern("Object"));
              return ALLOC_node_module_def_in(parent, name, body_scope);
          }
          return ALLOC_node_module_def(name, body_scope);
      }

      case PM_BLOCK_NODE: {
          /* This is constructed at the call site below. Reaching here = unsupported */
          fprintf(stderr, "PM_BLOCK_NODE not handled at top-level\n");
          return ALLOC_node_nil();
      }

      case PM_YIELD_NODE: {
          pm_yield_node_t *n = (pm_yield_node_t *)node;
          uint32_t arg_idx = arg_index(tc);
          uint32_t cnt = 0;
          NODE *seq = NULL;
          if (n->arguments) {
              cnt = (uint32_t)n->arguments->arguments.size;
              for (uint32_t i = 0; i < cnt; i++) {
                  NODE *a = T(tc, n->arguments->arguments.nodes[i]);
                  NODE *st = ALLOC_node_lvar_set(inc_arg_index(tc), a);
                  seq = seq ? ALLOC_node_seq(seq, st) : st;
              }
          }
          NODE *y = ALLOC_node_yield(cnt, arg_idx);
          rewind_arg_index(tc, arg_idx);
          return seq ? ALLOC_node_seq(seq, y) : y;
      }

      case PM_CALL_NODE: {
          pm_call_node_t *n = (pm_call_node_t *)node;
          pm_arguments_node_t *args = (pm_arguments_node_t *)n->arguments;
          uint32_t args_cnt = args ? (uint32_t)args->arguments.size : 0;

          /* binop fast path */
          if (n->receiver && args_cnt == 1 && is_binop_name(tc, n->name) && !n->block) {
              NODE *lhs = T(tc, n->receiver);
              NODE *rhs = T(tc, args->arguments.nodes[0]);
              return alloc_binop(tc, n->name, lhs, rhs);
          }

          /* a[i] / a[i] = v shortcuts */
          if (n->receiver && ceq(tc, n->name, "[]") && args_cnt == 1 && !n->block) {
              uint32_t ai = arg_index(tc);
              inc_arg_index(tc); inc_arg_index(tc); rewind_arg_index(tc, ai);
              return ALLOC_node_aref(T(tc, n->receiver), T(tc, args->arguments.nodes[0]), ai);
          }
          if (n->receiver && ceq(tc, n->name, "[]=") && args_cnt == 2 && !n->block) {
              uint32_t ai = arg_index(tc);
              inc_arg_index(tc); inc_arg_index(tc); inc_arg_index(tc); rewind_arg_index(tc, ai);
              return ALLOC_node_aset(T(tc, n->receiver),
                                     T(tc, args->arguments.nodes[0]),
                                     T(tc, args->arguments.nodes[1]),
                                     ai);
          }

          /* unary minus rewrite: foo.-@ */
          if (n->receiver && ceq(tc, n->name, "-@") && args_cnt == 0 && !n->block) {
              uint32_t ai = arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, ai);
              return ALLOC_node_uminus(T(tc, n->receiver), ai);
          }

          /* not: !x */
          if (n->receiver && ceq(tc, n->name, "!") && args_cnt == 0 && !n->block) {
              return ALLOC_node_not(T(tc, n->receiver));
          }

          /* general call */
          ID name = intern_constant(tc->parser, n->name);
          NODE *recv = n->receiver ? T(tc, n->receiver) : NULL;
          /* block */
          /* Defer block construction so its slot_base sits ABOVE the call's
           * arg staging slots — otherwise the block's params collide with
           * the outer's arg staging. */
          pm_node_t *block_pm = (n->block && PM_NODE_TYPE_P(n->block, PM_BLOCK_NODE)) ? n->block : NULL;
          /* &expr block-pass: the call's `block` slot holds a
           * BLOCK_ARGUMENT_NODE.  Rewrite to `expr.to_proc` and pass as
           * the block_node directly. */
          if (!block_pm && n->block && PM_NODE_TYPE_P(n->block, PM_BLOCK_ARGUMENT_NODE)) {
              pm_block_argument_node_t *ba = (pm_block_argument_node_t *)n->block;
              NODE *expr = ba->expression ? T(tc, ba->expression) : ALLOC_node_nil();
              struct method_cache *mc_tp = alloc_method_cache();
              NODE *to_proc = ALLOC_node_method_call(expr, korb_intern("to_proc"), 0, arg_index(tc), mc_tp);
              return build_call_simple(tc, recv, name, args ? &args->arguments : NULL, to_proc, recv != NULL);
          }
          return build_call_with_block(tc, recv, name, args ? &args->arguments : NULL, block_pm, recv != NULL);
      }

      case PM_BEGIN_NODE: {
          pm_begin_node_t *n = (pm_begin_node_t *)node;
          NODE *body = n->statements ? transduce_statements(tc, n->statements) : ALLOC_node_nil();
          if (n->rescue_clause) {
              /* The exception object always lands in `exc_idx`.  We may
               * reuse the user's named lvar (`rescue => e`) — the
               * is_a? checks read this slot too. */
              uint32_t exc_idx;
              {
                  pm_rescue_node_t *rc = (pm_rescue_node_t *)n->rescue_clause;
                  if (rc->reference && PM_NODE_TYPE_P(rc->reference, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                      pm_local_variable_target_node_t *lt = (pm_local_variable_target_node_t *)rc->reference;
                      int slot = lvar_slot(tc, lt->name, lt->depth);
                      if (slot < 0) slot = lvar_slot_any(tc, lt->name);
                      exc_idx = (slot >= 0) ? (uint32_t)slot : inc_arg_index(tc);
                  } else {
                      exc_idx = inc_arg_index(tc);
                  }
              }
              /* Build a rescue-clause chain from the back so we can
               * fall through to a re-raise when nothing matches.
               *   if K1 === exc || K2 === exc then body1
               *   elsif K3 === exc then body2
               *   else raise exc
               * end
               * `rescue` with no class list catches StandardError
               * (Ruby's default).  We approximate by matching anything.
               */
              NODE *exc_get_for_raise = ALLOC_node_lvar_get(exc_idx);
              NODE *chain = ALLOC_node_raise(exc_get_for_raise);
              /* Walk the chain backwards.  Build a temp array of clauses
               * first to make order easy. */
              pm_rescue_node_t *clauses[16];
              int n_clauses = 0;
              for (pm_rescue_node_t *rc = (pm_rescue_node_t *)n->rescue_clause;
                   rc && n_clauses < 16;
                   rc = rc->subsequent) {
                  clauses[n_clauses++] = rc;
              }
              for (int i = n_clauses - 1; i >= 0; i--) {
                  pm_rescue_node_t *rc = clauses[i];
                  /* If the user named the exception (`=> name`) and the
                   * lvar is a *different* slot than exc_idx (because we
                   * walked into multiple clauses with different names),
                   * copy through.  In practice all clauses in a single
                   * begin/rescue share one binding so this is rare. */
                  NODE *body_for_clause = rc->statements
                      ? transduce_statements(tc, rc->statements)
                      : ALLOC_node_nil();
                  /* Build cond: K1 === exc || K2 === exc || ...
                   * If exceptions list is empty, match anything. */
                  NODE *cond = NULL;
                  if (rc->exceptions.size == 0) {
                      cond = ALLOC_node_true();
                  } else {
                      for (size_t j = 0; j < rc->exceptions.size; j++) {
                          NODE *klass = T(tc, rc->exceptions.nodes[j]);
                          uint32_t ai = inc_arg_index(tc);
                          rewind_arg_index(tc, ai);
                          struct method_cache *mc = alloc_method_cache();
                          NODE *exc_get = ALLOC_node_lvar_get(exc_idx);
                          NODE *seq = ALLOC_node_lvar_set(ai, exc_get);
                          NODE *one = ALLOC_node_seq(seq,
                              ALLOC_node_method_call(klass, korb_intern("==="), 1, ai, mc));
                          cond = cond ? ALLOC_node_or(cond, one) : one;
                      }
                  }
                  chain = ALLOC_node_if(cond, body_for_clause, chain);
              }
              body = ALLOC_node_rescue(body, chain, exc_idx);
          }
          if (n->ensure_clause) {
              pm_ensure_node_t *en = (pm_ensure_node_t *)n->ensure_clause;
              NODE *eb = en->statements ? transduce_statements(tc, en->statements) : ALLOC_node_nil();
              body = ALLOC_node_ensure(body, eb);
          }
          return body;
      }

      case PM_INDEX_OPERATOR_WRITE_NODE: {
          /* a[i] op= v: rewrite as a[i] = a[i] op v */
          pm_index_operator_write_node_t *n = (pm_index_operator_write_node_t *)node;
          if (!n->arguments || n->arguments->arguments.size != 1) {
              fprintf(stderr, "INDEX_OPERATOR_WRITE: only 1-arg supported\n");
              return ALLOC_node_nil();
          }
          uint32_t ai = arg_index(tc);
          inc_arg_index(tc); inc_arg_index(tc); inc_arg_index(tc); rewind_arg_index(tc, ai);
          NODE *recv = T(tc, n->receiver);
          NODE *idx  = T(tc, n->arguments->arguments.nodes[0]);
          /* Note: this evaluates recv twice — not strictly correct but minimal */
          NODE *idx2 = T(tc, n->arguments->arguments.nodes[0]);
          NODE *recv2 = T(tc, n->receiver);
          NODE *cur = ALLOC_node_aref(recv, idx, ai);
          NODE *combined = alloc_binop(tc, n->binary_operator, cur, T(tc, n->value));
          return ALLOC_node_aset(recv2, idx2, combined, ai);
      }

      case PM_LAMBDA_NODE: {
          pm_lambda_node_t *n = (pm_lambda_node_t *)node;
          uint32_t params_cnt = 0;
          pm_constant_id_t lambda_rest_name = 0;
          pm_parameters_node_t *pn_l = NULL;
          if (n->parameters && PM_NODE_TYPE_P(n->parameters, PM_BLOCK_PARAMETERS_NODE)) {
              pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
              if (bp->parameters) {
                  pn_l = (pm_parameters_node_t *)bp->parameters;
                  params_cnt = (uint32_t)pn_l->requireds.size;
                  if (pn_l->rest && PM_NODE_TYPE_P(pn_l->rest, PM_REST_PARAMETER_NODE)) {
                      pm_rest_parameter_node_t *rp = (pm_rest_parameter_node_t *)pn_l->rest;
                      if (rp->name) lambda_rest_name = rp->name;
                  }
              }
          }
          push_frame(tc, &n->locals, true);
          uint32_t param_base = tc->frame->slot_base;
          int lambda_rest_slot = -1;
          if (lambda_rest_name) {
              int rs = lvar_slot(tc, lambda_rest_name, 0);
              if (rs >= 0) lambda_rest_slot = rs;
          }
          /* kwargs prelude — peel handled by proc_call into kwh_slot. */
          int lambda_kwh_slot = -1;
          NODE *kw_prologue = NULL;
          int lambda_kwrest_target = -1;
          if (pn_l) {
              bool has_kw = pn_l->keywords.size > 0 ||
                            (pn_l->keyword_rest && PM_NODE_TYPE_P(pn_l->keyword_rest, PM_KEYWORD_REST_PARAMETER_NODE));
              if (pn_l->keyword_rest && PM_NODE_TYPE_P(pn_l->keyword_rest, PM_KEYWORD_REST_PARAMETER_NODE)) {
                  pm_keyword_rest_parameter_node_t *kr =
                      (pm_keyword_rest_parameter_node_t *)pn_l->keyword_rest;
                  if (kr->name) lambda_kwrest_target = lvar_slot(tc, kr->name, 0);
              }
              if (has_kw) {
                  lambda_kwh_slot = (int)inc_arg_index(tc);
                  for (size_t i = 0; i < pn_l->keywords.size; i++) {
                      pm_node_t *kp = pn_l->keywords.nodes[i];
                      if (PM_NODE_TYPE_P(kp, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) {
                          pm_required_keyword_parameter_node_t *rk =
                              (pm_required_keyword_parameter_node_t *)kp;
                          int slot = lvar_slot(tc, rk->name, 0);
                          if (slot < 0) continue;
                          uint32_t ai = inc_arg_index(tc);
                          inc_arg_index(tc); rewind_arg_index(tc, ai);
                          struct method_cache *mc = alloc_method_cache();
                          NODE *karg = ALLOC_node_lvar_set(ai,
                              ALLOC_node_sym_lit(intern_constant(tc->parser, rk->name)));
                          NODE *fetch = ALLOC_node_seq(karg,
                              ALLOC_node_method_call(ALLOC_node_lvar_get((uint32_t)lambda_kwh_slot),
                                                     korb_intern("fetch"), 1, ai, mc));
                          NODE *ext = ALLOC_node_lvar_set((uint32_t)slot, fetch);
                          kw_prologue = kw_prologue ? ALLOC_node_seq(kw_prologue, ext) : ext;
                      } else if (PM_NODE_TYPE_P(kp, PM_OPTIONAL_KEYWORD_PARAMETER_NODE)) {
                          pm_optional_keyword_parameter_node_t *ok =
                              (pm_optional_keyword_parameter_node_t *)kp;
                          int slot = lvar_slot(tc, ok->name, 0);
                          if (slot < 0) continue;
                          NODE *def_val = T(tc, ok->value);
                          ID kid = intern_constant(tc->parser, ok->name);
                          uint32_t ai = inc_arg_index(tc);
                          inc_arg_index(tc); rewind_arg_index(tc, ai);
                          struct method_cache *mc_hk = alloc_method_cache();
                          NODE *hk_arg = ALLOC_node_lvar_set(ai, ALLOC_node_sym_lit(kid));
                          NODE *hk = ALLOC_node_seq(hk_arg,
                              ALLOC_node_method_call(ALLOC_node_lvar_get((uint32_t)lambda_kwh_slot),
                                                     korb_intern("has_key?"), 1, ai, mc_hk));
                          uint32_t ai2 = inc_arg_index(tc);
                          inc_arg_index(tc); rewind_arg_index(tc, ai2);
                          struct method_cache *mc_aref = alloc_method_cache();
                          NODE *karg = ALLOC_node_lvar_set(ai2, ALLOC_node_sym_lit(kid));
                          NODE *aref = ALLOC_node_seq(karg,
                              ALLOC_node_method_call(ALLOC_node_lvar_get((uint32_t)lambda_kwh_slot),
                                                     korb_intern("[]"), 1, ai2, mc_aref));
                          NODE *if_n = ALLOC_node_if(hk, aref, def_val);
                          NODE *set_lv = ALLOC_node_lvar_set((uint32_t)slot, if_n);
                          kw_prologue = kw_prologue ? ALLOC_node_seq(kw_prologue, set_lv) : set_lv;
                      }
                  }
                  if (lambda_kwrest_target >= 0) {
                      uint32_t ai_dup = inc_arg_index(tc);
                      rewind_arg_index(tc, ai_dup);
                      struct method_cache *mc_dup = alloc_method_cache();
                      NODE *dup = ALLOC_node_method_call(ALLOC_node_lvar_get((uint32_t)lambda_kwh_slot),
                                                        korb_intern("dup"), 0, ai_dup, mc_dup);
                      NODE *bind = ALLOC_node_lvar_set((uint32_t)lambda_kwrest_target, dup);
                      kw_prologue = kw_prologue ? ALLOC_node_seq(kw_prologue, bind) : bind;
                      for (size_t i = 0; i < pn_l->keywords.size; i++) {
                          pm_node_t *kp = pn_l->keywords.nodes[i];
                          ID kid = 0;
                          if (PM_NODE_TYPE_P(kp, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) {
                              kid = intern_constant(tc->parser, ((pm_required_keyword_parameter_node_t *)kp)->name);
                          } else if (PM_NODE_TYPE_P(kp, PM_OPTIONAL_KEYWORD_PARAMETER_NODE)) {
                              kid = intern_constant(tc->parser, ((pm_optional_keyword_parameter_node_t *)kp)->name);
                          } else continue;
                          uint32_t aid = inc_arg_index(tc);
                          inc_arg_index(tc); rewind_arg_index(tc, aid);
                          struct method_cache *mc_del = alloc_method_cache();
                          NODE *karg = ALLOC_node_lvar_set(aid, ALLOC_node_sym_lit(kid));
                          NODE *del = ALLOC_node_seq(karg,
                              ALLOC_node_method_call(ALLOC_node_lvar_get((uint32_t)lambda_kwrest_target),
                                                     korb_intern("delete"), 1, aid, mc_del));
                          kw_prologue = kw_prologue ? ALLOC_node_seq(kw_prologue, del) : del;
                      }
                  }
              }
          }
          NODE *body = n->body ? T(tc, n->body) : ALLOC_node_nil();
          if (kw_prologue) body = ALLOC_node_seq(kw_prologue, body);
          uint32_t env_size = tc->frame->max_cnt;
          pop_frame(tc);
          NODE *blk;
          if (lambda_kwh_slot >= 0) {
              blk = ALLOC_node_block_literal_kw(body, params_cnt, param_base,
                                                 env_size, (int32_t)lambda_rest_slot,
                                                 (int32_t)lambda_kwh_slot);
          } else if (lambda_rest_slot >= 0) {
              blk = ALLOC_node_block_literal_rest(body, params_cnt, param_base,
                                                   env_size, (int32_t)lambda_rest_slot);
          } else {
              blk = ALLOC_node_block_literal(body, params_cnt, param_base, env_size);
          }
          /* `-> { ... }` and `lambda { ... }` produce a *lambda* — same as
           * a block literal except is_lambda=true.  Emit a call to the
           * Kernel#lambda cfunc which flips the flag on the passed block. */
          struct method_cache *mc = alloc_method_cache();
          return ALLOC_node_func_call_block(korb_intern("lambda"), 0, arg_index(tc), blk, mc);
      }

      case PM_CASE_NODE: {
          /* case x; when a; X; when b, c; Y; else Z; end
           * → tmp = x;
           *   if (a === tmp) X
           *   elsif (b === tmp || c === tmp) Y
           *   else Z
           */
          pm_case_node_t *n = (pm_case_node_t *)node;
          NODE *subject = n->predicate ? T(tc, n->predicate) : NULL;
          uint32_t slot = inc_arg_index(tc);
          NODE *prep = subject ? ALLOC_node_lvar_set(slot, subject) : NULL;
          NODE *else_n = n->else_clause ? T(tc, (pm_node_t *)n->else_clause) : ALLOC_node_nil();
          NODE *chain = else_n;
          for (size_t i = n->conditions.size; i > 0; i--) {
              pm_when_node_t *wn = (pm_when_node_t *)n->conditions.nodes[i-1];
              NODE *body = wn->statements ? transduce_statements(tc, wn->statements) : ALLOC_node_nil();
              NODE *cond_chain = NULL;
              for (size_t j = 0; j < wn->conditions.size; j++) {
                  NODE *cv = T(tc, wn->conditions.nodes[j]);
                  NODE *eqq;
                  if (subject) {
                      /* cv.===(tmp) — stage tmp at arg slot, then method_call */
                      uint32_t ai = inc_arg_index(tc);
                      inc_arg_index(tc); /* extra slot for fallback */
                      rewind_arg_index(tc, ai);
                      struct method_cache *mc = alloc_method_cache();
                      NODE *seq_arg = ALLOC_node_lvar_set(ai, ALLOC_node_lvar_get(slot));
                      eqq = ALLOC_node_seq(seq_arg, ALLOC_node_method_call(cv, korb_intern("==="), 1, ai, mc));
                  } else {
                      eqq = cv;
                  }
                  cond_chain = cond_chain ? ALLOC_node_or(cond_chain, eqq) : eqq;
              }
              chain = ALLOC_node_if(cond_chain ? cond_chain : ALLOC_node_true(), body, chain);
          }
          rewind_arg_index(tc, slot);
          return prep ? ALLOC_node_seq(prep, chain) : chain;
      }

      case PM_WHEN_NODE: {
          /* should be handled inside PM_CASE_NODE */
          return ALLOC_node_nil();
      }

      case PM_MATCH_PREDICATE_NODE: {
          /* `expr in pattern` — returns true/false. */
          pm_match_predicate_node_t *mp = (pm_match_predicate_node_t *)node;
          NODE *subject = T(tc, mp->value);
          uint32_t subj_slot = inc_arg_index(tc);
          NODE *prep = ALLOC_node_lvar_set(subj_slot, subject);
          NODE *check = build_pattern_check(tc, mp->pattern, subj_slot);
          rewind_arg_index(tc, subj_slot);
          /* Coerce to true/false: anything truthy ⇒ true, falsy ⇒ false. */
          return ALLOC_node_seq(prep,
                                 ALLOC_node_if(check, ALLOC_node_true(), ALLOC_node_false()));
      }

      case PM_MATCH_REQUIRED_NODE: {
          /* `expr => pattern` — match or raise NoMatchingPatternError. */
          pm_match_required_node_t *mr = (pm_match_required_node_t *)node;
          NODE *subject = T(tc, mr->value);
          uint32_t subj_slot = inc_arg_index(tc);
          NODE *prep = ALLOC_node_lvar_set(subj_slot, subject);
          NODE *check = build_pattern_check(tc, mr->pattern, subj_slot);
          rewind_arg_index(tc, subj_slot);
          /* `if !check; raise; end` — but we have no node_raise without
           * an arg list; use a string. */
          NODE *err = ALLOC_node_raise(ALLOC_node_str_lit("NoMatchingPatternError", 22));
          return ALLOC_node_seq(prep,
                                 ALLOC_node_if(check, ALLOC_node_nil(), err));
      }

      case PM_CASE_MATCH_NODE: {
          /* case x in pattern1; body1 in pattern2; body2 ... else; eb end
           *   __t = x
           *   if check(pattern1, __t) then body1
           *   elsif check(pattern2, __t) then body2
           *   ...
           *   else eb (or nil)
           * end
           */
          pm_case_match_node_t *cm = (pm_case_match_node_t *)node;
          NODE *subject = cm->predicate ? T(tc, cm->predicate) : ALLOC_node_nil();
          uint32_t subj_slot = inc_arg_index(tc);
          NODE *prep = ALLOC_node_lvar_set(subj_slot, subject);
          NODE *else_n = cm->else_clause
              ? T(tc, (pm_node_t *)cm->else_clause)
              : ALLOC_node_nil();
          NODE *chain = else_n;
          for (size_t i = cm->conditions.size; i > 0; i--) {
              pm_in_node_t *in_n = (pm_in_node_t *)cm->conditions.nodes[i-1];
              NODE *body = in_n->statements
                  ? transduce_statements(tc, in_n->statements)
                  : ALLOC_node_nil();
              NODE *check = build_pattern_check(tc, in_n->pattern, subj_slot);
              chain = ALLOC_node_if(check, body, chain);
          }
          rewind_arg_index(tc, subj_slot);
          return ALLOC_node_seq(prep, chain);
      }

      case PM_SPLAT_NODE: {
          pm_splat_node_t *n = (pm_splat_node_t *)node;
          /* Used in arg lists. We just transduce the inner expression and let
           * the caller treat it as a splat (parser tracks splats explicitly).
           * Here just return the array expression. */
          if (n->expression) return ALLOC_node_splat_to_ary(T(tc, n->expression));
          return ALLOC_node_ary_new(0, 0);
      }

      case PM_MULTI_WRITE_NODE: {
          pm_multi_write_node_t *n = (pm_multi_write_node_t *)node;
          NODE *rhs = T(tc, n->value);
          uint32_t tmp_slot = inc_arg_index(tc);
          NODE *prep = ALLOC_node_lvar_set(tmp_slot, ALLOC_node_splat_to_ary(rhs));
          NODE *chain = prep;

          /* helper macro: build assign for one target given the get-expr */
          #define BUILD_TARGET_ASSIGN(target_node, get_expr) ({                       \
              NODE *_assign = NULL;                                                    \
              pm_node_t *_t = (target_node);                                           \
              NODE *_g = (get_expr);                                                   \
              if (PM_NODE_TYPE_P(_t, PM_LOCAL_VARIABLE_TARGET_NODE)) {                 \
                  pm_local_variable_target_node_t *_lt = (pm_local_variable_target_node_t *)_t; \
                  int _slot = lvar_slot(tc, _lt->name, _lt->depth);                    \
                  if (_slot < 0) _slot = lvar_slot_any(tc, _lt->name);                 \
                  if (_slot >= 0) _assign = ALLOC_node_lvar_set(_slot, _g);            \
              } else if (PM_NODE_TYPE_P(_t, PM_INSTANCE_VARIABLE_TARGET_NODE)) {       \
                  pm_instance_variable_target_node_t *_it = (pm_instance_variable_target_node_t *)_t; \
                  _assign = ALLOC_node_ivar_set(intern_constant(tc->parser, _it->name), _g); \
              } else if (PM_NODE_TYPE_P(_t, PM_CONSTANT_TARGET_NODE)) {                \
                  pm_constant_target_node_t *_ct = (pm_constant_target_node_t *)_t;    \
                  _assign = ALLOC_node_const_set(intern_constant(tc->parser, _ct->name), _g); \
              } else if (PM_NODE_TYPE_P(_t, PM_GLOBAL_VARIABLE_TARGET_NODE)) {         \
                  pm_global_variable_target_node_t *_gt = (pm_global_variable_target_node_t *)_t; \
                  _assign = ALLOC_node_gvar_set(intern_constant(tc->parser, _gt->name), _g); \
              } else if (PM_NODE_TYPE_P(_t, PM_CALL_TARGET_NODE)) {                    \
                  /* recv.name=(get_expr) — name already includes '='. */              \
                  pm_call_target_node_t *_ct = (pm_call_target_node_t *)_t;            \
                  NODE *_recv = T(tc, _ct->receiver);                                  \
                  ID _wname = intern_constant(tc->parser, _ct->name);                  \
                  uint32_t _ai = inc_arg_index(tc); rewind_arg_index(tc, _ai);         \
                  struct method_cache *_mc = alloc_method_cache();                     \
                  NODE *_st = ALLOC_node_lvar_set(_ai, _g);                            \
                  NODE *_call = ALLOC_node_method_call(_recv, _wname, 1, _ai, _mc);    \
                  _assign = ALLOC_node_seq(_st, _call);                                \
              } else if (PM_NODE_TYPE_P(_t, PM_INDEX_TARGET_NODE)) {                   \
                  /* recv[args] = get_expr — currently only single-index supported. */ \
                  pm_index_target_node_t *_it = (pm_index_target_node_t *)_t;          \
                  if (_it->arguments && _it->arguments->arguments.size == 1) {         \
                      NODE *_recv = T(tc, _it->receiver);                              \
                      NODE *_idx = T(tc, _it->arguments->arguments.nodes[0]);          \
                      uint32_t _ai = inc_arg_index(tc);                                \
                      inc_arg_index(tc); inc_arg_index(tc); rewind_arg_index(tc, _ai); \
                      _assign = ALLOC_node_aset(_recv, _idx, _g, _ai);                 \
                  }                                                                    \
              }                                                                        \
              _assign;                                                                 \
          })

          /* lefts: ary[0..lefts.size-1] */
          uint32_t lefts_n = (uint32_t)n->lefts.size;
          uint32_t rights_n = (uint32_t)n->rights.size;
          for (uint32_t i = 0; i < lefts_n; i++) {
              NODE *get = ALLOC_node_ary_aget(ALLOC_node_lvar_get(tmp_slot), i);
              NODE *assign = BUILD_TARGET_ASSIGN(n->lefts.nodes[i], get);
              if (assign) chain = ALLOC_node_seq(chain, assign);
          }
          /* middle splat: ary[lefts_n .. len-rights_n] */
          if (n->rest && PM_NODE_TYPE_P(n->rest, PM_SPLAT_NODE)) {
              pm_splat_node_t *splat = (pm_splat_node_t *)n->rest;
              if (splat->expression) {
                  NODE *slice = ALLOC_node_ary_slice_middle(
                      ALLOC_node_lvar_get(tmp_slot), lefts_n, rights_n);
                  NODE *assign = BUILD_TARGET_ASSIGN(splat->expression, slice);
                  if (assign) chain = ALLOC_node_seq(chain, assign);
              }
              /* `*` with no name: discard. */
          }
          /* rights: ary[len - rights_n + i] = ary[-(rights_n - i)] */
          for (uint32_t i = 0; i < rights_n; i++) {
              uint32_t neg_offset = rights_n - i;
              NODE *get = ALLOC_node_ary_aget_neg(ALLOC_node_lvar_get(tmp_slot), neg_offset);
              NODE *assign = BUILD_TARGET_ASSIGN(n->rights.nodes[i], get);
              if (assign) chain = ALLOC_node_seq(chain, assign);
          }
          #undef BUILD_TARGET_ASSIGN
          rewind_arg_index(tc, tmp_slot);
          return chain;
      }

      case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {
          pm_instance_variable_operator_write_node_t *n = (pm_instance_variable_operator_write_node_t *)node;
          ID iv = intern_constant(tc->parser, n->name);
          NODE *cur = ALLOC_node_ivar_get(iv);
          NODE *rhs = T(tc, n->value);
          NODE *combined = alloc_binop(tc, n->binary_operator, cur, rhs);
          return ALLOC_node_ivar_set(iv, combined);
      }
      case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {
          pm_instance_variable_or_write_node_t *n = (pm_instance_variable_or_write_node_t *)node;
          ID iv = intern_constant(tc->parser, n->name);
          NODE *cur = ALLOC_node_ivar_get(iv);
          NODE *rhs = T(tc, n->value);
          /* @x ||= rhs  ⇒  @x || (@x = rhs) */
          return ALLOC_node_or(cur, ALLOC_node_ivar_set(iv, rhs));
      }
      case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: {
          pm_instance_variable_and_write_node_t *n = (pm_instance_variable_and_write_node_t *)node;
          ID iv = intern_constant(tc->parser, n->name);
          NODE *cur = ALLOC_node_ivar_get(iv);
          NODE *rhs = T(tc, n->value);
          return ALLOC_node_and(cur, ALLOC_node_ivar_set(iv, rhs));
      }

      case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
          pm_local_variable_or_write_node_t *n = (pm_local_variable_or_write_node_t *)node;
          int slot = lvar_slot(tc, n->name, n->depth);
          if (slot < 0) slot = lvar_slot_any(tc, n->name);
          NODE *cur = ALLOC_node_lvar_get(slot);
          return ALLOC_node_or(cur, ALLOC_node_lvar_set(slot, T(tc, n->value)));
      }
      case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
          pm_local_variable_and_write_node_t *n = (pm_local_variable_and_write_node_t *)node;
          int slot = lvar_slot(tc, n->name, n->depth);
          if (slot < 0) slot = lvar_slot_any(tc, n->name);
          NODE *cur = ALLOC_node_lvar_get(slot);
          return ALLOC_node_and(cur, ALLOC_node_lvar_set(slot, T(tc, n->value)));
      }

      case PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE: {
          pm_global_variable_operator_write_node_t *n = (pm_global_variable_operator_write_node_t *)node;
          ID name = intern_constant(tc->parser, n->name);
          NODE *cur = ALLOC_node_gvar_get(name);
          NODE *rhs = T(tc, n->value);
          NODE *combined = alloc_binop(tc, n->binary_operator, cur, rhs);
          return ALLOC_node_gvar_set(name, combined);
      }
      case PM_CONSTANT_OPERATOR_WRITE_NODE: {
          pm_constant_operator_write_node_t *n = (pm_constant_operator_write_node_t *)node;
          ID name = intern_constant(tc->parser, n->name);
          NODE *cur = ALLOC_node_const_get(name);
          NODE *rhs = T(tc, n->value);
          NODE *combined = alloc_binop(tc, n->binary_operator, cur, rhs);
          return ALLOC_node_const_set(name, combined);
      }

      case PM_GLOBAL_VARIABLE_OR_WRITE_NODE: {
          pm_global_variable_or_write_node_t *n = (pm_global_variable_or_write_node_t *)node;
          ID name = intern_constant(tc->parser, n->name);
          return ALLOC_node_or(ALLOC_node_gvar_get(name),
                               ALLOC_node_gvar_set(name, T(tc, n->value)));
      }

      case PM_CONSTANT_OR_WRITE_NODE: {
          pm_constant_or_write_node_t *n = (pm_constant_or_write_node_t *)node;
          ID name = intern_constant(tc->parser, n->name);
          return ALLOC_node_or(ALLOC_node_const_get(name),
                               ALLOC_node_const_set(name, T(tc, n->value)));
      }

      case PM_KEYWORD_HASH_NODE: {
          pm_keyword_hash_node_t *n = (pm_keyword_hash_node_t *)node;
          /* treat as a regular hash */
          return build_container(tc, &n->elements, false, true, false);
      }

      case PM_DEFINED_NODE: {
          pm_defined_node_t *n = (pm_defined_node_t *)node;
          pm_node_t *expr = n->value;
          if (!expr) return ALLOC_node_nil();
          /* Compile-time string for syntactically obvious cases. */
          switch (PM_NODE_TYPE(expr)) {
            case PM_SELF_NODE:
              return ALLOC_node_str_lit("self", 4);
            case PM_NIL_NODE:
              return ALLOC_node_str_lit("expression", 10);
            case PM_TRUE_NODE: case PM_FALSE_NODE:
              return ALLOC_node_str_lit("expression", 10);
            case PM_INTEGER_NODE: case PM_FLOAT_NODE: case PM_STRING_NODE:
            case PM_SYMBOL_NODE: case PM_ARRAY_NODE: case PM_HASH_NODE:
              return ALLOC_node_str_lit("expression", 10);
            case PM_LOCAL_VARIABLE_READ_NODE:
              /* lvars are scope-resolved at parse time; always defined. */
              return ALLOC_node_str_lit("local-variable", 14);
            case PM_INSTANCE_VARIABLE_READ_NODE: {
              /* "instance-variable" only if @x is set on self. */
              pm_instance_variable_read_node_t *iv = (pm_instance_variable_read_node_t *)expr;
              ID name = intern_constant(tc->parser, iv->name);
              uint32_t ai = inc_arg_index(tc);
              rewind_arg_index(tc, ai);
              struct method_cache *mc = alloc_method_cache();
              NODE *self_node = ALLOC_node_self();
              NODE *defined_p = ALLOC_node_seq(
                  ALLOC_node_lvar_set(ai, ALLOC_node_sym_lit(name)),
                  ALLOC_node_method_call(self_node, korb_intern("instance_variable_defined?"),
                                         1, ai, mc));
              return ALLOC_node_if(defined_p,
                                   ALLOC_node_str_lit("instance-variable", 17),
                                   ALLOC_node_nil());
            }
            case PM_GLOBAL_VARIABLE_READ_NODE: {
              pm_global_variable_read_node_t *gv = (pm_global_variable_read_node_t *)expr;
              ID gname = intern_constant(tc->parser, gv->name);
              /* nil-check the gvar value (proxy: if nil-or-undef, treat
               * as undefined). */
              NODE *get = ALLOC_node_gvar_get(gname);
              uint32_t ai = inc_arg_index(tc);
              rewind_arg_index(tc, ai);
              NODE *seq = ALLOC_node_lvar_set(ai, get);
              NODE *cond = ALLOC_node_seq(seq, ALLOC_node_lvar_get(ai));
              return ALLOC_node_if(cond,
                                   ALLOC_node_str_lit("global-variable", 15),
                                   ALLOC_node_nil());
            }
            case PM_CONSTANT_READ_NODE: {
              pm_constant_read_node_t *cr = (pm_constant_read_node_t *)expr;
              ID cname = intern_constant(tc->parser, cr->name);
              /* Check via Object.const_defined?(name) — Module#const_get
               * raises on missing.  We use the runtime side-effect: try
               * to look up; rescue nil. */
              uint32_t ai = inc_arg_index(tc);
              rewind_arg_index(tc, ai);
              struct method_cache *mc = alloc_method_cache();
              NODE *recv = ALLOC_node_const_get(korb_intern("Object"));
              NODE *seq = ALLOC_node_lvar_set(ai, ALLOC_node_sym_lit(cname));
              NODE *defined_p = ALLOC_node_seq(seq,
                  ALLOC_node_method_call(recv, korb_intern("const_defined?"), 1, ai, mc));
              return ALLOC_node_if(defined_p,
                                   ALLOC_node_str_lit("constant", 8),
                                   ALLOC_node_nil());
            }
            case PM_CALL_NODE: {
              /* Check at runtime via recv.respond_to?(name).  When the
               * receiver itself is a literal (1+2 → 1.+(2)), it's an
               * "expression" rather than a "method".  Otherwise it's
               * "method" if respond_to? is true, nil otherwise. */
              pm_call_node_t *cn = (pm_call_node_t *)expr;
              ID method_name = intern_constant(tc->parser, cn->name);
              /* If the call has a receiver that is itself a literal,
               * defined? returns "expression". */
              if (cn->receiver) {
                  switch (PM_NODE_TYPE(cn->receiver)) {
                    case PM_INTEGER_NODE: case PM_FLOAT_NODE:
                    case PM_STRING_NODE:  case PM_SYMBOL_NODE:
                    case PM_ARRAY_NODE:   case PM_HASH_NODE:
                    case PM_TRUE_NODE:    case PM_FALSE_NODE:
                    case PM_NIL_NODE:
                      return ALLOC_node_str_lit("expression", 10);
                    default: break;
                  }
              }
              NODE *recv_node = cn->receiver ? T(tc, cn->receiver) : ALLOC_node_self();
              uint32_t ai = inc_arg_index(tc);
              inc_arg_index(tc); rewind_arg_index(tc, ai);
              struct method_cache *mc = alloc_method_cache();
              NODE *karg = ALLOC_node_lvar_set(ai, ALLOC_node_sym_lit(method_name));
              NODE *check = ALLOC_node_seq(karg,
                  ALLOC_node_method_call(recv_node, korb_intern("respond_to?"),
                                          1, ai, mc));
              return ALLOC_node_if(check,
                                    ALLOC_node_str_lit("method", 6),
                                    ALLOC_node_nil());
            }
            default:
              return ALLOC_node_str_lit("expression", 10);
          }
      }

      case PM_SUPER_NODE: {
          pm_super_node_t *n = (pm_super_node_t *)node;
          uint32_t arg_idx = arg_index(tc);
          uint32_t cnt = 0;
          NODE *seq = NULL;
          if (n->arguments) {
              cnt = (uint32_t)n->arguments->arguments.size;
              for (uint32_t i = 0; i < cnt; i++) {
                  NODE *a = T(tc, n->arguments->arguments.nodes[i]);
                  NODE *st = ALLOC_node_lvar_set(inc_arg_index(tc), a);
                  seq = seq ? ALLOC_node_seq(seq, st) : st;
              }
          }
          NODE *sup = ALLOC_node_super(cnt, arg_idx);
          rewind_arg_index(tc, arg_idx);
          return seq ? ALLOC_node_seq(seq, sup) : sup;
      }
      case PM_FORWARDING_SUPER_NODE: {
          /* zero-arg super (no parens) — pass current method's args */
          return ALLOC_node_super_forward();
      }

      case PM_NUMBERED_REFERENCE_READ_NODE:
      case PM_BACK_REFERENCE_READ_NODE:
        return ALLOC_node_nil();

      case PM_X_STRING_NODE:
      case PM_INTERPOLATED_X_STRING_NODE: {
          /* shell exec — stub */
          return ALLOC_node_str_lit("", 0);
      }

      case PM_REGULAR_EXPRESSION_NODE: {
          /* stub: return string for now */
          pm_regular_expression_node_t *n = (pm_regular_expression_node_t *)node;
          long len = (long)pm_string_length(&n->unescaped);
          const char *src = (const char *)pm_string_source(&n->unescaped);
          char *buf = korb_xmalloc_atomic(len + 1);
          memcpy(buf, src, len); buf[len] = 0;
          return ALLOC_node_str_lit(buf, (uint32_t)len);
      }

      case PM_INTERPOLATED_SYMBOL_NODE: {
          pm_interpolated_symbol_node_t *n = (pm_interpolated_symbol_node_t *)node;
          NODE *str = build_container(tc, &n->parts, false, false, true);
          return ALLOC_node_str_to_sym(str);
      }

      case PM_BLOCK_ARGUMENT_NODE: {
          /* Reached here only as a sub-expression (the call site rewrites
           * &expr in build_call_with_block).  Pass the expr through. */
          pm_block_argument_node_t *n = (pm_block_argument_node_t *)node;
          if (n->expression) return T(tc, n->expression);
          return ALLOC_node_nil();
      }

      case PM_RESCUE_MODIFIER_NODE: {
          pm_rescue_modifier_node_t *n = (pm_rescue_modifier_node_t *)node;
          /* expr rescue rescue_expr */
          uint32_t exc_slot = inc_arg_index(tc);
          NODE *body = T(tc, n->expression);
          NODE *rescue_body = T(tc, n->rescue_expression);
          rewind_arg_index(tc, exc_slot);
          return ALLOC_node_rescue(body, rescue_body, exc_slot);
      }

      case PM_CALL_OPERATOR_WRITE_NODE: {
          /* a.b op= v  ⇒  a.b=(a.b op v) */
          pm_call_operator_write_node_t *n = (pm_call_operator_write_node_t *)node;
          NODE *recv = T(tc, n->receiver);
          NODE *recv2 = T(tc, n->receiver);
          ID rname = intern_constant(tc->parser, n->read_name);
          ID wname = intern_constant(tc->parser, n->write_name);
          uint32_t ai = arg_index(tc);
          inc_arg_index(tc); inc_arg_index(tc); rewind_arg_index(tc, ai);
          struct method_cache *mc = alloc_method_cache();
          NODE *cur = ALLOC_node_method_call(recv, rname, 0, ai, mc);
          NODE *rhs = T(tc, n->value);
          NODE *combined = alloc_binop(tc, n->binary_operator, cur, rhs);
          /* call writer with combined */
          NODE *st = ALLOC_node_lvar_set(ai, combined);
          struct method_cache *mc2 = alloc_method_cache();
          NODE *call = ALLOC_node_method_call(recv2, wname, 1, ai, mc2);
          rewind_arg_index(tc, ai);
          return ALLOC_node_seq(st, call);
      }
      case PM_CALL_OR_WRITE_NODE: {
          pm_call_or_write_node_t *n = (pm_call_or_write_node_t *)node;
          NODE *recv = T(tc, n->receiver);
          NODE *recv2 = T(tc, n->receiver);
          ID rname = intern_constant(tc->parser, n->read_name);
          ID wname = intern_constant(tc->parser, n->write_name);
          uint32_t ai = arg_index(tc);
          inc_arg_index(tc); inc_arg_index(tc); rewind_arg_index(tc, ai);
          struct method_cache *mc = alloc_method_cache();
          NODE *cur = ALLOC_node_method_call(recv, rname, 0, ai, mc);
          NODE *rhs = T(tc, n->value);
          struct method_cache *mc2 = alloc_method_cache();
          NODE *st = ALLOC_node_lvar_set(ai, rhs);
          NODE *call = ALLOC_node_method_call(recv2, wname, 1, ai, mc2);
          rewind_arg_index(tc, ai);
          return ALLOC_node_or(cur, ALLOC_node_seq(st, call));
      }
      case PM_INDEX_OR_WRITE_NODE: {
          pm_index_or_write_node_t *n = (pm_index_or_write_node_t *)node;
          /* a[i] ||= v ⇒ a[i] || a[i] = v */
          if (!n->arguments || n->arguments->arguments.size != 1) return ALLOC_node_nil();
          uint32_t ai = arg_index(tc);
          inc_arg_index(tc); inc_arg_index(tc); inc_arg_index(tc); rewind_arg_index(tc, ai);
          NODE *recv = T(tc, n->receiver);
          NODE *idx = T(tc, n->arguments->arguments.nodes[0]);
          NODE *cur = ALLOC_node_aref(recv, idx, ai);
          NODE *recv2 = T(tc, n->receiver);
          NODE *idx2 = T(tc, n->arguments->arguments.nodes[0]);
          NODE *rhs = T(tc, n->value);
          NODE *set = ALLOC_node_aset(recv2, idx2, rhs, ai);
          return ALLOC_node_or(cur, set);
      }
      case PM_INDEX_AND_WRITE_NODE: {
          pm_index_and_write_node_t *n = (pm_index_and_write_node_t *)node;
          if (!n->arguments || n->arguments->arguments.size != 1) return ALLOC_node_nil();
          uint32_t ai = arg_index(tc);
          inc_arg_index(tc); inc_arg_index(tc); inc_arg_index(tc); rewind_arg_index(tc, ai);
          NODE *recv = T(tc, n->receiver);
          NODE *idx = T(tc, n->arguments->arguments.nodes[0]);
          NODE *cur = ALLOC_node_aref(recv, idx, ai);
          NODE *recv2 = T(tc, n->receiver);
          NODE *idx2 = T(tc, n->arguments->arguments.nodes[0]);
          NODE *rhs = T(tc, n->value);
          NODE *set = ALLOC_node_aset(recv2, idx2, rhs, ai);
          return ALLOC_node_and(cur, set);
      }
      /* PM_SOURCE_FILE_NODE / PM_SOURCE_LINE_NODE handled earlier with
       * proper line lookup via prism's newline_list. */
      case PM_SOURCE_ENCODING_NODE: {
          return ALLOC_node_str_lit("UTF-8", 5);
      }
      case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE: {
          /* stub — return string */
          pm_interpolated_regular_expression_node_t *n = (pm_interpolated_regular_expression_node_t *)node;
          return build_container(tc, &n->parts, false, false, true);
      }
      case PM_INDEX_TARGET_NODE: {
          /* used in multi-assign: target shape, transduce as aset later */
          return ALLOC_node_nil();
      }

      case PM_ALIAS_METHOD_NODE: {
          /* `alias new_name old_name` — KEYWORD, not a method call.
           * Goes through node_alias_method which uses cref directly,
           * bypassing any user redefinition of Module#alias_method. */
          pm_alias_method_node_t *n = (pm_alias_method_node_t *)node;
          NODE *new_arg = T(tc, n->new_name);
          NODE *old_arg = T(tc, n->old_name);
          if (!new_arg) new_arg = ALLOC_node_nil();
          if (!old_arg) old_arg = ALLOC_node_nil();
          return ALLOC_node_alias_method(new_arg, old_arg);
      }

      /* PM_FOR_NODE handled above (lowered to .each with parent-frame param). */

      default:
        fprintf(stderr, "[koruby] unsupported node: %s (line %d)\n",
                pm_node_type_to_str(PM_NODE_TYPE(node)), tc->last_line);
        return ALLOC_node_nil();
    }
}

NODE *
koruby_parse(const char *src, size_t len, const char *filename)
{
    pm_parser_t parser;
    pm_options_t options = {0};
    if (filename) pm_options_filepath_set(&options, filename);
    pm_parser_init(&parser, (const uint8_t *)src, len, &options);
    pm_node_t *root = pm_parse(&parser);

    struct transduce_context tc = { 0 };
    tc.parser = &parser;
    NODE *r = T(&tc, root);

    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);

    return r;
}
