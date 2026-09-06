#ifndef ASTRO_HOLE_ARCH_NONE_H
#define ASTRO_HOLE_ARCH_NONE_H

// No loader backend for this architecture: every body keeps running on its
// pool-mode dispatcher (portable C, correct everywhere).  astro_cs_instantiate
// reports "unsupported" and the store never builds op/ objects.
//
// To add one, copy hole/arch_x86_64.h and fill in the contract in hole/arch.h.

#define ASTRO_ARCH_SUPPORTED 0
#define ASTRO_ARCH_NAME      "portable (pool only)"

#endif
