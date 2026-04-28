// wastro — folded-and-stack-style WAT parser
//
// `#include`'d from main.c.  Owns:
//
//   * the type-aware folded-S-expr expression parser
//   * `(export ...)` / `(import ...)` inline-binding helpers
//   * the stack-style (bare-keyword) WAT parser used by the
//     spec-testsuite text format
//   * the two-pass `(func ...)` driver that registers names first
//     then parses bodies, plus `wastro_load_module_buf` /
//     `wastro_load_module` / `wastro_load_module_OLD` entries
//
// All parsers share the tokenizer state and the WASTRO_FUNCS / GLOBALS
// / TYPES / TABLE module-state arrays declared in main.c.

// =====================================================================
// Expression parser (folded S-expr form, type-aware)
// =====================================================================

typedef struct {
    char *names[64];
    wtype_t types[64];
    uint32_t cnt;
} LocalEnv;

// Label environment for structured control flow.  Each entry corresponds
// to an enclosing block / loop.  names[0] is outermost, names[cnt-1]
// innermost.  result_types[i] is the carried-value type for `br i`
// (WT_VOID if the block / loop has no result).  is_loop[i] tells the
// parser whether `br` to label i carries the block's result type
// (block) or no value (loop) — wasm spec quirk.
typedef struct {
    char *names[32];
    wtype_t result_types[32];
    int    is_loop[32];     // 1 if this label is a loop, 0 if block
    uint32_t cnt;
} LabelEnv;

typedef struct { NODE *node; wtype_t type; } TypedExpr;

// Forward declarations for the stack-style body parser (defined later).
struct OpStack_;  struct StmtList_;
static TypedExpr parse_body_seq(LocalEnv *env, LabelEnv *labels, int allow_else_terminator, int *out_else);
static void load_module_binary(const uint8_t *buf, size_t sz);
NODE *wastro_load_module_buf(const char *buf, size_t sz);

static int
label_env_lookup(const LabelEnv *labels, const Token *t, uint32_t *out_depth)
{
    if (t->kind == T_INT) {
        if ((uint64_t)t->int_value >= labels->cnt) return 0;
        *out_depth = (uint32_t)t->int_value;
        return 1;
    }
    if (t->kind != T_IDENT) return 0;
    for (int i = (int)labels->cnt - 1; i >= 0; i--) {
        const char *nm = labels->names[i];
        if (nm && strlen(nm) == t->len && memcmp(nm, t->start, t->len) == 0) {
            *out_depth = (uint32_t)((int)labels->cnt - 1 - i);
            return 1;
        }
    }
    return 0;
}

static const char *
wtype_name(wtype_t t)
{
    switch (t) {
    case WT_VOID: return "void";
    case WT_I32:  return "i32";
    case WT_I64:  return "i64";
    case WT_F32:  return "f32";
    case WT_F64:  return "f64";
    case WT_POLY: return "poly";
    }
    return "?";
}

// Parse a wasm value-type keyword (i32 / i64 / f32 / f64).
// Caller has already consumed any leading paren etc.
static wtype_t
parse_wtype(void)
{
    if (cur_tok.kind != T_KEYWORD) parse_error("expected wasm type");
    if (tok_is_keyword("i32")) { next_token(); return WT_I32; }
    if (tok_is_keyword("i64")) { next_token(); return WT_I64; }
    if (tok_is_keyword("f32")) { next_token(); return WT_F32; }
    if (tok_is_keyword("f64")) { next_token(); return WT_F64; }
    parse_error("unknown value type (expected i32 / i64 / f32 / f64)");
    return WT_VOID;
}

// Parse the first wasm type from a (result T+) clause.  Multi-value
// (post-1.0) extras are silently consumed and discarded, since
// wastro models only single-result functions.
static wtype_t
parse_result_type(void)
{
    wtype_t r = parse_wtype();
    while (cur_tok.kind == T_KEYWORD &&
           (tok_is_keyword("i32") || tok_is_keyword("i64") ||
            tok_is_keyword("f32") || tok_is_keyword("f64"))) {
        parse_wtype();
    }
    return r;
}

static int
local_env_lookup(const LocalEnv *env, const Token *t)
{
    if (t->kind == T_INT) return (int)t->int_value;
    if (t->kind != T_IDENT) parse_error("expected local ref");
    for (uint32_t i = 0; i < env->cnt; i++) {
        const char *n = env->names[i];
        if (n && strlen(n) == t->len && memcmp(n, t->start, t->len) == 0) return (int)i;
    }
    fprintf(stderr, "wastro: unknown local '%.*s'\n", (int)t->len, t->start);
    exit(1);
}

static char *
dup_token_str(const Token *t)
{
    char *s = malloc(t->len + 1);
    memcpy(s, t->start, t->len);
    s[t->len] = '\0';
    return s;
}

static TypedExpr parse_expr(LocalEnv *env, LabelEnv *labels);

static void
expect_type(wtype_t got, wtype_t want, const char *site)
{
    if (got == WT_POLY) return;   // wasm polymorphic stack
    if (got != want) {
        fprintf(stderr, "wastro: type mismatch at %s: expected %s, got %s\n",
                site, wtype_name(want), wtype_name(got));
        if (wastro_parse_active) {
            snprintf(wastro_parse_message, sizeof(wastro_parse_message),
                     "type mismatch at %s: expected %s, got %s",
                     site, wtype_name(want), wtype_name(got));
            longjmp(wastro_parse_jmp, 1);
        }
        exit(1);
    }
}

// Build a right-leaning seq from N statements; result type is the
// last statement's type (matches wasm "tail value" semantics).
static TypedExpr
build_seq(TypedExpr *stmts, uint32_t n)
{
    if (n == 0) return (TypedExpr){ALLOC_node_i32_const(0), WT_I32};
    NODE *acc = stmts[n - 1].node;
    wtype_t t = stmts[n - 1].type;
    for (int i = (int)n - 2; i >= 0; i--) {
        acc = ALLOC_node_seq(stmts[i].node, acc);
    }
    return (TypedExpr){acc, t};
}

static TypedExpr
parse_seq_until_rparen(LocalEnv *env, LabelEnv *labels)
{
    // Delegates to the unified body parser so that bodies may mix
    // folded `(...)` and bare stack-style instructions seamlessly.
    return parse_body_seq(env, labels, 0, NULL);
}

// `(if (result T)? <cond> (then ...) (else ...)?)`
static TypedExpr
parse_if(LocalEnv *env, LabelEnv *labels)
{
    wtype_t result_t = WT_I32;     // default — wasm spec defaults to no result, but we model void as i32 for now.
    int has_result = 0;
    if (cur_tok.kind == T_LPAREN) {
        const char *save_pos = src_pos;
        Token save_tok = cur_tok;
        next_token();
        if (tok_is_keyword("result")) {
            next_token();
            result_t = parse_result_type();
            expect_rparen();
            has_result = 1;
        }
        else {
            src_pos = save_pos;
            cur_tok = save_tok;
        }
    }

    TypedExpr cond = parse_expr(env, labels);
    expect_type(cond.type, WT_I32, "if condition");

    // The `if` introduces a label that `br N` from inside the
    // then/else bodies can target.  Push it before parsing branches.
    if (labels->cnt >= 32) parse_error("too many nested labels");
    labels->names[labels->cnt] = NULL;
    labels->result_types[labels->cnt] = result_t;
    labels->is_loop[labels->cnt] = 0;
    labels->cnt++;

    expect_lparen();
    expect_keyword("then");
    TypedExpr then_branch = parse_seq_until_rparen(env, labels);
    expect_rparen();

    TypedExpr else_branch;
    int has_else = 0;
    if (cur_tok.kind == T_LPAREN) {
        expect_lparen();
        expect_keyword("else");
        else_branch = parse_seq_until_rparen(env, labels);
        expect_rparen();
        has_else = 1;
    }
    else {
        // No else clause — synthesize a no-op of the appropriate type.
        // The implicit else of an if-without-result is void.
        else_branch = (TypedExpr){ALLOC_node_nop(), WT_VOID};
    }
    labels->cnt--;

    if (has_result) {
        if (then_branch.type != WT_POLY) expect_type(then_branch.type, result_t, "if-then branch");
        if (else_branch.type != WT_POLY) expect_type(else_branch.type, result_t, "if-else branch");
    }
    else {
        // No declared result type.  If then-branch produces a value
        // and an else exists, both must match — and that value type
        // becomes the if's result.  Otherwise the if is void.
        if (has_else && then_branch.type != WT_VOID && else_branch.type != WT_VOID) {
            expect_type(else_branch.type, then_branch.type, "if-else branch");
            result_t = then_branch.type;
        }
        else {
            // Either there's no else, or one branch is void: treat as void.
            result_t = WT_VOID;
        }
    }
    return (TypedExpr){
        ALLOC_node_if(cond.node, then_branch.node, else_branch.node),
        result_t,
    };
}

// Parse the next operand, OR synthesize a polymorphic placeholder if
// the next token is ')' — this matches the wasm spec's polymorphic
// stack rule, where instructions following a `br` / `return` /
// `unreachable` may omit their operands in folded WAT because the
// validator treats the stack as having any type.
static TypedExpr
parse_expr_or_poly(LocalEnv *env, LabelEnv *labels)
{
    if (cur_tok.kind == T_RPAREN) {
        return (TypedExpr){ALLOC_node_unreachable(), WT_POLY};
    }
    return parse_expr(env, labels);
}

// Generic binary-op helper: parse two operands, validate they have
// the expected operand type, and return the result with the given
// result type.
#define BIN_OP(KW, OPND_T, RES_T, ALLOC)                            \
    if (tok_is_keyword(KW)) {                                       \
        next_token();                                               \
        TypedExpr l = parse_expr_or_poly(env, labels);              \
        TypedExpr r = parse_expr_or_poly(env, labels);              \
        expect_type(l.type, OPND_T, KW " left");                    \
        expect_type(r.type, OPND_T, KW " right");                   \
        expect_rparen();                                            \
        return (TypedExpr){ALLOC(l.node, r.node), RES_T};           \
    }

#define UN_OP(KW, OPND_T, RES_T, ALLOC)                             \
    if (tok_is_keyword(KW)) {                                       \
        next_token();                                               \
        TypedExpr e = parse_expr_or_poly(env, labels);              \
        expect_type(e.type, OPND_T, KW " operand");                 \
        expect_rparen();                                            \
        return (TypedExpr){ALLOC(e.node), RES_T};                   \
    }

