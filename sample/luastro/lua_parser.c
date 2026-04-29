// luastro parser — Lua 5.4 recursive-descent + Pratt expression parser.
//
// Produces NODE trees consumable by EVAL.  Identifiers are resolved
// to local / upval / global; captured locals are recorded so that
// node_make_closure can allocate heap cells for them.
//
// Variable-arity nodes (calls, table constructors, multi-assign, etc.)
// store their data in module-level side arrays
//   LUASTRO_NODE_ARR[]  for NODE *
//   LUASTRO_U32_ARR[]   for uint32_t
// and the parser registers each block via reg_node_arr / reg_u32_arr,
// receiving back the index that becomes a uint32_t operand.

#include <ctype.h>
#include "context.h"
#include "node.h"
#include "lua_token.h"

// kind_* are defined (with external linkage) in node_alloc.c, which is
// compiled as part of node.c.  Declare those we compare against here.
extern const struct NodeKind
    kind_node_local_get,  kind_node_upval_get, kind_node_global_get,
    kind_node_field_get,  kind_node_index_get,
    kind_node_call,       kind_node_method_call,
    kind_node_call_arg0,  kind_node_call_arg1, kind_node_call_arg2, kind_node_call_arg3,
    kind_node_local_set,  kind_node_box_get,   kind_node_box_set,
    kind_node_int_add, kind_node_int_sub, kind_node_int_mul;

// --- Side-array storage --------------------------------------------

NODE     **LUASTRO_NODE_ARR     = NULL;
uint32_t   LUASTRO_NODE_ARR_CNT = 0;
static uint32_t LUASTRO_NODE_ARR_CAP = 0;

uint32_t  *LUASTRO_U32_ARR      = NULL;
uint32_t   LUASTRO_U32_ARR_CNT  = 0;
static uint32_t LUASTRO_U32_ARR_CAP  = 0;

static uint32_t
reg_node_arr(NODE * const *nodes, uint32_t cnt)
{
    if (LUASTRO_NODE_ARR_CNT + cnt > LUASTRO_NODE_ARR_CAP) {
        uint32_t cap = LUASTRO_NODE_ARR_CAP ? LUASTRO_NODE_ARR_CAP : 64;
        while (cap < LUASTRO_NODE_ARR_CNT + cnt) cap *= 2;
        LUASTRO_NODE_ARR = (NODE **)realloc(LUASTRO_NODE_ARR, cap * sizeof(NODE *));
        LUASTRO_NODE_ARR_CAP = cap;
    }
    uint32_t base = LUASTRO_NODE_ARR_CNT;
    for (uint32_t i = 0; i < cnt; i++) LUASTRO_NODE_ARR[base + i] = nodes[i];
    LUASTRO_NODE_ARR_CNT += cnt;
    return base;
}

static uint32_t
reg_u32_arr(const uint32_t *vals, uint32_t cnt)
{
    if (LUASTRO_U32_ARR_CNT + cnt > LUASTRO_U32_ARR_CAP) {
        uint32_t cap = LUASTRO_U32_ARR_CAP ? LUASTRO_U32_ARR_CAP : 64;
        while (cap < LUASTRO_U32_ARR_CNT + cnt) cap *= 2;
        LUASTRO_U32_ARR = (uint32_t *)realloc(LUASTRO_U32_ARR, cap * sizeof(uint32_t));
        LUASTRO_U32_ARR_CAP = cap;
    }
    uint32_t base = LUASTRO_U32_ARR_CNT;
    for (uint32_t i = 0; i < cnt; i++) LUASTRO_U32_ARR[base + i] = vals[i];
    LUASTRO_U32_ARR_CNT += cnt;
    return base;
}

// --- Compile-time function context ---------------------------------

typedef struct LocalSlot {
    struct LuaString *name;
    uint32_t idx;
    bool     is_captured;
} LocalSlot;

typedef struct UpvalEntry {
    struct LuaString *name;
    bool     from_local;
    uint32_t src_idx;
} UpvalEntry;

#define LP_MAX_LOCALS 256
#define LP_MAX_UPVALS 256

typedef struct ParseScope {
    uint32_t local_base;
    bool     is_loop;
    struct ParseScope *parent;
} ParseScope;

// Track every local-access node we emit so that, once parsing of a
// function body is complete and we know which locals were captured by
// nested closures, we can rewrite the references from node_local_*
// (direct slot) to node_box_* (heap cell).
typedef struct LocalRef {
    NODE *node;
    uint32_t slot_idx;
} LocalRef;
#define LP_MAX_LOCAL_REFS  4096

typedef struct ParseFunc {
    LocalSlot  locals [LP_MAX_LOCALS];
    uint32_t   nlocals;
    UpvalEntry upvals [LP_MAX_UPVALS];
    uint32_t   nupvals;
    uint32_t   nparams;
    bool       is_vararg;
    const char *name;
    ParseScope *scope;
    struct ParseFunc *parent;

    LocalRef   local_refs[LP_MAX_LOCAL_REFS];
    uint32_t   nlocal_refs;
} ParseFunc;

