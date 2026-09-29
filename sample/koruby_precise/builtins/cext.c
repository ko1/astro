/* koruby_precise — builtins/cext.c: the CRuby C-extension compatibility layer
 * (docs/c_ext_api_design.md §9).  #included into korb_runtime.c's TU.
 *
 * An extension sees VALUEs that are either immediates (koruby's own bits) or
 * HANDLES: addresses of cells in never-moving blocks that the GC keeps pointed at
 * the object.  One object → one cell (extensions compare VALUEs with ==).
 * A cell is LOCAL while some extension frame holds it (a strong root), RETIRED
 * when none does (weak: kept and updated while the object lives, or while a
 * dmark reaches it via rb_gc_mark), PERM for classes / registered globals.
 *
 * The C API has no context argument, so the current CTX is a thread-local —
 * the one global this layer needs (CRuby's GET_EC() is the same). */
#include <dlfcn.h>
#include <poll.h>
#include <setjmp.h>
#include <stdarg.h>

typedef uintptr_t KVALUE;   /* the extension-side VALUE (immediate or handle) */

/* ---- handle blocks ------------------------------------------------------- */
enum { KCX_FREE = 0, KCX_LOCAL = 1, KCX_RETIRED = 2, KCX_PERM = 3 };
#define KCX_MARKED 0x10u     /* reached by rb_gc_mark during this collection */
#define KCX_REG    0x20u     /* named by a registered address during this collection */
#define KCX_STATE(b) ((b) & 0x0Fu)
#define KCX_BLOCK_BYTES 131072u
#define KCX_CELLS 6000u

struct korb_cext_block {
    korb_value cells[KCX_CELLS];          /* first: a handle's block is its address rounded down */
    uint32_t   refs[KCX_CELLS];           /* extension frames holding the cell */
    uint32_t   seen[KCX_CELLS];           /* serial of the last frame that recorded it */
    uint8_t    state[KCX_CELLS];
    struct korb_cext_block *next;
};
_Static_assert(sizeof(struct korb_cext_block) <= KCX_BLOCK_BYTES, "cext block");

struct korb_cext_method { void *fn; int arity; struct korb_method *entry; };   /* entry: the method rb_call_super continues from */
typedef KVALUE (*rb_alloc_func_kcx)(KVALUE);
struct korb_cext_alloc { uint32_t class_serial; rb_alloc_func_kcx fn; };

struct korb_cext_vm {
    struct korb_cext_block *blocks;
    korb_value *free_list;                /* free cells, chained through the cell word */
    /* object → cell (open addressing, rebuilt after every collection) */
    korb_value **map; uint32_t map_capa, map_n;
    KVALUE **reg; uint32_t reg_n, reg_capa;           /* rb_gc_register_address */
    struct korb_cext_method *methods; uint32_t method_n;
    struct korb_cext_alloc *allocs; uint32_t alloc_n, alloc_capa;
    struct { char *path; void *dl; } *libs; uint32_t lib_n, lib_capa;
    korb_value errinfo;                   /* rb_errinfo (a root) */
    KVALUE cblock_maker;                  /* [proc maker, lambda maker] for C-function blocks */
    struct rb_io *io; size_t io_n;        /* GetOpenFile's per-descriptor rb_io_t */
    uintptr_t pending_state;              /* what rb_protect caught, for rb_jump_tag */
    uint32_t serial;
    /* set around a dmark call so rb_gc_mark can visit */
    void *gc_ctx; void (*gc_visit)(void *, void **);
};

struct korb_cext_jmp {
    jmp_buf jb;
    RESULT r;
    struct korb_cext_jmp *prev;
};

struct korb_cext_frame {
    struct korb_cext_frame *prev;
    korb_value *slots;                    /* scratch for the koruby calls this frame makes */
    uint32_t serial;
    korb_value **hs; uint32_t hn, hcap;
    korb_value *hs_inline[16];
    NODE *block; korb_value *def_env; korb_value *captured_self;
    struct korb_method *entry;            /* rb_call_super (NULL for allocate / new) */
    KVALUE self;
    bool kw_given;                        /* the last argument came as keywords */
};

static _Thread_local CTX *korb_cext_ctx;

#define KCX_API __attribute__((used, visibility("default")))

/* The extension sees CRuby's immediates: nil and false are koruby's swapped (CRuby
 * Qfalse = 0 is C-false, and extensions write `if (rb_obj_is_kind_of(...))`).
 * true / undef / Fixnum / Flonum / Symbol keep koruby's bits. */
#define KQfalse ((KVALUE)0x00)
#define KQnil   ((KVALUE)0x04)
#define KQtrue  ((KVALUE)0x14)
#define KQundef ((KVALUE)0x24)
static inline bool kcx_imm_p(const KVALUE v) { return (v & 7u) != 0 || v == 0; }
#define RTEST_K(h) ((((KVALUE)(h)) & ~(KVALUE)4) != 0)

static CTX *
kcx_c(void)
{
    CTX *const c = korb_cext_ctx;
    if (UNLIKELY(c == NULL)) { fprintf(stderr, "koruby: C API called outside an extension call\n"); abort(); }
    return c;
}

static struct korb_cext_vm *
kcx_vm(struct korb_vm *vm)
{
    if (!vm->cext) {
        vm->cext = calloc(1, sizeof(struct korb_cext_vm));
        if (!vm->cext) { fprintf(stderr, "koruby: oom (cext)\n"); abort(); }
    }
    return vm->cext;
}

static inline struct korb_cext_block *
kcx_block_of(const korb_value *cell)
{
    return (struct korb_cext_block *)((uintptr_t)cell & ~(uintptr_t)(KCX_BLOCK_BYTES - 1));
}
static inline uint32_t kcx_index(const korb_value *cell) { return (uint32_t)(cell - kcx_block_of(cell)->cells); }

static korb_value *
kcx_cell_alloc(struct korb_cext_vm *x)
{
    if (!x->free_list) {
        struct korb_cext_block *b = aligned_alloc(KCX_BLOCK_BYTES, KCX_BLOCK_BYTES);
        if (!b) { fprintf(stderr, "koruby: oom (cext handles)\n"); abort(); }
        memset(b, 0, sizeof *b);
        b->next = x->blocks; x->blocks = b;
        for (uint32_t i = KCX_CELLS; i-- > 0; ) { b->cells[i] = (korb_value)(uintptr_t)x->free_list; x->free_list = &b->cells[i]; }
    }
    korb_value *const cell = x->free_list;
    x->free_list = (korb_value *)(uintptr_t)*cell;
    return cell;
}

static void
kcx_cell_free(struct korb_cext_vm *x, korb_value *cell)
{
    struct korb_cext_block *const b = kcx_block_of(cell);
    const uint32_t i = kcx_index(cell);
    b->state[i] = KCX_FREE; b->refs[i] = 0; b->seen[i] = 0;
    *cell = (korb_value)(uintptr_t)x->free_list;
    x->free_list = cell;
}

static inline uint32_t kcx_hash(korb_value v) { uint64_t h = (uint64_t)v * 0x9E3779B97F4A7C15ull; return (uint32_t)(h >> 32); }

static void
kcx_map_put(struct korb_cext_vm *x, korb_value *cell)
{
    if ((x->map_n + 1) * 2 > x->map_capa) {
        const uint32_t oc = x->map_capa, nc = oc ? oc * 2 : 1024;
        korb_value **const om = x->map;
        x->map = calloc(nc, sizeof *x->map);
        if (!x->map) { fprintf(stderr, "koruby: oom (cext map)\n"); abort(); }
        x->map_capa = nc; x->map_n = 0;
        for (uint32_t i = 0; i < oc; i++) if (om[i]) kcx_map_put(x, om[i]);
        free(om);
    }
    uint32_t i = kcx_hash(*cell) & (x->map_capa - 1);
    while (x->map[i]) i = (i + 1) & (x->map_capa - 1);
    x->map[i] = cell; x->map_n++;
}

static korb_value *
kcx_map_get(const struct korb_cext_vm *x, korb_value v)
{
    if (x->map_capa == 0) return NULL;
    for (uint32_t i = kcx_hash(v) & (x->map_capa - 1); x->map[i]; i = (i + 1) & (x->map_capa - 1))
        if (*x->map[i] == v) return x->map[i];
    return NULL;
}

/* Record that frame `f` holds `cell`. */
static void
kcx_frame_hold(struct korb_cext_frame *f, korb_value *cell)
{
    struct korb_cext_block *const b = kcx_block_of(cell);
    const uint32_t i = kcx_index(cell);
    if (b->seen[i] == f->serial) return;
    b->seen[i] = f->serial;
    if (f->hn == f->hcap) {
        const uint32_t nc = f->hcap * 2;
        korb_value **const nh = malloc(nc * sizeof *nh);
        if (!nh) { fprintf(stderr, "koruby: oom (cext frame)\n"); abort(); }
        memcpy(nh, f->hs, f->hn * sizeof *nh);
        if (f->hs != f->hs_inline) free(f->hs);
        f->hs = nh; f->hcap = nc;
    }
    f->hs[f->hn++] = cell;
    b->refs[i]++;
    if (KCX_STATE(b->state[i]) == KCX_RETIRED) b->state[i] = (uint8_t)((b->state[i] & ~0x0Fu) | KCX_LOCAL);
}

/* koruby value → extension VALUE (a handle held by the current frame). */
static KVALUE
kcx_wrap(CTX *c, korb_value v)
{
    if (kcx_imm_p((KVALUE)v)) return ((KVALUE)v & ~(KVALUE)4) == 0 ? (KVALUE)v ^ 4 : (KVALUE)v;
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    korb_value *cell = kcx_map_get(x, v);
    if (!cell) {
        cell = kcx_cell_alloc(x);
        *cell = v;
        kcx_block_of(cell)->state[kcx_index(cell)] = KCX_RETIRED;
        kcx_map_put(x, cell);
    }
    if (c->cext_top) kcx_frame_hold(c->cext_top, cell);
    return (KVALUE)(uintptr_t)cell;
}

/* A handle that outlives every frame (classes, registered globals). */
static KVALUE
kcx_wrap_perm(CTX *c, korb_value v)
{
    const KVALUE h = kcx_wrap(c, v);
    if (!kcx_imm_p(h)) {
        korb_value *const cell = (korb_value *)(uintptr_t)h;
        struct korb_cext_block *const b = kcx_block_of(cell);
        b->state[kcx_index(cell)] = (uint8_t)((b->state[kcx_index(cell)] & ~0x0Fu) | KCX_PERM);
    }
    return h;
}

KVALUE rb_funcall_passing_block(KVALUE recv, KVALUE mid, int argc, const KVALUE *argv);
KVALUE rb_funcallv(KVALUE recv, KVALUE mid, int argc, const KVALUE *argv);
static inline korb_value
kcx_unwrap(KVALUE h)
{
    if (!kcx_imm_p(h)) return *(korb_value *)(uintptr_t)h;
    return (h & ~(KVALUE)4) == 0 ? (korb_value)(h ^ 4) : (korb_value)h;   /* Qfalse 0 <-> false 4, Qnil 4 <-> nil 0 */
}

static void
kcx_frame_pop(struct korb_cext_vm *x, struct korb_cext_frame *f)
{
    for (uint32_t k = 0; k < f->hn; k++) {
        korb_value *const cell = f->hs[k];
        struct korb_cext_block *const b = kcx_block_of(cell);
        const uint32_t i = kcx_index(cell);
        if (--b->refs[i] == 0 && KCX_STATE(b->state[i]) == KCX_LOCAL)
            b->state[i] = (uint8_t)((b->state[i] & ~0x0Fu) | KCX_RETIRED);
    }
    if (f->hs != f->hs_inline) free(f->hs);
}

/* ---- GC hooks (called from context.h's AROH_* macros, in the backends' TUs) ---- */
static _Thread_local struct korb_cext_vm *korb_cext_gc_vm;   /* during a collection */

static bool
kcx_handle_p(const struct korb_cext_vm *x, KVALUE h)
{
    if (kcx_imm_p(h)) return false;
    for (const struct korb_cext_block *b = x->blocks; b; b = b->next)
        if (h >= (KVALUE)(uintptr_t)b->cells && h < (KVALUE)(uintptr_t)(b->cells + KCX_CELLS)) return true;
    return false;
}

void
korb_cext_visit_roots(CTX *c, void *ctx, void (*visit)(void *, void **))
{
    struct korb_cext_vm *const x = c->vm->cext;
    for (uint32_t r = 0; r < x->reg_n; r++) {
        const KVALUE h = *x->reg[r];
        if (kcx_handle_p(x, h)) {
            korb_value *const cl = (korb_value *)(uintptr_t)h;
            kcx_block_of(cl)->state[kcx_index(cl)] |= KCX_REG;
        }
    }
    x->gc_ctx = ctx; x->gc_visit = visit;
    korb_cext_gc_vm = x;
    for (struct korb_cext_block *b = x->blocks; b; b = b->next)
        for (uint32_t i = 0; i < KCX_CELLS; i++) {
            const uint8_t st = b->state[i];
            if (KCX_STATE(st) == KCX_LOCAL || KCX_STATE(st) == KCX_PERM || (st & KCX_REG))
                ARO_GC_VISIT_EDGE(ctx, visit, &b->cells[i]);
        }
    ARO_GC_VISIT_EDGE(ctx, visit, &x->errinfo);
}

/* After a collection: retired cells follow their object or die with it; the
 * object → cell map is rebuilt (objects moved). */
void
korb_cext_weak_walk(CTX *c)
{
    struct korb_cext_vm *const x = c->vm->cext;
    if (!x) return;
    x->gc_ctx = NULL; x->gc_visit = NULL;
    korb_cext_gc_vm = NULL;
    memset(x->map, 0, x->map_capa * sizeof *x->map);
    x->map_n = 0;
    for (struct korb_cext_block *b = x->blocks; b; b = b->next)
        for (uint32_t i = 0; i < KCX_CELLS; i++) {
            const uint8_t s = b->state[i];
            if (KCX_STATE(s) == KCX_FREE) continue;
            if (KCX_STATE(s) == KCX_RETIRED && !(s & (KCX_MARKED | KCX_REG))) {
                void *const nv = aro_gc_finalize_check(c, (void *)(uintptr_t)b->cells[i]);
                if (!nv) { kcx_cell_free(x, &b->cells[i]); continue; }
                b->cells[i] = (korb_value)(uintptr_t)nv;
            }
            b->state[i] = (uint8_t)(s & 0x0Fu);
            kcx_map_put(x, &b->cells[i]);
        }
}

/* ---- T_DATA objects ------------------------------------------------------- */
/* A KorbObject (class, ivars) with the C payload after it; KORB_FL_CDATA marks it. */
typedef struct KorbCData {
    KorbObject obj;
    const struct rb_data_type_struct *type;   /* NULL: untyped Data_Wrap_Struct */
    void *data;
    void (*dmark)(void *);
    void (*dfree)(void *);
} KorbCData;

struct rb_data_type_struct {
    const char *wrap_struct_name;
    struct { void (*dmark)(void *); void (*dfree)(void *); size_t (*dsize)(const void *); void (*dcompact)(void *); void *reserved[1]; } function;
    const struct rb_data_type_struct *parent;
    void *data;
    KVALUE flags;
};
#define KCX_DEFAULT_FREE ((void (*)(void *))-1)

/* From the OBJECT scan: run the extension's dmark with rb_gc_mark wired to this collection. */
void
korb_cext_scan(void *payload, void *ctx, void (*visit)(void *, void **))
{
    KorbCData *const d = payload;
    struct korb_cext_vm *const x = korb_cext_gc_vm;
    if (!d->dmark || !d->data || !x) return;
    x->gc_ctx = ctx; x->gc_visit = visit;
    d->dmark(d->data);
}

void
korb_cext_finalize(void *payload)
{
    KorbCData *const d = payload;
    if (!d->data) return;
    if (d->dfree == KCX_DEFAULT_FREE) free(d->data);
    else if (d->dfree) d->dfree(d->data);
    d->data = NULL;
}

/* ---- frames, jumps and the trampolines ------------------------------------ */
static void
kcx_frame_push(CTX *c, struct korb_cext_frame *f, korb_value *slots)
{
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    f->prev = c->cext_top;
    f->slots = slots;
    f->serial = ++x->serial ? x->serial : ++x->serial;   /* 0 = "never seen" */
    f->hs = f->hs_inline; f->hn = 0; f->hcap = 16;
    f->block = NULL; f->def_env = NULL; f->captured_self = NULL;
    f->entry = NULL; f->self = KQnil; f->kw_given = false;
    c->cext_top = f;
}

static void
kcx_frame_leave(CTX *c, struct korb_cext_frame *f)
{
    kcx_frame_pop(c->vm->cext, f);
    c->cext_top = f->prev;
}

/* Unwind to the innermost extension entry / rb_protect with `r` (not NORMAL). */
__attribute__((noreturn)) static void
kcx_throw(CTX *c, RESULT r)
{
    struct korb_cext_jmp *const j = c->cext_jmp;
    if (!j) { fprintf(stderr, "koruby: exception in a C extension outside any entry\n"); abort(); }
    j->r = r;
    longjmp(j->jb, 1);
}

