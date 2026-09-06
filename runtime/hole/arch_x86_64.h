// Two phases, two guards: constants + compile-side macros on first inclusion,
// the relocation applier when the loader includes us with ASTRO_LOADER_IMPL.
#ifndef ASTRO_HOLE_ARCH_X86_64_H
#define ASTRO_HOLE_ARCH_X86_64_H

// x86-64 backend for the loader path.  See hole/arch.h for the contract.

#define ASTRO_ARCH_SUPPORTED 1
#define ASTRO_ARCH_NAME      "x86-64"

// op/*.o build flags.  -mcmodel=medium makes the compiler address the hole
// symbol with a 64-bit immediate (movabs) instead of a RIP-relative form, so
// the value can be patched per instance; -fno-pic/-fno-plt keep calls and data
// references direct; -fno-jump-tables keeps .rodata free of code addresses that
// would need their own relocation pass.
#define ASTRO_ARCH_CFLAGS \
    "-fno-pic -fno-plt -fno-jump-tables -mcmodel=medium -fno-asynchronous-unwind-tables"

// The medium code model reaches .rodata with 32-bit absolute addresses, so the
// executable view of an instance has to live in the low 2 GB.
#define ASTRO_ARCH_ARENA_LO 0x20000000u
#define ASTRO_ARCH_ARENA_HI 0x70000000u

// ---- compile side: how a hole becomes an immediate --------------------------
#ifdef ASTRO_SD_PATCH
// `P` is `extern char _astro_hole_base[]`, so `P + k` is a link-time constant
// and its relocation carries k as the addend — that addend is the hole number.
//
// The asm is not decoration.  Written as plain C, gcc folds `base + k` into an
// addressing-mode displacement, or derives one hole from another
// (`lea -0x54(%r12)`) — and then patching the immediates independently produces
// code that reads another node's values.  One movabs per hole, opaque to the
// optimiser, is the property the loader depends on.  The "i" constraint is why
// inline SDs are always_inline in this mode.
#define ASTRO_ARCH_HOLE_IMM(k) __extension__({ \
    uintptr_t _hv; __asm__("movabsq $%p1, %0" : "=r"(_hv) : "i"(P + (k))); _hv; })
#endif

#endif /* ASTRO_HOLE_ARCH_X86_64_H */

// ---- load side: relocation application ---------------------------------------
#if defined(ASTRO_LOADER_IMPL) && !defined(ASTRO_HOLE_ARCH_X86_64_LOAD_H)
#define ASTRO_HOLE_ARCH_X86_64_LOAD_H

#define ASTRO_ARCH_ELF_MACHINE EM_X86_64

#ifndef R_X86_64_GOTPCRELX
#define R_X86_64_GOTPCRELX 41
#endif
#ifndef R_X86_64_REX_GOTPCRELX
#define R_X86_64_REX_GOTPCRELX 42
#endif

// Bytes written by a relocation of this type; 0 = this backend cannot do it.
static inline size_t
astro_arch_reloc_width(unsigned type)
{
    switch (type) {
    case R_X86_64_64:  return 8;
    case R_X86_64_32:
    case R_X86_64_32S:
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
    case R_X86_64_GOTPCREL:
    case R_X86_64_GOTPCRELX:
    case R_X86_64_REX_GOTPCRELX: return 4;
    default: return 0;
    }
}

// True when the type resolves through a GOT slot the loader must allocate.
static inline bool
astro_arch_reloc_needs_got(unsigned type)
{
    return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX
        || type == R_X86_64_REX_GOTPCRELX;
}

// Apply one relocation.  `where` is in the writable view, `wherex` the address
// the same bytes will run at, `S` the resolved symbol value, `A` the addend,
// `got`/`gotx` the writable/executable address of this relocation's GOT slot
// (only when astro_arch_reloc_needs_got).  false = out of range / unsupported.
static inline bool
astro_arch_reloc_apply(unsigned type, char *where, uintptr_t wherex,
                       uintptr_t S, int64_t A, char *got, uintptr_t gotx)
{
    switch (type) {
    case R_X86_64_64: { uint64_t v = S + (uint64_t)A; memcpy(where, &v, 8); return true; }
    case R_X86_64_32: {
        uint64_t v = S + (uint64_t)A;
        if (v > 0xffffffffu) return false;
        uint32_t w = (uint32_t)v; memcpy(where, &w, 4); return true;
    }
    case R_X86_64_32S: {
        int64_t v = (int64_t)S + A;
        if (v < INT32_MIN || v > INT32_MAX) return false;
        int32_t w = (int32_t)v; memcpy(where, &w, 4); return true;
    }
    case R_X86_64_PC32:
    case R_X86_64_PLT32: {
        int64_t v = (int64_t)S + A - (int64_t)wherex;
        if (v < INT32_MIN || v > INT32_MAX) return false;
        int32_t w = (int32_t)v; memcpy(where, &w, 4); return true;
    }
    case R_X86_64_GOTPCREL:
    case R_X86_64_GOTPCRELX:
    case R_X86_64_REX_GOTPCRELX: {
        // -fno-plt turns host calls into `call *sym@GOTPCREL(%rip)`; give each
        // instance its own slot so the target is reachable from anywhere.
        memcpy(got, &S, 8);
        int64_t v = (int64_t)gotx + A - (int64_t)wherex;
        if (v < INT32_MIN || v > INT32_MAX) return false;
        int32_t w = (int32_t)v; memcpy(where, &w, 4); return true;
    }
    default: return false;
    }
}

// The two views alias the same pages; x86 keeps their instruction caches
// coherent, and an instance is only reachable after its dispatcher pointer is
// published, so nothing further is needed here.
static inline void
astro_arch_sync_icache(const char *addr, size_t len) { (void)addr; (void)len; }

#endif /* ASTRO_LOADER_IMPL */