static TypedExpr
parse_op(LocalEnv *env, LabelEnv *labels)
{
    if (cur_tok.kind != T_KEYWORD) parse_error("expected keyword");

    // ------- i32 ops -------
    if (tok_is_keyword("i32.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected integer literal");
        int32_t v = (int32_t)cur_tok.int_value;
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_i32_const(v), WT_I32};
    }
    BIN_OP("i32.add",   WT_I32, WT_I32, ALLOC_node_i32_add)
    BIN_OP("i32.sub",   WT_I32, WT_I32, ALLOC_node_i32_sub)
    BIN_OP("i32.mul",   WT_I32, WT_I32, ALLOC_node_i32_mul)
    BIN_OP("i32.div_s", WT_I32, WT_I32, ALLOC_node_i32_div_s)
    BIN_OP("i32.div_u", WT_I32, WT_I32, ALLOC_node_i32_div_u)
    BIN_OP("i32.rem_s", WT_I32, WT_I32, ALLOC_node_i32_rem_s)
    BIN_OP("i32.rem_u", WT_I32, WT_I32, ALLOC_node_i32_rem_u)
    BIN_OP("i32.and",   WT_I32, WT_I32, ALLOC_node_i32_and)
    BIN_OP("i32.or",    WT_I32, WT_I32, ALLOC_node_i32_or)
    BIN_OP("i32.xor",   WT_I32, WT_I32, ALLOC_node_i32_xor)
    BIN_OP("i32.shl",   WT_I32, WT_I32, ALLOC_node_i32_shl)
    BIN_OP("i32.shr_s", WT_I32, WT_I32, ALLOC_node_i32_shr_s)
    BIN_OP("i32.shr_u", WT_I32, WT_I32, ALLOC_node_i32_shr_u)
    BIN_OP("i32.rotl",  WT_I32, WT_I32, ALLOC_node_i32_rotl)
    BIN_OP("i32.rotr",  WT_I32, WT_I32, ALLOC_node_i32_rotr)
    BIN_OP("i32.eq",    WT_I32, WT_I32, ALLOC_node_i32_eq)
    BIN_OP("i32.ne",    WT_I32, WT_I32, ALLOC_node_i32_ne)
    BIN_OP("i32.lt_s",  WT_I32, WT_I32, ALLOC_node_i32_lt_s)
    BIN_OP("i32.lt_u",  WT_I32, WT_I32, ALLOC_node_i32_lt_u)
    BIN_OP("i32.le_s",  WT_I32, WT_I32, ALLOC_node_i32_le_s)
    BIN_OP("i32.le_u",  WT_I32, WT_I32, ALLOC_node_i32_le_u)
    BIN_OP("i32.gt_s",  WT_I32, WT_I32, ALLOC_node_i32_gt_s)
    BIN_OP("i32.gt_u",  WT_I32, WT_I32, ALLOC_node_i32_gt_u)
    BIN_OP("i32.ge_s",  WT_I32, WT_I32, ALLOC_node_i32_ge_s)
    BIN_OP("i32.ge_u",  WT_I32, WT_I32, ALLOC_node_i32_ge_u)
    UN_OP ("i32.eqz",    WT_I32, WT_I32, ALLOC_node_i32_eqz)
    UN_OP ("i32.clz",    WT_I32, WT_I32, ALLOC_node_i32_clz)
    UN_OP ("i32.ctz",    WT_I32, WT_I32, ALLOC_node_i32_ctz)
    UN_OP ("i32.popcnt", WT_I32, WT_I32, ALLOC_node_i32_popcnt)

    // ------- i64 ops -------
    if (tok_is_keyword("i64.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected integer literal");
        uint64_t v = (uint64_t)cur_tok.int_value;
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_i64_const(v), WT_I64};
    }
    BIN_OP("i64.add",   WT_I64, WT_I64, ALLOC_node_i64_add)
    BIN_OP("i64.sub",   WT_I64, WT_I64, ALLOC_node_i64_sub)
    BIN_OP("i64.mul",   WT_I64, WT_I64, ALLOC_node_i64_mul)
    BIN_OP("i64.div_s", WT_I64, WT_I64, ALLOC_node_i64_div_s)
    BIN_OP("i64.div_u", WT_I64, WT_I64, ALLOC_node_i64_div_u)
    BIN_OP("i64.rem_s", WT_I64, WT_I64, ALLOC_node_i64_rem_s)
    BIN_OP("i64.rem_u", WT_I64, WT_I64, ALLOC_node_i64_rem_u)
    BIN_OP("i64.and",   WT_I64, WT_I64, ALLOC_node_i64_and)
    BIN_OP("i64.or",    WT_I64, WT_I64, ALLOC_node_i64_or)
    BIN_OP("i64.xor",   WT_I64, WT_I64, ALLOC_node_i64_xor)
    BIN_OP("i64.shl",   WT_I64, WT_I64, ALLOC_node_i64_shl)
    BIN_OP("i64.shr_s", WT_I64, WT_I64, ALLOC_node_i64_shr_s)
    BIN_OP("i64.shr_u", WT_I64, WT_I64, ALLOC_node_i64_shr_u)
    BIN_OP("i64.rotl",  WT_I64, WT_I64, ALLOC_node_i64_rotl)
    BIN_OP("i64.rotr",  WT_I64, WT_I64, ALLOC_node_i64_rotr)
    BIN_OP("i64.eq",    WT_I64, WT_I32, ALLOC_node_i64_eq)
    BIN_OP("i64.ne",    WT_I64, WT_I32, ALLOC_node_i64_ne)
    BIN_OP("i64.lt_s",  WT_I64, WT_I32, ALLOC_node_i64_lt_s)
    BIN_OP("i64.lt_u",  WT_I64, WT_I32, ALLOC_node_i64_lt_u)
    BIN_OP("i64.le_s",  WT_I64, WT_I32, ALLOC_node_i64_le_s)
    BIN_OP("i64.le_u",  WT_I64, WT_I32, ALLOC_node_i64_le_u)
    BIN_OP("i64.gt_s",  WT_I64, WT_I32, ALLOC_node_i64_gt_s)
    BIN_OP("i64.gt_u",  WT_I64, WT_I32, ALLOC_node_i64_gt_u)
    BIN_OP("i64.ge_s",  WT_I64, WT_I32, ALLOC_node_i64_ge_s)
    BIN_OP("i64.ge_u",  WT_I64, WT_I32, ALLOC_node_i64_ge_u)
    UN_OP ("i64.eqz",    WT_I64, WT_I32, ALLOC_node_i64_eqz)
    UN_OP ("i64.clz",    WT_I64, WT_I64, ALLOC_node_i64_clz)
    UN_OP ("i64.ctz",    WT_I64, WT_I64, ALLOC_node_i64_ctz)
    UN_OP ("i64.popcnt", WT_I64, WT_I64, ALLOC_node_i64_popcnt)

    // ------- f32 ops -------
    if (tok_is_keyword("f32.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected numeric literal");
        double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
        uint32_t bits = token_to_f32_bits(&cur_tok, dv);
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_f32_const(bits), WT_F32};
    }
    BIN_OP("f32.add",      WT_F32, WT_F32, ALLOC_node_f32_add)
    BIN_OP("f32.sub",      WT_F32, WT_F32, ALLOC_node_f32_sub)
    BIN_OP("f32.mul",      WT_F32, WT_F32, ALLOC_node_f32_mul)
    BIN_OP("f32.div",      WT_F32, WT_F32, ALLOC_node_f32_div)
    BIN_OP("f32.min",      WT_F32, WT_F32, ALLOC_node_f32_min)
    BIN_OP("f32.max",      WT_F32, WT_F32, ALLOC_node_f32_max)
    BIN_OP("f32.copysign", WT_F32, WT_F32, ALLOC_node_f32_copysign)
    BIN_OP("f32.eq",       WT_F32, WT_I32, ALLOC_node_f32_eq)
    BIN_OP("f32.ne",       WT_F32, WT_I32, ALLOC_node_f32_ne)
    BIN_OP("f32.lt",       WT_F32, WT_I32, ALLOC_node_f32_lt)
    BIN_OP("f32.le",       WT_F32, WT_I32, ALLOC_node_f32_le)
    BIN_OP("f32.gt",       WT_F32, WT_I32, ALLOC_node_f32_gt)
    BIN_OP("f32.ge",       WT_F32, WT_I32, ALLOC_node_f32_ge)
    UN_OP ("f32.abs",      WT_F32, WT_F32, ALLOC_node_f32_abs)
    UN_OP ("f32.neg",      WT_F32, WT_F32, ALLOC_node_f32_neg)
    UN_OP ("f32.sqrt",     WT_F32, WT_F32, ALLOC_node_f32_sqrt)
    UN_OP ("f32.ceil",     WT_F32, WT_F32, ALLOC_node_f32_ceil)
    UN_OP ("f32.floor",    WT_F32, WT_F32, ALLOC_node_f32_floor)
    UN_OP ("f32.trunc",    WT_F32, WT_F32, ALLOC_node_f32_trunc)
    UN_OP ("f32.nearest",  WT_F32, WT_F32, ALLOC_node_f32_nearest)

    // ------- f64 ops -------
    if (tok_is_keyword("f64.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected numeric literal");
        double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
        uint64_t bits = token_to_f64_bits(&cur_tok, dv);
        double dvb; memcpy(&dvb, &bits, 8);
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_f64_const(dvb), WT_F64};
    }
    BIN_OP("f64.add",      WT_F64, WT_F64, ALLOC_node_f64_add)
    BIN_OP("f64.sub",      WT_F64, WT_F64, ALLOC_node_f64_sub)
    BIN_OP("f64.mul",      WT_F64, WT_F64, ALLOC_node_f64_mul)
    BIN_OP("f64.div",      WT_F64, WT_F64, ALLOC_node_f64_div)
    BIN_OP("f64.min",      WT_F64, WT_F64, ALLOC_node_f64_min)
    BIN_OP("f64.max",      WT_F64, WT_F64, ALLOC_node_f64_max)
    BIN_OP("f64.copysign", WT_F64, WT_F64, ALLOC_node_f64_copysign)
    BIN_OP("f64.eq",       WT_F64, WT_I32, ALLOC_node_f64_eq)
    BIN_OP("f64.ne",       WT_F64, WT_I32, ALLOC_node_f64_ne)
    BIN_OP("f64.lt",       WT_F64, WT_I32, ALLOC_node_f64_lt)
    BIN_OP("f64.le",       WT_F64, WT_I32, ALLOC_node_f64_le)
    BIN_OP("f64.gt",       WT_F64, WT_I32, ALLOC_node_f64_gt)
    BIN_OP("f64.ge",       WT_F64, WT_I32, ALLOC_node_f64_ge)
    UN_OP ("f64.abs",      WT_F64, WT_F64, ALLOC_node_f64_abs)
    UN_OP ("f64.neg",      WT_F64, WT_F64, ALLOC_node_f64_neg)
    UN_OP ("f64.sqrt",     WT_F64, WT_F64, ALLOC_node_f64_sqrt)
    UN_OP ("f64.ceil",     WT_F64, WT_F64, ALLOC_node_f64_ceil)
    UN_OP ("f64.floor",    WT_F64, WT_F64, ALLOC_node_f64_floor)
    UN_OP ("f64.trunc",    WT_F64, WT_F64, ALLOC_node_f64_trunc)
    UN_OP ("f64.nearest",  WT_F64, WT_F64, ALLOC_node_f64_nearest)

    // ------- conversions -------
    UN_OP ("i32.wrap_i64",         WT_I64, WT_I32, ALLOC_node_i32_wrap_i64)
    UN_OP ("i64.extend_i32_s",     WT_I32, WT_I64, ALLOC_node_i64_extend_i32_s)
    UN_OP ("i64.extend_i32_u",     WT_I32, WT_I64, ALLOC_node_i64_extend_i32_u)
    UN_OP ("i32.extend8_s",        WT_I32, WT_I32, ALLOC_node_i32_extend8_s)
    UN_OP ("i32.extend16_s",       WT_I32, WT_I32, ALLOC_node_i32_extend16_s)
    UN_OP ("i64.extend8_s",        WT_I64, WT_I64, ALLOC_node_i64_extend8_s)
    UN_OP ("i64.extend16_s",       WT_I64, WT_I64, ALLOC_node_i64_extend16_s)
    UN_OP ("i64.extend32_s",       WT_I64, WT_I64, ALLOC_node_i64_extend32_s)
    UN_OP ("i32.trunc_f32_s",      WT_F32, WT_I32, ALLOC_node_i32_trunc_f32_s)
    UN_OP ("i32.trunc_f32_u",      WT_F32, WT_I32, ALLOC_node_i32_trunc_f32_u)
    UN_OP ("i32.trunc_f64_s",      WT_F64, WT_I32, ALLOC_node_i32_trunc_f64_s)
    UN_OP ("i32.trunc_f64_u",      WT_F64, WT_I32, ALLOC_node_i32_trunc_f64_u)
    UN_OP ("i64.trunc_f32_s",      WT_F32, WT_I64, ALLOC_node_i64_trunc_f32_s)
    UN_OP ("i64.trunc_f32_u",      WT_F32, WT_I64, ALLOC_node_i64_trunc_f32_u)
    UN_OP ("i64.trunc_f64_s",      WT_F64, WT_I64, ALLOC_node_i64_trunc_f64_s)
    UN_OP ("i64.trunc_f64_u",      WT_F64, WT_I64, ALLOC_node_i64_trunc_f64_u)
    UN_OP ("i32.trunc_sat_f32_s",  WT_F32, WT_I32, ALLOC_node_i32_trunc_sat_f32_s)
    UN_OP ("i32.trunc_sat_f32_u",  WT_F32, WT_I32, ALLOC_node_i32_trunc_sat_f32_u)
    UN_OP ("i32.trunc_sat_f64_s",  WT_F64, WT_I32, ALLOC_node_i32_trunc_sat_f64_s)
    UN_OP ("i32.trunc_sat_f64_u",  WT_F64, WT_I32, ALLOC_node_i32_trunc_sat_f64_u)
    UN_OP ("i64.trunc_sat_f32_s",  WT_F32, WT_I64, ALLOC_node_i64_trunc_sat_f32_s)
    UN_OP ("i64.trunc_sat_f32_u",  WT_F32, WT_I64, ALLOC_node_i64_trunc_sat_f32_u)
    UN_OP ("i64.trunc_sat_f64_s",  WT_F64, WT_I64, ALLOC_node_i64_trunc_sat_f64_s)
    UN_OP ("i64.trunc_sat_f64_u",  WT_F64, WT_I64, ALLOC_node_i64_trunc_sat_f64_u)
    UN_OP ("f32.convert_i32_s",    WT_I32, WT_F32, ALLOC_node_f32_convert_i32_s)
    UN_OP ("f32.convert_i32_u",    WT_I32, WT_F32, ALLOC_node_f32_convert_i32_u)
    UN_OP ("f32.convert_i64_s",    WT_I64, WT_F32, ALLOC_node_f32_convert_i64_s)
    UN_OP ("f32.convert_i64_u",    WT_I64, WT_F32, ALLOC_node_f32_convert_i64_u)
    UN_OP ("f64.convert_i32_s",    WT_I32, WT_F64, ALLOC_node_f64_convert_i32_s)
    UN_OP ("f64.convert_i32_u",    WT_I32, WT_F64, ALLOC_node_f64_convert_i32_u)
    UN_OP ("f64.convert_i64_s",    WT_I64, WT_F64, ALLOC_node_f64_convert_i64_s)
    UN_OP ("f64.convert_i64_u",    WT_I64, WT_F64, ALLOC_node_f64_convert_i64_u)
    UN_OP ("f32.demote_f64",       WT_F64, WT_F32, ALLOC_node_f32_demote_f64)
    UN_OP ("f64.promote_f32",      WT_F32, WT_F64, ALLOC_node_f64_promote_f32)
    UN_OP ("i32.reinterpret_f32",  WT_F32, WT_I32, ALLOC_node_i32_reinterpret_f32)
    UN_OP ("i64.reinterpret_f64",  WT_F64, WT_I64, ALLOC_node_i64_reinterpret_f64)
    UN_OP ("f32.reinterpret_i32",  WT_I32, WT_F32, ALLOC_node_f32_reinterpret_i32)
    UN_OP ("f64.reinterpret_i64",  WT_I64, WT_F64, ALLOC_node_f64_reinterpret_i64)

    // ------- locals (type-erased) -------
    if (tok_is_keyword("local.get")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        expect_rparen();
        return (TypedExpr){alloc_local_get(env->types[idx], (uint32_t)idx), env->types[idx]};
    }
    if (tok_is_keyword("local.set")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        TypedExpr e = parse_expr(env, labels);
        expect_type(e.type, env->types[idx], "local.set value");
        expect_rparen();
        return (TypedExpr){alloc_local_set(env->types[idx], (uint32_t)idx, e.node), WT_VOID};
    }
    if (tok_is_keyword("local.tee")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        TypedExpr e = parse_expr(env, labels);
        expect_type(e.type, env->types[idx], "local.tee value");
        expect_rparen();
        return (TypedExpr){alloc_local_tee(env->types[idx], (uint32_t)idx, e.node), env->types[idx]};
    }

    // ------- memory load/store -------
    // Load instructions take `offset=N` and `align=N` immediates plus
    // an i32 address operand.  We accept and discard the align hint.
    {
        // Helper macro that consumes optional offset=N / align=N and
        // expects the address expr.
#define MEM_LOAD(KW, RES_T, ALLOC)                                  \
        if (tok_is_keyword(KW)) {                                   \
            next_token();                                           \
            uint32_t offset = 0;                                    \
            while (cur_tok.kind == T_KEYWORD) {                     \
                if (cur_tok.len > 7 && memcmp(cur_tok.start, "offset=", 7) == 0) { \
                    offset = (uint32_t)strtoul(cur_tok.start + 7, NULL, 0); \
                    next_token();                                   \
                } else if (cur_tok.len > 6 && memcmp(cur_tok.start, "align=", 6) == 0) { \
                    next_token();   /* discard */                   \
                } else break;                                       \
            }                                                       \
            TypedExpr addr = parse_expr_or_poly(env, labels);       \
            expect_type(addr.type, WT_I32, KW " address");          \
            expect_rparen();                                        \
            return (TypedExpr){ALLOC(offset, addr.node), RES_T};    \
        }
#define MEM_STORE(KW, VAL_T, ALLOC)                                 \
        if (tok_is_keyword(KW)) {                                   \
            next_token();                                           \
            uint32_t offset = 0;                                    \
            while (cur_tok.kind == T_KEYWORD) {                     \
                if (cur_tok.len > 7 && memcmp(cur_tok.start, "offset=", 7) == 0) { \
                    offset = (uint32_t)strtoul(cur_tok.start + 7, NULL, 0); \
                    next_token();                                   \
                } else if (cur_tok.len > 6 && memcmp(cur_tok.start, "align=", 6) == 0) { \
                    next_token();                                   \
                } else break;                                       \
            }                                                       \
            TypedExpr addr  = parse_expr_or_poly(env, labels);      \
            TypedExpr value = parse_expr_or_poly(env, labels);      \
            expect_type(addr.type, WT_I32, KW " address");          \
            expect_type(value.type, VAL_T, KW " value");             \
            expect_rparen();                                        \
            return (TypedExpr){ALLOC(offset, addr.node, value.node), WT_VOID}; \
        }

        MEM_LOAD ("i32.load",     WT_I32, ALLOC_node_i32_load)
        MEM_LOAD ("i32.load8_s",  WT_I32, ALLOC_node_i32_load8_s)
        MEM_LOAD ("i32.load8_u",  WT_I32, ALLOC_node_i32_load8_u)
        MEM_LOAD ("i32.load16_s", WT_I32, ALLOC_node_i32_load16_s)
        MEM_LOAD ("i32.load16_u", WT_I32, ALLOC_node_i32_load16_u)
        MEM_LOAD ("i64.load",     WT_I64, ALLOC_node_i64_load)
        MEM_LOAD ("i64.load8_s",  WT_I64, ALLOC_node_i64_load8_s)
        MEM_LOAD ("i64.load8_u",  WT_I64, ALLOC_node_i64_load8_u)
        MEM_LOAD ("i64.load16_s", WT_I64, ALLOC_node_i64_load16_s)
        MEM_LOAD ("i64.load16_u", WT_I64, ALLOC_node_i64_load16_u)
        MEM_LOAD ("i64.load32_s", WT_I64, ALLOC_node_i64_load32_s)
        MEM_LOAD ("i64.load32_u", WT_I64, ALLOC_node_i64_load32_u)
        MEM_LOAD ("f32.load",     WT_F32, ALLOC_node_f32_load)
        MEM_LOAD ("f64.load",     WT_F64, ALLOC_node_f64_load)
        MEM_STORE("i32.store",    WT_I32, ALLOC_node_i32_store)
        MEM_STORE("i32.store8",   WT_I32, ALLOC_node_i32_store8)
        MEM_STORE("i32.store16",  WT_I32, ALLOC_node_i32_store16)
        MEM_STORE("i64.store",    WT_I64, ALLOC_node_i64_store)
        MEM_STORE("i64.store8",   WT_I64, ALLOC_node_i64_store8)
        MEM_STORE("i64.store16",  WT_I64, ALLOC_node_i64_store16)
        MEM_STORE("i64.store32",  WT_I64, ALLOC_node_i64_store32)
        MEM_STORE("f32.store",    WT_F32, ALLOC_node_f32_store)
        MEM_STORE("f64.store",    WT_F64, ALLOC_node_f64_store)
#undef MEM_LOAD
#undef MEM_STORE
    }
    if (tok_is_keyword("memory.size")) {
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_memory_size(), WT_I32};
    }
    if (tok_is_keyword("memory.grow")) {
        next_token();
        TypedExpr d = parse_expr(env, labels);
        expect_type(d.type, WT_I32, "memory.grow argument");
        expect_rparen();
        return (TypedExpr){ALLOC_node_memory_grow(d.node), WT_I32};
    }

    // ------- globals -------
    if (tok_is_keyword("global.get")) {
        next_token();
        // Lookup global by $name or numeric index.
        int idx = -1;
        if (cur_tok.kind == T_INT) {
            idx = (int)cur_tok.int_value;
        } else if (cur_tok.kind == T_IDENT) {
            for (uint32_t i = 0; i < WASTRO_GLOBAL_CNT; i++) {
                const char *gn = WASTRO_GLOBAL_NAMES[i];
                if (gn && strlen(gn) == cur_tok.len && memcmp(gn, cur_tok.start, cur_tok.len) == 0) {
                    idx = (int)i; break;
                }
            }
        }
        if (idx < 0 || (uint32_t)idx >= WASTRO_GLOBAL_CNT) {
            fprintf(stderr, "wastro: unknown global\n"); exit(1);
        }
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_global_get((uint32_t)idx), WASTRO_GLOBAL_TYPES[idx]};
    }
    if (tok_is_keyword("global.set")) {
        next_token();
        int idx = -1;
        if (cur_tok.kind == T_INT) {
            idx = (int)cur_tok.int_value;
        } else if (cur_tok.kind == T_IDENT) {
            for (uint32_t i = 0; i < WASTRO_GLOBAL_CNT; i++) {
                const char *gn = WASTRO_GLOBAL_NAMES[i];
                if (gn && strlen(gn) == cur_tok.len && memcmp(gn, cur_tok.start, cur_tok.len) == 0) {
                    idx = (int)i; break;
                }
            }
        }
        if (idx < 0 || (uint32_t)idx >= WASTRO_GLOBAL_CNT) {
            fprintf(stderr, "wastro: unknown global\n"); exit(1);
        }
        if (!WASTRO_GLOBAL_MUT[idx]) {
            fprintf(stderr, "wastro: assignment to immutable global\n"); exit(1);
        }
        next_token();
        TypedExpr v = parse_expr(env, labels);
        expect_type(v.type, WASTRO_GLOBAL_TYPES[idx], "global.set value");
        expect_rparen();
        return (TypedExpr){ALLOC_node_global_set((uint32_t)idx, v.node), WT_VOID};
    }

    // ------- traps -------
    if (tok_is_keyword("unreachable")) {
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_unreachable(), WT_POLY};
    }

    // ------- control -------
    if (tok_is_keyword("nop")) {
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_nop(), WT_VOID};
    }
    // drop — evaluate the operand for side effects, discard value.
    if (tok_is_keyword("drop")) {
        next_token();
        TypedExpr e = parse_expr(env, labels);
        expect_rparen();
        return (TypedExpr){ALLOC_node_drop(e.node), WT_VOID};
    }
    // select — `(select <v1> <v2> <cond>)`.  v1 and v2 must have the
    // same type; cond is i32.  Result is v1 if cond != 0, else v2.
    if (tok_is_keyword("select")) {
        next_token();
        // Optional `(result T)` annotation (post-1.0 typed select).
        if (cur_tok.kind == T_LPAREN) {
            const char *save_pos = src_pos;
            Token save_tok = cur_tok;
            next_token();
            if (tok_is_keyword("result")) {
                next_token();
                parse_wtype();   // discard — used as a hint only
                expect_rparen();
            }
            else {
                src_pos = save_pos; cur_tok = save_tok;
            }
        }
        TypedExpr v1 = parse_expr(env, labels);
        TypedExpr v2 = parse_expr(env, labels);
        TypedExpr cond = parse_expr(env, labels);
        expect_type(cond.type, WT_I32, "select condition");
        if (v1.type != v2.type && v1.type != WT_POLY && v2.type != WT_POLY)
            parse_error("select: v1 and v2 type mismatch");
        expect_rparen();
        return (TypedExpr){ALLOC_node_select(v1.node, v2.node, cond.node), v1.type};
    }
    if (tok_is_keyword("if")) {
        next_token();
        TypedExpr r = parse_if(env, labels);
        expect_rparen();
        return r;
    }

    // ------- block / loop / br / br_if / return -------
    // (block (result T)? <expr>+) — labelled scope, br N exits past it.
    // (loop  (result T)? <expr>+) — labelled scope, br N restarts the loop.
    if (tok_is_keyword("block") || tok_is_keyword("loop")) {
        int is_loop = tok_is_keyword("loop");
        next_token();
        // optional $label
        char *label_name = NULL;
        if (cur_tok.kind == T_IDENT) {
            label_name = dup_token_str(&cur_tok);
            next_token();
        }
        // optional (result T)
        wtype_t result_t = WT_VOID;
        if (cur_tok.kind == T_LPAREN) {
            const char *save_pos = src_pos;
            Token save_tok = cur_tok;
            next_token();
            if (tok_is_keyword("result")) {
                next_token();
                result_t = parse_result_type();
                expect_rparen();
            }
            else {
                src_pos = save_pos;
                cur_tok = save_tok;
            }
        }
        // Push label
        if (labels->cnt >= 32) parse_error("too many nested labels");
        labels->names[labels->cnt] = label_name;
        labels->result_types[labels->cnt] = result_t;
        labels->is_loop[labels->cnt] = is_loop;
        labels->cnt++;
        TypedExpr body = parse_seq_until_rparen(env, labels);
        expect_rparen();
        labels->cnt--;
        if (label_name) free(label_name);
        // Validate body's tail type matches the declared result.
        // If no result, allow whatever (the value is discarded).
        if (result_t != WT_VOID) {
            // If body type came out as void (e.g., last stmt was br), that's OK
            // — block's value comes from br_value at runtime.  POLY
            // (from a tail-branching expression) also satisfies any
            // expected result type.
            if (body.type != WT_VOID && body.type != WT_POLY && body.type != result_t) {
                fprintf(stderr,
                    "wastro: type mismatch at %s body: expected %s, got %s\n",
                    is_loop ? "loop" : "block",
                    wtype_name(result_t), wtype_name(body.type));
                if (wastro_parse_active) {
                    snprintf(wastro_parse_message, sizeof(wastro_parse_message),
                             "type mismatch at %s body: expected %s, got %s",
                             is_loop ? "loop" : "block",
                             wtype_name(result_t), wtype_name(body.type));
                    longjmp(wastro_parse_jmp, 1);
                }
                exit(1);
            }
        }
        NODE *node = is_loop ? ALLOC_node_loop(body.node)
                             : ALLOC_node_block(body.node);
        return (TypedExpr){node, result_t};
    }

    // (br $label | N)  / (br $label | N <value>)
    if (tok_is_keyword("br")) {
        next_token();
        uint32_t depth;
        if (!label_env_lookup(labels, &cur_tok, &depth)) parse_error("unknown label in br");
        next_token();
        if (cur_tok.kind == T_RPAREN) {
            expect_rparen();
            return (TypedExpr){ALLOC_node_br(depth), WT_POLY};
        }
        TypedExpr v = parse_expr(env, labels);
        // Multi-value: keep the LAST carry value (which most likely
        // matches the target's declared result type when extras are
        // a post-1.0 multi-value list).
        int saw_multi = 0;
        while (cur_tok.kind == T_LPAREN) {
            v = parse_expr(env, labels);
            saw_multi = 1;
        }
        if (!saw_multi) {
            wtype_t want = labels->result_types[labels->cnt - 1 - depth];
            if (want != WT_VOID) expect_type(v.type, want, "br value");
        }
        expect_rparen();
        return (TypedExpr){ALLOC_node_br_v(depth, v.node), WT_POLY};
    }

    // (br_if $label | N <cond>)  /  (br_if $label | N <value> <cond>)
    if (tok_is_keyword("br_if")) {
        next_token();
        uint32_t depth;
        if (!label_env_lookup(labels, &cur_tok, &depth)) parse_error("unknown label in br_if");
        next_token();
        TypedExpr first = parse_expr(env, labels);
        if (cur_tok.kind == T_RPAREN) {
            expect_type(first.type, WT_I32, "br_if condition");
            expect_rparen();
            return (TypedExpr){ALLOC_node_br_if(depth, first.node), WT_VOID};
        }
        // br_if with value: first is value, second is cond.
        TypedExpr cond = parse_expr(env, labels);
        expect_type(cond.type, WT_I32, "br_if condition");
        wtype_t want = labels->result_types[labels->cnt - 1 - depth];
        if (want != WT_VOID) expect_type(first.type, want, "br_if value");
        expect_rparen();
        return (TypedExpr){ALLOC_node_br_if_v(depth, cond.node, first.node), WT_VOID};
    }

    // (br_table $L0 ... $Ldefault <idx>)
    // (br_table $L0 ... $Ldefault <value> <idx>)
    if (tok_is_keyword("br_table")) {
        next_token();
        // Collect labels.  At least one is required; the last one is the
        // default.  Operands (value/idx) follow.
        uint32_t depths[64]; uint32_t cnt = 0;
        while (cur_tok.kind == T_IDENT || cur_tok.kind == T_INT) {
            if (cnt >= 64) parse_error("br_table: too many labels");
            uint32_t d;
            if (!label_env_lookup(labels, &cur_tok, &d)) parse_error("unknown label in br_table");
            depths[cnt++] = d;
            next_token();
        }
        if (cnt == 0) parse_error("br_table needs at least one label");
        uint32_t default_depth = depths[cnt - 1];
        uint32_t target_cnt = cnt - 1;
        // Allocate slots in WASTRO_BR_TABLE.
        if (WASTRO_BR_TABLE_CNT + target_cnt > WASTRO_BR_TABLE_CAP) {
            WASTRO_BR_TABLE_CAP = WASTRO_BR_TABLE_CAP ? WASTRO_BR_TABLE_CAP * 2 : 64;
            while (WASTRO_BR_TABLE_CAP < WASTRO_BR_TABLE_CNT + target_cnt) WASTRO_BR_TABLE_CAP *= 2;
            WASTRO_BR_TABLE = realloc(WASTRO_BR_TABLE, sizeof(uint32_t) * WASTRO_BR_TABLE_CAP);
        }
        uint32_t target_index = WASTRO_BR_TABLE_CNT;
        for (uint32_t i = 0; i < target_cnt; i++) WASTRO_BR_TABLE[target_index + i] = depths[i];
        WASTRO_BR_TABLE_CNT += target_cnt;

        TypedExpr first = parse_expr(env, labels);
        if (cur_tok.kind == T_RPAREN) {
            // (br_table ... <idx>) — no carried value
            expect_type(first.type, WT_I32, "br_table index");
            expect_rparen();
            return (TypedExpr){
                ALLOC_node_br_table(target_index, target_cnt, default_depth, first.node),
                WT_POLY,
            };
        }
        TypedExpr idx = parse_expr(env, labels);
        expect_type(idx.type, WT_I32, "br_table index");
        expect_rparen();
        return (TypedExpr){
            ALLOC_node_br_table_v(target_index, target_cnt, default_depth, idx.node, first.node),
            WT_POLY,
        };
    }

    // (return)  /  (return <value>)
    if (tok_is_keyword("return")) {
        next_token();
        if (cur_tok.kind == T_RPAREN) {
            expect_rparen();
            return (TypedExpr){ALLOC_node_return(), WT_POLY};
        }
        TypedExpr v = parse_expr(env, labels);
        // Multi-value extras (post-1.0) — discard.
        while (cur_tok.kind == T_LPAREN) (void)parse_expr(env, labels);
        expect_rparen();
        return (TypedExpr){ALLOC_node_return_v(v.node), WT_POLY};
    }

    // ------- call_indirect -------
    // `(call_indirect (type $sig) <args>... <idx>)`.  Wasm 1.0 has a
    // single function table at index 0; we omit the optional table
    // index (always 0).  Type-check is structural at runtime.
    if (tok_is_keyword("call_indirect")) {
        next_token();
        // Optional table reference (`$tab` or numeric index) — wasm 1.0
        // only has one table so we accept-and-ignore.
        if (cur_tok.kind == T_IDENT || cur_tok.kind == T_INT) next_token();
        // (type $sig) — required in 1.0
        expect_lparen();
        expect_keyword("type");
        int type_idx = -1;
        if (cur_tok.kind == T_INT) {
            type_idx = (int)cur_tok.int_value;
        } else if (cur_tok.kind == T_IDENT) {
            for (uint32_t i = 0; i < WASTRO_TYPE_CNT; i++) {
                const char *tn = WASTRO_TYPE_NAMES[i];
                if (tn && strlen(tn) == cur_tok.len && memcmp(tn, cur_tok.start, cur_tok.len) == 0) {
                    type_idx = (int)i; break;
                }
            }
        }
        if (type_idx < 0 || (uint32_t)type_idx >= WASTRO_TYPE_CNT) {
            fprintf(stderr, "wastro: call_indirect: unknown type\n"); exit(1);
        }
        next_token();
        expect_rparen();
        struct wastro_type_sig *sig = &WASTRO_TYPES[type_idx];
        // Optional inline (param ...) and (result ...) forms — these
        // duplicate the (type ...) info in some WAT styles; accept and
        // skip if present.
        while (cur_tok.kind == T_LPAREN) {
            const char *save_pos = src_pos;
            Token save_tok = cur_tok;
            next_token();
            if (tok_is_keyword("param") || tok_is_keyword("result")) {
                // skip the form
                int depth = 1;
                while (cur_tok.kind != T_EOF && depth > 0) {
                    if (cur_tok.kind == T_LPAREN) depth++;
                    else if (cur_tok.kind == T_RPAREN) {
                        depth--;
                        if (depth == 0) { next_token(); break; }
                    }
                    next_token();
                }
            }
            else {
                src_pos = save_pos;
                cur_tok = save_tok;
                break;
            }
        }
        // Args + index.  Args first (count = sig->param_cnt), then idx.
        NODE *args[8]; uint32_t argc = 0;
        // We need to parse exactly sig->param_cnt args, then 1 idx.
        // Rather than counting args eagerly, we parse all expressions
        // until ')' and treat the LAST as the index.
        TypedExpr exprs[16];
        uint32_t en = 0;
        while (cur_tok.kind != T_RPAREN) {
            if (en >= 16) parse_error("call_indirect: too many operands");
            exprs[en++] = parse_expr(env, labels);
        }
        if (en < 1) parse_error("call_indirect requires an index operand");
        // If under-supplied (probably due to a polymorphic-stack
        // operand consuming the rest), pad with poly placeholders.
        while (en < sig->param_cnt + 1 && en < 16) {
            exprs[en++] = (TypedExpr){ALLOC_node_unreachable(), WT_POLY};
        }
        if (en - 1 != sig->param_cnt) {
            fprintf(stderr,
                "wastro: call_indirect: expected %u args + idx, got %u operands\n",
                sig->param_cnt, en);
            if (wastro_parse_active) {
                snprintf(wastro_parse_message, sizeof(wastro_parse_message),
                         "call_indirect arity mismatch");
                longjmp(wastro_parse_jmp, 1);
            }
            exit(1);
        }
        for (uint32_t i = 0; i < sig->param_cnt; i++) {
            if (exprs[i].type != WT_POLY)
                expect_type(exprs[i].type, sig->param_types[i], "call_indirect arg");
            args[argc++] = exprs[i].node;
        }
        if (exprs[en - 1].type != WT_POLY)
            expect_type(exprs[en - 1].type, WT_I32, "call_indirect index");
        NODE *idx = exprs[en - 1].node;
        expect_rparen();
        NODE *call_node;
        switch (argc) {
        case 0: call_node = ALLOC_node_call_indirect_0((uint32_t)type_idx, idx); break;
        case 1: call_node = ALLOC_node_call_indirect_1((uint32_t)type_idx, idx, args[0]); break;
        case 2: call_node = ALLOC_node_call_indirect_2((uint32_t)type_idx, idx, args[0], args[1]); break;
        case 3: call_node = ALLOC_node_call_indirect_3((uint32_t)type_idx, idx, args[0], args[1], args[2]); break;
        case 4: call_node = ALLOC_node_call_indirect_4((uint32_t)type_idx, idx, args[0], args[1], args[2], args[3]); break;
        default:
            parse_error("call_indirect arity > 4 not supported yet");
            return (TypedExpr){NULL, WT_VOID};
        }
        return (TypedExpr){call_node, sig->result_type};
    }

    // ------- call -------
    if (tok_is_keyword("call")) {
        next_token();
        int func_idx = resolve_func(&cur_tok);
        struct wastro_function *callee = &WASTRO_FUNCS[func_idx];
        next_token();
        NODE *args[8]; uint32_t argc = 0;
        int saw_poly = 0;
        while (cur_tok.kind != T_RPAREN) {
            if (argc >= 8) parse_error("call arity > 8 not supported");
            TypedExpr a = parse_expr(env, labels);
            if (a.type == WT_POLY) saw_poly = 1;
            if (argc >= callee->param_cnt) {
                // Spec testsuite cases pass extra operands when they
                // are downstream of a polymorphic-stack instruction;
                // tolerate by ignoring extras.
                continue;
            }
            if (a.type != WT_POLY)
                expect_type(a.type, callee->param_types[argc], "call argument");
            args[argc++] = a.node;
        }
        // Pad missing args with unreachable placeholders if we crossed
        // a polymorphic-stack barrier (e.g. (call $f (br 0))).
        if (argc < callee->param_cnt) {
            if (!saw_poly) {
                fprintf(stderr, "wastro: '%s' expects %u arg(s), got %u\n",
                        callee->name ? callee->name : "<unnamed>",
                        callee->param_cnt, argc);
                if (wastro_parse_active) {
                    snprintf(wastro_parse_message, sizeof(wastro_parse_message),
                             "'%s' expects %u arg(s), got %u",
                             callee->name ? callee->name : "<unnamed>",
                             callee->param_cnt, argc);
                    longjmp(wastro_parse_jmp, 1);
                }
                exit(1);
            }
            while (argc < callee->param_cnt) {
                args[argc++] = ALLOC_node_unreachable();
            }
        }
        expect_rparen();
        NODE *call_node;
        if (callee->is_import) {
            switch (argc) {
            case 0: call_node = ALLOC_node_host_call_0((uint32_t)func_idx); break;
            case 1: call_node = ALLOC_node_host_call_1((uint32_t)func_idx, args[0]); break;
            case 2: call_node = ALLOC_node_host_call_2((uint32_t)func_idx, args[0], args[1]); break;
            case 3: call_node = ALLOC_node_host_call_3((uint32_t)func_idx, args[0], args[1], args[2]); break;
            default:
                parse_error("host call arity > 3 not supported");
                return (TypedExpr){NULL, WT_VOID};
            }
        }
        else {
            uint32_t local_cnt = callee->local_cnt;
            NODE *body = WASTRO_FUNCS[func_idx].body;  // may be NULL if forward ref
            switch (argc) {
            case 0: call_node = ALLOC_node_call_0((uint32_t)func_idx, local_cnt, body); break;
            case 1: call_node = ALLOC_node_call_1((uint32_t)func_idx, local_cnt, args[0], body); break;
            case 2: call_node = ALLOC_node_call_2((uint32_t)func_idx, local_cnt, args[0], args[1], body); break;
            case 3: call_node = ALLOC_node_call_3((uint32_t)func_idx, local_cnt, args[0], args[1], args[2], body); break;
            case 4: call_node = ALLOC_node_call_4((uint32_t)func_idx, local_cnt, args[0], args[1], args[2], args[3], body); break;
            default:
                parse_error("call arity 5..8 needs node_call_5..8");
                return (TypedExpr){NULL, WT_VOID};
            }
            register_call_body_fixup(call_node, (uint32_t)func_idx, (uint8_t)argc);
        }
        return (TypedExpr){call_node, callee->result_type};
    }

    parse_error("unknown operator");
    return (TypedExpr){NULL, WT_VOID};
}