static inline korb_value
kcx_check(CTX *c, RESULT r)
{
    korb_cext_ctx = c;                        /* a green-thread switch inside may have moved it */
    if (UNLIKELY(r.state != KORB_NORMAL)) kcx_throw(c, r);
    return r.value;
}

static inline korb_value *kcx_slots(CTX *c) { return c->cext_top->slots; }

typedef KVALUE (*kcx_f0)(KVALUE);
typedef KVALUE (*kcx_fv)(int, const KVALUE *, KVALUE);

static KVALUE
kcx_call_fn(void *fn, int arity, KVALUE self, int argc, const KVALUE *a)
{
    switch (arity) {
      case -1: return ((kcx_fv)fn)(argc, a, self);
      case 0:  return ((KVALUE (*)(KVALUE))fn)(self);
      case 1:  return ((KVALUE (*)(KVALUE, KVALUE))fn)(self, a[0]);
      case 2:  return ((KVALUE (*)(KVALUE, KVALUE, KVALUE))fn)(self, a[0], a[1]);
      case 3:  return ((KVALUE (*)(KVALUE, KVALUE, KVALUE, KVALUE))fn)(self, a[0], a[1], a[2]);
      case 4:  return ((KVALUE (*)(KVALUE, KVALUE, KVALUE, KVALUE, KVALUE))fn)(self, a[0], a[1], a[2], a[3]);
      case 5:  return ((KVALUE (*)(KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE))fn)(self, a[0], a[1], a[2], a[3], a[4]);
      case 6:  return ((KVALUE (*)(KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE))fn)(self, a[0], a[1], a[2], a[3], a[4], a[5]);
      case 7:  return ((KVALUE (*)(KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE))fn)(self, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
      case 8:  return ((KVALUE (*)(KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE, KVALUE))fn)(self, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
      default: abort();
    }
}

static KVALUE kcx_ary_from(CTX *c, int argc, const KVALUE *argv);   /* fwd */
/* exported API used before its definition */
KVALUE rb_obj_alloc(KVALUE klass);
KVALUE rb_class_new_instance(int argc, const KVALUE *argv, KVALUE klass);
char  *rb_string_ptr(KVALUE str);
long   rb_string_len(KVALUE str);
KVALUE rb_obj_class(KVALUE obj);
KVALUE rb_obj_frozen_p(KVALUE obj);
KVALUE rb_obj_is_kind_of(KVALUE obj, KVALUE klass);
const char *rb_class2name(KVALUE klass);
KVALUE rb_class_path(KVALUE klass);
KVALUE rb_class_superclass(KVALUE klass);

/* Run fn (an extension method) under a frame and a jump target. */
static RESULT
korb_cext_invoke(CTX *c, korb_value *slots, korb_ref self, korb_slice args, NODE *block, korb_value *def_env,
                 korb_value *captured_self, const struct korb_cext_method *m)
{
    CTX *const saved_ctx = korb_cext_ctx;
    korb_cext_ctx = c;
    struct korb_cext_frame f;
    kcx_frame_push(c, &f, slots);
    f.block = block; f.def_env = def_env; f.captured_self = captured_self; f.entry = m->entry;
    struct korb_cext_jmp j; j.prev = c->cext_jmp; c->cext_jmp = &j;
    RESULT volatile r;
    if (setjmp(j.jb) == 0) {
        const uint32_t argc = KORB_SLICE_LEN(args);
        KVALUE av[argc ? argc : 1];
        f.self = kcx_wrap(c, KORB_REF_GET(self));
        for (uint32_t i = 0; i < argc; i++) av[i] = kcx_wrap(c, KORB_SLICE_GET(args, i));
        f.kw_given = argc > 0 && KORB_HASH_P(KORB_SLICE_GET(args, argc - 1)) &&
                     (((AroObjectHeader *)(uintptr_t)KORB_SLICE_GET(args, argc - 1))->flags & KORB_FL_KWARGS);
        KVALUE ret;
        if (m->arity == -3) ret = rb_obj_alloc(f.self);                      /* Class#allocate */
        else if (m->arity == -2) ret = ((KVALUE (*)(KVALUE, KVALUE))m->fn)(f.self, kcx_ary_from(c, (int)argc, av));
        else {
            if (m->arity >= 0 && (uint32_t)m->arity != argc)
                kcx_throw(c, korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected %d)", argc, m->arity));
            ret = kcx_call_fn(m->fn, m->arity, f.self, (int)argc, av);
        }
        r = RESULT_OK(kcx_unwrap(ret));
    } else {
        r = j.r;
    }
    c->cext_jmp = j.prev;
    kcx_frame_leave(c, &f);
    korb_cext_ctx = saved_ctx;
    return r;
}

/* One trampoline per extension method: the CFUNC ABI carries no method entry. */
#define KCX_MAX_METHODS 4096
static RESULT korb_cext_tramp_run(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *blk,
                                  korb_value *env, korb_value *cs, uint32_t idx);
/* i is a 6-digit base-4 string; pasted as hex, each nibble is one digit */
#define KCX_B4(h) ((((h) >> 20) & 15) * 1024 + (((h) >> 16) & 15) * 256 + (((h) >> 12) & 15) * 64 + \
                   (((h) >> 8) & 15) * 16 + (((h) >> 4) & 15) * 4 + ((h) & 15))
#define KCX_T(i) static RESULT kcx_t##i(CTX *c, korb_value *s, korb_ref self, korb_slice a, NODE *b, korb_value *e, korb_value *cs) \
    { return korb_cext_tramp_run(c, s, self, a, b, e, cs, KCX_B4(0x##i)); }
#define KCX_T4(i)   KCX_T(i##0) KCX_T(i##1) KCX_T(i##2) KCX_T(i##3)
#define KCX_T16(i)  KCX_T4(i##0) KCX_T4(i##1) KCX_T4(i##2) KCX_T4(i##3)
#define KCX_T64(i)  KCX_T16(i##0) KCX_T16(i##1) KCX_T16(i##2) KCX_T16(i##3)
#define KCX_T256(i) KCX_T64(i##0) KCX_T64(i##1) KCX_T64(i##2) KCX_T64(i##3)
#define KCX_T1024(i) KCX_T256(i##0) KCX_T256(i##1) KCX_T256(i##2) KCX_T256(i##3)
/* base-4 digit strings: 6 digits = 4096 trampolines */
KCX_T1024(0) KCX_T1024(1) KCX_T1024(2) KCX_T1024(3)
#define KCX_R(i) kcx_t##i,
#define KCX_R4(i)   KCX_R(i##0) KCX_R(i##1) KCX_R(i##2) KCX_R(i##3)
#define KCX_R16(i)  KCX_R4(i##0) KCX_R4(i##1) KCX_R4(i##2) KCX_R4(i##3)
#define KCX_R64(i)  KCX_R16(i##0) KCX_R16(i##1) KCX_R16(i##2) KCX_R16(i##3)
#define KCX_R256(i) KCX_R64(i##0) KCX_R64(i##1) KCX_R64(i##2) KCX_R64(i##3)
#define KCX_R1024(i) KCX_R256(i##0) KCX_R256(i##1) KCX_R256(i##2) KCX_R256(i##3)
static const korb_method_blk_fn korb_cext_tramps[KCX_MAX_METHODS] = { KCX_R1024(0) KCX_R1024(1) KCX_R1024(2) KCX_R1024(3) };

static RESULT
korb_cext_tramp_run(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *blk, korb_value *env,
                    korb_value *cs, uint32_t idx)
{
    const struct korb_cext_method *const m = &c->vm->cext->methods[idx];
    return korb_cext_invoke(c, slots, self, a, blk, env, cs, m);
}

/* ---- conversions used everywhere ------------------------------------------- */
#define K(h)   kcx_unwrap(h)
#define W(v)   kcx_wrap(c, (v))

static korb_value
kcx_send(CTX *c, korb_value recv, uint32_t mid, int argc, const KVALUE *argv)
{
    korb_value *const s = kcx_slots(c);
    s[0] = recv;
    for (int i = 0; i < argc; i++) s[1 + i] = K(argv[i]);
    return kcx_check(c, korb_send(c, s + 1 + argc, mid, 0, (uint32_t)argc));
}

static inline uint32_t kcx_id(CTX *c, const char *name) { return korb_intern(c->vm, name, strlen(name)); }

static korb_value
kcx_send_name(CTX *c, korb_value recv, const char *mid, int argc, const KVALUE *argv)
{
    return kcx_send(c, recv, kcx_id(c, mid), argc, argv);
}

static KVALUE
kcx_ary_from(CTX *c, int argc, const KVALUE *argv)
{
    korb_value *const s = kcx_slots(c);
    s[0] = kcx_check(c, korb_ary_new(c, s + 1, (uint32_t)argc));
    for (int i = 0; i < argc; i++) (void)kcx_check(c, korb_ary_push_val(c, s + 1, KORB_REF_AT(&s[0]), K(argv[i])));
    return W(s[0]);
}

/* ---- the exported C API ------------------------------------------------------ */
KCX_API KVALUE rb_mKernel, rb_mComparable, rb_mEnumerable, rb_mMath, rb_mProcess, rb_mGC;
KCX_API KVALUE rb_cBasicObject, rb_cObject, rb_cModule, rb_cClass, rb_cString, rb_cSymbol,
               rb_cArray, rb_cHash, rb_cInteger, rb_cFloat, rb_cNumeric, rb_cNilClass,
               rb_cTrueClass, rb_cFalseClass, rb_cProc, rb_cMethod, rb_cRange, rb_cIO, rb_cFile,
               rb_cTime, rb_cStruct, rb_cRational, rb_cComplex, rb_cRegexp, rb_cMatch,
               rb_cEncoding, rb_cData, rb_cThread;
KCX_API KVALUE rb_eException, rb_eStandardError, rb_eRuntimeError, rb_eArgError, rb_eTypeError,
               rb_eNameError, rb_eNoMethodError, rb_eIndexError, rb_eKeyError, rb_eRangeError,
               rb_eIOError, rb_eEOFError, rb_eLoadError, rb_eNotImpError, rb_eFrozenError,
               rb_eZeroDivError, rb_eFloatDomainError, rb_eStopIteration, rb_eSecurityError,
               rb_eSystemCallError, rb_eNoMemError, rb_eScriptError, rb_eSyntaxError,
               rb_eEncodingError, rb_eEncCompatError, rb_eInterrupt, rb_eSignal, rb_eFatal,
               rb_eSystemExit, rb_eLocalJumpError, rb_eMathDomainError, rb_eThreadError;
KCX_API KVALUE rb_eIOTimeoutError, rb_mWaitReadable, rb_mWaitWritable;

static korb_value
kcx_const_path(CTX *c, const char *path)
{
    korb_value v = korb_const_get_path(c->vm, korb_intern(c->vm, path, strlen(path)));
    return v;
}

static void
kcx_init_globals(CTX *c)
{
    static const struct { KVALUE *var; const char *name; } g[] = {
        { &rb_mKernel, "Kernel" }, { &rb_mComparable, "Comparable" }, { &rb_mEnumerable, "Enumerable" },
        { &rb_mMath, "Math" }, { &rb_mProcess, "Process" }, { &rb_mGC, "GC" },
        { &rb_cBasicObject, "BasicObject" }, { &rb_cObject, "Object" }, { &rb_cModule, "Module" },
        { &rb_cClass, "Class" }, { &rb_cString, "String" }, { &rb_cSymbol, "Symbol" }, { &rb_cArray, "Array" },
        { &rb_cHash, "Hash" }, { &rb_cInteger, "Integer" }, { &rb_cFloat, "Float" }, { &rb_cNumeric, "Numeric" },
        { &rb_cNilClass, "NilClass" }, { &rb_cTrueClass, "TrueClass" }, { &rb_cFalseClass, "FalseClass" },
        { &rb_cProc, "Proc" }, { &rb_cMethod, "Method" }, { &rb_cRange, "Range" }, { &rb_cIO, "IO" },
        { &rb_cFile, "File" }, { &rb_cTime, "Time" }, { &rb_cStruct, "Struct" }, { &rb_cRational, "Rational" },
        { &rb_cComplex, "Complex" }, { &rb_cRegexp, "Regexp" }, { &rb_cMatch, "MatchData" },
        { &rb_cEncoding, "Encoding" }, { &rb_cData, "Data" }, { &rb_cThread, "Thread" },
        { &rb_eException, "Exception" }, { &rb_eStandardError, "StandardError" }, { &rb_eRuntimeError, "RuntimeError" },
        { &rb_eArgError, "ArgumentError" }, { &rb_eTypeError, "TypeError" }, { &rb_eNameError, "NameError" },
        { &rb_eNoMethodError, "NoMethodError" }, { &rb_eIndexError, "IndexError" }, { &rb_eKeyError, "KeyError" },
        { &rb_eRangeError, "RangeError" }, { &rb_eIOError, "IOError" }, { &rb_eEOFError, "EOFError" },
        { &rb_eLoadError, "LoadError" }, { &rb_eNotImpError, "NotImplementedError" }, { &rb_eFrozenError, "FrozenError" },
        { &rb_eZeroDivError, "ZeroDivisionError" }, { &rb_eFloatDomainError, "FloatDomainError" },
        { &rb_eStopIteration, "StopIteration" }, { &rb_eSecurityError, "SecurityError" },
        { &rb_eSystemCallError, "SystemCallError" }, { &rb_eNoMemError, "NoMemoryError" },
        { &rb_eScriptError, "ScriptError" }, { &rb_eSyntaxError, "SyntaxError" }, { &rb_eEncodingError, "EncodingError" },
        { &rb_eEncCompatError, "Encoding::CompatibilityError" }, { &rb_eInterrupt, "Interrupt" },
        { &rb_eSignal, "SignalException" }, { &rb_eFatal, "Exception" }, { &rb_eSystemExit, "SystemExit" },
        { &rb_eLocalJumpError, "LocalJumpError" }, { &rb_eMathDomainError, "Math::DomainError" },
        { &rb_eThreadError, "ThreadError" }, { &rb_eIOTimeoutError, "IO::TimeoutError" },
        { &rb_mWaitReadable, "IO::WaitReadable" }, { &rb_mWaitWritable, "IO::WaitWritable" },
    };
    for (size_t i = 0; i < sizeof g / sizeof g[0]; i++) {
        korb_value v = kcx_const_path(c, g[i].name);
        *g[i].var = kcx_wrap_perm(c, v);
    }
}

/* ---- type queries ---- */
KCX_API int
rb_type(KVALUE h)
{
    const korb_value v = K(h);
    if (v == KORB_NIL) return 0x11;
    if (v == KORB_TRUE) return 0x12;
    if (v == KORB_FALSE) return 0x13;
    if (v == KORB_UNDEF) return 0x16;
    if (FIXNUM_P(v)) return 0x15;
    if (SYMBOL_P(v)) return 0x14;
    if (FLONUM_P(v)) return 0x04;
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_STRING: return 0x05;
      case KORB_OBJ_ARRAY: return 0x07;
      case KORB_OBJ_HASH: return 0x08;
      case KORB_OBJ_FLOAT: return 0x04;
      case KORB_OBJ_BIGNUM: return 0x0a;
      case KORB_OBJ_REGEXP: return 0x06;
      case KORB_OBJ_MATCHDATA: return 0x0d;
      case KORB_OBJ_RATIONAL: return 0x0f;
      case KORB_OBJ_COMPLEX: return 0x0e;
      case KORB_OBJ_CLASS: return VAL2CLASS(v)->is_module ? 0x03 : 0x02;
      case KORB_OBJ_OBJECT: {
        if (((AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_CDATA) return 0x0c;
        CTX *const c = kcx_c();                          /* an IO is a plain object here: T_FILE by class */
        return rb_cIO && korb_class_le(korb_class_obj_of(c, v), K(rb_cIO)) ? 0x0b : 0x01;
      }
      default: return 0x01;
    }
}

KCX_API void
rb_check_type(KVALUE h, int t)
{
    if (rb_type(h) == t) return;
    static const char *const names[0x20] = {
        [0x01] = "Object", [0x02] = "Class", [0x03] = "Module", [0x04] = "Float", [0x05] = "String",
        [0x06] = "Regexp", [0x07] = "Array", [0x08] = "Hash", [0x09] = "Struct", [0x0a] = "Integer",
        [0x0b] = "File", [0x0c] = "Data", [0x0d] = "MatchData", [0x0e] = "Complex", [0x0f] = "Rational",
        [0x11] = "nil", [0x12] = "true", [0x13] = "false", [0x14] = "Symbol", [0x15] = "Integer",
    };
    CTX *const c = kcx_c();
    const char *want = (t >= 0 && t < 0x20 && names[t]) ? names[t] : "unknown";
    char got[192];
    const korb_value v = K(h);
    if (v == KORB_NIL) snprintf(got, sizeof got, "nil");
    else if (v == KORB_TRUE) snprintf(got, sizeof got, "true");
    else if (v == KORB_FALSE) snprintf(got, sizeof got, "false");
    else korb_class_qname_into(c, korb_class_obj_of(c, v), got, sizeof got);
    kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_TYPE, 0, "wrong argument type %s (expected %s)", got, want));
}

/* ---- memory ---- */
KCX_API void *ruby_xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
KCX_API void *ruby_xmalloc2(size_t n, size_t size) { return ruby_xmalloc(n * size); }
KCX_API void *ruby_xcalloc(size_t n, size_t size) { void *p = calloc(n ? n : 1, size ? size : 1); if (!p) abort(); return p; }
KCX_API void *ruby_xrealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) abort(); return q; }
KCX_API void *ruby_xrealloc2(void *p, size_t n, size_t size) { return ruby_xrealloc(p, n * size); }
KCX_API void  ruby_xfree(void *p) { free(p); }

/* ---- GC ---- */
KCX_API void
rb_gc_register_address(KVALUE *addr)
{
    struct korb_cext_vm *const x = kcx_vm(kcx_c()->vm);
    for (uint32_t i = 0; i < x->reg_n; i++) if (x->reg[i] == addr) return;
    if (x->reg_n == x->reg_capa) {
        x->reg_capa = x->reg_capa ? x->reg_capa * 2 : 16;
        x->reg = realloc(x->reg, x->reg_capa * sizeof *x->reg);
        if (!x->reg) abort();
    }
    x->reg[x->reg_n++] = addr;
}
KCX_API void
rb_gc_unregister_address(KVALUE *addr)
{
    struct korb_cext_vm *const x = kcx_vm(kcx_c()->vm);
    for (uint32_t i = 0; i < x->reg_n; i++)
        if (x->reg[i] == addr) { x->reg[i] = x->reg[--x->reg_n]; return; }
}
KCX_API void rb_global_variable(KVALUE *addr) { rb_gc_register_address(addr); }
KCX_API void
rb_gc_register_mark_object(KVALUE h)
{
    if (!kcx_imm_p(h)) (void)kcx_wrap_perm(kcx_c(), K(h));
}
/* Called from a dmark: keep the cell (and its object) through this collection. */
KCX_API void
rb_gc_mark(KVALUE h)
{
    struct korb_cext_vm *const x = korb_cext_gc_vm;
    if (!x || kcx_imm_p(h) || !x->gc_visit || !kcx_handle_p(x, h)) return;
    korb_value *const cell = (korb_value *)(uintptr_t)h;
    struct korb_cext_block *const b = kcx_block_of(cell);
    const uint32_t i = kcx_index(cell);
    const uint8_t st = b->state[i];
    if (KCX_STATE(st) != KCX_RETIRED || (st & (KCX_MARKED | KCX_REG))) return;   /* roots were visited already */
    b->state[i] = (uint8_t)(st | KCX_MARKED);
    ARO_GC_VISIT_EDGE(x->gc_ctx, x->gc_visit, cell);
}
KCX_API void rb_gc_mark_movable(KVALUE h) { rb_gc_mark(h); }
KCX_API void rb_gc_mark_maybe(KVALUE h) { rb_gc_mark(h); }
KCX_API KVALUE rb_gc_location(KVALUE h) { return h; }
KCX_API void rb_gc(void) { CTX *const c = kcx_c(); (void)kcx_send_name(c, kcx_const_path(c, "GC"), "start", 0, NULL); }
KCX_API KVALUE rb_gc_start(void) { rb_gc(); return KQnil; }

/* ---- symbols ---- */
KCX_API KVALUE rb_intern2(const char *name, long len) { return (KVALUE)korb_intern(kcx_c()->vm, name, (size_t)len); }
KCX_API KVALUE rb_intern(const char *name) { return rb_intern2(name, (long)strlen(name)); }
KCX_API KVALUE rb_intern3(const char *name, long len, void *enc) { (void)enc; return rb_intern2(name, len); }
KCX_API const char *rb_id2name(KVALUE id) { return korb_sym_name(kcx_c()->vm, (uint32_t)id); }
KCX_API KVALUE rb_id2sym(KVALUE id) { return (KVALUE)ID2SYM((uint32_t)id); }
KCX_API KVALUE
rb_sym2id(KVALUE h)
{
    const korb_value v = K(h);
    if (SYMBOL_P(v)) return (KVALUE)SYM2ID(v);
    CTX *const c = kcx_c();
    kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_TYPE, 0, "not a symbol"));
}
KCX_API KVALUE
rb_id2str(KVALUE id)
{
    CTX *const c = kcx_c();
    const char *const n = korb_sym_name(c->vm, (uint32_t)id);
    return W(kcx_check(c, korb_str_new(c, kcx_slots(c), n, (uint32_t)strlen(n))));
}
KCX_API KVALUE rb_sym2str(KVALUE h) { return rb_id2str(rb_sym2id(h)); }
KCX_API KVALUE
rb_to_id(KVALUE h)
{
    CTX *const c = kcx_c();
    const korb_value v = K(h);
    if (SYMBOL_P(v)) return (KVALUE)SYM2ID(v);
    if (KORB_STRING_P(v)) return (KVALUE)korb_intern(c->vm, korb_strbuf_data(VAL2STR(v)->buf), VAL2STR(v)->len);
    const korb_value s = kcx_send_name(c, v, "to_str", 0, NULL);
    return (KVALUE)korb_intern(c->vm, korb_strbuf_data(VAL2STR(s)->buf), VAL2STR(s)->len);
}
KCX_API KVALUE rb_to_symbol(KVALUE h) { return rb_id2sym(rb_to_id(h)); }
KCX_API KVALUE rb_check_id(volatile KVALUE *namep) { return rb_to_id(*namep); }