static ParseFunc *PF_CURRENT = NULL;

// Forward decls
static NODE *parse_chunk(void);
static NODE *parse_block(void);
static NODE *parse_stat(void);
static NODE *parse_expr(void);
static NODE *parse_expr_prec(int min_prec);
static NODE *parse_simple_exp(void);
static NODE *parse_suffixed_exp(void);
static NODE *parse_primary_exp(void);
static NODE *parse_table_constructor(void);
static NODE *parse_function_body(bool is_method, struct LuaString *name);
static NODE *parse_args(NODE *callee, bool is_method, struct LuaString *method_name);
static NODE *resolve_name_to_node(struct LuaString *name);
static NODE *resolve_name_to_set(struct LuaString *name, NODE *rhs);

static const Token *cur(void) { return lua_tok_cur(); }

static bool
accept(ltok_t k)
{
    if (cur()->kind == k) { lua_tok_next(); return true; }
    return false;
}

static void
expect(ltok_t k, const char *what)
{
    if (cur()->kind != k) {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected %s", what);
        lua_tok_error(buf);
    }
    lua_tok_next();
}

static struct LuaString *
expect_ident(void)
{
    if (cur()->kind != LT_IDENT) lua_tok_error("expected identifier");
    struct LuaString *n = lua_str_intern_n(cur()->start, cur()->len);
    lua_tok_next();
    return n;
}

// --- Local/upvalue resolution ---------------------------------------

static int
pf_find_local(ParseFunc *pf, struct LuaString *name)
{
    for (int i = (int)pf->nlocals - 1; i >= 0; i--)
        if (pf->locals[i].name == name) return i;
    return -1;
}

static int
pf_find_upval(ParseFunc *pf, struct LuaString *name)
{
    for (int i = 0; i < (int)pf->nupvals; i++)
        if (pf->upvals[i].name == name) return i;
    return -1;
}

static int
pf_resolve_upval(ParseFunc *pf, struct LuaString *name)
{
    if (!pf->parent) return -1;
    int existing = pf_find_upval(pf, name);
    if (existing >= 0) return existing;

    int local_idx = pf_find_local(pf->parent, name);
    if (local_idx >= 0) {
        pf->parent->locals[local_idx].is_captured = true;
        if (pf->nupvals >= LP_MAX_UPVALS) lua_tok_error("too many upvalues");
        pf->upvals[pf->nupvals] = (UpvalEntry){name, true, (uint32_t)local_idx};
        return (int)pf->nupvals++;
    }
    int up_idx = pf_resolve_upval(pf->parent, name);
    if (up_idx >= 0) {
        if (pf->nupvals >= LP_MAX_UPVALS) lua_tok_error("too many upvalues");
        pf->upvals[pf->nupvals] = (UpvalEntry){name, false, (uint32_t)up_idx};
        return (int)pf->nupvals++;
    }
    return -1;
}

static uint32_t
pf_add_local(ParseFunc *pf, struct LuaString *name)
{
    if (pf->nlocals >= LP_MAX_LOCALS) lua_tok_error("too many local variables");
    uint32_t idx = pf->nlocals;
    pf->locals[idx] = (LocalSlot){name, idx, false};
    pf->nlocals++;
    return idx;
}

// --- Scope stack ----------------------------------------------------

static ParseScope *
scope_enter(bool is_loop)
{
    ParseScope *s = (ParseScope *)calloc(1, sizeof(ParseScope));
    s->local_base = PF_CURRENT->nlocals;
    s->is_loop    = is_loop;
    s->parent     = PF_CURRENT->scope;
    PF_CURRENT->scope = s;
    return s;
}

static void
scope_leave(void)
{
    ParseScope *s = PF_CURRENT->scope;
    PF_CURRENT->scope = s->parent;
    free(s);
}

// --- Resolve a name to a get / set node -----------------------------

static void
pf_track_local_ref(NODE *n, uint32_t slot_idx)
{
    if (PF_CURRENT->nlocal_refs >= LP_MAX_LOCAL_REFS) return;   // overflow: skip
    PF_CURRENT->local_refs[PF_CURRENT->nlocal_refs++] =
        (LocalRef){n, slot_idx};
}

static NODE *
resolve_name_to_node(struct LuaString *name)
{
    int idx = pf_find_local(PF_CURRENT, name);
    if (idx >= 0) {
        NODE *n = ALLOC_node_local_get((uint32_t)idx);
        pf_track_local_ref(n, (uint32_t)idx);
        return n;
    }
    int up = pf_resolve_upval(PF_CURRENT, name);
    if (up >= 0) return ALLOC_node_upval_get((uint32_t)up);
    return ALLOC_node_global_get(name);
}

static NODE *
resolve_name_to_set(struct LuaString *name, NODE *rhs)
{
    int idx = pf_find_local(PF_CURRENT, name);
    if (idx >= 0) {
        NODE *n = ALLOC_node_local_set((uint32_t)idx, rhs);
        pf_track_local_ref(n, (uint32_t)idx);
        return n;
    }
    int up = pf_resolve_upval(PF_CURRENT, name);
    if (up >= 0) return ALLOC_node_upval_set((uint32_t)up, rhs);
    return ALLOC_node_global_set(name, rhs);
}