#undef BIN_OP

static TypedExpr
parse_expr(LocalEnv *env, LabelEnv *labels)
{
    // Polymorphic-stack rule: if a previous tail-branching instr
    // (br / return / unreachable) emitted POLY, downstream operand
    // slots may simply be missing in folded WAT (the validator
    // accepts it because the stack is statically dead).  Synthesize
    // a polymorphic placeholder when we hit a closing paren where an
    // operand was expected.
    if (cur_tok.kind == T_RPAREN) {
        return (TypedExpr){ALLOC_node_unreachable(), WT_POLY};
    }
    expect_lparen();
    return parse_op(env, labels);
}

// =====================================================================
// Inline (export ...) / (import ...) helpers
// =====================================================================
//
// WAT 1.0 lets memory/table/global/func declarations carry their
// export and import bindings inline:
//
//   (memory $name (export "m") (import "env" "mem") 1 16)
//   (table $name (export "t") funcref (elem $f0 $f1))
//   (global $name (export "g") (mut i32) (i32.const 0))
//   (func $name (import "env" "fn") (param i32) (result i32))
//
// We parse `(export ...)` and `(import ...)` as zero-or-more leading
// inline declarations.  Helpers return 1 if they consumed one and 0
// otherwise, leaving cur_tok at the next token to inspect.

