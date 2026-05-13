#ifndef ASTRO_DEBUG_H
#define ASTRO_DEBUG_H 1

// ASTro debug assertion macro.
//
// ASTRO_ASSERT(expr) is the framework-wide drop-in for the C library
// assert().  Define ASTRO_DEBUG=1 at compile time to enable checks
// (typically with -DASTRO_DEBUG=1 in CFLAGS, or `#define ASTRO_DEBUG 1`
// before including this header).  Otherwise checks compile out to nothing.
//
// Use ASTRO_ASSERT for invariants you want to verify cheaply during
// development.  For runtime-conditional diagnostics (e.g. stress-mode
// checks that only fire when an env var is set), wrap the entire block
// in `if (ASTRO_DEBUG && runtime_flag) { ... }` — the compiler folds
// the leading constant and drops the block under !ASTRO_DEBUG.

#ifndef ASTRO_DEBUG
#  define ASTRO_DEBUG 0
#endif

#if ASTRO_DEBUG
#  include <assert.h>
#  define ASTRO_ASSERT(expr) assert(expr)
#else
#  define ASTRO_ASSERT(expr) ((void)0)
#endif

#endif