/* ---- numbers ---- */
static RESULT kcx_raise_c(CTX *c, unsigned et, const char *msg) { return korb_raise(c, kcx_slots(c), et, 0, "%s", msg); }

/* "nil" / "true" / "false" / the class path — the name CRuby's conversion errors use */
static void
kcx_type_name(CTX *c, korb_value v, char *out, size_t sz)
{
    if (v == KORB_NIL) snprintf(out, sz, "nil");
    else if (v == KORB_TRUE) snprintf(out, sz, "true");
    else if (v == KORB_FALSE) snprintf(out, sz, "false");
    else korb_class_qname_into(c, korb_class_obj_of(c, v), out, sz);
}
/* rb_convert_type: `v.conv` if it answers, else "no implicit conversion of X into Target" */
static korb_value
kcx_convert(CTX *c, korb_value v, const char *target, const char *conv)
{
    if (v != KORB_NIL && v != KORB_TRUE && v != KORB_FALSE && korb_responds_to(c, v, kcx_id(c, conv)))
        return kcx_send_name(c, v, conv, 0, NULL);
    char nm[192]; kcx_type_name(c, v, nm, sizeof nm);
    kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_TYPE, 0, "no implicit conversion of %s into %s", nm, target));
}

KCX_API long
rb_num2long(KVALUE h)
{
    CTX *const c = kcx_c();
    korb_value v = K(h);
    if (FIXNUM_P(v)) return (long)FIX2LONG(v);
    if (KORB_FLOAT_P(v)) {
        double d; (void)korb_num_to_d(v, &d);
        if (d < 9.2233720368547758e18 && d >= -9.2233720368547758e18) return (long)d;
        kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_RANGE, 0, "float %g out of range of integer", d));
    }
    if (!KORB_BIGNUM_P(v)) v = kcx_convert(c, v, "Integer", "to_int");
    if (FIXNUM_P(v)) return (long)FIX2LONG(v);
    kcx_throw(c, kcx_raise_c(c, KORB_E_RANGE, "bignum too big to convert into 'long'"));
}
KCX_API unsigned long
rb_num2ulong(KVALUE h)
{
    CTX *const c = kcx_c();
    const korb_value v = K(h);
    if (KORB_BIGNUM_P(v)) {
        const korb_value s = kcx_send_name(c, v, "to_s", 0, NULL);
        errno = 0;
        const unsigned long long u = strtoull(korb_strbuf_data(VAL2STR(s)->buf), NULL, 10);
        if (errno || korb_strbuf_data(VAL2STR(s)->buf)[0] == '-')
            kcx_throw(c, kcx_raise_c(c, KORB_E_RANGE, "bignum out of range of unsigned long"));
        return (unsigned long)u;
    }
    return (unsigned long)rb_num2long(h);
}
KCX_API long long rb_num2ll(KVALUE h) { return rb_num2long(h); }
KCX_API unsigned long long rb_num2ull(KVALUE h) { return rb_num2ulong(h); }
KCX_API long
rb_num2int(KVALUE h)
{
    const long l = rb_num2long(h);
    if (l > INT32_MAX || l < INT32_MIN) {
        CTX *const c = kcx_c();
        kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_RANGE, 0, "integer %ld too %s to convert to 'int'", l, l < 0 ? "small" : "big"));
    }
    return l;
}
KCX_API long rb_fix2int(KVALUE h) { return rb_num2int(h); }
KCX_API unsigned long rb_num2uint(KVALUE h) { return (unsigned long)(unsigned int)rb_num2long(h); }
KCX_API double
rb_num2dbl(KVALUE h)
{
    CTX *const c = kcx_c();
    korb_value v = K(h);
    double d;
    if (FIXNUM_P(v) || KORB_FLOAT_P(v) || KORB_BIGNUM_P(v)) { (void)korb_num_to_d(v, &d); return d; }
    if (KORB_STRING_P(v) || v == KORB_NIL || v == KORB_TRUE || v == KORB_FALSE) {
        char nm[192]; kcx_type_name(c, v, nm, sizeof nm);
        kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_TYPE, 0, "no implicit conversion of %s into Float", nm));
    }
    v = kcx_send_name(c, v, "to_f", 0, NULL);
    (void)korb_num_to_d(v, &d);
    return d;
}
KCX_API KVALUE
rb_int2inum(intptr_t n)
{
    if (n >= (intptr_t)(INTPTR_MIN >> 1) && n <= (intptr_t)(INTPTR_MAX >> 1)) return (KVALUE)LONG2FIX(n);
    CTX *const c = kcx_c();
    char buf[32]; const int l = snprintf(buf, sizeof buf, "%ld", (long)n);
    korb_value *const s = kcx_slots(c);
    s[0] = kcx_check(c, korb_str_new(c, s + 1, buf, (uint32_t)l));
    return W(kcx_check(c, korb_send(c, s + 1, kcx_id(c, "to_i"), 0, 0)));
}
KCX_API KVALUE
rb_uint2inum(uintptr_t n)
{
    if (n <= (uintptr_t)(INTPTR_MAX >> 1)) return (KVALUE)LONG2FIX((intptr_t)n);
    CTX *const c = kcx_c();
    char buf[32]; const int l = snprintf(buf, sizeof buf, "%lu", (unsigned long)n);
    korb_value *const s = kcx_slots(c);
    s[0] = kcx_check(c, korb_str_new(c, s + 1, buf, (uint32_t)l));
    return W(kcx_check(c, korb_send(c, s + 1, kcx_id(c, "to_i"), 0, 0)));
}
KCX_API KVALUE rb_ll2inum(long long n) { return rb_int2inum((intptr_t)n); }
KCX_API KVALUE rb_ull2inum(unsigned long long n) { return rb_uint2inum((uintptr_t)n); }
KCX_API KVALUE
rb_float_new(double d)
{
    CTX *const c = kcx_c();
    return W(kcx_check(c, korb_float_new(c, kcx_slots(c), d)));
}
KCX_API double rb_float_value(KVALUE h) { double d = 0; (void)korb_num_to_d(K(h), &d); return d; }
KCX_API KVALUE rb_Integer(KVALUE h) { CTX *const c = kcx_c(); return W(kcx_send_name(c, korb_builtin_class_obj(c->vm, KORB_C_OBJECT), "Integer", 1, &h)); }
KCX_API KVALUE rb_Float(KVALUE h) { CTX *const c = kcx_c(); return W(kcx_send_name(c, korb_builtin_class_obj(c->vm, KORB_C_OBJECT), "Float", 1, &h)); }

/* ---- strings ---- */
KCX_API KVALUE
rb_str_new(const char *ptr, long len)
{
    CTX *const c = kcx_c();
    if (len < 0) kcx_throw(c, kcx_raise_c(c, KORB_E_ARGUMENT, "negative string size (or size too big)"));
    korb_value *const s = kcx_slots(c);
    KorbString *const str = korb_str_alloc(c, s, (uint32_t)len);
    if (ptr) memcpy(korb_strbuf_data(str->buf), ptr, (size_t)len);
    else memset(korb_strbuf_data(str->buf), 0, (size_t)len);
    KORB_STR_ENC_SET((korb_value)str, KORB_ENC_BINARY);   /* CRuby: rb_str_new is ASCII-8BIT */
    return W((korb_value)str);
}
static void kcx_set_enc(KVALUE h, uint32_t idx) { KORB_STR_ENC_SET(K(h), idx); }
KCX_API KVALUE rb_str_new_cstr(const char *ptr) { return rb_str_new(ptr, (long)strlen(ptr)); }
KCX_API KVALUE rb_usascii_str_new(const char *ptr, long len) { const KVALUE r = rb_str_new(ptr, len); kcx_set_enc(r, KORB_ENC_USASCII); return r; }
KCX_API KVALUE rb_usascii_str_new_cstr(const char *ptr) { return rb_usascii_str_new(ptr, (long)strlen(ptr)); }
KCX_API KVALUE rb_utf8_str_new(const char *ptr, long len) { const KVALUE r = rb_str_new(ptr, len); kcx_set_enc(r, KORB_ENC_UTF8); return r; }
KCX_API KVALUE rb_utf8_str_new_cstr(const char *ptr) { return rb_utf8_str_new(ptr, (long)strlen(ptr)); }
/* empty, with room for capa bytes: extensions write into RSTRING_PTR up to the capacity */
KCX_API KVALUE
rb_str_buf_new(long capa)
{
    CTX *const c = kcx_c();
    KorbString *const str = korb_str_alloc(c, kcx_slots(c), (uint32_t)(capa > 0 ? capa : 0));
    str->len = 0;
    korb_strbuf_data(str->buf)[0] = '\0';
    KORB_STR_ENC_SET((korb_value)str, KORB_ENC_BINARY);
    return W((korb_value)str);
}
KCX_API KVALUE
rb_str_cat(KVALUE str, const char *ptr, long len)
{
    CTX *const c = kcx_c();
    korb_value *const s = kcx_slots(c);
    s[0] = K(str);
    (void)kcx_check(c, korb_str_cat(c, s + 1, KORB_REF_AT(&s[0]), ptr, (uint32_t)len));
    return str;
}
KCX_API KVALUE rb_str_buf_cat(KVALUE str, const char *ptr, long len) { return rb_str_cat(str, ptr, len); }
KCX_API KVALUE rb_str_cat2(KVALUE str, const char *ptr) { return rb_str_cat(str, ptr, (long)strlen(ptr)); }
KCX_API KVALUE rb_str_buf_cat2(KVALUE str, const char *ptr) { return rb_str_cat2(str, ptr); }
KCX_API KVALUE rb_str_cat_cstr(KVALUE str, const char *ptr) { return rb_str_cat2(str, ptr); }
KCX_API KVALUE rb_str_append(KVALUE str, KVALUE str2) { CTX *const c = kcx_c(); (void)kcx_send_name(c, K(str), "<<", 1, &str2); return str; }
KCX_API KVALUE rb_str_buf_append(KVALUE str, KVALUE str2) { return rb_str_append(str, str2); }
KCX_API KVALUE rb_str_concat(KVALUE str, KVALUE obj) { return rb_str_append(str, obj); }
KCX_API KVALUE rb_str_dup(KVALUE str) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(str), "dup", 0, NULL)); }
KCX_API KVALUE rb_str_freeze(KVALUE str) { CTX *const c = kcx_c(); (void)kcx_send_name(c, K(str), "freeze", 0, NULL); return str; }
KCX_API KVALUE rb_str_plus(KVALUE a, KVALUE b) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(a), "+", 1, &b)); }
KCX_API KVALUE rb_str_times(KVALUE a, KVALUE b) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(a), "*", 1, &b)); }
KCX_API KVALUE rb_str_equal(KVALUE a, KVALUE b) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(a), "==", 1, &b)); }
KCX_API int
rb_str_cmp(KVALUE a, KVALUE b)
{
    const KorbString *const x = VAL2STR(K(a)), *const y = VAL2STR(K(b));
    const uint32_t n = x->len < y->len ? x->len : y->len;
    const int r = memcmp(korb_strbuf_data(x->buf), korb_strbuf_data(y->buf), n);
    if (r) return r < 0 ? -1 : 1;
    return x->len == y->len ? 0 : x->len < y->len ? -1 : 1;
}
KCX_API KVALUE
rb_str_substr(KVALUE str, long beg, long len)
{
    CTX *const c = kcx_c();
    const KVALUE a[2] = { (KVALUE)LONG2FIX(beg), (KVALUE)LONG2FIX(len) };
    return W(kcx_send_name(c, K(str), "byteslice", 2, a));
}
KCX_API KVALUE rb_str_intern(KVALUE str) { return rb_to_symbol(str); }
KCX_API KVALUE rb_str_inspect(KVALUE str) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(str), "inspect", 0, NULL)); }
KCX_API KVALUE
rb_str_resize(KVALUE str, long len)
{
    CTX *const c = kcx_c();
    korb_value *const s = kcx_slots(c);
    s[0] = K(str);
    KorbString *const x = korb_str_ensure(c, s + 1, KORB_REF_AT(&s[0]), (uint32_t)len);
    if ((uint32_t)len > x->len) memset(korb_strbuf_data(x->buf) + x->len, 0, (size_t)len - x->len);
    x->len = (uint32_t)len;
    korb_strbuf_data(x->buf)[len] = '\0';
    return str;
}
KCX_API void
rb_str_set_len(KVALUE str, long len)
{
    KorbString *const x = VAL2STR(K(str));
    x->len = (uint32_t)len;
    korb_strbuf_data(x->buf)[len] = '\0';
}
KCX_API KVALUE
rb_check_string_type(KVALUE h)
{
    CTX *const c = kcx_c();
    const korb_value v = K(h);
    if (KORB_STRING_P(v)) return h;
    if (!korb_responds_to(c, v, kcx_id(c, "to_str"))) return KQnil;
    return W(kcx_send_name(c, v, "to_str", 0, NULL));
}
KCX_API KVALUE
rb_str_to_str(KVALUE h)
{
    CTX *const c = kcx_c();
    const KVALUE r = rb_check_string_type(h);
    if (r != KQnil) return r;
    char nm[192];
    const korb_value v = K(h);
    if (v == KORB_NIL) snprintf(nm, sizeof nm, "nil");
    else if (v == KORB_TRUE) snprintf(nm, sizeof nm, "true");
    else if (v == KORB_FALSE) snprintf(nm, sizeof nm, "false");
    else korb_class_qname_into(c, korb_class_obj_of(c, v), nm, sizeof nm);
    kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_TYPE, 0, "no implicit conversion of %s into String", nm));
}
KCX_API KVALUE rb_string_value(volatile KVALUE *ptr) { if (!KORB_STRING_P(K(*ptr))) *ptr = rb_str_to_str(*ptr); return *ptr; }
KCX_API ARO_BORROW char *rb_string_value_ptr(volatile KVALUE *ptr) { return rb_string_ptr(rb_string_value(ptr)); }
KCX_API ARO_BORROW char *rb_string_value_cstr(volatile KVALUE *ptr)
{
    const KVALUE s = rb_string_value(ptr);
    char *const p = rb_string_ptr(s);
    const long n = rb_string_len(s);
    if ((long)strlen(p) != n) { CTX *const c = kcx_c(); kcx_throw(c, kcx_raise_c(c, KORB_E_ARGUMENT, "string contains null byte")); }
    return p;
}
KCX_API KVALUE rb_obj_as_string(KVALUE h) { CTX *const c = kcx_c(); const korb_value v = K(h); return KORB_STRING_P(v) ? h : W(kcx_send_name(c, v, "to_s", 0, NULL)); }

