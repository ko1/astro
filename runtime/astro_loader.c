// ASTro loader path (docs/idea_code_store.md §7): copy-and-patch instantiation
// of a specialized dispatcher.
//
// #included from astro_code_store.c (pool mode, x86-64 Linux only).  The store
// keeps a second object of every SD source in op/, compiled -fno-pic
// -mcmodel=medium with every hole left as an R_X86_64_64 relocation against
// `_astro_hole_base` whose addend is the hole index (astro_hole.h).
// astro_cs_instantiate(n) copies that object's .text/.rodata into a private
// chunk, writes n's pool values over the hole relocations (immediates instead
// of P[k] loads), resolves the remaining relocations (host symbols through a
// per-instance GOT, local sections directly) and installs the copy as n's
// dispatcher.  The chunk lives in the low 2 GB — the medium code model refers
// to .rodata with 32-bit absolute addresses.  Anything unexpected returns
// false and leaves the node on its pool-mode SD.

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

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

#ifndef R_X86_64_GOTPCRELX
#define R_X86_64_GOTPCRELX 41
#endif
#ifndef R_X86_64_REX_GOTPCRELX
#define R_X86_64_REX_GOTPCRELX 42
#endif

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
    size_t used, size;
    bool failed;
} astro_ld_arena;

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
    for (uintptr_t hint = 0x20000000u; hint < 0x70000000u; hint += 0x10000000u) {
        void *p = mmap((void *)hint, reserve, PROT_READ | PROT_EXEC,
                       MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (p == MAP_FAILED) continue;
        if ((uintptr_t)p != hint) { munmap(p, reserve); continue; }   // old kernel: hint ignored
        x = p;
        break;
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

// Bump-allocate `size` bytes aligned to `align`; *xp receives the executable
// address of the same bytes.  x86 keeps the two views' i-cache coherent, and an
// instance is only reachable once its dispatcher pointer is published.
static char *
astro_ld_alloc(size_t size, size_t align, char **xp)
{
    if (!astro_ld_arena_init()) return NULL;
    if (align < 16) align = 16;
    const size_t off = (astro_ld_arena.used + align - 1) & ~(align - 1);
    if (size > astro_ld_arena.size - off) return NULL;
    astro_ld_arena.used = off + size;
    *xp = astro_ld_arena.x + off;
    return astro_ld_arena.w + off;
}

// Give back the most recent chunk (bump allocator) when patching fails.
static void
astro_ld_unalloc(const char *w, size_t size)
{
    if (w + size == astro_ld_arena.w + astro_ld_arena.used)
        astro_ld_arena.used = (size_t)(w - astro_ld_arena.w);
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
    bool bad;
};

static struct {
    struct astro_ld_obj *v;
    uint32_t n, capa;
} astro_ld_objs;

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
    if (!o->data || fread(o->data, 1, (size_t)len, fp) != (size_t)len) { fclose(fp); return false; }
    fclose(fp);
    o->size = (size_t)len;
    o->eh = (const Elf64_Ehdr *)o->data;
    const Elf64_Ehdr *const eh = o->eh;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64
        || eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_type != ET_REL || eh->e_machine != EM_X86_64
        || eh->e_shentsize != sizeof(Elf64_Shdr) || eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) > o->size)
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

static struct astro_ld_obj *
astro_ld_obj_get(const char *name)
{
    for (uint32_t i = 0; i < astro_ld_objs.n; i++)
        if (strcmp(astro_ld_objs.v[i].name, name) == 0) return &astro_ld_objs.v[i];
    if (strlen(name) >= sizeof(astro_ld_objs.v[0].name)) return NULL;
    if (astro_ld_objs.n == astro_ld_objs.capa) {
        astro_ld_objs.capa = astro_ld_objs.capa ? astro_ld_objs.capa * 2 : 64;
        astro_ld_objs.v = realloc(astro_ld_objs.v, sizeof(*astro_ld_objs.v) * astro_ld_objs.capa);
        if (!astro_ld_objs.v) { fprintf(stderr, "astro_loader: out of memory\n"); exit(1); }
    }
    struct astro_ld_obj *const o = &astro_ld_objs.v[astro_ld_objs.n++];
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
    o->bad = !ok;
    return o;
}

