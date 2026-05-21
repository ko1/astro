#ifndef ASTRO_CODE_STORE_H
#define ASTRO_CODE_STORE_H

// ASTro Code Store
//
// Manages specialized dispatchers: lookup from shared objects,
// generate specialized C source, and build into all.so.
//
// Requires: NODE, node_hash_t, node_dispatcher_func_t
// defined before including this header.

#include <stdbool.h>

// Initialize code store and load all.so from store_dir (if it exists).
// src_dir: directory containing node.h, node_eval.c etc. (used for #include in generated .c)
//          Can be overridden by ASTRO_CS_SRC_DIR environment variable.
// version: cache version (e.g., mtime of host binary). 0 to skip version check.
//          If changed from stored version, code store is cleared and rebuilt.
void astro_cs_init(const char *store_dir, const char *src_dir, uint64_t version);

// Look up specialized code for node's hash in code store.
// If found, replaces the node's dispatcher and returns true.
// `file` (nullable) is the source filename of the entry — needed for
// PGC (Hopt) lookup.  Pass NULL to force AOT-only (SD_<Horg>) lookup.
bool astro_cs_load(NODE *n, const char *file);

// Generate specialized C source for entry node.
//   file == NULL: AOT — writes <store_dir>/c/SD_<Horg>.c
//   file != NULL: PGC — writes <store_dir>/c/SD_<Hopt>.c and appends
//                 (Horg, file, line) → Hopt to hopt_index.txt
void astro_cs_compile(NODE *entry, const char *file);

// Build all SD_*.c in store_dir into all.so (make -j).
// extra_cflags: additional compiler flags (e.g., Ruby include paths). Can be NULL.
void astro_cs_build(const char *extra_cflags);

// Reload all.so (dlclose + dlopen). Use after build to apply immediately.
void astro_cs_reload(void);

// Print disassembly of the specialized dispatcher for node (via objdump).
// Does nothing if the node is not specialized.
void astro_cs_disasm(NODE *n);

// ---------------------------------------------------------------------------
// Static (linker-resolved) code store — used by `--generate-executable`.
//
// In dlopen mode the code store finds SD_<hash> via dlsym at runtime.  In
// static mode (= the exe build path), the SD functions are linked directly
// into the exe and a static table maps hash → function pointer.
// astro_cs_load consults this table BEFORE falling back to the dlopen
// path, so a hybrid build (some static + some dlopen) works seamlessly.
//
// `name` is optional metadata for diagnostics (objdump / --disasm).  Pass
// NULL if not needed.
// ---------------------------------------------------------------------------

struct astro_cs_static_entry {
    node_hash_t hash;
    node_dispatcher_func_t func;
    const char *name;          // "SD_<hash>" or NULL
};

// Register a static SD table.  `n` is the number of entries.  Subsequent
// astro_cs_load calls consult this table first.  Safe to call multiple
// times (entries accumulate); pass NULL to clear (n is ignored).
void astro_cs_static_init(struct astro_cs_static_entry *table, size_t n);

// Emit a complete C source file defining the static SD lookup table.
// Each call to astro_cs_compile(entry, NULL) registers the entry's hash
// internally; this function reads that list, writes extern decls for
// every SD symbol and a sentinel-terminated static_entry array.
//
//   sd_proto_macro — preprocessor name expanding to an SD declaration
//                    with one argument (the function name).  E.g.:
//                       #define ASTRO_SD_PROTO(N) RESULT N(CTX *, NODE *, VALUE *)
//                    The host writes this macro before including the
//                    emitted file (typically via the host's node.h).
//                    Pass NULL to use the default name "ASTRO_SD_PROTO".
//
// The emitted file looks like:
//
//      ASTRO_SD_PROTO(SD_abcd);
//      ASTRO_SD_PROTO(SD_ef01);
//      struct astro_cs_static_entry astro_cs_static_table[] = {
//          { 0xabcd, (node_dispatcher_func_t)SD_abcd, "SD_abcd" },
//          { 0xef01, (node_dispatcher_func_t)SD_ef01, "SD_ef01" },
//          { 0, 0, 0 },
//      };
//      size_t astro_cs_static_table_size = 2;
//
// The exe build links this file alongside the SD_*.o files; the exe
// driver calls astro_cs_static_init(astro_cs_static_table,
// astro_cs_static_table_size) right after astro_cs_init.
void astro_cs_emit_static_table(FILE *fp, const char *sd_proto_macro);

// Reset the per-compile registry of static SD entries (call between
// compile sessions).  Idempotent.
void astro_cs_reset_static_registry(void);

#endif
