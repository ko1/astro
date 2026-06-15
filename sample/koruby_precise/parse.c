/* koruby_precise v2 — parse.c: prism AST → koruby NODE transduction (M0).
 *
 * Local variables use the one-stack model (v2_design §7.8): each lvar
 * access bakes a negative cursor offset
 *
 *     off = index - locals_cnt - chain
 *
 * where `chain` is the staging depth at that program point (the sum of
 * slot_counts of the enclosing dispatchers within the current frame).
 * `chain` is known during transduction; `locals_cnt` only at scope end, so
 * nodes bake (index - chain) and the frame-pop fixup subtracts locals_cnt.
 *
 * Cached structural hashes stay correct because nothing hashes a NODE
 * before its frame is popped: main.c calls INIT() (which dlopens the code
 * store) only after PARSE, so the OPTIMIZE call inside every ALLOC is a
 * no-op during parsing.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "prism.h"
#include "node.h"

uint32_t koruby_toplevel_locals_cnt = 0;

struct kp_frame {
    const pm_constant_id_list_t *locals;
    uint32_t bake_base;
    int32_t saved_chain;
    bool uses_block;          /* yield / block_given? seen → reserve 2 frame-top cells */
    struct kp_frame *prev;
};

struct kp_ctx {
    pm_parser_t *parser;
    CTX *c;
    const char *fname;
    struct kp_frame *frame;
    int32_t chain;            /* staging depth at the current program point */
    int32_t **bake_list;      /* lvar-offset cells awaiting locals_cnt fixup */
    uint32_t bake_cnt, bake_capa;
};

/* Evaluate BODY (allocations / transduction of the children of a node
 * whose dispatcher claims `n_slots` staging slots).  The dispatcher
 * advances the cursor by slot_count before evaluating ANY operand, so all
 * child subtrees see chain + n_slots. */
#define WITH_CHAIN(tc, n_slots, BODY) ({ \
    int32_t _saved = (tc)->chain;        \
    (tc)->chain = _saved + (int32_t)(n_slots); \
    __typeof__(BODY) _r = (BODY);        \
    (tc)->chain = _saved;                \
    _r; \
})

static NODE *transduce(struct kp_ctx *tc, const pm_node_t *node);

/* ---------------------------------------------------------------------- */

static uint32_t
kp_line(struct kp_ctx *tc, const pm_node_t *node)
{
    int32_t line = pm_newline_list_line(&tc->parser->newline_list,
                                        node->location.start,
                                        tc->parser->start_line);
    return line < 0 ? 0 : (uint32_t)line;
}