// Try `(export "name")`.  Returns 1 if consumed; *out is dup'd name.
static int
try_inline_export(char **out)
{
    if (cur_tok.kind != T_LPAREN) return 0;
    const char *save_pos = src_pos;
    Token save_tok = cur_tok;
    next_token();
    if (!tok_is_keyword("export")) {
        src_pos = save_pos; cur_tok = save_tok;
        return 0;
    }
    next_token();
    if (cur_tok.kind != T_STRING) parse_error("(export ...) expects name string");
    *out = dup_token_str(&cur_tok);
    next_token();
    expect_rparen();
    return 1;
}

// Try `(import "mod" "fld")`.  Returns 1 if consumed.  out_mod/out_fld
// must be at least 64 bytes each.
static int
try_inline_import(char *out_mod, char *out_fld)
{
    if (cur_tok.kind != T_LPAREN) return 0;
    const char *save_pos = src_pos;
    Token save_tok = cur_tok;
    next_token();
    if (!tok_is_keyword("import")) {
        src_pos = save_pos; cur_tok = save_tok;
        return 0;
    }
    next_token();
    if (cur_tok.kind != T_STRING) parse_error("(import ...) expects module string");
    size_t ml = cur_tok.len < 63 ? cur_tok.len : 63;
    memcpy(out_mod, cur_tok.start, ml); out_mod[ml] = 0;
    next_token();
    if (cur_tok.kind != T_STRING) parse_error("(import ...) expects field string");
    size_t fl = cur_tok.len < 63 ? cur_tok.len : 63;
    memcpy(out_fld, cur_tok.start, fl); out_fld[fl] = 0;
    next_token();
    expect_rparen();
    return 1;
}

// =====================================================================
// Stack-style WAT support
// =====================================================================
//
// In stack-style WAT, instructions appear bare (no surrounding parens)
// and operands flow via an implicit operand stack.  We track that
// stack at parse time, popping when an instr consumes operands and
// pushing the resulting NODE.  Folded `(...)` sub-expressions (used
// inside `parse_expr`) are handled by the existing recursive parser
// and their result is pushed onto the same operand stack.
//
// Limitation: this parser does NOT perform let-floating to preserve
// side-effect ordering when a void instr is encountered while values
// remain on the stack.  Real-world compiler output and the bulk of
// the spec testsuite stay within "consume what you push" patterns and
// are unaffected.  For pathological inputs we trust the wasm
// validator's typing pre-conditions.

typedef struct {
    TypedExpr items[256];
    uint32_t cnt;
} OpStack;

static void
op_push(OpStack *s, NODE *n, wtype_t t)
{
    if (s->cnt >= 256) parse_error("operand stack overflow");
    s->items[s->cnt].node = n;
    s->items[s->cnt].type = t;
    s->cnt++;
}

static TypedExpr
op_pop(OpStack *s, wtype_t want, const char *site)
{
    if (s->cnt == 0) {
        // Stack underflow under the polymorphic-stack rule: synthesize
        // a polymorphic value.  This appears in the spec testsuite
        // where br / return are followed by instructions whose stack
        // effect is statically dead but the validator still types
        // permissively.  The synthesized node is `unreachable` so
        // that any erroneous fall-through traps cleanly.
        TypedExpr e = { ALLOC_node_unreachable(), WT_POLY };
        return e;
    }
    TypedExpr e = s->items[--s->cnt];
    if (e.type == WT_POLY) return e;
    if (want != WT_VOID && want != WT_POLY && e.type != want) {
        fprintf(stderr, "wastro: %s: expected %s, got %s\n",
                site, wtype_name(want), wtype_name(e.type));
        if (wastro_parse_active) {
            snprintf(wastro_parse_message, sizeof(wastro_parse_message),
                     "%s: expected %s, got %s",
                     site, wtype_name(want), wtype_name(e.type));
            longjmp(wastro_parse_jmp, 1);
        }
        exit(1);
    }
    return e;
}

typedef struct {
    NODE *items[1024];
    uint32_t cnt;
} StmtList;

static void
stmts_append(StmtList *l, NODE *n)
{
    if (l->cnt >= 1024) parse_error("too many statements in body");
    l->items[l->cnt++] = n;
}

// Build a body NODE from accumulated statements + optional final
// stack-top value.  Statements run in order; the final value (if any)
// becomes the body's result.
static NODE *
build_body_node(StmtList *L, NODE *final_val)
{
    if (L->cnt == 0) {
        return final_val ? final_val : ALLOC_node_nop();
    }
    NODE *acc = final_val ? final_val : L->items[L->cnt - 1];
    int start = final_val ? (int)L->cnt - 1 : (int)L->cnt - 2;
    for (int i = start; i >= 0; i--) {
        acc = ALLOC_node_seq(L->items[i], acc);
    }
    return acc;
}

// Forward declarations.
static void parse_bare_instr(LocalEnv *env, LabelEnv *labels, OpStack *S, StmtList *L);
static TypedExpr parse_body_seq(LocalEnv *env, LabelEnv *labels, int allow_else_terminator, int *out_else);

// Generic stack-style binary op: pop r:OPND, pop l:OPND, push (op l r):RES.
#define STACK_BIN(KW, OPND_T, RES_T, ALLOC) \
    if (tok_is_keyword(KW)) { \
        next_token(); \
        TypedExpr r = op_pop(S, OPND_T, KW " right"); \
        TypedExpr l = op_pop(S, OPND_T, KW " left"); \
        op_push(S, ALLOC(l.node, r.node), RES_T); \
        return; \
    }
#define STACK_UN(KW, OPND_T, RES_T, ALLOC) \
    if (tok_is_keyword(KW)) { \
        next_token(); \
        TypedExpr e = op_pop(S, OPND_T, KW " operand"); \
        op_push(S, ALLOC(e.node), RES_T); \
        return; \
    }

