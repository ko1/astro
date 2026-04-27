// wastro — WebAssembly subset on ASTro.
//
// v0: minimal WAT (text format) front-end + driver.  Only the
// folded S-expression form is supported.  Subset:
//
//   (module
//     (func $name (export "name")? (param $p i32)* (result i32)?
//       <expr>* )* )
//
//   <expr> ::=
//     (i32.const N)
//     (local.get $name | N)
//     (local.set $name | N <expr>)
//     (i32.add | i32.sub | i32.mul | i32.eq | i32.lt_s <expr> <expr>)
//     (if (result i32)? <cond-expr> (then <expr>+) (else <expr>+)?)
//     (call $name | N <expr>*)
//
// Multi-statement function bodies are folded right-to-left into
// node_seq nodes.  v0 has no memory, no loops, no traps — just
// enough to make `fib`, `tak`, and similar recursive numeric
// programs run end-to-end.

#include <ctype.h>
#include <errno.h>
#include "context.h"
#include "node.h"
#include "astro_code_store.h"

struct wastro_option OPTION;

// =====================================================================
// Tokenizer
// =====================================================================

typedef enum {
    T_LPAREN,
    T_RPAREN,
    T_IDENT,    // $foo
    T_KEYWORD,  // module / func / i32.add / ...
    T_INT,
    T_STRING,
    T_EOF,
} token_kind_t;

typedef struct {
    token_kind_t kind;
    const char *start;
    size_t len;
    int64_t int_value;
    double float_value;
    int has_dot;     // 1 if the numeric token contained '.'/'e'/'E' — i.e., a float
} Token;

static const char *src_pos;
static const char *src_end;
static Token cur_tok;

static void
skip_ws_and_comments(void)
{
    for (;;) {
        while (src_pos < src_end && isspace((unsigned char)*src_pos)) src_pos++;
        if (src_pos + 1 < src_end && src_pos[0] == ';' && src_pos[1] == ';') {
            while (src_pos < src_end && *src_pos != '\n') src_pos++;
            continue;
        }
        if (src_pos + 1 < src_end && src_pos[0] == '(' && src_pos[1] == ';') {
            src_pos += 2;
            int depth = 1;
            while (src_pos + 1 < src_end && depth > 0) {
                if (src_pos[0] == '(' && src_pos[1] == ';') { depth++; src_pos += 2; }
                else if (src_pos[0] == ';' && src_pos[1] == ')') { depth--; src_pos += 2; }
                else src_pos++;
            }
            continue;
        }
        break;
    }
}

static int
is_keyword_char(int ch)
{
    return isalnum(ch) || ch == '.' || ch == '_';
}

static void
next_token(void)
{
    skip_ws_and_comments();
    if (src_pos >= src_end) { cur_tok.kind = T_EOF; return; }
    char ch = *src_pos;

    if (ch == '(') { cur_tok.kind = T_LPAREN; cur_tok.start = src_pos++; cur_tok.len = 1; return; }
    if (ch == ')') { cur_tok.kind = T_RPAREN; cur_tok.start = src_pos++; cur_tok.len = 1; return; }

    if (ch == '"') {
        const char *start = ++src_pos;
        while (src_pos < src_end && *src_pos != '"') src_pos++;
        cur_tok.kind = T_STRING;
        cur_tok.start = start;
        cur_tok.len = (size_t)(src_pos - start);
        if (src_pos < src_end) src_pos++; // consume closing "
        return;
    }

    if (ch == '$') {
        const char *start = src_pos++;
        while (src_pos < src_end && (isalnum((unsigned char)*src_pos) || *src_pos == '_' || *src_pos == '$' || *src_pos == '.')) src_pos++;
        cur_tok.kind = T_IDENT;
        cur_tok.start = start;
        cur_tok.len = (size_t)(src_pos - start);
        return;
    }

    if (ch == '-' || ch == '+' || isdigit((unsigned char)ch)) {
        const char *start = src_pos;
        // Decide int vs float by scanning for '.', 'e', 'E', 'p', 'P'.
        const char *p = src_pos;
        if (*p == '+' || *p == '-') p++;
        int saw_dot = 0;
        while (p < src_end &&
               (isalnum((unsigned char)*p) || *p == '.' || *p == '_' ||
                *p == '+' || *p == '-')) {
            if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'p' || *p == 'P') {
                saw_dot = 1;
            }
            p++;
        }
        char *end;
        if (saw_dot) {
            errno = 0;
            double dv = strtod(src_pos, &end);
            if (end != src_pos && errno == 0) {
                src_pos = end;
                cur_tok.kind = T_INT;        // tagged via has_dot for caller
                cur_tok.start = start;
                cur_tok.len = (size_t)(src_pos - start);
                cur_tok.float_value = dv;
                cur_tok.int_value = (int64_t)dv;  // caller can use either
                cur_tok.has_dot = 1;
                return;
            }
        }
        else {
            errno = 0;
            long long v = strtoll(src_pos, &end, 0);
            if (end != src_pos && errno == 0) {
                src_pos = end;
                cur_tok.kind = T_INT;
                cur_tok.start = start;
                cur_tok.len = (size_t)(src_pos - start);
                cur_tok.int_value = (int64_t)v;
                cur_tok.has_dot = 0;
                return;
            }
        }
    }

    // keyword
    const char *start = src_pos;
    while (src_pos < src_end && is_keyword_char((unsigned char)*src_pos)) src_pos++;
    cur_tok.kind = T_KEYWORD;
    cur_tok.start = start;
    cur_tok.len = (size_t)(src_pos - start);
}

