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
    char *s = ko_xmalloc_atomic(c->length + 1);
    memcpy(s, c->start, c->length);
    s[c->length] = 0;
    return s;
}

static ID
intern_constant(pm_parser_t *parser, pm_constant_id_t cid) {
    pm_constant_t *c = pm_constant_pool_id_to_constant(&parser->constant_pool, cid);
    return ko_intern_n((const char *)c->start, (long)c->length);
}

static struct method_cache *alloc_method_cache(void) {
    return ko_xcalloc(1, sizeof(struct method_cache));
}

static void push_frame(struct transduce_context *tc, pm_constant_id_list_t *locals, bool is_block) {
    struct frame_context *f = ko_xmalloc(sizeof(*f));
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
    static const char *ops[] = {"+","-","*","/","%","<","<=",">",">=","==","!=","<<",">>","&","|","^", NULL};
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

/* Build call: receiver is optional (NULL = func_call). args list is pm_arguments_node_t
   children (already known length). args_cnt = number of pre-evaluated args.
   block is optional. */
static NODE *
build_call_simple(struct transduce_context *tc, NODE *recv, ID name,
                  pm_node_list_t *args, NODE *block_node, bool is_method)
{
    uint32_t arg_cnt = args ? (uint32_t)args->size : 0;
    uint32_t call_arg_idx = arg_index(tc);
    /* place args into call_arg_idx..call_arg_idx+arg_cnt-1 */
    NODE *seq = NULL;
    for (uint32_t i = 0; i < arg_cnt; i++) {
        NODE *arg = T(tc, args->nodes[i]);
        if (!arg) arg = ALLOC_node_nil();
        NODE *set = ALLOC_node_lvar_set(inc_arg_index(tc), arg);
        seq = seq ? ALLOC_node_seq(seq, set) : set;
    }
    NODE *call;
    struct method_cache *mc = alloc_method_cache();
    if (block_node) {
        if (is_method) {
            call = ALLOC_node_method_call_block(recv, name, arg_cnt, call_arg_idx, block_node, mc);
        } else {
            call = ALLOC_node_func_call_block(name, arg_cnt, call_arg_idx, block_node, mc);
        }
    } else {
        if (is_method) {
            call = ALLOC_node_method_call(recv, name, arg_cnt, call_arg_idx, mc);
        } else {
            call = ALLOC_node_func_call(name, arg_cnt, call_arg_idx, mc);
        }
    }
    rewind_arg_index(tc, call_arg_idx);
    return seq ? ALLOC_node_seq(seq, call) : call;
}

/* For container literals: pre-evaluate items into successive arg slots. */
static NODE *
build_container(struct transduce_context *tc, pm_node_list_t *items, bool is_array, bool is_hash, bool is_str_concat)
{
    uint32_t n = (uint32_t)items->size;
    if (is_hash) n = 0; /* see below: pairs */
    uint32_t arg_idx = arg_index(tc);
    NODE *seq = NULL;
    if (is_hash) {
        /* items are AssocNode with key+value */
        for (size_t i = 0; i < items->size; i++) {
            pm_node_t *node = items->nodes[i];
            pm_node_t *key=NULL, *val=NULL;
            if (PM_NODE_TYPE_P(node, PM_ASSOC_NODE)) {
                pm_assoc_node_t *as = (pm_assoc_node_t *)node;
                key = as->key; val = as->value;
            } else continue;
            NODE *kn = T(tc, key);
            NODE *vn = T(tc, val);
            NODE *ks = ALLOC_node_lvar_set(inc_arg_index(tc), kn);
            NODE *vs = ALLOC_node_lvar_set(inc_arg_index(tc), vn);
            NODE *pair = ALLOC_node_seq(ks, vs);
            seq = seq ? ALLOC_node_seq(seq, pair) : pair;
            n += 2;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            NODE *en = T(tc, items->nodes[i]);
            if (!en) en = ALLOC_node_nil();
            NODE *st = ALLOC_node_lvar_set(inc_arg_index(tc), en);
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
    char *s = ko_xmalloc_atomic(buf.length + 1);
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
              char *t = ko_xmalloc_atomic(strlen(s) + 2);
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
          char *buf = ko_xmalloc_atomic(len + 1);
          memcpy(buf, src, len); buf[len] = 0;
          return ALLOC_node_str_lit(buf, (uint32_t)len);
      }
      case PM_SYMBOL_NODE: {
          pm_symbol_node_t *n = (pm_symbol_node_t *)node;
          long len = (long)pm_string_length(&n->unescaped);
          const char *src = (const char *)pm_string_source(&n->unescaped);
          ID id = ko_intern_n(src, len);
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
          NODE *parent = n->parent ? T(tc, n->parent) : ALLOC_node_const_get(ko_intern("Object"));
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
          return ALLOC_node_while(cond, body);
      }
      case PM_UNTIL_NODE: {
          pm_until_node_t *n = (pm_until_node_t *)node;
          NODE *cond = T(tc, n->predicate);
          NODE *body = transduce_statements(tc, n->statements);
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
          NODE *v = n->arguments && n->arguments->arguments.size > 0
              ? T(tc, n->arguments->arguments.nodes[0]) : ALLOC_node_nil();
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
          return build_container(tc, &n->elements, true, false, false);
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
          uint32_t params_cnt = 0;
          if (n->parameters) {
              pm_parameters_node_t *pn = (pm_parameters_node_t *)n->parameters;
              params_cnt = (uint32_t)pn->requireds.size;
          }
          push_frame(tc, &n->locals, false);
          NODE *body = n->body ? T(tc, n->body) : ALLOC_node_nil();
          uint32_t locals = tc->frame->max_cnt;
          pop_frame(tc);
          /* def body is invoked with caller's fp moved into our frame —
           * body uses fp[0..locals-1] directly, no extra scope advance. */
          code_repo_add(ko_id_name(name), body, false);
          return ALLOC_node_def(name, body, params_cnt, locals);
      }

      case PM_CLASS_NODE: {
          pm_class_node_t *n = (pm_class_node_t *)node;
          ID name = intern_constant(tc->parser, n->name);
          NODE *super = n->superclass ? T(tc, n->superclass) : ALLOC_node_const_get(ko_intern("Object"));
          push_frame(tc, &n->locals, false);
          NODE *body = n->body ? T(tc, n->body) : ALLOC_node_nil();
          uint32_t mx = tc->frame->max_cnt;
          pop_frame(tc);
          NODE *body_scope = ALLOC_node_scope(mx, body);
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
          NODE *block_node = NULL;
          if (n->block && PM_NODE_TYPE_P(n->block, PM_BLOCK_NODE)) {
              pm_block_node_t *bn = (pm_block_node_t *)n->block;
              uint32_t params_cnt = 0;
              if (bn->parameters && PM_NODE_TYPE_P(bn->parameters, PM_BLOCK_PARAMETERS_NODE)) {
                  pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)bn->parameters;
                  if (bp->parameters && PM_NODE_TYPE_P((pm_node_t *)bp->parameters, PM_PARAMETERS_NODE)) {
                      params_cnt = (uint32_t)((pm_parameters_node_t *)bp->parameters)->requireds.size;
                  }
              }
              push_frame(tc, &bn->locals, true);
              uint32_t param_base = tc->frame->slot_base;
              NODE *body = bn->body ? T(tc, bn->body) : ALLOC_node_nil();
              uint32_t env_size = tc->frame->max_cnt;
              pop_frame(tc);
              block_node = ALLOC_node_block_literal(body, params_cnt, param_base, env_size);
          }
          return build_call_simple(tc, recv, name, args ? &args->arguments : NULL, block_node, recv != NULL);
      }

      case PM_BEGIN_NODE: {
          pm_begin_node_t *n = (pm_begin_node_t *)node;
          NODE *body = n->statements ? transduce_statements(tc, n->statements) : ALLOC_node_nil();
          if (n->rescue_clause) {
              pm_rescue_node_t *rc = (pm_rescue_node_t *)n->rescue_clause;
              NODE *rb = rc->statements ? transduce_statements(tc, rc->statements) : ALLOC_node_nil();
              uint32_t exc_idx = inc_arg_index(tc);
              return ALLOC_node_rescue(body, rb, exc_idx);
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
          uint32_t param_base = arg_index(tc);
          push_frame(tc, &n->locals, false);
          NODE *body = n->body ? T(tc, n->body) : ALLOC_node_nil();
          uint32_t env_size = tc->frame->max_cnt;
          pop_frame(tc);
          return ALLOC_node_block_literal(body, params_cnt, param_base, env_size);
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
