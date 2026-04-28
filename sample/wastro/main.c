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
#include <setjmp.h>
#include <stdarg.h>
#include "context.h"
#include "node.h"
#include "astro_code_store.h"

struct wastro_option OPTION;

// Trap recovery for the spec-test harness.  When `wastro_trap_active`
// is set, traps longjmp to the saved buffer and stash the message;
// otherwise they print and exit() as in the standalone driver.
static jmp_buf  wastro_trap_jmp;
static int      wastro_trap_active = 0;
static char     wastro_trap_message[256];

// Parse-error recovery for the harness.  When `wastro_parse_active`
// is set, parse errors longjmp instead of exiting, letting the
// harness skip unsupported modules and continue.
static jmp_buf  wastro_parse_jmp;
static int      wastro_parse_active = 0;
static char     wastro_parse_message[256];

// Module-level errors (binary decoder, etc.) — print and either
// longjmp via the harness's parse_jmp, or exit() in the standalone
// driver.  Use this in places where parse_error's "near 'tok'"
// message is misleading because we're not lexing text.
__attribute__((noreturn))
static void
wastro_die(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "wastro: %s\n", buf);
    if (wastro_parse_active) {
        snprintf(wastro_parse_message, sizeof(wastro_parse_message), "%s", buf);
        longjmp(wastro_parse_jmp, 1);
    }
    exit(1);
}

void
wastro_trap(const char *msg)
{
    if (wastro_trap_active) {
        snprintf(wastro_trap_message, sizeof(wastro_trap_message), "%s", msg);
        longjmp(wastro_trap_jmp, 1);
    }
    fprintf(stderr, "wastro: trap: %s\n", msg);
    exit(1);
}

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