// After a function body is fully parsed, rewrite local-access nodes
// for captured slots to use the box variants (which dereference the
// heap cell pointer instead of branching on a runtime tag).
static void
pf_finalize_local_refs(ParseFunc *pf)
{
    for (uint32_t i = 0; i < pf->nlocal_refs; i++) {
        LocalRef *r = &pf->local_refs[i];
        if (!pf->locals[r->slot_idx].is_captured) continue;
        NODE *n = r->node;
        if (n->head.kind == &kind_node_local_get) {
            n->head.kind = &kind_node_box_get;
            n->head.dispatcher      = kind_node_box_get.default_dispatcher;
            n->head.dispatcher_name = kind_node_box_get.default_dispatcher_name;
        } else if (n->head.kind == &kind_node_local_set) {
            n->head.kind = &kind_node_box_set;
            n->head.dispatcher      = kind_node_box_set.default_dispatcher;
            n->head.dispatcher_name = kind_node_box_set.default_dispatcher_name;
        }
    }
}

// --- Top-level entry ------------------------------------------------

// struct ParsedChunk { ... };  // declared in main.c
struct ParsedChunk;

NODE *
PARSE_lua(const char *src, const char *filename, struct ParsedChunk *out)
{
    lua_tok_init(src, filename);
    lua_tok_next();

    ParseFunc top = {0};
    top.is_vararg = true;
    top.name      = "<chunk>";
    PF_CURRENT = &top;

    scope_enter(false);
    NODE *body = parse_block();
    scope_leave();

    pf_finalize_local_refs(&top);

    if (cur()->kind != LT_EOF) lua_tok_error("expected end of input");

    if (out) {
        out->body      = body;
        out->nlocals   = top.nlocals;
        out->nupvals   = top.nupvals;
        out->nparams   = top.nparams;
        out->is_vararg = top.is_vararg;
    }
    PF_CURRENT = NULL;
    return body;
}

// --- Block / statement ----------------------------------------------

static bool
block_terminator(ltok_t k)
{
    return k == LT_EOF || k == LT_END || k == LT_ELSE || k == LT_ELSEIF || k == LT_UNTIL;
}

static NODE *
parse_block(void)
{
    NODE *seq = NULL;
    while (!block_terminator(cur()->kind)) {
        if (cur()->kind == LT_RETURN) {
            lua_tok_next();
            NODE *retexpr = NULL;
            uint32_t nret = 0;
            NODE *args[LUASTRO_MAX_RETS];
            if (!block_terminator(cur()->kind) && cur()->kind != LT_SEMI) {
                args[nret++] = parse_expr();
                while (accept(LT_COMMA)) {
                    if (nret >= LUASTRO_MAX_RETS) lua_tok_error("too many return values");
                    args[nret++] = parse_expr();
                }
                if (nret == 1) retexpr = args[0];
                else {
                    uint32_t base = reg_node_arr(args, nret);
                    retexpr = ALLOC_node_value_list(base, nret);
                }
            }
            accept(LT_SEMI);
            if (!retexpr) retexpr = ALLOC_node_nil();   // never pass NULL children
            NODE *retnode = ALLOC_node_return(retexpr, nret);
            seq = seq ? ALLOC_node_seq(seq, retnode) : retnode;
            break;
        }
        NODE *stat = parse_stat();
        if (stat) seq = seq ? ALLOC_node_seq(seq, stat) : stat;
    }
    return seq ? seq : ALLOC_node_nop();
}

// --- Statement parsers ----------------------------------------------

static NODE *
parse_local_stat(void)
{
    if (accept(LT_FUNCTION)) {
        struct LuaString *name = expect_ident();
        uint32_t idx = pf_add_local(PF_CURRENT, name);
        NODE *fn = parse_function_body(false, name);
        NODE *n = ALLOC_node_local_set(idx, fn);
        pf_track_local_ref(n, idx);
        return n;
    }
    struct LuaString *names[LP_MAX_LOCALS];
    uint32_t nnames = 0;
    names[nnames++] = expect_ident();
    if (accept(LT_LT)) { expect(LT_IDENT, "attribute"); expect(LT_GT, "'>'"); }
    while (accept(LT_COMMA)) {
        names[nnames++] = expect_ident();
        if (accept(LT_LT)) { expect(LT_IDENT, "attribute"); expect(LT_GT, "'>'"); }
    }
    NODE *exps[LP_MAX_LOCALS] = {0};
    uint32_t nexps = 0;
    if (accept(LT_ASSIGN)) {
        exps[nexps++] = parse_expr();
        while (accept(LT_COMMA)) exps[nexps++] = parse_expr();
    }
    uint32_t indices[LP_MAX_LOCALS];
    for (uint32_t i = 0; i < nnames; i++) indices[i] = pf_add_local(PF_CURRENT, names[i]);
    // Hot path: `local x = expr` — single LHS, single RHS.  Emit the
    // specialized node so ASTroGen can recurse into rhs and bake an
    // SD that inlines the rhs's evaluation directly (no @noinline
    // trampoline through the side array).
    if (nnames == 1 && nexps == 1) {
        return ALLOC_node_local_decl_one(indices[0], exps[0]);
    }
    uint32_t lhs_idx = reg_u32_arr(indices, nnames);
    uint32_t rhs_idx = nexps ? reg_node_arr(exps, nexps) : 0;
    return ALLOC_node_local_decl(lhs_idx, nnames, rhs_idx, nexps);
}