// Parse one bare-keyword stack-style instruction.  cur_tok is at the
// keyword.  Updates S (operand stack) and L (statement list).
static void
parse_bare_instr(LocalEnv *env, LabelEnv *labels, OpStack *S, StmtList *L)
{
    if (cur_tok.kind != T_KEYWORD) parse_error("expected instruction keyword");

    // ---- consts ----
    if (tok_is_keyword("i32.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected i32 literal");
        int32_t v = (int32_t)cur_tok.int_value;
        next_token();
        op_push(S, ALLOC_node_i32_const(v), WT_I32);
        return;
    }
    if (tok_is_keyword("i64.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected i64 literal");
        uint64_t v = (uint64_t)cur_tok.int_value;
        next_token();
        op_push(S, ALLOC_node_i64_const(v), WT_I64);
        return;
    }
    if (tok_is_keyword("f32.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected f32 literal");
        double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
        uint32_t bits = token_to_f32_bits(&cur_tok, dv);
        next_token();
        op_push(S, ALLOC_node_f32_const(bits), WT_F32);
        return;
    }
    if (tok_is_keyword("f64.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected f64 literal");
        double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
        uint64_t bits = token_to_f64_bits(&cur_tok, dv);
        double dvb; memcpy(&dvb, &bits, 8);
        next_token();
        op_push(S, ALLOC_node_f64_const(dvb), WT_F64);
        return;
    }

    // ---- numeric ops ----
    STACK_BIN("i32.add",   WT_I32, WT_I32, ALLOC_node_i32_add)
    STACK_BIN("i32.sub",   WT_I32, WT_I32, ALLOC_node_i32_sub)
    STACK_BIN("i32.mul",   WT_I32, WT_I32, ALLOC_node_i32_mul)
    STACK_BIN("i32.div_s", WT_I32, WT_I32, ALLOC_node_i32_div_s)
    STACK_BIN("i32.div_u", WT_I32, WT_I32, ALLOC_node_i32_div_u)
    STACK_BIN("i32.rem_s", WT_I32, WT_I32, ALLOC_node_i32_rem_s)
    STACK_BIN("i32.rem_u", WT_I32, WT_I32, ALLOC_node_i32_rem_u)
    STACK_BIN("i32.and",   WT_I32, WT_I32, ALLOC_node_i32_and)
    STACK_BIN("i32.or",    WT_I32, WT_I32, ALLOC_node_i32_or)
    STACK_BIN("i32.xor",   WT_I32, WT_I32, ALLOC_node_i32_xor)
    STACK_BIN("i32.shl",   WT_I32, WT_I32, ALLOC_node_i32_shl)
    STACK_BIN("i32.shr_s", WT_I32, WT_I32, ALLOC_node_i32_shr_s)
    STACK_BIN("i32.shr_u", WT_I32, WT_I32, ALLOC_node_i32_shr_u)
    STACK_BIN("i32.rotl",  WT_I32, WT_I32, ALLOC_node_i32_rotl)
    STACK_BIN("i32.rotr",  WT_I32, WT_I32, ALLOC_node_i32_rotr)
    STACK_BIN("i32.eq",    WT_I32, WT_I32, ALLOC_node_i32_eq)
    STACK_BIN("i32.ne",    WT_I32, WT_I32, ALLOC_node_i32_ne)
    STACK_BIN("i32.lt_s",  WT_I32, WT_I32, ALLOC_node_i32_lt_s)
    STACK_BIN("i32.lt_u",  WT_I32, WT_I32, ALLOC_node_i32_lt_u)
    STACK_BIN("i32.le_s",  WT_I32, WT_I32, ALLOC_node_i32_le_s)
    STACK_BIN("i32.le_u",  WT_I32, WT_I32, ALLOC_node_i32_le_u)
    STACK_BIN("i32.gt_s",  WT_I32, WT_I32, ALLOC_node_i32_gt_s)
    STACK_BIN("i32.gt_u",  WT_I32, WT_I32, ALLOC_node_i32_gt_u)
    STACK_BIN("i32.ge_s",  WT_I32, WT_I32, ALLOC_node_i32_ge_s)
    STACK_BIN("i32.ge_u",  WT_I32, WT_I32, ALLOC_node_i32_ge_u)
    STACK_UN ("i32.eqz",    WT_I32, WT_I32, ALLOC_node_i32_eqz)
    STACK_UN ("i32.clz",    WT_I32, WT_I32, ALLOC_node_i32_clz)
    STACK_UN ("i32.ctz",    WT_I32, WT_I32, ALLOC_node_i32_ctz)
    STACK_UN ("i32.popcnt", WT_I32, WT_I32, ALLOC_node_i32_popcnt)

    STACK_BIN("i64.add",   WT_I64, WT_I64, ALLOC_node_i64_add)
    STACK_BIN("i64.sub",   WT_I64, WT_I64, ALLOC_node_i64_sub)
    STACK_BIN("i64.mul",   WT_I64, WT_I64, ALLOC_node_i64_mul)
    STACK_BIN("i64.div_s", WT_I64, WT_I64, ALLOC_node_i64_div_s)
    STACK_BIN("i64.div_u", WT_I64, WT_I64, ALLOC_node_i64_div_u)
    STACK_BIN("i64.rem_s", WT_I64, WT_I64, ALLOC_node_i64_rem_s)
    STACK_BIN("i64.rem_u", WT_I64, WT_I64, ALLOC_node_i64_rem_u)
    STACK_BIN("i64.and",   WT_I64, WT_I64, ALLOC_node_i64_and)
    STACK_BIN("i64.or",    WT_I64, WT_I64, ALLOC_node_i64_or)
    STACK_BIN("i64.xor",   WT_I64, WT_I64, ALLOC_node_i64_xor)
    STACK_BIN("i64.shl",   WT_I64, WT_I64, ALLOC_node_i64_shl)
    STACK_BIN("i64.shr_s", WT_I64, WT_I64, ALLOC_node_i64_shr_s)
    STACK_BIN("i64.shr_u", WT_I64, WT_I64, ALLOC_node_i64_shr_u)
    STACK_BIN("i64.rotl",  WT_I64, WT_I64, ALLOC_node_i64_rotl)
    STACK_BIN("i64.rotr",  WT_I64, WT_I64, ALLOC_node_i64_rotr)
    STACK_BIN("i64.eq",    WT_I64, WT_I32, ALLOC_node_i64_eq)
    STACK_BIN("i64.ne",    WT_I64, WT_I32, ALLOC_node_i64_ne)
    STACK_BIN("i64.lt_s",  WT_I64, WT_I32, ALLOC_node_i64_lt_s)
    STACK_BIN("i64.lt_u",  WT_I64, WT_I32, ALLOC_node_i64_lt_u)
    STACK_BIN("i64.le_s",  WT_I64, WT_I32, ALLOC_node_i64_le_s)
    STACK_BIN("i64.le_u",  WT_I64, WT_I32, ALLOC_node_i64_le_u)
    STACK_BIN("i64.gt_s",  WT_I64, WT_I32, ALLOC_node_i64_gt_s)
    STACK_BIN("i64.gt_u",  WT_I64, WT_I32, ALLOC_node_i64_gt_u)
    STACK_BIN("i64.ge_s",  WT_I64, WT_I32, ALLOC_node_i64_ge_s)
    STACK_BIN("i64.ge_u",  WT_I64, WT_I32, ALLOC_node_i64_ge_u)
    STACK_UN ("i64.eqz",    WT_I64, WT_I32, ALLOC_node_i64_eqz)
    STACK_UN ("i64.clz",    WT_I64, WT_I64, ALLOC_node_i64_clz)
    STACK_UN ("i64.ctz",    WT_I64, WT_I64, ALLOC_node_i64_ctz)
    STACK_UN ("i64.popcnt", WT_I64, WT_I64, ALLOC_node_i64_popcnt)

    STACK_BIN("f32.add",      WT_F32, WT_F32, ALLOC_node_f32_add)
    STACK_BIN("f32.sub",      WT_F32, WT_F32, ALLOC_node_f32_sub)
    STACK_BIN("f32.mul",      WT_F32, WT_F32, ALLOC_node_f32_mul)
    STACK_BIN("f32.div",      WT_F32, WT_F32, ALLOC_node_f32_div)
    STACK_BIN("f32.min",      WT_F32, WT_F32, ALLOC_node_f32_min)
    STACK_BIN("f32.max",      WT_F32, WT_F32, ALLOC_node_f32_max)
    STACK_BIN("f32.copysign", WT_F32, WT_F32, ALLOC_node_f32_copysign)
    STACK_BIN("f32.eq",       WT_F32, WT_I32, ALLOC_node_f32_eq)
    STACK_BIN("f32.ne",       WT_F32, WT_I32, ALLOC_node_f32_ne)
    STACK_BIN("f32.lt",       WT_F32, WT_I32, ALLOC_node_f32_lt)
    STACK_BIN("f32.le",       WT_F32, WT_I32, ALLOC_node_f32_le)
    STACK_BIN("f32.gt",       WT_F32, WT_I32, ALLOC_node_f32_gt)
    STACK_BIN("f32.ge",       WT_F32, WT_I32, ALLOC_node_f32_ge)
    STACK_UN ("f32.abs",      WT_F32, WT_F32, ALLOC_node_f32_abs)
    STACK_UN ("f32.neg",      WT_F32, WT_F32, ALLOC_node_f32_neg)
    STACK_UN ("f32.sqrt",     WT_F32, WT_F32, ALLOC_node_f32_sqrt)
    STACK_UN ("f32.ceil",     WT_F32, WT_F32, ALLOC_node_f32_ceil)
    STACK_UN ("f32.floor",    WT_F32, WT_F32, ALLOC_node_f32_floor)
    STACK_UN ("f32.trunc",    WT_F32, WT_F32, ALLOC_node_f32_trunc)
    STACK_UN ("f32.nearest",  WT_F32, WT_F32, ALLOC_node_f32_nearest)

    STACK_BIN("f64.add",      WT_F64, WT_F64, ALLOC_node_f64_add)
    STACK_BIN("f64.sub",      WT_F64, WT_F64, ALLOC_node_f64_sub)
    STACK_BIN("f64.mul",      WT_F64, WT_F64, ALLOC_node_f64_mul)
    STACK_BIN("f64.div",      WT_F64, WT_F64, ALLOC_node_f64_div)
    STACK_BIN("f64.min",      WT_F64, WT_F64, ALLOC_node_f64_min)
    STACK_BIN("f64.max",      WT_F64, WT_F64, ALLOC_node_f64_max)
    STACK_BIN("f64.copysign", WT_F64, WT_F64, ALLOC_node_f64_copysign)
    STACK_BIN("f64.eq",       WT_F64, WT_I32, ALLOC_node_f64_eq)
    STACK_BIN("f64.ne",       WT_F64, WT_I32, ALLOC_node_f64_ne)
    STACK_BIN("f64.lt",       WT_F64, WT_I32, ALLOC_node_f64_lt)
    STACK_BIN("f64.le",       WT_F64, WT_I32, ALLOC_node_f64_le)
    STACK_BIN("f64.gt",       WT_F64, WT_I32, ALLOC_node_f64_gt)
    STACK_BIN("f64.ge",       WT_F64, WT_I32, ALLOC_node_f64_ge)
    STACK_UN ("f64.abs",      WT_F64, WT_F64, ALLOC_node_f64_abs)
    STACK_UN ("f64.neg",      WT_F64, WT_F64, ALLOC_node_f64_neg)
    STACK_UN ("f64.sqrt",     WT_F64, WT_F64, ALLOC_node_f64_sqrt)
    STACK_UN ("f64.ceil",     WT_F64, WT_F64, ALLOC_node_f64_ceil)
    STACK_UN ("f64.floor",    WT_F64, WT_F64, ALLOC_node_f64_floor)
    STACK_UN ("f64.trunc",    WT_F64, WT_F64, ALLOC_node_f64_trunc)
    STACK_UN ("f64.nearest",  WT_F64, WT_F64, ALLOC_node_f64_nearest)

    // ---- conversions ----
    STACK_UN("i32.wrap_i64",        WT_I64, WT_I32, ALLOC_node_i32_wrap_i64)
    STACK_UN("i64.extend_i32_s",    WT_I32, WT_I64, ALLOC_node_i64_extend_i32_s)
    STACK_UN("i64.extend_i32_u",    WT_I32, WT_I64, ALLOC_node_i64_extend_i32_u)
    STACK_UN("i32.extend8_s",       WT_I32, WT_I32, ALLOC_node_i32_extend8_s)
    STACK_UN("i32.extend16_s",      WT_I32, WT_I32, ALLOC_node_i32_extend16_s)
    STACK_UN("i64.extend8_s",       WT_I64, WT_I64, ALLOC_node_i64_extend8_s)
    STACK_UN("i64.extend16_s",      WT_I64, WT_I64, ALLOC_node_i64_extend16_s)
    STACK_UN("i64.extend32_s",      WT_I64, WT_I64, ALLOC_node_i64_extend32_s)
    STACK_UN("i32.trunc_f32_s",     WT_F32, WT_I32, ALLOC_node_i32_trunc_f32_s)
    STACK_UN("i32.trunc_f32_u",     WT_F32, WT_I32, ALLOC_node_i32_trunc_f32_u)
    STACK_UN("i32.trunc_f64_s",     WT_F64, WT_I32, ALLOC_node_i32_trunc_f64_s)
    STACK_UN("i32.trunc_f64_u",     WT_F64, WT_I32, ALLOC_node_i32_trunc_f64_u)
    STACK_UN("i64.trunc_f32_s",     WT_F32, WT_I64, ALLOC_node_i64_trunc_f32_s)
    STACK_UN("i64.trunc_f32_u",     WT_F32, WT_I64, ALLOC_node_i64_trunc_f32_u)
    STACK_UN("i64.trunc_f64_s",     WT_F64, WT_I64, ALLOC_node_i64_trunc_f64_s)
    STACK_UN("i64.trunc_f64_u",     WT_F64, WT_I64, ALLOC_node_i64_trunc_f64_u)
    STACK_UN("i32.trunc_sat_f32_s", WT_F32, WT_I32, ALLOC_node_i32_trunc_sat_f32_s)
    STACK_UN("i32.trunc_sat_f32_u", WT_F32, WT_I32, ALLOC_node_i32_trunc_sat_f32_u)
    STACK_UN("i32.trunc_sat_f64_s", WT_F64, WT_I32, ALLOC_node_i32_trunc_sat_f64_s)
    STACK_UN("i32.trunc_sat_f64_u", WT_F64, WT_I32, ALLOC_node_i32_trunc_sat_f64_u)
    STACK_UN("i64.trunc_sat_f32_s", WT_F32, WT_I64, ALLOC_node_i64_trunc_sat_f32_s)
    STACK_UN("i64.trunc_sat_f32_u", WT_F32, WT_I64, ALLOC_node_i64_trunc_sat_f32_u)
    STACK_UN("i64.trunc_sat_f64_s", WT_F64, WT_I64, ALLOC_node_i64_trunc_sat_f64_s)
    STACK_UN("i64.trunc_sat_f64_u", WT_F64, WT_I64, ALLOC_node_i64_trunc_sat_f64_u)
    STACK_UN("f32.convert_i32_s",   WT_I32, WT_F32, ALLOC_node_f32_convert_i32_s)
    STACK_UN("f32.convert_i32_u",   WT_I32, WT_F32, ALLOC_node_f32_convert_i32_u)
    STACK_UN("f32.convert_i64_s",   WT_I64, WT_F32, ALLOC_node_f32_convert_i64_s)
    STACK_UN("f32.convert_i64_u",   WT_I64, WT_F32, ALLOC_node_f32_convert_i64_u)
    STACK_UN("f64.convert_i32_s",   WT_I32, WT_F64, ALLOC_node_f64_convert_i32_s)
    STACK_UN("f64.convert_i32_u",   WT_I32, WT_F64, ALLOC_node_f64_convert_i32_u)
    STACK_UN("f64.convert_i64_s",   WT_I64, WT_F64, ALLOC_node_f64_convert_i64_s)
    STACK_UN("f64.convert_i64_u",   WT_I64, WT_F64, ALLOC_node_f64_convert_i64_u)
    STACK_UN("f32.demote_f64",      WT_F64, WT_F32, ALLOC_node_f32_demote_f64)
    STACK_UN("f64.promote_f32",     WT_F32, WT_F64, ALLOC_node_f64_promote_f32)
    STACK_UN("i32.reinterpret_f32", WT_F32, WT_I32, ALLOC_node_i32_reinterpret_f32)
    STACK_UN("i64.reinterpret_f64", WT_F64, WT_I64, ALLOC_node_i64_reinterpret_f64)
    STACK_UN("f32.reinterpret_i32", WT_I32, WT_F32, ALLOC_node_f32_reinterpret_i32)
    STACK_UN("f64.reinterpret_i64", WT_I64, WT_F64, ALLOC_node_f64_reinterpret_i64)

    // ---- locals / globals ----
    if (tok_is_keyword("local.get")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        op_push(S, alloc_local_get(env->types[idx], (uint32_t)idx), env->types[idx]);
        return;
    }
    if (tok_is_keyword("local.set")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        TypedExpr e = op_pop(S, env->types[idx], "local.set value");
        stmts_append(L, alloc_local_set(env->types[idx], (uint32_t)idx, e.node));
        return;
    }
    if (tok_is_keyword("local.tee")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        TypedExpr e = op_pop(S, env->types[idx], "local.tee value");
        op_push(S, alloc_local_tee(env->types[idx], (uint32_t)idx, e.node), env->types[idx]);
        return;
    }
    if (tok_is_keyword("global.get")) {
        next_token();
        int idx = -1;
        if (cur_tok.kind == T_INT) idx = (int)cur_tok.int_value;
        else if (cur_tok.kind == T_IDENT) {
            for (uint32_t i = 0; i < WASTRO_GLOBAL_CNT; i++) {
                const char *gn = WASTRO_GLOBAL_NAMES[i];
                if (gn && strlen(gn) == cur_tok.len && memcmp(gn, cur_tok.start, cur_tok.len) == 0) { idx = (int)i; break; }
            }
        }
        if (idx < 0 || (uint32_t)idx >= WASTRO_GLOBAL_CNT) parse_error("unknown global");
        next_token();
        op_push(S, ALLOC_node_global_get((uint32_t)idx), WASTRO_GLOBAL_TYPES[idx]);
        return;
    }
    if (tok_is_keyword("global.set")) {
        next_token();
        int idx = -1;
        if (cur_tok.kind == T_INT) idx = (int)cur_tok.int_value;
        else if (cur_tok.kind == T_IDENT) {
            for (uint32_t i = 0; i < WASTRO_GLOBAL_CNT; i++) {
                const char *gn = WASTRO_GLOBAL_NAMES[i];
                if (gn && strlen(gn) == cur_tok.len && memcmp(gn, cur_tok.start, cur_tok.len) == 0) { idx = (int)i; break; }
            }
        }
        if (idx < 0 || (uint32_t)idx >= WASTRO_GLOBAL_CNT) parse_error("unknown global");
        if (!WASTRO_GLOBAL_MUT[idx]) parse_error("assignment to immutable global");
        next_token();
        TypedExpr e = op_pop(S, WASTRO_GLOBAL_TYPES[idx], "global.set value");
        stmts_append(L, ALLOC_node_global_set((uint32_t)idx, e.node));
        return;
    }

    // ---- memory ops ----
    // Helper: consume optional offset=N / align=N immediates.
#define STACK_LOAD(KW, RES_T, ALLOC) \
    if (tok_is_keyword(KW)) { \
        next_token(); \
        uint32_t offset = 0; \
        while (cur_tok.kind == T_KEYWORD) { \
            if (cur_tok.len > 7 && memcmp(cur_tok.start, "offset=", 7) == 0) { offset = (uint32_t)strtoul(cur_tok.start + 7, NULL, 0); next_token(); } \
            else if (cur_tok.len > 6 && memcmp(cur_tok.start, "align=", 6) == 0) { next_token(); } \
            else break; \
        } \
        TypedExpr addr = op_pop(S, WT_I32, KW " address"); \
        op_push(S, ALLOC(offset, addr.node), RES_T); \
        return; \
    }
#define STACK_STORE(KW, VAL_T, ALLOC) \
    if (tok_is_keyword(KW)) { \
        next_token(); \
        uint32_t offset = 0; \
        while (cur_tok.kind == T_KEYWORD) { \
            if (cur_tok.len > 7 && memcmp(cur_tok.start, "offset=", 7) == 0) { offset = (uint32_t)strtoul(cur_tok.start + 7, NULL, 0); next_token(); } \
            else if (cur_tok.len > 6 && memcmp(cur_tok.start, "align=", 6) == 0) { next_token(); } \
            else break; \
        } \
        TypedExpr value = op_pop(S, VAL_T, KW " value"); \
        TypedExpr addr  = op_pop(S, WT_I32, KW " address"); \
        stmts_append(L, ALLOC(offset, addr.node, value.node)); \
        return; \
    }
    STACK_LOAD ("i32.load",     WT_I32, ALLOC_node_i32_load)
    STACK_LOAD ("i32.load8_s",  WT_I32, ALLOC_node_i32_load8_s)
    STACK_LOAD ("i32.load8_u",  WT_I32, ALLOC_node_i32_load8_u)
    STACK_LOAD ("i32.load16_s", WT_I32, ALLOC_node_i32_load16_s)
    STACK_LOAD ("i32.load16_u", WT_I32, ALLOC_node_i32_load16_u)
    STACK_LOAD ("i64.load",     WT_I64, ALLOC_node_i64_load)
    STACK_LOAD ("i64.load8_s",  WT_I64, ALLOC_node_i64_load8_s)
    STACK_LOAD ("i64.load8_u",  WT_I64, ALLOC_node_i64_load8_u)
    STACK_LOAD ("i64.load16_s", WT_I64, ALLOC_node_i64_load16_s)
    STACK_LOAD ("i64.load16_u", WT_I64, ALLOC_node_i64_load16_u)
    STACK_LOAD ("i64.load32_s", WT_I64, ALLOC_node_i64_load32_s)
    STACK_LOAD ("i64.load32_u", WT_I64, ALLOC_node_i64_load32_u)
    STACK_LOAD ("f32.load",     WT_F32, ALLOC_node_f32_load)
    STACK_LOAD ("f64.load",     WT_F64, ALLOC_node_f64_load)
    STACK_STORE("i32.store",    WT_I32, ALLOC_node_i32_store)
    STACK_STORE("i32.store8",   WT_I32, ALLOC_node_i32_store8)
    STACK_STORE("i32.store16",  WT_I32, ALLOC_node_i32_store16)
    STACK_STORE("i64.store",    WT_I64, ALLOC_node_i64_store)
    STACK_STORE("i64.store8",   WT_I64, ALLOC_node_i64_store8)
    STACK_STORE("i64.store16",  WT_I64, ALLOC_node_i64_store16)
    STACK_STORE("i64.store32",  WT_I64, ALLOC_node_i64_store32)
    STACK_STORE("f32.store",    WT_F32, ALLOC_node_f32_store)
    STACK_STORE("f64.store",    WT_F64, ALLOC_node_f64_store)
#undef STACK_LOAD
#undef STACK_STORE
    if (tok_is_keyword("memory.size")) {
        next_token();
        op_push(S, ALLOC_node_memory_size(), WT_I32);
        return;
    }
    if (tok_is_keyword("memory.grow")) {
        next_token();
        TypedExpr d = op_pop(S, WT_I32, "memory.grow argument");
        op_push(S, ALLOC_node_memory_grow(d.node), WT_I32);
        return;
    }

    // ---- drop / select / nop / unreachable ----
    if (tok_is_keyword("drop")) {
        next_token();
        TypedExpr e = op_pop(S, WT_VOID, "drop operand");
        stmts_append(L, ALLOC_node_drop(e.node));
        return;
    }
    if (tok_is_keyword("select")) {
        next_token();
        // optional (result T)?
        if (cur_tok.kind == T_LPAREN) {
            const char *save_pos = src_pos;
            Token save_tok = cur_tok;
            next_token();
            if (tok_is_keyword("result")) {
                next_token();
                parse_wtype();
                expect_rparen();
            }
            else { src_pos = save_pos; cur_tok = save_tok; }
        }
        TypedExpr cond = op_pop(S, WT_I32, "select condition");
        TypedExpr v2 = op_pop(S, WT_VOID, "select v2");
        TypedExpr v1 = op_pop(S, WT_VOID, "select v1");
        if (v1.type != v2.type && v1.type != WT_POLY && v2.type != WT_POLY)
            parse_error("select: v1 and v2 type mismatch");
        op_push(S, ALLOC_node_select(v1.node, v2.node, cond.node), v1.type);
        return;
    }
    if (tok_is_keyword("nop"))         { next_token(); stmts_append(L, ALLOC_node_nop()); return; }
    if (tok_is_keyword("unreachable")) { next_token(); stmts_append(L, ALLOC_node_unreachable()); return; }

    // ---- block / loop / if (stack-style) ----
    if (tok_is_keyword("block") || tok_is_keyword("loop")) {
        int is_loop = tok_is_keyword("loop");
        next_token();
        char *label_name = NULL;
        if (cur_tok.kind == T_IDENT) {
            label_name = dup_token_str(&cur_tok);
            next_token();
        }
        // optional (result T) or inline (type $sig)/(param)/(result)
        wtype_t result_t = WT_VOID;
        while (cur_tok.kind == T_LPAREN) {
            const char *save_pos = src_pos;
            Token save_tok = cur_tok;
            next_token();
            if (tok_is_keyword("result")) {
                next_token();
                result_t = parse_result_type();
                expect_rparen();
            }
            else if (tok_is_keyword("type") || tok_is_keyword("param")) {
                int depth = 1;
                while (cur_tok.kind != T_EOF && depth > 0) {
                    if (cur_tok.kind == T_LPAREN) depth++;
                    else if (cur_tok.kind == T_RPAREN) {
                        depth--;
                        if (depth == 0) { next_token(); break; }
                    }
                    next_token();
                }
            }
            else { src_pos = save_pos; cur_tok = save_tok; break; }
        }
        if (labels->cnt >= 32) parse_error("too many nested labels");
        labels->names[labels->cnt] = label_name;
        labels->result_types[labels->cnt] = result_t;
        labels->is_loop[labels->cnt] = is_loop;
        labels->cnt++;
        TypedExpr body = parse_body_seq(env, labels, 0, NULL);
        // Expect `end` keyword to close stack-style block.
        if (!tok_is_keyword("end")) parse_error("expected `end` to close block/loop");
        next_token();
        // Optional matching label after end.
        if (cur_tok.kind == T_IDENT) next_token();
        labels->cnt--;
        if (label_name) free(label_name);
        if (result_t != WT_VOID && body.type != WT_VOID && body.type != result_t)
            parse_error("block/loop result type mismatch");
        NODE *node = is_loop ? ALLOC_node_loop(body.node) : ALLOC_node_block(body.node);
        if (result_t != WT_VOID) op_push(S, node, result_t);
        else stmts_append(L, node);
        return;
    }

    if (tok_is_keyword("if")) {
        next_token();
        char *label_name = NULL;
        if (cur_tok.kind == T_IDENT) {
            label_name = dup_token_str(&cur_tok);
            next_token();
        }
        wtype_t result_t = WT_VOID;
        while (cur_tok.kind == T_LPAREN) {
            const char *save_pos = src_pos;
            Token save_tok = cur_tok;
            next_token();
            if (tok_is_keyword("result")) {
                next_token();
                result_t = parse_result_type();
                expect_rparen();
            }
            else if (tok_is_keyword("type") || tok_is_keyword("param")) {
                int depth = 1;
                while (cur_tok.kind != T_EOF && depth > 0) {
                    if (cur_tok.kind == T_LPAREN) depth++;
                    else if (cur_tok.kind == T_RPAREN) {
                        depth--;
                        if (depth == 0) { next_token(); break; }
                    }
                    next_token();
                }
            }
            else { src_pos = save_pos; cur_tok = save_tok; break; }
        }
        TypedExpr cond = op_pop(S, WT_I32, "if condition");
        if (labels->cnt >= 32) parse_error("too many nested labels");
        labels->names[labels->cnt] = label_name;
        labels->result_types[labels->cnt] = result_t;
        labels->is_loop[labels->cnt] = 0;
        labels->cnt++;
        int saw_else = 0;
        TypedExpr then_b = parse_body_seq(env, labels, 1, &saw_else);
        TypedExpr else_b = {ALLOC_node_nop(), WT_VOID};
        if (saw_else) {
            next_token();   // consume `else`
            if (cur_tok.kind == T_IDENT) next_token();   // optional label
            else_b = parse_body_seq(env, labels, 0, NULL);
        }
        if (!tok_is_keyword("end")) parse_error("expected `end` to close if");
        next_token();
        if (cur_tok.kind == T_IDENT) next_token();
        labels->cnt--;
        if (label_name) free(label_name);
        if (result_t != WT_VOID) {
            if (then_b.type != WT_VOID && then_b.type != result_t) parse_error("if-then result type mismatch");
            if (else_b.type != WT_VOID && else_b.type != result_t) parse_error("if-else result type mismatch");
        }
        NODE *node = ALLOC_node_if(cond.node, then_b.node, else_b.node);
        if (result_t != WT_VOID) op_push(S, node, result_t);
        else stmts_append(L, node);
        return;
    }

    // ---- br / br_if / br_table / return ----
    if (tok_is_keyword("br")) {
        next_token();
        uint32_t depth;
        if (!label_env_lookup(labels, &cur_tok, &depth)) parse_error("unknown label in br");
        next_token();
        wtype_t want = labels->result_types[labels->cnt - 1 - depth];
        if (want != WT_VOID && S->cnt > 0) {
            TypedExpr v = op_pop(S, want, "br value");
            stmts_append(L, ALLOC_node_br_v(depth, v.node));
        }
        else {
            stmts_append(L, ALLOC_node_br(depth));
        }
        return;
    }
    if (tok_is_keyword("br_if")) {
        next_token();
        uint32_t depth;
        if (!label_env_lookup(labels, &cur_tok, &depth)) parse_error("unknown label in br_if");
        next_token();
        TypedExpr cond = op_pop(S, WT_I32, "br_if condition");
        wtype_t want = labels->result_types[labels->cnt - 1 - depth];
        if (want != WT_VOID && S->cnt > 0 && S->items[S->cnt - 1].type == want) {
            TypedExpr v = op_pop(S, want, "br_if value");
            stmts_append(L, ALLOC_node_br_if_v(depth, cond.node, v.node));
        }
        else {
            stmts_append(L, ALLOC_node_br_if(depth, cond.node));
        }
        return;
    }
    if (tok_is_keyword("br_table")) {
        next_token();
        uint32_t depths[64]; uint32_t cnt = 0;
        while (cur_tok.kind == T_IDENT || cur_tok.kind == T_INT) {
            if (cnt >= 64) parse_error("br_table: too many labels");
            uint32_t d;
            if (!label_env_lookup(labels, &cur_tok, &d)) parse_error("unknown label in br_table");
            depths[cnt++] = d;
            next_token();
        }
        if (cnt == 0) parse_error("br_table needs at least one label");
        uint32_t default_depth = depths[cnt - 1];
        uint32_t target_cnt = cnt - 1;
        if (WASTRO_BR_TABLE_CNT + target_cnt > WASTRO_BR_TABLE_CAP) {
            WASTRO_BR_TABLE_CAP = WASTRO_BR_TABLE_CAP ? WASTRO_BR_TABLE_CAP * 2 : 64;
            while (WASTRO_BR_TABLE_CAP < WASTRO_BR_TABLE_CNT + target_cnt) WASTRO_BR_TABLE_CAP *= 2;
            WASTRO_BR_TABLE = realloc(WASTRO_BR_TABLE, sizeof(uint32_t) * WASTRO_BR_TABLE_CAP);
        }
        uint32_t target_index = WASTRO_BR_TABLE_CNT;
        for (uint32_t i = 0; i < target_cnt; i++) WASTRO_BR_TABLE[target_index + i] = depths[i];
        WASTRO_BR_TABLE_CNT += target_cnt;
        TypedExpr idx = op_pop(S, WT_I32, "br_table index");
        wtype_t want = labels->result_types[labels->cnt - 1 - default_depth];
        if (want != WT_VOID && S->cnt > 0 && S->items[S->cnt - 1].type == want) {
            TypedExpr v = op_pop(S, want, "br_table value");
            stmts_append(L, ALLOC_node_br_table_v(target_index, target_cnt, default_depth, idx.node, v.node));
        }
        else {
            stmts_append(L, ALLOC_node_br_table(target_index, target_cnt, default_depth, idx.node));
        }
        return;
    }
    if (tok_is_keyword("return")) {
        next_token();
        if (S->cnt > 0) {
            TypedExpr v = S->items[--S->cnt];
            stmts_append(L, ALLOC_node_return_v(v.node));
        }
        else {
            stmts_append(L, ALLOC_node_return());
        }
        return;
    }

    // ---- call / call_indirect ----
    if (tok_is_keyword("call")) {
        next_token();
        int func_idx = resolve_func(&cur_tok);
        struct wastro_function *callee = &WASTRO_FUNCS[func_idx];
        next_token();
        uint32_t argc = callee->param_cnt;
        if (argc > 8) parse_error("call arity > 8 not supported");
        NODE *args[8];
        // Pop args in reverse (last pushed = last positional arg).
        for (int i = (int)argc - 1; i >= 0; i--) {
            TypedExpr a = op_pop(S, callee->param_types[i], "call argument");
            args[i] = a.node;
        }
        NODE *call_node;
        if (callee->is_import) {
            switch (argc) {
            case 0: call_node = ALLOC_node_host_call_0((uint32_t)func_idx); break;
            case 1: call_node = ALLOC_node_host_call_1((uint32_t)func_idx, args[0]); break;
            case 2: call_node = ALLOC_node_host_call_2((uint32_t)func_idx, args[0], args[1]); break;
            case 3: call_node = ALLOC_node_host_call_3((uint32_t)func_idx, args[0], args[1], args[2]); break;
            default: parse_error("host call arity > 3 not supported"); return;
            }
        }
        else {
            uint32_t local_cnt = callee->local_cnt;
            NODE *body = WASTRO_FUNCS[func_idx].body;  // may be NULL if forward ref
            switch (argc) {
            case 0: call_node = ALLOC_node_call_0((uint32_t)func_idx, local_cnt, body); break;
            case 1: call_node = ALLOC_node_call_1((uint32_t)func_idx, local_cnt, args[0], body); break;
            case 2: call_node = ALLOC_node_call_2((uint32_t)func_idx, local_cnt, args[0], args[1], body); break;
            case 3: call_node = ALLOC_node_call_3((uint32_t)func_idx, local_cnt, args[0], args[1], args[2], body); break;
            case 4: call_node = ALLOC_node_call_4((uint32_t)func_idx, local_cnt, args[0], args[1], args[2], args[3], body); break;
            default: parse_error("call arity 5..8 not supported yet"); return;
            }
            register_call_body_fixup(call_node, (uint32_t)func_idx, (uint8_t)argc);
        }
        if (callee->result_type == WT_VOID) stmts_append(L, call_node);
        else op_push(S, call_node, callee->result_type);
        return;
    }
    if (tok_is_keyword("call_indirect")) {
        next_token();
        // optional table reference
        if (cur_tok.kind == T_IDENT || cur_tok.kind == T_INT) next_token();
        expect_lparen();
        expect_keyword("type");
        int type_idx = -1;
        if (cur_tok.kind == T_INT) type_idx = (int)cur_tok.int_value;
        else if (cur_tok.kind == T_IDENT) {
            for (uint32_t i = 0; i < WASTRO_TYPE_CNT; i++) {
                const char *tn = WASTRO_TYPE_NAMES[i];
                if (tn && strlen(tn) == cur_tok.len && memcmp(tn, cur_tok.start, cur_tok.len) == 0) { type_idx = (int)i; break; }
            }
        }
        if (type_idx < 0 || (uint32_t)type_idx >= WASTRO_TYPE_CNT) parse_error("call_indirect: unknown type");
        next_token();
        expect_rparen();
        // skip optional inline (param ...)/(result ...)
        while (cur_tok.kind == T_LPAREN) {
            const char *save_pos = src_pos;
            Token save_tok = cur_tok;
            next_token();
            if (tok_is_keyword("param") || tok_is_keyword("result")) {
                int depth = 1;
                while (cur_tok.kind != T_EOF && depth > 0) {
                    if (cur_tok.kind == T_LPAREN) depth++;
                    else if (cur_tok.kind == T_RPAREN) {
                        depth--;
                        if (depth == 0) { next_token(); break; }
                    }
                    next_token();
                }
            }
            else { src_pos = save_pos; cur_tok = save_tok; break; }
        }
        struct wastro_type_sig *sig = &WASTRO_TYPES[type_idx];
        if (sig->param_cnt > 4) parse_error("call_indirect arity > 4 not supported yet");
        TypedExpr idx = op_pop(S, WT_I32, "call_indirect index");
        NODE *args[4];
        for (int i = (int)sig->param_cnt - 1; i >= 0; i--) {
            TypedExpr a = op_pop(S, sig->param_types[i], "call_indirect arg");
            args[i] = a.node;
        }
        NODE *call_node;
        switch (sig->param_cnt) {
        case 0: call_node = ALLOC_node_call_indirect_0((uint32_t)type_idx, idx.node); break;
        case 1: call_node = ALLOC_node_call_indirect_1((uint32_t)type_idx, idx.node, args[0]); break;
        case 2: call_node = ALLOC_node_call_indirect_2((uint32_t)type_idx, idx.node, args[0], args[1]); break;
        case 3: call_node = ALLOC_node_call_indirect_3((uint32_t)type_idx, idx.node, args[0], args[1], args[2]); break;
        case 4: call_node = ALLOC_node_call_indirect_4((uint32_t)type_idx, idx.node, args[0], args[1], args[2], args[3]); break;
        default: parse_error("call_indirect arity > 4 not supported yet"); return;
        }
        if (sig->result_type == WT_VOID) stmts_append(L, call_node);
        else op_push(S, call_node, sig->result_type);
        return;
    }

    fprintf(stderr, "wastro: unknown bare instruction '%.*s'\n",
            (int)cur_tok.len, cur_tok.start);
    exit(1);
}