// Per WAT spec, idchars are alnum plus the punctuation set:
//   ! # $ % & ' * + - . / : < = > ? @ \ ^ _ ` | ~
// We use this for keyword tokens, which is what `offset=N`, `align=N`,
// and `nan:canonical` rely on.
static int
is_keyword_char(int ch)
{
    if (isalnum(ch)) return 1;
    switch (ch) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '/': case ':':
    case '<': case '=': case '>': case '?': case '@': case '\\':
    case '^': case '_': case '`': case '|': case '~':
        return 1;
    }
    return 0;
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

    if (ch == '-' || ch == '+' || isdigit((unsigned char)ch) ||
        (ch == 'n' && src_pos + 2 < src_end && src_pos[1] == 'a' && src_pos[2] == 'n') ||
        (ch == 'i' && src_pos + 2 < src_end && src_pos[1] == 'n' && src_pos[2] == 'f')) {
        const char *start = src_pos;
        const char *p = src_pos;
        int neg = 0;
        if (*p == '+') { p++; }
        else if (*p == '-') { p++; neg = 1; }
        // Determine numeric span: digits / hex / dot / e / p / sign-after-e.
        int is_float = 0;
        int is_hex = 0;
        // Recognise nan / inf shorthand.
        int is_nan_or_inf = 0;
        if (p + 2 < src_end && p[0] == 'n' && p[1] == 'a' && p[2] == 'n') {
            is_nan_or_inf = 1; is_float = 1;
        }
        else if (p + 2 < src_end && p[0] == 'i' && p[1] == 'n' && p[2] == 'f') {
            is_nan_or_inf = 1; is_float = 1;
        }
        if (!is_nan_or_inf) {
            if (p + 1 < src_end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                is_hex = 1;
                p += 2;
            }
        }
        const char *num_start = p;
        if (is_nan_or_inf) {
            // skip nan / inf and optional `:0xNN` payload (`nan:canonical`,
            // `nan:arithmetic`, `nan:0x...`).
            p += 3;
            if (p < src_end && *p == ':') {
                p++;
                while (p < src_end && (isalnum((unsigned char)*p) || *p == '_')) p++;
            }
        }
        else {
            while (p < src_end && (isalnum((unsigned char)*p) || *p == '.' || *p == '_')) {
                if (*p == '.' || (!is_hex && (*p == 'e' || *p == 'E')) ||
                    (is_hex && (*p == 'p' || *p == 'P'))) is_float = 1;
                if (((*p == 'e' || *p == 'E') && !is_hex) ||
                    ((*p == 'p' || *p == 'P') && is_hex)) {
                    p++;
                    if (p < src_end && (*p == '+' || *p == '-')) p++;
                    continue;
                }
                p++;
            }
        }
        // Build a copy with underscores stripped for strtoull / strtod.
        char numbuf[256];
        size_t bi = 0;
        if (neg && bi < sizeof(numbuf)) numbuf[bi++] = '-';
        if (is_hex) {
            if (bi + 2 < sizeof(numbuf)) { numbuf[bi++] = '0'; numbuf[bi++] = 'x'; }
        }
        for (const char *q = num_start; q < p && bi + 1 < sizeof(numbuf); q++) {
            if (*q == '_') continue;
            numbuf[bi++] = *q;
        }
        numbuf[bi] = 0;

        errno = 0;
        if (is_float) {
            double dv = strtod(numbuf, NULL);
            if (errno != 0 && !is_nan_or_inf) {
                // tolerate inexact
                errno = 0;
            }
            src_pos = p;
            cur_tok.kind = T_INT;
            cur_tok.start = start;
            cur_tok.len = (size_t)(src_pos - start);
            cur_tok.float_value = dv;
            cur_tok.int_value = (int64_t)dv;
            cur_tok.has_dot = 1;
            return;
        }
        else {
            // Integer: parse as unsigned, allow sign separately.  Wasm
            // integer literals can be either signed or unsigned (e.g.
            // `i32.const 0xFFFFFFFF` == -1).  We keep raw bits.
            errno = 0;
            const char *parsep = numbuf;
            int neg2 = 0;
            if (*parsep == '-') { neg2 = 1; parsep++; }
            // Wasm integer literals: leading 0 does NOT denote octal
            // (per wasm spec).  Use base 16 for `0x` / `0X` prefix,
            // otherwise base 10.
            int base = 10;
            if (parsep[0] == '0' && (parsep[1] == 'x' || parsep[1] == 'X')) base = 16;
            unsigned long long uv = strtoull(parsep, NULL, base);
            uint64_t v = neg2 ? -(uint64_t)uv : (uint64_t)uv;
            src_pos = p;
            cur_tok.kind = T_INT;
            cur_tok.start = start;
            cur_tok.len = (size_t)(src_pos - start);
            cur_tok.int_value = (int64_t)v;
            cur_tok.has_dot = 0;
            return;
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

__attribute__((noreturn))
static void
parse_error(const char *msg)
{
    if (wastro_parse_active) {
        snprintf(wastro_parse_message, sizeof(wastro_parse_message),
                 "%s (near '%.*s')", msg, (int)cur_tok.len, cur_tok.start);
        longjmp(wastro_parse_jmp, 1);
    }
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

// Currently-being-parsed function's WASTRO_FUNCS index.  Set on entry
// to parse_func_body / binary code-section parsing and read by the
// local-op allocators so they stamp `frame_id` into each node — the
// AOT specializer later uses frame_id to look up local types and emit
// the right `((struct wastro_frame_<id> *)frame)->Lx` cast.  Threaded
// via a module-level static instead of through every parser function.
static int CUR_FUNC_IDX = -1;

// ----- Phase 3: per-function typed-frame struct emission helpers -----
//
// Called from generated SPECIALIZE_* code (wastro_gen.rb) when emitting
// SD code that needs to reference `struct wastro_frame_<fid>`.
//
// The struct has one typed field per wasm local at its natural C type
// (int32_t / int64_t / float / double).  This is what lets gcc SROA
// the slots into registers in the inner loop — see docs/todo.md for
// why a uint64_t array couldn't.

const char *
wastro_local_ctype(uint32_t fid, uint32_t idx)
{
    switch (WASTRO_FUNCS[fid].local_types[idx]) {
    case WT_I32: return "int32_t";
    case WT_I64: return "int64_t";
    case WT_F32: return "float";
    case WT_F64: return "double";
    default:     return "uint64_t";
    }
}

const char *
wastro_local_as_macro(uint32_t fid, uint32_t idx)
{
    switch (WASTRO_FUNCS[fid].local_types[idx]) {
    case WT_I32: return "AS_I32";
    case WT_I64: return "AS_I64";
    case WT_F32: return "AS_F32";
    case WT_F64: return "AS_F64";
    default:     return "";  // identity (uint64_t)
    }
}

const char *
wastro_local_from_macro(uint32_t fid, uint32_t idx)
{
    switch (WASTRO_FUNCS[fid].local_types[idx]) {
    case WT_I32: return "FROM_I32";
    case WT_I64: return "FROM_I64";
    case WT_F32: return "FROM_F32";
    case WT_F64: return "FROM_F64";
    default:     return "";
    }
}

// Emit `struct wastro_frame_<fid> { ... };` to fp, guarded with #ifndef
// so multiple SDs in the same .c file share one definition.
void
wastro_emit_frame_struct(FILE *fp, uint32_t fid)
{
    fprintf(fp, "#ifndef WASTRO_FRAME_%u_DEFINED\n", fid);
    fprintf(fp, "#define WASTRO_FRAME_%u_DEFINED 1\n", fid);
    fprintf(fp, "struct wastro_frame_%u {\n", fid);
    struct wastro_function *fn = &WASTRO_FUNCS[fid];
    for (uint32_t i = 0; i < fn->local_cnt; i++) {
        fprintf(fp, "    %s L%u;\n", wastro_local_ctype(fid, i), i);
    }
    fprintf(fp, "};\n");
    fprintf(fp, "#endif\n\n");
}

// Pending body-slot fix-up for node_call_N nodes.  At allocation
// time the callee's body may not be parsed yet (forward reference);
// every call site is appended here and patched in one post-parse
// sweep so that the specializer can recurse from caller into callee
// via the body slot.
struct pending_call_body {
    NODE *call_node;
    uint32_t func_index;
    uint8_t arity;
};
#define MAX_PENDING_CALL_BODY 65536
static struct pending_call_body PENDING_CALL_BODY[MAX_PENDING_CALL_BODY];
static uint32_t PENDING_CALL_BODY_CNT = 0;

static inline void
register_call_body_fixup(NODE *call_node, uint32_t func_index, uint8_t arity)
{
    if (PENDING_CALL_BODY_CNT >= MAX_PENDING_CALL_BODY) {
        fprintf(stderr, "wastro: too many call sites (>%u)\n", MAX_PENDING_CALL_BODY);
        exit(1);
    }
    PENDING_CALL_BODY[PENDING_CALL_BODY_CNT++] = (struct pending_call_body){
        call_node, func_index, arity
    };
}

static void
wastro_fixup_call_bodies(void)
{
    for (uint32_t i = 0; i < PENDING_CALL_BODY_CNT; i++) {
        struct pending_call_body *p = &PENDING_CALL_BODY[i];
        NODE *body = WASTRO_FUNCS[p->func_index].body;
        switch (p->arity) {
        case 0: p->call_node->u.node_call_0.body = body; break;
        case 1: p->call_node->u.node_call_1.body = body; break;
        case 2: p->call_node->u.node_call_2.body = body; break;
        case 3: p->call_node->u.node_call_3.body = body; break;
        case 4: p->call_node->u.node_call_4.body = body; break;
        }
    }
    PENDING_CALL_BODY_CNT = 0;
}

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

// Type signatures from `(type $sig (func ...))`.  Indexed by the wasm
// type-index space.  Used by call_indirect for runtime type checks.
#define WASTRO_MAX_TYPES 64
struct wastro_type_sig WASTRO_TYPES[WASTRO_MAX_TYPES];
uint32_t WASTRO_TYPE_CNT = 0;
static char *WASTRO_TYPE_NAMES[WASTRO_MAX_TYPES];

// Function table for call_indirect (single funcref table per wasm 1.0).
// Each slot is a function index into WASTRO_FUNCS, or -1 if uninitialized.
int32_t *WASTRO_TABLE = NULL;
uint32_t WASTRO_TABLE_SIZE = 0;        // current size
static uint32_t WASTRO_TABLE_MAX = 0;  // max growth limit (informational)
static int      MOD_HAS_TABLE = 0;

// Deferred elem segments — function refs are resolved after all
// (func) and (import func) forms have been registered, so that elem
// can appear anywhere in the module form.
struct elem_pending {
    uint32_t offset;
    uint32_t cnt;
    Token *refs;          // copy of each func-ref token (T_IDENT or T_INT)
};
#define WASTRO_MAX_ELEM_SEGS 64
static struct elem_pending PENDING_ELEMS[WASTRO_MAX_ELEM_SEGS];
static uint32_t PENDING_ELEM_CNT = 0;

// Deferred (export "name" (func $f|N)) — resolved post-scan since
// the export may name a function declared later in the source.
struct export_pending {
    char *name;
    Token ref;            // function ref token
};
#define WASTRO_MAX_EXPORTS 1024
static struct export_pending PENDING_EXPORTS[WASTRO_MAX_EXPORTS];
static uint32_t PENDING_EXPORT_CNT = 0;

// (start $f) — function called at module instantiation.  -1 if none.
static Token MOD_START_TOK;
static int   MOD_HAS_START = 0;
int          MOD_START_FUNC = -1;     // resolved after scan_module

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
// Stub for imports declared in the WAT but with no host binding.
// Traps if the wasm code actually invokes the import.  Allows modules
// that *declare* but never *call* unbound imports to load (useful for
// the spec testsuite which has lots of placeholder imports).
VALUE host_unbound_trap(CTX *c, VALUE *args, uint32_t argc) {
    (void)c; (void)args; (void)argc;
    wastro_trap("call to unbound host import");
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
// Spec-testsuite "spectest" module: empty no-ops we register so that
// the standard wasm testsuite imports load without traps when invoked.
static VALUE host_spectest_noop(CTX *c, VALUE *args, uint32_t argc) {
    (void)c; (void)args; (void)argc; return 0;
}

static const struct host_entry HOST_REGISTRY[] = {
    { "env", "log_i32",     host_log_i32,     { WT_I32 },        1, WT_VOID },
    { "env", "log_i64",     host_log_i64,     { WT_I64 },        1, WT_VOID },
    { "env", "log_f32",     host_log_f32,     { WT_F32 },        1, WT_VOID },
    { "env", "log_f64",     host_log_f64,     { WT_F64 },        1, WT_VOID },
    { "env", "putchar",     host_putchar,     { WT_I32 },        1, WT_VOID },
    { "env", "print_bytes", host_print_bytes, { WT_I32, WT_I32 },2, WT_VOID },
    // spectest.* — referenced by the wasm spec-test bench.  Stubs.
    { "spectest", "print",         host_spectest_noop, { 0 }, 0, WT_VOID },
    { "spectest", "print_i32",     host_spectest_noop, { WT_I32 }, 1, WT_VOID },
    { "spectest", "print_i64",     host_spectest_noop, { WT_I64 }, 1, WT_VOID },
    { "spectest", "print_f32",     host_spectest_noop, { WT_F32 }, 1, WT_VOID },
    { "spectest", "print_f64",     host_spectest_noop, { WT_F64 }, 1, WT_VOID },
    { "spectest", "print_i32_f32", host_spectest_noop, { WT_I32, WT_F32 }, 2, WT_VOID },
    { "spectest", "print_f64_f64", host_spectest_noop, { WT_F64, WT_F64 }, 2, WT_VOID },
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

// Decode a numeric token text exactly into f32/f64 bits.  Handles:
//   nan / +nan / -nan         — canonical NaN
//   nan:0xPAYLOAD             — NaN with the given mantissa bits
//   nan:canonical             — canonical NaN (treated like `nan`)
//   nan:arithmetic            — arithmetic NaN (treated like `nan`)
//   inf / +inf / -inf         — infinities
//   any other numeric form    — strtod via the pre-parsed float_value
static uint32_t
token_to_f32_bits(const Token *t, double fallback_dv)
{
    int neg = 0;
    const char *p = t->start;
    const char *end = t->start + t->len;
    if (p < end && (*p == '+' || *p == '-')) { if (*p == '-') neg = 1; p++; }
    if (end - p >= 3 && memcmp(p, "nan", 3) == 0) {
        (void)fallback_dv;
        uint32_t bits = 0x7F800000u;          // exponent all 1s
        if (neg) bits |= 0x80000000u;
        p += 3;
        if (p < end && *p == ':') {
            p++;
            if (end - p >= 9 && memcmp(p, "canonical", 9) == 0) {
                bits |= 0x00400000u;          // quiet NaN bit
            }
            else if (end - p >= 10 && memcmp(p, "arithmetic", 10) == 0) {
                bits |= 0x00400000u;
            }
            else {
                // hex payload, possibly with 0x prefix and underscores
                if (end - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
                uint32_t payload = 0;
                while (p < end) {
                    char c = *p++;
                    if (c == '_') continue;
                    if (c >= '0' && c <= '9') payload = payload * 16 + (uint32_t)(c - '0');
                    else if (c >= 'a' && c <= 'f') payload = payload * 16 + (uint32_t)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') payload = payload * 16 + (uint32_t)(c - 'A' + 10);
                    else break;
                }
                bits |= (payload & 0x7FFFFFu);
            }
        }
        else {
            bits |= 0x00400000u;              // bare nan = canonical (quiet)
        }
        return bits;
    }
    if (end - p >= 3 && memcmp(p, "inf", 3) == 0) {
        return neg ? 0xFF800000u : 0x7F800000u;
    }
    // Generic numeric — re-parse with strtof directly to f32 to avoid
    // double-rounding artefacts that bite the spec testsuite at
    // the float / max-finite boundary.
    char buf[256];
    size_t bi = 0;
    for (const char *q = t->start; q < end && bi + 1 < sizeof(buf); q++) {
        if (*q != '_') buf[bi++] = *q;
    }
    buf[bi] = 0;
    float fv = strtof(buf, NULL);
    (void)fallback_dv;
    uint32_t b; memcpy(&b, &fv, 4);
    return b;
}

static uint64_t
token_to_f64_bits(const Token *t, double fallback_dv)
{
    int neg = 0;
    const char *p = t->start;
    const char *end = t->start + t->len;
    if (p < end && (*p == '+' || *p == '-')) { if (*p == '-') neg = 1; p++; }
    if (end - p >= 3 && memcmp(p, "nan", 3) == 0) {
        uint64_t bits = 0x7FF0000000000000ull;
        if (neg) bits |= 0x8000000000000000ull;
        p += 3;
        if (p < end && *p == ':') {
            p++;
            if (end - p >= 9 && memcmp(p, "canonical", 9) == 0) {
                bits |= 0x0008000000000000ull;
            }
            else if (end - p >= 10 && memcmp(p, "arithmetic", 10) == 0) {
                bits |= 0x0008000000000000ull;
            }
            else {
                if (end - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
                uint64_t payload = 0;
                while (p < end) {
                    char c = *p++;
                    if (c == '_') continue;
                    if (c >= '0' && c <= '9') payload = payload * 16 + (uint64_t)(c - '0');
                    else if (c >= 'a' && c <= 'f') payload = payload * 16 + (uint64_t)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') payload = payload * 16 + (uint64_t)(c - 'A' + 10);
                    else break;
                }
                bits |= (payload & 0xFFFFFFFFFFFFFull);
            }
        }
        else {
            bits |= 0x0008000000000000ull;
        }
        return bits;
    }
    if (end - p >= 3 && memcmp(p, "inf", 3) == 0) {
        return neg ? 0xFFF0000000000000ull : 0x7FF0000000000000ull;
    }
    // Generic numeric — re-parse with strtod for max precision.
    char buf[256];
    size_t bi = 0;
    for (const char *q = t->start; q < end && bi + 1 < sizeof(buf); q++) {
        if (*q != '_') buf[bi++] = *q;
    }
    buf[bi] = 0;
    double dv = strtod(buf, NULL);
    (void)fallback_dv;
    uint64_t b; memcpy(&b, &dv, 8);
    return b;
}

static uint8_t *
decode_wasm_str(const Token *t, uint32_t *out_len)
{
    // Per wasm spec, string escapes are:
    //   \n \t \r \" \' \\ \u{HEX...}  +  \HH  (two hex digits → one byte)
    // Note that there is no `\0` shorthand: the null byte is `\00`.
    uint8_t *buf = malloc(t->len + 1);
    uint32_t bi = 0;
    for (size_t i = 0; i < t->len; i++) {
        unsigned char ch = (unsigned char)t->start[i];
        if (ch == '\\' && i + 1 < t->len) {
            unsigned char e = (unsigned char)t->start[i + 1];
            if (e == 'n')       { buf[bi++] = '\n'; i++; }
            else if (e == 't')  { buf[bi++] = '\t'; i++; }
            else if (e == 'r')  { buf[bi++] = '\r'; i++; }
            else if (e == '\\') { buf[bi++] = '\\'; i++; }
            else if (e == '\'') { buf[bi++] = '\''; i++; }
            else if (e == '"')  { buf[bi++] = '"';  i++; }
            else if (e == 'u' && i + 2 < t->len && t->start[i + 2] == '{') {
                // \u{XXXX} — UTF-8 encode the codepoint.
                size_t j = i + 3;
                uint32_t cp = 0;
                while (j < t->len && t->start[j] != '}') {
                    char cc = t->start[j];
                    if (!isxdigit((unsigned char)cc)) break;
                    cp = cp * 16 + (uint32_t)((cc <= '9') ? (cc - '0') :
                                               (cc <= 'F') ? (cc - 'A' + 10) :
                                                             (cc - 'a' + 10));
                    j++;
                }
                if (j < t->len && t->start[j] == '}') {
                    if (cp < 0x80) buf[bi++] = (uint8_t)cp;
                    else if (cp < 0x800) {
                        buf[bi++] = 0xC0 | (cp >> 6);
                        buf[bi++] = 0x80 | (cp & 0x3F);
                    }
                    else if (cp < 0x10000) {
                        buf[bi++] = 0xE0 | (cp >> 12);
                        buf[bi++] = 0x80 | ((cp >> 6) & 0x3F);
                        buf[bi++] = 0x80 | (cp & 0x3F);
                    }
                    else {
                        buf[bi++] = 0xF0 | (cp >> 18);
                        buf[bi++] = 0x80 | ((cp >> 12) & 0x3F);
                        buf[bi++] = 0x80 | ((cp >> 6) & 0x3F);
                        buf[bi++] = 0x80 | (cp & 0x3F);
                    }
                    i = j;
                }
                else buf[bi++] = ch;
            }
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
        return (TypedExpr){ALLOC_node_local_get((uint32_t)CUR_FUNC_IDX, (uint32_t)idx), env->types[idx]};
    }
    if (tok_is_keyword("local.set")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        TypedExpr e = parse_expr(env, labels);
        expect_type(e.type, env->types[idx], "local.set value");
        expect_rparen();
        return (TypedExpr){ALLOC_node_local_set((uint32_t)CUR_FUNC_IDX, (uint32_t)idx, e.node), WT_VOID};
    }
    if (tok_is_keyword("local.tee")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        TypedExpr e = parse_expr(env, labels);
        expect_type(e.type, env->types[idx], "local.tee value");
        expect_rparen();
        return (TypedExpr){ALLOC_node_local_tee((uint32_t)CUR_FUNC_IDX, (uint32_t)idx, e.node), env->types[idx]};
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
        op_push(S, ALLOC_node_local_get((uint32_t)CUR_FUNC_IDX, (uint32_t)idx), env->types[idx]);
        return;
    }
    if (tok_is_keyword("local.set")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        TypedExpr e = op_pop(S, env->types[idx], "local.set value");
        stmts_append(L, ALLOC_node_local_set((uint32_t)CUR_FUNC_IDX, (uint32_t)idx, e.node));
        return;
    }
    if (tok_is_keyword("local.tee")) {
        next_token();
        int idx = local_env_lookup(env, &cur_tok);
        next_token();
        TypedExpr e = op_pop(S, env->types[idx], "local.tee value");
        op_push(S, ALLOC_node_local_tee((uint32_t)CUR_FUNC_IDX, (uint32_t)idx, e.node), env->types[idx]);
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
    WASTRO_FUNCS[idx].entry = ALLOC_node_function_frame((uint32_t)idx, body.node);
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

// =====================================================================
// Binary .wasm decoder
// =====================================================================
//
// Decodes a wasm 1.0 binary module into the same in-memory state
// (WASTRO_TYPES, WASTRO_FUNCS, WASTRO_GLOBALS, WASTRO_TABLE,
// MOD_DATA_SEGS, ...) that the WAT parser populates.  Function bodies
// (Code section) are converted to AST via the same OpStack/StmtList
// machinery used by the stack-style WAT parser.

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} BinReader;

static void bin_check(BinReader *r, size_t need, const char *what) {
    if ((size_t)(r->end - r->p) < need) {
        wastro_die("binary: short read at %s", what);
    }
}
static uint8_t bin_u8(BinReader *r) {
    bin_check(r, 1, "u8");
    return *r->p++;
}
static uint32_t bin_u32(BinReader *r) {
    bin_check(r, 4, "u32");
    uint32_t v; memcpy(&v, r->p, 4); r->p += 4;
    return v;
}
static uint64_t bin_u64(BinReader *r) {
    bin_check(r, 8, "u64");
    uint64_t v; memcpy(&v, r->p, 8); r->p += 8;
    return v;
}
static uint64_t bin_leb_u(BinReader *r, int max_bits) {
    uint64_t v = 0; int shift = 0;
    while (1) {
        bin_check(r, 1, "leb_u");
        uint8_t b = *r->p++;
        v |= ((uint64_t)(b & 0x7F)) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
        if (shift >= max_bits + 7) {
            wastro_die("binary: LEB128 overflow");
        }
    }
    return v;
}
static int64_t bin_leb_s(BinReader *r, int max_bits) {
    int64_t v = 0; int shift = 0;
    uint8_t b;
    do {
        bin_check(r, 1, "leb_s");
        b = *r->p++;
        v |= ((int64_t)(b & 0x7F)) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= -(int64_t)1 << shift;   // sign-extend
    (void)max_bits;
    return v;
}
static uint32_t bin_leb_u32(BinReader *r) { return (uint32_t)bin_leb_u(r, 32); }
static uint64_t bin_leb_u64(BinReader *r) { return bin_leb_u(r, 64); }
static int32_t  bin_leb_s32(BinReader *r) { return (int32_t)bin_leb_s(r, 32); }
static int64_t  bin_leb_s64(BinReader *r) { return bin_leb_s(r, 64); }

static wtype_t bin_valtype(uint8_t b) {
    switch (b) {
    case 0x7F: return WT_I32;
    case 0x7E: return WT_I64;
    case 0x7D: return WT_F32;
    case 0x7C: return WT_F64;
    }
    wastro_die("binary: unknown valtype 0x%02x", b);
}

// Bin-side function-body parser — opcode stream → AST.  Mirrors
// parse_bare_instr but reads from BinReader.  Calls itself
// recursively for block/loop/if bodies.
static TypedExpr parse_bin_code_seq(BinReader *r, LocalEnv *env, LabelEnv *labels, int allow_else, int *got_else);

static wtype_t bin_blocktype(BinReader *r) {
    bin_check(r, 1, "blocktype");
    uint8_t b = *r->p;
    if (b == 0x40) { r->p++; return WT_VOID; }
    if (b == 0x7F || b == 0x7E || b == 0x7D || b == 0x7C) {
        r->p++;
        return bin_valtype(b);
    }
    // s33 typeidx (multi-value) — read & discard.  Body type is
    // taken from the block's tail value at runtime.
    (void)bin_leb_s64(r);
    return WT_VOID;
}

static void parse_memarg(BinReader *r, uint32_t *out_offset) {
    (void)bin_leb_u32(r);   // align — informational
    *out_offset = bin_leb_u32(r);
}

#define BBIN(OPND_T, RES_T, ALLOC) do { \
        TypedExpr rr = op_pop(&S, OPND_T, "bin op right"); \
        TypedExpr ll = op_pop(&S, OPND_T, "bin op left"); \
        op_push(&S, ALLOC(ll.node, rr.node), RES_T); \
    } while (0)
#define BUN(OPND_T, RES_T, ALLOC) do { \
        TypedExpr ee = op_pop(&S, OPND_T, "un op"); \
        op_push(&S, ALLOC(ee.node), RES_T); \
    } while (0)
#define BLOAD(RES_T, ALLOC) do { \
        uint32_t off; parse_memarg(r, &off); \
        TypedExpr addr = op_pop(&S, WT_I32, "load addr"); \
        op_push(&S, ALLOC(off, addr.node), RES_T); \
    } while (0)
#define BSTORE(VAL_T, ALLOC) do { \
        uint32_t off; parse_memarg(r, &off); \
        TypedExpr value = op_pop(&S, VAL_T, "store value"); \
        TypedExpr addr  = op_pop(&S, WT_I32, "store addr"); \
        stmts_append(&L, ALLOC(off, addr.node, value.node)); \
    } while (0)

static TypedExpr
parse_bin_code_seq(BinReader *r, LocalEnv *env, LabelEnv *labels, int allow_else, int *got_else)
{
    OpStack S = {0};
    StmtList L = {0};
    if (got_else) *got_else = 0;
    while (1) {
        bin_check(r, 1, "code");
        uint8_t op = *r->p++;
        if (op == 0x0B) break;   // end
        if (op == 0x05) {
            if (!allow_else) wastro_die("binary: unexpected 0x05 (else)");
            if (got_else) *got_else = 1;
            break;
        }
        switch (op) {
        case 0x00: stmts_append(&L, ALLOC_node_unreachable()); break;
        case 0x01: stmts_append(&L, ALLOC_node_nop()); break;

        case 0x02: case 0x03: {  // block / loop
            int is_loop = (op == 0x03);
            wtype_t bt = bin_blocktype(r);
            if (labels->cnt >= 32) parse_error("too many nested labels");
            labels->names[labels->cnt] = NULL;
            labels->result_types[labels->cnt] = bt;
            labels->is_loop[labels->cnt] = is_loop;
            labels->cnt++;
            TypedExpr body = parse_bin_code_seq(r, env, labels, 0, NULL);
            labels->cnt--;
            NODE *node = is_loop ? ALLOC_node_loop(body.node) : ALLOC_node_block(body.node);
            if (bt != WT_VOID) op_push(&S, node, bt); else stmts_append(&L, node);
        } break;

        case 0x04: {  // if
            wtype_t bt = bin_blocktype(r);
            TypedExpr cond = op_pop(&S, WT_I32, "if cond");
            if (labels->cnt >= 32) parse_error("too many nested labels");
            labels->names[labels->cnt] = NULL;
            labels->result_types[labels->cnt] = bt;
            labels->is_loop[labels->cnt] = 0;
            labels->cnt++;
            int saw_else = 0;
            TypedExpr th = parse_bin_code_seq(r, env, labels, 1, &saw_else);
            TypedExpr el = {ALLOC_node_nop(), WT_VOID};
            if (saw_else) el = parse_bin_code_seq(r, env, labels, 0, NULL);
            labels->cnt--;
            NODE *node = ALLOC_node_if(cond.node, th.node, el.node);
            if (bt != WT_VOID) op_push(&S, node, bt); else stmts_append(&L, node);
        } break;

        case 0x0C: {  // br l
            uint32_t depth = bin_leb_u32(r);
            wtype_t want = labels->result_types[labels->cnt - 1 - depth];
            if (!labels->is_loop[labels->cnt - 1 - depth] && want != WT_VOID && S.cnt > 0) {
                TypedExpr v = op_pop(&S, want, "br value");
                stmts_append(&L, ALLOC_node_br_v(depth, v.node));
            } else {
                stmts_append(&L, ALLOC_node_br(depth));
            }
        } break;
        case 0x0D: {  // br_if l
            uint32_t depth = bin_leb_u32(r);
            TypedExpr cond = op_pop(&S, WT_I32, "br_if cond");
            wtype_t want = labels->result_types[labels->cnt - 1 - depth];
            if (!labels->is_loop[labels->cnt - 1 - depth] && want != WT_VOID && S.cnt > 0 && S.items[S.cnt - 1].type == want) {
                TypedExpr v = op_pop(&S, want, "br_if value");
                stmts_append(&L, ALLOC_node_br_if_v(depth, cond.node, v.node));
            } else {
                stmts_append(&L, ALLOC_node_br_if(depth, cond.node));
            }
        } break;
        case 0x0E: {  // br_table
            uint32_t cnt = bin_leb_u32(r);
            uint32_t depths[256];
            if (cnt > 256) parse_error("br_table too large");
            for (uint32_t i = 0; i < cnt; i++) depths[i] = bin_leb_u32(r);
            uint32_t default_depth = bin_leb_u32(r);
            if (WASTRO_BR_TABLE_CNT + cnt > WASTRO_BR_TABLE_CAP) {
                WASTRO_BR_TABLE_CAP = WASTRO_BR_TABLE_CAP ? WASTRO_BR_TABLE_CAP * 2 : 64;
                while (WASTRO_BR_TABLE_CAP < WASTRO_BR_TABLE_CNT + cnt) WASTRO_BR_TABLE_CAP *= 2;
                WASTRO_BR_TABLE = realloc(WASTRO_BR_TABLE, sizeof(uint32_t) * WASTRO_BR_TABLE_CAP);
            }
            uint32_t target_index = WASTRO_BR_TABLE_CNT;
            for (uint32_t i = 0; i < cnt; i++) WASTRO_BR_TABLE[target_index + i] = depths[i];
            WASTRO_BR_TABLE_CNT += cnt;
            TypedExpr idx = op_pop(&S, WT_I32, "br_table idx");
            wtype_t want = labels->result_types[labels->cnt - 1 - default_depth];
            if (!labels->is_loop[labels->cnt - 1 - default_depth] && want != WT_VOID && S.cnt > 0 && S.items[S.cnt - 1].type == want) {
                TypedExpr v = op_pop(&S, want, "br_table value");
                stmts_append(&L, ALLOC_node_br_table_v(target_index, cnt, default_depth, idx.node, v.node));
            } else {
                stmts_append(&L, ALLOC_node_br_table(target_index, cnt, default_depth, idx.node));
            }
        } break;
        case 0x0F: {  // return
            if (S.cnt > 0) {
                TypedExpr v = S.items[--S.cnt];
                stmts_append(&L, ALLOC_node_return_v(v.node));
            } else stmts_append(&L, ALLOC_node_return());
        } break;
        case 0x10: {  // call
            uint32_t fi = bin_leb_u32(r);
            struct wastro_function *callee = &WASTRO_FUNCS[fi];
            if (callee->param_cnt > 8) parse_error("call arity > 8 not supported");
            NODE *args[8];
            for (int i = (int)callee->param_cnt - 1; i >= 0; i--) {
                TypedExpr a = op_pop(&S, callee->param_types[i], "call arg");
                args[i] = a.node;
            }
            NODE *cn;
            if (callee->is_import) {
                switch (callee->param_cnt) {
                case 0: cn = ALLOC_node_host_call_0(fi); break;
                case 1: cn = ALLOC_node_host_call_1(fi, args[0]); break;
                case 2: cn = ALLOC_node_host_call_2(fi, args[0], args[1]); break;
                case 3: cn = ALLOC_node_host_call_3(fi, args[0], args[1], args[2]); break;
                default: parse_error("host call arity > 3 not supported"); cn = NULL;
                }
            } else {
                uint32_t lc = callee->local_cnt;
                NODE *body = callee->body;  // may be NULL if forward ref
                switch (callee->param_cnt) {
                case 0: cn = ALLOC_node_call_0(fi, lc, body); break;
                case 1: cn = ALLOC_node_call_1(fi, lc, args[0], body); break;
                case 2: cn = ALLOC_node_call_2(fi, lc, args[0], args[1], body); break;
                case 3: cn = ALLOC_node_call_3(fi, lc, args[0], args[1], args[2], body); break;
                case 4: cn = ALLOC_node_call_4(fi, lc, args[0], args[1], args[2], args[3], body); break;
                default: parse_error("call arity 5..8 not supported"); cn = NULL;
                }
                register_call_body_fixup(cn, fi, (uint8_t)callee->param_cnt);
            }
            if (callee->result_type == WT_VOID) stmts_append(&L, cn);
            else op_push(&S, cn, callee->result_type);
        } break;
        case 0x11: {  // call_indirect
            uint32_t ti = bin_leb_u32(r);
            uint8_t table = bin_u8(r);
            (void)table;
            struct wastro_type_sig *sig = &WASTRO_TYPES[ti];
            if (sig->param_cnt > 4) parse_error("call_indirect arity > 4 not supported");
            TypedExpr idx = op_pop(&S, WT_I32, "call_indirect idx");
            NODE *args[4];
            for (int i = (int)sig->param_cnt - 1; i >= 0; i--) {
                TypedExpr a = op_pop(&S, sig->param_types[i], "ci arg");
                args[i] = a.node;
            }
            NODE *cn;
            switch (sig->param_cnt) {
            case 0: cn = ALLOC_node_call_indirect_0(ti, idx.node); break;
            case 1: cn = ALLOC_node_call_indirect_1(ti, idx.node, args[0]); break;
            case 2: cn = ALLOC_node_call_indirect_2(ti, idx.node, args[0], args[1]); break;
            case 3: cn = ALLOC_node_call_indirect_3(ti, idx.node, args[0], args[1], args[2]); break;
            case 4: cn = ALLOC_node_call_indirect_4(ti, idx.node, args[0], args[1], args[2], args[3]); break;
            default: parse_error("ci arity > 4 not supported"); cn = NULL;
            }
            if (sig->result_type == WT_VOID) stmts_append(&L, cn);
            else op_push(&S, cn, sig->result_type);
        } break;

        case 0x1A: {  // drop
            TypedExpr e = op_pop(&S, WT_VOID, "drop");
            stmts_append(&L, ALLOC_node_drop(e.node));
        } break;
        case 0x1B: {  // select
            TypedExpr cond = op_pop(&S, WT_I32, "select cond");
            TypedExpr v2 = op_pop(&S, WT_VOID, "select v2");
            TypedExpr v1 = op_pop(&S, WT_VOID, "select v1");
            if (v1.type != v2.type) parse_error("select: type mismatch");
            op_push(&S, ALLOC_node_select(v1.node, v2.node, cond.node), v1.type);
        } break;

        case 0x20: {  // local.get
            uint32_t li = bin_leb_u32(r);
            op_push(&S, ALLOC_node_local_get((uint32_t)CUR_FUNC_IDX, li), env->types[li]);
        } break;
        case 0x21: {  // local.set
            uint32_t li = bin_leb_u32(r);
            TypedExpr e = op_pop(&S, env->types[li], "local.set");
            stmts_append(&L, ALLOC_node_local_set((uint32_t)CUR_FUNC_IDX, li, e.node));
        } break;
        case 0x22: {  // local.tee
            uint32_t li = bin_leb_u32(r);
            TypedExpr e = op_pop(&S, env->types[li], "local.tee");
            op_push(&S, ALLOC_node_local_tee((uint32_t)CUR_FUNC_IDX, li, e.node), env->types[li]);
        } break;
        case 0x23: {  // global.get
            uint32_t gi = bin_leb_u32(r);
            op_push(&S, ALLOC_node_global_get(gi), WASTRO_GLOBAL_TYPES[gi]);
        } break;
        case 0x24: {  // global.set
            uint32_t gi = bin_leb_u32(r);
            TypedExpr e = op_pop(&S, WASTRO_GLOBAL_TYPES[gi], "global.set");
            stmts_append(&L, ALLOC_node_global_set(gi, e.node));
        } break;

        case 0x28: BLOAD(WT_I32, ALLOC_node_i32_load); break;
        case 0x29: BLOAD(WT_I64, ALLOC_node_i64_load); break;
        case 0x2A: BLOAD(WT_F32, ALLOC_node_f32_load); break;
        case 0x2B: BLOAD(WT_F64, ALLOC_node_f64_load); break;
        case 0x2C: BLOAD(WT_I32, ALLOC_node_i32_load8_s); break;
        case 0x2D: BLOAD(WT_I32, ALLOC_node_i32_load8_u); break;
        case 0x2E: BLOAD(WT_I32, ALLOC_node_i32_load16_s); break;
        case 0x2F: BLOAD(WT_I32, ALLOC_node_i32_load16_u); break;
        case 0x30: BLOAD(WT_I64, ALLOC_node_i64_load8_s); break;
        case 0x31: BLOAD(WT_I64, ALLOC_node_i64_load8_u); break;
        case 0x32: BLOAD(WT_I64, ALLOC_node_i64_load16_s); break;
        case 0x33: BLOAD(WT_I64, ALLOC_node_i64_load16_u); break;
        case 0x34: BLOAD(WT_I64, ALLOC_node_i64_load32_s); break;
        case 0x35: BLOAD(WT_I64, ALLOC_node_i64_load32_u); break;
        case 0x36: BSTORE(WT_I32, ALLOC_node_i32_store); break;
        case 0x37: BSTORE(WT_I64, ALLOC_node_i64_store); break;
        case 0x38: BSTORE(WT_F32, ALLOC_node_f32_store); break;
        case 0x39: BSTORE(WT_F64, ALLOC_node_f64_store); break;
        case 0x3A: BSTORE(WT_I32, ALLOC_node_i32_store8); break;
        case 0x3B: BSTORE(WT_I32, ALLOC_node_i32_store16); break;
        case 0x3C: BSTORE(WT_I64, ALLOC_node_i64_store8); break;
        case 0x3D: BSTORE(WT_I64, ALLOC_node_i64_store16); break;
        case 0x3E: BSTORE(WT_I64, ALLOC_node_i64_store32); break;
        case 0x3F: { (void)bin_u8(r); op_push(&S, ALLOC_node_memory_size(), WT_I32); } break;
        case 0x40: { (void)bin_u8(r); TypedExpr d = op_pop(&S, WT_I32, "memory.grow"); op_push(&S, ALLOC_node_memory_grow(d.node), WT_I32); } break;

        case 0x41: { int32_t v = bin_leb_s32(r); op_push(&S, ALLOC_node_i32_const(v), WT_I32); } break;
        case 0x42: { int64_t v = bin_leb_s64(r); op_push(&S, ALLOC_node_i64_const((uint64_t)v), WT_I64); } break;
        case 0x43: { uint32_t b = bin_u32(r); op_push(&S, ALLOC_node_f32_const(b), WT_F32); } break;
        case 0x44: { uint64_t b = bin_u64(r); double dv; memcpy(&dv,&b,8); op_push(&S, ALLOC_node_f64_const(dv), WT_F64); } break;

        case 0x45: BUN (WT_I32, WT_I32, ALLOC_node_i32_eqz); break;
        case 0x46: BBIN(WT_I32, WT_I32, ALLOC_node_i32_eq); break;
        case 0x47: BBIN(WT_I32, WT_I32, ALLOC_node_i32_ne); break;
        case 0x48: BBIN(WT_I32, WT_I32, ALLOC_node_i32_lt_s); break;
        case 0x49: BBIN(WT_I32, WT_I32, ALLOC_node_i32_lt_u); break;
        case 0x4A: BBIN(WT_I32, WT_I32, ALLOC_node_i32_gt_s); break;
        case 0x4B: BBIN(WT_I32, WT_I32, ALLOC_node_i32_gt_u); break;
        case 0x4C: BBIN(WT_I32, WT_I32, ALLOC_node_i32_le_s); break;
        case 0x4D: BBIN(WT_I32, WT_I32, ALLOC_node_i32_le_u); break;
        case 0x4E: BBIN(WT_I32, WT_I32, ALLOC_node_i32_ge_s); break;
        case 0x4F: BBIN(WT_I32, WT_I32, ALLOC_node_i32_ge_u); break;
        case 0x50: BUN (WT_I64, WT_I32, ALLOC_node_i64_eqz); break;
        case 0x51: BBIN(WT_I64, WT_I32, ALLOC_node_i64_eq); break;
        case 0x52: BBIN(WT_I64, WT_I32, ALLOC_node_i64_ne); break;
        case 0x53: BBIN(WT_I64, WT_I32, ALLOC_node_i64_lt_s); break;
        case 0x54: BBIN(WT_I64, WT_I32, ALLOC_node_i64_lt_u); break;
        case 0x55: BBIN(WT_I64, WT_I32, ALLOC_node_i64_gt_s); break;
        case 0x56: BBIN(WT_I64, WT_I32, ALLOC_node_i64_gt_u); break;
        case 0x57: BBIN(WT_I64, WT_I32, ALLOC_node_i64_le_s); break;
        case 0x58: BBIN(WT_I64, WT_I32, ALLOC_node_i64_le_u); break;
        case 0x59: BBIN(WT_I64, WT_I32, ALLOC_node_i64_ge_s); break;
        case 0x5A: BBIN(WT_I64, WT_I32, ALLOC_node_i64_ge_u); break;
        case 0x5B: BBIN(WT_F32, WT_I32, ALLOC_node_f32_eq); break;
        case 0x5C: BBIN(WT_F32, WT_I32, ALLOC_node_f32_ne); break;
        case 0x5D: BBIN(WT_F32, WT_I32, ALLOC_node_f32_lt); break;
        case 0x5E: BBIN(WT_F32, WT_I32, ALLOC_node_f32_gt); break;
        case 0x5F: BBIN(WT_F32, WT_I32, ALLOC_node_f32_le); break;
        case 0x60: BBIN(WT_F32, WT_I32, ALLOC_node_f32_ge); break;
        case 0x61: BBIN(WT_F64, WT_I32, ALLOC_node_f64_eq); break;
        case 0x62: BBIN(WT_F64, WT_I32, ALLOC_node_f64_ne); break;
        case 0x63: BBIN(WT_F64, WT_I32, ALLOC_node_f64_lt); break;
        case 0x64: BBIN(WT_F64, WT_I32, ALLOC_node_f64_gt); break;
        case 0x65: BBIN(WT_F64, WT_I32, ALLOC_node_f64_le); break;
        case 0x66: BBIN(WT_F64, WT_I32, ALLOC_node_f64_ge); break;

        case 0x67: BUN (WT_I32, WT_I32, ALLOC_node_i32_clz); break;
        case 0x68: BUN (WT_I32, WT_I32, ALLOC_node_i32_ctz); break;
        case 0x69: BUN (WT_I32, WT_I32, ALLOC_node_i32_popcnt); break;
        case 0x6A: BBIN(WT_I32, WT_I32, ALLOC_node_i32_add); break;
        case 0x6B: BBIN(WT_I32, WT_I32, ALLOC_node_i32_sub); break;
        case 0x6C: BBIN(WT_I32, WT_I32, ALLOC_node_i32_mul); break;
        case 0x6D: BBIN(WT_I32, WT_I32, ALLOC_node_i32_div_s); break;
        case 0x6E: BBIN(WT_I32, WT_I32, ALLOC_node_i32_div_u); break;
        case 0x6F: BBIN(WT_I32, WT_I32, ALLOC_node_i32_rem_s); break;
        case 0x70: BBIN(WT_I32, WT_I32, ALLOC_node_i32_rem_u); break;
        case 0x71: BBIN(WT_I32, WT_I32, ALLOC_node_i32_and); break;
        case 0x72: BBIN(WT_I32, WT_I32, ALLOC_node_i32_or); break;
        case 0x73: BBIN(WT_I32, WT_I32, ALLOC_node_i32_xor); break;
        case 0x74: BBIN(WT_I32, WT_I32, ALLOC_node_i32_shl); break;
        case 0x75: BBIN(WT_I32, WT_I32, ALLOC_node_i32_shr_s); break;
        case 0x76: BBIN(WT_I32, WT_I32, ALLOC_node_i32_shr_u); break;
        case 0x77: BBIN(WT_I32, WT_I32, ALLOC_node_i32_rotl); break;
        case 0x78: BBIN(WT_I32, WT_I32, ALLOC_node_i32_rotr); break;
        case 0x79: BUN (WT_I64, WT_I64, ALLOC_node_i64_clz); break;
        case 0x7A: BUN (WT_I64, WT_I64, ALLOC_node_i64_ctz); break;
        case 0x7B: BUN (WT_I64, WT_I64, ALLOC_node_i64_popcnt); break;
        case 0x7C: BBIN(WT_I64, WT_I64, ALLOC_node_i64_add); break;
        case 0x7D: BBIN(WT_I64, WT_I64, ALLOC_node_i64_sub); break;
        case 0x7E: BBIN(WT_I64, WT_I64, ALLOC_node_i64_mul); break;
        case 0x7F: BBIN(WT_I64, WT_I64, ALLOC_node_i64_div_s); break;
        case 0x80: BBIN(WT_I64, WT_I64, ALLOC_node_i64_div_u); break;
        case 0x81: BBIN(WT_I64, WT_I64, ALLOC_node_i64_rem_s); break;
        case 0x82: BBIN(WT_I64, WT_I64, ALLOC_node_i64_rem_u); break;
        case 0x83: BBIN(WT_I64, WT_I64, ALLOC_node_i64_and); break;
        case 0x84: BBIN(WT_I64, WT_I64, ALLOC_node_i64_or); break;
        case 0x85: BBIN(WT_I64, WT_I64, ALLOC_node_i64_xor); break;
        case 0x86: BBIN(WT_I64, WT_I64, ALLOC_node_i64_shl); break;
        case 0x87: BBIN(WT_I64, WT_I64, ALLOC_node_i64_shr_s); break;
        case 0x88: BBIN(WT_I64, WT_I64, ALLOC_node_i64_shr_u); break;
        case 0x89: BBIN(WT_I64, WT_I64, ALLOC_node_i64_rotl); break;
        case 0x8A: BBIN(WT_I64, WT_I64, ALLOC_node_i64_rotr); break;
        case 0x8B: BUN (WT_F32, WT_F32, ALLOC_node_f32_abs); break;
        case 0x8C: BUN (WT_F32, WT_F32, ALLOC_node_f32_neg); break;
        case 0x8D: BUN (WT_F32, WT_F32, ALLOC_node_f32_ceil); break;
        case 0x8E: BUN (WT_F32, WT_F32, ALLOC_node_f32_floor); break;
        case 0x8F: BUN (WT_F32, WT_F32, ALLOC_node_f32_trunc); break;
        case 0x90: BUN (WT_F32, WT_F32, ALLOC_node_f32_nearest); break;
        case 0x91: BUN (WT_F32, WT_F32, ALLOC_node_f32_sqrt); break;
        case 0x92: BBIN(WT_F32, WT_F32, ALLOC_node_f32_add); break;
        case 0x93: BBIN(WT_F32, WT_F32, ALLOC_node_f32_sub); break;
        case 0x94: BBIN(WT_F32, WT_F32, ALLOC_node_f32_mul); break;
        case 0x95: BBIN(WT_F32, WT_F32, ALLOC_node_f32_div); break;
        case 0x96: BBIN(WT_F32, WT_F32, ALLOC_node_f32_min); break;
        case 0x97: BBIN(WT_F32, WT_F32, ALLOC_node_f32_max); break;
        case 0x98: BBIN(WT_F32, WT_F32, ALLOC_node_f32_copysign); break;
        case 0x99: BUN (WT_F64, WT_F64, ALLOC_node_f64_abs); break;
        case 0x9A: BUN (WT_F64, WT_F64, ALLOC_node_f64_neg); break;
        case 0x9B: BUN (WT_F64, WT_F64, ALLOC_node_f64_ceil); break;
        case 0x9C: BUN (WT_F64, WT_F64, ALLOC_node_f64_floor); break;
        case 0x9D: BUN (WT_F64, WT_F64, ALLOC_node_f64_trunc); break;
        case 0x9E: BUN (WT_F64, WT_F64, ALLOC_node_f64_nearest); break;
        case 0x9F: BUN (WT_F64, WT_F64, ALLOC_node_f64_sqrt); break;
        case 0xA0: BBIN(WT_F64, WT_F64, ALLOC_node_f64_add); break;
        case 0xA1: BBIN(WT_F64, WT_F64, ALLOC_node_f64_sub); break;
        case 0xA2: BBIN(WT_F64, WT_F64, ALLOC_node_f64_mul); break;
        case 0xA3: BBIN(WT_F64, WT_F64, ALLOC_node_f64_div); break;
        case 0xA4: BBIN(WT_F64, WT_F64, ALLOC_node_f64_min); break;
        case 0xA5: BBIN(WT_F64, WT_F64, ALLOC_node_f64_max); break;
        case 0xA6: BBIN(WT_F64, WT_F64, ALLOC_node_f64_copysign); break;

        case 0xA7: BUN(WT_I64, WT_I32, ALLOC_node_i32_wrap_i64); break;
        case 0xA8: BUN(WT_F32, WT_I32, ALLOC_node_i32_trunc_f32_s); break;
        case 0xA9: BUN(WT_F32, WT_I32, ALLOC_node_i32_trunc_f32_u); break;
        case 0xAA: BUN(WT_F64, WT_I32, ALLOC_node_i32_trunc_f64_s); break;
        case 0xAB: BUN(WT_F64, WT_I32, ALLOC_node_i32_trunc_f64_u); break;
        case 0xAC: BUN(WT_I32, WT_I64, ALLOC_node_i64_extend_i32_s); break;
        case 0xAD: BUN(WT_I32, WT_I64, ALLOC_node_i64_extend_i32_u); break;
        case 0xAE: BUN(WT_F32, WT_I64, ALLOC_node_i64_trunc_f32_s); break;
        case 0xAF: BUN(WT_F32, WT_I64, ALLOC_node_i64_trunc_f32_u); break;
        case 0xB0: BUN(WT_F64, WT_I64, ALLOC_node_i64_trunc_f64_s); break;
        case 0xB1: BUN(WT_F64, WT_I64, ALLOC_node_i64_trunc_f64_u); break;
        case 0xB2: BUN(WT_I32, WT_F32, ALLOC_node_f32_convert_i32_s); break;
        case 0xB3: BUN(WT_I32, WT_F32, ALLOC_node_f32_convert_i32_u); break;
        case 0xB4: BUN(WT_I64, WT_F32, ALLOC_node_f32_convert_i64_s); break;
        case 0xB5: BUN(WT_I64, WT_F32, ALLOC_node_f32_convert_i64_u); break;
        case 0xB6: BUN(WT_F64, WT_F32, ALLOC_node_f32_demote_f64); break;
        case 0xB7: BUN(WT_I32, WT_F64, ALLOC_node_f64_convert_i32_s); break;
        case 0xB8: BUN(WT_I32, WT_F64, ALLOC_node_f64_convert_i32_u); break;
        case 0xB9: BUN(WT_I64, WT_F64, ALLOC_node_f64_convert_i64_s); break;
        case 0xBA: BUN(WT_I64, WT_F64, ALLOC_node_f64_convert_i64_u); break;
        case 0xBB: BUN(WT_F32, WT_F64, ALLOC_node_f64_promote_f32); break;
        case 0xBC: BUN(WT_F32, WT_I32, ALLOC_node_i32_reinterpret_f32); break;
        case 0xBD: BUN(WT_F64, WT_I64, ALLOC_node_i64_reinterpret_f64); break;
        case 0xBE: BUN(WT_I32, WT_F32, ALLOC_node_f32_reinterpret_i32); break;
        case 0xBF: BUN(WT_I64, WT_F64, ALLOC_node_f64_reinterpret_i64); break;
        case 0xC0: BUN(WT_I32, WT_I32, ALLOC_node_i32_extend8_s); break;
        case 0xC1: BUN(WT_I32, WT_I32, ALLOC_node_i32_extend16_s); break;
        case 0xC2: BUN(WT_I64, WT_I64, ALLOC_node_i64_extend8_s); break;
        case 0xC3: BUN(WT_I64, WT_I64, ALLOC_node_i64_extend16_s); break;
        case 0xC4: BUN(WT_I64, WT_I64, ALLOC_node_i64_extend32_s); break;

        case 0xFC: {
            uint32_t sub = bin_leb_u32(r);
            switch (sub) {
            case 0: BUN(WT_F32, WT_I32, ALLOC_node_i32_trunc_sat_f32_s); break;
            case 1: BUN(WT_F32, WT_I32, ALLOC_node_i32_trunc_sat_f32_u); break;
            case 2: BUN(WT_F64, WT_I32, ALLOC_node_i32_trunc_sat_f64_s); break;
            case 3: BUN(WT_F64, WT_I32, ALLOC_node_i32_trunc_sat_f64_u); break;
            case 4: BUN(WT_F32, WT_I64, ALLOC_node_i64_trunc_sat_f32_s); break;
            case 5: BUN(WT_F32, WT_I64, ALLOC_node_i64_trunc_sat_f32_u); break;
            case 6: BUN(WT_F64, WT_I64, ALLOC_node_i64_trunc_sat_f64_s); break;
            case 7: BUN(WT_F64, WT_I64, ALLOC_node_i64_trunc_sat_f64_u); break;
            default:
                wastro_die("binary: unsupported 0xFC subop %u", sub);
            }
        } break;

        default:
            wastro_die("binary: unknown opcode 0x%02x", op);
        }
    }
    NODE *final_val = NULL;
    wtype_t final_t = WT_VOID;
    if (S.cnt >= 1) {
        final_val = S.items[S.cnt - 1].node;
        final_t = S.items[S.cnt - 1].type;
        for (uint32_t i = 0; i + 1 < S.cnt; i++) stmts_append(&L, S.items[i].node);
    }
    NODE *body = build_body_node(&L, final_val);
    return (TypedExpr){body, final_t};
}

#undef BBIN
#undef BUN
#undef BLOAD
#undef BSTORE

// Decode a const expr (used in global init / data offset / elem offset).
// Only `*.const X end` and `global.get X end` forms are accepted.
static VALUE
parse_const_expr(BinReader *r, wtype_t *out_type)
{
    uint8_t op = bin_u8(r);
    VALUE v = 0;
    if (op == 0x41) {
        v = FROM_I32(bin_leb_s32(r));
        if (out_type) *out_type = WT_I32;
    } else if (op == 0x42) {
        v = FROM_I64(bin_leb_s64(r));
        if (out_type) *out_type = WT_I64;
    } else if (op == 0x43) {
        v = FROM_U32(bin_u32(r));
        if (out_type) *out_type = WT_F32;
    } else if (op == 0x44) {
        v = FROM_U64(bin_u64(r));
        if (out_type) *out_type = WT_F64;
    } else if (op == 0x23) {
        uint32_t gi = bin_leb_u32(r);
        v = WASTRO_GLOBALS ? WASTRO_GLOBALS[gi] : 0;
        if (out_type) *out_type = WASTRO_GLOBAL_TYPES[gi];
    } else {
        wastro_die("binary: unsupported const-expr opcode 0x%02x", op);
    }
    if (bin_u8(r) != 0x0B) {
        wastro_die("binary: const-expr missing end");
    }
    return v;
}

static void
load_module_binary(const uint8_t *buf, size_t sz)
{
    BinReader R = { buf, buf + sz };
    if (sz < 8 || memcmp(buf, "\0asm", 4) != 0) {
        wastro_die("binary: bad magic");
    }
    R.p += 4;
    uint32_t version = bin_u32(&R);
    if (version != 1) {
        wastro_die("binary: unsupported version %u", version);
    }

    // Track imported function count for the Function section indexing.
    uint32_t imported_funcs = 0;

    // First-pass: section dispatch.  Sections are unique except Custom.
    while (R.p < R.end) {
        uint8_t sid = bin_u8(&R);
        uint32_t ssize = bin_leb_u32(&R);
        const uint8_t *send = R.p + ssize;
        bin_check(&R, ssize, "section payload");
        BinReader S2 = { R.p, send };

        switch (sid) {
        case 0:   // Custom section — skip
            break;
        case 1: { // Type
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                if (bin_u8(&S2) != 0x60) parse_error("binary: bad type form");
                struct wastro_type_sig sig = {0};
                sig.param_cnt = bin_leb_u32(&S2);
                if (sig.param_cnt > WASTRO_MAX_PARAMS) parse_error("binary: too many params");
                for (uint32_t k = 0; k < sig.param_cnt; k++)
                    sig.param_types[k] = bin_valtype(bin_u8(&S2));
                uint32_t rc = bin_leb_u32(&S2);
                if (rc > 1) parse_error("binary: multi-result not supported");
                sig.result_type = rc ? bin_valtype(bin_u8(&S2)) : WT_VOID;
                if (WASTRO_TYPE_CNT >= WASTRO_MAX_TYPES) parse_error("binary: too many types");
                WASTRO_TYPES[WASTRO_TYPE_CNT++] = sig;
            }
        } break;
        case 2: { // Import
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t ml = bin_leb_u32(&S2);
                char mod[64]; if (ml >= 64) parse_error("binary: import mod too long");
                bin_check(&S2, ml, "import mod"); memcpy(mod, S2.p, ml); mod[ml] = 0; S2.p += ml;
                uint32_t fl = bin_leb_u32(&S2);
                char fld[64]; if (fl >= 64) parse_error("binary: import fld too long");
                bin_check(&S2, fl, "import fld"); memcpy(fld, S2.p, fl); fld[fl] = 0; S2.p += fl;
                uint8_t kind = bin_u8(&S2);
                if (kind == 0x00) {
                    uint32_t ti = bin_leb_u32(&S2);
                    int fi = WASTRO_FUNC_CNT;
                    if (fi >= WASTRO_MAX_FUNCS) parse_error("binary: too many funcs");
                    WASTRO_FUNCS[fi].is_import = 1;
                    WASTRO_FUNCS[fi].name = NULL;
                    WASTRO_FUNCS[fi].local_cnt = 0;
                    const struct host_entry *he = find_host(mod, fld);
                    struct wastro_type_sig *sig = &WASTRO_TYPES[ti];
                    if (he) {
                        WASTRO_FUNCS[fi].host_fn = he->fn;
                        WASTRO_FUNCS[fi].param_cnt = he->param_cnt;
                        for (uint32_t k = 0; k < he->param_cnt; k++)
                            WASTRO_FUNCS[fi].param_types[k] = he->param_types[k];
                        WASTRO_FUNCS[fi].result_type = he->result_type;
                    } else {
                        WASTRO_FUNCS[fi].host_fn = host_unbound_trap;
                        WASTRO_FUNCS[fi].param_cnt = sig->param_cnt;
                        for (uint32_t k = 0; k < sig->param_cnt; k++)
                            WASTRO_FUNCS[fi].param_types[k] = sig->param_types[k];
                        WASTRO_FUNCS[fi].result_type = sig->result_type;
                    }
                    WASTRO_FUNC_CNT++;
                    imported_funcs++;
                } else if (kind == 0x01) {
                    (void)bin_u8(&S2);   // reftype (funcref = 0x70)
                    uint8_t flags = bin_u8(&S2);
                    uint32_t init = bin_leb_u32(&S2);
                    uint32_t mx = (flags & 1) ? bin_leb_u32(&S2) : 0xFFFFFFFFu;
                    if (MOD_HAS_TABLE) parse_error("binary: multiple tables");
                    MOD_HAS_TABLE = 1;
                    WASTRO_TABLE_SIZE = init; WASTRO_TABLE_MAX = mx;
                    WASTRO_TABLE = malloc(sizeof(int32_t) * (init ? init : 1));
                    for (uint32_t k = 0; k < init; k++) WASTRO_TABLE[k] = -1;
                } else if (kind == 0x02) {
                    uint8_t flags = bin_u8(&S2);
                    MOD_MEM_INITIAL_PAGES = bin_leb_u32(&S2);
                    MOD_MEM_MAX_PAGES = (flags & 1) ? bin_leb_u32(&S2) : 0xFFFFFFFFu;
                    MOD_HAS_MEMORY = 1;
                } else if (kind == 0x03) {
                    uint8_t vt = bin_u8(&S2);
                    uint8_t mut = bin_u8(&S2);
                    if (WASTRO_GLOBAL_CNT >= WASTRO_MAX_GLOBALS) parse_error("binary: too many globals");
                    if (!WASTRO_GLOBALS) WASTRO_GLOBALS = calloc(WASTRO_MAX_GLOBALS, sizeof(VALUE));
                    WASTRO_GLOBALS[WASTRO_GLOBAL_CNT] = 0;
                    WASTRO_GLOBAL_TYPES[WASTRO_GLOBAL_CNT] = bin_valtype(vt);
                    WASTRO_GLOBAL_MUT[WASTRO_GLOBAL_CNT] = mut;
                    WASTRO_GLOBAL_NAMES[WASTRO_GLOBAL_CNT] = NULL;
                    WASTRO_GLOBAL_CNT++;
                } else parse_error("binary: bad import kind");
            }
        } break;
        case 3: { // Function — typeidx for each defined func
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t ti = bin_leb_u32(&S2);
                int fi = WASTRO_FUNC_CNT;
                if (fi >= WASTRO_MAX_FUNCS) parse_error("binary: too many funcs");
                struct wastro_type_sig *sig = &WASTRO_TYPES[ti];
                WASTRO_FUNCS[fi].name = NULL;
                WASTRO_FUNCS[fi].is_import = 0;
                WASTRO_FUNCS[fi].param_cnt = sig->param_cnt;
                for (uint32_t k = 0; k < sig->param_cnt; k++)
                    WASTRO_FUNCS[fi].param_types[k] = sig->param_types[k];
                WASTRO_FUNCS[fi].result_type = sig->result_type;
                WASTRO_FUNCS[fi].local_cnt = sig->param_cnt;   // body sets total
                for (uint32_t k = 0; k < sig->param_cnt; k++)
                    WASTRO_FUNCS[fi].local_types[k] = sig->param_types[k];
                WASTRO_FUNC_CNT++;
            }
        } break;
        case 4: { // Table
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                (void)bin_u8(&S2);   // reftype
                uint8_t flags = bin_u8(&S2);
                uint32_t init = bin_leb_u32(&S2);
                uint32_t mx = (flags & 1) ? bin_leb_u32(&S2) : 0xFFFFFFFFu;
                if (MOD_HAS_TABLE) parse_error("binary: multiple tables");
                MOD_HAS_TABLE = 1;
                WASTRO_TABLE_SIZE = init; WASTRO_TABLE_MAX = mx;
                WASTRO_TABLE = malloc(sizeof(int32_t) * (init ? init : 1));
                for (uint32_t k = 0; k < init; k++) WASTRO_TABLE[k] = -1;
            }
        } break;
        case 5: { // Memory
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint8_t flags = bin_u8(&S2);
                MOD_MEM_INITIAL_PAGES = bin_leb_u32(&S2);
                MOD_MEM_MAX_PAGES = (flags & 1) ? bin_leb_u32(&S2) : 0xFFFFFFFFu;
                MOD_HAS_MEMORY = 1;
            }
        } break;
        case 6: { // Global
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint8_t vt = bin_u8(&S2);
                uint8_t mut = bin_u8(&S2);
                wtype_t wtv = bin_valtype(vt);
                wtype_t got_t;
                VALUE init_val = parse_const_expr(&S2, &got_t);
                if (WASTRO_GLOBAL_CNT >= WASTRO_MAX_GLOBALS) parse_error("binary: too many globals");
                if (!WASTRO_GLOBALS) WASTRO_GLOBALS = calloc(WASTRO_MAX_GLOBALS, sizeof(VALUE));
                WASTRO_GLOBALS[WASTRO_GLOBAL_CNT] = init_val;
                WASTRO_GLOBAL_TYPES[WASTRO_GLOBAL_CNT] = wtv;
                WASTRO_GLOBAL_MUT[WASTRO_GLOBAL_CNT] = mut;
                WASTRO_GLOBAL_NAMES[WASTRO_GLOBAL_CNT] = NULL;
                WASTRO_GLOBAL_CNT++;
            }
        } break;
        case 7: { // Export
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t nl = bin_leb_u32(&S2);
                bin_check(&S2, nl, "export name");
                char *name = malloc(nl + 1); memcpy(name, S2.p, nl); name[nl] = 0; S2.p += nl;
                uint8_t kind = bin_u8(&S2);
                uint32_t idx = bin_leb_u32(&S2);
                if (kind == 0) {
                    WASTRO_FUNCS[idx].exported = 1;
                    WASTRO_FUNCS[idx].export_name = name;
                } else {
                    free(name);   // mem/global/table exports — ignored
                }
            }
        } break;
        case 8: { // Start
            uint32_t fi = bin_leb_u32(&S2);
            MOD_HAS_START = 1;
            MOD_START_FUNC = (int)fi;
        } break;
        case 9: { // Element
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t flags = bin_leb_u32(&S2);
                if (flags != 0) {
                    wastro_die("binary: elem flag %u not supported", flags);
                }
                wtype_t off_t;
                VALUE off_v = parse_const_expr(&S2, &off_t);
                uint32_t off = AS_U32(off_v);
                uint32_t ec = bin_leb_u32(&S2);
                for (uint32_t k = 0; k < ec; k++) {
                    uint32_t fi = bin_leb_u32(&S2);
                    if (!MOD_HAS_TABLE) parse_error("binary: elem without table");
                    if (off + k >= WASTRO_TABLE_SIZE) parse_error("binary: elem overflows table");
                    WASTRO_TABLE[off + k] = (int32_t)fi;
                }
            }
        } break;
        case 10: { // Code
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t body_size = bin_leb_u32(&S2);
                const uint8_t *body_end = S2.p + body_size;
                int fi = (int)imported_funcs + (int)i;
                struct wastro_function *fn = &WASTRO_FUNCS[fi];
                LocalEnv env = {0};
                env.cnt = fn->param_cnt;
                for (uint32_t k = 0; k < env.cnt; k++) {
                    env.names[k] = NULL;
                    env.types[k] = fn->param_types[k];
                }
                // locals: vec of (count, valtype)
                BinReader BR = { S2.p, body_end };
                uint32_t lg = bin_leb_u32(&BR);
                for (uint32_t g = 0; g < lg; g++) {
                    uint32_t cnt = bin_leb_u32(&BR);
                    wtype_t lt = bin_valtype(bin_u8(&BR));
                    for (uint32_t k = 0; k < cnt; k++) {
                        if (env.cnt >= 64) parse_error("binary: too many locals");
                        env.names[env.cnt] = NULL;
                        env.types[env.cnt] = lt;
                        env.cnt++;
                    }
                }
                fn->local_cnt = env.cnt;
                for (uint32_t k = 0; k < env.cnt; k++) fn->local_types[k] = env.types[k];
                LabelEnv labels = {0};
                int save_idx = CUR_FUNC_IDX;
                CUR_FUNC_IDX = fi;
                TypedExpr body = parse_bin_code_seq(&BR, &env, &labels, 0, NULL);
                CUR_FUNC_IDX = save_idx;
                if (BR.p != body_end) {
                    wastro_die("binary: code body length mismatch");
                }
                fn->body = body.node;
                fn->entry = ALLOC_node_function_frame((uint32_t)fi, body.node);
                S2.p = body_end;
            }
        } break;
        case 11: { // Data
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t flags = bin_leb_u32(&S2);
                uint32_t off = 0;
                if (flags == 0) {
                    wtype_t ot;
                    VALUE off_v = parse_const_expr(&S2, &ot);
                    off = AS_U32(off_v);
                } else if (flags == 2) {
                    (void)bin_leb_u32(&S2);   // memidx (always 0)
                    wtype_t ot;
                    VALUE off_v = parse_const_expr(&S2, &ot);
                    off = AS_U32(off_v);
                } else {
                    wastro_die("binary: data flag %u not supported", flags);
                }
                uint32_t dl = bin_leb_u32(&S2);
                bin_check(&S2, dl, "data bytes");
                if (MOD_DATA_SEG_CNT >= WASTRO_MAX_DATA_SEGS) parse_error("binary: too many data");
                MOD_DATA_SEGS[MOD_DATA_SEG_CNT].offset = off;
                MOD_DATA_SEGS[MOD_DATA_SEG_CNT].length = dl;
                uint8_t *bytes = malloc(dl);
                memcpy(bytes, S2.p, dl);
                MOD_DATA_SEGS[MOD_DATA_SEG_CNT].bytes = bytes;
                MOD_DATA_SEG_CNT++;
                S2.p += dl;
            }
        } break;
        case 12: { // DataCount (post-1.0; ignore)
            (void)bin_leb_u32(&S2);
        } break;
        default:
            fprintf(stderr, "wastro: binary: unknown section id %u (skipping)\n", sid);
            break;
        }
        R.p = send;
    }
}