/* RSTRING_PTR: move the bytes into the non-moving space once; later calls
 * return the same pointer until the string reallocates them (as CRuby's) —
 * stable across collections, but still a borrow of the string's bytes. */
KCX_API ARO_BORROW char *rb_string_ptr(KVALUE h)
{
    CTX *const c = kcx_c();
    korb_value *const cell = (korb_value *)(uintptr_t)h;
    KorbString *s = VAL2STR(*cell);
    if (aro_gc_pinned_p(s->buf)) return korb_strbuf_data(s->buf);
    if (c->cext_top) kcx_frame_hold(c->cext_top, cell);       /* a root across the copy */
    korb_value *const slots = kcx_slots(c);
    c->slots_top = slots;
    if (slots > c->slots_high_water) c->slots_high_water = slots;
    KorbStrBuf *const nb = aro_gc_alloc_byte_pinned_raw(c, sizeof(KorbStrBuf) + s->capa + 1);
    if (!nb) kcx_throw(c, kcx_raise_c(c, KORB_E_NOTIMPL, "C extensions need the copy GC (BARUBY_GC=5)"));
    nb->head.flags = KORB_OBJ_STR_BUF;
    s = VAL2STR(*cell);                                       /* the alloc may have collected */
    memcpy(korb_strbuf_data(nb), korb_strbuf_data(s->buf), (size_t)s->len + 1);
    ARO_STORE(c, s, (korb_value *)(uintptr_t)&s->buf, (korb_value)(uintptr_t)nb);
    return korb_strbuf_data(nb);
}
KCX_API long rb_string_len(KVALUE h) { return (long)VAL2STR(K(h))->len; }

/* ---- formatting (rb_sprintf and the raise family) ---- */
static void
kcx_buf_put(char **b, size_t *n, size_t *cap, const char *p, size_t len)
{
    if (*n + len + 1 > *cap) {
        while (*n + len + 1 > *cap) *cap = *cap ? *cap * 2 : 128;
        *b = ruby_xrealloc(*b, *cap);
    }
    memcpy(*b + *n, p, len); *n += len; (*b)[*n] = '\0';
}

/* printf with PRIsVALUE ("%li\v": to_s, "%+li\v": inspect).  Returns malloc'd bytes. */
static char *
kcx_vformat(CTX *c, const char *fmt, va_list ap, size_t *outlen)
{
    char *b = NULL; size_t n = 0, cap = 0;
    kcx_buf_put(&b, &n, &cap, "", 0);
    for (const char *p = fmt; *p; ) {
        if (*p != '%') { const char *q = strchr(p, '%'); size_t l = q ? (size_t)(q - p) : strlen(p); kcx_buf_put(&b, &n, &cap, p, l); p += l; continue; }
        const char *const start = p++;
        if (*p == '%') { kcx_buf_put(&b, &n, &cap, "%", 1); p++; continue; }
        bool plus = false;
        while (strchr("-+ #0", *p)) { if (*p == '+') plus = true; p++; }
        int width = -1, prec = -1;
        if (*p == '*') { width = va_arg(ap, int); p++; } else while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') { p++; if (*p == '*') { prec = va_arg(ap, int); p++; } else while (*p >= '0' && *p <= '9') p++; }
        char len[3] = { 0 };
        if (*p == 'h' || *p == 'l') { len[0] = *p++; if (*p == len[0]) len[1] = *p++; }
        else if (strchr("zjtLq", *p)) len[0] = *p++;
        const char conv = *p ? *p++ : '\0';
        if (conv == 'i' && len[0] == 'l' && !len[1] && *p == '\v') {         /* PRIsVALUE */
            p++;
            const KVALUE h = va_arg(ap, KVALUE);
            const korb_value v = kcx_send_name(c, K(h), plus ? "inspect" : "to_s", 0, NULL);
            if (KORB_STRING_P(v)) kcx_buf_put(&b, &n, &cap, korb_strbuf_data(VAL2STR(v)->buf), VAL2STR(v)->len);
            continue;
        }
        char spec[64]; size_t sl = (size_t)(p - start); if (sl >= sizeof spec) sl = sizeof spec - 1;
        memcpy(spec, start, sl); spec[sl] = '\0';
        char tmp[512]; int w = 0;
        #define KCX_FMT(T) do { T a_ = va_arg(ap, T); \
            if (width >= 0 && prec >= 0) w = snprintf(tmp, sizeof tmp, spec, width, prec, a_); \
            else if (width >= 0 || prec >= 0) w = snprintf(tmp, sizeof tmp, spec, width >= 0 ? width : prec, a_); \
            else w = snprintf(tmp, sizeof tmp, spec, a_); } while (0)
        switch (conv) {
          case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'c':
            if (len[0] == 'l' && len[1] == 'l') KCX_FMT(long long);
            else if (len[0] == 'l') KCX_FMT(long);
            else if (len[0] == 'z') KCX_FMT(size_t);
            else if (len[0] == 'j') KCX_FMT(intmax_t);
            else if (len[0] == 't') KCX_FMT(ptrdiff_t);
            else if (len[0] == 'q') KCX_FMT(long long);
            else KCX_FMT(int);
            break;
          case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            if (len[0] == 'L') KCX_FMT(long double); else KCX_FMT(double);
            break;
          case 's': {
            const char *a = va_arg(ap, const char *);
            if (!a) a = "(null)";
            if (width < 0 && prec < 0 && !strpbrk(spec + 1, "0123456789.")) { kcx_buf_put(&b, &n, &cap, a, strlen(a)); continue; }
            if (width >= 0 && prec >= 0) w = snprintf(tmp, sizeof tmp, spec, width, prec, a);
            else if (width >= 0 || prec >= 0) w = snprintf(tmp, sizeof tmp, spec, width >= 0 ? width : prec, a);
            else w = snprintf(tmp, sizeof tmp, spec, a);
            break;
          }
          case 'p': KCX_FMT(void *); break;
          default: kcx_buf_put(&b, &n, &cap, spec, sl); continue;
        }
        #undef KCX_FMT
        if (w > 0) kcx_buf_put(&b, &n, &cap, tmp, (size_t)w < sizeof tmp ? (size_t)w : sizeof tmp - 1);
    }
    *outlen = n;
    return b;
}

static korb_value
kcx_vformat_str(CTX *c, const char *fmt, va_list ap)
{
    size_t n;
    char *const b = kcx_vformat(c, fmt, ap, &n);
    const korb_value s = kcx_check(c, korb_str_new(c, kcx_slots(c), b, (uint32_t)n));
    free(b);
    return s;
}

KCX_API KVALUE
rb_vsprintf(const char *fmt, va_list ap)
{
    CTX *const c = kcx_c();
    return W(kcx_vformat_str(c, fmt, ap));
}
KCX_API KVALUE rb_sprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); const KVALUE r = rb_vsprintf(fmt, ap); va_end(ap); return r; }
KCX_API KVALUE
rb_str_catf(KVALUE str, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); const KVALUE s = rb_vsprintf(fmt, ap); va_end(ap);
    return rb_str_append(str, s);
}

/* ---- exceptions ---- */
KCX_API __attribute__((noreturn)) void
rb_exc_raise(KVALUE exc)
{
    CTX *const c = kcx_c();
    const korb_value e = K(exc);
    (void)kcx_send_name(c, e, "raise", 1, &exc);
    kcx_throw(c, RESULT_RAISE_(e));       /* raise always unwinds; not reached */
}
KCX_API KVALUE rb_exc_new_str(KVALUE klass, KVALUE str) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(klass), "new", 1, &str)); }
KCX_API KVALUE rb_exc_new(KVALUE klass, const char *ptr, long len) { return rb_exc_new_str(klass, rb_utf8_str_new(ptr, len)); }
KCX_API KVALUE rb_exc_new_cstr(KVALUE klass, const char *ptr) { return rb_exc_new(klass, ptr, (long)strlen(ptr)); }

static __attribute__((noreturn)) void
kcx_vraise(KVALUE klass, const char *fmt, va_list ap)
{
    CTX *const c = kcx_c();
    const KVALUE msg = W(kcx_vformat_str(c, fmt, ap));
    KORB_STR_ENC_SET(K(msg), KORB_ENC_UTF8);
    rb_exc_raise(rb_exc_new_str(klass, msg));
}
KCX_API __attribute__((noreturn)) void rb_raise(KVALUE klass, const char *fmt, ...) { va_list ap; va_start(ap, fmt); kcx_vraise(klass, fmt, ap); }
KCX_API __attribute__((noreturn)) void rb_fatal(const char *fmt, ...) { va_list ap; va_start(ap, fmt); kcx_vraise(rb_eFatal, fmt, ap); }
KCX_API __attribute__((noreturn)) void
rb_bug(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[BUG] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    abort();
}
KCX_API __attribute__((noreturn)) void
rb_syserr_fail(int err, const char *msg)
{
    CTX *const c = kcx_c();
    const KVALUE a[2] = { msg ? rb_str_new_cstr(msg) : KQnil, (KVALUE)LONG2FIX(err) };
    rb_exc_raise(W(kcx_send_name(c, K(rb_eSystemCallError), "new", 2, a)));
}
KCX_API __attribute__((noreturn)) void rb_sys_fail(const char *msg) { rb_syserr_fail(errno, msg); }
KCX_API __attribute__((noreturn)) void
rb_sys_fail_str(KVALUE msg)
{
    const int e = errno;
    CTX *const c = kcx_c();
    const KVALUE a[2] = { msg, (KVALUE)LONG2FIX(e) };
    rb_exc_raise(W(kcx_send_name(c, K(rb_eSystemCallError), "new", 2, a)));
}
KCX_API __attribute__((noreturn)) void rb_notimplement(void) { rb_raise(rb_eNotImpError, "function is unimplemented on this machine"); }
KCX_API __attribute__((noreturn)) void rb_memerror(void) { rb_raise(rb_eNoMemError, "failed to allocate memory"); }
KCX_API __attribute__((noreturn)) void rb_num_zerodiv(void) { rb_raise(rb_eZeroDivError, "divided by 0"); }
KCX_API __attribute__((noreturn)) void
rb_error_frozen_object(KVALUE obj)
{
    rb_raise(rb_eFrozenError, "can't modify frozen %" "l" "i" "\v" ": %" "+l" "i" "\v", rb_obj_class(obj), obj);
}
KCX_API __attribute__((noreturn)) void
rb_frozen_error_raise(KVALUE obj, const char *fmt, ...)
{
    (void)obj;
    va_list ap; va_start(ap, fmt); kcx_vraise(rb_eFrozenError, fmt, ap);
}
KCX_API void
rb_check_frozen(KVALUE obj)
{
    if (!kcx_imm_p(obj) && RTEST_K(rb_obj_frozen_p(obj))) rb_error_frozen_object(obj);
}
KCX_API __attribute__((noreturn)) void rb_iter_break(void) { rb_raise(rb_eNotImpError, "rb_iter_break is not supported by koruby yet"); }
KCX_API __attribute__((noreturn)) void rb_iter_break_value(KVALUE v) { (void)v; rb_iter_break(); }
KCX_API __attribute__((noreturn)) void
rb_throw_obj(KVALUE tag, KVALUE val)
{
    CTX *const c = kcx_c();
    const KVALUE a[2] = { tag, val };
    (void)kcx_send_name(c, korb_builtin_class_obj(c->vm, KORB_C_OBJECT), "throw", 2, a);
    abort();
}
KCX_API __attribute__((noreturn)) void rb_throw(const char *tag, KVALUE val) { rb_throw_obj(rb_id2sym(rb_intern(tag)), val); }
KCX_API __attribute__((noreturn)) void
rb_exit(int status)
{
    CTX *const c = kcx_c();
    const KVALUE a[1] = { (KVALUE)LONG2FIX(status) };
    (void)kcx_send_name(c, korb_builtin_class_obj(c->vm, KORB_C_OBJECT), "exit", 1, a);
    abort();
}
KCX_API __attribute__((noreturn)) void
rb_error_arity(int argc, int min, int max)
{
    CTX *const c = kcx_c();
    if (min == max) kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_ARGUMENT, 0, "wrong number of arguments (given %d, expected %d)", argc, min));
    if (max < 0)    kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_ARGUMENT, 0, "wrong number of arguments (given %d, expected %d+)", argc, min));
    kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_ARGUMENT, 0, "wrong number of arguments (given %d, expected %d..%d)", argc, min, max));
}
KCX_API void rb_check_arity(int argc, int min, int max) { if (argc < min || (max >= 0 && argc > max)) rb_error_arity(argc, min, max); }

static void
kcx_vwarn(const char *fmt, va_list ap)
{
    CTX *const c = kcx_c();
    size_t n;
    char *const b = kcx_vformat(c, fmt, ap, &n);
    const KVALUE s = rb_utf8_str_new_cstr("warning: ");
    (void)rb_str_cat(s, b, (long)n);
    (void)rb_str_cat(s, "\n", 1);
    free(b);
    (void)kcx_send_name(c, korb_const_get(c->vm, korb_intern(c->vm, "$stderr", 7)), "write", 1, &s);
}
KCX_API void rb_warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); kcx_vwarn(fmt, ap); va_end(ap); }
KCX_API void
rb_warning(const char *fmt, ...)
{
    CTX *const c = kcx_c();
    if (korb_const_get(c->vm, korb_intern(c->vm, "$VERBOSE", 8)) != KORB_TRUE) return;
    va_list ap; va_start(ap, fmt); kcx_vwarn(fmt, ap); va_end(ap);
}

KCX_API KVALUE
rb_errinfo(void)
{
    CTX *const c = kcx_c();
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    return x->errinfo == KORB_NIL ? KQnil : W(x->errinfo);
}
KCX_API void rb_set_errinfo(KVALUE err) { kcx_vm(kcx_c()->vm)->errinfo = K(err); }

/* rb_protect's state is the koruby unwind state (RAISE = 2, ...); 0 = none. */

#define KCX_TRY(c, j) do { (j).prev = (c)->cext_jmp; (c)->cext_jmp = &(j); } while (0)
#define KCX_END(c, j) ((c)->cext_jmp = (j).prev)