static __attribute__((noreturn)) void
kp_failf(struct kp_ctx *tc, const pm_node_t *node, const char *fmt, ...)
{
    fprintf(stderr, "%s:%u: ", tc->fname, node ? kp_line(tc, node) : 0);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* Subset boundary: emit a node that raises NotImplementedError when (if)
 * this program point is reached.  Everything else in the file keeps
 * working — rubyharness scores per line, so a parse-time exit here would
 * zero whole corpus files over one exotic construct. */
static NODE *
kp_unsupported(struct kp_ctx *tc, const pm_node_t *node, const char *what)
{
    (void)tc;
    return ALLOC_node_unsupported(what, node ? kp_line(tc, node) : 0);
}

/* constant_id → interned (cstr, len) via the parser's constant pool */
static uint32_t
kp_intern_cid(struct kp_ctx *tc, pm_constant_id_t cid)
{
    pm_constant_t *ct = pm_constant_pool_id_to_constant(&tc->parser->constant_pool, cid);
    return korb_intern(tc->c->vm, (const char *)ct->start, ct->length);
}

static const char *
kp_cid_cstr(struct kp_ctx *tc, pm_constant_id_t cid)
{
    return korb_sym_name(tc->c->vm, kp_intern_cid(tc, cid));
}

/* ---- frames + lvar offset bake ---------------------------------------- */

static void
push_frame(struct kp_ctx *tc, const pm_constant_id_list_t *locals)
{
    struct kp_frame *f = malloc(sizeof(*f));
    if (!f) abort();
    f->locals = locals;
    f->bake_base = tc->bake_cnt;
    f->saved_chain = tc->chain;
    f->uses_block = false;
    f->prev = tc->frame;
    tc->frame = f;
    tc->chain = 0;
}

/* Returns the frame size.  Every frame reserves a self cell on top (base[fs-1]);
 * a yielding frame also reserves the 3-cell block group {block_entry, def_env,
 * captured_self} just below it.  Layout top-down:
 *   [locals... | block_entry(fs-4) | def_env(fs-3) | captured_self(fs-2) | self(fs-1)] */
static uint32_t
pop_frame(struct kp_ctx *tc)
{
    struct kp_frame *f = tc->frame;
    uint32_t frame_size = (uint32_t)f->locals->size + 1u + (f->uses_block ? 3u : 0u);
    for (uint32_t i = f->bake_base; i < tc->bake_cnt; i++) {
        *tc->bake_list[i] -= (int32_t)frame_size;
    }
    tc->bake_cnt = f->bake_base;
    tc->chain = f->saved_chain;
    tc->frame = f->prev;
    free(f);
    return frame_size;
}

static void
bake_add(struct kp_ctx *tc, int32_t *cell)
{
    if (tc->bake_cnt == tc->bake_capa) {
        tc->bake_capa = tc->bake_capa ? tc->bake_capa * 2 : 1024;
        tc->bake_list = realloc(tc->bake_list, sizeof(int32_t *) * tc->bake_capa);
        if (!tc->bake_list) abort();
    }
    tc->bake_list[tc->bake_cnt++] = cell;
}

static uint32_t
lvar_index(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid)
{
    const pm_constant_id_list_t *list = tc->frame->locals;
    for (size_t i = 0; i < list->size; i++) {
        if (list->ids[i] == cid) return (uint32_t)i;
    }
    kp_failf(tc, node, "koruby_precise: local '%s' not in scope table", kp_cid_cstr(tc, cid));
}

/* Index of an outer variable `depth` enclosing scopes out (prism depth). */
static uint32_t
lvar_index_at(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid, uint32_t depth)
{
    struct kp_frame *f = tc->frame;
    for (uint32_t d = 0; d < depth; d++) {
        f = f ? f->prev : NULL;
    }
    if (!f) kp_failf(tc, node, "koruby_precise: outer scope depth %u not found", depth);
    for (size_t i = 0; i < f->locals->size; i++) {
        if (f->locals->ids[i] == cid) return (uint32_t)i;
    }
    kp_failf(tc, node, "koruby_precise: outer local '%s' not at depth %u", kp_cid_cstr(tc, cid), depth);
}

static NODE *
bake_lget(struct kp_ctx *tc, uint32_t index)
{
    NODE *n = ALLOC_node_lget((int32_t)index - tc->chain);
    bake_add(tc, &n->u.node_lget.off);
    return n;
}

static NODE *
bake_lset(struct kp_ctx *tc, uint32_t index, NODE *rval)
{
    NODE *n = ALLOC_node_lset((int32_t)index - tc->chain, rval);
    bake_add(tc, &n->u.node_lset.off);
    return n;
}

/* Outer-variable get/set (depth >= 1).  prev_off addresses the current frame's
 * PREV cell (bf[0] = base[-1]): baked -1 - chain, pop subtracts frame_size →
 * -(frame_size+1) - chain.  depth/index are constants (no fixup). */
static NODE *
bake_eget(struct kp_ctx *tc, uint32_t depth, uint32_t index)
{
    NODE *n = ALLOC_node_eget(-1 - tc->chain, depth, index);
    bake_add(tc, &n->u.node_eget.prev_off);
    return n;
}

static NODE *
bake_eset(struct kp_ctx *tc, uint32_t depth, uint32_t index, NODE *rval)
{
    NODE *n = ALLOC_node_eset(-1 - tc->chain, depth, index, rval);
    bake_add(tc, &n->u.node_eset.prev_off);
    return n;
}

/* depth==0 → local, depth>=1 → outer.  Centralizes the read/write dispatch. */
static NODE *
lvar_read(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid, uint32_t depth)
{
    if (depth == 0) return bake_lget(tc, lvar_index(tc, node, cid));
    return bake_eget(tc, depth, lvar_index_at(tc, node, cid, depth));
}

static NODE *
lvar_write(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid, uint32_t depth, NODE *rval)
{
    if (depth == 0) return bake_lset(tc, lvar_index(tc, node, cid), rval);
    return bake_eset(tc, depth, lvar_index_at(tc, node, cid, depth), rval);
}

/* ---- helpers ----------------------------------------------------------- */

static NODE *
lit_nil(void)
{
    return ALLOC_node_lit(KORB_NIL);
}

static NODE *
transduce_statements(struct kp_ctx *tc, const pm_statements_node_t *stmts)
{
    if (stmts == NULL || stmts->body.size == 0) return lit_nil();
    NODE *acc = NULL;
    for (size_t i = 0; i < stmts->body.size; i++) {
        NODE *one = transduce(tc, stmts->body.nodes[i]);
        acc = acc ? ALLOC_node_seq(acc, one) : one;
    }
    return acc;
}

static NODE *
transduce_opt(struct kp_ctx *tc, const pm_node_t *node)
{
    return node ? transduce(tc, node) : lit_nil();
}

static bool
kp_integer_value(const pm_integer_t *integer, intptr_t *out)
{
    uint64_t mag;
    if (integer->values == NULL) {
        mag = integer->value;
    }
    else if (integer->length <= 2) {
        mag = (uint64_t)integer->values[0];
        if (integer->length == 2) mag |= (uint64_t)integer->values[1] << 32;
    }
    else {
        return false;
    }
    if (integer->negative) {
        if (mag > (uint64_t)FIXNUM_MAX + 1) return false;
        *out = -(intptr_t)mag;
        return true;
    }
    if (mag > (uint64_t)FIXNUM_MAX) return false;
    *out = (intptr_t)mag;
    return true;
}

/* malloc-backed copy of a pm_string (NODE operands are immortal) */
static const char *
kp_strdup_pm(const pm_string_t *s, uint32_t *len_out)
{
    size_t len = pm_string_length(s);
    char *buf = malloc(len + 1);
    if (!buf) abort();
    memcpy(buf, pm_string_source(s), len);
    buf[len] = '\0';
    *len_out = (uint32_t)len;
    return buf;
}

/* ---- operators --------------------------------------------------------- */

extern const struct NodeKind kind_node_plus;         /* all binops share slot_count */
extern const struct NodeKind kind_node_ary_push;     /* array-literal push chain */
extern const struct NodeKind kind_node_dstr_concat;  /* string-interp concat chain */
extern const struct NodeKind kind_node_hash_set;     /* hash-literal set chain */
extern const struct NodeKind kind_node_range_new;    /* range literal */

enum kp_binop {
    KP_BINOP_NONE = 0,
    KP_PLUS, KP_MINUS, KP_MUL, KP_DIV, KP_MOD,
    KP_LT, KP_LE, KP_GT, KP_GE, KP_EQ, KP_NEQ,
};

static enum kp_binop
kp_binop_kind(const char *name)
{
    if (strcmp(name, "+") == 0)  return KP_PLUS;
    if (strcmp(name, "-") == 0)  return KP_MINUS;
    if (strcmp(name, "*") == 0)  return KP_MUL;
    if (strcmp(name, "/") == 0)  return KP_DIV;
    if (strcmp(name, "%") == 0)  return KP_MOD;
    if (strcmp(name, "<") == 0)  return KP_LT;
    if (strcmp(name, "<=") == 0) return KP_LE;
    if (strcmp(name, ">") == 0)  return KP_GT;
    if (strcmp(name, ">=") == 0) return KP_GE;
    if (strcmp(name, "==") == 0) return KP_EQ;
    if (strcmp(name, "!=") == 0) return KP_NEQ;
    return KP_BINOP_NONE;
}

static NODE *
alloc_binop(enum kp_binop op, NODE *lhs, NODE *rhs, uint32_t line)
{
    switch (op) {
      case KP_PLUS:  return ALLOC_node_plus(lhs, rhs, line);
      case KP_MINUS: return ALLOC_node_minus(lhs, rhs, line);
      case KP_MUL:   return ALLOC_node_mul(lhs, rhs, line);
      case KP_DIV:   return ALLOC_node_div(lhs, rhs, line);
      case KP_MOD:   return ALLOC_node_mod(lhs, rhs, line);
      case KP_LT:    return ALLOC_node_lt(lhs, rhs, line);
      case KP_LE:    return ALLOC_node_le(lhs, rhs, line);
      case KP_GT:    return ALLOC_node_gt(lhs, rhs, line);
      case KP_GE:    return ALLOC_node_ge(lhs, rhs, line);
      case KP_EQ:    return ALLOC_node_eq(lhs, rhs);
      case KP_NEQ:   return ALLOC_node_neq(lhs, rhs);
      default:       abort();
    }
}

/* ---- calls -------------------------------------------------------------- */

/* Parse a block literal into a node_entry (its own scope; registered as an
 * AOT entry like a method body).  docs/v2_blocks_design.md. */
static NODE *
transduce_block(struct kp_ctx *tc, const pm_block_node_t *blk)
{
    push_frame(tc, &blk->locals);

    uint32_t bparams = 0;
    if (blk->parameters) {
        if (!PM_NODE_TYPE_P(blk->parameters, PM_BLOCK_PARAMETERS_NODE)) {
            pop_frame(tc);
            return kp_unsupported(tc, blk->parameters, "numbered/it block parameters");
        }
        const pm_block_parameters_node_t *bp =
            (const pm_block_parameters_node_t *)blk->parameters;
        const pm_parameters_node_t *ps = bp->parameters;
        if (bp->locals.size) {
            pop_frame(tc);
            return kp_unsupported(tc, blk->parameters, "block-local variables (|x; y|)");
        }
        if (ps) {
            if (ps->optionals.size || ps->rest || ps->posts.size ||
                ps->keywords.size || ps->keyword_rest || ps->block) {
                pop_frame(tc);
                return kp_unsupported(tc, (const pm_node_t *)ps,
                                      "non-positional block parameters");
            }
            bparams = (uint32_t)ps->requireds.size;
            for (uint32_t i = 0; i < bparams; i++) {
                const pm_node_t *p = ps->requireds.nodes[i];
                if (!PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) {
                    pop_frame(tc);
                    return kp_unsupported(tc, p, "destructuring block parameter");
                }
                pm_constant_id_t cid = ((const pm_required_parameter_node_t *)p)->name;
                if (lvar_index(tc, p, cid) != i) {
                    kp_failf(tc, p, "koruby_precise: block param '%s' is not locals[%u]",
                             kp_cid_cstr(tc, cid), i);
                }
            }
        }
    }

    NODE *body;
    if (blk->body == NULL) {
        body = lit_nil();
    }
    else if (PM_NODE_TYPE_P(blk->body, PM_STATEMENTS_NODE)) {
        body = transduce_statements(tc, (const pm_statements_node_t *)blk->body);
    }
    else {
        body = kp_unsupported(tc, blk->body, "block body with rescue/ensure");
    }

    uint32_t frame_size = pop_frame(tc);    /* block locals (+2 if the block yields) */
    NODE *entry = ALLOC_node_entry(body, bparams, frame_size);
    /* node_entry is the dispatch root (yield → entry->head.dispatcher); its own
     * AOT entry, body inlined into its SD. */
    code_repo_add("block", entry, true);
    return entry;
}

/* Call with a literal block.  Bakes def_env_off (caller frame base) and
 * hands the node_entry + def_env to the callee.  B2: 0 or 1 positional arg. */
static NODE *
transduce_call_with_block(struct kp_ctx *tc, const pm_call_node_t *cn, uint32_t mid,
                          uint32_t line, const pm_arguments_node_t *args, size_t argc,
                          const pm_block_node_t *blk)
{
    if (argc > 1) return kp_unsupported(tc, (const pm_node_t *)cn, "call with block and >1 arg");

    NODE *entry = transduce_block(tc, blk);

    /* def_env_off: cursor → caller frame base = -(chain + slot_count); pop
     * subtracts the caller's frame_size (the slot_count = argc). */
    int32_t self_off = -1 - tc->chain - (int32_t)argc;  /* caller self = block's captured self */
    if (argc == 0) {
        NODE *call = ALLOC_node_call_blk0(mid, line, self_off, entry, -(tc->chain + 0));
        bake_add(tc, &call->u.node_call_blk0.def_env_off);
        return call;
    }
    NODE *a0;
    WITH_CHAIN(tc, 1, (a0 = transduce(tc, args->arguments.nodes[0])));
    NODE *call = ALLOC_node_call_blk1(mid, line, self_off, entry, -(tc->chain + 1), a0);
    bake_add(tc, &call->u.node_call_blk1.def_env_off);
    return call;
}

static NODE *
transduce_func_call(struct kp_ctx *tc, const pm_call_node_t *cn)
{
    uint32_t mid = kp_intern_cid(tc, cn->name);
    uint32_t line = kp_line(tc, (const pm_node_t *)cn);
    const pm_arguments_node_t *args = cn->arguments;
    size_t argc = args ? args->arguments.size : 0;

    /* block_given? — reads the current method's frame-top biseq cell. */
    if (argc == 0 && cn->block == NULL &&
        strcmp(kp_cid_cstr(tc, cn->name), "block_given?") == 0) {
        tc->frame->uses_block = true;
        return ALLOC_node_block_given(-4 - tc->chain);   /* block_entry cell (fs-4) */
    }

    /* attr_reader/writer/accessor :sym... → node_attr (defines getters/setters
     * on self = the enclosing class). */
    if (cn->block == NULL && argc > 0) {
        const char *nm = kp_cid_cstr(tc, cn->name);
        int mode = !strcmp(nm, "attr_reader") ? 0 : !strcmp(nm, "attr_writer") ? 1
                 : !strcmp(nm, "attr_accessor") ? 2 : -1;
        if (mode >= 0) {
            bool all_syms = true;
            for (size_t i = 0; i < argc; i++)
                if (!PM_NODE_TYPE_P(args->arguments.nodes[i], PM_SYMBOL_NODE)) { all_syms = false; break; }
            if (all_syms) {
                uint32_t count = (uint32_t)argc * (mode == 2 ? 2u : 1u);
                struct korb_attr_desc *descs = malloc(sizeof(*descs) * count);
                if (!descs) abort();
                uint32_t di = 0;
                for (size_t i = 0; i < argc; i++) {
                    const pm_symbol_node_t *sn = (const pm_symbol_node_t *)args->arguments.nodes[i];
                    const char *bn = (const char *)pm_string_source(&sn->unescaped);
                    size_t blen = pm_string_length(&sn->unescaped);
                    char buf[256];
                    if (blen + 2 >= sizeof(buf)) { free(descs); return kp_unsupported(tc, (const pm_node_t *)cn, "attr name too long"); }
                    buf[0] = '@'; memcpy(buf + 1, bn, blen);                 /* "@name" */
                    uint32_t ivar = korb_intern(tc->c->vm, buf, blen + 1);
                    uint32_t rmid = korb_intern(tc->c->vm, bn, blen);        /* "name" */
                    memcpy(buf, bn, blen); buf[blen] = '=';                  /* "name=" */
                    uint32_t wmid = korb_intern(tc->c->vm, buf, blen + 1);
                    if (mode != 1) { descs[di].mid = rmid; descs[di].ivar = ivar; descs[di].is_writer = 0; di++; }
                    if (mode != 0) { descs[di].mid = wmid; descs[di].ivar = ivar; descs[di].is_writer = 1; di++; }
                }
                return ALLOC_node_attr(-1 - tc->chain, descs, count);
            }
        }
    }

    if (cn->block) {
        if (!PM_NODE_TYPE_P(cn->block, PM_BLOCK_NODE)) {
            return kp_unsupported(tc, (const pm_node_t *)cn, "&block argument");
        }
        return transduce_call_with_block(tc, cn, mid, line, args, argc,
                                         (const pm_block_node_t *)cn->block);
    }

    /* caller self cell (base[fs-1]); the argc staged args advance the body
     * cursor, so offset back past them too. */
    int32_t self_off = -1 - tc->chain - (int32_t)argc;
    NODE *a[3];
    switch (argc) {
      case 0:
        return ALLOC_node_call0(mid, line, self_off);
      case 1:
        WITH_CHAIN(tc, 1, (a[0] = transduce(tc, args->arguments.nodes[0])));
        return ALLOC_node_call1(mid, line, self_off, a[0]);
      case 2:
        WITH_CHAIN(tc, 2, (a[0] = transduce(tc, args->arguments.nodes[0]),
                           a[1] = transduce(tc, args->arguments.nodes[1])));
        return ALLOC_node_call2(mid, line, self_off, a[0], a[1]);
      case 3:
        WITH_CHAIN(tc, 3, (a[0] = transduce(tc, args->arguments.nodes[0]),
                           a[1] = transduce(tc, args->arguments.nodes[1]),
                           a[2] = transduce(tc, args->arguments.nodes[2])));
        return ALLOC_node_call3(mid, line, self_off, a[0], a[1], a[2]);
      default:
        return kp_unsupported(tc, (const pm_node_t *)cn, "call with more than 3 arguments");
    }
}

static NODE *
transduce_call(struct kp_ctx *tc, const pm_call_node_t *cn)
{
    if (cn->receiver == NULL) {
        return transduce_func_call(tc, cn);
    }

    const char *name = kp_cid_cstr(tc, cn->name);
    uint32_t mid = kp_intern_cid(tc, cn->name);
    uint32_t line = kp_line(tc, (const pm_node_t *)cn);
    size_t argc = cn->arguments ? cn->arguments->arguments.size : 0;

    /* operator calls → dedicated binop / unary nodes */
    enum kp_binop op = kp_binop_kind(name);
    if (op != KP_BINOP_NONE && argc == 1) {
        uint32_t n_slots = kind_node_plus.slot_count;   /* lhs staging (all binops alike) */
        NODE *lhs, *rhs;
        WITH_CHAIN(tc, n_slots, (lhs = transduce(tc, cn->receiver),
                                 rhs = transduce(tc, cn->arguments->arguments.nodes[0])));
        return alloc_binop(op, lhs, rhs, line);
    }
    if (strcmp(name, "-@") == 0 && argc == 0) {
        return ALLOC_node_neg(transduce(tc, cn->receiver), line);
    }
    if (strcmp(name, "!") == 0 && argc == 0) {
        return ALLOC_node_not(transduce(tc, cn->receiver));
    }

    /* receiver method dispatch with a literal block: recv.mid(args) { ... } */
    if (cn->block) {
        if (!PM_NODE_TYPE_P(cn->block, PM_BLOCK_NODE))
            return kp_unsupported(tc, (const pm_node_t *)cn, "&block argument");
        if (argc > 1)
            return kp_unsupported(tc, (const pm_node_t *)cn, "receiver call with block and >1 arg");
        NODE *entry = transduce_block(tc, (const pm_block_node_t *)cn->block);
        /* def_env_off: cursor → caller frame base = -(chain + staging); staging
         * = recv(1) + argc.  bake_add fixes up by the caller's frame_size. */
        /* caller self; recv(1) + argc staged children advance the body cursor */
        int32_t self_off = -1 - tc->chain - (int32_t)(1 + argc);
        if (argc == 0) {
            uint32_t sc = 1;
            NODE *recv;
            WITH_CHAIN(tc, sc, (recv = transduce(tc, cn->receiver)));
            NODE *call = ALLOC_node_send_blk0(mid, line, self_off, entry, -(tc->chain + (int32_t)sc), recv);
            bake_add(tc, &call->u.node_send_blk0.def_env_off);
            return call;
        }
        uint32_t sc = 2;
        NODE *recv, *a0;
        WITH_CHAIN(tc, sc, (recv = transduce(tc, cn->receiver),
                            a0   = transduce(tc, cn->arguments->arguments.nodes[0])));
        NODE *call = ALLOC_node_send_blk1(mid, line, self_off, entry, -(tc->chain + (int32_t)sc), recv, a0);
        bake_add(tc, &call->u.node_send_blk1.def_env_off);
        return call;
    }
    if (argc > 3) {
        return kp_unsupported(tc, (const pm_node_t *)cn, "receiver method call with >3 args");
    }
    uint32_t sc = 1u + (uint32_t)argc;     /* recv + args staging */
    NODE *recv, *a[3];
    switch (argc) {
      case 0:
        WITH_CHAIN(tc, sc, (recv = transduce(tc, cn->receiver)));
        return ALLOC_node_send0(mid, line, recv);
      case 1:
        WITH_CHAIN(tc, sc, (recv = transduce(tc, cn->receiver),
                            a[0] = transduce(tc, cn->arguments->arguments.nodes[0])));
        return ALLOC_node_send1(mid, line, recv, a[0]);
      case 2:
        WITH_CHAIN(tc, sc, (recv = transduce(tc, cn->receiver),
                            a[0] = transduce(tc, cn->arguments->arguments.nodes[0]),
                            a[1] = transduce(tc, cn->arguments->arguments.nodes[1])));
        return ALLOC_node_send2(mid, line, recv, a[0], a[1]);
      default:
        WITH_CHAIN(tc, sc, (recv = transduce(tc, cn->receiver),
                            a[0] = transduce(tc, cn->arguments->arguments.nodes[0]),
                            a[1] = transduce(tc, cn->arguments->arguments.nodes[1]),
                            a[2] = transduce(tc, cn->arguments->arguments.nodes[2])));
        return ALLOC_node_send3(mid, line, recv, a[0], a[1], a[2]);
    }
}

/* ---- def ----------------------------------------------------------------- */

static NODE *
transduce_def(struct kp_ctx *tc, const pm_def_node_t *dn)
{
    if (dn->receiver) return kp_unsupported(tc, (const pm_node_t *)dn, "singleton method (def self.x)");

    uint32_t params_cnt = 0;
    if (dn->parameters) {
        const pm_parameters_node_t *ps = dn->parameters;
        if (ps->optionals.size || ps->rest || ps->posts.size ||
            ps->keywords.size || ps->keyword_rest || ps->block) {
            return kp_unsupported(tc, (const pm_node_t *)dn,
                                  "non-positional parameters (opt/rest/kw/block)");
        }
        params_cnt = (uint32_t)ps->requireds.size;
    }

    push_frame(tc, &dn->locals);

    /* prism orders def locals with the parameters first — the staged-args
     * window doubles as the parameter slots.  Verify the assumption. */
    if (dn->parameters) {
        for (uint32_t i = 0; i < params_cnt; i++) {
            const pm_node_t *p = dn->parameters->requireds.nodes[i];
            if (!PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) {
                pop_frame(tc);
                return kp_unsupported(tc, p, "non-plain required parameter");
            }
            pm_constant_id_t cid = ((const pm_required_parameter_node_t *)p)->name;
            if (lvar_index(tc, p, cid) != i) {
                kp_failf(tc, p, "koruby_precise: parameter '%s' is not locals[%u]",
                         kp_cid_cstr(tc, cid), i);
            }
        }
    }

    NODE *body;
    if (dn->body == NULL) {
        body = lit_nil();
    }
    else if (PM_NODE_TYPE_P(dn->body, PM_STATEMENTS_NODE)) {
        body = transduce_statements(tc, (const pm_statements_node_t *)dn->body);
    }
    else {
        body = kp_unsupported(tc, dn->body, "def body with rescue/ensure");
    }

    uint32_t uses_block = tc->frame->uses_block ? 1u : 0u;
    uint32_t frame_size = pop_frame(tc);   /* = locals + 2 if the method yields */

    uint32_t mid = kp_intern_cid(tc, dn->name);
    /* self at the def site (enclosing frame) = the default definee */
    NODE *def = ALLOC_node_def(mid, body, params_cnt, frame_size, uses_block, -1 - tc->chain);

    /* Every method body is its own AOT entry: call sites reach it through
     * body->head.dispatcher at runtime (specializer can't fold that). */
    code_repo_add(korb_sym_name(tc->c->vm, mid), body, true);
    return def;
}

/* `class Name ... end` → node_class carrying the class name + a node_entry for
 * the body (its own scope, run with self = the class). */
static NODE *
transduce_class(struct kp_ctx *tc, const pm_class_node_t *cn)
{
    if (cn->superclass)
        return kp_unsupported(tc, (const pm_node_t *)cn, "class inheritance (< superclass)");
    if (!PM_NODE_TYPE_P(cn->constant_path, PM_CONSTANT_READ_NODE))
        return kp_unsupported(tc, (const pm_node_t *)cn, "namespaced class name");
    uint32_t name_sym = kp_intern_cid(tc, cn->name);

    push_frame(tc, &cn->locals);
    NODE *body;
    if (cn->body == NULL)
        body = lit_nil();
    else if (PM_NODE_TYPE_P(cn->body, PM_STATEMENTS_NODE))
        body = transduce_statements(tc, (const pm_statements_node_t *)cn->body);
    else
        body = kp_unsupported(tc, cn->body, "class body with rescue/ensure");
    uint32_t frame_size = pop_frame(tc);

    NODE *entry = ALLOC_node_entry(body, 0, frame_size);
    code_repo_add("class", entry, true);          /* its own AOT entry */
    return ALLOC_node_class(name_sym, entry);
}

/* Array literal `[e0, e1, ...]` → inside-out push chain (variadic @child is
 * unsupported).  Element i nests i pushes deep, so it transduces at the chain
 * depth matching its runtime cursor offset. */
static NODE *
build_array(struct kp_ctx *tc, struct pm_node **elems, size_t n, uint32_t capa)
{
    if (n == 0) return ALLOC_node_array_new(capa);
    NODE *acc, *elem;
    uint32_t sc = kind_node_ary_push.slot_count;
    WITH_CHAIN(tc, sc, (acc  = build_array(tc, elems, n - 1, capa),
                        elem = transduce(tc, elems[n - 1])));
    return ALLOC_node_ary_push(acc, elem);
}

/* Hash literal `{k => v, ...}` → inside-out set chain (same shape as
 * build_array): hash_set(hash_set(hash_new(n), k0, v0), k1, v1)... */
static NODE *
build_hash(struct kp_ctx *tc, struct pm_node **assocs, size_t n, uint32_t capa)
{
    if (n == 0) return ALLOC_node_hash_new(capa);
    const pm_assoc_node_t *as = (const pm_assoc_node_t *)assocs[n - 1];
    NODE *acc, *key, *val;
    uint32_t sc = kind_node_hash_set.slot_count;
    WITH_CHAIN(tc, sc, (acc = build_hash(tc, assocs, n - 1, capa),
                        key = transduce(tc, as->key),
                        val = transduce(tc, as->value)));
    return ALLOC_node_hash_set(acc, key, val);
}

/* Interpolated string `"a#{x}b"` → inside-out concat chain (same shape as
 * build_array).  The accumulator is always a String; each part is appended
 * via its to_s inside node_dstr_concat. */
static NODE *
build_dstr(struct kp_ctx *tc, struct pm_node **parts, size_t n)
{
    if (n == 0) return ALLOC_node_str("", 0);
    NODE *acc, *part;
    uint32_t sc = kind_node_dstr_concat.slot_count;
    WITH_CHAIN(tc, sc, (acc  = build_dstr(tc, parts, n - 1),
                        part = transduce(tc, parts[n - 1])));
    return ALLOC_node_dstr_concat(acc, part);
}

/* ---- main dispatch -------------------------------------------------------- */

static NODE *
transduce(struct kp_ctx *tc, const pm_node_t *node)
{
    switch (PM_NODE_TYPE(node)) {
      case PM_PROGRAM_NODE: {
        const pm_program_node_t *pn = (const pm_program_node_t *)node;
        push_frame(tc, &pn->locals);
        NODE *body = transduce_statements(tc, pn->statements);
        koruby_toplevel_locals_cnt = pop_frame(tc);   /* frame_size for main's cursor */
        return body;
      }

      case PM_STATEMENTS_NODE:
        return transduce_statements(tc, (const pm_statements_node_t *)node);

      case PM_PARENTHESES_NODE: {
        const pm_parentheses_node_t *pn = (const pm_parentheses_node_t *)node;
        return transduce_opt(tc, pn->body);
      }

      case PM_BEGIN_NODE: {
        const pm_begin_node_t *bn = (const pm_begin_node_t *)node;
        if (bn->rescue_clause || bn->ensure_clause || bn->else_clause) {
            return kp_unsupported(tc, node, "begin/rescue/ensure");
        }
        return transduce_statements(tc, bn->statements);
      }

      /* ---- literals ---- */
      case PM_INTEGER_NODE: {
        const pm_integer_node_t *in = (const pm_integer_node_t *)node;
        intptr_t v;
        if (!kp_integer_value(&in->value, &v))
            return kp_unsupported(tc, node, "Integer literal beyond Fixnum range (Bignum)");
        return ALLOC_node_lit(LONG2FIX(v));
      }
      case PM_STRING_NODE: {
        const pm_string_node_t *sn = (const pm_string_node_t *)node;
        uint32_t len;
        const char *bytes = kp_strdup_pm(&sn->unescaped, &len);
        return ALLOC_node_str(bytes, len);
      }
      case PM_SYMBOL_NODE: {
        const pm_symbol_node_t *sn = (const pm_symbol_node_t *)node;
        size_t len = pm_string_length(&sn->unescaped);
        uint32_t id = korb_intern(tc->c->vm, (const char *)pm_string_source(&sn->unescaped), len);
        return ALLOC_node_lit(ID2SYM(id));
      }
      case PM_NIL_NODE:   return ALLOC_node_lit(KORB_NIL);
      case PM_TRUE_NODE:  return ALLOC_node_lit(KORB_TRUE);
      case PM_FALSE_NODE: return ALLOC_node_lit(KORB_FALSE);

      /* ---- self / instance variables (self cell at base[fs-1], -1-chain) ---- */
      case PM_SELF_NODE:
        return ALLOC_node_self(-1 - tc->chain);
      case PM_INSTANCE_VARIABLE_READ_NODE: {
        const pm_instance_variable_read_node_t *iv = (const pm_instance_variable_read_node_t *)node;
        return ALLOC_node_ivar_get(-1 - tc->chain, kp_intern_cid(tc, iv->name));
      }
      case PM_INSTANCE_VARIABLE_WRITE_NODE: {
        const pm_instance_variable_write_node_t *iw = (const pm_instance_variable_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, iw->name);
        NODE *val = transduce(tc, iw->value);    /* register child, current chain */
        return ALLOC_node_ivar_set(-1 - tc->chain, name, val);
      }

      case PM_ARRAY_NODE: {
        const pm_array_node_t *an = (const pm_array_node_t *)node;
        size_t cnt = an->elements.size;
        for (size_t i = 0; i < cnt; i++)             /* splat lands later */
            if (PM_NODE_TYPE(an->elements.nodes[i]) == PM_SPLAT_NODE)
                return kp_unsupported(tc, node, "array literal with splat (*)");
        return build_array(tc, an->elements.nodes, cnt, (uint32_t)cnt);
      }

      case PM_RANGE_NODE: {
        const pm_range_node_t *rn = (const pm_range_node_t *)node;
        if (!rn->left || !rn->right)
            return kp_unsupported(tc, node, "beginless/endless range");
        uint32_t excl = (rn->base.flags & PM_RANGE_FLAGS_EXCLUDE_END) ? 1u : 0u;
        NODE *b, *e;
        uint32_t sc = kind_node_range_new.slot_count;
        WITH_CHAIN(tc, sc, (b = transduce(tc, rn->left),
                            e = transduce(tc, rn->right)));
        return ALLOC_node_range_new(excl, b, e);
      }

      case PM_HASH_NODE: {
        const pm_hash_node_t *hn = (const pm_hash_node_t *)node;
        size_t cnt = hn->elements.size;
        for (size_t i = 0; i < cnt; i++)
            if (!PM_NODE_TYPE_P(hn->elements.nodes[i], PM_ASSOC_NODE))
                return kp_unsupported(tc, node, "hash literal with ** splat");
        return build_hash(tc, hn->elements.nodes, cnt, (uint32_t)cnt);
      }

      case PM_INTERPOLATED_STRING_NODE: {
        const pm_interpolated_string_node_t *in = (const pm_interpolated_string_node_t *)node;
        return build_dstr(tc, in->parts.nodes, in->parts.size);
      }
      case PM_EMBEDDED_STATEMENTS_NODE: {
        const pm_embedded_statements_node_t *en = (const pm_embedded_statements_node_t *)node;
        if (!en->statements) return ALLOC_node_lit(KORB_NIL);   /* #{} → "" via nil.to_s */
        return transduce_statements(tc, en->statements);
      }

      /* ---- locals (depth 0 = own frame, depth >= 1 = outer/closure) ---- */
      case PM_LOCAL_VARIABLE_READ_NODE: {
        const pm_local_variable_read_node_t *lr = (const pm_local_variable_read_node_t *)node;
        return lvar_read(tc, node, lr->name, lr->depth);
      }
      case PM_LOCAL_VARIABLE_WRITE_NODE: {
        const pm_local_variable_write_node_t *lw = (const pm_local_variable_write_node_t *)node;
        NODE *rval = transduce(tc, lw->value);   /* register child: no staging */
        return lvar_write(tc, node, lw->name, lw->depth, rval);
      }
      case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        /* x op= v  →  write(x, binop(read(x), v)) */
        const pm_local_variable_operator_write_node_t *ow =
            (const pm_local_variable_operator_write_node_t *)node;
        const char *opname = kp_cid_cstr(tc, ow->binary_operator);
        enum kp_binop op = kp_binop_kind(opname);
        if (op == KP_BINOP_NONE) {
            char what[64];
            snprintf(what, sizeof(what), "operator '%s='", opname);
            return kp_unsupported(tc, node, strdup(what));
        }
        uint32_t line = kp_line(tc, node);
        uint32_t n_slots = kind_node_plus.slot_count;
        NODE *lhs, *rhs;
        WITH_CHAIN(tc, n_slots, (lhs = lvar_read(tc, node, ow->name, ow->depth),
                                 rhs = transduce(tc, ow->value)));
        return lvar_write(tc, node, ow->name, ow->depth, alloc_binop(op, lhs, rhs, line));
      }
      case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
        const pm_local_variable_and_write_node_t *aw =
            (const pm_local_variable_and_write_node_t *)node;
        return ALLOC_node_and(lvar_read(tc, node, aw->name, aw->depth),
                              lvar_write(tc, node, aw->name, aw->depth, transduce(tc, aw->value)));
      }
      case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
        const pm_local_variable_or_write_node_t *ow =
            (const pm_local_variable_or_write_node_t *)node;
        return ALLOC_node_or(lvar_read(tc, node, ow->name, ow->depth),
                             lvar_write(tc, node, ow->name, ow->depth, transduce(tc, ow->value)));
      }

      /* ---- control flow ---- */
      case PM_IF_NODE: {
        const pm_if_node_t *ifn = (const pm_if_node_t *)node;
        NODE *cond = transduce(tc, ifn->predicate);     /* register child */
        NODE *then_b = transduce_statements(tc, ifn->statements);
        NODE *else_b = transduce_opt(tc, ifn->subsequent);
        return ALLOC_node_if(cond, then_b, else_b);
      }
      case PM_ELSE_NODE: {
        const pm_else_node_t *en = (const pm_else_node_t *)node;
        return transduce_statements(tc, en->statements);
      }
      case PM_UNLESS_NODE: {
        const pm_unless_node_t *un = (const pm_unless_node_t *)node;
        NODE *cond = transduce(tc, un->predicate);
        NODE *then_b = transduce_statements(tc, un->statements);  /* unless-true */
        NODE *else_b = un->else_clause
            ? transduce_statements(tc, un->else_clause->statements)
            : lit_nil();
        return ALLOC_node_if(cond, else_b, then_b);     /* swapped branches */
      }
      case PM_WHILE_NODE: {
        const pm_while_node_t *wn = (const pm_while_node_t *)node;
        if (PM_NODE_FLAG_P(wn, PM_LOOP_FLAGS_BEGIN_MODIFIER))
            return kp_unsupported(tc, node, "begin...end while (post-test loop)");
        NODE *cond = transduce(tc, wn->predicate);
        NODE *body = transduce_statements(tc, wn->statements);
        return ALLOC_node_while(cond, body, 0);
      }
      case PM_UNTIL_NODE: {
        const pm_until_node_t *un = (const pm_until_node_t *)node;
        if (PM_NODE_FLAG_P(un, PM_LOOP_FLAGS_BEGIN_MODIFIER))
            return kp_unsupported(tc, node, "begin...end until (post-test loop)");
        NODE *cond = transduce(tc, un->predicate);
        NODE *body = transduce_statements(tc, un->statements);
        return ALLOC_node_while(cond, body, 1);
      }
      case PM_AND_NODE: {
        const pm_and_node_t *an = (const pm_and_node_t *)node;
        return ALLOC_node_and(transduce(tc, an->left), transduce(tc, an->right));
      }
      case PM_OR_NODE: {
        const pm_or_node_t *on = (const pm_or_node_t *)node;
        return ALLOC_node_or(transduce(tc, on->left), transduce(tc, on->right));
      }
      case PM_RETURN_NODE: {
        const pm_return_node_t *rn = (const pm_return_node_t *)node;
        NODE *v;
        if (rn->arguments == NULL || rn->arguments->arguments.size == 0) {
            v = lit_nil();
        }
        else if (rn->arguments->arguments.size == 1) {
            v = transduce(tc, rn->arguments->arguments.nodes[0]);
        }
        else {
            return kp_unsupported(tc, node, "return with multiple values");
        }
        return ALLOC_node_return(v);
      }

      /* ---- blocks ---- */
      case PM_YIELD_NODE: {
        const pm_yield_node_t *yn = (const pm_yield_node_t *)node;
        tc->frame->uses_block = true;            /* this method reserves block cells */
        uint32_t line = kp_line(tc, node);
        size_t yargc = yn->arguments ? yn->arguments->arguments.size : 0;
        if (yargc == 0) {
            /* reserved cells: block_entry(fs-4), def_env(fs-3), captured_self(fs-2) */
            return ALLOC_node_yield0(line, -4 - tc->chain, -3 - tc->chain, -2 - tc->chain);
        }
        if (yargc == 1) {
            /* yield1 slot_count 1: a0 at cursor[-1]; reserved cells below */
            NODE *a0;
            WITH_CHAIN(tc, 1, (a0 = transduce(tc, yn->arguments->arguments.nodes[0])));
            return ALLOC_node_yield1(line, -4 - (tc->chain + 1), -3 - (tc->chain + 1), -2 - (tc->chain + 1), a0);
        }
        return kp_unsupported(tc, node, "yield with more than 1 value");
      }
      case PM_NEXT_NODE: {
        const pm_next_node_t *nn = (const pm_next_node_t *)node;
        NODE *v;
        if (nn->arguments == NULL || nn->arguments->arguments.size == 0) v = lit_nil();
        else if (nn->arguments->arguments.size == 1) v = transduce(tc, nn->arguments->arguments.nodes[0]);
        else return kp_unsupported(tc, node, "next with multiple values");
        return ALLOC_node_next(v);
      }

      /* ---- calls / def ---- */
      case PM_CALL_NODE:
        return transduce_call(tc, (const pm_call_node_t *)node);
      case PM_DEF_NODE:
        return transduce_def(tc, (const pm_def_node_t *)node);
      case PM_CLASS_NODE:
        return transduce_class(tc, (const pm_class_node_t *)node);
      case PM_CONSTANT_READ_NODE: {
        const pm_constant_read_node_t *cr = (const pm_constant_read_node_t *)node;
        return ALLOC_node_const(kp_intern_cid(tc, cr->name));
      }

      default: {
        char what[64];
        snprintf(what, sizeof(what), "syntax (prism node %d)", (int)PM_NODE_TYPE(node));
        return kp_unsupported(tc, node, strdup(what));
      }
    }
}