static NODE *
parse_if_stat(void)
{
    NODE *cond = parse_expr();
    expect(LT_THEN, "'then'");
    scope_enter(false);
    NODE *thenb = parse_block();
    scope_leave();
    NODE *elseb = NULL;
    if (accept(LT_ELSEIF)) {
        elseb = parse_if_stat();
    } else if (accept(LT_ELSE)) {
        scope_enter(false);
        elseb = parse_block();
        scope_leave();
        expect(LT_END, "'end'");
    } else {
        expect(LT_END, "'end'");
    }
    if (!elseb) elseb = ALLOC_node_nop();   // dispatch wrappers can't accept NULL children
    return ALLOC_node_if(cond, thenb, elseb);
}

static NODE *
parse_while_stat(void)
{
    NODE *cond = parse_expr();
    expect(LT_DO, "'do'");
    scope_enter(true);
    NODE *body = parse_block();
    scope_leave();
    expect(LT_END, "'end'");
    return ALLOC_node_while(cond, body);
}

static NODE *
parse_repeat_stat(void)
{
    scope_enter(true);
    NODE *body = parse_block();
    expect(LT_UNTIL, "'until'");
    NODE *cond = parse_expr();
    scope_leave();
    return ALLOC_node_repeat(body, cond);
}

static NODE *
parse_for_stat(void)
{
    struct LuaString *name1 = expect_ident();
    if (accept(LT_ASSIGN)) {
        NODE *start = parse_expr();
        expect(LT_COMMA, "','");
        NODE *limit = parse_expr();
        NODE *step  = NULL;
        if (accept(LT_COMMA)) step = parse_expr();
        expect(LT_DO, "'do'");
        scope_enter(true);
        uint32_t var_idx = pf_add_local(PF_CURRENT, name1);
        NODE *body = parse_block();
        scope_leave();
        expect(LT_END, "'end'");
        if (!step) step = ALLOC_node_int(1);
        // Pattern match the body: detect the classic accumulator
        //   for i = ...,...,... do sum = sum + i end
        // and emit a specialized node that keeps both `sum` and `i`
        // in scalar registers across the loop.
        if (body->head.kind == &kind_node_local_set) {
            uint32_t target = body->u.node_local_set.idx;
            NODE *rhs = body->u.node_local_set.rhs;
            if (rhs->head.kind == &kind_node_int_add) {
                NODE *l = rhs->u.node_int_add.l;
                NODE *r = rhs->u.node_int_add.r;
                if (l->head.kind == &kind_node_local_get &&
                    r->head.kind == &kind_node_local_get &&
                    l->u.node_local_get.idx == target &&
                    r->u.node_local_get.idx == var_idx) {
                    return ALLOC_node_numfor_int_sum(var_idx, target, start, limit, step);
                }
                // also accept `sum = i + sum` (commuted)
                if (r->head.kind == &kind_node_local_get &&
                    l->head.kind == &kind_node_local_get &&
                    r->u.node_local_get.idx == target &&
                    l->u.node_local_get.idx == var_idx) {
                    return ALLOC_node_numfor_int_sum(var_idx, target, start, limit, step);
                }
            }
        }
        return ALLOC_node_numfor(var_idx, start, limit, step, body);
    } else {
        struct LuaString *names[LP_MAX_LOCALS];
        uint32_t nnames = 0;
        names[nnames++] = name1;
        while (accept(LT_COMMA)) names[nnames++] = expect_ident();
        expect(LT_IN, "'='/'in'");
        NODE *exps[3] = {NULL, NULL, NULL};
        uint32_t nexps = 0;
        exps[nexps++] = parse_expr();
        while (accept(LT_COMMA) && nexps < 3) exps[nexps++] = parse_expr();
        expect(LT_DO, "'do'");
        scope_enter(true);
        uint32_t var_indices[LP_MAX_LOCALS];
        for (uint32_t i = 0; i < nnames; i++) var_indices[i] = pf_add_local(PF_CURRENT, names[i]);
        NODE *body = parse_block();
        scope_leave();
        expect(LT_END, "'end'");
        uint32_t lhs_idx = reg_u32_arr(var_indices, nnames);
        return ALLOC_node_genfor(lhs_idx, nnames,
                                 exps[0], exps[1] ? exps[1] : ALLOC_node_nil(),
                                 exps[2] ? exps[2] : ALLOC_node_nil(), body);
    }
}

