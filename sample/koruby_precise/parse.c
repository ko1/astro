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
    f->prev = tc->frame;
    tc->frame = f;
    tc->chain = 0;
}

static void
pop_frame(struct kp_ctx *tc)
{
    struct kp_frame *f = tc->frame;
    int32_t locals_cnt = (int32_t)f->locals->size;
    for (uint32_t i = f->bake_base; i < tc->bake_cnt; i++) {
        *tc->bake_list[i] -= locals_cnt;
    }
    tc->bake_cnt = f->bake_base;
    tc->chain = f->saved_chain;
    tc->frame = f->prev;
    free(f);
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

extern const struct NodeKind kind_node_plus;   /* all binops share slot_count */

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

static NODE *
transduce_func_call(struct kp_ctx *tc, const pm_call_node_t *cn)
{
    uint32_t mid = kp_intern_cid(tc, cn->name);
    uint32_t line = kp_line(tc, (const pm_node_t *)cn);
    const pm_arguments_node_t *args = cn->arguments;
    size_t argc = args ? args->arguments.size : 0;

    if (cn->block) return kp_unsupported(tc, (const pm_node_t *)cn, "block argument");

    NODE *a[3];
    switch (argc) {
      case 0:
        return ALLOC_node_call0(mid, line);
      case 1:
        WITH_CHAIN(tc, 1, (a[0] = transduce(tc, args->arguments.nodes[0])));
        return ALLOC_node_call1(mid, line, a[0]);
      case 2:
        WITH_CHAIN(tc, 2, (a[0] = transduce(tc, args->arguments.nodes[0]),
                           a[1] = transduce(tc, args->arguments.nodes[1])));
        return ALLOC_node_call2(mid, line, a[0], a[1]);
      case 3:
        WITH_CHAIN(tc, 3, (a[0] = transduce(tc, args->arguments.nodes[0]),
                           a[1] = transduce(tc, args->arguments.nodes[1]),
                           a[2] = transduce(tc, args->arguments.nodes[2])));
        return ALLOC_node_call3(mid, line, a[0], a[1], a[2]);
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
    uint32_t line = kp_line(tc, (const pm_node_t *)cn);
    size_t argc = cn->arguments ? cn->arguments->arguments.size : 0;
    if (cn->block) return kp_unsupported(tc, (const pm_node_t *)cn, "block argument");

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

    char what[128];
    snprintf(what, sizeof(what), "receiver method call '%s'", name);
    return kp_unsupported(tc, (const pm_node_t *)cn, strdup(what));
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

    uint32_t locals_cnt = (uint32_t)dn->locals.size;
    pop_frame(tc);

    uint32_t mid = kp_intern_cid(tc, dn->name);
    NODE *def = ALLOC_node_def(mid, body, params_cnt, locals_cnt);

    /* Every method body is its own AOT entry: call sites reach it through
     * body->head.dispatcher at runtime (specializer can't fold that). */
    code_repo_add(korb_sym_name(tc->c->vm, mid), body, true);
    return def;
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
        pop_frame(tc);
        koruby_toplevel_locals_cnt = (uint32_t)pn->locals.size;
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

      /* ---- locals ---- */
      case PM_LOCAL_VARIABLE_READ_NODE: {
        const pm_local_variable_read_node_t *lr = (const pm_local_variable_read_node_t *)node;
        if (lr->depth != 0) return kp_unsupported(tc, node, "closure local (depth > 0)");
        return bake_lget(tc, lvar_index(tc, node, lr->name));
      }
      case PM_LOCAL_VARIABLE_WRITE_NODE: {
        const pm_local_variable_write_node_t *lw = (const pm_local_variable_write_node_t *)node;
        if (lw->depth != 0) return kp_unsupported(tc, node, "closure local (depth > 0)");
        NODE *rval = transduce(tc, lw->value);   /* register child: no staging */
        return bake_lset(tc, lvar_index(tc, node, lw->name), rval);
      }
      case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        /* x op= v  →  lset(x, binop(lget(x), v)) */
        const pm_local_variable_operator_write_node_t *ow =
            (const pm_local_variable_operator_write_node_t *)node;
        if (ow->depth != 0) return kp_unsupported(tc, node, "closure local (depth > 0)");
        uint32_t idx = lvar_index(tc, node, ow->name);
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
        WITH_CHAIN(tc, n_slots, (lhs = bake_lget(tc, idx),
                                 rhs = transduce(tc, ow->value)));
        return bake_lset(tc, idx, alloc_binop(op, lhs, rhs, line));
      }
      case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
        const pm_local_variable_and_write_node_t *aw =
            (const pm_local_variable_and_write_node_t *)node;
        if (aw->depth != 0) return kp_unsupported(tc, node, "closure local (depth > 0)");
        uint32_t idx = lvar_index(tc, node, aw->name);
        return ALLOC_node_and(bake_lget(tc, idx),
                              bake_lset(tc, idx, transduce(tc, aw->value)));
      }
      case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
        const pm_local_variable_or_write_node_t *ow =
            (const pm_local_variable_or_write_node_t *)node;
        if (ow->depth != 0) return kp_unsupported(tc, node, "closure local (depth > 0)");
        uint32_t idx = lvar_index(tc, node, ow->name);
        return ALLOC_node_or(bake_lget(tc, idx),
                             bake_lset(tc, idx, transduce(tc, ow->value)));
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

      /* ---- calls / def ---- */
      case PM_CALL_NODE:
        return transduce_call(tc, (const pm_call_node_t *)node);
      case PM_DEF_NODE:
        return transduce_def(tc, (const pm_def_node_t *)node);

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