KCX_API KVALUE
rb_protect(KVALUE (*func)(KVALUE), KVALUE arg, int *state)
{
    CTX *const c = kcx_c();
    struct korb_cext_frame *const f = c->cext_top;
    struct korb_cext_jmp j;
    KCX_TRY(c, j);
    if (setjmp(j.jb) == 0) {
        const KVALUE r = func(arg);
        KCX_END(c, j);
        if (state) *state = 0;
        return r;
    }
    KCX_END(c, j);
    c->cext_top = f;                      /* inner frames were popped by their own entries */
    korb_cext_ctx = c;
    kcx_vm(c->vm)->errinfo = j.r.value;
    kcx_vm(c->vm)->pending_state = j.r.state;
    if (state) *state = (int)j.r.state;
    return KQnil;
}
KCX_API __attribute__((noreturn)) void
rb_jump_tag(int state)
{
    CTX *const c = kcx_c();
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    const RESULT r = { x->errinfo, (uintptr_t)(state ? (uintptr_t)state : x->pending_state) };
    if (r.state == KORB_RAISE) x->errinfo = KORB_NIL;
    kcx_throw(c, r);
}
KCX_API KVALUE
rb_ensure(KVALUE (*b_proc)(KVALUE), KVALUE data1, KVALUE (*e_proc)(KVALUE), KVALUE data2)
{
    CTX *const c = kcx_c();
    struct korb_cext_frame *const f = c->cext_top;
    struct korb_cext_jmp j;
    KCX_TRY(c, j);
    if (setjmp(j.jb) == 0) {
        const KVALUE r = b_proc(data1);
        KCX_END(c, j);
        e_proc(data2);
        return r;
    }
    KCX_END(c, j);
    c->cext_top = f; korb_cext_ctx = c;
    const KVALUE held = W(j.r.value);     /* a root across the ensure body */
    e_proc(data2);
    kcx_throw(c, (RESULT){ K(held), j.r.state });
}
static KVALUE
kcx_rescue(KVALUE (*b_proc)(KVALUE), KVALUE data1, KVALUE (*r_proc)(KVALUE, KVALUE), KVALUE data2,
           const KVALUE *classes, int ncls)
{
    CTX *const c = kcx_c();
    struct korb_cext_frame *const f = c->cext_top;
    struct korb_cext_jmp j;
    KCX_TRY(c, j);
    if (setjmp(j.jb) == 0) {
        const KVALUE r = b_proc(data1);
        KCX_END(c, j);
        return r;
    }
    KCX_END(c, j);
    c->cext_top = f; korb_cext_ctx = c;
    if (j.r.state != KORB_RAISE) kcx_throw(c, j.r);
    const KVALUE exc = W(j.r.value);
    bool hit = false;
    for (int i = 0; i < ncls && !hit; i++) hit = RTEST_K(rb_obj_is_kind_of(exc, classes[i]));
    if (!hit) kcx_throw(c, (RESULT){ K(exc), KORB_RAISE });
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    const korb_value saved = x->errinfo;
    x->errinfo = K(exc);
    const KVALUE r = r_proc ? r_proc(data2, exc) : KQnil;
    x->errinfo = saved;
    return r;
}
KCX_API KVALUE
rb_rescue2(KVALUE (*b_proc)(KVALUE), KVALUE data1, KVALUE (*r_proc)(KVALUE, KVALUE), KVALUE data2, ...)
{
    KVALUE cls[16]; int n = 0;
    va_list ap; va_start(ap, data2);
    for (KVALUE k; n < 16 && (k = va_arg(ap, KVALUE)) != 0; ) cls[n++] = k;
    va_end(ap);
    return kcx_rescue(b_proc, data1, r_proc, data2, cls, n);
}
KCX_API KVALUE
rb_rescue(KVALUE (*b_proc)(KVALUE), KVALUE data1, KVALUE (*r_proc)(KVALUE, KVALUE), KVALUE data2)
{
    const KVALUE cls[1] = { rb_eStandardError };
    return kcx_rescue(b_proc, data1, r_proc, data2, cls, 1);
}

/* ---- arrays / hashes ---- */
KCX_API KVALUE rb_ary_new_capa(long capa) { CTX *const c = kcx_c(); return W(kcx_check(c, korb_ary_new(c, kcx_slots(c), (uint32_t)(capa < 0 ? 0 : capa)))); }
KCX_API KVALUE rb_ary_new(void) { return rb_ary_new_capa(0); }
KCX_API KVALUE rb_ary_new_from_values(long n, const KVALUE *elts) { return kcx_ary_from(kcx_c(), (int)n, elts); }
KCX_API KVALUE
rb_ary_new_from_args(long n, ...)
{
    KVALUE a[n > 0 ? n : 1];
    va_list ap; va_start(ap, n);
    for (long i = 0; i < n; i++) a[i] = va_arg(ap, KVALUE);
    va_end(ap);
    return rb_ary_new_from_values(n, a);
}
KCX_API KVALUE
rb_ary_push(KVALUE ary, KVALUE item)
{
    CTX *const c = kcx_c();
    korb_value *const s = kcx_slots(c);
    s[0] = K(ary);
    (void)kcx_check(c, korb_ary_push_val(c, s + 1, KORB_REF_AT(&s[0]), K(item)));
    return ary;
}
KCX_API long rb_array_len(KVALUE ary) { return (long)VAL2ARY(K(ary))->len; }
KCX_API KVALUE
rb_ary_entry(KVALUE ary, long idx)
{
    CTX *const c = kcx_c();
    const korb_value v = K(ary);
    const KorbArray *const a = VAL2ARY(v);
    if (idx < 0) idx += (long)a->len;
    if (idx < 0 || idx >= (long)a->len) return KQnil;
    return W(korb_items_data(a->items)[idx]);
}
KCX_API void rb_ary_store(KVALUE ary, long idx, KVALUE val) { CTX *const c = kcx_c(); const KVALUE a[2] = { (KVALUE)LONG2FIX(idx), val }; (void)kcx_send_name(c, K(ary), "[]=", 2, a); }
#define KCX_SEND0(fn, name) KCX_API KVALUE fn(KVALUE o) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(o), name, 0, NULL)); }
#define KCX_SEND1(fn, name) KCX_API KVALUE fn(KVALUE o, KVALUE a) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(o), name, 1, &a)); }
KCX_SEND0(rb_ary_pop, "pop")
KCX_SEND0(rb_ary_shift, "shift")
KCX_SEND1(rb_ary_unshift, "unshift")
KCX_SEND0(rb_ary_dup, "dup")
KCX_SEND1(rb_ary_join, "join")
KCX_SEND1(rb_ary_concat, "concat")
KCX_SEND0(rb_ary_clear, "clear")
KCX_SEND1(rb_ary_includes, "include?")
KCX_SEND1(rb_hash_aref, "[]")
KCX_SEND1(rb_hash_fetch, "fetch")
KCX_SEND1(rb_hash_delete, "delete")
KCX_SEND0(rb_hash_clear, "clear")
KCX_SEND0(rb_hash_size, "size")
KCX_SEND0(rb_hash_keys, "keys")
KCX_SEND0(rb_inspect, "inspect")
KCX_SEND0(rb_obj_dup, "dup")
KCX_SEND0(rb_obj_clone, "clone")
KCX_SEND0(rb_obj_id, "object_id")
KCX_SEND0(rb_singleton_class, "singleton_class")
KCX_SEND0(rb_class_name, "name")
KCX_SEND1(rb_equal, "==")
KCX_SEND1(rb_eql, "eql?")
KCX_SEND1(rb_class_inherited_p, "<=")
KCX_SEND1(rb_obj_method, "method")
KCX_API KVALUE rb_hash_new_capa(long capa) { CTX *const c = kcx_c(); return W(kcx_check(c, korb_hash_new(c, kcx_slots(c), (uint32_t)(capa < 0 ? 0 : capa)))); }
KCX_API KVALUE rb_hash_new(void) { return rb_hash_new_capa(0); }
KCX_API KVALUE rb_hash_aset(KVALUE h, KVALUE k, KVALUE v) { CTX *const c = kcx_c(); const KVALUE a[2] = { k, v }; (void)kcx_send_name(c, K(h), "[]=", 2, a); return v; }
KCX_API KVALUE rb_hash_lookup2(KVALUE h, KVALUE k, KVALUE def) { CTX *const c = kcx_c(); const KVALUE a[2] = { k, def }; return W(kcx_send_name(c, K(h), "fetch", 2, a)); }
KCX_API KVALUE rb_hash_lookup(KVALUE h, KVALUE k) { return rb_hash_lookup2(h, k, KQnil); }
KCX_API size_t rb_hash_size_num(KVALUE h) { return (size_t)VAL2HASH(K(h))->len; }
KCX_API void
rb_hash_foreach(KVALUE h, int (*func)(KVALUE, KVALUE, KVALUE), KVALUE arg)
{
    CTX *const c = kcx_c();
    const KVALUE pairs = W(kcx_send_name(c, K(h), "to_a", 0, NULL));
    const long n = rb_array_len(pairs);
    for (long i = 0; i < n; i++) {
        const KVALUE pr = rb_ary_entry(pairs, i);
        const KVALUE k = rb_ary_entry(pr, 0), v = rb_ary_entry(pr, 1);
        const int st = func(k, v, arg);
        if (st == 1) break;                   /* ST_STOP */
        if (st == 2) (void)rb_hash_delete(h, k);   /* ST_DELETE */
    }
}
static KVALUE
kcx_check_conv(KVALUE h, bool (*is)(korb_value), const char *conv)
{
    CTX *const c = kcx_c();
    const korb_value v = K(h);
    if (is(v)) return h;
    if (!korb_responds_to(c, v, kcx_id(c, conv))) return KQnil;
    return W(kcx_send_name(c, v, conv, 0, NULL));
}
static bool kcx_is_ary(korb_value v) { return KORB_ARRAY_P(v); }
static bool kcx_is_hash(korb_value v) { return KORB_HASH_P(v); }
KCX_API KVALUE rb_check_array_type(KVALUE h) { return kcx_check_conv(h, kcx_is_ary, "to_ary"); }
KCX_API KVALUE rb_check_hash_type(KVALUE h) { return kcx_check_conv(h, kcx_is_hash, "to_hash"); }
KCX_API KVALUE rb_Array(KVALUE h) { CTX *const c = kcx_c(); return W(kcx_send_name(c, korb_builtin_class_obj(c->vm, KORB_C_OBJECT), "Array", 1, &h)); }

/* ---- classes, modules, methods ---- */
static KVALUE
kcx_define_mod(KVALUE outer, const char *name, KVALUE super, bool is_module)
{
    CTX *const c = kcx_c();
    const KVALUE sym = rb_id2sym(rb_intern(name));
    const KVALUE q[2] = { sym, KQfalse };
    if (RTEST_K(W(kcx_send_name(c, K(outer), "const_defined?", 2, q)))) {
        const KVALUE k = W(kcx_send_name(c, K(outer), "const_get", 2, q));
        const korb_value kv = K(k);
        if (!KORB_CLASS_P(kv) || (bool)VAL2CLASS(kv)->is_module != is_module) {
            char nm[192]; kcx_type_name(c, kv, nm, sizeof nm);
            if (K(outer) == K(rb_cObject)) rb_raise(rb_eTypeError, "%s is not a %s (%s)", name, is_module ? "module" : "class", nm);
            rb_raise(rb_eTypeError, "%" "l" "i" "\v" "::%s is not a %s (%s)", rb_class_path(outer), name, is_module ? "module" : "class", nm);
        }
        if (!is_module && super && RTEST_K(rb_equal(rb_class_superclass(k), super)) == false)   /* CRuby's argument order */
            rb_raise(rb_eTypeError, "superclass mismatch for class %" "l" "i" "\v" " (%" "l" "i" "\v" " is given but was %" "l" "i" "\v" ")",
                     rb_class_path(k), rb_class_superclass(k), super);
        return kcx_wrap_perm(c, K(k));
    }
    const KVALUE k = is_module ? W(kcx_send_name(c, K(rb_cModule), "new", 0, NULL))
                               : W(kcx_send_name(c, K(rb_cClass), "new", 1, &super));
    const KVALUE a[2] = { sym, k };
    (void)kcx_send_name(c, K(outer), "const_set", 2, a);
    return kcx_wrap_perm(c, K(k));
}
KCX_API KVALUE rb_define_class_under(KVALUE outer, const char *name, KVALUE super) { return kcx_define_mod(outer, name, super, false); }
KCX_API KVALUE rb_define_module_under(KVALUE outer, const char *name) { return kcx_define_mod(outer, name, 0, true); }
KCX_API KVALUE rb_define_class(const char *name, KVALUE super) { return kcx_define_mod(rb_cObject, name, super, false); }
KCX_API KVALUE rb_define_module(const char *name) { return kcx_define_mod(rb_cObject, name, 0, true); }
KCX_API KVALUE rb_define_class_id_under(KVALUE outer, KVALUE id, KVALUE super) { return kcx_define_mod(outer, rb_id2name(id), super, false); }
KCX_API KVALUE rb_define_module_id_under(KVALUE outer, KVALUE id) { return kcx_define_mod(outer, rb_id2name(id), 0, true); }
KCX_API void rb_include_module(KVALUE k, KVALUE m) { CTX *const c = kcx_c(); (void)kcx_send_name(c, K(k), "include", 1, &m); }
KCX_API void rb_prepend_module(KVALUE k, KVALUE m) { CTX *const c = kcx_c(); (void)kcx_send_name(c, K(k), "prepend", 1, &m); }
KCX_API void rb_extend_object(KVALUE o, KVALUE m) { CTX *const c = kcx_c(); (void)kcx_send_name(c, K(o), "extend", 1, &m); }

static void
kcx_def(KVALUE klass, const char *name, void *fn, int arity, uint8_t vis)
{
    CTX *const c = kcx_c();
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    if (arity < -2 || arity > 8) rb_raise(rb_eArgError, "arity out of range: %d for -2..8", arity);
    if (!x->methods) x->methods = calloc(KCX_MAX_METHODS, sizeof *x->methods);
    /* one trampoline per (function, arity, class): two names for one C function in one
     * class are == (CRuby), and rb_call_super knows which method it is continuing */
    uint32_t idx = 0;
    while (idx < x->method_n && (x->methods[idx].fn != fn || x->methods[idx].arity != arity ||
                                 x->methods[idx].entry->owner != K(klass))) idx++;
    const bool fresh = idx == x->method_n;
    if (fresh) {
        if (x->method_n == KCX_MAX_METHODS) rb_raise(rb_eNotImpError, "too many C extension methods (koruby limit %d)", KCX_MAX_METHODS);
        x->method_n++;
        x->methods[idx].fn = fn; x->methods[idx].arity = arity;
    }
    korb_class_def_cfn_blk(c, K(klass), name, korb_cext_tramps[idx], arity >= 0 ? arity : -1);
    if (fresh) {   /* entries are libc-immortal: the first definition's is the one super continues from */
        const KorbClass *const k = VAL2CLASS(K(klass));
        x->methods[idx].entry = k->methods[korb_mtab_slot(k, korb_intern(c->vm, name, strlen(name)))];
    }
    if (vis) {   /* korb_class_method_slot would start a fresh entry: look the new one up */
        const KorbClass *const k = VAL2CLASS(K(klass));
        const int32_t sl = korb_mtab_slot(k, korb_intern(c->vm, name, strlen(name)));
        if (sl >= 0) k->methods[sl]->visibility = vis;
    }
}
KCX_API void rb_define_method(KVALUE k, const char *name, void *fn, int arity) { kcx_def(k, name, fn, arity, 0); }
KCX_API void rb_define_private_method(KVALUE k, const char *name, void *fn, int arity) { kcx_def(k, name, fn, arity, 1); }
KCX_API void rb_define_protected_method(KVALUE k, const char *name, void *fn, int arity) { kcx_def(k, name, fn, arity, 2); }
KCX_API void rb_define_method_id(KVALUE k, KVALUE mid, void *fn, int arity) { kcx_def(k, rb_id2name(mid), fn, arity, 0); }
KCX_API void
rb_define_singleton_method(KVALUE obj, const char *name, void *fn, int arity)
{
    kcx_def(kcx_wrap_perm(kcx_c(), K(rb_singleton_class(obj))), name, fn, arity, 0);
}
KCX_API void
rb_define_module_function(KVALUE mod, const char *name, void *fn, int arity)
{
    rb_define_private_method(mod, name, fn, arity);
    rb_define_singleton_method(mod, name, fn, arity);
}
KCX_API void rb_define_global_function(const char *name, void *fn, int arity) { rb_define_module_function(rb_mKernel, name, fn, arity); }
KCX_API void rb_undef_method(KVALUE k, const char *name) { CTX *const c = kcx_c(); const KVALUE s = rb_id2sym(rb_intern(name)); (void)kcx_send_name(c, K(k), "undef_method", 1, &s); }
KCX_API void
rb_alias(KVALUE k, KVALUE nid, KVALUE oid)
{
    CTX *const c = kcx_c();
    const KVALUE a[2] = { rb_id2sym(nid), rb_id2sym(oid) };
    (void)kcx_send_name(c, K(k), "alias_method", 2, a);
}
KCX_API void rb_define_alias(KVALUE k, const char *n, const char *o) { rb_alias(k, rb_intern(n), rb_intern(o)); }
KCX_API void
rb_define_attr(KVALUE k, const char *name, int read, int write)
{
    CTX *const c = kcx_c();
    const KVALUE s = rb_id2sym(rb_intern(name));
    if (read) (void)kcx_send_name(c, K(k), "attr_reader", 1, &s);
    if (write) (void)kcx_send_name(c, K(k), "attr_writer", 1, &s);
}

