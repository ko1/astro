#ifndef ASTRO_HOLE_H
#define ASTRO_HOLE_H

// Holes (docs/idea_code_store.md §7): site-specific values an SD template
// used to read from the NODE tree (symbol IDs, line numbers, cache addresses,
// child NODE pointers ...).  In pool mode each SD *instance* owns a table of
// hole values (`n->head.pool`), filled once at load by the generated
// SD_<h>_fill and read by the SD as P[k].
//
// The element type is deliberately NOT uintptr_t / VALUE: on LP64 `unsigned
// long long` is a distinct TBAA class from VALUE (long), so stores into the slot
// area never force P[k] to be reloaded (on wasm32 VALUE is `long long` and the
// two may alias — only the optimisation is lost, never correctness); and it is
// 64-bit on wasm32 as well, so a Symbol VALUE (id << 4 | tag) always fits.

#include <stdint.h>

typedef unsigned long long astro_hole_t;

struct Node;
// Generated per public SD: fills `pool` (when non-NULL) for the tree rooted
// at `n` and returns the hole count.
typedef uint32_t (*astro_pool_fill_t)(const struct Node *n, astro_hole_t *pool);
// Build n's pool with `fill` and install it (astro_code_store.c).  Pools are
// immortal: an activation of an older SD generation may still hold P.
void astro_cs_pool_attach(struct Node *n, astro_pool_fill_t fill);

#ifdef ASTRO_SD_POOL
// Inside an SD translation unit `P` is the pool of the enclosing public SD
// (root: loaded from n->head.pool; inline SDs receive `P + offset`).
#define ASTRO_POOL_PARAM astro_hole_t const *restrict const P
#define HOLE_U32(k) ((uint32_t)P[k])
#define HOLE_I32(k) ((int32_t)(uint32_t)P[k])
#define HOLE_U64(k) ((uint64_t)P[k])
#define HOLE_PTR(k) ((void *)(uintptr_t)P[k])
#endif

#endif