// Load a module from a memory buffer.  Detects binary vs text by
// magic.  Caller retains ownership of `buf` until done with the
// module — text-mode parsing keeps pointers into it.
NODE *
wastro_load_module_buf(const char *buf, size_t sz)
{
    if (sz >= 4 && (uint8_t)buf[0] == 0 && buf[1] == 'a' && buf[2] == 's' && buf[3] == 'm') {
        load_module_binary((const uint8_t *)buf, sz);
        wastro_fixup_call_bodies();
        return NULL;
    }
    MODULE_TEXT_START = buf;
    const char *func_offsets[WASTRO_MAX_FUNCS];
    int n = 0;
    scan_module(buf, sz, func_offsets, &n);

    // Resolve deferred (export "name" (func $f)) — the func may be
    // declared later in the source, so we resolve post-scan.
    for (uint32_t s = 0; s < PENDING_EXPORT_CNT; s++) {
        struct export_pending *ep = &PENDING_EXPORTS[s];
        int fi = resolve_func(&ep->ref);
        WASTRO_FUNCS[fi].exported = 1;
        WASTRO_FUNCS[fi].export_name = ep->name;
    }
    PENDING_EXPORT_CNT = 0;

    if (MOD_HAS_START) {
        MOD_START_FUNC = resolve_func(&MOD_START_TOK);
    }

    for (uint32_t s = 0; s < PENDING_ELEM_CNT; s++) {
        struct elem_pending *ep = &PENDING_ELEMS[s];
        if (!MOD_HAS_TABLE) { fprintf(stderr, "wastro: (elem ...) without (table ...)\n"); exit(1); }
        for (uint32_t i = 0; i < ep->cnt; i++) {
            int fi = resolve_func(&ep->refs[i]);
            if (ep->offset + i >= WASTRO_TABLE_SIZE) {
                fprintf(stderr, "wastro: (elem) overflows table\n"); exit(1);
            }
            WASTRO_TABLE[ep->offset + i] = fi;
        }
        free(ep->refs);
    }
    PENDING_ELEM_CNT = 0;

    for (int i = 0; i < n; i++) {
        if (!func_offsets[i]) continue;
        src_pos = func_offsets[i];
        src_end = buf + sz;
        next_token();
        parse_func_pass2(i);
    }
    wastro_fixup_call_bodies();
    return NULL;
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
    return wastro_load_module_buf(buf, (size_t)sz);
}

