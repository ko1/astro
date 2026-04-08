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
bool astro_cs_load(NODE *n);

// Generate specialized C source for entry node.
// Writes <store_dir>/SD_<hash>.c
void astro_cs_compile(NODE *entry);

// Build all SD_*.c in store_dir into all.so (make -j).
// extra_cflags: additional compiler flags (e.g., Ruby include paths). Can be NULL.
void astro_cs_build(const char *extra_cflags);

// Reload all.so (dlclose + dlopen). Use after build to apply immediately.
void astro_cs_reload(void);

// Print disassembly of the specialized dispatcher for node (via objdump).
// Does nothing if the node is not specialized.
void astro_cs_disasm(NODE *n);

#endif
