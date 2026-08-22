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
#include <stdint.h>

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

// Compile SD_*.c → o/*.o in parallel without linking all.so — for exe builds
// that link the objects themselves.  Environment CC / CFLAGS override the
// store Makefile's defaults (cross builds substitute their toolchain here).
void astro_cs_build_objs(const char *extra_cflags);

// Reload all.so (dlclose + dlopen). Use after build to apply immediately.
void astro_cs_reload(void);

// Register a secondary "preload" shared object whose SD_<hash> symbols are
// searched as a fallback when the primary all.so does not provide them.  This
// lets an embedder host a fixed prelude's specialized dispatchers in their own
// .so so they need not be re-baked into every program's code store.  Opt-in:
// samples that never call this are entirely unaffected.  `path` NULL clears the
// handle; a dlopen failure simply leaves no preload handle (the SDs are then
// not found, exactly as before).
void astro_cs_set_preload(const char *path);

// ---------------------------------------------------------------------------
// Static SD table (dlopen-free hosts, e.g. wasm32-wasip1)
// ---------------------------------------------------------------------------
//
// Instead of dlopen'ing all.so, a build may link the SD_*.c directly into the
// program together with one generated translation unit that defines these two
// functions.  astro_cs_load then resolves through the table, so the AOT flow is
// unchanged apart from *when* the C compiler runs: the SD sources are emitted
// by an earlier run (astro_cs_compile writes plain files — no toolchain needed)
// and compiled by the host build, not by astro_cs_build.
//
// Both have weak no-table defaults, so a build that links no generated table
// behaves exactly as before.
//
//   lookup: "SD_<hash>"/"PGSD_<hash>" → dispatcher, or NULL if absent.
//   count:  number of entries; 0 means "no static table linked".

node_dispatcher_func_t astro_cs_static_sd_lookup(const char *sym);
uint32_t               astro_cs_static_sd_count(void);

// Print disassembly of the specialized dispatcher for node (via objdump).
// Does nothing if the node is not specialized.
void astro_cs_disasm(NODE *n);

// ---------------------------------------------------------------------------
// Per-process SD compile log (--generate-executable support)
// ---------------------------------------------------------------------------
//
// When `astro_cs_log_compiles` is true, every astro_cs_compile call —
// whether it freshly emits the SD source or hits the on-disk cache —
// records `(hash, SD_<hash>)` in an internal log.  The exe-build
// helper iterates this log to know which SD_*.c files to link and
// which SD symbols to bake into the embedded AST's dispatcher slots.
//
// Off by default (recording costs memory and is irrelevant for the
// REPL / JIT / dlopen paths).  astro_build_aot_executable turns it
// on internally before its bake pass.

extern bool astro_cs_log_compiles;

uint32_t astro_cs_compile_log_size(void);
void     astro_cs_compile_log_get(uint32_t i,
                                  node_hash_t *out_hash,
                                  const char **out_name);
void     astro_cs_reset_compile_log(void);

#endif