#if 0
// Old in-place body of wastro_load_module — superseded by
// wastro_load_module_buf which does the same work on memory.
NODE *
wastro_load_module_OLD(const char *path)
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

    // Detect binary by magic: 0x00 'a' 's' 'm'.
    if (sz >= 4 && (uint8_t)buf[0] == 0 && buf[1] == 'a' && buf[2] == 's' && buf[3] == 'm') {
        load_module_binary((const uint8_t *)buf, (size_t)sz);
        return NULL;
    }

    MODULE_TEXT_START = buf;

    const char *func_offsets[WASTRO_MAX_FUNCS];
    int n = 0;
    scan_module(buf, (size_t)sz, func_offsets, &n);

    // Resolve deferred (export "name" (func $f)) — the func may be
    // declared later in the source, so we resolve post-scan.
    for (uint32_t s = 0; s < PENDING_EXPORT_CNT; s++) {
        struct export_pending *ep = &PENDING_EXPORTS[s];
        int fi = resolve_func(&ep->ref);
        WASTRO_FUNCS[fi].exported = 1;
        WASTRO_FUNCS[fi].export_name = ep->name;   // ownership transferred
    }
    PENDING_EXPORT_CNT = 0;

    if (MOD_HAS_START) {
        MOD_START_FUNC = resolve_func(&MOD_START_TOK);
    }

    // Resolve deferred elem segments — function names from elem can
    // refer to funcs defined later in the source, so we resolve here
    // (after all func names are registered).
    for (uint32_t s = 0; s < PENDING_ELEM_CNT; s++) {
        struct elem_pending *ep = &PENDING_ELEMS[s];
        if (!MOD_HAS_TABLE) {
            fprintf(stderr, "wastro: (elem ...) without (table ...)\n"); exit(1);
        }
        for (uint32_t i = 0; i < ep->cnt; i++) {
            int fi = resolve_func(&ep->refs[i]);
            if (ep->offset + i >= WASTRO_TABLE_SIZE) {
                fprintf(stderr,
                    "wastro: (elem) overflows table (size %u, writing index %u)\n",
                    WASTRO_TABLE_SIZE, ep->offset + i);
                exit(1);
            }
            WASTRO_TABLE[ep->offset + i] = fi;
        }
        free(ep->refs);
    }
    PENDING_ELEM_CNT = 0;

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
#endif

