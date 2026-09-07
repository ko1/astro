// ASTro node-weaving loader (docs/idea_code_store.md §7).
//
// A normal loader resolves an object's relocations from a symbol table, once,
// when a module is mapped.  This one instantiates the SAME object once per AST
// node and resolves its relocations from that node's run-time state — the hole
// values the pool already collected: interned IDs, inline-cache addresses, the
// child NODE pointers.  The node is the symbol table, and the values are woven
// into a private copy of the shared template.
//
// #included from astro_code_store.c when the pool path is on, the OS can give
// us aliased W^X mappings, and the architecture has a backend.  Everything
// instruction-set specific — which relocations exist, how a hole becomes an
// immediate, where the code may live, what flags op/*.o needs, i-cache
// coherency — lives in loader/arch_<isa>.h behind the contract in loader/arch.h.
// Anything unsupported returns false and the body keeps its pool-mode SD.

#define ASTRO_LOADER_IMPL 1
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "loader/arch.h"

// memfd_create is behind _GNU_SOURCE, and this file is #included late into the
// host's translation unit — call the syscall directly instead.
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static int
astro_ld_memfd(const char *name, unsigned flags)
{
#ifdef SYS_memfd_create
    return (int)syscall(SYS_memfd_create, name, flags);
#else
    (void)name; (void)flags;
    return -1;
#endif
}

// ---- code arena ------------------------------------------------------------
//
// One reservation, two views of the same pages (memfd): a writable one the
// loader patches through, and an executable one in the low 2 GB (the medium
// code model addresses .rodata with 32-bit absolutes, so the address the code
// runs at must fit there).  Aliasing keeps W^X without a single per-chunk
// mprotect, which is what let chunks be page-granular before: instances now
// pack at their own alignment (16 B floor, raised to the object's strictest
// section alignment — 32 B for the .rodata.cst32 the AVX constants land in).

static struct {
    char *w, *x;                 // write view / exec view of the same bytes
    size_t used, prev_used, size;
    bool failed;
} astro_ld_arena;

// The host's executable segments, so "near enough for rel32" is a checked
// property and not a hope.  /proc/self/maps is read rather than link.h's
// dl_iterate_phdr, which needs _GNU_SOURCE this late-included file cannot set.
struct astro_ld_text { uintptr_t lo, hi; };

static struct astro_ld_text
astro_ld_host_text(void)
{
    struct astro_ld_text t = { UINTPTR_MAX, 0 };
    char exe[ASTRO_CS_PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        FILE *const f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                unsigned long lo = 0, hi = 0;
                char perms[8] = {0}, path[512] = {0};
                if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &lo, &hi, perms, path) < 3) continue;
                if (perms[2] != 'x') continue;
                const char *p = path;
                while (*p == ' ' || *p == '\t') p++;
                if (strcmp(p, exe) != 0) continue;
                if ((uintptr_t)lo < t.lo) t.lo = (uintptr_t)lo;
                if ((uintptr_t)hi > t.hi) t.hi = (uintptr_t)hi;
            }
            fclose(f);
        }
    }
    if (t.hi == 0) {              // no maps: fall back to one known address
        const uintptr_t a = (uintptr_t)&astro_ld_host_text;
        t.lo = a; t.hi = a + 1;
    }
    return t;
}

// Can every rel32 between [x, x+size) and the host text fit in 32 bits?
static bool
astro_ld_in_rel32(uintptr_t x, size_t size, struct astro_ld_text t)
{
    const int64_t d1 = (int64_t)(t.hi - x);            // furthest forward jump
    const int64_t d2 = (int64_t)((x + size) - t.lo);   // furthest backward jump
    return d1 < INT32_MAX && d1 > INT32_MIN && d2 < INT32_MAX && d2 > INT32_MIN;
}