static int
tok_is_keyword(const char *kw)
{
    if (cur_tok.kind != T_KEYWORD) return 0;
    size_t kl = strlen(kw);
    return kl == cur_tok.len && memcmp(cur_tok.start, kw, kl) == 0;
}

static int
tok_eq_string(const Token *t, const char *s)
{
    size_t sl = strlen(s);
    return sl == t->len && memcmp(t->start, s, sl) == 0;
}

static void
parse_error(const char *msg)
{
    fprintf(stderr, "wastro: parse error: %s (near '%.*s')\n",
            msg, (int)cur_tok.len, cur_tok.start);
    exit(1);
}

static void
expect_lparen(void) { if (cur_tok.kind != T_LPAREN) parse_error("expected '('"); next_token(); }
static void
expect_rparen(void) { if (cur_tok.kind != T_RPAREN) parse_error("expected ')'"); next_token(); }
static void
expect_keyword(const char *kw) { if (!tok_is_keyword(kw)) parse_error(kw); next_token(); }

// =====================================================================
// Module / function tables
// =====================================================================

struct wastro_function WASTRO_FUNCS[WASTRO_MAX_FUNCS];
uint32_t WASTRO_FUNC_CNT = 0;

// Module-level state for memory, globals, br_table targets.

// Globals: parser-managed flat arrays.
VALUE *WASTRO_GLOBALS = NULL;
static wtype_t WASTRO_GLOBAL_TYPES[WASTRO_MAX_GLOBALS];
static int     WASTRO_GLOBAL_MUT[WASTRO_MAX_GLOBALS];   // 1 = mut, 0 = const
static char   *WASTRO_GLOBAL_NAMES[WASTRO_MAX_GLOBALS]; // optional $name
static uint32_t WASTRO_GLOBAL_CNT = 0;

// br_table targets.
uint32_t *WASTRO_BR_TABLE = NULL;
static uint32_t WASTRO_BR_TABLE_CNT = 0;
static uint32_t WASTRO_BR_TABLE_CAP = 0;

// Memory declaration captured during parse (applied to CTX in driver).
static uint32_t MOD_MEM_INITIAL_PAGES = 0;
static uint32_t MOD_MEM_MAX_PAGES = 65536;
static int      MOD_HAS_MEMORY = 0;

// Data segments — written to memory at instantiation.
struct wastro_data_seg {
    uint32_t offset;
    uint32_t length;
    uint8_t *bytes;
};
#define WASTRO_MAX_DATA_SEGS 64
static struct wastro_data_seg MOD_DATA_SEGS[WASTRO_MAX_DATA_SEGS];
static uint32_t MOD_DATA_SEG_CNT = 0;

// =====================================================================
// Built-in host function registry (env.*)
// =====================================================================

