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
    if (!block_pm) {
        return build_call_simple(tc, recv, name, args, NULL, is_method);
    }
    pm_block_node_t *bn = (pm_block_node_t *)block_pm;
    uint32_t params_cnt = 0;
    if (bn->parameters && PM_NODE_TYPE_P(bn->parameters, PM_BLOCK_PARAMETERS_NODE)) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)bn->parameters;
        if (bp->parameters && PM_NODE_TYPE_P((pm_node_t *)bp->parameters, PM_PARAMETERS_NODE)) {
            params_cnt = (uint32_t)((pm_parameters_node_t *)bp->parameters)->requireds.size;
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
    NODE *body = bn->body ? T(tc, bn->body) : ALLOC_node_nil();
    uint32_t env_size = tc->frame->max_cnt;
    pop_frame(tc);
    NODE *block_node = ALLOC_node_block_literal(body, params_cnt, param_base, env_size);

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

static NODE *
T(struct transduce_context *tc, pm_node_t *node)
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
              /* rest */
              if (pn->rest) {
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
              /* keyword params: not supported, skip for now */
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
              fprintf(stderr, "[koruby] only def self.foo supported\n");
              return ALLOC_node_singleton_def(name, body, required_cnt, locals);
          }
          return ALLOC_node_def_full(name, body, required_cnt, total_cnt, (int32_t)rest_slot, locals);
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
          return build_call_with_block(tc, recv, name, args ? &args->arguments : NULL, block_pm, recv != NULL);
      }

      case PM_BEGIN_NODE: {
          pm_begin_node_t *n = (pm_begin_node_t *)node;
          NODE *body = n->statements ? transduce_statements(tc, n->statements) : ALLOC_node_nil();
          if (n->rescue_clause) {
              pm_rescue_node_t *rc = (pm_rescue_node_t *)n->rescue_clause;
              NODE *rb = rc->statements ? transduce_statements(tc, rc->statements) : ALLOC_node_nil();
              /* If `rescue => e`, bind exception to lvar `e` */
              uint32_t exc_idx;
              if (rc->reference && PM_NODE_TYPE_P(rc->reference, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                  pm_local_variable_target_node_t *lt = (pm_local_variable_target_node_t *)rc->reference;
                  int slot = lvar_slot(tc, lt->name, lt->depth);
                  if (slot < 0) slot = lvar_slot_any(tc, lt->name);
                  exc_idx = (slot >= 0) ? (uint32_t)slot : inc_arg_index(tc);
              } else {
                  exc_idx = inc_arg_index(tc);
              }
              body = ALLOC_node_rescue(body, rb, exc_idx);
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
          if (n->parameters && PM_NODE_TYPE_P(n->parameters, PM_BLOCK_PARAMETERS_NODE)) {
              pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
              if (bp->parameters)
                  params_cnt = (uint32_t)((pm_parameters_node_t *)bp->parameters)->requireds.size;
          }
          push_frame(tc, &n->locals, true);
          uint32_t param_base = tc->frame->slot_base;
          NODE *body = n->body ? T(tc, n->body) : ALLOC_node_nil();
          uint32_t env_size = tc->frame->max_cnt;
          pop_frame(tc);
          return ALLOC_node_block_literal(body, params_cnt, param_base, env_size);
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
          /* convert rhs to array if not already */
          uint32_t tmp_slot = inc_arg_index(tc);
          NODE *prep = ALLOC_node_lvar_set(tmp_slot, ALLOC_node_splat_to_ary(rhs));
          NODE *chain = prep;
          /* lefts: each gets ary[i] */
          uint32_t i = 0;
          for (i = 0; i < n->lefts.size; i++) {
              pm_node_t *target = n->lefts.nodes[i];
              NODE *get = ALLOC_node_ary_aget(ALLOC_node_lvar_get(tmp_slot), i);
              NODE *assign = NULL;
              if (PM_NODE_TYPE_P(target, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                  pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)target;
                  int slot = lvar_slot(tc, t->name, t->depth);
                  if (slot < 0) slot = lvar_slot_any(tc, t->name);
                  if (slot >= 0) assign = ALLOC_node_lvar_set(slot, get);
              } else if (PM_NODE_TYPE_P(target, PM_INSTANCE_VARIABLE_TARGET_NODE)) {
                  pm_instance_variable_target_node_t *t = (pm_instance_variable_target_node_t *)target;
                  assign = ALLOC_node_ivar_set(intern_constant(tc->parser, t->name), get);
              } else if (PM_NODE_TYPE_P(target, PM_CONSTANT_TARGET_NODE)) {
                  pm_constant_target_node_t *t = (pm_constant_target_node_t *)target;
                  assign = ALLOC_node_const_set(intern_constant(tc->parser, t->name), get);
              } else if (PM_NODE_TYPE_P(target, PM_GLOBAL_VARIABLE_TARGET_NODE)) {
                  pm_global_variable_target_node_t *t = (pm_global_variable_target_node_t *)target;
                  assign = ALLOC_node_gvar_set(intern_constant(tc->parser, t->name), get);
              } else if (PM_NODE_TYPE_P(target, PM_CALL_TARGET_NODE)) {
                  /* obj.attr = ... — call attr= on the receiver */
                  pm_call_target_node_t *t = (pm_call_target_node_t *)target;
                  ID setter_name = intern_constant(tc->parser, t->name);
                  /* Stage the argument and emit a method_call to setter */
                  uint32_t ai = inc_arg_index(tc);
                  inc_arg_index(tc); /* extra slot for fallback */
                  rewind_arg_index(tc, ai);
                  struct method_cache *mc = alloc_method_cache();
                  NODE *seq_arg = ALLOC_node_lvar_set(ai, get);
                  NODE *recv_n = T(tc, t->receiver);
                  assign = ALLOC_node_seq(seq_arg, ALLOC_node_method_call(recv_n, setter_name, 1, ai, mc));
              } else if (PM_NODE_TYPE_P(target, PM_INDEX_TARGET_NODE)) {
                  /* a[i] = ... in multi-assign */
                  pm_index_target_node_t *t = (pm_index_target_node_t *)target;
                  uint32_t ai = inc_arg_index(tc);
                  inc_arg_index(tc); inc_arg_index(tc);
                  rewind_arg_index(tc, ai);
                  if (t->arguments && t->arguments->arguments.size >= 1) {
                      NODE *recv_n = T(tc, t->receiver);
                      NODE *idx_n  = T(tc, t->arguments->arguments.nodes[0]);
                      assign = ALLOC_node_aset(recv_n, idx_n, get, ai);
                  }
              }
              if (assign) chain = ALLOC_node_seq(chain, assign);
          }
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
          /* simplified: just check by trying to evaluate; for lvar/ivar/const we
           * can be smarter, but this is a quick stub */
          (void)n;
          return ALLOC_node_str_lit("expression", 10);
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
          /* &expr — used at call site to convert to block; handled at callsite */
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
      case PM_SOURCE_FILE_NODE: {
          /* __FILE__ */
          return ALLOC_node_str_lit(
              tc->parser->filepath.source ? (const char *)tc->parser->filepath.source : "(unknown)",
              (uint32_t)(tc->parser->filepath.length));
      }
      case PM_SOURCE_LINE_NODE: {
          return ALLOC_node_num(0);
      }
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

      case PM_FOR_NODE: {
          /* for x in coll; body; end ⇒ coll.each {|x| body} */
          pm_for_node_t *n = (pm_for_node_t *)node;
          /* Build a synthetic call with a block.  Simplified: only LocalTarget. */
          (void)n;
          fprintf(stderr, "[koruby] PM_FOR_NODE not yet supported\n");
          return ALLOC_node_nil();
      }

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
    pm_parser_init(&parser, (const uint8_t *)src, len, &options);
    pm_node_t *root = pm_parse(&parser);

    struct transduce_context tc = { 0 };
    tc.parser = &parser;
    NODE *r = T(&tc, root);

    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);

    return r;
}