static bool
astro_ld_arena_init(void)
{
    if (astro_ld_arena.w) return true;
    if (astro_ld_arena.failed) return false;
    astro_ld_arena.failed = true;                 // cleared on success

    const size_t reserve = (size_t)256 << 20;
    const int fd = astro_ld_memfd("astro_sd", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)reserve) != 0) { close(fd); return false; }

    char *x = MAP_FAILED;
    if (astro_ld_arena_near()) {
        // Direct rel32 calls into the host: the arena has to land within ±2GB
        // of its text.  Probe outwards from it — the classic JIT trick —
        // starting below, where a PIE leaves room.  Every candidate is checked
        // against the measured text range, so a hit is a fact, not a hope.
        const struct astro_ld_text t = astro_ld_host_text();
        const uintptr_t anchor = t.lo & ~(uintptr_t)0xfffff;
        for (size_t off = reserve; off < ((size_t)1 << 31) && x == MAP_FAILED; off += reserve) {
            for (int up = 0; up < 2; up++) {
                if (!up && anchor < off) continue;
                const uintptr_t hint = up ? (t.hi + off) & ~(uintptr_t)0xfffff : anchor - off;
                if (!astro_ld_in_rel32(hint, reserve, t)) continue;
                void *p = mmap((void *)hint, reserve, PROT_READ | PROT_EXEC,
                               MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
                if (p == MAP_FAILED) continue;
                if ((uintptr_t)p != hint) { munmap(p, reserve); continue; }
                x = p;
                break;
            }
        }
        if (x != MAP_FAILED && getenv("ASTRO_LD_TRACE"))
            fprintf(stderr, "astro_ld: host text %p-%p, arena %p (+%lld MB)\n",
                    (void *)t.lo, (void *)t.hi, (void *)x,
                    (long long)(((int64_t)(uintptr_t)x - (int64_t)t.lo) >> 20));
    }
    if (x == MAP_FAILED) {
        // Nowhere near the host: any page is still correct, because a call that
        // does not reach is routed through a stub in the arena.
        void *const p = mmap(NULL, reserve, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) x = p;
    }
    if (x == MAP_FAILED) { close(fd); return false; }

    void *w = mmap(NULL, reserve, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (w == MAP_FAILED) { munmap(x, reserve); return false; }

    astro_ld_arena.w = (char *)w;
    astro_ld_arena.x = x;
    astro_ld_arena.size = reserve;
    astro_ld_arena.used = 0;
    astro_ld_arena.failed = false;
    return true;
}

// One GOT slot per host symbol for the whole arena, not one per instance: the
// slot holds the symbol's address, which is the same for every instance that
// refers to it.  Per-instance slots cost 457 KB on optcarrot (58,618 slots for
// 77 distinct symbols).  Open addressing, keyed by the target address.
#define ASTRO_LD_GOT_CAPA 2048
static struct { uintptr_t target; char *w; uintptr_t x; } astro_ld_got[ASTRO_LD_GOT_CAPA];
static uint32_t astro_ld_got_n;

static char *astro_ld_alloc(size_t size, size_t align, char **xp);

static bool
astro_ld_got_for(const uintptr_t target, char **const wp, uintptr_t *const xp)
{
    uint32_t h = (uint32_t)((target >> 3) * 2654435761u) & (ASTRO_LD_GOT_CAPA - 1);
    for (;;) {
        if (astro_ld_got[h].target == target) {
            *wp = astro_ld_got[h].w; *xp = astro_ld_got[h].x;
            return true;
        }
        if (astro_ld_got[h].w == NULL) break;
        h = (h + 1) & (ASTRO_LD_GOT_CAPA - 1);
    }
    if (astro_ld_got_n >= ASTRO_LD_GOT_CAPA / 2) return false;   // keep it sparse
    char *x = NULL;
    char *const w = astro_ld_alloc(ASTRO_ARCH_GOT_SLOT, ASTRO_ARCH_GOT_SLOT, &x);
    if (!w) return false;
    astro_ld_got[h].target = target;
    astro_ld_got[h].w = w;
    astro_ld_got[h].x = (uintptr_t)x;
    astro_ld_got_n++;
    *wp = w; *xp = (uintptr_t)x;
    return true;
}

#if ASTRO_ARCH_STUB_SIZE
// One trampoline per far target, shared by every instance: the targets are a
// handful of libc entry points, and a stub in the arena is reachable from any
// call site in it.
static struct { uintptr_t target, stub; } astro_ld_stub[512];
static uint32_t astro_ld_stub_n;

static uintptr_t
astro_ld_stub_for(const uintptr_t target)
{
    for (uint32_t i = 0; i < astro_ld_stub_n; i++)
        if (astro_ld_stub[i].target == target) return astro_ld_stub[i].stub;
    if (astro_ld_stub_n == sizeof(astro_ld_stub) / sizeof(astro_ld_stub[0])) return 0;
    char *x = NULL;
    char *const w = astro_ld_alloc(ASTRO_ARCH_STUB_SIZE, 16, &x);
    if (!w) return 0;
    astro_arch_make_stub(w, target);
    astro_ld_stub[astro_ld_stub_n].target = target;
    astro_ld_stub[astro_ld_stub_n].stub = (uintptr_t)x;
    astro_ld_stub_n++;
    return (uintptr_t)x;
}
#endif

// Bump-allocate `size` bytes aligned to `align`; *xp receives the executable
// address of the same bytes.  x86 keeps the two views' i-cache coherent, and an
// instance is only reachable once its dispatcher pointer is published.
static char *
astro_ld_alloc(size_t size, size_t align, char **xp)
{
    if (!astro_ld_arena_init()) return NULL;
    if (align < 16) align = 16;
    if (align > astro_ld_arena.size - astro_ld_arena.used) return NULL;
    const size_t off = (astro_ld_arena.used + align - 1) & ~(align - 1);
    if (off > astro_ld_arena.size || size > astro_ld_arena.size - off) return NULL;
    astro_ld_arena.prev_used = astro_ld_arena.used;   // for unalloc (padding too)
    astro_ld_arena.used = off + size;
    *xp = astro_ld_arena.x + off;
    return astro_ld_arena.w + off;
}

// Give back the most recent chunk (bump allocator) when patching fails —
// including the padding that was skipped to align it.
static void
astro_ld_unalloc(const char *w, size_t size)
{
    if (w + size == astro_ld_arena.w + astro_ld_arena.used)
        astro_ld_arena.used = astro_ld_arena.prev_used;
}

// ---- object cache: op/<SD>.o read once per SD name --------------------------

struct astro_ld_obj {
    char name[64];
    uint8_t *data;
    size_t size;
    const Elf64_Ehdr *eh;
    const Elf64_Shdr *sh;
    const char *shstr;
    const Elf64_Sym *sym;
    size_t nsym;
    const char *str;
    size_t strsz;
};

static char astro_ld_preload_dir[ASTRO_CS_DIR_MAX];   // set by astro_cs_set_preload

static bool
astro_ld_read(struct astro_ld_obj *o, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= (long)sizeof(Elf64_Ehdr)) { fclose(fp); return false; }
    o->data = malloc((size_t)len);
    if (!o->data || fread(o->data, 1, (size_t)len, fp) != (size_t)len) {
        fclose(fp); free(o->data); o->data = NULL; return false;
    }
    fclose(fp);
    o->size = (size_t)len;
    o->eh = (const Elf64_Ehdr *)o->data;
    const Elf64_Ehdr *const eh = o->eh;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64
        || eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_type != ET_REL || eh->e_machine != ASTRO_ARCH_ELF_MACHINE
        || eh->e_shentsize != sizeof(Elf64_Shdr))
        return false;
    // Bounds without overflow: e_shoff is 64-bit and could wrap the addition.
    if (eh->e_shoff > o->size
        || (o->size - eh->e_shoff) / sizeof(Elf64_Shdr) < eh->e_shnum)
        return false;
    o->sh = (const Elf64_Shdr *)(o->data + eh->e_shoff);
    if (eh->e_shstrndx >= eh->e_shnum) return false;
    // Every section this loader reads from must lie inside the file.
    for (unsigned i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr *const sh = &o->sh[i];
        if (sh->sh_type == SHT_NOBITS) continue;
        if (sh->sh_offset > o->size || sh->sh_size > o->size - sh->sh_offset) return false;
    }
    o->shstr = (const char *)(o->data + o->sh[eh->e_shstrndx].sh_offset);
    for (unsigned i = 0; i < eh->e_shnum; i++) {
        if (o->sh[i].sh_type != SHT_SYMTAB) continue;
        if (o->sh[i].sh_entsize != sizeof(Elf64_Sym)) return false;
        if (o->sh[i].sh_link >= eh->e_shnum) return false;
        const Elf64_Shdr *const st = &o->sh[o->sh[i].sh_link];
        if (st->sh_type != SHT_STRTAB || st->sh_size == 0) return false;
        o->sym = (const Elf64_Sym *)(o->data + o->sh[i].sh_offset);
        o->nsym = o->sh[i].sh_size / sizeof(Elf64_Sym);
        o->str = (const char *)(o->data + st->sh_offset);
        o->strsz = st->sh_size;
        if (o->str[o->strsz - 1] != '\0') return false;   // names are NUL-terminated in range
    }
    return o->sym != NULL;
}

