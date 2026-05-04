#include "prism.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <alloca.h>
#include "node.h"
#include "context.h"
#include "astro_jit.h"

#define TRANSDUCE(n) transduce(tc, (pm_node_t *)(n), indent+1)

static const char *pm_node_type_name(pm_node_type_t type);

static const char *
alloc_cstr(pm_parser_t *parser, pm_constant_id_t cid)
{
    pm_constant_t *c = pm_constant_pool_id_to_constant(&parser->constant_pool, cid);
    char *str = malloc(c->length + 1);
    memcpy(str, c->start, c->length);
    str[c->length] = 0;
    return str;
}

// Pending call-site link.  Used by both -s (static_lang, sets
// `node_call_static.body`) and -p (pg_mode, sets `node_pg_call_<N>.sp_body`
// or `node_call2.sp_body` for fallback) for forward references /
// self-recursive calls.  `target_field` points directly at the NODE *
// operand slot that callsite_resolve will write the body into.
//
// For pg_call_<N> we also need to patch `locals_cnt` (the callee's
// frame size — used to size the VLA in the fast path).  When
// `locals_cnt_field` is non-NULL, callsite_resolve also writes the
// resolved body's locals_cnt to that uint32_t slot.
struct callsite {
    struct callsite *prev;
    const char *name;
    NODE *cn;
    NODE **target_field;
    uint32_t *locals_cnt_field;
};

struct callsite *callsites = NULL;

// Permanent list of every `node_pg_call_<N>` allocated (only emitted
// when -p is passed).  build_code_store_pgsd walks this list at exit
// to update each call site's `sp_body` operand from the just-finished
// run's `cc->body`, so PGSDs are baked with profile-derived speculation
// rather than parse-time last-def-wins.  Plain `node_call_<N>` (without
// -p) doesn't have an sp_body operand, so we don't track them.
struct pg_call_link {
    struct pg_call_link *next;
    NODE *cn;
    NODE **sp_body_field;     // points into n->u.node_pg_call<N>.sp_body
    struct callcache *cc;     // points into n->u.node_pg_call<N>.cc
};
static struct pg_call_link *all_pg_call_nodes_head = NULL;

static void
remember_pg_call_node(NODE *cn, NODE **sp_body_field, struct callcache *cc)
{
    struct pg_call_link *link = malloc(sizeof(*link));
    link->next = all_pg_call_nodes_head;
    link->cn = cn;
    link->sp_body_field = sp_body_field;
    link->cc = cc;
    all_pg_call_nodes_head = link;
}

// Walk every tracked pg_call NODE.  For each whose cc has been
// populated this run (cc->serial != 0 implies cc->body was set by a
// slowpath), set `sp_body = cc->body` and clear the cached HORG/HOPT
// hashes so subsequent astro_cs_compile recomputes HOPT against the
// new sp_body.  Called from main.c immediately before
// build_code_store_pgsd's astro_cs_compile pass.
void
naruby_update_sp_bodies_from_cc(void)
{
    for (struct pg_call_link *l = all_pg_call_nodes_head; l; l = l->next) {
        if (l->cc->serial == 0 || l->cc->body == NULL) continue;
        // sp_body is excluded from HORG (naruby_gen.rb hash_call kind=horg)
        // but participates in HOPT — overwrite the slot, then invalidate
        // both cached hashes up the parent chain so the next HOPT()
        // recomputes against the cc-derived body.
        *l->sp_body_field = l->cc->body;
        clear_hash(l->cn);
    }
}

static void
callsite_add_full(const char *name, NODE *cn, NODE **target_field, uint32_t *locals_cnt_field)
{
    struct callsite *cs = malloc(sizeof(struct callsite));
    cs->cn = cn;
    cs->name = name;
    cs->target_field = target_field;
    cs->locals_cnt_field = locals_cnt_field;
    cs->prev = callsites;
    callsites = cs;
}

static void
callsite_add(const char *name, NODE *cn, NODE **target_field)
{
    callsite_add_full(name, cn, target_field, NULL);
}

// Resolve forward references at parse-end: each call-site registered
// via `callsite_add[_full]` (because its target body wasn't yet in
// code_repo when the parser saw the call) gets its `sp_body` /
// `locals_cnt` fields patched here once every `def` has been parsed.
//
// PGO-aware sp_body selection (= "use the body the previous run
// observed") is NOT done here.  Instead, that's done at the end of
// `build_code_store_pgsd` by walking the all_pg_call_nodes list and
// snapping `sp_body = cc->body` from the just-finished run's cc state.
// Persistence rides on the SD/PGSD's own HOPT-keyed identity in
// hopt_index.txt — no separate profile file.
static void
callsite_resolve(void)
{
    struct callsite *cs = callsites;
    callsites = NULL;

    while (cs) {
        struct callsite *cs_prev = cs->prev;
        NODE *body = code_repo_find_by_name(cs->name);
        if (body == NULL) {
            printf("%s is not defined.\n", cs->name);
            exit(1);
        }
        if (cs->target_field) {
            *cs->target_field = body;
        }
        if (cs->locals_cnt_field) {
            *cs->locals_cnt_field = code_repo_find_locals_cnt_by_body(body);
        }
        // sp_body is excluded from HORG (see naruby_gen.rb), so the
        // patch doesn't change HORG; clear_hash is for the
        // static_lang path (where node_call_static.body IS hashed) and
        // is harmless elsewhere.
        clear_hash(cs->cn);
        free(cs);
        cs = cs_prev;
    }
}

// Generic fallback call-node builder.  Used for static_lang (-s) and
// for argc > 3 (where node_pg_call_<N> isn't available).  Static-lang
// goes through `node_call_static` (parse-time body resolution); the
// default path emits the plain `node_call` (cc-indirect dispatch, no
// sp_body — PGSD bake doesn't apply, only AOT SDs).
static NODE *
alloc_call(const char *fname, uint32_t args_cnt, uint32_t call_arg_idx)
{
    if (OPTION.static_lang) {
        NODE *body = code_repo_find_by_name(fname);
        if (body == NULL) {
            body = ALLOC_node_call_static(NULL, call_arg_idx);
            callsite_add(fname, body, &body->u.node_call_static.body);
        }
        return body;
    }
    else {
        return ALLOC_node_call(fname, args_cnt, call_arg_idx);
    }
}

// Arity-specialized AOT/interp call construction.  Mirrors
// `alloc_pg_call_specialized` but emits `node_call_<N>` (no sp_body —
// body is resolved through cc on every call).  Used in plain/AOT
// modes for argc ≤ 3 so that AOT and PG share the same arg-eval /
// fresh-frame shape; the only remaining difference between the two is
// "cc->body indirect dispatch" vs "sp_body baked direct call", which
// is exactly the inlining win we want to measure.
//
// Returns NULL if this arity isn't supported (caller falls back to
// lset+seq + alloc_call).
static NODE *
alloc_call_specialized(const char *fname, uint32_t call_arg_idx,
                       uint32_t args_cnt, NODE **arg_nodes)
{
    if (args_cnt > 3) return NULL;

    NODE *body = code_repo_find_by_name(fname);
    uint32_t locals_cnt = body ? code_repo_find_locals_cnt_by_name(fname) : 1;
    if (locals_cnt < args_cnt) locals_cnt = args_cnt;

    NODE *cn;
    uint32_t *lc_target;
    switch (args_cnt) {
      case 0:
        cn = ALLOC_node_call_0(fname, call_arg_idx, locals_cnt);
        lc_target = &cn->u.node_call_0.locals_cnt;
        break;
      case 1:
        cn = ALLOC_node_call_1(fname, call_arg_idx, locals_cnt, arg_nodes[0]);
        lc_target = &cn->u.node_call_1.locals_cnt;
        break;
      case 2:
        cn = ALLOC_node_call_2(fname, call_arg_idx, locals_cnt,
                               arg_nodes[0], arg_nodes[1]);
        lc_target = &cn->u.node_call_2.locals_cnt;
        break;
      case 3:
        cn = ALLOC_node_call_3(fname, call_arg_idx, locals_cnt,
                               arg_nodes[0], arg_nodes[1], arg_nodes[2]);
        lc_target = &cn->u.node_call_3.locals_cnt;
        break;
      default:
        return NULL;
    }

    if (!body) {
        // Forward ref: only locals_cnt needs patching at parse end
        // (no sp_body to resolve).
        callsite_add_full(fname, cn, NULL, lc_target);
    }
    return cn;
}