// =====================================================================
// Spec test harness — `.wast` runner
// =====================================================================
//
// `.wast` files are WAT plus assertion forms used by the wasm spec
// testsuite.  We support the most common subset:
//   (module ...)
//   (assert_return (invoke "name" args...) result)
//   (assert_trap (invoke "name" args...) "trap-msg")
//   (invoke "name" args...)
//   (register "name")    — accepted but ignored (no cross-module link)
//   (assert_invalid ...) | (assert_malformed ...) — reported as skipped
//   (assert_exhaustion ...) — reported as skipped

static CTX *wastro_instantiate(uint32_t initial_local_slots);
static VALUE wastro_invoke(CTX *c, int func_idx, VALUE *args, uint32_t argc);

static void
wastro_reset_module(void)
{
    for (uint32_t i = 0; i < WASTRO_FUNC_CNT; i++) {
        if (WASTRO_FUNCS[i].name) free((void *)WASTRO_FUNCS[i].name);
        if (WASTRO_FUNCS[i].export_name) free((void *)WASTRO_FUNCS[i].export_name);
    }
    memset(WASTRO_FUNCS, 0, sizeof(WASTRO_FUNCS));
    WASTRO_FUNC_CNT = 0;

    if (WASTRO_GLOBALS) { free(WASTRO_GLOBALS); WASTRO_GLOBALS = NULL; }
    for (uint32_t i = 0; i < WASTRO_GLOBAL_CNT; i++) {
        if (WASTRO_GLOBAL_NAMES[i]) free(WASTRO_GLOBAL_NAMES[i]);
        WASTRO_GLOBAL_NAMES[i] = NULL;
    }
    WASTRO_GLOBAL_CNT = 0;

    if (WASTRO_BR_TABLE) { free(WASTRO_BR_TABLE); WASTRO_BR_TABLE = NULL; }
    WASTRO_BR_TABLE_CNT = 0;
    WASTRO_BR_TABLE_CAP = 0;

    for (uint32_t i = 0; i < MOD_DATA_SEG_CNT; i++) {
        if (MOD_DATA_SEGS[i].bytes) free(MOD_DATA_SEGS[i].bytes);
    }
    MOD_DATA_SEG_CNT = 0;

    for (uint32_t i = 0; i < WASTRO_TYPE_CNT; i++) {
        if (WASTRO_TYPE_NAMES[i]) { free(WASTRO_TYPE_NAMES[i]); WASTRO_TYPE_NAMES[i] = NULL; }
    }
    memset(WASTRO_TYPES, 0, sizeof(WASTRO_TYPES));
    WASTRO_TYPE_CNT = 0;

    if (WASTRO_TABLE) { free(WASTRO_TABLE); WASTRO_TABLE = NULL; }
    WASTRO_TABLE_SIZE = 0;
    WASTRO_TABLE_MAX = 0;
    MOD_HAS_TABLE = 0;

    MOD_HAS_MEMORY = 0;
    MOD_MEM_INITIAL_PAGES = 0;
    MOD_MEM_MAX_PAGES = 65536;

    MOD_HAS_START = 0;
    MOD_START_FUNC = -1;

    PENDING_ELEM_CNT = 0;
    PENDING_EXPORT_CNT = 0;
}