static NODE *
parse_do_stat(void)
{
    scope_enter(false);
    NODE *body = parse_block();
    scope_leave();
    expect(LT_END, "'end'");
    return body;
}

static NODE *
parse_function_stat(void)
{
    struct LuaString *first = expect_ident();
    NODE *target = resolve_name_to_node(first);
    bool is_method = false;
    struct LuaString *last_name = first;
    while (accept(LT_DOT)) {
        struct LuaString *fld = expect_ident();
        target = ALLOC_node_field_get(target, fld);
        last_name = fld;
    }
    if (accept(LT_COLON)) {
        last_name = expect_ident();
        target = ALLOC_node_field_get(target, last_name);
        is_method = true;
    }
    NODE *fn = parse_function_body(is_method, last_name);
    if (target->head.kind == &kind_node_field_get) {
        return ALLOC_node_field_set(target->u.node_field_get.recv,
                                    target->u.node_field_get.field, fn);
    }
    return resolve_name_to_set(first, fn);
}

static NODE *
parse_label_stat(void)
{
    struct LuaString *name = expect_ident();
    expect(LT_DBLCOLON, "'::'");
    return ALLOC_node_label(name);
}

static NODE *
parse_goto_stat(void)
{
    struct LuaString *name = expect_ident();
    return ALLOC_node_goto(name);
}

static bool
is_call_node(NODE *n)
{
    return n->head.kind == &kind_node_call ||
           n->head.kind == &kind_node_method_call ||
           n->head.kind == &kind_node_call_arg0 ||
           n->head.kind == &kind_node_call_arg1 ||
           n->head.kind == &kind_node_call_arg2 ||
           n->head.kind == &kind_node_call_arg3;
}

static NODE *
parse_single_assign(NODE *lhs, NODE *rhs)
{
    if (lhs->head.kind == &kind_node_local_get) {
        uint32_t idx = lhs->u.node_local_get.idx;
        NODE *n = ALLOC_node_local_set(idx, rhs);
        pf_track_local_ref(n, idx);
        return n;
    }
    if (lhs->head.kind == &kind_node_upval_get)
        return ALLOC_node_upval_set(lhs->u.node_upval_get.idx, rhs);
    if (lhs->head.kind == &kind_node_global_get)
        return ALLOC_node_global_set(lhs->u.node_global_get.name, rhs);
    if (lhs->head.kind == &kind_node_field_get)
        return ALLOC_node_field_set(lhs->u.node_field_get.recv,
                                    lhs->u.node_field_get.field, rhs);
    if (lhs->head.kind == &kind_node_index_get)
        return ALLOC_node_index_set(lhs->u.node_index_get.recv,
                                    lhs->u.node_index_get.key, rhs);
    lua_tok_error("invalid assignment target");
}

static NODE *
parse_expr_stat(void)
{
    NODE *first = parse_suffixed_exp();
    if (cur()->kind == LT_ASSIGN || cur()->kind == LT_COMMA) {
        NODE *lhs[LP_MAX_LOCALS] = {first};
        uint32_t nlhs = 1;
        while (accept(LT_COMMA)) lhs[nlhs++] = parse_suffixed_exp();
        expect(LT_ASSIGN, "'='");
        NODE *rhs[LP_MAX_LOCALS]; uint32_t nrhs = 0;
        rhs[nrhs++] = parse_expr();
        while (accept(LT_COMMA)) rhs[nrhs++] = parse_expr();
        if (nlhs == 1 && nrhs == 1) return parse_single_assign(lhs[0], rhs[0]);
        uint32_t lhs_idx = reg_node_arr(lhs, nlhs);
        uint32_t rhs_idx = reg_node_arr(rhs, nrhs);
        return ALLOC_node_multi_assign(lhs_idx, nlhs, rhs_idx, nrhs);
    }
    if (!is_call_node(first)) lua_tok_error("syntax error: expected statement");
    return first;
}

static NODE *
parse_stat(void)
{
    switch (cur()->kind) {
    case LT_SEMI:     lua_tok_next(); return NULL;
    case LT_IF:       lua_tok_next(); return parse_if_stat();
    case LT_WHILE:    lua_tok_next(); return parse_while_stat();
    case LT_DO:       lua_tok_next(); return parse_do_stat();
    case LT_FOR:      lua_tok_next(); return parse_for_stat();
    case LT_REPEAT:   lua_tok_next(); return parse_repeat_stat();
    case LT_FUNCTION: lua_tok_next(); return parse_function_stat();
    case LT_LOCAL:    lua_tok_next(); return parse_local_stat();
    case LT_BREAK:    lua_tok_next(); return ALLOC_node_break();
    case LT_GOTO:     lua_tok_next(); return parse_goto_stat();
    case LT_DBLCOLON: lua_tok_next(); return parse_label_stat();
    case LT_RETURN:   return NULL;   // handled in parse_block
    default:          return parse_expr_stat();
    }
}

// --- Expression parsing (Pratt) -------------------------------------

typedef struct { int lprec, rprec; int op; } BinOp;
#define OP_LT 100
#define OP_LE 101
#define OP_GT 102
#define OP_GE 103
#define OP_EQ 104
#define OP_NEQ 105
#define OP_AND 106
#define OP_OR  107
#define OP_CONCAT 108