// Arity-specialized PG call construction.  In pg_mode for argc ≤ 3 we
// fold argument evaluation into the call node itself (`node_pg_call_<N>`)
// instead of emitting separate `node_lset` + `node_seq` chain.  This
// gives one EVAL_ function body containing arg evaluation, store, and
// dispatch — gcc sees the dataflow and can inline the chain better.
//
// Returns NULL if this arity isn't supported (caller falls back to
// lset+seq + alloc_call).  Used unconditionally for argc≤3 calls in
// non-static_lang mode — the parser doesn't gate on -p any more.
// Whether PGSDs actually get baked for these call sites is a runtime
// decision (`-p`), not a parser decision.
static NODE *
alloc_pg_call_specialized(const char *fname, uint32_t call_arg_idx,
                          uint32_t args_cnt, NODE **arg_nodes)
{
    if (args_cnt > 3) return NULL;

    NODE *body = code_repo_find_by_name(fname);
    NODE *placeholder = body ? body : ALLOC_node_num(0);
    // If body already known (forward decl resolved), we have its
    // locals_cnt now.  Else use placeholder 1; callsite_resolve will
    // overwrite at end of parse.  Min 1 because VLA `VALUE F[0]` is
    // ill-defined; pg_call0 special-cases inside its body.
    uint32_t locals_cnt = body ? code_repo_find_locals_cnt_by_name(fname) : 1;
    if (locals_cnt < args_cnt) locals_cnt = args_cnt;

    NODE *cn;
    NODE **sp_target;
    uint32_t *lc_target;
    switch (args_cnt) {
      case 0:
        cn = ALLOC_node_pg_call0(fname, call_arg_idx, locals_cnt, placeholder);
        sp_target = &cn->u.node_pg_call0.sp_body;
        lc_target = &cn->u.node_pg_call0.locals_cnt;
        break;
      case 1:
        cn = ALLOC_node_pg_call1(fname, call_arg_idx, locals_cnt, placeholder,
                                 arg_nodes[0]);
        sp_target = &cn->u.node_pg_call1.sp_body;
        lc_target = &cn->u.node_pg_call1.locals_cnt;
        break;
      case 2:
        cn = ALLOC_node_pg_call2(fname, call_arg_idx, locals_cnt, placeholder,
                                 arg_nodes[0], arg_nodes[1]);
        sp_target = &cn->u.node_pg_call2.sp_body;
        lc_target = &cn->u.node_pg_call2.locals_cnt;
        break;
      case 3:
        cn = ALLOC_node_pg_call3(fname, call_arg_idx, locals_cnt, placeholder,
                                 arg_nodes[0], arg_nodes[1], arg_nodes[2]);
        sp_target = &cn->u.node_pg_call3.sp_body;
        lc_target = &cn->u.node_pg_call3.locals_cnt;
        break;
      default:
        return NULL;
    }

    // Forward ref: register for callsite_resolve at parse end.  Already-
    // resolved calls (body found in code_repo) skip registration — their
    // sp_body is set above to `placeholder == body`.
    if (!body) {
        callsite_add_full(fname, cn, sp_target, lc_target);
    }
    // Track for build_code_store_pgsd: at exit, the bake walks this
    // list and updates each node's sp_body from cc->body.
    {
        struct callcache *cc_ptr = NULL;
        switch (args_cnt) {
          case 0: cc_ptr = &cn->u.node_pg_call0.cc; break;
          case 1: cc_ptr = &cn->u.node_pg_call1.cc; break;
          case 2: cc_ptr = &cn->u.node_pg_call2.cc; break;
          case 3: cc_ptr = &cn->u.node_pg_call3.cc; break;
        }
        remember_pg_call_node(cn, sp_target, cc_ptr);
    }
    return cn;
}

static int
integer2int(pm_integer_t *integer)
{
    int n = integer->value;
    if (integer->negative) {
        return -n;
    }
    else {
        return n;
    }
}

struct frame_context {
    uint32_t arg_index;
    uint32_t max_cnt;
    pm_constant_id_list_t *locals;
    struct frame_context *prev;
};

struct transduce_context {
    struct frame_context *frame;
    pm_parser_t *parser;
    bool verbose;
};

static void
push_frame(struct transduce_context *tc, pm_constant_id_list_t *locals)
{
    struct frame_context *f = malloc(sizeof(struct frame_context));
    f->prev = tc->frame;
    tc->frame = f;
    f->locals = locals;
    f->arg_index = f->max_cnt = locals->size;
}

static void
pop_frame(struct transduce_context *tc)
{
    struct frame_context *f = tc->frame;
    tc->frame = f->prev;
    free(f);
}

static uint32_t
lvar_index(struct transduce_context *tc, pm_constant_id_t cid)
{
    pm_constant_id_list_t *list = tc->frame->locals;
    for (size_t i=0; i<list->size; i++) {
        if (list->ids[i] == cid) {
            return i;
        }
    }
    fprintf(stderr, "lvar is not found...");
    exit(1);
}

static uint32_t
arg_index(struct transduce_context *tc)
{
    return tc->frame->arg_index;
}

static uint32_t
increment_arg_index(struct transduce_context *tc)
{
    uint32_t arg_index = tc->frame->arg_index;
    tc->frame->arg_index = arg_index + 1;
    if (tc->frame->max_cnt < tc->frame->arg_index) {
        tc->frame->max_cnt = tc->frame->arg_index;
    }
    return arg_index;
}

static void
rewind_arg_index(struct transduce_context *tc, uint32_t idx)
{
    tc->frame->arg_index = idx;
}

static bool
ceq(struct transduce_context *tc, pm_constant_id_t cid, const char *str)
{
    pm_constant_t *c = pm_constant_pool_id_to_constant(&tc->parser->constant_pool, cid);

    if (c->length == strlen(str)) {
        return memcmp(c->start, str, c->length) == 0;
    }
    else {
        return false;
    }
}

static bool
is_binop(struct transduce_context *tc, pm_constant_id_t name)
{
    if (0) {}
    else if (ceq(tc, name, "+")) return true;
    else if (ceq(tc, name, "*")) return true;
    else if (ceq(tc, name, "-")) return true;
    else if (ceq(tc, name, "/")) return true;
    else if (ceq(tc, name, "%")) return true;
    else if (ceq(tc, name, "<")) return true;
    else if (ceq(tc, name, "<=")) return true;
    else if (ceq(tc, name, ">")) return true;
    else if (ceq(tc, name, ">=")) return true;
    else if (ceq(tc, name, "!=")) return true;
    else if (ceq(tc, name, "==")) return true;

    return false;
}

static NODE *
alloc_binop(struct transduce_context *tc, pm_constant_id_t name, NODE *lhs, NODE *rhs)
{
    if (0) {}
    else if (ceq(tc, name, "+")) {
        return ALLOC_node_add(lhs, rhs);
    }
    else if (ceq(tc, name, "*")) {
        return ALLOC_node_mul(lhs, rhs);
    }
    else if (ceq(tc, name, "-")) {
        return ALLOC_node_sub(lhs, rhs);
    }
    else if (ceq(tc, name, "/")) {
        return ALLOC_node_div(lhs, rhs);
    }
    else if (ceq(tc, name, "%")) {
        return ALLOC_node_mod(lhs, rhs);
    }
    else if (ceq(tc, name, "<")) {
        return ALLOC_node_lt(lhs, rhs);
    }
    else if (ceq(tc, name, "<=")) {
        return ALLOC_node_le(lhs, rhs);
    }
    else if (ceq(tc, name, ">")) {
        return ALLOC_node_gt(lhs, rhs);
    }
    else if (ceq(tc, name, ">=")) {
        return ALLOC_node_ge(lhs, rhs);
    }
    else if (ceq(tc, name, "!=")) {
        return ALLOC_node_neq(lhs, rhs);
    }
    else if (ceq(tc, name, "==")) {
        return ALLOC_node_eq(lhs, rhs);
    }
    else {
        return NULL;
    }
}

#pragma GCC diagnostic ignored "-Wunused-variable"