// Parse a constant expression `(T.const X)` from the active token
// stream, advance past it, and return the encoded VALUE plus type.
static VALUE
wast_parse_const_value(wtype_t *out_type)
{
    expect_lparen();
    VALUE v = 0;
    if (tok_is_keyword("i32.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected i32 literal");
        v = FROM_I32((int32_t)cur_tok.int_value);
        if (out_type) *out_type = WT_I32;
        next_token();
    }
    else if (tok_is_keyword("i64.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected i64 literal");
        v = FROM_I64((int64_t)cur_tok.int_value);
        if (out_type) *out_type = WT_I64;
        next_token();
    }
    else if (tok_is_keyword("f32.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected f32 literal");
        double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
        uint32_t bits = token_to_f32_bits(&cur_tok, dv);
        v = FROM_U32(bits);
        if (out_type) *out_type = WT_F32;
        next_token();
    }
    else if (tok_is_keyword("f64.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected f64 literal");
        double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
        uint64_t bits = token_to_f64_bits(&cur_tok, dv);
        v = FROM_U64(bits);
        if (out_type) *out_type = WT_F64;
        next_token();
    }
    else parse_error("expected (T.const X) value");
    expect_rparen();
    return v;
}

// Skip a balanced (...) form starting at cur_tok='('.  Used to skip
// over assert_invalid / assert_malformed contents we don't validate.
static void
wast_skip_balanced(void)
{
    int depth = 0;
    do {
        if (cur_tok.kind == T_LPAREN) depth++;
        else if (cur_tok.kind == T_RPAREN) depth--;
        else if (cur_tok.kind == T_EOF) parse_error("unbalanced parens");
        next_token();
    } while (depth > 0);
}

