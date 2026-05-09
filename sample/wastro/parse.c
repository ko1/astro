// wastro — parser side (WAT + WASM + .wast harness)
//
// Owns: trap / parse-error recovery, the module-state singletons
// (functions, globals, types, table, br_table, data segments, etc.),
// per-function parse helpers (alloc_local_*, fixups), and the
// tokenizer / parser / decoder / .wast runner sub-files (included
// here so they can share file-static state).
//
// main.c is the driver: it parses CLI options, calls into here to
// load the module, then runs the export.

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include "context.h"
#include "node.h"
#include "parse.h"

// =====================================================================
// Parse-error / trap recovery
// =====================================================================

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

#include "wat_tokenizer.c"

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

// Pick the right typed `local.get` / `local.set` / `local.tee` node
// kind for a wasm local based on its declared type.  Used everywhere
// the parser emits a local op so the AOT codegen sees a typed slot
// access at every call site (frame[idx].i32 / .f64 / etc.) and gcc
// can SROA the slot at its real C type.
static NODE *
alloc_local_get(wtype_t t, uint32_t index)
{
    switch (t) {
    case WT_I32: return ALLOC_node_local_get_i32(index);
    case WT_I64: return ALLOC_node_local_get_i64(index);
    case WT_F32: return ALLOC_node_local_get_f32(index);
    case WT_F64: return ALLOC_node_local_get_f64(index);
    default:     return ALLOC_node_local_get_i32(index);  // shouldn't happen
    }
}

static NODE *
alloc_local_set(wtype_t t, uint32_t index, NODE *expr)
{
    switch (t) {
    case WT_I32: return ALLOC_node_local_set_i32(index, expr);
    case WT_I64: return ALLOC_node_local_set_i64(index, expr);
    case WT_F32: return ALLOC_node_local_set_f32(index, expr);
    case WT_F64: return ALLOC_node_local_set_f64(index, expr);
    default:     return ALLOC_node_local_set_i32(index, expr);
    }
}

static NODE *
alloc_local_tee(wtype_t t, uint32_t index, NODE *expr)
{
    switch (t) {
    case WT_I32: return ALLOC_node_local_tee_i32(index, expr);
    case WT_I64: return ALLOC_node_local_tee_i64(index, expr);
    case WT_F32: return ALLOC_node_local_tee_f32(index, expr);
    case WT_F64: return ALLOC_node_local_tee_f64(index, expr);
    default:     return ALLOC_node_local_tee_i32(index, expr);
    }
}

// Pending body-slot fix-up for node_call_N nodes.  At allocation
// time the callee's body may not be parsed yet (forward reference);
// every call site is appended here and patched in one post-parse
// sweep so that the specializer can recurse from caller into callee
// via the body slot.
// `arity` encodes which node_call_* kind owns this fixup: 0..4 for the
// fixed-arity variants, PENDING_ARITY_VAR for `node_call_var`.
#define PENDING_ARITY_VAR 0xFF

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
        case PENDING_ARITY_VAR: p->call_node->u.node_call_var.body = body; break;
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
uint32_t MOD_MEM_INITIAL_PAGES = 0;
uint32_t MOD_MEM_MAX_PAGES = 65536;
int      MOD_HAS_MEMORY = 0;

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
int   MOD_HAS_START = 0;
int   MOD_START_FUNC = -1;     // resolved after scan_module

// Data segments — written to memory at instantiation.
struct wastro_data_seg MOD_DATA_SEGS[WASTRO_MAX_DATA_SEGS];
uint32_t MOD_DATA_SEG_CNT = 0;


// Module-state lookup helpers — search WASTRO_FUNCS by export-name or
// by `$name` / numeric reference.  Used from both WAT parser, .wasm
// decoder, .wast harness, and the top-level driver.
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

// Per-function / per-signature param/local storage helpers.  The
// arrays in `wastro_function` and `wastro_type_sig` are heap pointers
// (NULL when empty); these helpers grow / replace them on demand.
//
// Usage idioms:
//   - bulk write from a known-size source:
//        func_set_params(fn, sig->param_cnt);
//        memcpy(fn->param_types, sig->param_types, ...);
//   - incremental write inside a parse loop:
//        func_ensure_params(fn, k + 1);
//        fn->param_types[k] = t;
//        ...; fn->param_cnt = k_total;