KCX_API void
rb_define_alloc_func(KVALUE klass, KVALUE (*func)(KVALUE))
{
    CTX *const c = kcx_c();
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    const uint32_t serial = VAL2CLASS(K(klass))->serial;
    uint32_t i = 0;
    while (i < x->alloc_n && x->allocs[i].class_serial != serial) i++;
    if (i == x->alloc_n) {
        if (x->alloc_n == x->alloc_capa) { x->alloc_capa = x->alloc_capa ? x->alloc_capa * 2 : 16; x->allocs = ruby_xrealloc(x->allocs, x->alloc_capa * sizeof *x->allocs); }
        x->alloc_n++;
    }
    x->allocs[i].class_serial = serial; x->allocs[i].fn = func;   /* korb_obj_new looks it up */
}
KCX_API void rb_undef_alloc_func(KVALUE klass) { rb_define_alloc_func(klass, NULL); }
/* korb_obj_new's hook: does klass (or an ancestor) have a C allocator (or an undef'd one)? */
static bool
korb_cext_has_alloc(CTX *c, korb_value k)
{
    const struct korb_cext_vm *const x = c->vm->cext;
    if (x->alloc_n == 0) return false;
    for (; KORB_CLASS_P(k); k = VAL2CLASS(k)->superclass)
        for (uint32_t i = 0; i < x->alloc_n; i++)
            if (x->allocs[i].class_serial == VAL2CLASS(k)->serial) return true;
    return false;
}
/* korb_obj_new for such a class: run the allocator under an extension frame */
static RESULT
korb_cext_obj_alloc(CTX *c, korb_value *slots, korb_value klass)
{
    static const struct korb_cext_method m = { NULL, -3, NULL };
    slots[0] = klass;
    return korb_cext_invoke(c, slots + 1, KORB_REF_AT(&slots[0]), KORB_SLICE_MAKE(slots, 0), NULL, NULL, NULL, &m);
}
KCX_API KVALUE
rb_obj_alloc(KVALUE klass)
{
    CTX *const c = kcx_c();
    const korb_value k = K(klass);
    const struct korb_cext_vm *const x = c->vm->cext;
    for (korb_value a = k; KORB_CLASS_P(a); a = VAL2CLASS(a)->superclass)
        for (uint32_t i = 0; x && i < x->alloc_n; i++)
            if (x->allocs[i].class_serial == VAL2CLASS(a)->serial) {
                if (!x->allocs[i].fn) rb_raise(rb_eTypeError, "allocator undefined for %s", rb_class2name(klass));
                return x->allocs[i].fn(klass);
            }
    return W(kcx_check(c, korb_obj_new(c, kcx_slots(c), k)));   /* no allocator up the chain: the hook passes it through */
}
KCX_API void
rb_obj_call_init_kw(KVALUE obj, int argc, const KVALUE *argv, int kw_splat)
{
    (void)kw_splat;
    CTX *const c = kcx_c();
    struct korb_cext_frame *const f = c->cext_top;
    korb_value *const s = kcx_slots(c);
    s[0] = K(obj);
    for (int i = 0; i < argc; i++) s[1 + i] = K(argv[i]);
    const uint32_t mid = kcx_id(c, "initialize");
    if (f->block) (void)kcx_check(c, korb_send_blk(c, s + 1 + argc, mid, 0, (uint32_t)argc, f->block, f->def_env, f->captured_self, 0, KORB_UNDEF));
    else (void)kcx_check(c, korb_send(c, s + 1 + argc, mid, 0, (uint32_t)argc));
}
KCX_API void rb_obj_call_init(KVALUE obj, int argc, const KVALUE *argv) { rb_obj_call_init_kw(obj, argc, argv, 0); }
KCX_API KVALUE
rb_class_new_instance_kw(int argc, const KVALUE *argv, KVALUE klass, int kw_splat)
{
    const KVALUE o = rb_obj_alloc(klass);
    rb_obj_call_init_kw(o, argc, argv, kw_splat);
    return o;
}
KCX_API KVALUE rb_class_new_instance(int argc, const KVALUE *argv, KVALUE klass) { return rb_class_new_instance_kw(argc, argv, klass, 0); }
KCX_API KVALUE rb_class_of(KVALUE obj) { CTX *const c = kcx_c(); return W(korb_class_obj_of(c, K(obj))); }
KCX_API KVALUE rb_obj_class(KVALUE obj) { return rb_class_of(obj); }   /* korb_class_obj_of skips singletons */
KCX_API KVALUE
rb_class_superclass(KVALUE k)
{
    CTX *const c = kcx_c();
    const korb_value v = K(k);
    if (!KORB_CLASS_P(v) || VAL2CLASS(v)->is_module) return W(kcx_send_name(c, v, "superclass", 0, NULL));
    return W(VAL2CLASS(v)->superclass);
}
KCX_API KVALUE
rb_class_path(KVALUE k)
{
    const KVALUE n = rb_class_name(k);
    return n == KQnil ? rb_inspect(k) : n;
}
KCX_API const char *
rb_class2name(KVALUE k)
{
    CTX *const c = kcx_c();
    const korb_value n = K(rb_class_path(k));
    return korb_sym_name(c->vm, korb_intern(c->vm, korb_strbuf_data(VAL2STR(n)->buf), VAL2STR(n)->len));   /* interned = immortal */
}
KCX_API const char *rb_obj_classname(KVALUE o) { return rb_class2name(rb_obj_class(o)); }
/* These are C in CRuby and do not dispatch: an extension may define #freeze / #kind_of?
 * itself and call them from inside it (openssl's SSLContext#freeze is #setup, which
 * calls rb_obj_freeze).  Run koruby's builtins directly. */
static KVALUE
kcx_builtin(korb_method_fn fn, KVALUE self, int argc, const KVALUE *argv)
{
    CTX *const c = kcx_c();
    korb_value *const s = kcx_slots(c);
    for (int i = 0; i < argc; i++) s[i] = K(argv[i]);
    s[argc] = K(self);
    return W(kcx_check(c, fn(c, s + argc + 1, KORB_REF_AT(&s[argc]), KORB_SLICE_MAKE(s, (uint32_t)argc))));
}
KCX_API KVALUE rb_obj_freeze(KVALUE o) { if (!kcx_imm_p(o)) (void)kcx_builtin(korb_m_obj_freeze, o, 0, NULL); return o; }
KCX_API KVALUE rb_obj_frozen_p(KVALUE o) { return kcx_imm_p(o) ? KQtrue : kcx_builtin(korb_m_obj_frozen_q, o, 0, NULL); }
KCX_API KVALUE rb_obj_is_kind_of(KVALUE o, KVALUE k) { return kcx_builtin(korb_m_obj_is_a, o, 1, &k); }
KCX_API KVALUE rb_obj_is_instance_of(KVALUE o, KVALUE k) { CTX *const c = kcx_c(); return korb_class_obj_of(c, K(o)) == K(k) ? KQtrue : KQfalse; }
KCX_API KVALUE
rb_any_to_s(KVALUE o)
{
    CTX *const c = kcx_c();
    const KVALUE s = rb_id2sym(rb_intern("to_s"));
    const KVALUE um = W(kcx_send_name(c, K(rb_mKernel), "instance_method", 1, &s));
    return W(kcx_send_name(c, K(um), "bind_call", 1, &o));
}
KCX_API int rb_respond_to(KVALUE o, KVALUE mid) { CTX *const c = kcx_c(); return korb_responds_to(c, K(o), (uint32_t)mid); }

/* ---- ivars / constants / globals ---- */
KCX_API KVALUE rb_ivar_get(KVALUE o, KVALUE id) { CTX *const c = kcx_c(); return W(korb_ivar_get(c, K(o), ID2SYM((uint32_t)id))); }
KCX_API KVALUE
rb_ivar_set(KVALUE o, KVALUE id, KVALUE v)
{
    CTX *const c = kcx_c();
    korb_value *const s = kcx_slots(c);
    s[0] = K(o);
    (void)kcx_check(c, korb_ivar_set(c, s + 1, KORB_REF_AT(&s[0]), ID2SYM((uint32_t)id), K(v)));
    return v;
}
KCX_API KVALUE rb_ivar_defined(KVALUE o, KVALUE id) { CTX *const c = kcx_c(); return korb_ivar_defined(c, K(o), ID2SYM((uint32_t)id)) ? KQtrue : KQfalse; }
KCX_API KVALUE rb_iv_get(KVALUE o, const char *name) { return rb_ivar_get(o, rb_intern(name)); }
KCX_API KVALUE rb_iv_set(KVALUE o, const char *name, KVALUE v) { return rb_ivar_set(o, rb_intern(name), v); }
KCX_API KVALUE rb_attr_get(KVALUE o, KVALUE id) { return rb_ivar_get(o, id); }
KCX_API void rb_define_const(KVALUE m, const char *name, KVALUE v) { CTX *const c = kcx_c(); const KVALUE a[2] = { rb_id2sym(rb_intern(name)), v }; (void)kcx_send_name(c, K(m), "const_set", 2, a); }
KCX_API void rb_define_global_const(const char *name, KVALUE v) { rb_define_const(rb_cObject, name, v); }
KCX_API void rb_const_set(KVALUE m, KVALUE id, KVALUE v) { rb_define_const(m, rb_id2name(id), v); }
KCX_API KVALUE rb_const_get(KVALUE m, KVALUE id) { CTX *const c = kcx_c(); const KVALUE s = rb_id2sym(id); return W(kcx_send_name(c, K(m), "const_get", 1, &s)); }
KCX_API KVALUE rb_const_get_at(KVALUE m, KVALUE id) { CTX *const c = kcx_c(); const KVALUE a[2] = { rb_id2sym(id), KQfalse }; return W(kcx_send_name(c, K(m), "const_get", 2, a)); }
KCX_API int rb_const_defined(KVALUE m, KVALUE id) { CTX *const c = kcx_c(); const KVALUE s = rb_id2sym(id); return RTEST_K(W(kcx_send_name(c, K(m), "const_defined?", 1, &s))); }
KCX_API int rb_const_defined_at(KVALUE m, KVALUE id) { CTX *const c = kcx_c(); const KVALUE a[2] = { rb_id2sym(id), KQfalse }; return RTEST_K(W(kcx_send_name(c, K(m), "const_defined?", 2, a))); }
KCX_API KVALUE
rb_path_to_class(KVALUE path)
{
    CTX *const c = kcx_c();
    const KVALUE k = W(kcx_send_name(c, K(rb_cObject), "const_get", 1, &path));
    if (!KORB_CLASS_P(K(k))) rb_raise(rb_eTypeError, "%" "l" "i" "\v" " does not refer to class/module", path);
    return kcx_wrap_perm(c, K(k));
}
KCX_API KVALUE rb_path2class(const char *path) { return rb_path_to_class(rb_str_new_cstr(path)); }
KCX_API KVALUE rb_cvar_get(KVALUE k, KVALUE id) { CTX *const c = kcx_c(); const KVALUE s = rb_id2sym(id); return W(kcx_send_name(c, K(k), "class_variable_get", 1, &s)); }
KCX_API void rb_cvar_set(KVALUE k, KVALUE id, KVALUE v) { CTX *const c = kcx_c(); const KVALUE a[2] = { rb_id2sym(id), v }; (void)kcx_send_name(c, K(k), "class_variable_set", 2, a); }
KCX_API KVALUE rb_eval_string(const char *str) { CTX *const c = kcx_c(); const KVALUE s = rb_utf8_str_new_cstr(str); return W(kcx_send_name(c, kcx_const_path(c, "TOPLEVEL_BINDING"), "eval", 1, &s)); }
static KVALUE kcx_eval_thunk(KVALUE s) { return rb_eval_string((const char *)(uintptr_t)s); }
KCX_API KVALUE rb_eval_string_protect(const char *str, int *state) { return rb_protect(kcx_eval_thunk, (KVALUE)(uintptr_t)str, state); }
KCX_API KVALUE rb_gv_get(const char *name) { char b[256]; snprintf(b, sizeof b, "%s%s", name[0] == '$' ? "" : "$", name); return rb_eval_string(b); }
KCX_API KVALUE
rb_gv_set(const char *name, KVALUE v)
{
    CTX *const c = kcx_c();
    char b[256]; snprintf(b, sizeof b, "->(v) { %s%s = v }", name[0] == '$' ? "" : "$", name);
    const KVALUE l = rb_eval_string(b);
    (void)kcx_send_name(c, K(l), "call", 1, &v);
    return v;
}

/* ---- calls / blocks ---- */
KCX_API KVALUE rb_funcallv(KVALUE recv, KVALUE mid, int argc, const KVALUE *argv) { CTX *const c = kcx_c(); return W(kcx_send(c, K(recv), (uint32_t)mid, argc, argv)); }
KCX_API KVALUE rb_funcallv_kw(KVALUE recv, KVALUE mid, int argc, const KVALUE *argv, int kw) { (void)kw; return rb_funcallv(recv, mid, argc, argv); }
KCX_API KVALUE
rb_funcall(KVALUE recv, KVALUE mid, int n, ...)
{
    KVALUE a[n > 0 ? n : 1];
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, KVALUE);
    va_end(ap);
    return rb_funcallv(recv, mid, n, a);
}
KCX_API KVALUE
rb_funcallv_public(KVALUE recv, KVALUE mid, int argc, const KVALUE *argv)
{
    KVALUE a[argc + 1];
    a[0] = rb_id2sym(mid);
    for (int i = 0; i < argc; i++) a[1 + i] = argv[i];
    return rb_funcallv(recv, rb_intern("public_send"), argc + 1, a);
}
KCX_API KVALUE
rb_funcall_passing_block(KVALUE recv, KVALUE mid, int argc, const KVALUE *argv)
{
    CTX *const c = kcx_c();
    struct korb_cext_frame *const f = c->cext_top;
    if (!f->block) return rb_funcallv(recv, mid, argc, argv);
    korb_value *const s = kcx_slots(c);
    s[0] = K(recv);
    for (int i = 0; i < argc; i++) s[1 + i] = K(argv[i]);
    return W(kcx_check(c, korb_send_blk(c, s + 1 + argc, (uint32_t)mid, 0, (uint32_t)argc, f->block, f->def_env, f->captured_self, 0, KORB_UNDEF)));
}
KCX_API KVALUE rb_current_receiver(void) { return kcx_c()->cext_top->self; }
KCX_API int rb_block_given_p(void) { const struct korb_cext_frame *const f = kcx_c()->cext_top; return f && f->block != NULL; }
KCX_API void rb_need_block(void) { if (!rb_block_given_p()) { CTX *const c = kcx_c(); kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_LOCALJUMP, 0, "no block given")); } }
KCX_API int rb_keyword_given_p(void) { const struct korb_cext_frame *const f = kcx_c()->cext_top; return f && f->kw_given; }
KCX_API KVALUE
rb_yield_values2(int argc, const KVALUE *argv)
{
    CTX *const c = kcx_c();
    struct korb_cext_frame *const f = c->cext_top;
    if (!f || !f->block) kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_LOCALJUMP, 0, "no block given"));
    korb_value *const s = kcx_slots(c);
    for (int i = 0; i < argc; i++) s[i] = K(argv[i]);
    return W(kcx_check(c, korb_block_yield(c, s + argc, f->block, f->def_env, s, (uint32_t)argc, f->captured_self)));
}
KCX_API KVALUE rb_yield(KVALUE v) { return rb_yield_values2(1, &v); }
KCX_API KVALUE
rb_yield_values(int n, ...)
{
    KVALUE a[n > 0 ? n : 1];
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, KVALUE);
    va_end(ap);
    return rb_yield_values2(n, a);
}
KCX_API KVALUE
rb_yield_splat(KVALUE ary)
{
    const long n = rb_array_len(ary);
    KVALUE a[n > 0 ? n : 1];
    for (long i = 0; i < n; i++) a[i] = rb_ary_entry(ary, i);
    return rb_yield_values2((int)n, a);
}
KCX_API KVALUE
rb_block_proc(void)
{
    CTX *const c = kcx_c();
    struct korb_cext_frame *const f = c->cext_top;
    if (!f || !f->block) rb_raise(rb_eArgError, "tried to create Proc object without a block");
    if (f->def_env == KORB_BLK_FWD) return W(*f->captured_self);     /* a forwarded Proc */
    if (f->block == KORB_BLK_CPROC) rb_raise(rb_eNotImpError, "rb_block_proc of a C-level block is not supported by koruby yet");
    return W(kcx_check(c, korb_make_proc(c, kcx_slots(c), f->block, f->def_env, *f->captured_self, 0)));
}
KCX_API KVALUE
rb_proc_call(KVALUE proc, KVALUE args)
{
    const long n = rb_array_len(args);
    KVALUE a[n > 0 ? n : 1];
    for (long i = 0; i < n; i++) a[i] = rb_ary_entry(args, i);
    return rb_funcallv(proc, rb_intern("call"), (int)n, a);
}
KCX_API KVALUE rb_obj_is_proc(KVALUE o) { return rb_obj_is_kind_of(o, rb_cProc); }
struct rb_data_type_struct;
void  *rb_check_typeddata(KVALUE obj, const struct rb_data_type_struct *type);
KVALUE rb_data_typed_object_wrap(KVALUE klass, void *datap, const struct rb_data_type_struct *type);
/* A Proc passed as the block: the &proc path (block = its iseq, def_env = KORB_BLK_FWD). */
KCX_API KVALUE
rb_funcall_with_block(KVALUE recv, KVALUE mid, int argc, const KVALUE *argv, KVALUE procval)
{
    CTX *const c = kcx_c();
    if (procval == KQnil) return rb_funcallv(recv, mid, argc, argv);
    const korb_value pv = K(procval);
    if (!KORB_PROC_P(pv)) rb_raise(rb_eTypeError, "wrong argument type %s (expected Proc)", rb_obj_classname(procval));
    korb_value *const s = kcx_slots(c);
    s[0] = pv; s[1] = K(recv);
    for (int i = 0; i < argc; i++) s[2 + i] = K(argv[i]);
    NODE *const iseq = VAL2PROC(s[0])->iseq;
    return W(kcx_check(c, korb_send_blk(c, s + 2 + argc, (uint32_t)mid, 0, (uint32_t)argc, iseq ? iseq : KORB_BLK_CPROC,
                                        KORB_BLK_FWD, &s[0], 0, KORB_UNDEF)));
}
KCX_API KVALUE rb_funcall_with_block_kw(KVALUE recv, KVALUE mid, int argc, const KVALUE *argv, KVALUE procval, int kw)
{ (void)kw; return rb_funcall_with_block(recv, mid, argc, argv, procval); }