// Parse `(invoke "name" args...)`.  Returns 1 if found and resolved,
// 0 if invoke target unresolved.  Caller has consumed the leading '('
// and is at cur_tok='invoke'.
static int
wast_parse_invoke(int *func_idx_out, VALUE *args_out, uint32_t *argc_out)
{
    next_token();   // consume 'invoke'
    if (cur_tok.kind != T_STRING) parse_error("invoke: expected name string");
    char name[128]; size_t nl = cur_tok.len < 127 ? cur_tok.len : 127;
    memcpy(name, cur_tok.start, nl); name[nl] = 0;
    next_token();
    int fi = wastro_find_export(name);
    *argc_out = 0;
    while (cur_tok.kind == T_LPAREN) {
        wtype_t at;
        args_out[(*argc_out)++] = wast_parse_const_value(&at);
    }
    expect_rparen();
    *func_idx_out = fi;
    return fi >= 0;
}

// Run a single test and report.  Returns 1 = pass, 0 = fail, -1 = skip.
typedef enum { TR_PASS = 1, TR_FAIL = 0, TR_SKIP = -1 } TestResult;

static TestResult
wast_run_assert_return(int line, CTX *c)
{
    expect_lparen();
    if (!tok_is_keyword("invoke")) {
        wast_skip_balanced();
        // skip the rest
        while (cur_tok.kind != T_RPAREN) {
            if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
            else next_token();
        }
        expect_rparen();
        return TR_SKIP;
    }
    int func_idx;
    VALUE args[WASTRO_MAX_PARAMS];
    uint32_t argc;
    if (!wast_parse_invoke(&func_idx, args, &argc)) {
        // unresolved — skip and skip remaining args
        while (cur_tok.kind != T_RPAREN) {
            if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
            else next_token();
        }
        expect_rparen();
        return TR_SKIP;
    }
    // Optional expected result.  Multi-value (post-1.0) extras are
    // accepted and discarded — we compare only the first.
    int has_expected = 0;
    VALUE expected = 0;
    wtype_t exp_t = WT_VOID;
    if (cur_tok.kind == T_LPAREN) {
        expected = wast_parse_const_value(&exp_t);
        has_expected = 1;
        while (cur_tok.kind == T_LPAREN) {
            wtype_t t; (void)wast_parse_const_value(&t);
        }
    }
    expect_rparen();

    if (setjmp(wastro_trap_jmp) == 0) {
        wastro_trap_active = 1;
        VALUE got = wastro_invoke(c, func_idx, args, argc);
        wastro_trap_active = 0;
        if (has_expected) {
            int ok = 0;
            switch (exp_t) {
            case WT_I32: ok = (AS_I32(got) == AS_I32(expected)); break;
            case WT_I64: ok = (AS_I64(got) == AS_I64(expected)); break;
            case WT_F32: {
                uint32_t gb = (uint32_t)got, eb = (uint32_t)expected;
                float gf, ef; memcpy(&gf, &gb, 4); memcpy(&ef, &eb, 4);
                ok = (gb == eb) || (gf != gf && ef != ef);   // bit-exact OR both NaN
            } break;
            case WT_F64: {
                uint64_t gb = got, eb = expected;
                double gf, ef; memcpy(&gf, &gb, 8); memcpy(&ef, &eb, 8);
                ok = (gb == eb) || (gf != gf && ef != ef);
            } break;
            default: ok = 1;
            }
            if (!ok) {
                fprintf(stderr, "FAIL line %d: %s expected=%llx got=%llx\n",
                        line, wtype_name(exp_t),
                        (unsigned long long)expected, (unsigned long long)got);
                return TR_FAIL;
            }
        }
        return TR_PASS;
    }
    else {
        wastro_trap_active = 0;
        fprintf(stderr, "FAIL line %d: unexpected trap (%s)\n", line, wastro_trap_message);
        return TR_FAIL;
    }
}

static TestResult
wast_run_assert_trap(int line, CTX *c)
{
    expect_lparen();
    if (!tok_is_keyword("invoke")) {
        wast_skip_balanced();
        while (cur_tok.kind != T_RPAREN) {
            if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
            else next_token();
        }
        expect_rparen();
        return TR_SKIP;
    }
    int func_idx;
    VALUE args[WASTRO_MAX_PARAMS];
    uint32_t argc;
    if (!wast_parse_invoke(&func_idx, args, &argc)) {
        while (cur_tok.kind != T_RPAREN) {
            if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
            else next_token();
        }
        expect_rparen();
        return TR_SKIP;
    }
    if (cur_tok.kind == T_STRING) next_token();   // expected trap msg — ignored
    expect_rparen();

    if (setjmp(wastro_trap_jmp) == 0) {
        wastro_trap_active = 1;
        wastro_invoke(c, func_idx, args, argc);
        wastro_trap_active = 0;
        fprintf(stderr, "FAIL line %d: expected trap, returned normally\n", line);
        return TR_FAIL;
    }
    else {
        wastro_trap_active = 0;
        return TR_PASS;
    }
}

