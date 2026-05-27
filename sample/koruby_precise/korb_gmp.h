/* korb_gmp.h — selects between system GMP and bundled mini-gmp.
 *
 * By default we use the system GMP (`-lgmp`).  Define KORB_USE_MINI_GMP
 * (typically `make MINI_GMP=1`) to use the in-tree vendor/mini-gmp/
 * sources instead — useful when targeting wasm, embedded, or any
 * environment without GMP installed.  The two are ABI-incompatible so
 * the choice is made once at compile time. */

#ifndef KORB_GMP_H
#define KORB_GMP_H

#ifdef KORB_USE_MINI_GMP
# include "vendor/mini-gmp/mini-gmp.h"
#else
# include <gmp.h>
#endif

#endif
