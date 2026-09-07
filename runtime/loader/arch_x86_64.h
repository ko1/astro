// Two phases, two guards: constants + compile-side macros on first inclusion,
// the relocation applier when the loader includes us with ASTRO_LOADER_IMPL.
#ifndef ASTRO_HOLE_ARCH_X86_64_H
#define ASTRO_HOLE_ARCH_X86_64_H

// x86-64 backend for the loader path.  See loader/arch.h for the contract.

#define ASTRO_ARCH_SUPPORTED 1
#define ASTRO_ARCH_NAME      "x86-64"

// op/*.o build flags.  -mcmodel=medium makes the compiler address the hole
// symbol with a 64-bit immediate (movabs) instead of a RIP-relative form, so
// the value can be patched per instance; -fno-pic/-fno-plt keep calls and data
// references direct; -fno-jump-tables keeps .rodata free of code addresses that
// would need their own relocation pass.
// ASTRO_SD_NO_DESC: the hole descriptor is data the loader reads from the
// all.so side, so emitting it here too would just be copied into the object.
// -fpie: data is rip-relative, so an instance runs correctly at any address
// (an absolute 32-bit datum reference would pin the arena below 4 GB, which
// buys 16 bytes of code per SD and costs the freedom to place it), and host
// calls stay direct rel32.  Holes are 64-bit absolute immediates, which -fpie
// leaves alone.  A call that turns out not to reach goes through an arena stub,
// so the same object is correct whether or not the arena landed near the host.
// Measured on optcarrot (sp4): near 231.0 fps, far (stubs) 228.1, and the
// -fno-plt/GOT shape this replaced 223.7.
#define ASTRO_ARCH_CFLAGS \
    "-fpie -fno-jump-tables -fno-asynchronous-unwind-tables -DASTRO_SD_NO_DESC"

// The medium code model reaches .rodata with 32-bit absolute addresses, so the
// executable view of an instance has to live in the low 2 GB.
// A call whose target is out of rel32 range needs a trampoline, exactly as a
// linker's PLT entry does: movabs $target,%r11 ; jmp *%r11.  %r11 is the ABI's
// scratch register, so no argument or return register is disturbed.
#define ASTRO_ARCH_STUB_SIZE 16
#define ASTRO_ARCH_ARENA_LO 0x20000000u
#define ASTRO_ARCH_ARENA_HI 0x70000000u

#endif /* ASTRO_HOLE_ARCH_X86_64_H */

// ---- compile side: how a hole becomes an immediate --------------------------
// Own guard: the first include may come from a translation unit that has not
// defined ASTRO_SD_PATCH (the host), and a later one may.
#if defined(ASTRO_SD_PATCH) && !defined(ASTRO_HOLE_ARCH_X86_64_IMM_H)
#define ASTRO_HOLE_ARCH_X86_64_IMM_H
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
        // Wrap-safe: signed overflow in the check itself would be UB.
        const int64_t v = (int64_t)(S + (uint64_t)A);
        if (v < INT32_MIN || v > INT32_MAX) return false;
        int32_t w = (int32_t)v; memcpy(where, &w, 4); return true;
    }
    case R_X86_64_PC32:
    case R_X86_64_PLT32: {
        const int64_t v = (int64_t)(S + (uint64_t)A - wherex);
        if (v < INT32_MIN || v > INT32_MAX) return false;
        int32_t w = (int32_t)v; memcpy(where, &w, 4); return true;
    }
    case R_X86_64_GOTPCREL:
    case R_X86_64_GOTPCRELX:
    case R_X86_64_REX_GOTPCRELX: {
        // -fno-plt turns host calls into `call *sym@GOTPCREL(%rip)`; give each
        // instance its own slot so the target is reachable from anywhere.
        if (!got) return false;                    // caller reserved no slot
        memcpy(got, &S, 8);
        const int64_t v = (int64_t)(gotx + (uint64_t)A - wherex);
        if (v < INT32_MIN || v > INT32_MAX) return false;
        int32_t w = (int32_t)v; memcpy(where, &w, 4); return true;
    }
    default: return false;
    }
}

static inline bool
astro_arch_reloc_is_pcrel32(const unsigned type)
{
    return type == R_X86_64_PC32 || type == R_X86_64_PLT32;
}

static inline bool
astro_arch_reloc_pcrel32_fits(const uintptr_t S, const int64_t A, const uintptr_t wherex)
{
    const int64_t v = (int64_t)(S + (uint64_t)A - wherex);
    return v >= INT32_MIN && v <= INT32_MAX;
}

static inline void
astro_arch_make_stub(char *const w, const uintptr_t target)
{
    static const unsigned char code[ASTRO_ARCH_STUB_SIZE] = {
        0x49, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0,   // movabs $target,%r11
        0x41, 0xff, 0xe3,                      // jmp *%r11
        0xcc, 0xcc, 0xcc                       // pad
    };
    memcpy(w, code, sizeof(code));
    memcpy(w + 2, &target, 8);
}

// The two views alias the same pages; x86 keeps their instruction caches
// coherent, and an instance is only reachable after its dispatcher pointer is
// published, so nothing further is needed here.
static inline void
astro_arch_sync_icache(const char *addr, size_t len) { (void)addr; (void)len; }

#endif /* ASTRO_LOADER_IMPL */
