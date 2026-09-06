#ifndef ASTRO_HOLE_ARCH_AARCH64_H
#define ASTRO_HOLE_ARCH_AARCH64_H

// aarch64 backend — NOT IMPLEMENTED YET.  Declaring it unsupported keeps every
// body on the pool path (correct, portable); this file exists to hold the
// findings needed to write it, so the work starts from a spec rather than a
// blank page.
//
// What a backend has to solve here (differs from x86-64 in every point):
//
//  1. Immediates.  There is no movabs: a 64-bit constant is MOVZ + 3×MOVK, and
//     the relocations are R_AARCH64_MOVW_UABS_G0_NC .. _G3.  The hole number
//     therefore arrives as FOUR relocations against the same symbol+addend, and
//     the loader must patch the imm16 field of each instruction rather than a
//     contiguous byte range.  ASTRO_ARCH_HOLE_IMM must emit that sequence in a
//     form the compiler will not CSE across holes (the x86 lesson).
//     A cheaper alternative worth measuring first: keep the value in the
//     instance's own literal pool and let the code LDR it (one relocation,
//     R_AARCH64_LD_PREL_LO19) — that is halfway between pool and immediate.
//  2. Calls.  BL reaches ±128 MB (R_AARCH64_CALL26/JUMP26).  Either place the
//     arena within ±128 MB of the host text, or route every host call through
//     the per-instance GOT with ADRP+LDR (R_AARCH64_ADR_GOT_PAGE +
//     LD64_GOT_LO12_NC), which is the portable choice and matches what the
//     x86-64 backend already does with -fno-plt.
//  3. Data addressing.  ADRP+ADD (R_AARCH64_ADR_PREL_PG_HI21 + ADD_ABS_LO12_NC)
//     reaches ±4 GB from the instruction, so .rodata inside the same chunk is
//     fine and no low-address arena window is needed: ARENA_LO/HI stay 0.
//  4. I-cache.  Unlike x86, the two views are NOT coherent: after patching an
//     instance the backend must run `dc cvau` / `dsb ish` / `ic ivau` / `isb`
//     over the copied range (__builtin___clear_cache covers this) before the
//     dispatcher pointer is published.  astro_arch_sync_icache is that hook.
//  5. Build flags.  -fno-plt has no meaning here; the equivalents are
//     -fno-pic (or -mcmodel=large with care) and -fno-jump-tables.
//
// Test plan when it lands: the loader fault-injection suite plus the AOT
// differential (interp / pool / loader-all / loader-hot must agree) from
// trials/2026-09-06-koruby-precise-perf/src/.

#define ASTRO_ARCH_SUPPORTED 0
#define ASTRO_ARCH_NAME      "aarch64 (pool only — loader backend not written)"

#endif