NODE *
transduce(struct transduce_context *tc, pm_node_t *node, int indent) {
    if (!node) return NULL;

    if (tc->verbose) {
        for (int i = 0; i < indent; i++) printf("  ");
        printf("Node type: %s\n", pm_node_type_name(node->type));
    }

    switch (node->type) {
      case PM_ALIAS_GLOBAL_VARIABLE_NODE: {
          pm_alias_global_variable_node_t *n = (pm_alias_global_variable_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_ALIAS_GLOBAL_VARIABLE_NODE\n");
          break;
      }
      case PM_ALIAS_METHOD_NODE: {
          pm_alias_method_node_t *n = (pm_alias_method_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_ALIAS_METHOD_NODE\n");
          break;
      }
      case PM_ALTERNATION_PATTERN_NODE: {
          pm_alternation_pattern_node_t *n = (pm_alternation_pattern_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_ALTERNATION_PATTERN_NODE\n");
          break;
      }
      case PM_AND_NODE: {
          pm_and_node_t *n = (pm_and_node_t *)(node);
          return ALLOC_node_if(TRANSDUCE(n->left), ALLOC_node_if(TRANSDUCE(n->right), ALLOC_node_num(1), ALLOC_node_num(0)), ALLOC_node_num(0));
      }
      case PM_ARGUMENTS_NODE: {
          pm_arguments_node_t *n = (pm_arguments_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_ARGUMENTS_NODE\n");
          break;
      }
      case PM_ARRAY_NODE: {
          pm_array_node_t *n = (pm_array_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_ARRAY_NODE\n");
          break;
      }
      case PM_ARRAY_PATTERN_NODE: {
          pm_array_pattern_node_t *n = (pm_array_pattern_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_ARRAY_PATTERN_NODE\n");
          break;
      }
      case PM_ASSOC_NODE: {
          pm_assoc_node_t *n = (pm_assoc_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_ASSOC_NODE\n");
          break;
      }
      case PM_ASSOC_SPLAT_NODE: {
          pm_assoc_splat_node_t *n = (pm_assoc_splat_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_ASSOC_SPLAT_NODE\n");
          break;
      }
      case PM_BACK_REFERENCE_READ_NODE: {
          pm_back_reference_read_node_t *n = (pm_back_reference_read_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_BACK_REFERENCE_READ_NODE\n");
          break;
      }
      case PM_BEGIN_NODE: {
          pm_begin_node_t *n = (pm_begin_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_BEGIN_NODE\n");
          break;
      }
      case PM_BLOCK_ARGUMENT_NODE: {
          pm_block_argument_node_t *n = (pm_block_argument_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_BLOCK_ARGUMENT_NODE\n");
          break;
      }
      case PM_BLOCK_LOCAL_VARIABLE_NODE: {
          pm_block_local_variable_node_t *n = (pm_block_local_variable_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_BLOCK_LOCAL_VARIABLE_NODE\n");
          break;
      }
      case PM_BLOCK_NODE: {
          pm_block_node_t *n = (pm_block_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_BLOCK_NODE\n");
          break;
      }
      case PM_BLOCK_PARAMETER_NODE: {
          pm_block_parameter_node_t *n = (pm_block_parameter_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_BLOCK_PARAMETER_NODE\n");
          break;
      }
      case PM_BLOCK_PARAMETERS_NODE: {
          pm_block_parameters_node_t *n = (pm_block_parameters_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_BLOCK_PARAMETERS_NODE\n");
          break;
      }
      case PM_BREAK_NODE: {
          pm_break_node_t *n = (pm_break_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_BREAK_NODE\n");
          break;
      }
      case PM_CALL_AND_WRITE_NODE: {
          pm_call_and_write_node_t *n = (pm_call_and_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CALL_AND_WRITE_NODE\n");
          break;
      }
      case PM_CALL_NODE: {
          pm_call_node_t *n = (pm_call_node_t *)(node);
          pm_arguments_node_t *args = (pm_arguments_node_t *)n->arguments;
          pm_node_t *lhs = n->receiver;
          pm_node_t *rhs = args ? args->arguments.nodes[0] : NULL;
          uint32_t args_cnt = args ? args->arguments.size : 0;

          if (args_cnt == 1 && is_binop(tc, n->name)) {
              return alloc_binop(tc, n->name, TRANSDUCE(lhs), TRANSDUCE(rhs));
          }
          else {
              const char *fname = alloc_cstr(tc->parser, n->name);
              uint32_t call_arg_idx = arg_index(tc);

              // arity ≤ 3 (non-static_lang): fold args into an arity-
              // specialized call NODE.  With `-p` (= we're going to PG-
              // bake at exit) emit `node_pg_call_<N>` so the call site
              // carries an `sp_body` operand that build_code_store_pgsd
              // updates from cc->body.  Without `-p`, emit the simpler
              // `node_call_<N>` (sp_body-less, cc-indirect dispatch,
              // no 2-step guard).
              if (!OPTION.static_lang && args_cnt <= 3) {
                  NODE *arg_nodes[3] = { NULL, NULL, NULL };
                  for (uint32_t i = 0; i < args_cnt; i++) {
                      increment_arg_index(tc);
                  }
                  for (uint32_t i = 0; i < args_cnt; i++) {
                      arg_nodes[i] = TRANSDUCE(args->arguments.nodes[i]);
                  }
                  rewind_arg_index(tc, call_arg_idx);
                  if (OPTION.pg_at_exit) {
                      return alloc_pg_call_specialized(fname, call_arg_idx,
                                                       args_cnt, arg_nodes);
                  }
                  return alloc_call_specialized(fname, call_arg_idx,
                                                args_cnt, arg_nodes);
              }

              // Generic path: emit lset chain that writes args into
              // fp[call_arg_idx + i], then dispatch to alloc_call.  Used
              // for static_lang and arity > 3.
              if (args != 0) {
                  NODE *nargs = NULL;

                  for (size_t i=0; i<args_cnt; i++) {
                      NODE *arg = ALLOC_node_lset(increment_arg_index(tc), TRANSDUCE(args->arguments.nodes[i]));

                      if (i==0) {
                          nargs = arg;
                      }
                      else {
                          nargs = ALLOC_node_seq(nargs, arg);
                      }
                  }

                  NODE *ncall = ALLOC_node_seq(nargs, alloc_call(fname, args_cnt, call_arg_idx));

                  rewind_arg_index(tc, call_arg_idx);
                  return ncall;
              }
              else {
                  return alloc_call(fname, args_cnt, call_arg_idx);
              }
          }
          break;
      }
      case PM_CALL_OPERATOR_WRITE_NODE: {
          pm_call_operator_write_node_t *n = (pm_call_operator_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CALL_OPERATOR_WRITE_NODE\n");
          break;
      }
      case PM_CALL_OR_WRITE_NODE: {
          pm_call_or_write_node_t *n = (pm_call_or_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CALL_OR_WRITE_NODE\n");
          break;
      }
      case PM_CALL_TARGET_NODE: {
          pm_call_target_node_t *n = (pm_call_target_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CALL_TARGET_NODE\n");
          break;
      }
      case PM_CAPTURE_PATTERN_NODE: {
          pm_capture_pattern_node_t *n = (pm_capture_pattern_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CAPTURE_PATTERN_NODE\n");
          break;
      }
      case PM_CASE_MATCH_NODE: {
          pm_case_match_node_t *n = (pm_case_match_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CASE_MATCH_NODE\n");
          break;
      }
      case PM_CASE_NODE: {
          pm_case_node_t *n = (pm_case_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CASE_NODE\n");
          break;
      }
      case PM_CLASS_NODE: {
          pm_class_node_t *n = (pm_class_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CLASS_NODE\n");
          break;
      }
      case PM_CLASS_VARIABLE_AND_WRITE_NODE: {
          pm_class_variable_and_write_node_t *n = (pm_class_variable_and_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CLASS_VARIABLE_AND_WRITE_NODE\n");
          break;
      }
      case PM_CLASS_VARIABLE_OPERATOR_WRITE_NODE: {
          pm_class_variable_operator_write_node_t *n = (pm_class_variable_operator_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CLASS_VARIABLE_OPERATOR_WRITE_NODE\n");
          break;
      }
      case PM_CLASS_VARIABLE_OR_WRITE_NODE: {
          pm_class_variable_or_write_node_t *n = (pm_class_variable_or_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CLASS_VARIABLE_OR_WRITE_NODE\n");
          break;
      }
      case PM_CLASS_VARIABLE_READ_NODE: {
          pm_class_variable_read_node_t *n = (pm_class_variable_read_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CLASS_VARIABLE_READ_NODE\n");
          break;
      }
      case PM_CLASS_VARIABLE_TARGET_NODE: {
          pm_class_variable_target_node_t *n = (pm_class_variable_target_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CLASS_VARIABLE_TARGET_NODE\n");
          break;
      }
      case PM_CLASS_VARIABLE_WRITE_NODE: {
          pm_class_variable_write_node_t *n = (pm_class_variable_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CLASS_VARIABLE_WRITE_NODE\n");
          break;
      }
      case PM_CONSTANT_AND_WRITE_NODE: {
          pm_constant_and_write_node_t *n = (pm_constant_and_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_AND_WRITE_NODE\n");
          break;
      }
      case PM_CONSTANT_OPERATOR_WRITE_NODE: {
          pm_constant_operator_write_node_t *n = (pm_constant_operator_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_OPERATOR_WRITE_NODE\n");
          break;
      }
      case PM_CONSTANT_OR_WRITE_NODE: {
          pm_constant_or_write_node_t *n = (pm_constant_or_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_OR_WRITE_NODE\n");
          break;
      }
      case PM_CONSTANT_PATH_AND_WRITE_NODE: {
          pm_constant_path_and_write_node_t *n = (pm_constant_path_and_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_PATH_AND_WRITE_NODE\n");
          break;
      }
      case PM_CONSTANT_PATH_NODE: {
          pm_constant_path_node_t *n = (pm_constant_path_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_PATH_NODE\n");
          break;
      }
      case PM_CONSTANT_PATH_OPERATOR_WRITE_NODE: {
          pm_constant_path_operator_write_node_t *n = (pm_constant_path_operator_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_PATH_OPERATOR_WRITE_NODE\n");
          break;
      }
      case PM_CONSTANT_PATH_OR_WRITE_NODE: {
          pm_constant_path_or_write_node_t *n = (pm_constant_path_or_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_PATH_OR_WRITE_NODE\n");
          break;
      }
      case PM_CONSTANT_PATH_TARGET_NODE: {
          pm_constant_path_target_node_t *n = (pm_constant_path_target_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_PATH_TARGET_NODE\n");
          break;
      }
      case PM_CONSTANT_PATH_WRITE_NODE: {
          pm_constant_path_write_node_t *n = (pm_constant_path_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_PATH_WRITE_NODE\n");
          break;
      }
      case PM_CONSTANT_READ_NODE: {
          pm_constant_read_node_t *n = (pm_constant_read_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_READ_NODE\n");
          break;
      }
      case PM_CONSTANT_TARGET_NODE: {
          pm_constant_target_node_t *n = (pm_constant_target_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_TARGET_NODE\n");
          break;
      }
      case PM_CONSTANT_WRITE_NODE: {
          pm_constant_write_node_t *n = (pm_constant_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_CONSTANT_WRITE_NODE\n");
          break;
      }
      case PM_DEF_NODE: {
          pm_def_node_t *n = (pm_def_node_t *)(node);
          const char *name = alloc_cstr(tc->parser, n->name);
          NODE *fn;
          uint32_t max_cnt, params_cnt = 0;

          if (n->parameters) {
              pm_parameters_node_t *pn = (pm_parameters_node_t *)n->parameters;
              params_cnt = pn->requireds.size;
          }

          push_frame(tc, &n->locals);
          {
              fn = TRANSDUCE(n->body);
              max_cnt = tc->frame->max_cnt;
          }
          pop_frame(tc);

          // The body's "true" locals count = params + declared body
          // locals (Prism's `n->locals.size`), NOT max_cnt.  max_cnt
          // includes the slot indices the parser assigned to nested
          // call args, which were used in the OLD calling convention
          // (writing args into caller's `fp[arg_index..]` then bumping
          // fp past them).  In the new pg_call_<N> path each call has
          // its own VLA frame, so nested call arg slots aren't
          // allocated in the body's frame.  Sizing F to max_cnt would
          // over-allocate (e.g. ackermann body has 2 declared locals
          // but max_cnt = 6 due to 4-deep nested calls).
          uint32_t body_locals = (uint32_t)n->locals.size;
          code_repo_add2(name, fn, true, body_locals);
          return ALLOC_node_def(name, fn, params_cnt, max_cnt);
      }
      case PM_DEFINED_NODE: {
          pm_defined_node_t *n = (pm_defined_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_DEFINED_NODE\n");
          break;
      }
      case PM_ELSE_NODE: {
          pm_else_node_t *n = (pm_else_node_t *)(node);
          return TRANSDUCE(n->statements);
      }
      case PM_EMBEDDED_STATEMENTS_NODE: {
          pm_embedded_statements_node_t *n = (pm_embedded_statements_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_EMBEDDED_STATEMENTS_NODE\n");
          break;
      }
      case PM_EMBEDDED_VARIABLE_NODE: {
          pm_embedded_variable_node_t *n = (pm_embedded_variable_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_EMBEDDED_VARIABLE_NODE\n");
          break;
      }
      case PM_ENSURE_NODE: {
          pm_ensure_node_t *n = (pm_ensure_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_ENSURE_NODE\n");
          break;
      }
      case PM_FALSE_NODE: {
          pm_false_node_t *n = (pm_false_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_FALSE_NODE\n");
          break;
      }
      case PM_FIND_PATTERN_NODE: {
          pm_find_pattern_node_t *n = (pm_find_pattern_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_FIND_PATTERN_NODE\n");
          break;
      }
      case PM_FLIP_FLOP_NODE: {
          pm_flip_flop_node_t *n = (pm_flip_flop_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_FLIP_FLOP_NODE\n");
          break;
      }
      case PM_FLOAT_NODE: {
          pm_float_node_t *n = (pm_float_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_FLOAT_NODE\n");
          break;
      }
      case PM_FOR_NODE: {
          pm_for_node_t *n = (pm_for_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_FOR_NODE\n");
          break;
      }
      case PM_FORWARDING_ARGUMENTS_NODE: {
          pm_forwarding_arguments_node_t *n = (pm_forwarding_arguments_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_FORWARDING_ARGUMENTS_NODE\n");
          break;
      }
      case PM_FORWARDING_PARAMETER_NODE: {
          pm_forwarding_parameter_node_t *n = (pm_forwarding_parameter_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_FORWARDING_PARAMETER_NODE\n");
          break;
      }
      case PM_FORWARDING_SUPER_NODE: {
          pm_forwarding_super_node_t *n = (pm_forwarding_super_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_FORWARDING_SUPER_NODE\n");
          break;
      }
      case PM_GLOBAL_VARIABLE_AND_WRITE_NODE: {
          pm_global_variable_and_write_node_t *n = (pm_global_variable_and_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_GLOBAL_VARIABLE_AND_WRITE_NODE\n");
          break;
      }
      case PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE: {
          pm_global_variable_operator_write_node_t *n = (pm_global_variable_operator_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE\n");
          break;
      }
      case PM_GLOBAL_VARIABLE_OR_WRITE_NODE: {
          pm_global_variable_or_write_node_t *n = (pm_global_variable_or_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_GLOBAL_VARIABLE_OR_WRITE_NODE\n");
          break;
      }
      case PM_GLOBAL_VARIABLE_READ_NODE: {
          pm_global_variable_read_node_t *n = (pm_global_variable_read_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_GLOBAL_VARIABLE_READ_NODE\n");
          break;
      }
      case PM_GLOBAL_VARIABLE_TARGET_NODE: {
          pm_global_variable_target_node_t *n = (pm_global_variable_target_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_GLOBAL_VARIABLE_TARGET_NODE\n");
          break;
      }
      case PM_GLOBAL_VARIABLE_WRITE_NODE: {
          pm_global_variable_write_node_t *n = (pm_global_variable_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_GLOBAL_VARIABLE_WRITE_NODE\n");
          break;
      }
      case PM_HASH_NODE: {
          pm_hash_node_t *n = (pm_hash_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_HASH_NODE\n");
          break;
      }
      case PM_HASH_PATTERN_NODE: {
          pm_hash_pattern_node_t *n = (pm_hash_pattern_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_HASH_PATTERN_NODE\n");
          break;
      }
      case PM_IF_NODE: {
          pm_if_node_t *n = (pm_if_node_t *)(node);
          return ALLOC_node_if(TRANSDUCE(n->predicate), TRANSDUCE(n->statements), n->subsequent ? TRANSDUCE(n->subsequent) : ALLOC_node_num(0));
      }
      case PM_IMAGINARY_NODE: {
          pm_imaginary_node_t *n = (pm_imaginary_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_IMAGINARY_NODE\n");
          break;
      }
      case PM_IMPLICIT_NODE: {
          pm_implicit_node_t *n = (pm_implicit_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_IMPLICIT_NODE\n");
          break;
      }
      case PM_IMPLICIT_REST_NODE: {
          pm_implicit_rest_node_t *n = (pm_implicit_rest_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_IMPLICIT_REST_NODE\n");
          break;
      }
      case PM_IN_NODE: {
          pm_in_node_t *n = (pm_in_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_IN_NODE\n");
          break;
      }
      case PM_INDEX_AND_WRITE_NODE: {
          pm_index_and_write_node_t *n = (pm_index_and_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INDEX_AND_WRITE_NODE\n");
          break;
      }
      case PM_INDEX_OPERATOR_WRITE_NODE: {
          pm_index_operator_write_node_t *n = (pm_index_operator_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INDEX_OPERATOR_WRITE_NODE\n");
          break;
      }
      case PM_INDEX_OR_WRITE_NODE: {
          pm_index_or_write_node_t *n = (pm_index_or_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INDEX_OR_WRITE_NODE\n");
          break;
      }
      case PM_INDEX_TARGET_NODE: {
          pm_index_target_node_t *n = (pm_index_target_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INDEX_TARGET_NODE\n");
          break;
      }
      case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: {
          pm_instance_variable_and_write_node_t *n = (pm_instance_variable_and_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INSTANCE_VARIABLE_AND_WRITE_NODE\n");
          break;
      }
      case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {
          pm_instance_variable_operator_write_node_t *n = (pm_instance_variable_operator_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE\n");
          break;
      }
      case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {
          pm_instance_variable_or_write_node_t *n = (pm_instance_variable_or_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INSTANCE_VARIABLE_OR_WRITE_NODE\n");
          break;
      }
      case PM_INSTANCE_VARIABLE_READ_NODE: {
          pm_instance_variable_read_node_t *n = (pm_instance_variable_read_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INSTANCE_VARIABLE_READ_NODE\n");
          break;
      }
      case PM_INSTANCE_VARIABLE_TARGET_NODE: {
          pm_instance_variable_target_node_t *n = (pm_instance_variable_target_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INSTANCE_VARIABLE_TARGET_NODE\n");
          break;
      }
      case PM_INSTANCE_VARIABLE_WRITE_NODE: {
          pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INSTANCE_VARIABLE_WRITE_NODE\n");
          break;
      }
      case PM_INTEGER_NODE: {
          pm_integer_node_t *n = (pm_integer_node_t *)(node);
          int val = integer2int(&n->value);
          // fprintf(stderr, "val:%d\n", val);
          return ALLOC_node_num(val);
      }
      case PM_INTERPOLATED_MATCH_LAST_LINE_NODE: {
          pm_interpolated_match_last_line_node_t *n = (pm_interpolated_match_last_line_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INTERPOLATED_MATCH_LAST_LINE_NODE\n");
          break;
      }
      case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE: {
          pm_interpolated_regular_expression_node_t *n = (pm_interpolated_regular_expression_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INTERPOLATED_REGULAR_EXPRESSION_NODE\n");
          break;
      }
      case PM_INTERPOLATED_STRING_NODE: {
          pm_interpolated_string_node_t *n = (pm_interpolated_string_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INTERPOLATED_STRING_NODE\n");
          break;
      }
      case PM_INTERPOLATED_SYMBOL_NODE: {
          pm_interpolated_symbol_node_t *n = (pm_interpolated_symbol_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INTERPOLATED_SYMBOL_NODE\n");
          break;
      }
      case PM_INTERPOLATED_X_STRING_NODE: {
          pm_interpolated_x_string_node_t *n = (pm_interpolated_x_string_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_INTERPOLATED_X_STRING_NODE\n");
          break;
      }
      case PM_IT_LOCAL_VARIABLE_READ_NODE: {
          pm_it_local_variable_read_node_t *n = (pm_it_local_variable_read_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_IT_LOCAL_VARIABLE_READ_NODE\n");
          break;
      }
      case PM_IT_PARAMETERS_NODE: {
          pm_it_parameters_node_t *n = (pm_it_parameters_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_IT_PARAMETERS_NODE\n");
          break;
      }
      case PM_KEYWORD_HASH_NODE: {
          pm_keyword_hash_node_t *n = (pm_keyword_hash_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_KEYWORD_HASH_NODE\n");
          break;
      }
      case PM_KEYWORD_REST_PARAMETER_NODE: {
          pm_keyword_rest_parameter_node_t *n = (pm_keyword_rest_parameter_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_KEYWORD_REST_PARAMETER_NODE\n");
          break;
      }
      case PM_LAMBDA_NODE: {
          pm_lambda_node_t *n = (pm_lambda_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_LAMBDA_NODE\n");
          break;
      }
      case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
          pm_local_variable_and_write_node_t *n = (pm_local_variable_and_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_LOCAL_VARIABLE_AND_WRITE_NODE\n");
          break;
      }
      case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
          pm_local_variable_operator_write_node_t *n = (pm_local_variable_operator_write_node_t *)(node);
          uint32_t lvar_idx = lvar_index(tc, n->name);
          NODE *rhs = alloc_binop(tc, n->binary_operator, ALLOC_node_lget(lvar_idx), TRANSDUCE(n->value));
          if (rhs == NULL) {
              fprintf(stderr, "unsupported\n");
              exit(1);
          }
          return ALLOC_node_lset(lvar_idx, rhs);
      }
      case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
          pm_local_variable_or_write_node_t *n = (pm_local_variable_or_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_LOCAL_VARIABLE_OR_WRITE_NODE\n");
          break;
      }
      case PM_LOCAL_VARIABLE_READ_NODE: {
          pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)(node);
          return ALLOC_node_lget(lvar_index(tc, n->name));
      }
      case PM_LOCAL_VARIABLE_TARGET_NODE: {
          pm_local_variable_target_node_t *n = (pm_local_variable_target_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_LOCAL_VARIABLE_TARGET_NODE\n");
          break;
      }
      case PM_LOCAL_VARIABLE_WRITE_NODE: {
          pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)(node);
          return ALLOC_node_lset(lvar_index(tc, n->name), TRANSDUCE(n->value));
      }
      case PM_MATCH_LAST_LINE_NODE: {
          pm_match_last_line_node_t *n = (pm_match_last_line_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_MATCH_LAST_LINE_NODE\n");
          break;
      }
      case PM_MATCH_PREDICATE_NODE: {
          pm_match_predicate_node_t *n = (pm_match_predicate_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_MATCH_PREDICATE_NODE\n");
          break;
      }
      case PM_MATCH_REQUIRED_NODE: {
          pm_match_required_node_t *n = (pm_match_required_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_MATCH_REQUIRED_NODE\n");
          break;
      }
      case PM_MATCH_WRITE_NODE: {
          pm_match_write_node_t *n = (pm_match_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_MATCH_WRITE_NODE\n");
          break;
      }
      case PM_MISSING_NODE: {
          pm_missing_node_t *n = (pm_missing_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_MISSING_NODE\n");
          break;
      }
      case PM_MODULE_NODE: {
          pm_module_node_t *n = (pm_module_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_MODULE_NODE\n");
          break;
      }
      case PM_MULTI_TARGET_NODE: {
          pm_multi_target_node_t *n = (pm_multi_target_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_MULTI_TARGET_NODE\n");
          break;
      }
      case PM_MULTI_WRITE_NODE: {
          pm_multi_write_node_t *n = (pm_multi_write_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_MULTI_WRITE_NODE\n");
          break;
      }
      case PM_NEXT_NODE: {
          pm_next_node_t *n = (pm_next_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_NEXT_NODE\n");
          break;
      }
      case PM_NIL_NODE: {
          pm_nil_node_t *n = (pm_nil_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_NIL_NODE\n");
          break;
      }
      case PM_NO_KEYWORDS_PARAMETER_NODE: {
          pm_no_keywords_parameter_node_t *n = (pm_no_keywords_parameter_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_NO_KEYWORDS_PARAMETER_NODE\n");
          break;
      }
      case PM_NUMBERED_PARAMETERS_NODE: {
          pm_numbered_parameters_node_t *n = (pm_numbered_parameters_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_NUMBERED_PARAMETERS_NODE\n");
          break;
      }
      case PM_NUMBERED_REFERENCE_READ_NODE: {
          pm_numbered_reference_read_node_t *n = (pm_numbered_reference_read_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_NUMBERED_REFERENCE_READ_NODE\n");
          break;
      }
      case PM_OPTIONAL_KEYWORD_PARAMETER_NODE: {
          pm_optional_keyword_parameter_node_t *n = (pm_optional_keyword_parameter_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_OPTIONAL_KEYWORD_PARAMETER_NODE\n");
          break;
      }
      case PM_OPTIONAL_PARAMETER_NODE: {
          pm_optional_parameter_node_t *n = (pm_optional_parameter_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_OPTIONAL_PARAMETER_NODE\n");
          break;
      }
      case PM_OR_NODE: {
          pm_or_node_t *n = (pm_or_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_OR_NODE\n");
          break;
      }
      case PM_PARAMETERS_NODE: {
          pm_parameters_node_t *n = (pm_parameters_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_PARAMETERS_NODE\n");
          break;
      }
      case PM_PARENTHESES_NODE: {
          pm_parentheses_node_t *n = (pm_parentheses_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_PARENTHESES_NODE\n");
          break;
      }
      case PM_PINNED_EXPRESSION_NODE: {
          pm_pinned_expression_node_t *n = (pm_pinned_expression_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_PINNED_EXPRESSION_NODE\n");
          break;
      }
      case PM_PINNED_VARIABLE_NODE: {
          pm_pinned_variable_node_t *n = (pm_pinned_variable_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_PINNED_VARIABLE_NODE\n");
          break;
      }
      case PM_POST_EXECUTION_NODE: {
          pm_post_execution_node_t *n = (pm_post_execution_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_POST_EXECUTION_NODE\n");
          break;
      }
      case PM_PRE_EXECUTION_NODE: {
          pm_pre_execution_node_t *n = (pm_pre_execution_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_PRE_EXECUTION_NODE\n");
          break;
      }
      case PM_PROGRAM_NODE: {
          pm_program_node_t *n = (pm_program_node_t *)(node);
          NODE *nn;
          push_frame(tc, &n->locals);
          {
              nn = TRANSDUCE(n->statements);
          }
          pop_frame(tc);
          return nn;
      }
      case PM_RANGE_NODE: {
          pm_range_node_t *n = (pm_range_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_RANGE_NODE\n");
          break;
      }
      case PM_RATIONAL_NODE: {
          pm_rational_node_t *n = (pm_rational_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_RATIONAL_NODE\n");
          break;
      }
      case PM_REDO_NODE: {
          pm_redo_node_t *n = (pm_redo_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_REDO_NODE\n");
          break;
      }
      case PM_REGULAR_EXPRESSION_NODE: {
          pm_regular_expression_node_t *n = (pm_regular_expression_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_REGULAR_EXPRESSION_NODE\n");
          break;
      }
      case PM_REQUIRED_KEYWORD_PARAMETER_NODE: {
          pm_required_keyword_parameter_node_t *n = (pm_required_keyword_parameter_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_REQUIRED_KEYWORD_PARAMETER_NODE\n");
          break;
      }
      case PM_REQUIRED_PARAMETER_NODE: {
          pm_required_parameter_node_t *n = (pm_required_parameter_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_REQUIRED_PARAMETER_NODE\n");
          break;
      }
      case PM_RESCUE_MODIFIER_NODE: {
          pm_rescue_modifier_node_t *n = (pm_rescue_modifier_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_RESCUE_MODIFIER_NODE\n");
          break;
      }
      case PM_RESCUE_NODE: {
          pm_rescue_node_t *n = (pm_rescue_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_RESCUE_NODE\n");
          break;
      }
      case PM_REST_PARAMETER_NODE: {
          pm_rest_parameter_node_t *n = (pm_rest_parameter_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_REST_PARAMETER_NODE\n");
          break;
      }
      case PM_RETRY_NODE: {
          pm_retry_node_t *n = (pm_retry_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_RETRY_NODE\n");
          break;
      }
      case PM_RETURN_NODE: {
          pm_return_node_t *n = (pm_return_node_t *)(node);
          NODE *value;
          if (n->arguments) {
              pm_arguments_node_t *args = (pm_arguments_node_t *)n->arguments;
              // Multiple values (`return a, b`) aren't representable in
              // naruby's int64-only VALUE, so we just take the first.
              if (args->arguments.size > 0) {
                  value = TRANSDUCE(args->arguments.nodes[0]);
              } else {
                  value = ALLOC_node_num(0);
              }
          } else {
              // Bare `return` → return 0.
              value = ALLOC_node_num(0);
          }
          return ALLOC_node_return(value);
      }
      case PM_SELF_NODE: {
          pm_self_node_t *n = (pm_self_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_SELF_NODE\n");
          break;
      }
      case PM_SHAREABLE_CONSTANT_NODE: {
          pm_shareable_constant_node_t *n = (pm_shareable_constant_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_SHAREABLE_CONSTANT_NODE\n");
          break;
      }
      case PM_SINGLETON_CLASS_NODE: {
          pm_singleton_class_node_t *n = (pm_singleton_class_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_SINGLETON_CLASS_NODE\n");
          break;
      }
      case PM_SOURCE_ENCODING_NODE: {
          pm_source_encoding_node_t *n = (pm_source_encoding_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_SOURCE_ENCODING_NODE\n");
          break;
      }
      case PM_SOURCE_FILE_NODE: {
          pm_source_file_node_t *n = (pm_source_file_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_SOURCE_FILE_NODE\n");
          break;
      }
      case PM_SOURCE_LINE_NODE: {
          pm_source_line_node_t *n = (pm_source_line_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_SOURCE_LINE_NODE\n");
          break;
      }
      case PM_SPLAT_NODE: {
          pm_splat_node_t *n = (pm_splat_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_SPLAT_NODE\n");
          break;
      }
      case PM_STATEMENTS_NODE: {
          pm_statements_node_t *n = (pm_statements_node_t *)(node);
          NODE *seq = NULL;
          NODE **nodes = alloca(sizeof(NODE *) * n->body.size);

          for (size_t i=0; i<n->body.size; i++) {
              nodes[i] = TRANSDUCE(n->body.nodes[i]);
          }

          for (size_t i=1; i <= n->body.size; i++) {
              NODE *stmt = nodes[n->body.size - i];

              if (seq) {
                  seq = ALLOC_node_seq(stmt, seq);
              }
              else {
                  seq = stmt;
              }
          }
          return seq;
      }
      case PM_STRING_NODE: {
          pm_string_node_t *n = (pm_string_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_STRING_NODE\n");
          break;
      }
      case PM_SUPER_NODE: {
          pm_super_node_t *n = (pm_super_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_SUPER_NODE\n");
          break;
      }
      case PM_SYMBOL_NODE: {
          pm_symbol_node_t *n = (pm_symbol_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_SYMBOL_NODE\n");
          break;
      }
      case PM_TRUE_NODE: {
          pm_true_node_t *n = (pm_true_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_TRUE_NODE\n");
          break;
      }
      case PM_UNDEF_NODE: {
          pm_undef_node_t *n = (pm_undef_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_UNDEF_NODE\n");
          break;
      }
      case PM_UNLESS_NODE: {
          pm_unless_node_t *n = (pm_unless_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_UNLESS_NODE\n");
          break;
      }
      case PM_UNTIL_NODE: {
          pm_until_node_t *n = (pm_until_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_UNTIL_NODE\n");
          break;
      }
      case PM_WHEN_NODE: {
          pm_when_node_t *n = (pm_when_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_WHEN_NODE\n");
          break;
      }
      case PM_WHILE_NODE: {
          pm_while_node_t *n = (pm_while_node_t *)(node);
          return ALLOC_node_while(TRANSDUCE(n->predicate), TRANSDUCE(n->statements));
      }
      case PM_X_STRING_NODE: {
          pm_x_string_node_t *n = (pm_x_string_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_X_STRING_NODE\n");
          break;
      }
      case PM_YIELD_NODE: {
          pm_yield_node_t *n = (pm_yield_node_t *)(node);
          fprintf(stderr, "unsupported node: PM_YIELD_NODE\n");
          break;
      }
    }
    fprintf(stderr, "unreachable:%s\n", pm_node_type_name(node->type));
    exit(1);
}

static void
show_help(void)
{
    printf("naruby [options] [script]\n");
    printf("\n");
    printf("Default: load any cached SD/PGSD from code_store/ and run.\n");
    printf("Combine -c and -p to AOT-bake before run + PG-bake after.\n");
    printf("\n");
    printf("Run mode:\n");
    printf("  -i, --plain          interpreter only, ignore code_store\n");
    printf("  --aot-compile        compile-only (do not run)\n");
    printf("  -j                   JIT mode (worker process)\n");
    printf("  -s                   static-lang mode (parse-time call resolution)\n");
    printf("\n");
    printf("Bake (orthogonal — combine freely):\n");
    printf("  -c, --aot            AOT-bake SDs before run\n");
    printf("  -p, --pg             PG-bake PGSDs after run (uses cc state)\n");
    printf("  -b                   skip BOTH bakes (timing-only cached run)\n");
    printf("\n");
    printf("Other:\n");
    printf("  --ccs                clear code_store/ before run\n");
    printf("  -q                   quiet\n");
    printf("  -h                   show this help\n");
}

static bool
match_long(const char *arg, const char *name)
{
    return strcmp(arg, name) == 0;
}

static const char *
parse_option(int argc, char *argv[])
{
    const char *fname = NULL;

    if (argc == 0) {
        fprintf(stderr, "no input\n");
        exit(1);
    }

    for (int i = 1; i<argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-') {
            if (fname) fprintf(stderr, "ignore %s with %s\n", fname, arg);
            fname = arg;
            continue;
        }

        // Long options first.
        if (match_long(arg, "--plain"))             { OPTION.plain = true; continue; }
        if (match_long(arg, "--aot")
            || match_long(arg, "--aot-compile-first")) { OPTION.compile_first = true; continue; }
        if (match_long(arg, "--pg")
            || match_long(arg, "--pg-compile"))     { OPTION.pg_at_exit = true; continue; }
        if (match_long(arg, "--aot-compile"))       { OPTION.compile_only = true; continue; }
        if (match_long(arg, "--ccs")
            || match_long(arg, "--clear-code-store")) { OPTION.clear_store = true; continue; }

        // Short options.  Single char after `-`.
        if (arg[1] == '\0' || arg[2] != '\0') {
            fprintf(stderr, "unknown option: %s\n", arg);
            show_help();
            exit(1);
        }
        switch (arg[1]) {
          case 'h': show_help(); exit(0);
          case 's': OPTION.static_lang   = true; break;
          case 'i': OPTION.plain         = true; break;
          case 'c': OPTION.compile_first = true; break;
          case 'p': OPTION.pg_at_exit    = true; break;
          case 'b': OPTION.skip_bake     = true; break;
          case 'q': OPTION.quiet         = true; break;
          case 'j':
            OPTION.jit = true;
            astro_jit_start("/tmp/astrojit_l1.sock");
            break;
          default:
            fprintf(stderr, "unknown option: %s\n", arg);
            show_help();
            exit(1);
        }
    }

    if (fname == NULL) {
        fprintf(stderr, "no input\n");
        exit(1);
    }

    return fname;
}

static const char *
read_file_all(const char *filename)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen");
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(fp);
        return NULL;
    }

    long size = ftell(fp);
    if (size < 0) {
        perror("ftell");
        fclose(fp);
        return NULL;
    }

    rewind(fp);

    char *buffer = malloc(size + 1);
    if (!buffer) {
        perror("malloc");
        fclose(fp);
        return NULL;
    }

    size_t read_size = fread(buffer, 1, size, fp);
    if (read_size != (size_t)size) {
        perror("fread");
        free(buffer);
        fclose(fp);
        return NULL;
    }

    buffer[size] = '\0';

    fclose(fp);

    return buffer;
}

// Latest source file parsed by PARSE().  Used by build_code_store as
// the `file` argument for top-level astro_cs_compile so the program AST
// gets a stable PGC identity (PGSD_<HOPT>) keyed off the script path.
const char *naruby_current_source_file = NULL;

NODE *
PARSE(int argc, char *argv[])
{
    const char *fname = parse_option(argc, argv);
    naruby_current_source_file = fname;
    const char *src = read_file_all(fname);
    pm_parser_t parser;
    pm_options_t options = {0};

    pm_parser_init(&parser, (const uint8_t *)src, strlen(src), &options);
    pm_node_t *root = pm_parse(&parser);
    NODE *ast;

    if (root != NULL) {
        struct transduce_context tc = {
            .frame = NULL,
            .parser = &parser,
            // .verbose = true,
        };
        ast = transduce(&tc, root, 0);
        if (ast == NULL) {
            ast = ALLOC_node_num(0);
        }
    } else {
        fprintf(stderr, "Parse failed.\n");
        exit(1);
    }

    pm_parser_free(&parser);
    callsite_resolve();
    return ast;
}


static const char *
pm_node_type_name(pm_node_type_t type)
{
    switch (type) {
      case PM_ALIAS_GLOBAL_VARIABLE_NODE: return "AliasGlobalVariableNode";
      case PM_ALIAS_METHOD_NODE: return "AliasMethodNode";
      case PM_ALTERNATION_PATTERN_NODE: return "AlternationPatternNode";
      case PM_AND_NODE: return "AndNode";
      case PM_ARGUMENTS_NODE: return "ArgumentsNode";
      case PM_ARRAY_NODE: return "ArrayNode";
      case PM_ARRAY_PATTERN_NODE: return "ArrayPatternNode";
      case PM_ASSOC_NODE: return "AssocNode";
      case PM_ASSOC_SPLAT_NODE: return "AssocSplatNode";
      case PM_BACK_REFERENCE_READ_NODE: return "BackReferenceReadNode";
      case PM_BEGIN_NODE: return "BeginNode";
      case PM_BLOCK_ARGUMENT_NODE: return "BlockArgumentNode";
      case PM_BLOCK_LOCAL_VARIABLE_NODE: return "BlockLocalVariableNode";
      case PM_BLOCK_NODE: return "BlockNode";
      case PM_BLOCK_PARAMETER_NODE: return "BlockParameterNode";
      case PM_BLOCK_PARAMETERS_NODE: return "BlockParametersNode";
      case PM_BREAK_NODE: return "BreakNode";
      case PM_CALL_AND_WRITE_NODE: return "CallAndWriteNode";
      case PM_CALL_NODE: return "CallNode";
      case PM_CALL_OPERATOR_WRITE_NODE: return "CallOperatorWriteNode";
      case PM_CALL_OR_WRITE_NODE: return "CallOrWriteNode";
      case PM_CALL_TARGET_NODE: return "CallTargetNode";
      case PM_CAPTURE_PATTERN_NODE: return "CapturePatternNode";
      case PM_CASE_MATCH_NODE: return "CaseMatchNode";
      case PM_CASE_NODE: return "CaseNode";
      case PM_CLASS_NODE: return "ClassNode";
      case PM_CLASS_VARIABLE_AND_WRITE_NODE: return "ClassVariableAndWriteNode";
      case PM_CLASS_VARIABLE_OPERATOR_WRITE_NODE: return "ClassVariableOperatorWriteNode";
      case PM_CLASS_VARIABLE_OR_WRITE_NODE: return "ClassVariableOrWriteNode";
      case PM_CLASS_VARIABLE_READ_NODE: return "ClassVariableReadNode";
      case PM_CLASS_VARIABLE_TARGET_NODE: return "ClassVariableTargetNode";
      case PM_CLASS_VARIABLE_WRITE_NODE: return "ClassVariableWriteNode";
      case PM_CONSTANT_AND_WRITE_NODE: return "ConstantAndWriteNode";
      case PM_CONSTANT_OPERATOR_WRITE_NODE: return "ConstantOperatorWriteNode";
      case PM_CONSTANT_OR_WRITE_NODE: return "ConstantOrWriteNode";
      case PM_CONSTANT_PATH_AND_WRITE_NODE: return "ConstantPathAndWriteNode";
      case PM_CONSTANT_PATH_NODE: return "ConstantPathNode";
      case PM_CONSTANT_PATH_OPERATOR_WRITE_NODE: return "ConstantPathOperatorWriteNode";
      case PM_CONSTANT_PATH_OR_WRITE_NODE: return "ConstantPathOrWriteNode";
      case PM_CONSTANT_PATH_TARGET_NODE: return "ConstantPathTargetNode";
      case PM_CONSTANT_PATH_WRITE_NODE: return "ConstantPathWriteNode";
      case PM_CONSTANT_READ_NODE: return "ConstantReadNode";
      case PM_CONSTANT_TARGET_NODE: return "ConstantTargetNode";
      case PM_CONSTANT_WRITE_NODE: return "ConstantWriteNode";
      case PM_DEF_NODE: return "DefNode";
      case PM_DEFINED_NODE: return "DefinedNode";
      case PM_ELSE_NODE: return "ElseNode";
      case PM_EMBEDDED_STATEMENTS_NODE: return "EmbeddedStatementsNode";
      case PM_EMBEDDED_VARIABLE_NODE: return "EmbeddedVariableNode";
      case PM_ENSURE_NODE: return "EnsureNode";
      case PM_FALSE_NODE: return "FalseNode";
      case PM_FIND_PATTERN_NODE: return "FindPatternNode";
      case PM_FLIP_FLOP_NODE: return "FlipFlopNode";
      case PM_FLOAT_NODE: return "FloatNode";
      case PM_FOR_NODE: return "ForNode";
      case PM_FORWARDING_ARGUMENTS_NODE: return "ForwardingArgumentsNode";
      case PM_FORWARDING_PARAMETER_NODE: return "ForwardingParameterNode";
      case PM_FORWARDING_SUPER_NODE: return "ForwardingSuperNode";
      case PM_GLOBAL_VARIABLE_AND_WRITE_NODE: return "GlobalVariableAndWriteNode";
      case PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE: return "GlobalVariableOperatorWriteNode";
      case PM_GLOBAL_VARIABLE_OR_WRITE_NODE: return "GlobalVariableOrWriteNode";
      case PM_GLOBAL_VARIABLE_READ_NODE: return "GlobalVariableReadNode";
      case PM_GLOBAL_VARIABLE_TARGET_NODE: return "GlobalVariableTargetNode";
      case PM_GLOBAL_VARIABLE_WRITE_NODE: return "GlobalVariableWriteNode";
      case PM_HASH_NODE: return "HashNode";
      case PM_HASH_PATTERN_NODE: return "HashPatternNode";
      case PM_IF_NODE: return "IfNode";
      case PM_IMAGINARY_NODE: return "ImaginaryNode";
      case PM_IMPLICIT_NODE: return "ImplicitNode";
      case PM_IMPLICIT_REST_NODE: return "ImplicitRestNode";
      case PM_IN_NODE: return "InNode";
      case PM_INDEX_AND_WRITE_NODE: return "IndexAndWriteNode";
      case PM_INDEX_OPERATOR_WRITE_NODE: return "IndexOperatorWriteNode";
      case PM_INDEX_OR_WRITE_NODE: return "IndexOrWriteNode";
      case PM_INDEX_TARGET_NODE: return "IndexTargetNode";
      case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: return "InstanceVariableAndWriteNode";
      case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: return "InstanceVariableOperatorWriteNode";
      case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: return "InstanceVariableOrWriteNode";
      case PM_INSTANCE_VARIABLE_READ_NODE: return "InstanceVariableReadNode";
      case PM_INSTANCE_VARIABLE_TARGET_NODE: return "InstanceVariableTargetNode";
      case PM_INSTANCE_VARIABLE_WRITE_NODE: return "InstanceVariableWriteNode";
      case PM_INTEGER_NODE: return "IntegerNode";
      case PM_INTERPOLATED_MATCH_LAST_LINE_NODE: return "InterpolatedMatchLastLineNode";
      case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE: return "InterpolatedRegularExpressionNode";
      case PM_INTERPOLATED_STRING_NODE: return "InterpolatedStringNode";
      case PM_INTERPOLATED_SYMBOL_NODE: return "InterpolatedSymbolNode";
      case PM_INTERPOLATED_X_STRING_NODE: return "InterpolatedXStringNode";
      case PM_IT_LOCAL_VARIABLE_READ_NODE: return "ItLocalVariableReadNode";
      case PM_IT_PARAMETERS_NODE: return "ItParametersNode";
      case PM_KEYWORD_HASH_NODE: return "KeywordHashNode";
      case PM_KEYWORD_REST_PARAMETER_NODE: return "KeywordRestParameterNode";
      case PM_LAMBDA_NODE: return "LambdaNode";
      case PM_LOCAL_VARIABLE_AND_WRITE_NODE: return "LocalVariableAndWriteNode";
      case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: return "LocalVariableOperatorWriteNode";
      case PM_LOCAL_VARIABLE_OR_WRITE_NODE: return "LocalVariableOrWriteNode";
      case PM_LOCAL_VARIABLE_READ_NODE: return "LocalVariableReadNode";
      case PM_LOCAL_VARIABLE_TARGET_NODE: return "LocalVariableTargetNode";
      case PM_LOCAL_VARIABLE_WRITE_NODE: return "LocalVariableWriteNode";
      case PM_MATCH_LAST_LINE_NODE: return "MatchLastLineNode";
      case PM_MATCH_PREDICATE_NODE: return "MatchPredicateNode";
      case PM_MATCH_REQUIRED_NODE: return "MatchRequiredNode";
      case PM_MATCH_WRITE_NODE: return "MatchWriteNode";
      case PM_MISSING_NODE: return "MissingNode";
      case PM_MODULE_NODE: return "ModuleNode";
      case PM_MULTI_TARGET_NODE: return "MultiTargetNode";
      case PM_MULTI_WRITE_NODE: return "MultiWriteNode";
      case PM_NEXT_NODE: return "NextNode";
      case PM_NIL_NODE: return "NilNode";
      case PM_NO_KEYWORDS_PARAMETER_NODE: return "NoKeywordsParameterNode";
      case PM_NUMBERED_PARAMETERS_NODE: return "NumberedParametersNode";
      case PM_NUMBERED_REFERENCE_READ_NODE: return "NumberedReferenceReadNode";
      case PM_OPTIONAL_KEYWORD_PARAMETER_NODE: return "OptionalKeywordParameterNode";
      case PM_OPTIONAL_PARAMETER_NODE: return "OptionalParameterNode";
      case PM_OR_NODE: return "OrNode";
      case PM_PARAMETERS_NODE: return "ParametersNode";
      case PM_PARENTHESES_NODE: return "ParenthesesNode";
      case PM_PINNED_EXPRESSION_NODE: return "PinnedExpressionNode";
      case PM_PINNED_VARIABLE_NODE: return "PinnedVariableNode";
      case PM_POST_EXECUTION_NODE: return "PostExecutionNode";
      case PM_PRE_EXECUTION_NODE: return "PreExecutionNode";
      case PM_PROGRAM_NODE: return "ProgramNode";
      case PM_RANGE_NODE: return "RangeNode";
      case PM_RATIONAL_NODE: return "RationalNode";
      case PM_REDO_NODE: return "RedoNode";
      case PM_REGULAR_EXPRESSION_NODE: return "RegularExpressionNode";
      case PM_REQUIRED_KEYWORD_PARAMETER_NODE: return "RequiredKeywordParameterNode";
      case PM_REQUIRED_PARAMETER_NODE: return "RequiredParameterNode";
      case PM_RESCUE_MODIFIER_NODE: return "RescueModifierNode";
      case PM_RESCUE_NODE: return "RescueNode";
      case PM_REST_PARAMETER_NODE: return "RestParameterNode";
      case PM_RETRY_NODE: return "RetryNode";
      case PM_RETURN_NODE: return "ReturnNode";
      case PM_SELF_NODE: return "SelfNode";
      case PM_SHAREABLE_CONSTANT_NODE: return "ShareableConstantNode";
      case PM_SINGLETON_CLASS_NODE: return "SingletonClassNode";
      case PM_SOURCE_ENCODING_NODE: return "SourceEncodingNode";
      case PM_SOURCE_FILE_NODE: return "SourceFileNode";
      case PM_SOURCE_LINE_NODE: return "SourceLineNode";
      case PM_SPLAT_NODE: return "SplatNode";
      case PM_STATEMENTS_NODE: return "StatementsNode";
      case PM_STRING_NODE: return "StringNode";
      case PM_SUPER_NODE: return "SuperNode";
      case PM_SYMBOL_NODE: return "SymbolNode";
      case PM_TRUE_NODE: return "TrueNode";
      case PM_UNDEF_NODE: return "UndefNode";
      case PM_UNLESS_NODE: return "UnlessNode";
      case PM_UNTIL_NODE: return "UntilNode";
      case PM_WHEN_NODE: return "WhenNode";
      case PM_WHILE_NODE: return "WhileNode";
      case PM_X_STRING_NODE: return "XStringNode";
      case PM_YIELD_NODE: return "YieldNode";
      case PM_SCOPE_NODE: return "ScopeNode";
      default: return "Unknown";
    }
}
