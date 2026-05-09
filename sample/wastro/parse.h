#ifndef WASTRO_PARSE_H
#define WASTRO_PARSE_H 1

#include <stdint.h>
#include "context.h"

// =====================================================================
// Public parser entries
// =====================================================================

// Load a .wat or .wasm module from disk.  Populates WASTRO_FUNCS[] and
// the module-level state below.
NODE *wastro_load_module(const char *path);

// Load a binary .wasm module from a memory buffer.
NODE *wastro_load_module_buf(const char *buf, size_t sz);

// Run a .wast spec-test file (inline harness).  Returns process exit
// status (0 = all pass).
int wastro_run_wast(const char *path);

// Find an exported function by name; returns -1 if not found.
int wastro_find_export(const char *name);

// Trap from generated code — installed by main.c's segv handler too.
__attribute__((noreturn)) void wastro_trap(const char *msg);

// Linear-memory reservation: 8 GB virtual at PROT_NONE per CTX.
// Defined here so the .wast harness in parse.c can munmap a CTX's
// memory using the same value as the driver in main.c.
#define WASTRO_VM_RESERVE_BYTES (8ULL * 1024ULL * 1024ULL * 1024ULL)

// Single-CTX assumption: wastro runs one module at a time on one
// thread.  Set by main.c's wastro_instantiate; cleared by the .wast
// harness when it tears down a CTX between modules.
extern CTX *wastro_segv_ctx;
void wastro_install_segv_handler(void);

// Driver entries called by the .wast harness so it can spin up a CTX
// per test module and invoke its functions through the same path the
// CLI uses.
CTX  *wastro_instantiate(uint32_t initial_local_slots);
VALUE wastro_invoke(CTX *c, int func_idx, VALUE *args, uint32_t argc);

// =====================================================================
// Module state — populated by the parser, consumed by the driver.
// =====================================================================

// Memory declaration.
extern int      MOD_HAS_MEMORY;
extern uint32_t MOD_MEM_INITIAL_PAGES;
extern uint32_t MOD_MEM_MAX_PAGES;

// (start ...) function (-1 if none).
extern int MOD_HAS_START;
extern int MOD_START_FUNC;

// Data segments captured during parse.
struct wastro_data_seg {
    uint32_t offset;
    uint32_t length;
    uint8_t *bytes;
};
#define WASTRO_MAX_DATA_SEGS 64
extern struct wastro_data_seg MOD_DATA_SEGS[WASTRO_MAX_DATA_SEGS];
extern uint32_t MOD_DATA_SEG_CNT;

#endif // WASTRO_PARSE_H
