// wastro — WebAssembly 1.0 (MVP) on ASTro.
//
// This translation unit is the wastro driver.  The front-end has been
// split across several `#include`'d .c files for navigability — see
// `docs/runtime.md` for the architecture overview.  Layout:
//
//   main.c               preamble, traps, linear memory + SIGSEGV,
//                        module-state declarations, AOT compile
//                        loop, CLI entry / `main()`
//   wat_tokenizer.c      lexer + parse-error helpers + token-to-bits
//                        converters used by both WAT and .wast paths
//   host_imports.c       `env.*` / `spectest.*` host registry
//   wat_parser.c         folded-S-expr + (export/import) inline
//                        helpers + stack-style + (func ...) parser
//   wasm_decoder.c       binary `.wasm` decoder
//   wast_runner.c        spec-test (`.wast`) harness
//
// Generated dispatchers live in node_*.c (built from node.def by
// ASTroGen via wastro_gen.rb), pulled in through node.c.

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <signal.h>
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
// Linear memory: virtual reservation + guard-page bounds checking
// =====================================================================
//
// Wasm 1.0 addresses are u32 plus a u32 offset, so the worst-case
// effective address fits in 33 bits.  We mmap an 8 GB region per CTX
// at PROT_NONE up front; the pages actually exposed to wasm are flipped
// to PROT_READ|PROT_WRITE via mprotect (initial pages at module load,
// extras via memory.grow).  Anything past the live region is
// PROT_NONE, so an OOB load/store from generated code triggers
// SIGSEGV — a signal handler converts that into wastro_trap.
//
// Net effect: every wasm load/store skips the explicit bounds compare
// the interpreter used to do.  Spec compliance is preserved (OOB still
// traps) and the hot path matches wasmtime — which uses the same trick.

#define WASTRO_VM_RESERVE_BYTES (8ULL * 1024 * 1024 * 1024)  // 8 GB

// Single-CTX assumption: wastro runs one module at a time on one
// thread, so a global is fine.  Set right after CTX construction.
static CTX *wastro_segv_ctx = NULL;

static void
wastro_segv_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)sig; (void)ucontext;
    if (wastro_segv_ctx && wastro_segv_ctx->memory) {
        uintptr_t base = (uintptr_t)wastro_segv_ctx->memory;
        uintptr_t fault = (uintptr_t)info->si_addr;
        if (fault >= base && fault < base + WASTRO_VM_RESERVE_BYTES) {
            wastro_trap("out of bounds memory access");  // longjmps if active
            // If no jmp set, wastro_trap exits — never returns.
        }
    }
    // Not from our wasm memory — restore default handler and re-raise so
    // the original SIGSEGV (real bug) surfaces with its core dump.
    struct sigaction dfl;
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    dfl.sa_flags = 0;
    sigaction(SIGSEGV, &dfl, NULL);
}

static void
wastro_install_segv_handler(void)
{
    static int installed = 0;
    if (installed) return;
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = wastro_segv_handler;
    sigaction(SIGSEGV, &sa, NULL);
    installed = 1;
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

#include "host_imports.c"
#include "wat_parser.c"
#include "wasm_decoder.c"
#include "wast_runner.c"

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
        astro_cs_compile(WASTRO_FUNCS[i].body, NULL);
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

static CTX *
wastro_instantiate(uint32_t initial_local_slots)
{
    CTX *c = malloc(sizeof(CTX));
    c->fp = c->stack;
    c->sp = c->stack + initial_local_slots;
    if (MOD_HAS_MEMORY) {
        // Reserve 8 GB virtual at PROT_NONE; mprotect the initial pages
        // R/W, leaving the rest as a guard region that catches OOB via
        // SIGSEGV.  Pages stay zero-filled lazily by the kernel on first
        // access, so we don't need calloc/memset here.
        c->memory = mmap(NULL, WASTRO_VM_RESERVE_BYTES, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (c->memory == MAP_FAILED) {
            fprintf(stderr, "wastro: mmap %llu bytes failed: %s\n",
                    (unsigned long long)WASTRO_VM_RESERVE_BYTES, strerror(errno));
            exit(1);
        }
        size_t bytes = (size_t)MOD_MEM_INITIAL_PAGES * WASTRO_PAGE_SIZE;
        if (bytes && mprotect(c->memory, bytes, PROT_READ | PROT_WRITE) != 0) {
            fprintf(stderr, "wastro: mprotect failed: %s\n", strerror(errno));
            exit(1);
        }
        c->memory_pages = MOD_MEM_INITIAL_PAGES;
        c->memory_max_pages = MOD_MEM_MAX_PAGES;
        c->memory_size_bytes = (uint64_t)MOD_MEM_INITIAL_PAGES * WASTRO_PAGE_SIZE;
    } else {
        c->memory = NULL;
        c->memory_pages = 0;
        c->memory_max_pages = 0;
        c->memory_size_bytes = 0;
    }
    wastro_segv_ctx = c;
    wastro_install_segv_handler();
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
    union wastro_slot F[local_cnt];
    for (uint32_t i = 0; i < argc; i++) F[i].raw = args[i];
    for (uint32_t i = argc; i < local_cnt; i++) F[i].raw = 0;
    RESULT r = EVAL(c, fn->body, F);
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