#undef STACK_BIN
#undef STACK_UN

// Parse a sequence of body instrs.  Stops on either ')' (when called
// inside a `parse_expr`-style folded form) or on `end` / `else`
// keywords (when called for a stack-style block/loop/if body).  For
// the function body, both apply (implicitly only `)` since the body
// is wrapped in (func ...)).
//
// `out_else` is set to 1 if we stopped at `else`; 0 if at `end` or `)`.
// Returns the typed value of the body — either the top of the
// operand stack (if any) or void.
static TypedExpr
parse_body_seq(LocalEnv *env, LabelEnv *labels, int allow_else_terminator, int *out_else)
{
    OpStack S = {0};
    StmtList L = {0};
    if (out_else) *out_else = 0;
    while (1) {
        if (cur_tok.kind == T_RPAREN || cur_tok.kind == T_EOF) break;
        if (cur_tok.kind == T_KEYWORD) {
            if (tok_is_keyword("end")) break;
            if (allow_else_terminator && tok_is_keyword("else")) {
                if (out_else) *out_else = 1;
                break;
            }
        }
        if (cur_tok.kind == T_LPAREN) {
            // Folded sub-expression — existing parse_expr handles it
            // and returns one TypedExpr (with type WT_VOID for
            // statement-only expressions).
            TypedExpr e = parse_expr(env, labels);
            if (e.type == WT_VOID) stmts_append(&L, e.node);
            else op_push(&S, e.node, e.type);
        }
        else if (cur_tok.kind == T_KEYWORD) {
            parse_bare_instr(env, labels, &S, &L);
        }
        else {
            parse_error("expected instruction");
        }
    }
    NODE *final_val = NULL;
    wtype_t final_t = WT_VOID;
    if (S.cnt >= 1) {
        // Use the most recently pushed value as the body's result.
        // (Wasm validation guarantees there's exactly 1 if the block
        // declared a result.)
        final_val = S.items[S.cnt - 1].node;
        final_t = S.items[S.cnt - 1].type;
        // Earlier stack values that were never consumed are dropped —
        // their NODEs would not contribute to the body's value, but to
        // preserve their side effects we sequence them into L first.
        for (uint32_t i = 0; i + 1 < S.cnt; i++) {
            stmts_append(&L, S.items[i].node);
        }
    }
    NODE *body = build_body_node(&L, final_val);
    return (TypedExpr){body, final_t};
}

// =====================================================================
// (func ...) parser — two-pass: pass 1 only registers names, pass 2 parses bodies.
// =====================================================================

typedef struct {
    const char *func_start;  // pointer to '(' that begins the func form
    LocalEnv env;
    int idx;
} FuncStub;

static void
parse_func_header(int idx)
{
    // ( func ($name)? (export "n")* (import ...)? (param ...)* (result ...)? <body...>
    // Pass-1 reads name + exports + signature so that forward
    // references between functions resolve correctly when bodies are
    // parsed in pass 2 (e.g. recursive `(call $f ...)` to a func
    // declared later in the source).
    expect_keyword("func");
    if (cur_tok.kind == T_IDENT) {
        WASTRO_FUNCS[idx].name = dup_token_str(&cur_tok);
        next_token();
    }
    // Inline (export "n")* and optional (import "m" "f").
    while (cur_tok.kind == T_LPAREN) {
        const char *save_pos = src_pos;
        Token save_tok = cur_tok;
        next_token();
        if (tok_is_keyword("export")) {
            next_token();
            if (cur_tok.kind != T_STRING) parse_error("expected export name string");
            WASTRO_FUNCS[idx].exported = 1;
            WASTRO_FUNCS[idx].export_name = dup_token_str(&cur_tok);
            next_token();
            expect_rparen();
        }
        else if (tok_is_keyword("import")) {
            // (import "mod" "fld") — skip; the body is empty for inline imports
            next_token();
            if (cur_tok.kind == T_STRING) next_token();
            if (cur_tok.kind == T_STRING) next_token();
            expect_rparen();
            WASTRO_FUNCS[idx].is_import = 1;
            WASTRO_FUNCS[idx].host_fn = host_unbound_trap;
        }
        else {
            src_pos = save_pos;
            cur_tok = save_tok;
            break;
        }
    }
    // Now read (param ...)* and (result ...)? to capture signature.
    WASTRO_FUNCS[idx].result_type = WT_VOID;
    uint32_t pcnt = 0;
    while (cur_tok.kind == T_LPAREN) {
        const char *save_pos = src_pos;
        Token save_tok = cur_tok;
        next_token();
        if (tok_is_keyword("param")) {
            next_token();
            if (cur_tok.kind == T_IDENT) next_token();   // ignore $name
            while (cur_tok.kind == T_KEYWORD &&
                   (tok_is_keyword("i32") || tok_is_keyword("i64") ||
                    tok_is_keyword("f32") || tok_is_keyword("f64"))) {
                wtype_t t = parse_wtype();
                if (pcnt < WASTRO_MAX_PARAMS) {
                    WASTRO_FUNCS[idx].param_types[pcnt] = t;
                }
                pcnt++;
            }
            expect_rparen();
        }
        else if (tok_is_keyword("result")) {
            next_token();
            WASTRO_FUNCS[idx].result_type = parse_result_type();
            expect_rparen();
        }
        else if (tok_is_keyword("type")) {
            // (type $sig) — fetch from the type table
            next_token();
            int ti = -1;
            if (cur_tok.kind == T_INT) ti = (int)cur_tok.int_value;
            else if (cur_tok.kind == T_IDENT) {
                for (uint32_t i = 0; i < WASTRO_TYPE_CNT; i++) {
                    const char *tn = WASTRO_TYPE_NAMES[i];
                    if (tn && strlen(tn) == cur_tok.len && memcmp(tn, cur_tok.start, cur_tok.len) == 0) {
                        ti = (int)i; break;
                    }
                }
            }
            next_token();
            expect_rparen();
            if (ti >= 0 && (uint32_t)ti < WASTRO_TYPE_CNT) {
                struct wastro_type_sig *sig = &WASTRO_TYPES[ti];
                pcnt = sig->param_cnt;
                for (uint32_t k = 0; k < sig->param_cnt; k++)
                    WASTRO_FUNCS[idx].param_types[k] = sig->param_types[k];
                WASTRO_FUNCS[idx].result_type = sig->result_type;
            }
        }
        else {
            src_pos = save_pos;
            cur_tok = save_tok;
            break;
        }
    }
    WASTRO_FUNCS[idx].param_cnt = pcnt;
    WASTRO_FUNCS[idx].local_cnt = pcnt;   // body adds (local) entries
}

// Expects cur_tok to be the '(' of the func.  Consumes the entire form.
static void
parse_func_body(int idx, LocalEnv *env)
{
    LabelEnv labels = {0};
    // Push the implicit function-body label.  `br N` from inside a
    // function targets this label at depth N where the outermost
    // (function exit) is the deepest depth.
    labels.names[labels.cnt] = NULL;
    labels.result_types[labels.cnt] = WASTRO_FUNCS[idx].result_type;
    labels.is_loop[labels.cnt] = 0;
    labels.cnt++;
    int save_idx = CUR_FUNC_IDX;
    CUR_FUNC_IDX = idx;
    TypedExpr body = parse_body_seq(env, &labels, 0, NULL);
    CUR_FUNC_IDX = save_idx;
    (void)body.type;
    WASTRO_FUNCS[idx].body = body.node;
}