// Read the object for one weave and drop it again.  It was cached once, but
// the reuse is 1.1 instances per object (optcarrot): nodes rarely share a
// shape, so the cache only kept a few MB resident for nothing.  The second
// read of a repeated shape comes from the page cache.
static bool
astro_ld_obj_open(struct astro_ld_obj *const o, const char *const name)
{
    if (strlen(name) >= sizeof(o->name)) return false;
    memset(o, 0, sizeof(*o));
    strcpy(o->name, name);
    char path[ASTRO_CS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/op/%s.o", astro_cs.store_dir, name);
    bool ok = astro_ld_read(o, path);
    if (!ok && astro_ld_preload_dir[0]) {
        free(o->data); o->data = NULL; o->sym = NULL;
        snprintf(path, sizeof(path), "%s/op/%s.o", astro_ld_preload_dir, name);
        ok = astro_ld_read(o, path);
    }
    if (!ok) { free(o->data); o->data = NULL; }
    return ok;
}

static void
astro_ld_obj_close(struct astro_ld_obj *const o)
{
    free(o->data);
    o->data = NULL;
}

// ---- instantiate ------------------------------------------------------------

static struct { uint32_t n, failed; size_t bytes; } astro_ld_stats;

// Values for the holes.  Normally the pool path already built them on this node;
// if it did not (a policy that weaves at bind time need not keep a pool at all),
// run the same generated filler into a scratch buffer — same traversal, same
// order, same numbers.
struct astro_ld_values { const astro_hole_t *v; uint32_t n; astro_hole_t *owned; };

static bool
astro_ld_values_get(NODE *n, const char *sd_sym, struct astro_ld_values *out)
{
    // ASTRO_LD_SCRATCH=1 takes the scratch path even when a pool exists, so the
    // pool-independent route is exercised by the test suite (both must agree).
    static int scratch = -1;
    if (scratch < 0) { const char *e = getenv("ASTRO_LD_SCRATCH"); scratch = (e && *e && strcmp(e, "0")) ? 1 : 0; }
    if (n->head.pool && !scratch) { out->v = n->head.pool; out->n = n->head.nholes; out->owned = NULL; return true; }
    const uint8_t *const desc = astro_cs_dlsym_desc(sd_sym);
    if (!desc) return false;
    const uint32_t cnt = astro_hole_walk(n, desc, NULL);
    astro_hole_t *buf = malloc(sizeof(*buf) * (cnt ? cnt : 1));
    if (!buf) return false;
    astro_hole_walk(n, desc, buf);
    out->v = buf; out->n = cnt; out->owned = buf;
    return true;
}

bool
astro_cs_instantiate(NODE *n)
{
    if (!n || !n->head.flags.is_specialized || !n->head.dispatcher_name) return false;
    // Instances are immortal (an activation may still be running in one), so a
    // node is woven at most once: a second call would leak a chunk.
    if (astro_ld_arena.x && (char *)n->head.dispatcher >= astro_ld_arena.x
        && (char *)n->head.dispatcher < astro_ld_arena.x + astro_ld_arena.size) return false;
    const char *const name = n->head.dispatcher_name;
    if (strncmp(name, "SD_", 3) != 0 && strncmp(name, "PGSD_", 5) != 0) return false;
    struct astro_ld_obj obj;
    struct astro_ld_obj *const o = &obj;
    if (!astro_ld_obj_open(o, name)) { astro_ld_stats.failed++; return false; }
    struct astro_ld_values hv;
    if (!astro_ld_values_get(n, name, &hv)) {
        astro_ld_obj_close(o); astro_ld_stats.failed++; return false;
    }
#define ASTRO_LD_RETURN(v) do { free(hv.owned); astro_ld_obj_close(o); return (v); } while (0)

    const unsigned shnum = o->eh->e_shnum;
    // Layout: every SHF_ALLOC section (text / rodata; nothing writable).
    int64_t *place = calloc(shnum, sizeof(*place));
    if (!place) ASTRO_LD_RETURN(false);
    size_t total = 0, maxalign = 16;
    for (unsigned i = 0; i < shnum; i++) {
        const Elf64_Shdr *const s = &o->sh[i];
        place[i] = -1;
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_type != SHT_PROGBITS && s->sh_type != SHT_NOBITS) continue;
        if ((s->sh_flags & SHF_WRITE) && s->sh_size > 0) { free(place); astro_ld_stats.failed++; ASTRO_LD_RETURN(false); }
        if (s->sh_size == 0) continue;
        const size_t align = s->sh_addralign > 1 ? s->sh_addralign : 1;
        // The arena is page-aligned, so aligning the offset aligns the address
        // only while the requirement is <= a page; and a power of two is what
        // the masking below assumes.  Anything else: leave the node on pool.
        if ((align & (align - 1)) != 0 || align > 4096) { free(place); astro_ld_stats.failed++; ASTRO_LD_RETURN(false); }
        if (align > maxalign) maxalign = align;   // the chunk carries the strictest one
        if (s->sh_size > SIZE_MAX - total - align) { free(place); astro_ld_stats.failed++; ASTRO_LD_RETURN(false); }
        total = (total + align - 1) & ~(align - 1);
        place[i] = (int64_t)total;
        total += s->sh_size;
    }
    // The relocation tables have to be well-formed before anything is copied.
    for (unsigned i = 0; i < shnum; i++) {
        const Elf64_Shdr *const s = &o->sh[i];
        if (s->sh_type != SHT_RELA || s->sh_info >= shnum || place[s->sh_info] < 0) continue;
        if (s->sh_entsize != sizeof(Elf64_Rela) || s->sh_size % sizeof(Elf64_Rela)) {
            free(place); astro_ld_stats.failed++; ASTRO_LD_RETURN(false);
        }
    }
    total = (total + 7) & ~(size_t)7;

    char *xbase = NULL;
    char *const base = astro_ld_alloc(total, maxalign, &xbase);   // base: write view, xbase: exec view
    if (!base) { free(place); astro_ld_stats.failed++; ASTRO_LD_RETURN(false); }
    for (unsigned i = 0; i < shnum; i++) {
        if (place[i] < 0) continue;
        const Elf64_Shdr *const s = &o->sh[i];
        if (s->sh_type == SHT_NOBITS) memset(base + place[i], 0, s->sh_size);
        else memcpy(base + place[i], o->data + s->sh_offset, s->sh_size);
    }

    // Relocate.
    bool ok = true;
    for (unsigned i = 0; i < shnum && ok; i++) {
        const Elf64_Shdr *const s = &o->sh[i];
        if (s->sh_type != SHT_RELA || s->sh_info >= shnum || place[s->sh_info] < 0) continue;
        const Elf64_Rela *const rel = (const Elf64_Rela *)(o->data + s->sh_offset);
        const size_t nrel = s->sh_size / sizeof(Elf64_Rela);
        const Elf64_Shdr *const tgt = &o->sh[s->sh_info];
        for (size_t k = 0; k < nrel && ok; k++) {
            const Elf64_Rela *const r = &rel[k];
            const unsigned t = ELF64_R_TYPE(r->r_info);
            const size_t si = ELF64_R_SYM(r->r_info);
            // The write must stay inside the copied section.  Width (and
            // whether the type is supported at all) is the backend's answer.
            const size_t rw = astro_arch_reloc_width(t);
            if (rw == 0 || si >= o->nsym || r->r_offset > tgt->sh_size
                || tgt->sh_size - r->r_offset < rw) { ok = false; break; }
            const Elf64_Sym *const sym = &o->sym[si];
            if (sym->st_name >= o->strsz) { ok = false; break; }
            const char *const sname = o->str + sym->st_name;
            char *const where = base + place[s->sh_info] + r->r_offset;         // store here
            const uintptr_t wherex = (uintptr_t)(xbase + place[s->sh_info] + r->r_offset);  // runs here
            const int64_t A = r->r_addend;

            if (sym->st_shndx == SHN_UNDEF && strcmp(sname, "_astro_hole_base") == 0) {
                // Hole: the addend is the index into n's pool.  A mismatch (a
                // stale op/ object against a pool built by another SD version)
                // must fail, never read past the table.
                // No GOT slot is reserved for holes (the value is the datum,
                // not an address to indirect through), so a GOT-shaped hole
                // relocation must fail rather than reach the backend with a
                // null slot.
                if (astro_arch_reloc_needs_got(t)) { ok = false; break; }
                if (A < 0 || (uint32_t)A >= hv.n) { ok = false; break; }
                // The hole's value IS the symbol value here: the addend picked
                // the slot, so the backend writes it with addend 0.
                ok = astro_arch_reloc_apply(t, where, wherex,
                                            (uintptr_t)hv.v[A], 0, NULL, 0);
                if (!ok) break;
                continue;
            }
            uintptr_t S;
            if (sym->st_shndx == SHN_UNDEF) {
                void *const p = dlsym(RTLD_DEFAULT, sname);
                if (!p) { ok = false; break; }
                S = (uintptr_t)p;
            } else if (sym->st_shndx == SHN_ABS) {
                S = (uintptr_t)sym->st_value;
            } else if (sym->st_shndx < shnum && place[sym->st_shndx] >= 0
                       && sym->st_value <= o->sh[sym->st_shndx].sh_size) {
                S = (uintptr_t)(xbase + place[sym->st_shndx] + sym->st_value);
            } else {
                ok = false; break;
            }
#if ASTRO_ARCH_STUB_SIZE
            // Out of rel32 reach (a libc entry point, say): route the call
            // through a trampoline in the arena instead of failing.
            if (astro_arch_reloc_is_pcrel32(t) && !astro_arch_reloc_pcrel32_fits(S, A, wherex)) {
                S = astro_ld_stub_for(S);
                if (!S) { ok = false; break; }
            }
#endif
            char *slot = NULL; uintptr_t slotx = 0;
            if (astro_arch_reloc_needs_got(t) && !astro_ld_got_for(S, &slot, &slotx)) {
                ok = false; break;
            }
            ok = astro_arch_reloc_apply(t, where, wherex, S, A, slot, slotx);
        }
    }

    // Entry point.
    void *entry = NULL;
    for (size_t k = 0; ok && k < o->nsym; k++) {
        const Elf64_Sym *const sym = &o->sym[k];
        if (ELF64_ST_TYPE(sym->st_info) == STT_FUNC && sym->st_shndx < shnum && place[sym->st_shndx] >= 0
            && sym->st_value < o->sh[sym->st_shndx].sh_size
            && sym->st_name < o->strsz && strcmp(o->str + sym->st_name, name) == 0) {
            entry = xbase + place[sym->st_shndx] + sym->st_value;
            break;
        }
    }
    free(place);
    if (!ok || !entry) { astro_ld_unalloc(base, total); astro_ld_stats.failed++; ASTRO_LD_RETURN(false); }
    astro_arch_sync_icache(xbase, total);
    n->head.dispatcher = (node_dispatcher_func_t)entry;
    if (getenv("ASTRO_LD_TRACE"))
        fprintf(stderr, "astro_ld: %s at %p (%zu B, align %zu, shared got %u) pool %p\n",
                name, (void *)entry, total, maxalign, astro_ld_got_n, (const void *)n->head.pool);
    astro_ld_stats.n++;
    astro_ld_stats.bytes += total;
    ASTRO_LD_RETURN(true);
#undef ASTRO_LD_RETURN
}

void
astro_cs_instantiate_stats(uint32_t *n, uint32_t *failed, size_t *bytes)
{
    if (n) *n = astro_ld_stats.n;
    if (failed) *failed = astro_ld_stats.failed;
    if (bytes) *bytes = astro_ld_stats.bytes;
}

#undef ASTRO_LOADER_IMPL
