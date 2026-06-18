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
    uint32_t synth_cnt;       /* synthetic temporaries appended after prism locals (e.g. case subject) */
    bool uses_block;          /* yield / block_given? seen → reserve 2 frame-top cells */
    bool module_function_mode; /* no-arg `module_function` seen in this class/module body */
    uint32_t method_mid;      /* enclosing def's name (0 = not a method body) — for super */
    uint32_t method_params;   /* enclosing def's positional param count — for forwarding super */
    uint32_t max_ref_depth;   /* B3: deepest outer-scope depth this block's body reads (0=none) */
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

/* node_send takes a parse-time NODE* array [recv, args...]; these build the
 * common small-arity synthetic sends used throughout the parser (op-assign,
 * []/[]= desugar, case/when ===, ...).  The staging depth a caller must reserve
 * is the element count (recv + args). */
static NODE *kp_send0(uint32_t mid, uint32_t line, NODE *recv) {
    NODE **argv = malloc(sizeof(NODE *)); if (!argv) abort();
    argv[0] = recv;
    return ALLOC_node_send(mid, line, argv, 1);
}
static NODE *kp_send1(uint32_t mid, uint32_t line, NODE *recv, NODE *a0) {
    NODE **argv = malloc(sizeof(NODE *) * 2); if (!argv) abort();
    argv[0] = recv; argv[1] = a0;
    return ALLOC_node_send(mid, line, argv, 2);
}
static NODE *kp_send2(uint32_t mid, uint32_t line, NODE *recv, NODE *a0, NODE *a1) {
    NODE **argv = malloc(sizeof(NODE *) * 3); if (!argv) abort();
    argv[0] = recv; argv[1] = a0; argv[2] = a1;
    return ALLOC_node_send(mid, line, argv, 3);
}
#define KP_SEND1_SC 2u   /* staging depth of a recv+1arg send */

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
    f->synth_cnt = 0;
    f->uses_block = false;
    f->module_function_mode = false;
    f->method_mid = 0;
    f->method_params = 0;
    f->max_ref_depth = 0;
    f->prev = tc->frame;
    tc->frame = f;
    tc->chain = 0;
}

/* Returns the frame size.  Every frame reserves self(fs-1) + def_class(fs-2)
 * on top; a yielding frame also reserves the 3-cell block group below them.
 * Layout top-down:
 *   [locals... | block_entry(fs-5) | def_env(fs-4) | captured_self(fs-3)
 *              | def_class(fs-2) | self(fs-1)] */
static uint32_t
pop_frame(struct kp_ctx *tc)
{
    struct kp_frame *f = tc->frame;
    uint32_t frame_size = (uint32_t)f->locals->size + f->synth_cnt + 2u + (f->uses_block ? 3u : 0u);
    for (uint32_t i = f->bake_base; i < tc->bake_cnt; i++) {
        *tc->bake_list[i] -= (int32_t)frame_size;
    }
    tc->bake_cnt = f->bake_base;
    tc->chain = f->saved_chain;
    /* B3: a nested block reaching depth d reaches depth d-1 from its parent, so
     * the parent (if reified as a Proc) must materialize that far too.  prism's
     * depths already stop at method boundaries, so methods stay at 0. */
    if (f->prev && f->max_ref_depth >= 1) {
        uint32_t up = f->max_ref_depth - 1;
        if (up > f->prev->max_ref_depth) f->prev->max_ref_depth = up;
    }
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

/* Reserve a synthetic temporary in the current frame, appended after prism
 * locals.  Returns its local index (usable with bake_lget/bake_lset). */
static uint32_t
alloc_synth_local(struct kp_ctx *tc)
{
    return (uint32_t)tc->frame->locals->size + tc->frame->synth_cnt++;
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
/* record the deepest outer-scope read/write so a reified Proc knows how many
 * enclosing scopes to materialize on escape (B3). */
static void kp_note_depth(struct kp_ctx *tc, uint32_t depth) {
    if (depth > tc->frame->max_ref_depth) tc->frame->max_ref_depth = depth;
}
static NODE *
lvar_read(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid, uint32_t depth)
{
    if (depth == 0) return bake_lget(tc, lvar_index(tc, node, cid));
    kp_note_depth(tc, depth);
    return bake_eget(tc, depth, lvar_index_at(tc, node, cid, depth));
}

static NODE *
lvar_write(struct kp_ctx *tc, const pm_node_t *node, pm_constant_id_t cid, uint32_t depth, NODE *rval)
{
    if (depth == 0) return bake_lset(tc, lvar_index(tc, node, cid), rval);
    kp_note_depth(tc, depth);
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
extern const struct NodeKind kind_node_caseeq;       /* case/when `v === subj` */
extern const struct NodeKind kind_node_aref;         /* recv[idx] */
extern const struct NodeKind kind_node_aset;         /* recv[idx] = val */
extern const struct NodeKind kind_node_ary_push;     /* array-literal push chain */
extern const struct NodeKind kind_node_ary_concat;   /* array-literal splat (*) chain */
extern const struct NodeKind kind_node_const_set;    /* FOO = expr */
static NODE *build_array(struct kp_ctx *tc, struct pm_node **elems, size_t n, uint32_t capa);
extern const struct NodeKind kind_node_hash_merge;   /* hash literal ** splat chain */
extern const struct NodeKind kind_node_dstr_concat;  /* string-interp concat chain */
extern const struct NodeKind kind_node_hash_set;     /* hash-literal set chain */
extern const struct NodeKind kind_node_range_new;    /* range literal */

enum kp_binop {
    KP_BINOP_NONE = 0,
    KP_PLUS, KP_MINUS, KP_MUL, KP_DIV, KP_MOD,
    KP_LT, KP_LE, KP_GT, KP_GE, KP_EQ, KP_NEQ,
    KP_BAND, KP_BOR, KP_BXOR, KP_SHL, KP_SHR,
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
    if (strcmp(name, "&") == 0)  return KP_BAND;
    if (strcmp(name, "|") == 0)  return KP_BOR;
    if (strcmp(name, "^") == 0)  return KP_BXOR;
    if (strcmp(name, "<<") == 0) return KP_SHL;
    if (strcmp(name, ">>") == 0) return KP_SHR;
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
      case KP_BAND:  return ALLOC_node_band(lhs, rhs, line);
      case KP_BOR:   return ALLOC_node_bor(lhs, rhs, line);
      case KP_BXOR:  return ALLOC_node_bxor(lhs, rhs, line);
      case KP_SHL:   return ALLOC_node_shl(lhs, rhs, line);
      case KP_SHR:   return ALLOC_node_shr(lhs, rhs, line);
      default:       abort();
    }
}

/* ---- calls -------------------------------------------------------------- */

/* Parse a block literal into a node_entry (its own scope; registered as an
 * AOT entry like a method body).  docs/v2_blocks_design.md. */
static NODE *
transduce_block_parts(struct kp_ctx *tc, const pm_constant_id_list_t *blk_locals,
                      const pm_node_t *blk_params, const pm_node_t *blk_body)
{
    push_frame(tc, blk_locals);

    uint32_t bparams = 0;
    uint32_t destructure_n = 0;     /* >0 for a single |(a,b,...)| destructuring param */
    uint8_t *destructure_spec = NULL;  /* per-param arity for mixed |a,(k,v)| (0=scalar) */
    if (blk_params && PM_NODE_TYPE_P(blk_params, PM_NUMBERED_PARAMETERS_NODE)) {
        /* `{ _1 * _2 }` — prism puts `_1`.._N in locals[0..N-1]; the body's
         * `_N` reads resolve as ordinary locals.  N = maximum referenced. */
        bparams = ((const pm_numbered_parameters_node_t *)blk_params)->maximum;
    } else if (blk_params && PM_NODE_TYPE_P(blk_params, PM_IT_PARAMETERS_NODE)) {
        /* `{ it * 2 }` — one implicit param `it`; the body reads it via a
         * PM_IT_LOCAL_VARIABLE_READ_NODE, mapped to local slot 0 in transduce. */
        bparams = 1;
    } else if (blk_params) {
        const pm_parameters_node_t *ps;
        if (PM_NODE_TYPE_P(blk_params, PM_BLOCK_PARAMETERS_NODE)) {
            const pm_block_parameters_node_t *bp = (const pm_block_parameters_node_t *)blk_params;
            if (bp->locals.size) {
                pop_frame(tc);
                return kp_unsupported(tc, blk_params, "block-local variables (|x; y|)");
            }
            ps = bp->parameters;
        } else if (PM_NODE_TYPE_P(blk_params, PM_PARAMETERS_NODE)) {   /* `->(x) {}` lambda params */
            ps = (const pm_parameters_node_t *)blk_params;
        } else {
            pop_frame(tc);
            return kp_unsupported(tc, blk_params, "unsupported block parameters");
        }
        if (ps) {
            if (ps->optionals.size || ps->rest || ps->posts.size ||
                ps->keywords.size || ps->keyword_rest || ps->block) {
                pop_frame(tc);
                return kp_unsupported(tc, (const pm_node_t *)ps,
                                      "non-positional block parameters");
            }
            /* single |(a, b, ...)| → destructure the one array arg into N locals */
            if (ps->requireds.size == 1 && PM_NODE_TYPE_P(ps->requireds.nodes[0], PM_MULTI_TARGET_NODE)) {
                const pm_multi_target_node_t *mt = (const pm_multi_target_node_t *)ps->requireds.nodes[0];
                if (mt->rest || mt->rights.size) {
                    pop_frame(tc);
                    return kp_unsupported(tc, ps->requireds.nodes[0], "block param with splat/post destructure");
                }
                for (uint32_t i = 0; i < mt->lefts.size; i++) {
                    const pm_node_t *t = mt->lefts.nodes[i];
                    pm_constant_id_t cid;
                    if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE))
                        cid = ((const pm_local_variable_target_node_t *)t)->name;
                    else if (PM_NODE_TYPE_P(t, PM_REQUIRED_PARAMETER_NODE))
                        cid = ((const pm_required_parameter_node_t *)t)->name;
                    else {
                        pop_frame(tc);
                        return kp_unsupported(tc, t, "nested destructuring block parameter");
                    }
                    if (lvar_index(tc, t, cid) != i)
                        kp_failf(tc, t, "koruby_precise: destructure target '%s' is not locals[%u]", kp_cid_cstr(tc, cid), i);
                }
                bparams = 1;
                destructure_n = (uint32_t)mt->lefts.size;
            } else {
                bparams = (uint32_t)ps->requireds.size;
                bool any_destr = false;
                for (uint32_t i = 0; i < bparams; i++)
                    if (PM_NODE_TYPE_P(ps->requireds.nodes[i], PM_MULTI_TARGET_NODE)) { any_destr = true; break; }
                if (!any_destr) {
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
                } else {
                    /* mixed scalar + |(...)| destructuring params, e.g. |a, (k, v)|.
                     * spec[i] = destructure arity (0 = scalar); local slots are
                     * assigned left-to-right, scalar=1 slot, destructure=N slots. */
                    uint8_t *spec = malloc(bparams ? bparams : 1);
                    if (!spec) abort();
                    uint32_t loc = 0;
                    for (uint32_t i = 0; i < bparams; i++) {
                        const pm_node_t *p = ps->requireds.nodes[i];
                        if (PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) {
                            spec[i] = 0;
                            pm_constant_id_t cid = ((const pm_required_parameter_node_t *)p)->name;
                            if (lvar_index(tc, p, cid) != loc)
                                kp_failf(tc, p, "koruby_precise: block param '%s' is not locals[%u]", kp_cid_cstr(tc, cid), loc);
                            loc++;
                        } else if (PM_NODE_TYPE_P(p, PM_MULTI_TARGET_NODE)) {
                            const pm_multi_target_node_t *mt = (const pm_multi_target_node_t *)p;
                            if (mt->rest || mt->rights.size || mt->lefts.size == 0 || mt->lefts.size > 255) {
                                free(spec); pop_frame(tc);
                                return kp_unsupported(tc, p, "block param with splat/post destructure");
                            }
                            spec[i] = (uint8_t)mt->lefts.size;
                            for (uint32_t j = 0; j < mt->lefts.size; j++) {
                                const pm_node_t *t = mt->lefts.nodes[j];
                                pm_constant_id_t cid;
                                if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE))
                                    cid = ((const pm_local_variable_target_node_t *)t)->name;
                                else if (PM_NODE_TYPE_P(t, PM_REQUIRED_PARAMETER_NODE))
                                    cid = ((const pm_required_parameter_node_t *)t)->name;
                                else {
                                    free(spec); pop_frame(tc);
                                    return kp_unsupported(tc, t, "nested destructuring block parameter");
                                }
                                if (lvar_index(tc, t, cid) != loc)
                                    kp_failf(tc, t, "koruby_precise: destructure target '%s' is not locals[%u]", kp_cid_cstr(tc, cid), loc);
                                loc++;
                            }
                        } else {
                            free(spec); pop_frame(tc);
                            return kp_unsupported(tc, p, "destructuring block parameter");
                        }
                    }
                    destructure_spec = spec;
                }
            }
        }
    }

    NODE *body;
    if (blk_body == NULL) {
        body = lit_nil();
    }
    else if (PM_NODE_TYPE_P(blk_body, PM_STATEMENTS_NODE)) {
        body = transduce_statements(tc, (const pm_statements_node_t *)blk_body);
    }
    else {
        body = kp_unsupported(tc, blk_body, "block body with rescue/ensure");
    }

    /* B3 capture metadata: how many enclosing scopes this block reaches, and
     * each one's local count (final prism locals->size — synth temps aren't
     * captured by closures).  Computed before pop_frame (needs the prev chain). */
    uint32_t cap_depth = tc->frame->max_ref_depth;
    uint16_t *cap_ns = NULL;
    if (cap_depth > 0) {
        cap_ns = malloc(sizeof(uint16_t) * cap_depth);
        if (!cap_ns) abort();
        struct kp_frame *encl = tc->frame->prev;
        for (uint32_t k = 0; k < cap_depth; k++) {
            cap_ns[k] = encl ? (uint16_t)encl->locals->size : 0;
            encl = encl ? encl->prev : NULL;
        }
    }
    uint32_t frame_size = pop_frame(tc);    /* block locals (+2 if the block yields) */
    NODE *entry = ALLOC_node_entry(body, bparams, frame_size, destructure_n, destructure_spec, cap_depth, cap_ns);
    /* node_entry is the dispatch root (yield → entry->head.dispatcher); its own
     * AOT entry, body inlined into its SD. */
    code_repo_add("block", entry, true);
    return entry;
}

