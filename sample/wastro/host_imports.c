// wastro — built-in host import registry (env.* and spectest.*)
//
// `#include`'d from main.c.  Each entry is a static C function with
// the standard wastro_host_fn_t signature plus a const table entry
// the parsers consult during `(import "...")` resolution.

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