/* ---------------------------------------------------------------------- */

NODE *
koruby_parse_source(CTX *c, const char *src, size_t len, const char *fname)
{
    pm_parser_t parser;
    pm_options_t options = { 0 };
    pm_options_filepath_set(&options, fname);
    pm_options_line_set(&options, 1);

    pm_parser_init(&parser, (const uint8_t *)src, len, &options);
    pm_node_t *root = pm_parse(&parser);

    if (parser.error_list.size > 0) {
        /* CRuby-compatible exit path: SyntaxError → stderr + exit 1 */
        for (const pm_diagnostic_t *d = (const pm_diagnostic_t *)parser.error_list.head;
             d != NULL; d = (const pm_diagnostic_t *)d->node.next) {
            int32_t line = pm_newline_list_line(&parser.newline_list,
                                                d->location.start, parser.start_line);
            fprintf(stderr, "%s:%d: %s\n", fname, line, d->message);
        }
        fprintf(stderr, "%s: syntax error (SyntaxError)\n", fname);
        exit(1);
    }

    struct kp_ctx tc = {
        .parser = &parser,
        .c = c,
        .fname = fname,
    };
    NODE *ast = transduce(&tc, root);
    if (ast == NULL) ast = lit_nil();
    free(tc.bake_list);

    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    pm_options_free(&options);
    return ast;
}
