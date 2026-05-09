// wastro — WebAssembly 1.0 (MVP) on ASTro.
//
// This translation unit is the wastro driver: CLI parsing, linear
// memory mapping with SIGSEGV-based bounds checking, AOT compile loop,
// module instantiation, function invocation, and `main()`.  The
// front-end (tokenizer / WAT parser / .wasm decoder / .wast harness
// + the parser-managed module state) lives in parse.c — see
// `docs/runtime.md` for the architecture overview.

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include "context.h"
#include "node.h"
#include "parse.h"
#include "astro_code_store.h"

struct wastro_option OPTION;

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

// Single-CTX assumption: wastro runs one module at a time on one
// thread, so a global is fine.  Set right after CTX construction.
// Declared in parse.h so the .wast harness can null it out when it
// tears down a CTX between modules.
CTX *wastro_segv_ctx = NULL;

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

void
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

CTX *
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
        if (!MOD_HAS_MEMORY) {
            fprintf(stderr, "wastro: (data ...) without (memory ...)\n");
            exit(1);
        }
        size_t mem_bytes = (size_t)c->memory_pages * WASTRO_PAGE_SIZE;
        if ((size_t)d->offset + d->length > mem_bytes) {
            wastro_trap("out of bounds memory access");
        }
        if (d->length == 0) continue;
        if (!c->memory) {
            fprintf(stderr, "wastro: (data ...) into 0-page memory\n");
            exit(1);
        }
        memcpy(c->memory + d->offset, d->bytes, d->length);
    }
    return c;
}

// Invoke a wastro function with VALUE args.  argc must match the
// function's declared arity.  Used for (start) and for the export
// invocation from the driver.  Returns a plain VALUE — branch state
// is consumed at the function boundary (RESULT.br_depth treated as
// "implicit function-body label" exit).
VALUE
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