/* A C function as a block: a T_DATA holding (func, data2) and a Ruby proc that calls back
 * through the hidden KorubyCextBlock.call. */
struct kcx_cblock { KVALUE (*func)(KVALUE, KVALUE, int, const KVALUE *, KVALUE); KVALUE data2; };
static void kcx_cblock_mark(void *p) { rb_gc_mark(((struct kcx_cblock *)p)->data2); }
static const struct rb_data_type_struct kcx_cblock_type = { "koruby/cblock", { kcx_cblock_mark, KCX_DEFAULT_FREE, NULL, NULL, { NULL } }, NULL, NULL, 0 };
static KVALUE
kcx_cblock_call(KVALUE self, KVALUE cd, KVALUE args, KVALUE blk)
{
    (void)self;
    const struct kcx_cblock *const b = rb_check_typeddata(cd, &kcx_cblock_type);
    const long n = rb_array_len(args);
    KVALUE av[n > 0 ? n : 1];
    for (long i = 0; i < n; i++) av[i] = rb_ary_entry(args, i);
    return b->func(n > 0 ? av[0] : KQnil, b->data2, (int)n, av, blk);
}
static KVALUE
kcx_cblock_proc(KVALUE (*func)(KVALUE, KVALUE, int, const KVALUE *, KVALUE), KVALUE data2, bool lambda)
{
    CTX *const c = kcx_c();
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    if (!x->cblock_maker) {
        const KVALUE mod = rb_define_module("KorubyCextBlock");
        rb_define_singleton_method(mod, "call", kcx_cblock_call, 3);
        x->cblock_maker = rb_eval_string("[->(cd) { proc { |*a, &b| KorubyCextBlock.call(cd, a, b) } },"
                                         " ->(cd) { lambda { |*a, &b| KorubyCextBlock.call(cd, a, b) } }]");
        rb_gc_register_mark_object(x->cblock_maker);
    }
    struct kcx_cblock *const b = ruby_xcalloc(1, sizeof *b);
    b->func = func; b->data2 = data2;
    const KVALUE cd = rb_data_typed_object_wrap(0, b, &kcx_cblock_type);
    return rb_funcall(rb_ary_entry(x->cblock_maker, lambda ? 1 : 0), rb_intern("call"), 1, cd);
}
KCX_API KVALUE rb_proc_new(void *func, KVALUE val) { return kcx_cblock_proc(func, val, true); }
KCX_API KVALUE
rb_block_call(KVALUE obj, KVALUE mid, int argc, const KVALUE *argv, void *func, KVALUE data2)
{
    if (!func) return rb_funcall_passing_block(obj, mid, argc, argv);
    return rb_funcall_with_block(obj, mid, argc, argv, kcx_cblock_proc(func, data2, false));
}
KCX_API KVALUE rb_block_call_kw(KVALUE obj, KVALUE mid, int argc, const KVALUE *argv, void *func, KVALUE data2, int kw)
{ (void)kw; return rb_block_call(obj, mid, argc, argv, func, data2); }

/* super from the running extension method: args below the cursor, as a `super` node stages them */
KCX_API KVALUE
rb_call_super(int argc, const KVALUE *argv)
{
    CTX *const c = kcx_c();
    struct korb_cext_frame *const f = c->cext_top;
    if (!f || !f->entry) rb_raise(rb_eRuntimeError, "super called outside of method");
    const struct korb_method *const e = f->entry;
    korb_value *const s = kcx_slots(c);
    for (int i = 0; i < argc; i++) s[i] = K(argv[i]);
    return W(kcx_check(c, korb_super(c, s + argc, e->orig_mid ? e->orig_mid : e->mid, 0, (uint32_t)argc,
                                     (korb_value)((uintptr_t)e | KORB_FID_METHOD), K(f->self), f->block, f->def_env,
                                     f->captured_self ? *f->captured_self : KORB_NIL, NULL)));
}
KCX_API KVALUE rb_call_super_kw(int argc, const KVALUE *argv, int kw) { (void)kw; return rb_call_super(argc, argv); }

/* rb_scan_args: "<lead><opt><*><trail><:><&>" (CRuby's format). */
KCX_API int
rb_scan_args(int argc, const KVALUE *argv, const char *fmt, ...)
{
    const char *p = fmt;
    int lead = 0, opt = 0, trail = 0; bool rest = false, kw = false, blk = false;
    if (*p >= '0' && *p <= '9') { lead = *p++ - '0'; if (*p >= '0' && *p <= '9') opt = *p++ - '0'; }
    if (*p == '*') { rest = true; p++; if (*p >= '0' && *p <= '9') trail = *p++ - '0'; }
    else if (*p >= '0' && *p <= '9') trail = *p++ - '0';
    if (*p == ':') { kw = true; p++; }
    if (*p == '&') { blk = true; p++; }
    KVALUE kwh = KQnil;
    if (kw && argc > 0 && rb_keyword_given_p()) { kwh = argv[argc - 1]; argc--; }
    const int req = lead + trail;
    if (argc < req || (!rest && argc > req + opt)) rb_error_arity(argc, req, rest ? -1 : req + opt);
    va_list ap; va_start(ap, fmt);
    int i = 0;
    for (int k = 0; k < lead; k++, i++) { KVALUE *const v = va_arg(ap, KVALUE *); if (v) *v = argv[i]; }
    const int nopt = argc - req < opt ? argc - req : opt;
    for (int k = 0; k < opt; k++) { KVALUE *const v = va_arg(ap, KVALUE *); if (v) *v = k < nopt ? argv[i] : KQnil; if (k < nopt) i++; }
    if (rest) {
        KVALUE *const v = va_arg(ap, KVALUE *);
        const int nrest = argc - i - trail;
        if (v) *v = rb_ary_new_from_values(nrest, argv + i);
        i += nrest;
    }
    for (int k = 0; k < trail; k++, i++) { KVALUE *const v = va_arg(ap, KVALUE *); if (v) *v = argv[i]; }
    if (kw) { KVALUE *const v = va_arg(ap, KVALUE *); if (v) *v = kwh; }
    if (blk) { KVALUE *const v = va_arg(ap, KVALUE *); if (v) *v = rb_block_given_p() ? rb_block_proc() : KQnil; }
    va_end(ap);
    return argc;
}
/* CRuby's messages: every missing (then every unknown) keyword, singular or plural */
static __attribute__((noreturn)) void
kcx_raise_keywords(const char *what, KVALUE keys)
{
    const long n = rb_array_len(keys);
    const KVALUE parts = rb_ary_new();
    for (long i = 0; i < n; i++) rb_ary_push(parts, rb_inspect(rb_ary_entry(keys, i)));
    rb_raise(rb_eArgError, "%s keyword%s: %" "l" "i" "\v", what, n > 1 ? "s" : "", rb_ary_join(parts, rb_str_new_cstr(", ")));
}
KCX_API int
rb_get_kwargs(KVALUE hash, const KVALUE *table, int required, int optional, KVALUE *values)
{
    const bool rest = optional < 0;
    if (rest) optional = -1 - optional;
    int found = 0;
    KVALUE missing = KQnil;
    for (int i = 0; i < required + optional; i++) {
        const KVALUE key = rb_id2sym(table[i]);
        const bool has = hash != KQnil && RTEST_K(rb_funcall(hash, rb_intern("key?"), 1, key));
        if (!has) {
            if (i < required) { if (missing == KQnil) missing = rb_ary_new(); rb_ary_push(missing, key); }
            if (values) values[i] = KQundef;
            continue;
        }
        found++;
        if (values) { values[i] = rb_hash_aref(hash, key); (void)rb_hash_delete(hash, key); }
    }
    if (missing != KQnil) kcx_raise_keywords("missing", missing);
    if (!rest && hash != KQnil && rb_hash_size_num(hash) > 0 && values) kcx_raise_keywords("unknown", rb_hash_keys(hash));
    return found;
}
KCX_API KVALUE rb_extract_keywords(KVALUE *orighash) { const KVALUE h = *orighash; *orighash = KQnil; return h; }

/* ---- T_DATA ---- */
static KVALUE
kcx_cdata_new(KVALUE klass, const struct rb_data_type_struct *type, void *data, void (*dmark)(void *), void (*dfree)(void *))
{
    CTX *const c = kcx_c();
    korb_value *const s = kcx_slots(c);
    s[0] = klass ? K(klass) : KORB_NIL;
    KorbCData *const d = korb_alloc(c, s + 1, sizeof(KorbCData), KORB_OBJ_OBJECT);
    d->obj.shape_id = 1;
    d->obj.head.flags |= KORB_FL_CDATA;
    if (s[0] != KORB_NIL) ARO_STORE(c, d, (korb_value *)(uintptr_t)&d->obj.klass, s[0]);
    d->type = type; d->data = data; d->dmark = dmark; d->dfree = dfree;
    if (dfree) aro_gc_finalize_register(c, d);
    return W((korb_value)d);
}
KCX_API KVALUE rb_data_typed_object_wrap(KVALUE klass, void *datap, const struct rb_data_type_struct *type)
{ return kcx_cdata_new(klass, type, datap, type->function.dmark, type->function.dfree); }
KCX_API KVALUE
rb_data_typed_object_zalloc(KVALUE klass, size_t size, const struct rb_data_type_struct *type)
{ return rb_data_typed_object_wrap(klass, ruby_xcalloc(1, size), type); }
KCX_API KVALUE rb_data_object_wrap(KVALUE klass, void *datap, void (*dmark)(void *), void (*dfree)(void *))
{ return kcx_cdata_new(klass, NULL, datap, dmark, dfree); }
KCX_API KVALUE rb_data_object_zalloc(KVALUE klass, size_t size, void (*dmark)(void *), void (*dfree)(void *))
{ return rb_data_object_wrap(klass, ruby_xcalloc(1, size), dmark, dfree); }
static KorbCData *
kcx_cdata(KVALUE h)
{
    const korb_value v = K(h);
    if (!KORB_OBJECT_P(v) || !(((AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_CDATA)) return NULL;
    return (KorbCData *)(uintptr_t)v;
}
KCX_API int rb_data_p(KVALUE h) { return kcx_cdata(h) != NULL; }
KCX_API const struct rb_data_type_struct *rb_typeddata_type(KVALUE h) { const KorbCData *const d = kcx_cdata(h); return d ? d->type : NULL; }
KCX_API void *rb_data_get_ptr(KVALUE h) { KorbCData *const d = kcx_cdata(h); return d ? d->data : NULL; }
KCX_API void rb_data_set_ptr(KVALUE h, void *p) { KorbCData *const d = kcx_cdata(h); if (d) d->data = p; }
KCX_API void **rb_data_ptr_ref(KVALUE h) { KorbCData *const d = kcx_cdata(h); if (!d) rb_raise(rb_eTypeError, "not a data object"); return &d->data; }
KCX_API int
rb_typeddata_inherited_p(const struct rb_data_type_struct *child, const struct rb_data_type_struct *parent)
{
    for (; child; child = child->parent) if (child == parent) return 1;
    return 0;
}
KCX_API int rb_typeddata_is_kind_of(KVALUE h, const struct rb_data_type_struct *type) { const KorbCData *const d = kcx_cdata(h); return d && rb_typeddata_inherited_p(d->type, type); }
KCX_API void *
rb_check_typeddata(KVALUE h, const struct rb_data_type_struct *type)
{
    const KorbCData *const d = kcx_cdata(h);
    if (d && rb_typeddata_inherited_p(d->type, type)) return d->data;
    CTX *const c = kcx_c();
    char got[192];
    const korb_value v = K(h);
    if (v == KORB_NIL) snprintf(got, sizeof got, "nil");
    else if (v == KORB_TRUE) snprintf(got, sizeof got, "true");
    else if (v == KORB_FALSE) snprintf(got, sizeof got, "false");
    else korb_class_qname_into(c, korb_class_obj_of(c, v), got, sizeof got);
    kcx_throw(c, korb_raise(c, kcx_slots(c), KORB_E_TYPE, 0, "wrong argument type %s (expected %s)", got, type->wrap_struct_name));
}

/* ---- threads: one OS thread, so these just call ---- */
KCX_API void *rb_thread_call_without_gvl(void *(*func)(void *), void *d1, void *ubf, void *d2) { (void)ubf; (void)d2; return func(d1); }
KCX_API void *rb_thread_call_with_gvl(void *(*func)(void *), void *d1) { return func(d1); }
KCX_API void *rb_nogvl(void *(*func)(void *), void *d1, void *ubf, void *d2, int flags) { (void)ubf; (void)d2; (void)flags; return func(d1); }
KCX_API void rb_thread_check_ints(void) {}
KCX_API KVALUE rb_thread_current(void) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(rb_cThread), "current", 0, NULL)); }
KCX_API void rb_thread_schedule(void) { CTX *const c = kcx_c(); (void)kcx_send_name(c, K(rb_cThread), "pass", 0, NULL); }

/* ---- loading ---- */
KCX_API void rb_ext_ractor_safe(bool flag) { (void)flag; }
KCX_API KVALUE
rb_require(const char *feature)
{
    CTX *const c = kcx_c();
    return W(kcx_check(c, korb_require_feature(c, kcx_slots(c), feature)));
}
KCX_API KVALUE rb_f_require(KVALUE self, KVALUE fname) { (void)self; return rb_require(rb_string_value_cstr(&fname)); }
KCX_API void rb_provide(const char *feature) { CTX *const c = kcx_c(); (void)korb_mark_loaded(c->vm, feature); korb_loaded_features_push(c, kcx_slots(c), feature); }
KCX_API void *
rb_ext_resolve_symbol(const char *feature, const char *symbol)
{
    const struct korb_cext_vm *const x = kcx_vm(kcx_c()->vm);
    size_t fl = strlen(feature);                      /* "digest.so" names our digest.kompat.so */
    const size_t el = KORB_KOMPAT_EXT_LEN;
    if (fl >= el && strcmp(feature + fl - el, KORB_KOMPAT_EXT) == 0) fl -= el;
    else if (fl >= 3 && strcmp(feature + fl - 3, ".so") == 0) fl -= 3;
    for (uint32_t i = 0; i < x->lib_n; i++) {
        const char *const p = x->libs[i].path;
        const size_t pl = strlen(p);
        if (pl >= fl + el + 1 && strcmp(p + pl - el, KORB_KOMPAT_EXT) == 0 && strncmp(p + pl - el - fl, feature, fl) == 0 && p[pl - el - 1 - fl] == '/')
            return dlsym(x->libs[i].dl, symbol);
    }
    return NULL;
}

static KVALUE kcx_run_init(KVALUE fn) { ((void (*)(void))(uintptr_t)fn)(); return KQnil; }

/* require of a C extension: dlopen it and run Init_<basename> under a frame. */
static RESULT
korb_cext_load(CTX *c, korb_value *slots, const char *path)
{
    void *const dl = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!dl) return korb_raise(c, slots, KORB_E_LOADERR, 0, "%s - %s", dlerror(), path);
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char init[256];
    snprintf(init, sizeof init, "Init_%.*s", (int)(strcspn(base, ".")), base);
    void *const fn = dlsym(dl, init);
    if (!fn) return korb_raise(c, slots, KORB_E_LOADERR, 0, "%s - %s: undefined symbol %s", path, path, init);
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    if (x->lib_n == x->lib_capa) { x->lib_capa = x->lib_capa ? x->lib_capa * 2 : 8; x->libs = ruby_xrealloc(x->libs, x->lib_capa * sizeof *x->libs); }
    x->libs[x->lib_n].path = strdup(path); x->libs[x->lib_n].dl = dl; x->lib_n++;
    CTX *const saved_ctx = korb_cext_ctx;
    korb_cext_ctx = c;
    struct korb_cext_frame f;
    kcx_frame_push(c, &f, slots);
    if (!rb_cObject) kcx_init_globals(c);
    int state = 0;
    (void)rb_protect(kcx_run_init, (KVALUE)(uintptr_t)fn, &state);
    RESULT r = RESULT_OK(KORB_TRUE);
    if (state) { r = (RESULT){ x->errinfo, (uintptr_t)state }; x->errinfo = KORB_NIL; }
    kcx_frame_leave(c, &f);
    korb_cext_ctx = saved_ctx;
    return r;
}