static BinOp
get_binop(ltok_t k)
{
    switch (k) {
    case LT_OR:      return (BinOp){1,1, OP_OR};
    case LT_AND:     return (BinOp){2,2, OP_AND};
    case LT_LT:      return (BinOp){3,3, OP_LT};
    case LT_GT:      return (BinOp){3,3, OP_GT};
    case LT_LE:      return (BinOp){3,3, OP_LE};
    case LT_GE:      return (BinOp){3,3, OP_GE};
    case LT_EQ:      return (BinOp){3,3, OP_EQ};
    case LT_NEQ:     return (BinOp){3,3, OP_NEQ};
    case LT_PIPE:    return (BinOp){4,4, LUA_OP_BOR};
    case LT_TILDE:   return (BinOp){5,5, LUA_OP_BXOR};
    case LT_AMP:     return (BinOp){6,6, LUA_OP_BAND};
    case LT_LSHIFT:  return (BinOp){7,7, LUA_OP_SHL};
    case LT_RSHIFT:  return (BinOp){7,7, LUA_OP_SHR};
    case LT_DOTDOT:  return (BinOp){9,8, OP_CONCAT};
    case LT_PLUS:    return (BinOp){10,10, LUA_OP_ADD};
    case LT_MINUS:   return (BinOp){10,10, LUA_OP_SUB};
    case LT_STAR:    return (BinOp){11,11, LUA_OP_MUL};
    case LT_SLASH:   return (BinOp){11,11, LUA_OP_DIV};
    case LT_DSLASH:  return (BinOp){11,11, LUA_OP_FLOORDIV};
    case LT_PERCENT: return (BinOp){11,11, LUA_OP_MOD};
    case LT_CARET:   return (BinOp){14,13, LUA_OP_POW};
    default:         return (BinOp){0,0,0};
    }
}

static NODE *
make_binop(int op, NODE *l, NODE *r)
{
    switch (op) {
    case OP_AND:    return ALLOC_node_and(l, r);
    case OP_OR:     return ALLOC_node_or (l, r);
    case OP_CONCAT: return ALLOC_node_concat(l, r);
    case OP_LT:     return ALLOC_node_lt (l, r);
    case OP_LE:     return ALLOC_node_le (l, r);
    case OP_GT:     return ALLOC_node_lt (r, l);
    case OP_GE:     return ALLOC_node_le (r, l);
    case OP_EQ:     return ALLOC_node_eq (l, r);
    case OP_NEQ:    return ALLOC_node_neq(l, r);
    case LUA_OP_ADD: return ALLOC_node_int_add(l, r);
    case LUA_OP_SUB: return ALLOC_node_int_sub(l, r);
    case LUA_OP_MUL: return ALLOC_node_int_mul(l, r);
    default:         return ALLOC_node_arith((uint32_t)op, l, r);
    }
}

static NODE *
parse_unary_or_simple(void)
{
    switch (cur()->kind) {
    case LT_MINUS: lua_tok_next(); return ALLOC_node_unm  (parse_expr_prec(12));
    case LT_NOT:   lua_tok_next(); return ALLOC_node_not  (parse_expr_prec(12));
    case LT_HASH:  lua_tok_next(); return ALLOC_node_len  (parse_expr_prec(12));
    case LT_TILDE: lua_tok_next(); return ALLOC_node_bnot (parse_expr_prec(12));
    default:       return parse_simple_exp();
    }
}

static NODE *
parse_expr_prec(int min_prec)
{
    NODE *left = parse_unary_or_simple();
    for (;;) {
        BinOp b = get_binop(cur()->kind);
        if (b.lprec == 0 || b.lprec < min_prec) break;
        lua_tok_next();
        NODE *right = parse_expr_prec(b.rprec + 1);
        left = make_binop(b.op, left, right);
    }
    return left;
}

static NODE *parse_expr(void) { return parse_expr_prec(1); }

static NODE *
parse_simple_exp(void)
{
    const Token *t = cur();
    switch (t->kind) {
    case LT_INT:    { int64_t v = t->int_value; lua_tok_next(); return ALLOC_node_int((uint64_t)v); }
    case LT_FLOAT:  { double  v = t->float_value; lua_tok_next(); return ALLOC_node_float(v); }
    case LT_STRING: {
        struct LuaString *s = lua_str_intern_n(t->str_value, t->str_len);
        lua_tok_next();
        return ALLOC_node_string(s);
    }
    case LT_NIL:    lua_tok_next(); return ALLOC_node_nil();
    case LT_TRUE:   lua_tok_next(); return ALLOC_node_true();
    case LT_FALSE:  lua_tok_next(); return ALLOC_node_false();
    case LT_ELLIPSIS:
        lua_tok_next();
        if (!PF_CURRENT->is_vararg) lua_tok_error("'...' outside vararg function");
        return ALLOC_node_vararg();
    case LT_LBRACE: return parse_table_constructor();
    case LT_FUNCTION:
        lua_tok_next();
        return parse_function_body(false, NULL);
    default:
        return parse_suffixed_exp();
    }
}