static VALUE host_log_i32(CTX *c, VALUE *args, uint32_t argc) {
    (void)c; (void)argc;
    printf("%d\n", (int)AS_I32(args[0])); fflush(stdout);
    return 0;
}
static VALUE host_log_i64(CTX *c, VALUE *args, uint32_t argc) {
    (void)c; (void)argc;
    printf("%lld\n", (long long)AS_I64(args[0])); fflush(stdout);
    return 0;
}
static VALUE host_log_f32(CTX *c, VALUE *args, uint32_t argc) {
    (void)c; (void)argc;
    printf("%g\n", (double)AS_F32(args[0])); fflush(stdout);
    return 0;
}
static VALUE host_log_f64(CTX *c, VALUE *args, uint32_t argc) {
    (void)c; (void)argc;
    printf("%g\n", AS_F64(args[0])); fflush(stdout);
    return 0;
}
static VALUE host_putchar(CTX *c, VALUE *args, uint32_t argc) {
    (void)c; (void)argc;
    putchar((int)(AS_I32(args[0]) & 0xFF));
    return 0;
}
// print_bytes(ptr, len) — write `len` bytes starting at memory[ptr] to stdout.
static VALUE host_print_bytes(CTX *c, VALUE *args, uint32_t argc) {
    (void)argc;
    uint32_t ptr = AS_U32(args[0]);
    uint32_t len = AS_U32(args[1]);
    if (!c->memory) wastro_trap("env.print_bytes called without memory");
    if ((uint64_t)ptr + len > (uint64_t)c->memory_pages * WASTRO_PAGE_SIZE)
        wastro_trap("env.print_bytes out of bounds");
    fwrite(c->memory + ptr, 1, len, stdout);
    fflush(stdout);
    return 0;
}

struct host_entry {
    const char *module;
    const char *field;
    wastro_host_fn_t fn;
    wtype_t param_types[8];
    uint32_t param_cnt;
    wtype_t result_type;
};
static const struct host_entry HOST_REGISTRY[] = {
    { "env", "log_i32",     host_log_i32,     { WT_I32 },        1, WT_VOID },
    { "env", "log_i64",     host_log_i64,     { WT_I64 },        1, WT_VOID },
    { "env", "log_f32",     host_log_f32,     { WT_F32 },        1, WT_VOID },
    { "env", "log_f64",     host_log_f64,     { WT_F64 },        1, WT_VOID },
    { "env", "putchar",     host_putchar,     { WT_I32 },        1, WT_VOID },
    { "env", "print_bytes", host_print_bytes, { WT_I32, WT_I32 },2, WT_VOID },
    { NULL,  NULL,          NULL,             { 0 },             0, WT_VOID },
};

static const struct host_entry *
find_host(const char *mod, const char *field)
{
    for (const struct host_entry *h = HOST_REGISTRY; h->module; h++) {
        if (strcmp(h->module, mod) == 0 && strcmp(h->field, field) == 0) return h;
    }
    return NULL;
}

// Decode a wasm string token (between the quotes) into raw bytes.
// Caller frees the returned buffer.  `*out_len` receives the byte
// count.  Wasm strings allow `\xx` (hex), `\n`, `\t`, `\r`, `\\`,
// `\'`, `\"`, `\0`.
static uint8_t *
decode_wasm_str(const Token *t, uint32_t *out_len)
{
    uint8_t *buf = malloc(t->len + 1);  // upper bound — escapes shrink
    uint32_t bi = 0;
    for (size_t i = 0; i < t->len; i++) {
        unsigned char ch = (unsigned char)t->start[i];
        if (ch == '\\' && i + 1 < t->len) {
            unsigned char e = (unsigned char)t->start[i + 1];
            if (e == 'n')      { buf[bi++] = '\n'; i++; }
            else if (e == 't') { buf[bi++] = '\t'; i++; }
            else if (e == 'r') { buf[bi++] = '\r'; i++; }
            else if (e == '\\') { buf[bi++] = '\\'; i++; }
            else if (e == '\'') { buf[bi++] = '\''; i++; }
            else if (e == '"') { buf[bi++] = '"';  i++; }
            else if (e == '0') { buf[bi++] = '\0'; i++; }
            else if (i + 2 < t->len && isxdigit(e) && isxdigit((unsigned char)t->start[i + 2])) {
                char hex[3] = { (char)e, t->start[i + 2], 0 };
                buf[bi++] = (uint8_t)strtoul(hex, NULL, 16);
                i += 2;
            }
            else buf[bi++] = ch;   // unrecognised — keep backslash literal
        }
        else {
            buf[bi++] = ch;
        }
    }
    *out_len = bi;
    return buf;
}