static const char *MODULE_TEXT_START;

// Parse one (func ...) form starting at cur_tok=='('.  This is the
// SECOND pass; we already know its index from the first pass.
static void
parse_func_pass2(int idx)
{
    expect_lparen();
    expect_keyword("func");
    LocalEnv env;
    env.cnt = 0;

    // skip optional $name
    if (cur_tok.kind == T_IDENT) next_token();
    // skip optional (export "...")
    if (cur_tok.kind == T_LPAREN) {
        const char *save_pos = src_pos;
        Token save_tok = cur_tok;
        next_token();
        if (tok_is_keyword("export")) {
            next_token();
            if (cur_tok.kind == T_STRING) next_token();
            expect_rparen();
        }
        else {
            src_pos = save_pos;
            cur_tok = save_tok;
        }
    }

    // (param $x T)* | (param T T ...) | (result T)*
    WASTRO_FUNCS[idx].result_type = WT_VOID;
    while (cur_tok.kind == T_LPAREN) {
        const char *save_pos = src_pos;
        Token save_tok = cur_tok;
        next_token();
        if (tok_is_keyword("param")) {
            next_token();
            char *pname = NULL;
            if (cur_tok.kind == T_IDENT) {
                pname = dup_token_str(&cur_tok);
                next_token();
            }
            // `(param T T ...)` (anonymous, multiple) OR `(param $name T)`.
            do {
                wtype_t pt = parse_wtype();
                if (env.cnt >= 64) parse_error("too many params");
                if (env.cnt >= WASTRO_MAX_PARAMS) parse_error("too many params (>8)");
                env.names[env.cnt] = pname;
                env.types[env.cnt] = pt;
                WASTRO_FUNCS[idx].param_types[env.cnt] = pt;
                env.cnt++;
                pname = NULL;
            } while (cur_tok.kind == T_KEYWORD);
            expect_rparen();
        }
        else if (tok_is_keyword("result")) {
            next_token();
            wtype_t rt = parse_result_type();
            WASTRO_FUNCS[idx].result_type = rt;
            expect_rparen();
        }
        else {
            // not a param/result — body or local section has begun
            src_pos = save_pos;
            cur_tok = save_tok;
            break;
        }
    }
    WASTRO_FUNCS[idx].param_cnt = env.cnt;

    // (local $x T)* | (local T T ...)
    while (cur_tok.kind == T_LPAREN) {
        const char *save_pos = src_pos;
        Token save_tok = cur_tok;
        next_token();
        if (tok_is_keyword("local")) {
            next_token();
            char *lname = NULL;
            if (cur_tok.kind == T_IDENT) {
                lname = dup_token_str(&cur_tok);
                next_token();
            }
            do {
                wtype_t lt = parse_wtype();
                if (env.cnt >= 64) parse_error("too many locals");
                env.names[env.cnt] = lname;
                env.types[env.cnt] = lt;
                env.cnt++;
                lname = NULL;
            } while (cur_tok.kind == T_KEYWORD);
            expect_rparen();
        }
        else {
            src_pos = save_pos;
            cur_tok = save_tok;
            break;
        }
    }
    WASTRO_FUNCS[idx].local_cnt = env.cnt;
    for (uint32_t i = 0; i < env.cnt; i++) {
        WASTRO_FUNCS[idx].local_types[i] = env.types[i];
    }

    parse_func_body(idx, &env);
    expect_rparen();
}