static NODE *
transduce_block(struct kp_ctx *tc, const pm_block_node_t *blk)
{
    return transduce_block_parts(tc, &blk->locals, blk->parameters, blk->body);
}

/* Call with a literal block.  Bakes def_env_off (caller frame base) and
 * hands the node_entry + def_env to the callee.  B2: 0 or 1 positional arg. */
/* Synthesize the block `{ |x| x.sym }` for `&:sym` (symbol-to-proc), reusing the
 * normal block machinery — a real node_entry, so dispatcher prefetch is safe. */
static NODE *
kp_symbol_block(struct kp_ctx *tc, uint32_t sym_id)
{
    static pm_constant_id_t one_id[1] = { 0 };       /* one synthetic local `x` */
    pm_constant_id_list_t fake; fake.ids = one_id; fake.size = 1; fake.capacity = 1;
    push_frame(tc, &fake);
    NODE *recv;
    WITH_CHAIN(tc, 1, (recv = bake_lget(tc, 0)));     /* x (local 0), staged as send recv */
    NODE *body = kp_send0(sym_id, 0, recv);
    uint32_t frame_size = pop_frame(tc);
    NODE *entry = ALLOC_node_entry(body, 1, frame_size, 0, NULL, 0, NULL);
    code_repo_add("symblock", entry, true);
    return entry;
}

/* Resolve a call's block: a literal `{ }` → real node_entry; `&:sym` → a
 * synthesized `{ |x| x.sym }` block; else NULL = unsupported. */
static NODE *
kp_block_entry(struct kp_ctx *tc, const pm_node_t *blk)
{
    if (PM_NODE_TYPE_P(blk, PM_BLOCK_NODE))
        return transduce_block(tc, (const pm_block_node_t *)blk);
    if (PM_NODE_TYPE_P(blk, PM_BLOCK_ARGUMENT_NODE)) {
        const pm_block_argument_node_t *ba = (const pm_block_argument_node_t *)blk;
        if (ba->expression && PM_NODE_TYPE_P(ba->expression, PM_SYMBOL_NODE)) {
            const pm_symbol_node_t *sn = (const pm_symbol_node_t *)ba->expression;
            size_t len = pm_string_length(&sn->unescaped);
            uint32_t id = korb_intern(tc->c->vm, (const char *)pm_string_source(&sn->unescaped), len);
            return kp_symbol_block(tc, id);
        }
    }
    return NULL;
}