// Walk the .wast file form-by-form.  For each `(module ...)`, reset
// state and load.  For each assertion, run it.  Print summary.
static int
wastro_run_wast(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("fread"); return 2; }
    buf[sz] = 0;
    fclose(f);

    src_pos = buf;
    src_end = buf + sz;
    next_token();

    int passed = 0, failed = 0, skipped = 0;
    int loaded_any = 0;
    int load_failed = 0;       // sticky: subsequent asserts auto-skip
    CTX *active_ctx = NULL;    // persistent instance across asserts
    while (cur_tok.kind != T_EOF) {
        // Track approximate line number for diagnostics.
        int line = 1;
        for (const char *p = buf; p < cur_tok.start; p++) if (*p == '\n') line++;

        if (cur_tok.kind != T_LPAREN) parse_error("wast: expected '('");
        const char *form_start = cur_tok.start;
        next_token();
        if (cur_tok.kind != T_KEYWORD) parse_error("wast: expected keyword");

        if (tok_is_keyword("module")) {
            // Find the closing ')' to delimit the module text.
            int depth = 1;
            while (depth > 0 && cur_tok.kind != T_EOF) {
                next_token();
                if (cur_tok.kind == T_LPAREN) depth++;
                else if (cur_tok.kind == T_RPAREN) depth--;
            }
            const char *form_end = src_pos;
            // Save outer parser state, load the module, restore.
            const char *o_pos = src_pos, *o_end = src_end;
            Token o_tok = cur_tok;
            // Free the previous module's instance (memory etc.).
            if (active_ctx) {
                if (active_ctx->memory) free(active_ctx->memory);
                free(active_ctx);
                active_ctx = NULL;
            }
            wastro_reset_module();
            load_failed = 0;
            if (setjmp(wastro_parse_jmp) == 0) {
                wastro_parse_active = 1;
                wastro_load_module_buf(form_start, (size_t)(form_end - form_start));
                wastro_parse_active = 0;
                loaded_any = 1;
                // Build a persistent instance for this module.  Any
                // (start) is invoked at instantiation.
                uint32_t locals = 0;
                if (MOD_HAS_START) locals = WASTRO_FUNCS[MOD_START_FUNC].local_cnt;
                for (uint32_t i = 0; i < WASTRO_FUNC_CNT; i++) {
                    if (WASTRO_FUNCS[i].local_cnt > locals)
                        locals = WASTRO_FUNCS[i].local_cnt;
                }
                if (locals < 16) locals = 16;
                if (setjmp(wastro_trap_jmp) == 0) {
                    wastro_trap_active = 1;
                    active_ctx = wastro_instantiate(locals);
                    wastro_trap_active = 0;
                }
                else {
                    wastro_trap_active = 0;
                    fprintf(stderr, "  skip line %d: instantiation trap: %s\n", line, wastro_trap_message);
                    load_failed = 1;
                    active_ctx = NULL;
                    goto skip_module_done;
                }
                if (MOD_HAS_START) {
                    if (setjmp(wastro_trap_jmp) == 0) {
                        wastro_trap_active = 1;
                        wastro_invoke(active_ctx, MOD_START_FUNC, NULL, 0);
                        wastro_trap_active = 0;
                    }
                    else { wastro_trap_active = 0; }
                }
            skip_module_done: ;
            }
            else {
                wastro_parse_active = 0;
                fprintf(stderr, "  skip line %d: module load failed: %s\n", line, wastro_parse_message);
                load_failed = 1;
                loaded_any = 1;   // we did "load" — subsequent asserts skip
            }
            src_pos = o_pos; src_end = o_end; cur_tok = o_tok;
            next_token();   // advance past the module's ')'
        }
        else if (tok_is_keyword("assert_return")) {
            next_token();
            if (!loaded_any || load_failed) {
                while (cur_tok.kind != T_RPAREN) {
                    if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
                    else next_token();
                }
                expect_rparen();
                skipped++; continue;
            }
            TestResult r;
            if (setjmp(wastro_parse_jmp) == 0) {
                wastro_parse_active = 1;
                r = wast_run_assert_return(line, active_ctx);
                wastro_parse_active = 0;
            }
            else {
                wastro_parse_active = 0;
                fprintf(stderr, "  skip line %d: parse error in assert: %s\n", line, wastro_parse_message);
                r = TR_SKIP;
            }
            if (r == TR_PASS) passed++;
            else if (r == TR_FAIL) failed++;
            else skipped++;
        }
        else if (tok_is_keyword("assert_trap")) {
            next_token();
            if (!loaded_any || load_failed) {
                while (cur_tok.kind != T_RPAREN) {
                    if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
                    else next_token();
                }
                expect_rparen();
                skipped++; continue;
            }
            TestResult r;
            if (setjmp(wastro_parse_jmp) == 0) {
                wastro_parse_active = 1;
                r = wast_run_assert_trap(line, active_ctx);
                wastro_parse_active = 0;
            }
            else {
                wastro_parse_active = 0;
                fprintf(stderr, "  skip line %d: parse error in assert: %s\n", line, wastro_parse_message);
                r = TR_SKIP;
            }
            if (r == TR_PASS) passed++;
            else if (r == TR_FAIL) failed++;
            else skipped++;
        }
        else if (tok_is_keyword("invoke")) {
            // Bare invoke — run on the active instance, ignore result.
            int fi; VALUE args[WASTRO_MAX_PARAMS]; uint32_t argc;
            if (loaded_any && !load_failed && active_ctx &&
                wast_parse_invoke(&fi, args, &argc)) {
                if (setjmp(wastro_trap_jmp) == 0) {
                    wastro_trap_active = 1;
                    wastro_invoke(active_ctx, fi, args, argc);
                    wastro_trap_active = 0;
                }
                else wastro_trap_active = 0;
            } else {
                while (cur_tok.kind != T_RPAREN) {
                    if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
                    else next_token();
                }
                expect_rparen();
            }
        }
        else if (tok_is_keyword("register")) {
            // (register "name") — ignored
            next_token();
            if (cur_tok.kind == T_STRING) next_token();
            if (cur_tok.kind == T_IDENT) next_token();
            expect_rparen();
        }
        else if (tok_is_keyword("assert_invalid") ||
                 tok_is_keyword("assert_malformed") ||
                 tok_is_keyword("assert_exhaustion") ||
                 tok_is_keyword("assert_unlinkable") ||
                 tok_is_keyword("assert_return_canonical_nan") ||
                 tok_is_keyword("assert_return_arithmetic_nan")) {
            // Forms we don't fully support — skip.
            next_token();
            while (cur_tok.kind != T_RPAREN) {
                if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
                else next_token();
            }
            expect_rparen();
            skipped++;
        }
        else {
            // Unknown form — skip.
            wast_skip_balanced();
            skipped++;
        }
    }

    if (active_ctx) {
        if (active_ctx->memory) free(active_ctx->memory);
        free(active_ctx);
    }
    fprintf(stderr, "\n%s: %d passed, %d failed, %d skipped\n",
            path, passed, failed, skipped);
    free(buf);
    return failed == 0 ? 0 : 1;
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
        // AOT compile both the bare body (for inner call_N callers) and
        // the entry adapter (for wastro_invoke).  Both reachable as SDs.
        astro_cs_compile(WASTRO_FUNCS[i].body, NULL);
        astro_cs_compile(WASTRO_FUNCS[i].entry, NULL);
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
        astro_cs_load(WASTRO_FUNCS[i].entry, NULL);
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

static CTX *
wastro_instantiate(uint32_t initial_local_slots)
{
    CTX *c = malloc(sizeof(CTX));
    c->fp = c->stack;
    c->sp = c->stack + initial_local_slots;
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
    for (uint32_t di = 0; di < MOD_DATA_SEG_CNT; di++) {
        struct wastro_data_seg *d = &MOD_DATA_SEGS[di];
        if (!MOD_HAS_MEMORY) wastro_die("(data ...) without (memory ...)");
        size_t mem_bytes = (size_t)c->memory_pages * WASTRO_PAGE_SIZE;
        if ((size_t)d->offset + d->length > mem_bytes) {
            wastro_trap("out of bounds memory access");
        }
        if (d->length == 0) continue;
        if (!c->memory) wastro_die("(data ...) into 0-page memory");
        memcpy(c->memory + d->offset, d->bytes, d->length);
    }
    return c;
}

// Invoke a wastro function with VALUE args.  argc must match the
// function's declared arity.  Used for (start) and for the export
// invocation from the driver.  Returns a plain VALUE — branch state
// is consumed at the function boundary (RESULT.br_depth treated as
// "implicit function-body label" exit).
static VALUE
wastro_invoke(CTX *c, int func_idx, VALUE *args, uint32_t argc)
{
    struct wastro_function *fn = &WASTRO_FUNCS[func_idx];
    if (argc != fn->param_cnt) {
        fprintf(stderr, "wastro_invoke: arity mismatch\n"); exit(1);
    }
    if (fn->is_import) return fn->host_fn(c, args, argc);
    uint32_t local_cnt = fn->local_cnt;
    VALUE F[local_cnt];
    for (uint32_t i = 0; i < argc; i++) F[i] = args[i];
    for (uint32_t i = argc; i < local_cnt; i++) F[i] = 0;
    // Dispatch via fn->entry — a node_function_frame wrapper whose AOT
    // specializer (wastro_gen.rb) emits the typed-struct adapter.  Plain
    // interp just forwards through to fn->body using F as VALUE[] frame.
    RESULT r = EVAL(c, fn->entry, F);
    return r.value;
}

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
        "  --clear-cs          delete code_store/ before starting\n"
        "\n"
        "If the module has a (start ...) function, it is invoked at\n"
        "instantiation time (before the user-named <export>).  If only\n"
        "<module.wat> is given and the module has (start), wastro\n"
        "instantiates and runs (start), then exits.\n");
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
    int test_mode = 0;           // --test
    while (ai < argc && argv[ai][0] == '-') {
        if (!strcmp(argv[ai], "-q") || !strcmp(argv[ai], "--quiet")) OPTION.quiet = true;
        else if (!strcmp(argv[ai], "-v") || !strcmp(argv[ai], "--verbose")) verbose = 1;
        else if (!strcmp(argv[ai], "--no-compile")) OPTION.no_compiled_code = true;
        else if (!strcmp(argv[ai], "-c")) compile_first = 1;
        else if (!strcmp(argv[ai], "--aot") || !strcmp(argv[ai], "--aot-compile")) aot_only_mode = 1;
        else if (!strcmp(argv[ai], "--clear-cs") || !strcmp(argv[ai], "--ccs")) clear_cs = 1;
        else if (!strcmp(argv[ai], "--test")) test_mode = 1;
        else if (!strcmp(argv[ai], "-h") || !strcmp(argv[ai], "--help")) usage();
        else { fprintf(stderr, "wastro: unknown option %s\n", argv[ai]); usage(); }
        ai++;
    }
    if (clear_cs) (void)system("rm -rf code_store");

    if (test_mode) {
        if (argc - ai < 1) { fprintf(stderr, "wastro: --test requires <foo.wast>\n"); usage(); }
        OPTION.no_compiled_code = true;   // pure-interpreter path for tests
        OPTION.quiet = true;
        INIT();
        return wastro_run_wast(argv[ai]);
    }

    if (aot_only_mode) {
        if (argc - ai < 1) usage();
    }
    else {
        // run: <module.wat> [<export> [arg ...]] — export optional if module has (start).
        if (argc - ai < 1) usage();
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

    int has_export_arg = (argc - ai >= 1);
    int func_idx = -1;
    if (has_export_arg) {
        const char *export_name = argv[ai++];
        func_idx = wastro_find_export(export_name);
        if (func_idx < 0) {
            fprintf(stderr, "wastro: export '%s' not found\n", export_name);
            return 1;
        }
    }
    else if (!MOD_HAS_START) {
        fprintf(stderr, "wastro: no <export> given and module has no (start)\n");
        usage();
    }

    // Allocate CTX with enough headroom for whichever function we
    // run first.  start fn (if any) goes through wastro_invoke; the
    // export invocation overwrites fp.
    uint32_t initial_locals = 0;
    if (MOD_HAS_START)             initial_locals = WASTRO_FUNCS[MOD_START_FUNC].local_cnt;
    if (func_idx >= 0 && WASTRO_FUNCS[func_idx].local_cnt > initial_locals)
        initial_locals = WASTRO_FUNCS[func_idx].local_cnt;
    CTX *c = wastro_instantiate(initial_locals);

    // Invoke (start) if present.
    if (MOD_HAS_START) {
        wastro_invoke(c, MOD_START_FUNC, NULL, 0);
    }

    if (func_idx < 0) return 0;   // start-only run

    struct wastro_function *fn = &WASTRO_FUNCS[func_idx];
    int provided = argc - ai;
    if ((uint32_t)provided != fn->param_cnt) {
        fprintf(stderr, "wastro: expects %u arg(s), got %d\n",
                fn->param_cnt, provided);
        return 1;
    }
    if (fn->is_import) {
        fprintf(stderr, "wastro: cannot directly invoke imported function\n");
        return 1;
    }
    VALUE args[WASTRO_MAX_PARAMS];
    for (uint32_t i = 0; i < fn->param_cnt; i++) {
        const char *s = argv[ai + i];
        switch (fn->param_types[i]) {
        case WT_I32: args[i] = FROM_I32((int32_t)strtol(s, NULL, 0)); break;
        case WT_I64: args[i] = FROM_I64((int64_t)strtoll(s, NULL, 0)); break;
        case WT_F32: args[i] = FROM_F32((float)strtod(s, NULL)); break;
        case WT_F64: args[i] = FROM_F64(strtod(s, NULL)); break;
        default:     args[i] = 0;
        }
    }
    VALUE result = wastro_invoke(c, func_idx, args, fn->param_cnt);
    switch (fn->result_type) {
    case WT_I32: printf("%d\n",        (int)AS_I32(result)); break;
    case WT_I64: printf("%lld\n", (long long)AS_I64(result)); break;
    case WT_F32: printf("%g\n",     (double)AS_F32(result)); break;
    case WT_F64: printf("%g\n",             AS_F64(result)); break;
    default:     break;  // void
    }
    return 0;
}