static NODE *
parse_primary_exp(void)
{
    if (accept(LT_LPAREN)) {
        NODE *e = parse_expr();
        expect(LT_RPAREN, "')'");
        return e;
    }
    if (cur()->kind == LT_IDENT) {
        struct LuaString *name = lua_str_intern_n(cur()->start, cur()->len);
        lua_tok_next();
        return resolve_name_to_node(name);
    }
    lua_tok_error("expected expression");
}

static NODE *
parse_suffixed_exp(void)
{
    NODE *node = parse_primary_exp();
    for (;;) {
        switch (cur()->kind) {
        case LT_DOT: {
            lua_tok_next();
            struct LuaString *fld = expect_ident();
            node = ALLOC_node_field_get(node, fld);
            break;
        }
        case LT_LBRACK: {
            lua_tok_next();
            NODE *key = parse_expr();
            expect(LT_RBRACK, "']'");
            node = ALLOC_node_index_get(node, key);
            break;
        }
        case LT_COLON: {
            lua_tok_next();
            struct LuaString *m = expect_ident();
            node = parse_args(node, true, m);
            break;
        }
        case LT_LPAREN:
        case LT_STRING:
        case LT_LBRACE:
            node = parse_args(node, false, NULL);
            break;
        default:
            return node;
        }
    }
}

static NODE *
parse_args(NODE *callee, bool is_method, struct LuaString *method_name)
{
    NODE *args[LP_MAX_LOCALS]; uint32_t nargs = 0;
    if (accept(LT_LPAREN)) {
        if (!accept(LT_RPAREN)) {
            args[nargs++] = parse_expr();
            while (accept(LT_COMMA)) args[nargs++] = parse_expr();
            expect(LT_RPAREN, "')'");
        }
    } else if (cur()->kind == LT_STRING) {
        struct LuaString *s = lua_str_intern_n(cur()->str_value, cur()->str_len);
        lua_tok_next();
        args[nargs++] = ALLOC_node_string(s);
    } else if (cur()->kind == LT_LBRACE) {
        args[nargs++] = parse_table_constructor();
    } else {
        lua_tok_error("expected arguments");
    }
    if (is_method) {
        uint32_t base = nargs ? reg_node_arr(args, nargs) : 0;
        return ALLOC_node_method_call(callee, method_name, base, nargs);
    }
    if (nargs == 0) return ALLOC_node_call_arg0(callee);
    if (nargs == 1) return ALLOC_node_call_arg1(callee, args[0]);
    if (nargs == 2) return ALLOC_node_call_arg2(callee, args[0], args[1]);
    if (nargs == 3) return ALLOC_node_call_arg3(callee, args[0], args[1], args[2]);
    uint32_t base = reg_node_arr(args, nargs);
    return ALLOC_node_call(callee, base, nargs);
}

static NODE *
parse_table_constructor(void)
{
    expect(LT_LBRACE, "'{'");
    NODE *seq_vals[LP_MAX_LOCALS];
    NODE *kv_pairs[LP_MAX_LOCALS * 2];
    uint32_t nseq = 0, nkv = 0;
    while (cur()->kind != LT_RBRACE) {
        if (cur()->kind == LT_LBRACK) {
            lua_tok_next();
            NODE *k = parse_expr();
            expect(LT_RBRACK, "']'");
            expect(LT_ASSIGN, "'='");
            NODE *v = parse_expr();
            kv_pairs[2*nkv] = k; kv_pairs[2*nkv+1] = v; nkv++;
        } else if (cur()->kind == LT_IDENT) {
            // Lookahead: peek the following token via a saved snapshot.
            // Cheap solution: save tokenizer state by re-reading the
            // identifier as a name-or-expression and checking what
            // follows once we've consumed it.
            struct LuaString *name = lua_str_intern_n(cur()->start, cur()->len);
            lua_tok_next();
            if (cur()->kind == LT_ASSIGN) {
                lua_tok_next();
                NODE *v = parse_expr();
                kv_pairs[2*nkv] = ALLOC_node_string(name);
                kv_pairs[2*nkv+1] = v;
                nkv++;
            } else {
                // Identifier was the start of an expression; rebuild it.
                NODE *prim = resolve_name_to_node(name);
                // Continue with suffix / binop parsing.
                for (;;) {
                    if (cur()->kind == LT_DOT) {
                        lua_tok_next();
                        struct LuaString *fld = expect_ident();
                        prim = ALLOC_node_field_get(prim, fld);
                    } else if (cur()->kind == LT_LBRACK) {
                        lua_tok_next();
                        NODE *k = parse_expr();
                        expect(LT_RBRACK, "']'");
                        prim = ALLOC_node_index_get(prim, k);
                    } else if (cur()->kind == LT_LPAREN ||
                               cur()->kind == LT_STRING ||
                               cur()->kind == LT_LBRACE) {
                        prim = parse_args(prim, false, NULL);
                    } else if (cur()->kind == LT_COLON) {
                        lua_tok_next();
                        struct LuaString *m = expect_ident();
                        prim = parse_args(prim, true, m);
                    } else break;
                }
                for (;;) {
                    BinOp b = get_binop(cur()->kind);
                    if (b.lprec == 0) break;
                    lua_tok_next();
                    NODE *right = parse_expr_prec(b.rprec + 1);
                    prim = make_binop(b.op, prim, right);
                }
                seq_vals[nseq++] = prim;
            }
        } else {
            seq_vals[nseq++] = parse_expr();
        }
        if (!accept(LT_COMMA) && !accept(LT_SEMI)) break;
    }
    expect(LT_RBRACE, "'}'");
    uint32_t seq_idx = nseq ? reg_node_arr(seq_vals, nseq) : 0;
    uint32_t kv_idx  = nkv  ? reg_node_arr(kv_pairs, 2*nkv) : 0;
    return ALLOC_node_table_new(seq_idx, nseq, kv_idx, nkv);
}