static wtype_t *
wtype_alloc(uint32_t cnt)
{
    return cnt ? (wtype_t *)calloc(cnt, sizeof(wtype_t)) : NULL;
}

static void
func_set_params(struct wastro_function *fn, uint32_t cnt)
{
    free(fn->param_types);
    fn->param_cnt = cnt;
    fn->param_types = wtype_alloc(cnt);
}

static void
func_set_locals(struct wastro_function *fn, uint32_t cnt)
{
    free(fn->local_types);
    fn->local_cnt = cnt;
    fn->local_types = wtype_alloc(cnt);
}

static void
sig_set_params(struct wastro_type_sig *sig, uint32_t cnt)
{
    free(sig->param_types);
    sig->param_cnt = cnt;
    sig->param_types = wtype_alloc(cnt);
}

// Grow an array (allocated by wtype_alloc) to at least `need` entries.
// Used when params/locals are appended one at a time inside a loop.
// `*cap` tracks the high-water mark distinct from the public count.
static void
wtype_grow(wtype_t **arr, uint32_t *cap, uint32_t need)
{
    if (need <= *cap) return;
    uint32_t capa = *cap ? *cap : 8;
    while (capa < need) capa *= 2;
    *arr = (wtype_t *)realloc(*arr, capa * sizeof(wtype_t));
    if (!*arr) {
        fprintf(stderr, "wastro: out of memory growing wtype array\n");
        exit(1);
    }
    *cap = capa;
}

// Same growing pattern but for arrays of NODE* (call argument
// sub-trees collected during parsing).  Capacity doubles on overflow.
static void
node_args_grow(NODE ***arr, uint32_t *cap, uint32_t need)
{
    if (need <= *cap) return;
    uint32_t capa = *cap ? *cap : 8;
    while (capa < need) capa *= 2;
    *arr = (NODE **)realloc(*arr, capa * sizeof(NODE *));
    if (!*arr) {
        fprintf(stderr, "wastro: out of memory growing NODE* array\n");
        exit(1);
    }
    *cap = capa;
}

// Module-global storage for the operand AST sub-trees of variable-
// arity call nodes (`node_call_var` / `node_call_indirect_var` /
// `node_host_call_var`).  Mirrors the WASTRO_BR_TABLE pattern: each
// var-call node carries `(args_index, args_cnt)` into this flat
// array, which is grown as the parser registers call sites.  Freed
// on module reset.
NODE   **WASTRO_CALL_ARGS = NULL;
uint32_t WASTRO_CALL_ARGS_CNT = 0;
static uint32_t WASTRO_CALL_ARGS_CAP = 0;

static uint32_t
wastro_register_call_args(NODE **args, uint32_t cnt)
{
    if (WASTRO_CALL_ARGS_CNT + cnt > WASTRO_CALL_ARGS_CAP) {
        uint32_t need = WASTRO_CALL_ARGS_CNT + cnt;
        uint32_t capa = WASTRO_CALL_ARGS_CAP ? WASTRO_CALL_ARGS_CAP : 64;
        while (capa < need) capa *= 2;
        WASTRO_CALL_ARGS = (NODE **)realloc(WASTRO_CALL_ARGS, capa * sizeof(NODE *));
        if (!WASTRO_CALL_ARGS) wastro_die("out of memory growing WASTRO_CALL_ARGS");
        WASTRO_CALL_ARGS_CAP = capa;
    }
    uint32_t base = WASTRO_CALL_ARGS_CNT;
    for (uint32_t i = 0; i < cnt; i++) WASTRO_CALL_ARGS[base + i] = args[i];
    WASTRO_CALL_ARGS_CNT += cnt;
    return base;
}

static void
wastro_reset_call_args(void)
{
    free(WASTRO_CALL_ARGS);
    WASTRO_CALL_ARGS = NULL;
    WASTRO_CALL_ARGS_CNT = 0;
    WASTRO_CALL_ARGS_CAP = 0;
}

#include "host_imports.c"
#include "wat_parser.c"
#include "wasm_decoder.c"
#include "wast_runner.c"