static NODE *
transduce_call_with_block(struct kp_ctx *tc, const pm_call_node_t *cn, uint32_t mid,
                          uint32_t line, const pm_arguments_node_t *args, size_t argc,
                          NODE *entry)
{
    /* def_env_off: cursor → caller frame base = -(chain + staging); staging =
     * argc.  bake_add fixes up by the caller's frame_size.  node_call_blk stages
     * the args via argv@children (any fixed arity). */
    int32_t self_off = -1 - tc->chain - (int32_t)argc;  /* caller self = block's captured self */
    NODE **argv = (argc ? malloc(sizeof(NODE *) * argc) : NULL);
    if (argc && !argv) abort();
    int32_t saved = tc->chain;
    tc->chain = saved + (int32_t)argc;
    for (size_t i = 0; i < argc; i++)
        argv[i] = transduce(tc, args->arguments.nodes[i]);
    tc->chain = saved;
    NODE *call = ALLOC_node_call_blk(mid, line, self_off, entry, -(tc->chain + (int32_t)argc), argv, (uint32_t)argc);
    bake_add(tc, &call->u.node_call_blk.def_env_off);
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
        return ALLOC_node_block_given(-5 - tc->chain);   /* block_entry cell (fs-5) */
    }

    /* bare `module_function` — promote subsequent defs to module (singleton)
     * methods too.  (The `module_function :sym` arg form stays a runtime no-op.) */
    if (argc == 0 && cn->block == NULL &&
        strcmp(kp_cid_cstr(tc, cn->name), "module_function") == 0) {
        tc->frame->module_function_mode = true;
        return lit_nil();
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
        /* proc { } / lambda { } — reify the literal block into a Proc object. */
        if (argc == 0 && PM_NODE_TYPE_P(cn->block, PM_BLOCK_NODE)) {
            const char *cnm = kp_cid_cstr(tc, cn->name);
            int is_lam = !strcmp(cnm, "lambda");
            if (is_lam || !strcmp(cnm, "proc")) {
                NODE *entry = transduce_block(tc, (const pm_block_node_t *)cn->block);
                int32_t self_off = -1 - tc->chain;
                NODE *mk = ALLOC_node_make_proc(entry, -tc->chain, self_off, (uint32_t)is_lam);
                bake_add(tc, &mk->u.node_make_proc.def_env_off);
                return mk;
            }
        }
        NODE *entry = kp_block_entry(tc, cn->block);
        if (!entry) return kp_unsupported(tc, (const pm_node_t *)cn, "&block argument (only literal block or &:sym)");
        return transduce_call_with_block(tc, cn, mid, line, args, argc, entry);
    }

    /* actual `*splat` call f(*arr): build the args Array, dispatch dynamically */
    {
        bool has_splat = false;
        for (size_t i = 0; i < argc; i++)
            if (PM_NODE_TYPE_P(args->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
        if (has_splat) {
            int32_t self_off = -1 - tc->chain - 1;       /* one staged child: the args array */
            NODE *arr;
            WITH_CHAIN(tc, 1, (arr = build_array(tc, args->arguments.nodes, argc, (uint32_t)argc)));
            return ALLOC_node_call_splat(mid, line, self_off, arr);
        }
    }

    /* caller self cell (base[fs-1]); the argc staged args advance the body
     * cursor, so offset back past them too.  node_call stages the args via
     * argv@children (any fixed arity). */
    {
        int32_t self_off = -1 - tc->chain - (int32_t)argc;
        NODE **argv = (argc ? malloc(sizeof(NODE *) * argc) : NULL);
        if (argc && !argv) abort();
        int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)argc;
        for (size_t i = 0; i < argc; i++)
            argv[i] = transduce(tc, args->arguments.nodes[i]);
        tc->chain = saved;
        return ALLOC_node_call(mid, line, self_off, argv, (uint32_t)argc);
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

    /* recv[idx] / recv[idx] = val → type-fast-path nodes (Array+fixnum inline,
     * else deopt to a real send).  No block, no splat args. */
    if (!cn->block) {
        if (mid == tc->c->vm->mid_aref && argc == 1 &&
            !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[0], PM_SPLAT_NODE)) {
            NODE *recv, *idx;
            WITH_CHAIN(tc, kind_node_aref.slot_count,
                       (recv = transduce(tc, cn->receiver),
                        idx  = transduce(tc, cn->arguments->arguments.nodes[0])));
            return ALLOC_node_aref(line, recv, idx);
        }
        if (mid == tc->c->vm->mid_aset && argc == 2 &&
            !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[0], PM_SPLAT_NODE) &&
            !PM_NODE_TYPE_P(cn->arguments->arguments.nodes[1], PM_SPLAT_NODE)) {
            NODE *recv, *idx, *val;
            WITH_CHAIN(tc, kind_node_aset.slot_count,
                       (recv = transduce(tc, cn->receiver),
                        idx  = transduce(tc, cn->arguments->arguments.nodes[0]),
                        val  = transduce(tc, cn->arguments->arguments.nodes[1])));
            return ALLOC_node_aset(line, recv, idx, val);
        }
    }

    /* receiver method dispatch with a block: recv.mid(args) { ... } or &:sym */
    if (cn->block) {
        NODE *entry = kp_block_entry(tc, cn->block);
        if (!entry) return kp_unsupported(tc, (const pm_node_t *)cn, "&block argument (only literal block or &:sym)");
        /* def_env_off: cursor → caller frame base = -(chain + staging); staging
         * = recv(1) + argc.  bake_add fixes up by the caller's frame_size.
         * node_send_blk stages [recv, args...] via argv@children (any arity). */
        uint32_t sc = 1u + (uint32_t)argc;
        int32_t self_off = -1 - tc->chain - (int32_t)sc;
        NODE **argv = malloc(sizeof(NODE *) * sc);
        if (!argv) abort();
        int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)sc;
        argv[0] = transduce(tc, cn->receiver);
        for (size_t i = 0; i < argc; i++)
            argv[1 + i] = transduce(tc, cn->arguments->arguments.nodes[i]);
        tc->chain = saved;
        NODE *call = ALLOC_node_send_blk(mid, line, self_off, entry, -(tc->chain + (int32_t)sc), argv, sc);
        bake_add(tc, &call->u.node_send_blk.def_env_off);
        return call;
    }
    /* actual `*splat` receiver call recv.m(*arr): build args Array, dynamic dispatch */
    {
        bool has_splat = false;
        for (size_t i = 0; i < argc; i++)
            if (PM_NODE_TYPE_P(cn->arguments->arguments.nodes[i], PM_SPLAT_NODE)) { has_splat = true; break; }
        if (has_splat) {
            NODE *recv, *arr;
            WITH_CHAIN(tc, 2, (recv = transduce(tc, cn->receiver),
                               arr  = build_array(tc, cn->arguments->arguments.nodes, argc, (uint32_t)argc)));
            return ALLOC_node_send_splat(mid, line, recv, arr);
        }
    }
    /* unified send (any fixed arity): stage [recv, arg0..] into a parse-time
     * NODE* array; node_send / node_send_safe stage them into consecutive slots
     * at eval.  Safe navigation (recv&.m) nil-checks recv before dispatch. */
    {
        bool safe = (cn->base.flags & PM_CALL_NODE_FLAGS_SAFE_NAVIGATION) != 0;
        uint32_t cnt = 1u + (uint32_t)argc;
        NODE **argv = malloc(sizeof(NODE *) * cnt);
        if (!argv) abort();
        int32_t saved = tc->chain;
        tc->chain = saved + (int32_t)cnt;
        argv[0] = transduce(tc, cn->receiver);
        for (size_t i = 0; i < argc; i++)
            argv[1 + i] = transduce(tc, cn->arguments->arguments.nodes[i]);
        tc->chain = saved;
        return safe ? ALLOC_node_send_safe(mid, line, argv, cnt)
                    : ALLOC_node_send(mid, line, argv, cnt);
    }
}

/* ---- def ----------------------------------------------------------------- */