/* ---- the openssl set: numbers, strings, encodings, IO, ractor-local storage ---- */
KCX_API int
rb_long2int(long n)
{
    if (n > INT32_MAX || n < INT32_MIN) rb_raise(rb_eRangeError, "integer %ld too %s to convert to 'int'", n, n < 0 ? "small" : "big");
    return (int)n;
}
KCX_API KVALUE
rb_str_to_inum(KVALUE str, int base, int badcheck)
{
    CTX *const c = kcx_c();
    if (badcheck) { const KVALUE a[2] = { str, (KVALUE)LONG2FIX(base) }; return W(kcx_send_name(c, korb_builtin_class_obj(c->vm, KORB_C_OBJECT), "Integer", 2, a)); }
    const KVALUE b = (KVALUE)LONG2FIX(base);
    return W(kcx_send_name(c, K(str), "to_i", 1, &b));
}
KCX_API KVALUE rb_cstr_to_inum(const char *str, int base, int badcheck) { return rb_str_to_inum(rb_str_new_cstr(str), base, badcheck); }

/* |val| as little-endian bytes (malloc'd), its length, and the sign */
static uint8_t *
kcx_abs_bytes(KVALUE val, size_t *len, int *sign)
{
    CTX *const c = kcx_c();
    const korb_value v = K(rb_Integer(val));
    const korb_value zero = LONG2FIX(0);
    *sign = FIXNUM_P(v) ? (FIX2LONG(v) > 0) - (FIX2LONG(v) < 0) : korb_int_cmp(v, zero);
    const KVALUE sixteen = (KVALUE)LONG2FIX(16);
    const korb_value hex = kcx_send_name(c, kcx_send_name(c, v, "abs", 0, NULL), "to_s", 1, &sixteen);
    const char *const h = korb_strbuf_data(VAL2STR(hex)->buf);
    const size_t hl = VAL2STR(hex)->len;
    *len = (hl + 1) / 2;
    uint8_t *const out = ruby_xcalloc(*len ? *len : 1, 1);
    for (size_t i = 0; i < hl; i++) {                 /* hex digit i from the right */
        const char ch = h[hl - 1 - i];
        const unsigned d = (ch >= '0' && ch <= '9') ? (unsigned)(ch - '0') : (unsigned)((ch | 0x20) - 'a' + 10);
        out[i / 2] |= (uint8_t)(d << (4 * (i % 2)));
    }
    while (*len > 0 && out[*len - 1] == 0) (*len)--;
    return out;
}
KCX_API size_t
rb_absint_size(KVALUE val, int *nlz_bits_ret)
{
    size_t n; int sign;
    uint8_t *const b = kcx_abs_bytes(val, &n, &sign);
    if (nlz_bits_ret) *nlz_bits_ret = n ? __builtin_clz((unsigned)b[n - 1]) - 24 : 0;
    free(b);
    return n;
}
KCX_API int
rb_integer_pack(KVALUE val, void *words, size_t numwords, size_t wordsize, size_t nails, int flags)
{
    (void)nails;
    size_t n; int sign;
    uint8_t *const le = kcx_abs_bytes(val, &n, &sign);
    const size_t total = numwords * wordsize;
    uint8_t *const tmp = ruby_xcalloc(total ? total : 1, 1);   /* |val| mod 256^total, little-endian */
    memcpy(tmp, le, n < total ? n : total);
    if (sign < 0 && (flags & 0x80)) {                  /* INTEGER_PACK_2COMP */
        unsigned carry = 1;
        for (size_t i = 0; i < total; i++) { const unsigned x = (uint8_t)~tmp[i] + carry; tmp[i] = (uint8_t)x; carry = x >> 8; }
    }
    const bool msw = flags & 0x01, msb = flags & 0x10;   /* INTEGER_PACK_NATIVE is little-endian here */
    uint8_t *const out = words;
    for (size_t w = 0; w < numwords; w++)
        for (size_t b = 0; b < wordsize; b++)
            out[(msw ? numwords - 1 - w : w) * wordsize + (msb ? wordsize - 1 - b : b)] = tmp[w * wordsize + b];
    const bool overflow = n > total;
    free(tmp); free(le);
    return overflow ? 2 * sign : sign;
}
KCX_API KVALUE
rb_integer_unpack(const void *words, size_t numwords, size_t wordsize, size_t nails, int flags)
{
    (void)nails;
    const uint8_t *const in = words;
    const bool msw = flags & 0x01, msb = flags & 0x10;
    const size_t total = numwords * wordsize;
    char *const hex = ruby_xmalloc(total * 2 + 2);
    size_t k = 0;
    for (size_t i = total; i-- > 0; ) {                /* most significant byte first */
        const size_t w = i / wordsize, b = i % wordsize;
        const uint8_t x = in[(msw ? numwords - 1 - w : w) * wordsize + (msb ? wordsize - 1 - b : b)];
        static const char digits[] = "0123456789abcdef";
        hex[k++] = digits[x >> 4]; hex[k++] = digits[x & 15];
    }
    if (k == 0) hex[k++] = '0';
    hex[k] = '\0';
    KVALUE r = rb_str_to_inum(rb_str_new_cstr(hex), 16, 0);
    free(hex);
    if (flags & 0x200) r = rb_funcall(r, rb_intern("-@"), 0);   /* INTEGER_PACK_NEGATIVE */
    return r;
}

KCX_API void rb_str_modify(KVALUE str) { rb_check_frozen(str); }
KCX_API void
rb_str_modify_expand(KVALUE str, long expand)
{
    rb_check_frozen(str);
    CTX *const c = kcx_c();
    korb_value *const s = kcx_slots(c);
    s[0] = K(str);
    (void)korb_str_ensure(c, s + 1, KORB_REF_AT(&s[0]), VAL2STR(s[0])->len + (uint32_t)expand);
}
KCX_API size_t rb_str_capacity(KVALUE str) { return VAL2STR(K(str))->capa; }
KCX_API KVALUE rb_str_locktmp(KVALUE str) { return str; }
KCX_API KVALUE rb_str_unlocktmp(KVALUE str) { return str; }
KCX_API KVALUE rb_str_resurrect(KVALUE str) { return rb_str_dup(str); }
KCX_API KVALUE rb_str_new_frozen(KVALUE str) { return RTEST_K(rb_obj_frozen_p(str)) ? str : rb_str_freeze(rb_str_dup(str)); }
KCX_API KVALUE
rb_str_drop_bytes(KVALUE str, long len)
{
    CTX *const c = kcx_c();
    const KVALUE a[2] = { (KVALUE)LONG2FIX(len), (KVALUE)LONG2FIX(rb_string_len(str)) };
    const KVALUE rest = W(kcx_send_name(c, K(str), "byteslice", 2, a));
    (void)kcx_send_name(c, K(str), "replace", 1, &rest);
    return str;
}
KCX_API KVALUE rb_String(KVALUE h) { CTX *const c = kcx_c(); return W(kcx_send_name(c, korb_builtin_class_obj(c->vm, KORB_C_OBJECT), "String", 1, &h)); }
static KVALUE
kcx_convert_type(KVALUE val, int type, const char *tname, const char *method, bool raise)
{
    if (rb_type(val) == type) return val;
    CTX *const c = kcx_c();
    const korb_value v = K(val);
    char nm[192]; kcx_type_name(c, v, nm, sizeof nm);
    if (v == KORB_NIL || v == KORB_TRUE || v == KORB_FALSE || !korb_responds_to(c, v, kcx_id(c, method))) {
        if (!raise) return KQnil;
        static const char *const implicit[] = { "to_int", "to_ary", "to_str", "to_sym", "to_hash", "to_proc", "to_io", NULL };
        bool imp = false;
        for (int i = 0; implicit[i]; i++) imp |= strcmp(method, implicit[i]) == 0;
        if (imp) rb_raise(rb_eTypeError, "no implicit conversion of %s into %s", nm, tname);
        rb_raise(rb_eTypeError, "can't convert %s into %s", nm, tname);
    }
    const KVALUE r = W(kcx_send_name(c, v, method, 0, NULL));
    if (rb_type(r) != type) {
        if (!raise && r == KQnil) return r;
        char rn[192]; kcx_type_name(c, K(r), rn, sizeof rn);
        rb_raise(rb_eTypeError, "can't convert %s to %s (%s#%s gives %s)", nm, tname, nm, method, rn);
    }
    return r;
}
KCX_API KVALUE rb_convert_type(KVALUE val, int type, const char *tname, const char *method) { return kcx_convert_type(val, type, tname, method, true); }
KCX_API KVALUE rb_check_convert_type(KVALUE val, int type, const char *tname, const char *method) { return kcx_convert_type(val, type, tname, method, false); }
KCX_API uintptr_t
rb_memhash(const void *ptr, long len)
{
    uint64_t h = 0xcbf29ce484222325ull;               /* FNV-1a */
    for (long i = 0; i < len; i++) { h ^= ((const uint8_t *)ptr)[i]; h *= 0x100000001b3ull; }
    return (uintptr_t)h;
}

/* encodings: an rb_encoding * is koruby's encoding index + 1 */
struct rb_encoding_kx;
#define KCX_ENC(idx) ((struct rb_encoding_kx *)(uintptr_t)((idx) + 1))
KCX_API struct rb_encoding_kx *rb_utf8_encoding(void) { return KCX_ENC(KORB_ENC_UTF8); }
KCX_API struct rb_encoding_kx *rb_ascii8bit_encoding(void) { return KCX_ENC(KORB_ENC_BINARY); }
KCX_API struct rb_encoding_kx *rb_usascii_encoding(void) { return KCX_ENC(KORB_ENC_USASCII); }
KCX_API int rb_utf8_encindex(void) { return KORB_ENC_UTF8; }
KCX_API int rb_ascii8bit_encindex(void) { return KORB_ENC_BINARY; }
KCX_API int rb_usascii_encindex(void) { return KORB_ENC_USASCII; }
KCX_API KVALUE rb_enc_associate_index(KVALUE obj, int idx) { if (KORB_STRING_P(K(obj))) KORB_STR_ENC_SET(K(obj), (uint32_t)idx); return obj; }
KCX_API KVALUE rb_enc_associate(KVALUE obj, struct rb_encoding_kx *enc) { return rb_enc_associate_index(obj, (int)((uintptr_t)enc - 1)); }
KCX_API int rb_enc_get_index(KVALUE obj) { return KORB_STRING_P(K(obj)) ? (int)KORB_STR_ENC(K(obj)) : -1; }
KCX_API KVALUE rb_enc_str_new(const char *ptr, long len, struct rb_encoding_kx *enc) { return rb_enc_associate(rb_str_new(ptr, len), enc); }
KCX_API KVALUE rb_external_str_new(const char *ptr, long len) { return rb_utf8_str_new(ptr, len); }
KCX_API KVALUE
rb_enc_sprintf(struct rb_encoding_kx *enc, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); const KVALUE r = rb_vsprintf(fmt, ap); va_end(ap);
    return rb_enc_associate(r, enc);
}

KCX_API void rb_hash_set_ifnone(KVALUE h, KVALUE v) { CTX *const c = kcx_c(); (void)kcx_send_name(c, K(h), "default=", 1, &v); }
KCX_API KVALUE rb_assoc_new(KVALUE a, KVALUE b) { const KVALUE v[2] = { a, b }; return rb_ary_new_from_values(2, v); }
KCX_API KVALUE rb_ary_aref(int argc, const KVALUE *argv, KVALUE ary) { CTX *const c = kcx_c(); return W(kcx_send_name(c, K(ary), "[]", argc, argv)); }
KCX_API void *rb_alloc_tmp_buffer(volatile KVALUE *store, long len) { void *const p = ruby_xmalloc((size_t)(len > 0 ? len : 1)); *store = (KVALUE)(uintptr_t)p; return p; }
KCX_API void rb_free_tmp_buffer(volatile KVALUE *store) { free((void *)(uintptr_t)*store); *store = 0; }
KCX_API void
rb_attr(KVALUE klass, KVALUE id, int read, int write, int ex)
{
    (void)ex;
    CTX *const c = kcx_c();
    const KVALUE s = rb_id2sym(id);
    if (read) (void)kcx_send_name(c, K(klass), "attr_reader", 1, &s);
    if (write) (void)kcx_send_name(c, K(klass), "attr_writer", 1, &s);
}
KCX_API KVALUE
rb_enumeratorize(KVALUE obj, KVALUE meth, int argc, const KVALUE *argv)
{
    KVALUE a[argc + 1];
    a[0] = meth;
    for (int i = 0; i < argc; i++) a[1 + i] = argv[i];
    return rb_funcallv(obj, rb_intern("enum_for"), argc + 1, a);
}
KCX_API KVALUE rb_frame_this_func(void) { const struct korb_cext_frame *const f = kcx_c()->cext_top; return f && f->entry ? (KVALUE)f->entry->mid : 0; }
KCX_API int rb_during_gc(void) { return korb_cext_gc_vm != NULL; }
KCX_API KVALUE rb_ruby_verbose_get(void) { return rb_gv_get("$VERBOSE"); }
KCX_API KVALUE rb_ruby_debug_get(void) { return rb_gv_get("$DEBUG"); }
KCX_API KVALUE
rb_syserr_new(int err, const char *msg)
{
    CTX *const c = kcx_c();
    const KVALUE a[2] = { msg ? rb_str_new_cstr(msg) : KQnil, (KVALUE)LONG2FIX(err) };
    return W(kcx_send_name(c, K(rb_eSystemCallError), "new", 2, a));
}
KCX_API __attribute__((noreturn)) void rb_eof_error(void) { rb_raise(rb_eEOFError, "end of file reached"); }

/* IO: one rb_io_t per descriptor (libc, stable), refreshed by every GetOpenFile */
struct rb_io { KVALUE self; int fd; int mode; };
KCX_API struct rb_io *
rb_io_get_fptr(KVALUE io)
{
    CTX *const c = kcx_c();
    struct korb_cext_vm *const x = kcx_vm(c->vm);
    const int fd = (int)rb_num2long(W(kcx_send_name(c, K(io), "fileno", 0, NULL)));
    if ((size_t)fd >= x->io_n) {
        const size_t nn = (size_t)fd + 16;
        x->io = ruby_xrealloc(x->io, nn * sizeof *x->io);
        memset(x->io + x->io_n, 0, (nn - x->io_n) * sizeof *x->io);
        x->io_n = nn;
    }
    struct rb_io *const f = &x->io[fd];
    f->fd = fd; f->self = KQnil; f->mode = 3;
    return f;
}
KCX_API void rb_io_check_readable(struct rb_io *f) { (void)f; }
KCX_API void rb_io_check_writable(struct rb_io *f) { (void)f; }
KCX_API void rb_io_check_closed(struct rb_io *f) { (void)f; }
KCX_API int rb_io_descriptor(KVALUE io) { return rb_io_get_fptr(io)->fd; }
KCX_API KVALUE rb_io_timeout(KVALUE io) { return rb_funcall(io, rb_intern("timeout"), 0); }
KCX_API KVALUE
rb_io_wait(KVALUE io, KVALUE events, KVALUE timeout)
{
    const long ev = rb_num2long(events);
    if (timeout == KQnil) timeout = rb_io_timeout(io);   /* RUBY_IO_TIMEOUT_DEFAULT: the IO's own #timeout */
    const KVALUE one = rb_ary_new_from_args(1, io);
    const KVALUE a[4] = { (ev & 1) ? one : KQnil, (ev & 4) ? one : KQnil, KQnil, timeout };
    const KVALUE r = rb_funcallv(rb_cIO, rb_intern("select"), 4, a);
    return r == KQnil ? KQfalse : events;
}
KCX_API KVALUE
rb_io_maybe_wait(int error, KVALUE io, KVALUE events, KVALUE timeout)
{
    if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) return rb_io_wait(io, events, timeout);
    rb_syserr_fail(error, NULL);
}
KCX_API int rb_thread_wait_fd(int fd) { struct pollfd p = { fd, POLLIN, 0 }; return poll(&p, 1, -1) > 0; }
KCX_API int rb_thread_fd_writable(int fd) { struct pollfd p = { fd, POLLOUT, 0 }; return poll(&p, 1, -1) > 0; }

/* ractor-local storage: one ractor, so a key is just a slot */
struct rb_ractor_local_key_struct { const void *type; void *ptr; KVALUE val; };
KCX_API struct rb_ractor_local_key_struct *
rb_ractor_local_storage_ptr_newkey(const void *type)
{
    struct rb_ractor_local_key_struct *const k = ruby_xcalloc(1, sizeof *k);
    k->type = type; k->val = KQnil;
    return k;
}
KCX_API void *rb_ractor_local_storage_ptr(struct rb_ractor_local_key_struct *k) { return k->ptr; }
KCX_API void rb_ractor_local_storage_ptr_set(struct rb_ractor_local_key_struct *k, void *p) { k->ptr = p; }
KCX_API struct rb_ractor_local_key_struct *
rb_ractor_local_storage_value_newkey(void)
{
    struct rb_ractor_local_key_struct *const k = rb_ractor_local_storage_ptr_newkey(NULL);
    rb_gc_register_address(&k->val);
    return k;
}
KCX_API KVALUE rb_ractor_local_storage_value(struct rb_ractor_local_key_struct *k) { return k->val; }
KCX_API void rb_ractor_local_storage_value_set(struct rb_ractor_local_key_struct *k, KVALUE v) { k->val = v; }