int
wastro_find_export(const char *name)
{
    for (uint32_t i = 0; i < WASTRO_FUNC_CNT; i++) {
        if (WASTRO_FUNCS[i].exported && WASTRO_FUNCS[i].export_name &&
            strcmp(WASTRO_FUNCS[i].export_name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// Find function index by `$name` or numeric reference.
static int
resolve_func(const Token *t)
{
    if (t->kind == T_INT) return (int)t->int_value;
    if (t->kind != T_IDENT) parse_error("expected function ref");
    for (uint32_t i = 0; i < WASTRO_FUNC_CNT; i++) {
        const char *fn = WASTRO_FUNCS[i].name;
        if (fn && (strlen(fn) == t->len) && memcmp(fn, t->start, t->len) == 0) {
            return (int)i;
        }
    }
    fprintf(stderr, "wastro: unknown function '%.*s'\n", (int)t->len, t->start);
    exit(1);
}

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
    if (got != want) {
        fprintf(stderr, "wastro: type mismatch at %s: expected %s, got %s\n",
                site, wtype_name(want), wtype_name(got));
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
    TypedExpr stmts[256];
    uint32_t n = 0;
    while (cur_tok.kind != T_RPAREN) {
        if (n >= 256) parse_error("too many statements");
        stmts[n++] = parse_expr(env, labels);
    }
    return build_seq(stmts, n);
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
            result_t = parse_wtype();
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

    if (has_result) {
        expect_type(then_branch.type, result_t, "if-then branch");
        expect_type(else_branch.type, result_t, "if-else branch");
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

// Generic binary-op helper: parse two operands, validate they have
// the expected operand type, and return the result with the given
// result type.
#define BIN_OP(KW, OPND_T, RES_T, ALLOC)                            \
    if (tok_is_keyword(KW)) {                                       \
        next_token();                                               \
        TypedExpr l = parse_expr(env, labels);                      \
        TypedExpr r = parse_expr(env, labels);                      \
        expect_type(l.type, OPND_T, KW " left");                    \
        expect_type(r.type, OPND_T, KW " right");                   \
        expect_rparen();                                            \
        return (TypedExpr){ALLOC(l.node, r.node), RES_T};           \
    }

#define UN_OP(KW, OPND_T, RES_T, ALLOC)                             \
    if (tok_is_keyword(KW)) {                                       \
        next_token();                                               \
        TypedExpr e = parse_expr(env, labels);                      \
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
        next_token();
        expect_rparen();
        float fv = (float)dv;
        uint32_t bits;
        memcpy(&bits, &fv, 4);
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
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_f64_const(dv), WT_F64};
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
        return (TypedExpr){ALLOC_node_local_get((uint32_t)idx), env->types[idx]};
    }
    if (tok_is_keyword("local.set")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        TypedExpr e = parse_expr(env, labels);
        expect_type(e.type, env->types[idx], "local.set value");
        expect_rparen();
        return (TypedExpr){ALLOC_node_local_set((uint32_t)idx, e.node), WT_VOID};
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
            TypedExpr addr = parse_expr(env, labels);               \
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
            TypedExpr addr  = parse_expr(env, labels);              \
            TypedExpr value = parse_expr(env, labels);              \
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
        return (TypedExpr){ALLOC_node_unreachable(), WT_VOID};
    }

    // ------- control -------
    if (tok_is_keyword("nop")) {
        next_token();
        expect_rparen();
        return (TypedExpr){ALLOC_node_nop(), WT_VOID};
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
                result_t = parse_wtype();
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
            // — block's value comes from br_value at runtime.
            if (body.type != WT_VOID && body.type != result_t) {
                fprintf(stderr,
                    "wastro: type mismatch at %s body: expected %s, got %s\n",
                    is_loop ? "loop" : "block",
                    wtype_name(result_t), wtype_name(body.type));
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
            return (TypedExpr){ALLOC_node_br(depth), WT_VOID};
        }
        TypedExpr v = parse_expr(env, labels);
        // Carried-value type must match target's result type.
        // (Loop targets carry no value per wasm spec, but we accept for simplicity.)
        wtype_t want = labels->result_types[labels->cnt - 1 - depth];
        if (want != WT_VOID) expect_type(v.type, want, "br value");
        expect_rparen();
        return (TypedExpr){ALLOC_node_br_v(depth, v.node), WT_VOID};
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
                WT_VOID,
            };
        }
        TypedExpr idx = parse_expr(env, labels);
        expect_type(idx.type, WT_I32, "br_table index");
        expect_rparen();
        return (TypedExpr){
            ALLOC_node_br_table_v(target_index, target_cnt, default_depth, idx.node, first.node),
            WT_VOID,
        };
    }

    // (return)  /  (return <value>)
    if (tok_is_keyword("return")) {
        next_token();
        if (cur_tok.kind == T_RPAREN) {
            expect_rparen();
            return (TypedExpr){ALLOC_node_return(), WT_VOID};
        }
        TypedExpr v = parse_expr(env, labels);
        expect_rparen();
        return (TypedExpr){ALLOC_node_return_v(v.node), WT_VOID};
    }

    // ------- call -------
    if (tok_is_keyword("call")) {
        next_token();
        int func_idx = resolve_func(&cur_tok);
        struct wastro_function *callee = &WASTRO_FUNCS[func_idx];
        next_token();
        NODE *args[8]; uint32_t argc = 0;
        while (cur_tok.kind != T_RPAREN) {
            if (argc >= 8) parse_error("call arity > 8 not supported");
            TypedExpr a = parse_expr(env, labels);
            if (argc >= callee->param_cnt) {
                fprintf(stderr, "wastro: too many args to '%s'\n",
                        callee->name ? callee->name : "<unnamed>");
                exit(1);
            }
            expect_type(a.type, callee->param_types[argc], "call argument");
            args[argc++] = a.node;
        }
        if (argc != callee->param_cnt) {
            fprintf(stderr, "wastro: '%s' expects %u arg(s), got %u\n",
                    callee->name ? callee->name : "<unnamed>",
                    callee->param_cnt, argc);
            exit(1);
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
            switch (argc) {
            case 0: call_node = ALLOC_node_call_0((uint32_t)func_idx, local_cnt); break;
            case 1: call_node = ALLOC_node_call_1((uint32_t)func_idx, local_cnt, args[0]); break;
            case 2: call_node = ALLOC_node_call_2((uint32_t)func_idx, local_cnt, args[0], args[1]); break;
            case 3: call_node = ALLOC_node_call_3((uint32_t)func_idx, local_cnt, args[0], args[1], args[2]); break;
            case 4: call_node = ALLOC_node_call_4((uint32_t)func_idx, local_cnt, args[0], args[1], args[2], args[3]); break;
            default:
                parse_error("call arity 5..8 needs node_call_5..8");
                return (TypedExpr){NULL, WT_VOID};
            }
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
    expect_lparen();
    return parse_op(env, labels);
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
    // ( func ($name)? (export "n")? (param $p i32)* (result i32)? <body...>
    expect_keyword("func");
    if (cur_tok.kind == T_IDENT) {
        WASTRO_FUNCS[idx].name = dup_token_str(&cur_tok);
        next_token();
    }
    if (cur_tok.kind == T_LPAREN) {
        // Look at next token for export
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
        else {
            src_pos = save_pos;
            cur_tok = save_tok;
        }
    }
}

// Expects cur_tok to be the '(' of the func.  Consumes the entire form.
static void
parse_func_body(int idx, LocalEnv *env)
{
    LabelEnv labels = {0};
    TypedExpr stmts[256];
    uint32_t n = 0;
    while (cur_tok.kind != T_RPAREN) {
        if (n >= 256) parse_error("too many top-level stmts in body");
        stmts[n++] = parse_expr(env, &labels);
    }
    TypedExpr body = build_seq(stmts, n);
    if (WASTRO_FUNCS[idx].result_type != WT_VOID) {
        // body.type may be WT_VOID if the body always tail-branches /
        // returns rather than producing a value structurally.  Allow that.
        if (body.type != WT_VOID) {
            expect_type(body.type, WASTRO_FUNCS[idx].result_type, "function body");
        }
    }
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
            wtype_t rt = parse_wtype();
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
            // (import "mod" "field" (func $name? (param T)* (result T)?))
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
            if (!tok_is_keyword("func")) parse_error("only (func ...) imports supported");
            next_token();
            char *iname = NULL;
            if (cur_tok.kind == T_IDENT) {
                iname = dup_token_str(&cur_tok);
                next_token();
            }
            // Allocate function index and resolve host fn from the registry.
            const struct host_entry *he = find_host(mod, fld);
            if (!he) {
                fprintf(stderr,
                    "wastro: unknown import \"%s\".\"%s\" (registry has env.{log_i32,log_i64,log_f32,log_f64,putchar,print_bytes})\n",
                    mod, fld);
                exit(1);
            }
            if (n >= WASTRO_MAX_FUNCS) parse_error("too many functions");
            int fi = n;
            WASTRO_FUNCS[fi].name = iname;
            WASTRO_FUNCS[fi].is_import = 1;
            WASTRO_FUNCS[fi].host_fn = he->fn;
            WASTRO_FUNCS[fi].param_cnt = he->param_cnt;
            for (uint32_t i = 0; i < he->param_cnt; i++)
                WASTRO_FUNCS[fi].param_types[i] = he->param_types[i];
            WASTRO_FUNCS[fi].result_type = he->result_type;
            WASTRO_FUNCS[fi].local_cnt = 0;
            // Skip the rest of the (func ...) signature — the host
            // registry is authoritative for the signature.  We just
            // need the parens balanced.
            int depth = 1;
            while (cur_tok.kind != T_EOF && depth > 0) {
                if (cur_tok.kind == T_LPAREN) depth++;
                else if (cur_tok.kind == T_RPAREN) depth--;
                if (depth > 0) next_token();
            }
            expect_rparen();
            expect_rparen();   // close outer (import ...)
            WASTRO_FUNC_CNT++;
            n++;
            // Imports occupy func indexes too — but mark a sentinel
            // so pass-2 doesn't try to parse a body for them.
            func_offsets[fi] = NULL;
        }
        else if (tok_is_keyword("memory")) {
            // (memory $name? min_pages max_pages?)
            next_token();
            if (cur_tok.kind == T_IDENT) next_token();   // discard $name
            if (cur_tok.kind != T_INT) parse_error("(memory ...) expects integer min");
            MOD_MEM_INITIAL_PAGES = (uint32_t)cur_tok.int_value;
            next_token();
            if (cur_tok.kind == T_INT) {
                MOD_MEM_MAX_PAGES = (uint32_t)cur_tok.int_value;
                next_token();
            }
            MOD_HAS_MEMORY = 1;
            expect_rparen();
        }
        else if (tok_is_keyword("global")) {
            // (global $name? (mut T)? T <init-expr>) — init is a const expr.
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
            else {
                gt = parse_wtype();
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
            // (data $name? (offset (i32.const N)) "bytes")
            // (data $name? (i32.const N) "bytes")    — sugar
            // (data $name? N "bytes")                 — bare offset (legacy)
            next_token();
            if (cur_tok.kind == T_IDENT) next_token();   // discard $name
            uint32_t offset = 0;
            if (cur_tok.kind == T_LPAREN) {
                next_token();
                if (tok_is_keyword("offset")) {
                    next_token();
                    expect_lparen();
                    expect_keyword("i32.const");
                    if (cur_tok.kind != T_INT) parse_error("expected offset literal");
                    offset = (uint32_t)cur_tok.int_value;
                    next_token();
                    expect_rparen();
                    expect_rparen();
                }
                else if (tok_is_keyword("i32.const")) {
                    next_token();
                    if (cur_tok.kind != T_INT) parse_error("expected offset literal");
                    offset = (uint32_t)cur_tok.int_value;
                    next_token();
                    expect_rparen();
                }
                else parse_error("malformed (data ...) offset");
            }
            else if (cur_tok.kind == T_INT) {
                offset = (uint32_t)cur_tok.int_value;
                next_token();
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
            if (MOD_DATA_SEG_CNT >= WASTRO_MAX_DATA_SEGS) parse_error("too many data segments");
            MOD_DATA_SEGS[MOD_DATA_SEG_CNT].offset = offset;
            MOD_DATA_SEGS[MOD_DATA_SEG_CNT].length = total;
            MOD_DATA_SEGS[MOD_DATA_SEG_CNT].bytes  = bytes;
            MOD_DATA_SEG_CNT++;
        }
        else {
            parse_error("only (func ...), (memory ...), (global ...), (data ...) supported at module top-level");
        }
    }
    expect_rparen();
    *func_count_out = n;
}

NODE *
wastro_load_module(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); exit(1); }
    buf[sz] = '\0';
    fclose(f);
    MODULE_TEXT_START = buf;

    const char *func_offsets[WASTRO_MAX_FUNCS];
    int n = 0;
    scan_module(buf, (size_t)sz, func_offsets, &n);

    // Pass 2: parse each func body in order.  Imports are skipped
    // (their func_offsets entry is NULL).
    for (int i = 0; i < n; i++) {
        if (!func_offsets[i]) continue;   // import slot
        src_pos = func_offsets[i];
        src_end = buf + sz;
        next_token();
        parse_func_pass2(i);
    }
    return NULL; // module-level AST not needed; functions are addressable via WASTRO_FUNCS.
}

// =====================================================================
// AOT compile / load (mirrors abruby's --aot-compile / -c flow)
// =====================================================================

static void
compile_all_funcs(int verbose)
{
    for (uint32_t i = 0; i < WASTRO_FUNC_CNT; i++) {
        if (verbose) {
            fprintf(stderr, "cs_compile: $%s\n",
                    WASTRO_FUNCS[i].name ? WASTRO_FUNCS[i].name + 1 : "anon");
        }
        astro_cs_compile(WASTRO_FUNCS[i].body, NULL);  // AOT: file=NULL
    }
    if (verbose) fprintf(stderr, "cs_build\n");
    astro_cs_build(NULL);
    astro_cs_reload();
}

static void
load_all_funcs(int verbose)
{
    for (uint32_t i = 0; i < WASTRO_FUNC_CNT; i++) {
        bool ok = astro_cs_load(WASTRO_FUNCS[i].body, NULL);
        if (verbose) {
            fprintf(stderr, "cs_load: $%s -> %s\n",
                    WASTRO_FUNCS[i].name ? WASTRO_FUNCS[i].name + 1 : "anon",
                    ok ? "specialized" : "default");
        }
    }
}

// =====================================================================
// Driver
// =====================================================================

static void
usage(void)
{
    fprintf(stderr,
        "usage: wastro [options] <module.wat> [<export> [arg ...]]\n"
        "options:\n"
        "  -q, --quiet         suppress code-store messages\n"
        "  -v, --verbose       trace cs_compile/build/load steps\n"
        "  --no-compile        disable code-store consultation entirely\n"
        "  -c                  AOT-compile all functions before running\n"
        "  --aot               AOT-compile only, then exit (no <export> needed)\n"
        "  --clear-cs          delete code_store/ before starting\n");
    exit(2);
}

int
main(int argc, char *argv[])
{
    int ai = 1;
    int compile_first = 0;       // -c
    int aot_only_mode = 0;       // --aot (no run)
    int clear_cs = 0;            // --clear-cs
    int verbose = 0;             // -v / --verbose
    while (ai < argc && argv[ai][0] == '-') {
        if (!strcmp(argv[ai], "-q") || !strcmp(argv[ai], "--quiet")) OPTION.quiet = true;
        else if (!strcmp(argv[ai], "-v") || !strcmp(argv[ai], "--verbose")) verbose = 1;
        else if (!strcmp(argv[ai], "--no-compile")) OPTION.no_compiled_code = true;
        else if (!strcmp(argv[ai], "-c")) compile_first = 1;
        else if (!strcmp(argv[ai], "--aot") || !strcmp(argv[ai], "--aot-compile")) aot_only_mode = 1;
        else if (!strcmp(argv[ai], "--clear-cs") || !strcmp(argv[ai], "--ccs")) clear_cs = 1;
        else if (!strcmp(argv[ai], "-h") || !strcmp(argv[ai], "--help")) usage();
        else { fprintf(stderr, "wastro: unknown option %s\n", argv[ai]); usage(); }
        ai++;
    }
    if (clear_cs) (void)system("rm -rf code_store");

    if (aot_only_mode) {
        // --aot: <module.wat> only.
        if (argc - ai < 1) usage();
    }
    else {
        // run: <module.wat> <export> [args]
        if (argc - ai < 2) usage();
    }
    const char *wat_path = argv[ai++];

    INIT();
    wastro_load_module(wat_path);

    if (aot_only_mode) {
        compile_all_funcs(verbose);
        return 0;
    }

    if (compile_first) {
        compile_all_funcs(verbose);
        load_all_funcs(verbose);
    }

    const char *export_name = argv[ai++];
    int func_idx = wastro_find_export(export_name);
    if (func_idx < 0) {
        fprintf(stderr, "wastro: export '%s' not found\n", export_name);
        return 1;
    }
    struct wastro_function *fn = &WASTRO_FUNCS[func_idx];
    int provided = argc - ai;
    if ((uint32_t)provided != fn->param_cnt) {
        fprintf(stderr, "wastro: '%s' expects %u arg(s), got %d\n",
                export_name, fn->param_cnt, provided);
        return 1;
    }

    CTX *c = malloc(sizeof(CTX));
    c->fp = c->stack;
    c->sp = c->stack + fn->local_cnt;
    c->br_depth = 0;
    c->br_value = 0;
    if (MOD_HAS_MEMORY) {
        size_t bytes = (size_t)MOD_MEM_INITIAL_PAGES * WASTRO_PAGE_SIZE;
        c->memory = bytes ? calloc(1, bytes) : NULL;
        c->memory_pages = MOD_MEM_INITIAL_PAGES;
        c->memory_max_pages = MOD_MEM_MAX_PAGES;
    } else {
        c->memory = NULL;
        c->memory_pages = 0;
        c->memory_max_pages = 0;
    }
    // Apply (data ...) segments now that memory is allocated.
    for (uint32_t di = 0; di < MOD_DATA_SEG_CNT; di++) {
        struct wastro_data_seg *d = &MOD_DATA_SEGS[di];
        if (!c->memory) {
            fprintf(stderr, "wastro: (data ...) without (memory ...)\n"); return 1;
        }
        size_t mem_bytes = (size_t)c->memory_pages * WASTRO_PAGE_SIZE;
        if ((size_t)d->offset + d->length > mem_bytes) {
            fprintf(stderr, "wastro: data segment %u overflows memory\n", di); return 1;
        }
        memcpy(c->memory + d->offset, d->bytes, d->length);
    }
    for (uint32_t i = 0; i < fn->param_cnt; i++) {
        const char *s = argv[ai + i];
        switch (fn->param_types[i]) {
        case WT_I32: c->fp[i] = FROM_I32((int32_t)strtol(s, NULL, 0)); break;
        case WT_I64: c->fp[i] = FROM_I64((int64_t)strtoll(s, NULL, 0)); break;
        case WT_F32: c->fp[i] = FROM_F32((float)strtod(s, NULL)); break;
        case WT_F64: c->fp[i] = FROM_F64(strtod(s, NULL)); break;
        default:     c->fp[i] = 0;
        }
    }
    for (uint32_t i = fn->param_cnt; i < fn->local_cnt; i++) {
        c->fp[i] = 0;
    }

    if (fn->is_import) {
        fprintf(stderr, "wastro: cannot directly invoke imported function '%s'\n", export_name);
        return 1;
    }
    VALUE result = EVAL(c, fn->body);
    if (c->br_depth == WASTRO_BR_RETURN) {
        result = c->br_value;
        c->br_depth = 0;
    }
    switch (fn->result_type) {
    case WT_I32: printf("%d\n",        (int)AS_I32(result)); break;
    case WT_I64: printf("%lld\n", (long long)AS_I64(result)); break;
    case WT_F32: printf("%g\n",     (double)AS_F32(result)); break;
    case WT_F64: printf("%g\n",             AS_F64(result)); break;
    default:     break;  // void
    }
    return 0;
}