static NODE *
transduce_def(struct kp_ctx *tc, const pm_def_node_t *dn)
{
    /* `def recv.name` — singleton method.  Evaluate the receiver in the enclosing
     * scope (staged as the node's child, like node_class's super); attach to its
     * singleton class. */
    NODE *recv_node = NULL;
    /* `module_function` mode: an instance def also becomes a singleton method on
     * self (the module).  recv_node = self for the singleton copy. */
    bool mod_func = !dn->receiver && tc->frame->module_function_mode;
    if (dn->receiver)   WITH_CHAIN(tc, 1, (recv_node = transduce(tc, dn->receiver)));
    else if (mod_func)  WITH_CHAIN(tc, 1, (recv_node = ALLOC_node_self(-1 - tc->chain)));

    uint32_t params_cnt = 0, req_cnt = 0, opt_cnt = 0;
    const pm_parameters_node_t *ps = dn->parameters;
    pm_constant_id_t blk_param_name = 0;   /* &blk param name (0 = none) */
    if (ps) {
        if (ps->block) {
            if (!ps->block->name)
                return kp_unsupported(tc, (const pm_node_t *)dn, "anonymous/forwarding block parameter (&)");
            blk_param_name = ps->block->name;
        }
        if (ps->posts.size && !ps->rest) {     /* posts only appear after a rest in Ruby */
            return kp_unsupported(tc, (const pm_node_t *)dn, "post parameters without rest");
        }
        if (ps->rest && !(PM_NODE_TYPE_P(ps->rest, PM_REST_PARAMETER_NODE) &&
                          ((const pm_rest_parameter_node_t *)ps->rest)->name))
            return kp_unsupported(tc, (const pm_node_t *)dn, "anonymous/forwarding rest parameter");
        req_cnt = (uint32_t)ps->requireds.size;
        opt_cnt = (uint32_t)ps->optionals.size;
        params_cnt = req_cnt + opt_cnt;   /* positional fixed slots; rest/keywords follow */
    }

    push_frame(tc, &dn->locals);
    tc->frame->method_mid = kp_intern_cid(tc, dn->name);   /* for `super` inside the body */
    tc->frame->method_params = params_cnt;

    /* prism orders def locals with the parameters first (required, then optional);
     * the staged-args window doubles as the parameter slots.  Verify the layout. */
    struct Node **opt_defaults = NULL;
    if (dn->parameters) {
        for (uint32_t i = 0; i < req_cnt; i++) {
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
        if (opt_cnt) {
            opt_defaults = malloc(sizeof(struct Node *) * opt_cnt);
            if (!opt_defaults) abort();
            for (uint32_t j = 0; j < opt_cnt; j++) {
                const pm_optional_parameter_node_t *op =
                    (const pm_optional_parameter_node_t *)dn->parameters->optionals.nodes[j];
                if (lvar_index(tc, (const pm_node_t *)op, op->name) != req_cnt + j) {
                    kp_failf(tc, (const pm_node_t *)op, "koruby_precise: optional '%s' is not locals[%u]",
                             kp_cid_cstr(tc, op->name), req_cnt + j);
                }
                /* default expr runs in method scope at the body cursor (chain 0) */
                opt_defaults[j] = transduce(tc, op->value);
            }
        }
    }

    /* *rest param: collects surplus positionals; its local slot follows req+opt. */
    int32_t rest_slot = -1;
    if (ps && ps->rest) {
        pm_constant_id_t rn = ((const pm_rest_parameter_node_t *)ps->rest)->name;
        rest_slot = (int32_t)lvar_index(tc, ps->rest, rn);
    }

    /* post params (after *rest): plain requireds occupying the slots right after
     * the rest slot, bound from the tail of the positional args. */
    uint32_t post_cnt = ps ? (uint32_t)ps->posts.size : 0;
    for (uint32_t i = 0; i < post_cnt; i++) {
        const pm_node_t *p = ps->posts.nodes[i];
        if (!PM_NODE_TYPE_P(p, PM_REQUIRED_PARAMETER_NODE)) { pop_frame(tc); return kp_unsupported(tc, p, "non-plain post parameter"); }
        pm_constant_id_t cid = ((const pm_required_parameter_node_t *)p)->name;
        if (lvar_index(tc, p, cid) != (uint32_t)rest_slot + 1 + i)
            kp_failf(tc, p, "koruby_precise: post '%s' is not locals[%u]", kp_cid_cstr(tc, cid), (uint32_t)rest_slot + 1 + i);
    }

    /* keyword params (required `k:` / optional `k: default`) + keyword-rest `**kw`,
     * occupying locals after the positional params; bound by name in korb_invoke_method. */
    struct korb_kw_info *kw_info = NULL;
    if (ps && (ps->keywords.size || ps->keyword_rest)) {
        kw_info = malloc(sizeof(*kw_info));
        if (!kw_info) abort();
        kw_info->count = (uint32_t)ps->keywords.size;
        kw_info->kwrest_slot = -1;
        kw_info->entries = ps->keywords.size ? malloc(sizeof(struct korb_kw_entry) * ps->keywords.size) : NULL;
        for (uint32_t j = 0; j < kw_info->count; j++) {
            const pm_node_t *kp = ps->keywords.nodes[j];
            pm_constant_id_t name; NODE *deflt = NULL;
            if (PM_NODE_TYPE_P(kp, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) {
                name = ((const pm_required_keyword_parameter_node_t *)kp)->name;
            } else if (PM_NODE_TYPE_P(kp, PM_OPTIONAL_KEYWORD_PARAMETER_NODE)) {
                const pm_optional_keyword_parameter_node_t *ok = (const pm_optional_keyword_parameter_node_t *)kp;
                name = ok->name;
                deflt = transduce(tc, ok->value);    /* default runs at body cursor */
            } else { pop_frame(tc); return kp_unsupported(tc, kp, "keyword parameter form"); }
            kw_info->entries[j].mid  = kp_intern_cid(tc, name);
            kw_info->entries[j].slot = lvar_index(tc, kp, name);
            kw_info->entries[j].deflt = deflt;
        }
        if (ps->keyword_rest && PM_NODE_TYPE_P(ps->keyword_rest, PM_KEYWORD_REST_PARAMETER_NODE)) {
            pm_constant_id_t kr = ((const pm_keyword_rest_parameter_node_t *)ps->keyword_rest)->name;
            if (kr) kw_info->kwrest_slot = (int32_t)lvar_index(tc, ps->keyword_rest, kr);
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
        /* def-body rescue/ensure — the body is an (implicit) begin node, handled
         * by the PM_BEGIN_NODE transducer (node_begin). */
        body = transduce(tc, dn->body);
    }

    if (blk_param_name) {                  /* `&blk` → materialize the block into the local at body entry */
        tc->frame->uses_block = true;      /* reserve the frame's block cells */
        int32_t dst = (int32_t)lvar_index(tc, (const pm_node_t *)ps->block, blk_param_name) - tc->chain;
        NODE *bp = ALLOC_node_blkparam(dst, -5 - tc->chain, -4 - tc->chain, -3 - tc->chain);
        bake_add(tc, &bp->u.node_blkparam.dst_off);
        body = ALLOC_node_seq(bp, body);
    }
    uint32_t uses_block = tc->frame->uses_block ? 1u : 0u;
    uint32_t frame_size = pop_frame(tc);   /* = locals + 2 if the method yields */

    uint32_t mid = kp_intern_cid(tc, dn->name);
    /* self at the def site (enclosing frame) = the default definee */
    NODE *def;
    if (mod_func) {
        /* module_function: define as instance method AND as a singleton on self. */
        NODE *idef = ALLOC_node_def(mid, body, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, -1 - tc->chain);
        NODE *sdef = ALLOC_node_singleton_def(mid, body, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, recv_node);
        def = ALLOC_node_seq(idef, sdef);
    } else {
        def = recv_node
            ? ALLOC_node_singleton_def(mid, body, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, recv_node)
            : ALLOC_node_def(mid, body, params_cnt, req_cnt, post_cnt, rest_slot, frame_size, uses_block, opt_defaults, kw_info, -1 - tc->chain);
    }

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
    if (!PM_NODE_TYPE_P(cn->constant_path, PM_CONSTANT_READ_NODE))
        return kp_unsupported(tc, (const pm_node_t *)cn, "namespaced class name");
    uint32_t name_sym = kp_intern_cid(tc, cn->name);

    /* superclass expression (evaluated in the ENCLOSING scope) → node_class's
     * staged child; nil when absent. */
    NODE *super_node;
    WITH_CHAIN(tc, 1, (super_node = cn->superclass ? transduce(tc, cn->superclass)
                                                   : ALLOC_node_lit(KORB_NIL)));

    push_frame(tc, &cn->locals);
    NODE *body;
    if (cn->body == NULL)
        body = lit_nil();
    else if (PM_NODE_TYPE_P(cn->body, PM_STATEMENTS_NODE))
        body = transduce_statements(tc, (const pm_statements_node_t *)cn->body);
    else
        body = kp_unsupported(tc, cn->body, "class body with rescue/ensure");
    uint32_t frame_size = pop_frame(tc);

    NODE *entry = ALLOC_node_entry(body, 0, frame_size, 0, NULL, 0, NULL);
    code_repo_add("class", entry, true);          /* its own AOT entry */
    return ALLOC_node_class(name_sym, entry, super_node);
}

/* Build `lget(t).[](i)` — index a synth-temp array; used by the general
 * multi-assign desugar (`a, b.x = arr` → t=arr; a=t[0]; b.x=t[1]). */
static NODE *
mw_index_get(struct kp_ctx *tc, uint32_t t, uint32_t i, uint32_t aref, uint32_t line)
{
    NODE *r, *k;
    WITH_CHAIN(tc, 2, (r = bake_lget(tc, t), k = ALLOC_node_lit(LONG2FIX((intptr_t)i))));
    return kp_send1(aref, line, r, k);
}

/* `module Name ... end` → node_module (own scope, run with self = the module). */
static NODE *
transduce_module(struct kp_ctx *tc, const pm_module_node_t *mn)
{
    if (!PM_NODE_TYPE_P(mn->constant_path, PM_CONSTANT_READ_NODE))
        return kp_unsupported(tc, (const pm_node_t *)mn, "namespaced module name");
    uint32_t name_sym = kp_intern_cid(tc, mn->name);
    push_frame(tc, &mn->locals);
    NODE *body;
    if (mn->body == NULL)
        body = lit_nil();
    else if (PM_NODE_TYPE_P(mn->body, PM_STATEMENTS_NODE))
        body = transduce_statements(tc, (const pm_statements_node_t *)mn->body);
    else
        body = kp_unsupported(tc, mn->body, "module body with rescue/ensure");
    uint32_t frame_size = pop_frame(tc);

    NODE *entry = ALLOC_node_entry(body, 0, frame_size, 0, NULL, 0, NULL);
    code_repo_add("module", entry, true);
    return ALLOC_node_module(name_sym, entry);
}

/* Array literal `[e0, e1, ...]` → inside-out push chain (variadic @child is
 * unsupported).  Element i nests i pushes deep, so it transduces at the chain
 * depth matching its runtime cursor offset. */
static NODE *
build_array(struct kp_ctx *tc, struct pm_node **elems, size_t n, uint32_t capa)
{
    if (n == 0) return ALLOC_node_array_new(capa);
    const pm_node_t *last = elems[n - 1];
    bool splat = PM_NODE_TYPE_P(last, PM_SPLAT_NODE);
    const pm_node_t *expr = splat ? (const pm_node_t *)((const pm_splat_node_t *)last)->expression : last;
    if (splat && expr == NULL) return build_array(tc, elems, n - 1, capa);   /* bare `*` → nothing */
    NODE *acc, *elem;
    uint32_t sc = kind_node_ary_push.slot_count;        /* concat shares the push layout */
    WITH_CHAIN(tc, sc, (acc  = build_array(tc, elems, n - 1, capa),
                        elem = transduce(tc, expr)));
    return splat ? ALLOC_node_ary_concat(acc, elem) : ALLOC_node_ary_push(acc, elem);
}

/* Hash literal `{k => v, ...}` → inside-out set chain (same shape as
 * build_array): hash_set(hash_set(hash_new(n), k0, v0), k1, v1)... */
static NODE *
build_hash(struct kp_ctx *tc, struct pm_node **assocs, size_t n, uint32_t capa)
{
    if (n == 0) return ALLOC_node_hash_new(capa);
    const pm_node_t *last = assocs[n - 1];
    if (PM_NODE_TYPE_P(last, PM_ASSOC_SPLAT_NODE)) {       /* `**h` → merge */
        const pm_node_t *expr = (const pm_node_t *)((const pm_assoc_splat_node_t *)last)->value;
        if (expr == NULL) return build_hash(tc, assocs, n - 1, capa);   /* bare `**` */
        NODE *acc, *src;
        uint32_t sc = kind_node_hash_merge.slot_count;
        WITH_CHAIN(tc, sc, (acc = build_hash(tc, assocs, n - 1, capa),
                            src = transduce(tc, expr)));
        return ALLOC_node_hash_merge(acc, src);
    }
    const pm_assoc_node_t *as = (const pm_assoc_node_t *)last;
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
        if (bn->else_clause)
            return kp_unsupported(tc, node, "begin/else");
        if (!bn->rescue_clause && !bn->ensure_clause)   /* plain begin/end */
            return transduce_statements(tc, bn->statements);

        uint32_t flags = 0;
        int32_t resc_var = 0;
        NODE *body = bn->statements ? transduce_statements(tc, bn->statements) : lit_nil();
        NODE *rescue_class = lit_nil();
        NODE *resc = lit_nil();
        NODE *ensure_b = lit_nil();

        if (bn->rescue_clause) {
            const pm_rescue_node_t *rc = bn->rescue_clause;
            if (rc->subsequent)                          /* multiple rescue clauses */
                return kp_unsupported(tc, node, "multiple rescue clauses");
            if (rc->exceptions.size > 1)                 /* rescue A, B — multiple classes */
                return kp_unsupported(tc, node, "rescue with multiple exception classes");
            flags |= 1u;
            /* bare rescue catches StandardError; `rescue C` catches C-and-below */
            rescue_class = (rc->exceptions.size == 1)
                ? transduce(tc, rc->exceptions.nodes[0])
                : ALLOC_node_const(korb_intern(tc->c->vm, "StandardError", 13));
            if (rc->reference) {
                if (!PM_NODE_TYPE_P(rc->reference, PM_LOCAL_VARIABLE_TARGET_NODE))
                    return kp_unsupported(tc, node, "rescue => non-local target");
                const pm_local_variable_target_node_t *ref = (const pm_local_variable_target_node_t *)rc->reference;
                uint32_t idx = lvar_index(tc, rc->reference, ref->name);
                resc_var = (int32_t)idx - tc->chain;     /* lvar offset (frame_size fixup below) */
                flags |= 4u;
            }
            resc = rc->statements ? transduce_statements(tc, rc->statements) : lit_nil();
        }
        if (bn->ensure_clause) {
            const pm_ensure_node_t *en = bn->ensure_clause;
            ensure_b = en->statements ? transduce_statements(tc, en->statements) : lit_nil();
            flags |= 2u;
        }
        NODE *nd = ALLOC_node_begin(body, rescue_class, resc, ensure_b, resc_var, flags);
        if (flags & 4u) bake_add(tc, &nd->u.node_begin.resc_var);
        return nd;
      }

      /* ---- literals ---- */
      case PM_INTEGER_NODE: {
        const pm_integer_node_t *in = (const pm_integer_node_t *)node;
        intptr_t v;
        if (!kp_integer_value(&in->value, &v))
            return kp_unsupported(tc, node, "Integer literal beyond Fixnum range (Bignum)");
        return ALLOC_node_lit(LONG2FIX(v));
      }
      case PM_FLOAT_NODE:
        return ALLOC_node_float(((const pm_float_node_t *)node)->value);

      case PM_RATIONAL_NODE: {     /* `2r` / `1.5r` → Rational */
        const pm_rational_node_t *rn = (const pm_rational_node_t *)node;
        intptr_t num, den;
        if (!kp_integer_value(&rn->numerator, &num) || !kp_integer_value(&rn->denominator, &den))
            return kp_unsupported(tc, node, "Rational literal beyond Fixnum range (Bignum)");
        return ALLOC_node_rational((uint64_t)num, (uint64_t)den);
      }

      case PM_IMAGINARY_NODE:      /* `3i` / `1.5i` / `2ri` → Complex(0, numeric) */
        return ALLOC_node_imaginary(transduce(tc, ((const pm_imaginary_node_t *)node)->numeric));

      case PM_IMPLICIT_NODE:       /* hash shorthand `{x:, y:}` — value = the named local/call */
        return transduce(tc, ((const pm_implicit_node_t *)node)->value);

      case PM_STRING_NODE: {
        const pm_string_node_t *sn = (const pm_string_node_t *)node;
        uint32_t len;
        const char *bytes = kp_strdup_pm(&sn->unescaped, &len);
        return ALLOC_node_str(bytes, len);
      }
      case PM_REGULAR_EXPRESSION_NODE: {   /* /pat/ → Regexp (matching via astrogre) */
        const pm_regular_expression_node_t *rn = (const pm_regular_expression_node_t *)node;
        uint32_t len;
        const char *bytes = kp_strdup_pm(&rn->unescaped, &len);
        uint32_t ci = (rn->base.flags & PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE) ? 1u : 0u;
        return ALLOC_node_regexp(bytes, len, ci);
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
      case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {   /* @x op= v */
        const pm_instance_variable_operator_write_node_t *ow =
            (const pm_instance_variable_operator_write_node_t *)node;
        const char *opname = kp_cid_cstr(tc, ow->binary_operator);
        enum kp_binop op = kp_binop_kind(opname);
        uint32_t opmid = kp_intern_cid(tc, ow->binary_operator);
        uint32_t name = kp_intern_cid(tc, ow->name), line = kp_line(tc, node);
        NODE *lhs, *rhs, *comb;
        if (op != KP_BINOP_NONE) {
            WITH_CHAIN(tc, kind_node_plus.slot_count, (lhs = ALLOC_node_ivar_get(-1 - tc->chain, name),
                                                       rhs = transduce(tc, ow->value)));
            comb = alloc_binop(op, lhs, rhs, line);
        } else {   /* &= |= ^= <<= >>= → method send */
            WITH_CHAIN(tc, KP_SEND1_SC, (lhs = ALLOC_node_ivar_get(-1 - tc->chain, name),
                                                        rhs = transduce(tc, ow->value)));
            comb = kp_send1(opmid, line, lhs, rhs);
        }
        return ALLOC_node_ivar_set(-1 - tc->chain, name, comb);
      }
      case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: {        /* @x &&= v */
        const pm_instance_variable_and_write_node_t *aw =
            (const pm_instance_variable_and_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, aw->name);
        return ALLOC_node_and(ALLOC_node_ivar_get(-1 - tc->chain, name),
                              ALLOC_node_ivar_set(-1 - tc->chain, name, transduce(tc, aw->value)));
      }
      case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {         /* @x ||= v */
        const pm_instance_variable_or_write_node_t *ow =
            (const pm_instance_variable_or_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, ow->name);
        return ALLOC_node_or(ALLOC_node_ivar_get(-1 - tc->chain, name),
                             ALLOC_node_ivar_set(-1 - tc->chain, name, transduce(tc, ow->value)));
      }

      case PM_ARRAY_NODE: {
        const pm_array_node_t *an = (const pm_array_node_t *)node;
        size_t cnt = an->elements.size;
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
        return build_hash(tc, hn->elements.nodes, cnt, (uint32_t)cnt);
      }

      case PM_KEYWORD_HASH_NODE: {       /* trailing `k: v` / `**h` args → a Hash (becomes kwargs) */
        const pm_keyword_hash_node_t *hn = (const pm_keyword_hash_node_t *)node;
        size_t cnt = hn->elements.size;
        return build_hash(tc, hn->elements.nodes, cnt, (uint32_t)cnt);
      }

      case PM_INTERPOLATED_STRING_NODE: {
        const pm_interpolated_string_node_t *in = (const pm_interpolated_string_node_t *)node;
        return build_dstr(tc, in->parts.nodes, in->parts.size);
      }
      case PM_INTERPOLATED_SYMBOL_NODE: {     /* :"...#{ }..." → dstr.to_sym */
        const pm_interpolated_symbol_node_t *in = (const pm_interpolated_symbol_node_t *)node;
        uint32_t to_sym = korb_intern(tc->c->vm, "to_sym", 6);
        NODE *str;
        WITH_CHAIN(tc, 1, (str = build_dstr(tc, in->parts.nodes, in->parts.size)));
        return kp_send0(to_sym, kp_line(tc, node), str);
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
      case PM_IT_LOCAL_VARIABLE_READ_NODE:    /* `it` — the block's single implicit param (slot 0) */
        return bake_lget(tc, 0);
      case PM_LOCAL_VARIABLE_WRITE_NODE: {
        const pm_local_variable_write_node_t *lw = (const pm_local_variable_write_node_t *)node;
        NODE *rval = transduce(tc, lw->value);   /* register child: no staging */
        return lvar_write(tc, node, lw->name, lw->depth, rval);
      }
      case PM_MULTI_WRITE_NODE: {
        /* pre..., [*splat,] post... = rhs  (depth-0 local targets) */
        const pm_multi_write_node_t *mw = (const pm_multi_write_node_t *)node;
        /* a depth-0 local target; returns its name cid or fails (unsupported) */
        #define MW_LOCAL_CID(t, outcid)                                         \
            do {                                                                \
                if (!PM_NODE_TYPE_P((t), PM_LOCAL_VARIABLE_TARGET_NODE))        \
                    return kp_unsupported(tc, (t), "non-local multi-assign target"); \
                if (((const pm_local_variable_target_node_t *)(t))->depth != 0) \
                    return kp_unsupported(tc, (t), "outer-scope multi-assign target"); \
                (outcid) = ((const pm_local_variable_target_node_t *)(t))->name; \
            } while (0)

        const bool has_splat = mw->rest && PM_NODE_TYPE_P(mw->rest, PM_SPLAT_NODE);
        /* `a, b, = rhs` (trailing comma → implicit rest) just discards extra rhs
         * elements, which the non-splat path already does — treat as no rest. */
        if (mw->rest && !has_splat && !PM_NODE_TYPE_P(mw->rest, PM_IMPLICIT_REST_NODE))
            return kp_unsupported(tc, node, "multi-assign with anonymous rest");

        if (!has_splat) {
            uint32_t nt = (uint32_t)mw->lefts.size;
            /* classify: all depth-0 locals → node_massign (fast); a mix of
             * local / @ivar / CONST → node_massign_het; a call/attribute target
             * (recv.x=) → general synth-temp desugar; anything else → no. */
            bool all_local = true, needs_general = false;
            for (uint32_t i = 0; i < nt; i++) {
                const pm_node_t *t = mw->lefts.nodes[i];
                if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                    if (((const pm_local_variable_target_node_t *)t)->depth != 0)
                        return kp_unsupported(tc, t, "outer-scope multi-assign target");
                } else if (PM_NODE_TYPE_P(t, PM_INSTANCE_VARIABLE_TARGET_NODE) ||
                           PM_NODE_TYPE_P(t, PM_CONSTANT_TARGET_NODE)) {
                    all_local = false;
                } else if (PM_NODE_TYPE_P(t, PM_CALL_TARGET_NODE)) {
                    all_local = false; needs_general = true;
                } else {
                    return kp_unsupported(tc, t, "non-local multi-assign target");
                }
            }
            if (needs_general) {
                /* t = rhs; target_i = t[i] ...; → result is the rhs array. */
                uint32_t tmp = alloc_synth_local(tc);
                uint32_t aref = korb_intern(tc->c->vm, "[]", 2);
                NODE *acc = bake_lset(tc, tmp, transduce(tc, mw->value));
                for (uint32_t i = 0; i < nt; i++) {
                    const pm_node_t *t = mw->lefts.nodes[i];
                    uint32_t line = kp_line(tc, t);
                    NODE *assign;
                    if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                        pm_constant_id_t cid = ((const pm_local_variable_target_node_t *)t)->name;
                        assign = lvar_write(tc, t, cid, 0, mw_index_get(tc, tmp, i, aref, line));
                    } else if (PM_NODE_TYPE_P(t, PM_INSTANCE_VARIABLE_TARGET_NODE)) {
                        uint32_t name = kp_intern_cid(tc, ((const pm_instance_variable_target_node_t *)t)->name);
                        assign = ALLOC_node_ivar_set(-1 - tc->chain, name, mw_index_get(tc, tmp, i, aref, line));
                    } else if (PM_NODE_TYPE_P(t, PM_CONSTANT_TARGET_NODE)) {
                        uint32_t name = kp_intern_cid(tc, ((const pm_constant_target_node_t *)t)->name);
                        NODE *v; uint32_t sc = kind_node_const_set.slot_count;
                        WITH_CHAIN(tc, sc, (v = mw_index_get(tc, tmp, i, aref, line)));
                        assign = ALLOC_node_const_set(name, v);
                    } else {   /* PM_CALL_TARGET_NODE: recv.setter=(t[i]) */
                        const pm_call_target_node_t *ct = (const pm_call_target_node_t *)t;
                        uint32_t setter = kp_intern_cid(tc, ct->name);
                        NODE *r, *v;
                        WITH_CHAIN(tc, 2, (r = transduce(tc, ct->receiver),
                                           v = mw_index_get(tc, tmp, i, aref, line)));
                        assign = kp_send1(setter, line, r, v);
                    }
                    acc = ALLOC_node_seq(acc, assign);
                }
                return ALLOC_node_seq(acc, bake_lget(tc, tmp));   /* massign value = rhs */
            }
            if (all_local) {
                int32_t *offs = malloc(sizeof(int32_t) * (nt ? nt : 1));
                if (!offs) abort();
                pm_constant_id_t cid;
                NODE *rhs = transduce(tc, mw->value);             /* register child */
                for (uint32_t i = 0; i < nt; i++) {
                    MW_LOCAL_CID(mw->lefts.nodes[i], cid);
                    offs[i] = (int32_t)lvar_index(tc, mw->lefts.nodes[i], cid) - tc->chain;
                }
                NODE *mn = ALLOC_node_massign(offs, nt, rhs);
                for (uint32_t i = 0; i < nt; i++) bake_add(tc, &offs[i]);
                return mn;
            }
            /* heterogeneous: {kind, data} per target (matches node_massign_het) */
            struct kp_mdesc { int32_t kind; int32_t data; };
            struct kp_mdesc *descs = malloc(sizeof(*descs) * (nt ? nt : 1));
            if (!descs) abort();
            NODE *rhs = transduce(tc, mw->value);                 /* register child */
            for (uint32_t i = 0; i < nt; i++) {
                const pm_node_t *t = mw->lefts.nodes[i];
                if (PM_NODE_TYPE_P(t, PM_LOCAL_VARIABLE_TARGET_NODE)) {
                    pm_constant_id_t cid = ((const pm_local_variable_target_node_t *)t)->name;
                    descs[i].kind = 0;
                    descs[i].data = (int32_t)lvar_index(tc, t, cid) - tc->chain;
                } else if (PM_NODE_TYPE_P(t, PM_INSTANCE_VARIABLE_TARGET_NODE)) {
                    descs[i].kind = 1;
                    descs[i].data = (int32_t)kp_intern_cid(tc, ((const pm_instance_variable_target_node_t *)t)->name);
                } else {       /* PM_CONSTANT_TARGET_NODE */
                    descs[i].kind = 2;
                    descs[i].data = (int32_t)kp_intern_cid(tc, ((const pm_constant_target_node_t *)t)->name);
                }
            }
            NODE *mn = ALLOC_node_massign_het(descs, nt, -1 - tc->chain, rhs);
            for (uint32_t i = 0; i < nt; i++) if (descs[i].kind == 0) bake_add(tc, &descs[i].data);
            return mn;
        }

        /* splat path: lefts(npre) | *splat | rights(npost) */
        const pm_splat_node_t *sp = (const pm_splat_node_t *)mw->rest;
        if (!sp->expression || !PM_NODE_TYPE_P(sp->expression, PM_LOCAL_VARIABLE_TARGET_NODE))
            return kp_unsupported(tc, node, "anonymous/non-local splat target");
        uint32_t npre = (uint32_t)mw->lefts.size, npost = (uint32_t)mw->rights.size;
        uint32_t no = npre + 1u + npost;
        int32_t *offs = malloc(sizeof(int32_t) * no);
        if (!offs) abort();
        pm_constant_id_t cid;
        NODE *rhs = transduce(tc, mw->value);
        for (uint32_t i = 0; i < npre; i++) {
            MW_LOCAL_CID(mw->lefts.nodes[i], cid);
            offs[i] = (int32_t)lvar_index(tc, mw->lefts.nodes[i], cid) - tc->chain;
        }
        MW_LOCAL_CID(sp->expression, cid);
        offs[npre] = (int32_t)lvar_index(tc, sp->expression, cid) - tc->chain;
        for (uint32_t i = 0; i < npost; i++) {
            MW_LOCAL_CID(mw->rights.nodes[i], cid);
            offs[npre + 1u + i] = (int32_t)lvar_index(tc, mw->rights.nodes[i], cid) - tc->chain;
        }
        NODE *mn = ALLOC_node_massign_splat(offs, npre, npost, rhs);
        for (uint32_t i = 0; i < no; i++) bake_add(tc, &offs[i]);
        return mn;
        #undef MW_LOCAL_CID
      }
      case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        /* x op= v  →  write(x, binop(read(x), v)) */
        const pm_local_variable_operator_write_node_t *ow =
            (const pm_local_variable_operator_write_node_t *)node;
        const char *opname = kp_cid_cstr(tc, ow->binary_operator);
        enum kp_binop op = kp_binop_kind(opname);
        uint32_t opmid = kp_intern_cid(tc, ow->binary_operator);
        uint32_t line = kp_line(tc, node);
        NODE *lhs, *rhs, *comb;
        if (op != KP_BINOP_NONE) {
            WITH_CHAIN(tc, kind_node_plus.slot_count, (lhs = lvar_read(tc, node, ow->name, ow->depth),
                                                       rhs = transduce(tc, ow->value)));
            comb = alloc_binop(op, lhs, rhs, line);
        } else {   /* &= |= ^= <<= >>= → method send */
            WITH_CHAIN(tc, KP_SEND1_SC, (lhs = lvar_read(tc, node, ow->name, ow->depth),
                                                        rhs = transduce(tc, ow->value)));
            comb = kp_send1(opmid, line, lhs, rhs);
        }
        return lvar_write(tc, node, ow->name, ow->depth, comb);
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
      case PM_INDEX_OR_WRITE_NODE: {        /* recv[key] ||= value (single index arg) */
        const pm_index_or_write_node_t *iw = (const pm_index_or_write_node_t *)node;
        size_t argc = iw->arguments ? iw->arguments->arguments.size : 0;
        if (iw->block || argc != 1)
            return kp_unsupported(tc, node, "index ||= with block or multiple index args");
        uint32_t aref = korb_intern(tc->c->vm, "[]", 2);
        uint32_t aset = korb_intern(tc->c->vm, "[]=", 3);
        uint32_t line = kp_line(tc, node);
        uint32_t t0 = alloc_synth_local(tc), t1 = alloc_synth_local(tc);
        /* evaluate recv + key once into temps (single-eval semantics) */
        NODE *store_recv = bake_lset(tc, t0, transduce(tc, iw->receiver));
        NODE *store_key  = bake_lset(tc, t1, transduce(tc, iw->arguments->arguments.nodes[0]));
        NODE *g_recv, *g_key;
        WITH_CHAIN(tc, 2, (g_recv = bake_lget(tc, t0), g_key = bake_lget(tc, t1)));
        NODE *get = kp_send1(aref, line, g_recv, g_key);
        NODE *s_recv, *s_key, *s_val;
        WITH_CHAIN(tc, 3, (s_recv = bake_lget(tc, t0), s_key = bake_lget(tc, t1),
                           s_val  = transduce(tc, iw->value)));
        NODE *set = kp_send2(aset, line, s_recv, s_key, s_val);
        return ALLOC_node_seq(store_recv, ALLOC_node_seq(store_key, ALLOC_node_or(get, set)));
      }
      case PM_INDEX_OPERATOR_WRITE_NODE: {  /* recv[key] op= value  →  recv[key] = recv[key] op value */
        const pm_index_operator_write_node_t *iw = (const pm_index_operator_write_node_t *)node;
        size_t argc = iw->arguments ? iw->arguments->arguments.size : 0;
        if (iw->block || argc != 1)
            return kp_unsupported(tc, node, "index op= with block or multiple index args");
        uint32_t aref = korb_intern(tc->c->vm, "[]", 2);
        uint32_t aset = korb_intern(tc->c->vm, "[]=", 3);
        enum kp_binop op = kp_binop_kind(kp_cid_cstr(tc, iw->binary_operator));
        uint32_t opmid = kp_intern_cid(tc, iw->binary_operator);
        uint32_t line = kp_line(tc, node);
        uint32_t t0 = alloc_synth_local(tc), t1 = alloc_synth_local(tc);
        /* evaluate recv + key once into temps (single-eval semantics) */
        NODE *store_recv = bake_lset(tc, t0, transduce(tc, iw->receiver));
        NODE *store_key  = bake_lset(tc, t1, transduce(tc, iw->arguments->arguments.nodes[0]));
        /* set(recv, key, (recv[key]) op value), nesting chains: set → binop → get */
        NODE *s_recv, *s_key, *newval;
        const uint32_t bsc = (op != KP_BINOP_NONE) ? kind_node_plus.slot_count : KP_SEND1_SC;
        WITH_CHAIN(tc, 3, (
            s_recv = bake_lget(tc, t0),
            s_key  = bake_lget(tc, t1),
            newval = WITH_CHAIN(tc, bsc, ({
                NODE *g_recv, *g_key, *get, *val;
                WITH_CHAIN(tc, 2, (g_recv = bake_lget(tc, t0), g_key = bake_lget(tc, t1)));
                get = kp_send1(aref, line, g_recv, g_key);
                val = transduce(tc, iw->value);
                (op != KP_BINOP_NONE) ? alloc_binop(op, get, val, line) : kp_send1(opmid, line, get, val);
            }))
        ));
        NODE *set = kp_send2(aset, line, s_recv, s_key, newval);
        return ALLOC_node_seq(store_recv, ALLOC_node_seq(store_key, set));
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
      case PM_CASE_NODE: {
        /* `case subj; when a, b; body; ...; else e; end` desugared to an if/elsif
         * chain.  With a subject, each `when v` tests `v === subj`, the subject
         * evaluated once into a synthetic temp.  Without a subject, each `when c`
         * is a plain truthiness test. */
        const pm_case_node_t *cn = (const pm_case_node_t *)node;
        const bool has_subj = cn->predicate != NULL;
        NODE *assign = NULL;
        uint32_t tmp = 0;
        if (has_subj) {
            tmp = alloc_synth_local(tc);
            NODE *subj = transduce(tc, cn->predicate);   /* register child: no staging */
            assign = bake_lset(tc, tmp, subj);
        }
        NODE *chain = cn->else_clause
            ? transduce_statements(tc, cn->else_clause->statements)
            : lit_nil();
        for (size_t wi = cn->conditions.size; wi-- > 0; ) {  /* fold last → first */
            const pm_when_node_t *wn = (const pm_when_node_t *)cn->conditions.nodes[wi];
            NODE *body = transduce_statements(tc, wn->statements);
            NODE *cond = NULL;
            for (size_t ci = 0; ci < wn->conditions.size; ci++) {
                const pm_node_t *cv = wn->conditions.nodes[ci];
                if (PM_NODE_TYPE_P(cv, PM_SPLAT_NODE))
                    return kp_unsupported(tc, cv, "when with splat");
                NODE *test;
                if (has_subj) {
                    NODE *vn, *targ; (void)kp_line(tc, cv);
                    WITH_CHAIN(tc, kind_node_caseeq.slot_count, (vn = transduce(tc, cv), targ = bake_lget(tc, tmp)));
                    test = ALLOC_node_caseeq(vn, targ);     /* v === subj (inline for value-eq types) */
                } else {
                    test = transduce(tc, cv);                       /* plain truthiness */
                }
                cond = cond ? ALLOC_node_or(cond, test) : test;
            }
            if (cond == NULL) cond = lit_nil();
            chain = ALLOC_node_if(cond, body, chain);
        }
        return has_subj ? ALLOC_node_seq(assign, chain) : chain;
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
        uint32_t post = PM_NODE_FLAG_P(wn, PM_LOOP_FLAGS_BEGIN_MODIFIER) ? 1u : 0u;
        NODE *cond = transduce(tc, wn->predicate);
        NODE *body = transduce_statements(tc, wn->statements);
        return ALLOC_node_while(cond, body, 0, post);
      }
      case PM_UNTIL_NODE: {
        const pm_until_node_t *un = (const pm_until_node_t *)node;
        uint32_t post = PM_NODE_FLAG_P(un, PM_LOOP_FLAGS_BEGIN_MODIFIER) ? 1u : 0u;
        NODE *cond = transduce(tc, un->predicate);
        NODE *body = transduce_statements(tc, un->statements);
        return ALLOC_node_while(cond, body, 1, post);
      }
      case PM_LAMBDA_NODE: {   /* ->(args) { body } — a lambda Proc */
        const pm_lambda_node_t *ln = (const pm_lambda_node_t *)node;
        NODE *entry = transduce_block_parts(tc, &ln->locals, ln->parameters, ln->body);
        int32_t self_off = -1 - tc->chain;
        NODE *mk = ALLOC_node_make_proc(entry, -tc->chain, self_off, 1u);
        bake_add(tc, &mk->u.node_make_proc.def_env_off);
        return mk;
      }
      case PM_FOR_NODE: {
        /* `for VAR in COLL; BODY; end` — VAR + BODY share the enclosing frame
         * (for introduces no scope), so BODY is transduced here, not in a block. */
        const pm_for_node_t *fn = (const pm_for_node_t *)node;
        if (!PM_NODE_TYPE_P(fn->index, PM_LOCAL_VARIABLE_TARGET_NODE))
            return kp_unsupported(tc, fn->index, "for-loop with non-local / destructured index");
        const pm_local_variable_target_node_t *iv = (const pm_local_variable_target_node_t *)fn->index;
        if (iv->depth != 0)
            return kp_unsupported(tc, fn->index, "for-loop with outer-scope index");
        uint32_t orig_local = alloc_synth_local(tc);
        uint32_t iter_local = alloc_synth_local(tc);
        NODE *coll = transduce(tc, fn->collection);
        NODE *body = fn->statements ? transduce_statements(tc, fn->statements) : lit_nil();
        NODE *fnode = ALLOC_node_for((int32_t)orig_local - tc->chain,
                                     (int32_t)iter_local - tc->chain,
                                     (int32_t)lvar_index(tc, fn->index, iv->name) - tc->chain,
                                     coll, body);
        bake_add(tc, &fnode->u.node_for.orig_off);
        bake_add(tc, &fnode->u.node_for.iter_off);
        bake_add(tc, &fnode->u.node_for.var_off);
        return fnode;
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
            /* `return a, b, ...` → return the values as an Array. */
            size_t cnt = rn->arguments->arguments.size;
            v = build_array(tc, rn->arguments->arguments.nodes, cnt, (uint32_t)cnt);
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
            /* reserved cells: block_entry(fs-5), def_env(fs-4), captured_self(fs-3) */
            return ALLOC_node_yield0(line, -5 - tc->chain, -4 - tc->chain, -3 - tc->chain);
        }
        if (yargc == 1) {
            /* yield1 slot_count 1: a0 at cursor[-1]; reserved cells below */
            NODE *a0;
            WITH_CHAIN(tc, 1, (a0 = transduce(tc, yn->arguments->arguments.nodes[0])));
            return ALLOC_node_yield1(line, -5 - (tc->chain + 1), -4 - (tc->chain + 1), -3 - (tc->chain + 1), a0);
        }
        return kp_unsupported(tc, node, "yield with more than 1 value");
      }
      case PM_NEXT_NODE: {
        const pm_next_node_t *nn = (const pm_next_node_t *)node;
        NODE *v;
        if (nn->arguments == NULL || nn->arguments->arguments.size == 0) v = lit_nil();
        else if (nn->arguments->arguments.size == 1) v = transduce(tc, nn->arguments->arguments.nodes[0]);
        else { size_t cnt = nn->arguments->arguments.size; v = build_array(tc, nn->arguments->arguments.nodes, cnt, (uint32_t)cnt); }  /* `next a, b` → [a, b] */
        return ALLOC_node_next(v);
      }
      case PM_BREAK_NODE: {
        const pm_break_node_t *bn = (const pm_break_node_t *)node;
        NODE *v;
        if (bn->arguments == NULL || bn->arguments->arguments.size == 0) v = lit_nil();
        else if (bn->arguments->arguments.size == 1) v = transduce(tc, bn->arguments->arguments.nodes[0]);
        else { size_t cnt = bn->arguments->arguments.size; v = build_array(tc, bn->arguments->arguments.nodes, cnt, (uint32_t)cnt); }  /* `break a, b` → [a, b] */
        return ALLOC_node_break(v);
      }
      case PM_RETRY_NODE:                               /* `retry` in a rescue → re-run the begin body */
        return ALLOC_node_retry();

      /* ---- calls / def ---- */
      case PM_CALL_NODE:
        return transduce_call(tc, (const pm_call_node_t *)node);
      case PM_DEF_NODE:
        return transduce_def(tc, (const pm_def_node_t *)node);
      case PM_CLASS_NODE:
        return transduce_class(tc, (const pm_class_node_t *)node);
      case PM_MODULE_NODE:
        return transduce_module(tc, (const pm_module_node_t *)node);
      case PM_CONSTANT_READ_NODE: {
        const pm_constant_read_node_t *cr = (const pm_constant_read_node_t *)node;
        return ALLOC_node_const(kp_intern_cid(tc, cr->name));
      }
      case PM_CONSTANT_PATH_NODE: {       /* `A::B` — koruby's const table is flat,
                                           * so resolve the rightmost name; the
                                           * namespace parent is a pure path. */
        const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)node;
        if (cp->parent != NULL &&
            !PM_NODE_TYPE_P(cp->parent, PM_CONSTANT_READ_NODE) &&
            !PM_NODE_TYPE_P(cp->parent, PM_CONSTANT_PATH_NODE))
            return kp_unsupported(tc, node, "constant path with a non-namespace parent");
        return ALLOC_node_const(kp_intern_cid(tc, cp->name));
      }

      /* Global variables `$x` reuse the flat const table — the `$` in the name
       * keeps them in a distinct namespace from constants.  Unset reads → nil. */
      case PM_GLOBAL_VARIABLE_READ_NODE:
        return ALLOC_node_const(kp_intern_cid(tc, ((const pm_global_variable_read_node_t *)node)->name));
      case PM_GLOBAL_VARIABLE_WRITE_NODE: {
        const pm_global_variable_write_node_t *gw = (const pm_global_variable_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, gw->name);
        NODE *val;
        WITH_CHAIN(tc, kind_node_const_set.slot_count, (val = transduce(tc, gw->value)));
        return ALLOC_node_const_set(name, val);
      }
      case PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE: {     /* `$x op= v` */
        const pm_global_variable_operator_write_node_t *gw =
            (const pm_global_variable_operator_write_node_t *)node;
        enum kp_binop op = kp_binop_kind(kp_cid_cstr(tc, gw->binary_operator));
        if (op == KP_BINOP_NONE) return kp_unsupported(tc, node, "global operator-assign");
        uint32_t name = kp_intern_cid(tc, gw->name), line = kp_line(tc, node);
        NODE *binop = WITH_CHAIN(tc, kind_node_const_set.slot_count, ({
            NODE *lhs, *rhs;
            WITH_CHAIN(tc, kind_node_plus.slot_count,
                       (lhs = ALLOC_node_const(name), rhs = transduce(tc, gw->value)));
            alloc_binop(op, lhs, rhs, line);
        }));
        return ALLOC_node_const_set(name, binop);
      }

      case PM_CONSTANT_WRITE_NODE: {     /* `FOO = expr` → VM const table */
        const pm_constant_write_node_t *cw = (const pm_constant_write_node_t *)node;
        uint32_t name = kp_intern_cid(tc, cw->name);
        NODE *val;
        uint32_t sc = kind_node_const_set.slot_count;
        WITH_CHAIN(tc, sc, (val = transduce(tc, cw->value)));
        return ALLOC_node_const_set(name, val);
      }

      case PM_RESCUE_MODIFIER_NODE: {   /* `expr rescue fallback` (catch-all) */
        const pm_rescue_modifier_node_t *rm = (const pm_rescue_modifier_node_t *)node;
        NODE *body = transduce(tc, rm->expression);
        NODE *rescue_class = ALLOC_node_const(korb_intern(tc->c->vm, "StandardError", 13));
        NODE *resc = transduce(tc, rm->rescue_expression);
        return ALLOC_node_begin(body, rescue_class, resc, lit_nil(), 0, 1u);   /* catch StandardError */
      }

      case PM_SUPER_NODE: {           /* super(...) — explicit args */
        const pm_super_node_t *sn = (const pm_super_node_t *)node;
        if (sn->block) return kp_unsupported(tc, node, "super with a block");
        uint32_t m_mid = tc->frame->method_mid;
        if (m_mid == 0) return kp_unsupported(tc, node, "super outside a method body");
        uint32_t line = kp_line(tc, node);
        const pm_arguments_node_t *args = sn->arguments;
        size_t argc = args ? args->arguments.size : 0;
        if (argc > 3) return kp_unsupported(tc, node, "super with more than 3 arguments");
        int32_t soff = -1 - tc->chain - (int32_t)argc, dco = -2 - tc->chain - (int32_t)argc;
        NODE *a[3];
        switch (argc) {
          case 0: return ALLOC_node_super0(m_mid, line, soff, dco);
          case 1: WITH_CHAIN(tc, 1, (a[0] = transduce(tc, args->arguments.nodes[0])));
                  return ALLOC_node_super1(m_mid, line, soff, dco, a[0]);
          case 2: WITH_CHAIN(tc, 2, (a[0] = transduce(tc, args->arguments.nodes[0]),
                                     a[1] = transduce(tc, args->arguments.nodes[1])));
                  return ALLOC_node_super2(m_mid, line, soff, dco, a[0], a[1]);
          default: WITH_CHAIN(tc, 3, (a[0] = transduce(tc, args->arguments.nodes[0]),
                                      a[1] = transduce(tc, args->arguments.nodes[1]),
                                      a[2] = transduce(tc, args->arguments.nodes[2])));
                  return ALLOC_node_super3(m_mid, line, soff, dco, a[0], a[1], a[2]);
        }
      }
      case PM_FORWARDING_SUPER_NODE: {   /* bare super — forward the method's params */
        const pm_forwarding_super_node_t *fn = (const pm_forwarding_super_node_t *)node;
        if (fn->block) return kp_unsupported(tc, node, "super with a block");
        uint32_t m_mid = tc->frame->method_mid;
        if (m_mid == 0) return kp_unsupported(tc, node, "super outside a method body");
        uint32_t line = kp_line(tc, node);
        uint32_t np = tc->frame->method_params;
        if (np > 3) return kp_unsupported(tc, node, "forwarding super with more than 3 params");
        int32_t soff = -1 - tc->chain - (int32_t)np, dco = -2 - tc->chain - (int32_t)np;
        NODE *a[3];
        switch (np) {
          case 0: return ALLOC_node_super0(m_mid, line, soff, dco);
          case 1: WITH_CHAIN(tc, 1, (a[0] = bake_lget(tc, 0)));
                  return ALLOC_node_super1(m_mid, line, soff, dco, a[0]);
          case 2: WITH_CHAIN(tc, 2, (a[0] = bake_lget(tc, 0), a[1] = bake_lget(tc, 1)));
                  return ALLOC_node_super2(m_mid, line, soff, dco, a[0], a[1]);
          default: WITH_CHAIN(tc, 3, (a[0] = bake_lget(tc, 0), a[1] = bake_lget(tc, 1), a[2] = bake_lget(tc, 2)));
                  return ALLOC_node_super3(m_mid, line, soff, dco, a[0], a[1], a[2]);
        }
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