// Pass 1: scan top-level (func ...) forms, register their names, and
// remember each func's source offset to revisit in pass 2.
static void
scan_module(const char *text, size_t len, const char **func_offsets, int *func_count_out)
{
    src_pos = text;
    src_end = text + len;
    next_token();
    expect_lparen();
    expect_keyword("module");
    // Optional $modname.
    if (cur_tok.kind == T_IDENT) next_token();

    // (module binary "..." "..." ...)  or (module quote "..." "..." ...).
    if (tok_is_keyword("binary")) {
        next_token();
        uint8_t *bin = NULL; uint32_t total = 0;
        while (cur_tok.kind == T_STRING) {
            uint32_t seg_len;
            uint8_t *seg = decode_wasm_str(&cur_tok, &seg_len);
            bin = realloc(bin, total + seg_len);
            memcpy(bin + total, seg, seg_len);
            total += seg_len;
            free(seg);
            next_token();
        }
        expect_rparen();
        load_module_binary(bin, total);
        free(bin);
        *func_count_out = 0;     // bodies already populated by binary loader
        return;
    }
    if (tok_is_keyword("quote")) {
        next_token();
        char *qbuf = NULL; uint32_t total = 0;
        while (cur_tok.kind == T_STRING) {
            uint32_t seg_len;
            uint8_t *seg = decode_wasm_str(&cur_tok, &seg_len);
            qbuf = realloc(qbuf, total + seg_len + 16);
            memcpy(qbuf + total, seg, seg_len);
            total += seg_len;
            free(seg);
            next_token();
        }
        expect_rparen();
        // Wrap as `(module ...)` and recurse.
        size_t wrap = total + 16;
        char *wrapped = malloc(wrap);
        int wlen = snprintf(wrapped, wrap, "(module %.*s)", (int)total, qbuf ? qbuf : "");
        wastro_load_module_buf(wrapped, (size_t)wlen);
        free(wrapped); if (qbuf) free(qbuf);
        *func_count_out = 0;     // recursive call populated bodies
        return;
    }

    int n = 0;
    while (cur_tok.kind == T_LPAREN) {
        const char *form_start_pos = cur_tok.start;
        next_token();
        if (tok_is_keyword("func")) {
            if (n >= WASTRO_MAX_FUNCS) parse_error("too many functions");
            int idx = n;
            WASTRO_FUNC_CNT++;
            func_offsets[n] = form_start_pos;
            parse_func_header(idx);
            // Skip the rest of this form (balanced parens).
            int depth = 1;
            while (cur_tok.kind != T_EOF && depth > 0) {
                if (cur_tok.kind == T_LPAREN) depth++;
                else if (cur_tok.kind == T_RPAREN) depth--;
                if (depth > 0) next_token();
            }
            expect_rparen();
            n++;
        }
        else if (tok_is_keyword("import")) {
            // (import "mod" "field" (func ... | memory ... | global ... | table ...))
            next_token();
            if (cur_tok.kind != T_STRING) parse_error("(import) expects module string");
            char mod[64]; size_t ml = cur_tok.len < 63 ? cur_tok.len : 63;
            memcpy(mod, cur_tok.start, ml); mod[ml] = 0;
            next_token();
            if (cur_tok.kind != T_STRING) parse_error("(import) expects field string");
            char fld[64]; size_t fl = cur_tok.len < 63 ? cur_tok.len : 63;
            memcpy(fld, cur_tok.start, fl); fld[fl] = 0;
            next_token();
            expect_lparen();
            if (tok_is_keyword("func")) {
                next_token();
                char *iname = NULL;
                if (cur_tok.kind == T_IDENT) {
                    iname = dup_token_str(&cur_tok);
                    next_token();
                }
                // Determine signature: from `(type $sig)` ref if given,
                // else from inline (param ...) / (result ...) forms,
                // else from the host registry.
                struct wastro_type_sig sig = {0};
                int sig_set = 0;
                while (cur_tok.kind == T_LPAREN) {
                    const char *save_pos = src_pos;
                    Token save_tok = cur_tok;
                    next_token();
                    if (tok_is_keyword("type")) {
                        next_token();
                        int ti = -1;
                        if (cur_tok.kind == T_INT) ti = (int)cur_tok.int_value;
                        else if (cur_tok.kind == T_IDENT) {
                            for (uint32_t i = 0; i < WASTRO_TYPE_CNT; i++) {
                                const char *tn = WASTRO_TYPE_NAMES[i];
                                if (tn && strlen(tn) == cur_tok.len && memcmp(tn, cur_tok.start, cur_tok.len) == 0) {
                                    ti = (int)i; break;
                                }
                            }
                        }
                        if (ti < 0 || (uint32_t)ti >= WASTRO_TYPE_CNT) parse_error("(import func type) unknown");
                        next_token();
                        expect_rparen();
                        sig = WASTRO_TYPES[ti];
                        sig_set = 1;
                    }
                    else if (tok_is_keyword("param")) {
                        next_token();
                        if (cur_tok.kind == T_IDENT) next_token();
                        do {
                            if (sig.param_cnt >= WASTRO_MAX_PARAMS) parse_error("import too many params");
                            sig.param_types[sig.param_cnt++] = parse_wtype();
                        } while (cur_tok.kind == T_KEYWORD);
                        expect_rparen();
                        sig_set = 1;
                    }
                    else if (tok_is_keyword("result")) {
                        next_token();
                        sig.result_type = parse_result_type();
                        expect_rparen();
                        sig_set = 1;
                    }
                    else {
                        src_pos = save_pos; cur_tok = save_tok; break;
                    }
                }
                expect_rparen();   // close (func ...)
                expect_rparen();   // close (import ...)
                if (n >= WASTRO_MAX_FUNCS) parse_error("too many functions");
                int fi = n;
                WASTRO_FUNCS[fi].name = iname;
                WASTRO_FUNCS[fi].is_import = 1;
                WASTRO_FUNCS[fi].local_cnt = 0;
                const struct host_entry *he = find_host(mod, fld);
                if (he) {
                    // Host-registry signature wins (authoritative).
                    WASTRO_FUNCS[fi].host_fn = he->fn;
                    WASTRO_FUNCS[fi].param_cnt = he->param_cnt;
                    for (uint32_t i = 0; i < he->param_cnt; i++)
                        WASTRO_FUNCS[fi].param_types[i] = he->param_types[i];
                    WASTRO_FUNCS[fi].result_type = he->result_type;
                }
                else if (sig_set) {
                    // Unknown host but explicit signature — register a
                    // stub host_fn that traps when invoked.  This
                    // tolerates spec tests that import unbound names.
                    extern VALUE host_unbound_trap(struct CTX_struct *c, VALUE *args, uint32_t argc);
                    WASTRO_FUNCS[fi].host_fn = host_unbound_trap;
                    WASTRO_FUNCS[fi].param_cnt = sig.param_cnt;
                    for (uint32_t i = 0; i < sig.param_cnt; i++)
                        WASTRO_FUNCS[fi].param_types[i] = sig.param_types[i];
                    WASTRO_FUNCS[fi].result_type = sig.result_type;
                }
                else {
                    // No host binding and no inline signature — accept
                    // as a no-op no-arg/no-result import that traps on
                    // call.  Spec tests reference unbound imports
                    // (especially `spectest.*`) without using them.
                    extern VALUE host_unbound_trap(struct CTX_struct *c, VALUE *args, uint32_t argc);
                    WASTRO_FUNCS[fi].host_fn = host_unbound_trap;
                    WASTRO_FUNCS[fi].param_cnt = 0;
                    WASTRO_FUNCS[fi].result_type = WT_VOID;
                }
                WASTRO_FUNC_CNT++;
                n++;
                func_offsets[fi] = NULL;
            }
            else if (tok_is_keyword("memory")) {
                // (import "m" "f" (memory N M?))
                next_token();
                if (cur_tok.kind == T_IDENT) next_token();
                if (cur_tok.kind != T_INT) parse_error("(import memory) expects min");
                MOD_MEM_INITIAL_PAGES = (uint32_t)cur_tok.int_value;
                next_token();
                if (cur_tok.kind == T_INT) {
                    MOD_MEM_MAX_PAGES = (uint32_t)cur_tok.int_value;
                    next_token();
                }
                // Spec testsuite's `spectest.memory` is defined to be
                // 1 initial page, 2 max pages.  Override so spec tests
                // that import it get the expected initial state.
                if (strcmp(mod, "spectest") == 0 && strcmp(fld, "memory") == 0) {
                    MOD_MEM_INITIAL_PAGES = 1;
                    MOD_MEM_MAX_PAGES = 2;
                }
                MOD_HAS_MEMORY = 1;
                expect_rparen();
                expect_rparen();
            }
            else if (tok_is_keyword("global")) {
                // (import "m" "f" (global $name? (mut)? T))
                next_token();
                char *gname = NULL;
                if (cur_tok.kind == T_IDENT) {
                    gname = dup_token_str(&cur_tok);
                    next_token();
                }
                wtype_t gt;
                int is_mut = 0;
                if (cur_tok.kind == T_LPAREN) {
                    next_token();
                    expect_keyword("mut");
                    gt = parse_wtype();
                    expect_rparen();
                    is_mut = 1;
                }
                else gt = parse_wtype();
                expect_rparen();
                expect_rparen();
                if (WASTRO_GLOBAL_CNT >= WASTRO_MAX_GLOBALS) parse_error("too many globals");
                if (!WASTRO_GLOBALS) WASTRO_GLOBALS = calloc(WASTRO_MAX_GLOBALS, sizeof(VALUE));
                // Pre-populate well-known spectest globals (used by
                // the wasm spec-test bench) with the values it expects.
                VALUE init_v = 0;
                if (strcmp(mod, "spectest") == 0) {
                    if (strcmp(fld, "global_i32") == 0) init_v = FROM_I32(666);
                    else if (strcmp(fld, "global_i64") == 0) init_v = FROM_I64(666);
                    else if (strcmp(fld, "global_f32") == 0) init_v = FROM_F32(666.6f);
                    else if (strcmp(fld, "global_f64") == 0) init_v = FROM_F64(666.6);
                }
                WASTRO_GLOBALS[WASTRO_GLOBAL_CNT] = init_v;
                WASTRO_GLOBAL_TYPES[WASTRO_GLOBAL_CNT] = gt;
                WASTRO_GLOBAL_MUT[WASTRO_GLOBAL_CNT] = is_mut;
                WASTRO_GLOBAL_NAMES[WASTRO_GLOBAL_CNT] = gname;
                WASTRO_GLOBAL_CNT++;
            }
            else if (tok_is_keyword("table")) {
                // (import "m" "f" (table $name? N M? funcref))
                next_token();
                if (cur_tok.kind == T_IDENT) next_token();
                if (cur_tok.kind != T_INT) parse_error("(import table) expects min");
                uint32_t init = (uint32_t)cur_tok.int_value;
                next_token();
                uint32_t maxn = 0xFFFFFFFFu;
                if (cur_tok.kind == T_INT) { maxn = (uint32_t)cur_tok.int_value; next_token(); }
                if (!tok_is_keyword("funcref") && !tok_is_keyword("anyfunc"))
                    parse_error("(import table) only supports funcref");
                next_token();
                expect_rparen();
                expect_rparen();
                if (MOD_HAS_TABLE) parse_error("multiple table declarations");
                MOD_HAS_TABLE = 1;
                WASTRO_TABLE_SIZE = init;
                WASTRO_TABLE_MAX = maxn;
                WASTRO_TABLE = malloc(sizeof(int32_t) * (init ? init : 1));
                for (uint32_t k = 0; k < init; k++) WASTRO_TABLE[k] = -1;
            }
            else parse_error("(import) only supports func / memory / global / table");
        }
        else if (tok_is_keyword("memory")) {
            // (memory $name?
            //   (export "n")*
            //   (import "m" "f")?
            //   N M? | (data "...")*
            // )
            next_token();
            if (cur_tok.kind == T_IDENT) next_token();   // discard $name
            char *exname = NULL; (void)exname;
            char imp_mod[64] = {0}, imp_fld[64] = {0};
            int has_import = 0;
            while (try_inline_export(&exname)) {
                /* memory exports are accepted but not propagated (single-memory) */
                if (exname) { free(exname); exname = NULL; }
            }
            if (try_inline_import(imp_mod, imp_fld)) {
                has_import = 1;
                while (try_inline_export(&exname)) { if (exname) { free(exname); exname = NULL; } }
            }
            if (cur_tok.kind == T_LPAREN) {
                // Inline (data "...") form — auto-size memory from data bytes.
                next_token();
                if (!tok_is_keyword("data")) parse_error("(memory ...) inline form requires (data ...)");
                next_token();
                uint8_t *bytes = NULL; uint32_t total = 0;
                while (cur_tok.kind == T_STRING) {
                    uint32_t seg_len;
                    uint8_t *seg = decode_wasm_str(&cur_tok, &seg_len);
                    bytes = realloc(bytes, total + seg_len);
                    memcpy(bytes + total, seg, seg_len);
                    total += seg_len;
                    free(seg);
                    next_token();
                }
                expect_rparen();   // close (data ...)
                // Page count = ceil(total / PAGE_SIZE).  Empty data
                // segments yield 0 pages per spec.
                uint32_t pages = (total + WASTRO_PAGE_SIZE - 1) / WASTRO_PAGE_SIZE;
                MOD_MEM_INITIAL_PAGES = pages;
                MOD_MEM_MAX_PAGES = pages;
                if (total > 0) {
                    if (MOD_DATA_SEG_CNT >= WASTRO_MAX_DATA_SEGS) parse_error("too many data segments");
                    MOD_DATA_SEGS[MOD_DATA_SEG_CNT].offset = 0;
                    MOD_DATA_SEGS[MOD_DATA_SEG_CNT].length = total;
                    MOD_DATA_SEGS[MOD_DATA_SEG_CNT].bytes = bytes;
                    MOD_DATA_SEG_CNT++;
                }
                else if (bytes) free(bytes);
            }
            else {
                if (cur_tok.kind != T_INT) parse_error("(memory ...) expects integer min");
                MOD_MEM_INITIAL_PAGES = (uint32_t)cur_tok.int_value;
                next_token();
                if (cur_tok.kind == T_INT) {
                    MOD_MEM_MAX_PAGES = (uint32_t)cur_tok.int_value;
                    next_token();
                }
                // Optional `shared` keyword (threads proposal — accept and ignore).
                if (cur_tok.kind == T_KEYWORD && cur_tok.len == 6 && memcmp(cur_tok.start, "shared", 6) == 0) next_token();
            }
            MOD_HAS_MEMORY = 1;
            (void)has_import; (void)imp_mod; (void)imp_fld;
            expect_rparen();
        }
        else if (tok_is_keyword("global")) {
            // (global $name? (export "n")* (import "m" "f")? (mut T)? T <init-expr>)
            next_token();
            char *gname = NULL;
            if (cur_tok.kind == T_IDENT) {
                gname = dup_token_str(&cur_tok);
                next_token();
            }
            char *exname = NULL;
            char imp_mod[64] = {0}, imp_fld[64] = {0};
            int has_import = 0;
            while (try_inline_export(&exname)) { if (exname) { free(exname); exname = NULL; } }
            if (try_inline_import(imp_mod, imp_fld)) {
                has_import = 1;
                while (try_inline_export(&exname)) { if (exname) { free(exname); exname = NULL; } }
            }
            wtype_t gt;
            int is_mut = 0;
            if (cur_tok.kind == T_LPAREN) {
                next_token();
                expect_keyword("mut");
                gt = parse_wtype();
                expect_rparen();
                is_mut = 1;
            }
            else {
                gt = parse_wtype();
            }
            if (has_import) {
                // Imported global — no init expr.
                if (WASTRO_GLOBAL_CNT >= WASTRO_MAX_GLOBALS) parse_error("too many globals");
                if (!WASTRO_GLOBALS) WASTRO_GLOBALS = calloc(WASTRO_MAX_GLOBALS, sizeof(VALUE));
                VALUE init_v = 0;
                if (strcmp(imp_mod, "spectest") == 0) {
                    if (strcmp(imp_fld, "global_i32") == 0) init_v = FROM_I32(666);
                    else if (strcmp(imp_fld, "global_i64") == 0) init_v = FROM_I64(666);
                    else if (strcmp(imp_fld, "global_f32") == 0) init_v = FROM_F32(666.6f);
                    else if (strcmp(imp_fld, "global_f64") == 0) init_v = FROM_F64(666.6);
                }
                WASTRO_GLOBALS[WASTRO_GLOBAL_CNT] = init_v;
                WASTRO_GLOBAL_TYPES[WASTRO_GLOBAL_CNT] = gt;
                WASTRO_GLOBAL_MUT[WASTRO_GLOBAL_CNT] = is_mut;
                WASTRO_GLOBAL_NAMES[WASTRO_GLOBAL_CNT] = gname;
                WASTRO_GLOBAL_CNT++;
                expect_rparen();
                continue;
            }
            // Init expression: only `*.const` constants supported in v0.6.
            // Parse the init expression and evaluate immediately; the
            // CTX is null because const folds without one.
            VALUE init_val = 0;
            expect_lparen();
            if (tok_is_keyword("i32.const")) {
                next_token();
                if (cur_tok.kind != T_INT) parse_error("expected i32 literal");
                init_val = FROM_I32((int32_t)cur_tok.int_value);
                next_token();
            }
            else if (tok_is_keyword("i64.const")) {
                next_token();
                if (cur_tok.kind != T_INT) parse_error("expected i64 literal");
                init_val = FROM_I64((int64_t)cur_tok.int_value);
                next_token();
            }
            else if (tok_is_keyword("f32.const")) {
                next_token();
                if (cur_tok.kind != T_INT) parse_error("expected f32 literal");
                double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
                init_val = FROM_F32((float)dv);
                next_token();
            }
            else if (tok_is_keyword("f64.const")) {
                next_token();
                if (cur_tok.kind != T_INT) parse_error("expected f64 literal");
                double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
                init_val = FROM_F64(dv);
                next_token();
            }
            else parse_error("global init must be *.const");
            expect_rparen();   // close init expr
            expect_rparen();   // close (global ...)

            if (WASTRO_GLOBAL_CNT >= WASTRO_MAX_GLOBALS) parse_error("too many globals");
            if (!WASTRO_GLOBALS) WASTRO_GLOBALS = calloc(WASTRO_MAX_GLOBALS, sizeof(VALUE));
            WASTRO_GLOBALS[WASTRO_GLOBAL_CNT] = init_val;
            WASTRO_GLOBAL_TYPES[WASTRO_GLOBAL_CNT] = gt;
            WASTRO_GLOBAL_MUT[WASTRO_GLOBAL_CNT] = is_mut;
            WASTRO_GLOBAL_NAMES[WASTRO_GLOBAL_CNT] = gname;
            WASTRO_GLOBAL_CNT++;
        }
        else if (tok_is_keyword("data")) {
            // (data $name? (memory $m)? (offset expr | const-expr) "bytes")
            // (data $name? N "bytes")                                — sugar
            // Passive form `(data "...")` (no offset) is parsed but
            // ignored (post-1.0 bulk-memory).
            next_token();
            if (cur_tok.kind == T_IDENT) next_token();   // discard $name
            // Optional (memory $m) reference — single memory in 1.0.
            if (cur_tok.kind == T_LPAREN) {
                const char *save_pos = src_pos;
                Token save_tok = cur_tok;
                next_token();
                if (tok_is_keyword("memory")) {
                    next_token();
                    if (cur_tok.kind == T_IDENT || cur_tok.kind == T_INT) next_token();
                    expect_rparen();
                }
                else {
                    src_pos = save_pos; cur_tok = save_tok;
                }
            }
            uint32_t offset = 0;
            int is_passive = 1;   // becomes 0 if an offset is given
            if (cur_tok.kind == T_LPAREN) {
                is_passive = 0;
                next_token();
                int wrapped_offset = 0;
                if (tok_is_keyword("offset")) {
                    wrapped_offset = 1;
                    next_token();
                    expect_lparen();
                }
                if (tok_is_keyword("i32.const")) {
                    next_token();
                    if (cur_tok.kind != T_INT) parse_error("expected offset literal");
                    offset = (uint32_t)cur_tok.int_value;
                    next_token();
                    expect_rparen();
                }
                else if (tok_is_keyword("global.get")) {
                    next_token();
                    int gi = -1;
                    if (cur_tok.kind == T_INT) gi = (int)cur_tok.int_value;
                    else if (cur_tok.kind == T_IDENT) {
                        for (uint32_t i = 0; i < WASTRO_GLOBAL_CNT; i++) {
                            const char *gn = WASTRO_GLOBAL_NAMES[i];
                            if (gn && strlen(gn) == cur_tok.len && memcmp(gn, cur_tok.start, cur_tok.len) == 0) { gi = (int)i; break; }
                        }
                    }
                    if (gi < 0 || (uint32_t)gi >= WASTRO_GLOBAL_CNT)
                        parse_error("(data ...) global.get: unknown global");
                    offset = AS_U32(WASTRO_GLOBALS[gi]);
                    next_token();
                    expect_rparen();
                }
                else parse_error("malformed (data ...) offset");
                if (wrapped_offset) expect_rparen();
            }
            else if (cur_tok.kind == T_INT) {
                offset = (uint32_t)cur_tok.int_value;
                next_token();
                is_passive = 0;
            }
            // Concatenate one or more "..." string operands.
            uint8_t *bytes = NULL;
            uint32_t total = 0;
            while (cur_tok.kind == T_STRING) {
                uint32_t seg_len;
                uint8_t *seg = decode_wasm_str(&cur_tok, &seg_len);
                bytes = realloc(bytes, total + seg_len);
                memcpy(bytes + total, seg, seg_len);
                total += seg_len;
                free(seg);
                next_token();
            }
            expect_rparen();
            if (is_passive) {
                // Passive data segment (post-1.0 bulk-memory).  Accept
                // and discard — no auto-init.
                if (bytes) free(bytes);
            }
            else {
                if (MOD_DATA_SEG_CNT >= WASTRO_MAX_DATA_SEGS) parse_error("too many data segments");
                MOD_DATA_SEGS[MOD_DATA_SEG_CNT].offset = offset;
                MOD_DATA_SEGS[MOD_DATA_SEG_CNT].length = total;
                MOD_DATA_SEGS[MOD_DATA_SEG_CNT].bytes  = bytes;
                MOD_DATA_SEG_CNT++;
            }
        }
        else if (tok_is_keyword("type")) {
            // (type $sig (func (param ...)* (result T)?))
            next_token();
            char *tname = NULL;
            if (cur_tok.kind == T_IDENT) {
                tname = dup_token_str(&cur_tok);
                next_token();
            }
            expect_lparen();
            expect_keyword("func");
            struct wastro_type_sig sig = {0};
            sig.result_type = WT_VOID;
            while (cur_tok.kind == T_LPAREN) {
                const char *save_pos = src_pos;
                Token save_tok = cur_tok;
                next_token();
                if (tok_is_keyword("param")) {
                    next_token();
                    if (cur_tok.kind == T_IDENT) next_token();   // discard $name
                    do {
                        if (sig.param_cnt >= WASTRO_MAX_PARAMS)
                            parse_error("(type) too many params");
                        sig.param_types[sig.param_cnt++] = parse_wtype();
                    } while (cur_tok.kind == T_KEYWORD);
                    expect_rparen();
                }
                else if (tok_is_keyword("result")) {
                    next_token();
                    sig.result_type = parse_result_type();
                    expect_rparen();
                }
                else {
                    src_pos = save_pos;
                    cur_tok = save_tok;
                    break;
                }
            }
            expect_rparen();   // close (func ...)
            expect_rparen();   // close (type ...)
            if (WASTRO_TYPE_CNT >= WASTRO_MAX_TYPES) parse_error("too many (type ...)");
            WASTRO_TYPES[WASTRO_TYPE_CNT] = sig;
            WASTRO_TYPE_NAMES[WASTRO_TYPE_CNT] = tname;
            WASTRO_TYPE_CNT++;
        }
        else if (tok_is_keyword("table")) {
            // (table $name?
            //   (export "n")*
            //   (import "m" "f")?
            //   N M? funcref | funcref (elem $f0 ...)
            // )
            next_token();
            if (cur_tok.kind == T_IDENT) next_token();   // discard $name
            char *exname = NULL;
            char imp_mod[64] = {0}, imp_fld[64] = {0};
            int has_import = 0;
            while (try_inline_export(&exname)) { if (exname) { free(exname); exname = NULL; } }
            if (try_inline_import(imp_mod, imp_fld)) {
                has_import = 1;
                while (try_inline_export(&exname)) { if (exname) { free(exname); exname = NULL; } }
            }
            uint32_t init = 0;
            uint32_t maxn = 0xFFFFFFFFu;
            int auto_elem = 0;
            if (cur_tok.kind == T_INT) {
                init = (uint32_t)cur_tok.int_value;
                next_token();
                if (cur_tok.kind == T_INT) {
                    maxn = (uint32_t)cur_tok.int_value;
                    next_token();
                }
                if (!tok_is_keyword("funcref") && !tok_is_keyword("anyfunc") && !tok_is_keyword("externref"))
                    parse_error("(table ...) only supports funcref");
                next_token();
            }
            else if (tok_is_keyword("funcref") || tok_is_keyword("anyfunc") || tok_is_keyword("externref")) {
                // Inline-elem form: `(table funcref (elem $f0 $f1 ...))`.
                next_token();
                auto_elem = 1;
            }
            else parse_error("(table ...) expects integer min or funcref");

            if (MOD_HAS_TABLE) parse_error("multiple (table ...) declarations (wasm 1.0 allows one)");
            MOD_HAS_TABLE = 1;

            if (auto_elem) {
                // Expect a `(elem ...)` form whose entry count determines the
                // table size, with offset 0.
                if (cur_tok.kind != T_LPAREN) parse_error("(table funcref ...) needs (elem ...)");
                next_token();
                if (!tok_is_keyword("elem")) parse_error("(table funcref ...) needs (elem ...)");
                next_token();
                if (PENDING_ELEM_CNT >= WASTRO_MAX_ELEM_SEGS) parse_error("too many (elem ...)");
                struct elem_pending *ep = &PENDING_ELEMS[PENDING_ELEM_CNT++];
                ep->offset = 0;
                ep->cnt = 0;
                ep->refs = malloc(sizeof(Token) * 64);
                while (cur_tok.kind != T_RPAREN) {
                    if (ep->cnt >= 64) parse_error("(elem) too many entries");
                    if (cur_tok.kind == T_LPAREN) {
                        next_token();
                        if (!tok_is_keyword("ref.func"))
                            parse_error("(table funcref (elem ...)) only accepts ref.func or func refs");
                        next_token();
                        ep->refs[ep->cnt++] = cur_tok;
                        next_token();
                        expect_rparen();
                    }
                    else {
                        ep->refs[ep->cnt++] = cur_tok;
                        next_token();
                    }
                }
                expect_rparen();   // close (elem ...)
                init = ep->cnt;
                maxn = init;
            }
            WASTRO_TABLE_SIZE = init;
            WASTRO_TABLE_MAX = maxn;
            WASTRO_TABLE = malloc(sizeof(int32_t) * (init ? init : 1));
            for (uint32_t k = 0; k < init; k++) WASTRO_TABLE[k] = -1;
            (void)has_import; (void)imp_mod; (void)imp_fld;
            expect_rparen();
        }
        else if (tok_is_keyword("elem")) {
            // (elem (offset (i32.const N)) $f0 $f1 ...)
            // (elem (i32.const N)         $f0 $f1 ...)
            // (elem N                     $f0 $f1 ...)
            // (elem $tab? <offset-form>    $f0 ...)
            next_token();
            // optional table reference
            if (cur_tok.kind == T_IDENT) next_token();
            uint32_t offset = 0;
            if (cur_tok.kind == T_LPAREN) {
                next_token();
                if (tok_is_keyword("offset")) {
                    next_token();
                    expect_lparen();
                    expect_keyword("i32.const");
                    if (cur_tok.kind != T_INT) parse_error("expected elem offset literal");
                    offset = (uint32_t)cur_tok.int_value;
                    next_token();
                    expect_rparen();
                    expect_rparen();
                }
                else if (tok_is_keyword("i32.const")) {
                    next_token();
                    if (cur_tok.kind != T_INT) parse_error("expected elem offset literal");
                    offset = (uint32_t)cur_tok.int_value;
                    next_token();
                    expect_rparen();
                }
                else parse_error("malformed (elem ...) offset");
            }
            else if (cur_tok.kind == T_INT) {
                offset = (uint32_t)cur_tok.int_value;
                next_token();
            }
            // Function references — collect tokens; resolve to func
            // indices after scan_module finishes (so that elem can
            // reference functions declared later in the module).  We
            // accept both bare `$foo` / `N` and `(ref.func $foo)` forms.
            if (PENDING_ELEM_CNT >= WASTRO_MAX_ELEM_SEGS) parse_error("too many (elem ...)");
            struct elem_pending *ep = &PENDING_ELEMS[PENDING_ELEM_CNT++];
            ep->offset = offset;
            ep->cnt = 0;
            ep->refs = malloc(sizeof(Token) * 64);
            while (cur_tok.kind != T_RPAREN) {
                if (ep->cnt >= 64) parse_error("(elem) too many entries (>64)");
                if (cur_tok.kind == T_LPAREN) {
                    next_token();
                    if (!tok_is_keyword("ref.func"))
                        parse_error("(elem ...) only accepts ref.func or func refs");
                    next_token();
                    ep->refs[ep->cnt++] = cur_tok;
                    next_token();
                    expect_rparen();
                }
                else {
                    ep->refs[ep->cnt++] = cur_tok;
                    next_token();
                }
            }
            expect_rparen();
        }
        else if (tok_is_keyword("export")) {
            // (export "name" (func $f|N))
            // (export "name" (memory N))   — memory/global/table exports accepted, ignored
            // (export "name" (global N))
            // (export "name" (table N))
            next_token();
            if (cur_tok.kind != T_STRING) parse_error("(export) expects name string");
            char *exname = dup_token_str(&cur_tok);
            next_token();
            expect_lparen();
            if (tok_is_keyword("func")) {
                next_token();
                if (PENDING_EXPORT_CNT >= WASTRO_MAX_EXPORTS) parse_error("too many exports");
                PENDING_EXPORTS[PENDING_EXPORT_CNT].name = exname;
                PENDING_EXPORTS[PENDING_EXPORT_CNT].ref = cur_tok;
                PENDING_EXPORT_CNT++;
                next_token();
                expect_rparen();
                expect_rparen();
            }
            else if (tok_is_keyword("memory") || tok_is_keyword("global") || tok_is_keyword("table")) {
                free(exname);
                next_token();
                if (cur_tok.kind == T_IDENT || cur_tok.kind == T_INT) next_token();
                expect_rparen();
                expect_rparen();
            }
            else parse_error("(export) only supports func / memory / global / table");
        }
        else if (tok_is_keyword("start")) {
            // (start $f | N)
            next_token();
            MOD_START_TOK = cur_tok;
            MOD_HAS_START = 1;
            next_token();
            expect_rparen();
        }
        else {
            parse_error("unknown module-level form");
        }
    }
    expect_rparen();
    *func_count_out = n;
}