// --- Function bodies ------------------------------------------------

static NODE *
parse_function_body(bool is_method, struct LuaString *name)
{
    ParseFunc *parent = PF_CURRENT;
    ParseFunc child = {0};
    child.parent = parent;
    child.name   = name ? lua_str_data(name) : "<anon>";
    PF_CURRENT = &child;
    scope_enter(false);

    if (is_method) {
        struct LuaString *self = lua_str_intern("self");
        pf_add_local(&child, self);
        child.nparams++;
    }

    expect(LT_LPAREN, "'('");
    if (cur()->kind != LT_RPAREN) {
        for (;;) {
            if (accept(LT_ELLIPSIS)) { child.is_vararg = true; break; }
            struct LuaString *pname = expect_ident();
            pf_add_local(&child, pname);
            child.nparams++;
            if (!accept(LT_COMMA)) break;
        }
    }
    expect(LT_RPAREN, "')'");
    NODE *body = parse_block();
    expect(LT_END, "'end'");

    pf_finalize_local_refs(&child);

    scope_leave();
    PF_CURRENT = parent;

    // Snapshot upvalue source pairs and captured-local indices into
    // U32_ARR so the make_closure node can read them at runtime.
    uint32_t up_pairs[LP_MAX_UPVALS * 2];
    for (uint32_t i = 0; i < child.nupvals; i++) {
        up_pairs[2*i]     = child.upvals[i].from_local ? 0 : 1;
        up_pairs[2*i + 1] = child.upvals[i].src_idx;
    }
    uint32_t up_idx = child.nupvals ? reg_u32_arr(up_pairs, 2 * child.nupvals) : 0;

    uint32_t cap_local_idxs[LP_MAX_LOCALS];
    uint32_t ncap = 0;
    for (uint32_t i = 0; i < child.nlocals; i++)
        if (child.locals[i].is_captured) cap_local_idxs[ncap++] = i;
    uint32_t cap_idx = ncap ? reg_u32_arr(cap_local_idxs, ncap) : 0;

    // Register the body separately so the AOT pipeline emits an SD_ entry
    // point for it.  Without this, recursive function calls dispatch via
    // the generic DISPATCH_<name> chain instead of the specialized SD.
    extern void code_repo_add(const char *name, NODE *body, bool force);
    code_repo_add(child.name, body, true);

    return ALLOC_node_make_closure(body, child.nparams, child.nlocals, child.nupvals,
                                   child.is_vararg ? 1 : 0,
                                   cap_idx, ncap, up_idx);
}

// --- runtime helpers used by node_multi_assign ---------------------

void
luastro_apply_assign(CTX *c, NODE *target, LuaValue v, LuaValue *frame)
{
    if (target->head.kind == &kind_node_local_get) {
        frame[target->u.node_local_get.idx] = v;
        return;
    }
    if (target->head.kind == &kind_node_upval_get) {
        extern LuaValue **LUASTRO_CUR_UPVALS;
        *LUASTRO_CUR_UPVALS[target->u.node_upval_get.idx] = v;
        return;
    }
    if (target->head.kind == &kind_node_global_get) {
        lua_table_set_str(c->globals, target->u.node_global_get.name, v);
        return;
    }
    if (target->head.kind == &kind_node_field_get) {
        LuaValue r = EVAL(c, target->u.node_field_get.recv, frame);
        if (!LV_IS_TBL(r)) lua_raisef(c, "attempt to index a %s value", lua_type_name(r));
        lua_table_set_str(LV_AS_TBL(r), target->u.node_field_get.field, v);
        return;
    }
    if (target->head.kind == &kind_node_index_get) {
        LuaValue r = EVAL(c, target->u.node_index_get.recv, frame);
        LuaValue k = EVAL(c, target->u.node_index_get.key,  frame);
        if (!LV_IS_TBL(r)) lua_raisef(c, "attempt to index a %s value", lua_type_name(r));
        lua_table_set(LV_AS_TBL(r), k, v);
        return;
    }
    lua_raisef(c, "invalid assignment target");
}