// ---- instantiate ------------------------------------------------------------

static struct { uint32_t n, failed; size_t bytes; } astro_ld_stats;

static bool
astro_ld_fits_s32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

bool
astro_cs_instantiate(NODE *n)
{
    if (!n || !n->head.flags.is_specialized || !n->head.pool || !n->head.dispatcher_name) return false;
    const char *const name = n->head.dispatcher_name;
    if (strncmp(name, "SD_", 3) != 0 && strncmp(name, "PGSD_", 5) != 0) return false;
    struct astro_ld_obj *const o = astro_ld_obj_get(name);
    if (!o || o->bad) { astro_ld_stats.failed++; return false; }

    const unsigned shnum = o->eh->e_shnum;
    // Layout: every SHF_ALLOC section (text / rodata; nothing writable).
    int64_t *place = calloc(shnum, sizeof(*place));
    if (!place) return false;
    size_t total = 0, maxalign = 16;
    for (unsigned i = 0; i < shnum; i++) {
        const Elf64_Shdr *const s = &o->sh[i];
        place[i] = -1;
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_type != SHT_PROGBITS && s->sh_type != SHT_NOBITS) continue;
        if ((s->sh_flags & SHF_WRITE) && s->sh_size > 0) { free(place); astro_ld_stats.failed++; return false; }
        if (s->sh_size == 0) continue;
        const size_t align = s->sh_addralign > 1 ? s->sh_addralign : 1;
        if (align > maxalign) maxalign = align;   // the chunk carries the strictest one
        total = (total + align - 1) & ~(align - 1);
        place[i] = (int64_t)total;
        total += s->sh_size;
    }
    // GOT slots: one per GOT-relative relocation (dedup is not worth it).
    size_t ngot = 0;
    for (unsigned i = 0; i < shnum; i++) {
        const Elf64_Shdr *const s = &o->sh[i];
        if (s->sh_type != SHT_RELA || s->sh_info >= shnum || place[s->sh_info] < 0) continue;
        const Elf64_Rela *const rel = (const Elf64_Rela *)(o->data + s->sh_offset);
        const size_t nrel = s->sh_size / sizeof(Elf64_Rela);
        for (size_t k = 0; k < nrel; k++) {
            const unsigned t = ELF64_R_TYPE(rel[k].r_info);
            if (t == R_X86_64_GOTPCREL || t == R_X86_64_GOTPCRELX || t == R_X86_64_REX_GOTPCRELX) ngot++;
        }
    }
    total = (total + 7) & ~(size_t)7;
    const size_t got_off = total;
    total += ngot * 8;

    char *xbase = NULL;
    char *const base = astro_ld_alloc(total, maxalign, &xbase);   // base: write view, xbase: exec view
    if (!base) { free(place); astro_ld_stats.failed++; return false; }
    for (unsigned i = 0; i < shnum; i++) {
        if (place[i] < 0) continue;
        const Elf64_Shdr *const s = &o->sh[i];
        if (s->sh_type == SHT_NOBITS) memset(base + place[i], 0, s->sh_size);
        else memcpy(base + place[i], o->data + s->sh_offset, s->sh_size);
    }

    // Relocate.
    size_t got_used = 0;
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
            // The write must stay inside the copied section — 8 bytes for the
            // 64-bit forms, 4 for the rest (a 4-byte reloc can sit 4 bytes from
            // the end, so demanding 8 everywhere rejects valid objects).
            const size_t rw = (t == R_X86_64_64) ? 8 : 4;
            if (si >= o->nsym || r->r_offset > tgt->sh_size || tgt->sh_size - r->r_offset < rw) { ok = false; break; }
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
                if (A < 0 || (uint32_t)A >= n->head.nholes) { ok = false; break; }
                const uint64_t v = (uint64_t)n->head.pool[A];
                switch (t) {
                case R_X86_64_64:  memcpy(where, &v, 8); break;
                case R_X86_64_32:  if (v > 0xffffffffu) ok = false; else { uint32_t w = (uint32_t)v; memcpy(where, &w, 4); } break;
                case R_X86_64_32S: if (!astro_ld_fits_s32((int64_t)v)) ok = false; else { int32_t w = (int32_t)v; memcpy(where, &w, 4); } break;
                default: ok = false;
                }
                continue;
            }
            uintptr_t S;
            if (sym->st_shndx == SHN_UNDEF) {
                void *const p = dlsym(RTLD_DEFAULT, sname);
                if (!p) { ok = false; break; }
                S = (uintptr_t)p;
            } else if (sym->st_shndx < shnum && place[sym->st_shndx] >= 0) {
                S = (uintptr_t)(xbase + place[sym->st_shndx] + sym->st_value);
            } else {
                ok = false; break;
            }
            switch (t) {
            case R_X86_64_64: { uint64_t v = S + (uint64_t)A; memcpy(where, &v, 8); break; }
            case R_X86_64_32: { uint64_t v = S + (uint64_t)A; if (v > 0xffffffffu) ok = false; else { uint32_t w = (uint32_t)v; memcpy(where, &w, 4); } break; }
            case R_X86_64_32S: { int64_t v = (int64_t)S + A; if (!astro_ld_fits_s32(v)) ok = false; else { int32_t w = (int32_t)v; memcpy(where, &w, 4); } break; }
            case R_X86_64_PC32:
            case R_X86_64_PLT32: { int64_t v = (int64_t)S + A - (int64_t)wherex; if (!astro_ld_fits_s32(v)) ok = false; else { int32_t w = (int32_t)v; memcpy(where, &w, 4); } break; }
            case R_X86_64_GOTPCREL:
            case R_X86_64_GOTPCRELX:
            case R_X86_64_REX_GOTPCRELX: {
                char *const slot = base + got_off + got_used * 8;
                const uintptr_t slotx = (uintptr_t)(xbase + got_off + got_used * 8);
                got_used++;
                memcpy(slot, &S, 8);
                int64_t v = (int64_t)slotx + A - (int64_t)wherex;
                if (!astro_ld_fits_s32(v)) ok = false; else { int32_t w = (int32_t)v; memcpy(where, &w, 4); }
                break;
            }
            default: ok = false;
            }
        }
    }

    // Entry point.
    void *entry = NULL;
    for (size_t k = 0; ok && k < o->nsym; k++) {
        const Elf64_Sym *const sym = &o->sym[k];
        if (ELF64_ST_TYPE(sym->st_info) == STT_FUNC && sym->st_shndx < shnum && place[sym->st_shndx] >= 0
            && strcmp(o->str + sym->st_name, name) == 0) {
            entry = xbase + place[sym->st_shndx] + sym->st_value;
            break;
        }
    }
    free(place);
    if (!ok || !entry) { astro_ld_unalloc(base, total); astro_ld_stats.failed++; return false; }
    n->head.dispatcher = (node_dispatcher_func_t)entry;
    if (getenv("ASTRO_LD_TRACE"))
        fprintf(stderr, "astro_ld: %s at %p (%zu B, align %zu, got %zu) pool %p\n",
                name, (void *)entry, total, maxalign, got_used, (const void *)n->head.pool);
    astro_ld_stats.n++;
    astro_ld_stats.bytes += total;
    return true;
}

void
astro_cs_instantiate_stats(uint32_t *n, uint32_t *failed, size_t *bytes)
{
    if (n) *n = astro_ld_stats.n;
    if (failed) *failed = astro_ld_stats.failed;
    if (bytes) *bytes = astro_ld_stats.bytes;
}
