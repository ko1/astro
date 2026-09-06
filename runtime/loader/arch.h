// Architecture backend for the loader path (docs/idea_code_store.md §7).
//
// The pool path is portable C and needs nothing from here.  The loader path —
// copy an SD's object per AST node and write the hole values into the code as
// immediates — is architecture work, and this header is the seam: everything
// that knows about an instruction set lives in one `arch_<isa>.h`, everything
// else (ELF reading, the arena, symbol resolution, the hot policy) is shared.
//
// A backend supplies two halves:
//
//   compile side (in the SD translation unit, under ASTRO_SD_PATCH)
//     ASTRO_ARCH_HOLE_IMM(k)   materialise hole k as an immediate whose
//                              relocation carries k as its addend, in a form
//                              the compiler cannot fold into another hole's
//                              (see arch_x86_64.h for why that matters)
//     ASTRO_ARCH_CFLAGS        flags the store must use for op/*.o so the
//                              holes survive as relocations (code model, no
//                              PIC/PLT indirection, no jump tables, ...)
//
//   load side (in the loader)
//     ASTRO_ARCH_ELF_MACHINE   EM_* this backend accepts
//     ASTRO_ARCH_ARENA_LO/HI   address window the executable view must sit in
//                              (0/0 = anywhere).  Set it when the code model
//                              addresses data with a limited-width immediate.
//     astro_arch_reloc_width(t)      bytes a relocation of type t writes (0 = unsupported)
//     astro_arch_reloc_needs_got(t)  true if t resolves through a GOT slot
//     astro_arch_reloc_apply(...)    write one relocation; false = out of range
//
// A backend that cannot do this yet sets ASTRO_ARCH_SUPPORTED 0: the loader
// then reports "unsupported" for every node and every body keeps running on
// its pool-mode dispatcher, which is always correct — the loader is an
// optimisation, never a requirement.

// No outer include guard on purpose: this header is included once for the
// compile side (macros) and again by the loader with ASTRO_LOADER_IMPL defined
// for the load side.  Each arch header guards its two phases separately.

// ASTRO_ARCH_FORCE_NONE builds as if this machine had no backend — the way a
// newly-added architecture starts, and the cheapest way to check that the
// pool-only fallback still works on a machine that does have one.
#if defined(ASTRO_ARCH_FORCE_NONE)
#  include "loader/arch_none.h"
#elif defined(__x86_64__)
#  include "loader/arch_x86_64.h"
#elif defined(__aarch64__)
#  include "loader/arch_aarch64.h"
#else
#  include "loader/arch_none.h"
#endif

#ifndef ASTRO_ARCH_DEFAULTS_DONE
#define ASTRO_ARCH_DEFAULTS_DONE
#ifndef ASTRO_ARCH_SUPPORTED
#define ASTRO_ARCH_SUPPORTED 0
#endif
#ifndef ASTRO_ARCH_NAME
#define ASTRO_ARCH_NAME "unknown"
#endif
#ifndef ASTRO_ARCH_ARENA_LO
#define ASTRO_ARCH_ARENA_LO 0
#endif
#ifndef ASTRO_ARCH_ARENA_HI
#define ASTRO_ARCH_ARENA_HI 0
#endif
#ifndef ASTRO_ARCH_GOT_SLOT
#define ASTRO_ARCH_GOT_SLOT 8
#endif
#endif
