#ifndef BARUBY_PRECISE_GC_H
#define BARUBY_PRECISE_GC_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "astro_debug.h"  /* ASTRO_ASSERT — used by ARO_ROOT_SCOPE bounds checks */

/* Types-only header — sample's context.h includes this directly to get
 * AroObjectHeader BEFORE defining CTX_struct.  See gc_types.h for the
 * layering rationale. */
#include "gc_types.h"

/* ---------------------------------------------------------------------------
 * Virtual-address reservation for region-based backends.
 *
 * Region-based backends (copy*, mark_compact*, mark_bump_gen, immix*) used
 * to declare a fixed REGION_BYTES / ARENA_BYTES / TENURED_BYTES upper
 * bound (typically 512 MiB - 1 GiB).  That bound was a program-limiting
 * fixed-length value — heap growth past it aborts as OOM even when
 * physical memory is plenty.
 *
 * We now reserve ARO_GC_REGION_VIRT_BYTES (= 64 GiB) of virtual address
 * space per region.  Physical pages commit only on first touch (Linux
 * overcommit + MAP_NORESERVE); collectors may MADV_DONTNEED freed
 * regions to release physical memory back to the OS.  64 GiB is far
 * larger than any practical baruby_precise program; the heap is
 * effectively unbounded.  Per-page / per-block / per-line sizes are
 * left fixed (they're tuning knobs, not program-limiting).
 * --------------------------------------------------------------------------- */
#if SIZE_MAX <= 0xffffffffu
/* 32bit (wasm32 等): アドレス空間が 4 GiB しかなく、遅延ページングも無いので
 * 「巨大な仮想領域を予約して実際に触った分だけ物理化する」戦略が成立しない。
 * ここは**実メモリ**になるため、必要量に合わせて小さく取る。 */
/* 512 MiB * 2 面 = 1 GiB を calloc で**実確保**する (アドレス空間 4 GiB の 1/4)。
 * 遅延ページングが無いので hello world でも RSS が 1.3 GB になる。本来は必要に
 * 応じて伸ばすべきで、この定数はその暫定。小さすぎると生存データを少し積んだ
 * だけで OOM する (2 要素配列 1.5M 個で 64 MiB では足りない)。 */
#  define ARO_GC_REGION_VIRT_BYTES  ((size_t)512u << 20)   /* 512 MiB 実メモリ */
#else
#  define ARO_GC_REGION_VIRT_BYTES  ((size_t)64u << 30)   /* 64 GiB virtual */
#endif

// Forward decls (defined in context.h)
struct CTX_struct;
typedef struct CTX_struct CTX;
/* VALUE forward decl — sample's context.h is the canonical source.
 * Default to `intptr_t` for samples that don't override, but skip when
 * sample's context.h has already typedef'd VALUE (= guards against
 * `conflicting types` when sample uses `uintptr_t` / `int64_t` / etc.,
 * which differ from `intptr_t` at the type-system level). */
#ifndef ARO_GC_VALUE_TYPEDEFED
typedef intptr_t VALUE;
#endif

// ---------------------------------------------------------------------------
// Pluggable GC backend interface.
//
// One of seventeen backends is selected at build time via -DBARUBY_GC=<n>:
//   1: none              — no GC, malloc + leak (baseline)
//   2: mark              — non-moving mark&sweep (per-object malloc list)
//   3: mark_gen          — mark&sweep + 2-gen
//   4: mark_gen_inc      — mark&sweep + 2-gen + incremental marking
//   5: copy              — Cheney semi-space (default)
//   6: copy_gen          — copying nursery + tenured (semispace tenured)
//   8: mark_compact      — single-region mark + Lisp-2 sliding compactor
//   9: mark_compact_gen  — nursery (copy) + tenured (mark + Lisp-2 compact)
//  10: bump              — bump-only, no GC (strictly faster `none`)
//  11: mark_bump_gen     — bump nursery + linked-list mark&sweep tenured
//                          (isolates nursery alloc strategy vs mark_gen)
//  12: immix             — block (32 KiB) / line (128 B) mark-region.
//                          Non-moving v1 (no evacuation).  Bump-allocate
//                          within holes (unmarked-line runs).
//  13: immix_gen         — generational variant: bump nursery (16 MiB) +
//                          Immix tenured (512 MiB).  Minor copy-promotes
//                          nursery survivors into tenured holes.  Major
//                          is regular Immix mark + line-mark sweep.
//  14: mark_bitmap_gen       — sticky mark&sweep with per-page bitmaps.  Same
//                          semantics as mark_gen but GCHeader = 8 B (no
//                          marked/old/dirty bytes — bits live in page
//                          bitmaps).  Young set found by walking pages
//                          (no young_next list).  BaArray (24 B payload)
//                          fits class-32 perfectly = 2× density vs mark_gen.
//  15: mark_card_gen
//  16: mark_freelist
//  (17 was copy_scramble, removed when round-robin PURGE in gc_copy
//   subsumed it — see gc_types.h ID 17 comment.)
//
// Gen / inc variants define ARO_GC_HAS_WB so callers know they must use
// aro_gc_store() instead of plain `*slot = v` for heap-pointer writes.
// ---------------------------------------------------------------------------

/* All type definitions (BARUBY_GC_* IDs, ARO_GC_HAS_FWD,
 * AroObjectHeader, AroGcStats, AroGcCommonState) live in gc_types.h.
 * Included above. */

/* Accessors.  `c->astro_gc` is `struct ASTroGC *` (forward decl in
 * context.h).  Cast to `AroGcCommonState *` is safe iff each backend's
 * ASTroGC has `AroGcCommonState common` as its first field.
 *
 * **CTX contract**: framework treats CTX as opaque except for the
 * `astro_gc` field.  Sample MUST declare CTX_struct with `struct ASTroGC
 * *astro_gc` (= the only field framework reads / writes).  All other
 * sample-internal fields (sp, env, frame chains, etc.) are invisible to
 * the framework — sample bridges them via AROH_VISIT_ROOTS macro. */
#define ARO_GC_COMMON(c) ((AroGcCommonState *)((c)->astro_gc))

/* `aro_gc_backend_name` identifies which backend was compiled in (=
 * compile-time constant per binary).  It is not per-instance state, so
 * keeping it as `const char *` global is fine. */
extern const char *aro_gc_backend_name;

/* ---------------------------------------------------------------------------
 * ARO_LOAD — slot-based read barrier hook.
 *
 * Reads a VALUE / typed-ptr slot and returns the raw heap pointer.
 * Currently a plain dereference (= identity), but kept as a dedicated
 * macro so future GC algorithms (= concurrent Brooks-style forwarding,
 * colored pointer remap, handle-table lookup) can inject their barrier
 * here without touching call sites.
 *
 * Sample callers cast the returned `void *` to the sample's struct
 * type.  `ctx` is currently unused but retained in the signature for
 * the future barrier hook (= same reason `VAL2ARY(c, v)` takes CTX
 * even though it folds to a plain cast today).
 * --------------------------------------------------------------------------- */

#define ARO_LOAD(c, slot_ptr) \
    ((void)(c), (void *)(uintptr_t)(*(VALUE *)(uintptr_t)(slot_ptr)))

/* ---------------------------------------------------------------------------
 * SCAN_EDGES helper — sample's SCAN_EDGES invokes this per slot.
 *
 * ARO_GC_VISIT_EDGE(ctx, fn, slot_ptr)
 *   Reads the slot, calls `fn` to forward the raw pointer (= write the
 *   new address back to a local `_aro_raw`), writes the new value back
 *   to the slot.  Fixnums / singletons (= low bits non-zero, or v==0)
 *   skip forwarding entirely.
 *
 *   Sister macro ARO_GC_VISIT_EDGE_PTR (below) handles raw typed-ptr
 *   slots — same logic, no IS_PTR filter (= caller guaranteed the slot
 *   holds a typed pointer).
 * --------------------------------------------------------------------------- */
/* `(uintptr_t)` intermediate strips ARO_GC_EDGE's const without
 * triggering -Wcast-qual (which is enabled in audit builds to plug the
 * memcpy / void* implicit-conversion holes). */
#define ARO_GC_VISIT_EDGE(ctx, fn, slot_ptr) do {                              \
    VALUE *_aro_vs   = (VALUE *)(uintptr_t)(slot_ptr);                         \
    VALUE  _aro_v    = *_aro_vs;                                               \
    if (((uintptr_t)_aro_v & 7u) == 0 && _aro_v != 0) {                        \
        void *_aro_raw = (void *)(uintptr_t)_aro_v;                            \
        (fn)((ctx), &_aro_raw);                                                \
        *_aro_vs = (VALUE)(uintptr_t)_aro_raw;                                 \
    }                                                                          \
} while (0)

/* Raw typed-ptr edge: slot holds an unscrambled C pointer (= sample
 * struct field typed as `T *`, not VALUE).  Used by samples that have
 * not migrated all typed-ptr fields to encoded VALUE storage.  Skip
 * the scramble decode entirely (= raw slot). */
#define ARO_GC_VISIT_EDGE_PTR(ctx, fn, slot_ptr)                               \
    ((fn)((ctx), (void **)(uintptr_t)(slot_ptr)))

/* ---------------------------------------------------------------------------
 * ARO_ROOT_SCOPE — lexical precise-root scope.
 *
 * Reserves N VALUE slots on the sample-owned root stack.  Slots are
 * zero-filled before the root-stack top is advanced, so an alloc-triggered
 * GC inside the scope sees only valid heap pointers (or 0) in the new range.
 * Callers store live VALUEs into the slots via ARO_ROOT(r, i) before any
 * inner allocation, and RELOAD any C-local pointer from the slot AFTER each
 * allocation-capable call (= moving GC can rewrite the slot).
 *
 * Sample contract — sample's context.h MUST provide:
 *   AROH_ROOT_STACK_TOP(c)        — VALUE *  current root-stack top
 *   AROH_ROOT_STACK_SET_TOP(c, p) — set the top (= alloc / release)
 *   AROH_ROOT_STACK_LIMIT(c)      — VALUE *  one-past-end (debug bound)
 *
 *   AROH_VISIT_ROOTS MUST scan the same root stack so GC sees the slots
 *   reserved here.  (Typically `for (p = base; p < AROH_ROOT_STACK_TOP(c); p++)`.)
 *
 * Usage:
 *   ARO_ROOT_SCOPE_START(c, r, 2) {
 *       ARO_ROOT(r, 0) = aro_gc_alloc(c, sizeof(BaArray));
 *       ARO_ROOT(r, 1) = (VALUE)aro_gc_alloc(c, items_sz);
 *
 *       BaArray *a = VAL2ARY(c, ARO_ROOT(r, 0));   // reload after each alloc
 *       a->items = (BaArrayItems *)ARO_ROOT(r, 1);
 *       ret = ARO_ROOT(r, 0);
 *   } ARO_ROOT_SCOPE_END(c, r);
 *   return ret;
 *
 * Rules (NOT enforced by C — sample-side discipline):
 *   - No return / break / continue / goto / longjmp across the scope.
 *     Store the return value in an outer local and return after
 *     ARO_ROOT_SCOPE_END.
 *   - For sample exception unwinds (= raise / longjmp), sample frame
 *     teardown MUST restore the root-stack top to the frame's saved
 *     value — otherwise dead slots stay live forever and the scratch
 *     grows monotonically.
 *   - Nested scopes are allowed.  Use a different `name` to avoid
 *     shadowing the inner cap constant (compiler warns under -Wshadow).
 *
 * Cost: zero-fill N slots, one store to advance root-stack top, one to
 * restore.  In release builds ARO_ROOT(r, i) folds to r[i] (= ASTRO_ASSERT
 * is no-op).
 * --------------------------------------------------------------------------- */

#define ARO_CAT2(a, b) a##b
#define ARO_CAT(a, b)  ARO_CAT2(a, b)

/* Pure-macro implementation: bodies reference sample-side AROH_ROOT_STACK_*
 * macros, so they are only expanded at the call site — samples that never
 * use ARO_ROOT_SCOPE_* don't need to provide the AROH_* macros at all.
 *
 * Zero-fill ([base, base+n)) is unconditional and runs BEFORE the root-stack
 * top is advanced.  Otherwise visit_roots can walk uninit'd slots between
 * SCOPE_START and the first store (= residue from a prior frame at the same
 * address — moving GC happily forwards that into to-space and crashes
 * later). */
#define ARO_ROOT_SCOPE_START(c, name, n)                                       \
    do {                                                                        \
        VALUE *name = AROH_ROOT_STACK_TOP(c);                                   \
        const size_t ARO_CAT(_aro_root_cap_, name) = (size_t)(n);               \
        (void)ARO_CAT(_aro_root_cap_, name);                                    \
        ASTRO_ASSERT((name) + ARO_CAT(_aro_root_cap_, name)                     \
                     <= AROH_ROOT_STACK_LIMIT(c));                              \
        for (size_t _aro_root_i = 0;                                            \
             _aro_root_i < ARO_CAT(_aro_root_cap_, name);                       \
             _aro_root_i++) (name)[_aro_root_i] = 0;                            \
        AROH_ROOT_STACK_SET_TOP((c),                                            \
                                (name) + ARO_CAT(_aro_root_cap_, name));

#define ARO_ROOT_SCOPE_END(c, name)                                            \
        AROH_ROOT_STACK_SET_TOP((c), (name));                                   \
    } while (0)

/* Early-exit helper.  Restores the root-stack top to the scope's saved
 * `name` value (= just like SCOPE_END's restore) but does NOT close the
 * outer do/while block — so the caller can `return` / `goto` out of the
 * scope without leaving dead slots in the root-scan range.
 *
 * Usage:
 *   ARO_ROOT_SCOPE_START(c, r, 2) {
 *       ARO_ROOT(r, 0) = aro_gc_alloc(c, sz);
 *       if (early_condition) {
 *           VALUE ret = ARO_ROOT(r, 0);
 *           ARO_ROOT_SCOPE_CANCEL(c, r);
 *           return ret;                   // safe: sp restored
 *       }
 *       ...
 *   } ARO_ROOT_SCOPE_END(c, r);
 *
 * After CANCEL the slots ARO_ROOT(r, i) are no longer protected — the
 * caller MUST exit the scope (return / longjmp) without further allocations
 * in the current scope.  Pairing CANCEL with SCOPE_END is allowed (= the
 * second SET_TOP is a no-op store), so cleanup paths that fall through to
 * the END are fine. */
#define ARO_ROOT_SCOPE_CANCEL(c, name)                                         \
    AROH_ROOT_STACK_SET_TOP((c), (name))

/* Slot access.  Release builds (= ASTRO_DEBUG=0) fold to `(name)[i]` because
 * ASTRO_ASSERT compiles to (void)0; the cap binding is `(void)ARO_CAT(...)`d
 * in SCOPE_START so it's just a stack-local const that the optimizer drops. */
#define ARO_ROOT(name, i)                                                      \
    ((name)[(ASTRO_ASSERT((size_t)(i) < ARO_CAT(_aro_root_cap_, name)),         \
             (size_t)(i))])

void  aro_gc_init(CTX *c);

/* aro_gc_account_external — tell the framework that `delta` bytes of
 * memory pressure live outside the GC heap (typical use: GMP / FILE *
 * buffers backed by libc malloc that are owned by GC objects via a
 * finalizer).  Positive delta on alloc, negative on free.  The
 * framework adds delta to bytes_since_gc; once threshold is exceeded
 * the next aro_gc_alloc triggers a collect, releasing the external
 * memory through AROH_FINALIZE.
 *
 * Without this hook, sample programs that allocate large external
 * resources (e.g., a `(* s 1103515245)` chain producing megabyte-scale
 * GMP limb buffers) see zero GC pressure from the framework's POV and
 * leak unboundedly until the OS kills them. */
void  aro_gc_account_external(CTX *c, ssize_t delta);

/* ---------------------------------------------------------------------------
 * Root-visitor contract: AROH_VISIT_ROOTS(c, ctx, edge_visit)
 *
 * Sample MUST define this macro in its context.h (or a header that
 * context.h includes) BEFORE any framework backend translation unit
 * pulls in gc.h.  Framework backends invoke it from each GC entry point
 * to iterate over all root slots (= VALUE / pointer storage that is NOT
 * reachable from already-scanned heap objects).
 *
 * For each root slot the macro body invokes:
 *   - ARO_GC_VISIT_EDGE((ctx), edge_visit, slot)  — for VALUE slots
 *   - ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, slot)  — for raw typed-ptr slots
 *
 * Layout examples:
 *   - baruby_precise: linear range c->env .. c->sp (= VALUE *).
 *     Defined inline in context.h = zero function-call overhead.
 *   - ascheme_precise: c->env (= struct sframe *), c->globals[*].value,
 *                      c->next_env (= tail-call pending), etc.
 *     Body is large; macro forwards to a sample-local function.
 *
 * `edge_visit` is the backend's per-slot callback (= forward_edge /
 * mark_edge / fwd_edge_compact / ...).  `ctx` is the backend's ASTroGC*.
 * Both are opaque to the sample.
 *
 * iter 76: framework は CTX-opaque 化。 backend は `c->sp` / `c->env` を
 * 直接 access しない (= sample stack convention に縛られない)。 root scan
 * の loop はすべてこの macro 経由。 */
#ifndef AROH_VISIT_ROOTS
#  error "Sample must define AROH_VISIT_ROOTS(c, ctx, edge_visit) " \
         "in its context.h before any framework gc_*.c is compiled"
#endif

/* aro_gc_fini — tear down the per-instance ASTroGC: release backend
 * resources (mmap'd regions, free-lists, mark bitmaps, etc.) and free
 * the heap-allocated ASTroGC struct itself.  Sets `c->astro_gc` to
 * NULL.  Safe to call on an already-finalized instance (no-op).
 *
 * On process exit the OS reclaims everything anyway, but a symmetric
 * fini matters for: (a) multi-instance use (creating + destroying
 * multiple GCs in one process), (b) valgrind / leak-sanitizer clean
 * runs, (c) future tests that re-init mid-process. */
void  aro_gc_fini(CTX *c);

/* Helpers for aro_gc_fini: release a backend's `large_head` chain.
 *
 * mmap-backed (LargeObj layout: { LargeObj *next; size_t map_bytes; ... }
 *              used by gc_mark / gc_mark_freelist / gc_immix / gc_mark_gen
 *              / gc_mark_gen_inc / gc_mark_bitmap_gen / gc_mark_card_gen /
 *              gc_immix_gen): munmap each entry.
 *
 * malloc-backed (LargeObj layout: { LargeObj *next; ... } used by
 *                gc_copy / gc_mark_compact): free each entry.
 *
 * Backends cast their `LargeObj *` to (void *)gc->large_head.  Both
 * helpers assume `next` is the first field — true for all our backends. */
struct AroGcLargeChainMmap   { struct AroGcLargeChainMmap   *next; size_t map_bytes; };
struct AroGcLargeChainMalloc { struct AroGcLargeChainMalloc *next; };

#include <sys/mman.h>
#include <stdlib.h>

static inline void
aro_gc_free_large_chain_mmap(void *head)
{
    struct AroGcLargeChainMmap *lo = (struct AroGcLargeChainMmap *)head;
    while (lo) {
        struct AroGcLargeChainMmap *next = lo->next;
        munmap(lo, lo->map_bytes);
        lo = next;
    }
}

static inline void
aro_gc_free_large_chain_malloc(void *head)
{
    struct AroGcLargeChainMalloc *lo = (struct AroGcLargeChainMalloc *)head;
    while (lo) {
        struct AroGcLargeChainMalloc *next = lo->next;
        free(lo);
        lo = next;
    }
}

/* aro_gc_alloc — allocate `payload_size` bytes for a sample-defined
 * scan-safe object.  Category = SCAN (= sample's SCAN_EDGES dispatches
 * at scan time, typically via sample's own ObjectHeader.type).
 *
 * **Return type**: VALUE = raw payload pointer cast to integer.  Sample
 * stores the result directly into a GC-visible slot (= sp[], object
 * field) and accesses via ARO_LOAD or plain cast.
 *
 * **CONTRACT 1 (zero-init)**: backend zero-inits the payload so a GC
 * scan immediately after alloc sees no stale heap-pointer bits.
 *
 * **CONTRACT 2 (GC-scan bound)**: caller MUST have set `c->sp` to its
 * current spill top before calling.  `c->env..c->sp` defines the root
 * scan range during any inner GC trigger. */
/* Backend-provided raw alloc — returns the payload pointer
 * (= AroObjectHeader * at offset 0).  Public API `aro_gc_alloc` casts
 * to VALUE in gc_common.c. */
void *aro_gc_alloc_raw(CTX *c, size_t payload_size);
VALUE aro_gc_alloc(CTX *c, size_t payload_size);

/* True iff p is in a retired (mprotect'd) PURGE plane — dereferencing would
 * SEGV.  Sample code can skip such an escaped/stale pointer defensively.
 * Only the copy backend (gc_copy.c) defines this; PURGE is a copy-GC feature. */
#include <stdbool.h>
bool aro_gc_addr_retired(CTX *c, const void *p);

/* aro_gc_alloc_byte — allocate `payload_size` raw bytes (no VALUE
 * scanning, no zero-init).  Used for BaString.bytes / other char[]
 * payloads.  Caller fills the bytes after return (decoded via
 * ARO_LOAD to obtain the writeable `char *`).  GC's heap walk skips
 * this category, so leftover freelist-link bytes are harmless.
 * Same c->sp contract as aro_gc_alloc. */
void *aro_gc_alloc_byte_raw(CTX *c, size_t payload_size);
VALUE aro_gc_alloc_byte(CTX *c, size_t payload_size);

/* aro_gc_realloc_payload / _byte_payload — grow a payload, copying contents.
 *
 * iter 76: framework は CTX-opaque 化したため、 これらは sample 側で実装
 * する。 park ロジックが sample stack convention (= c->sp slot 経由など)
 * に依存するため framework に置けない。
 *
 * 典型的な sample 実装パターン:
 *   1. in-place 成長を `aro_gc_realloc_in_place(c, old, new_size)` で試行。
 *      成功なら memcpy 不要で返す。
 *   2. old payload size を `aro_gc_size_of(old)` で取得。
 *   3. old payload を sample の root slot (= sp[0] 等) に park。
 *   4. `aro_gc_alloc(c, new_size)` (= scan-safe 版) または
 *      `aro_gc_alloc_byte(c, new_size)` (= byte 版) を呼ぶ。
 *   5. park slot から old を再 deref して memcpy。
 *   6. `aro_gc_reset_payload_header(newp, new_size)` で head を fresh state に。 */
void *aro_gc_realloc_payload(CTX *c, void *p, size_t new_size);
void *aro_gc_realloc_byte_payload(CTX *c, void *p, size_t new_size);

/* Restore framework-owned head fields after a sample realloc helper's
 * `aro_gc_alloc` + memcpy.  See gc_common.c. */
void aro_gc_reset_payload_header(void *payload, size_t new_size);

/* Backend-provided header accessor — given a payload pointer (the
 * value returned by aro_gc_alloc / aro_gc_alloc_byte), return the
 * stored size.  Used by the shared aro_gc_realloc_payload default in
 * gc_common.c to compute copy_bytes. */
size_t    aro_gc_size_of(void *payload);

/* Optional backend hook: try to grow `old` in place (no alloc + memcpy).
 * Returns the new payload pointer on success; the underlying buffer may
 * have moved (e.g., via realloc(3) → mremap), in which case the caller
 * must use the returned pointer.  Returns NULL to fall through to the
 * default alloc + memcpy path.
 *
 * Default impl in gc_common.c is `__attribute__((weak))` and returns
 * NULL.  Backends that own large objects on a malloc-backed list (e.g.,
 * gc_copy / gc_mark_compact via LargeObj) override this to call
 * realloc(3) on the matching LargeObj for cheap doublings of
 * BaArray.items / BaString.bytes during sieve / hash_chain workloads.
 *
 * NOT called from stress mode (we want every alloc to GC). */
void *aro_gc_realloc_in_place(CTX *c, void *old, size_t new_size);

void  aro_gc_collect(CTX *c);

/* Optional backend hook: enumerate every allocated payload in the heap
 * (backs a sample's ObjectSpace.each_object).  Returns false when the
 * backend cannot walk — the default in gc_common.c is weak and does so.
 *
 * The visitor MUST NOT allocate: a GC mid-walk moves objects and leaves
 * the cursor pointing at from-space.  It may see garbage that is not yet
 * collected (CRuby's each_object has the same property).  Nothing is
 * spent on this unless it is called — no per-alloc or per-collect
 * bookkeeping exists to support it. */
bool  aro_gc_each_object(CTX *c, void (*visit)(void *arg, void *payload), void *arg);

/* ---------------------------------------------------------------------------
 * Finalizer API — weak-reference + post-mark sweep pass.
 *
 * Use case: sample-allocated payload owns libc-malloc'd inner buffer that
 * the GC framework can't see (e.g., GMP's mpz/mpq internal limbs, FILE *
 * for an OBJ_PORT, etc.).  Without a finalizer hook the inner buffer leaks
 * when the payload becomes unreachable and is reclaimed by GC.
 *
 * Workflow:
 *   1. Sample calls `aro_gc_finalize_register(c, payload)` immediately
 *      after `aro_gc_alloc` so the GC framework "weakly" tracks payload.
 *      Weak = list is NOT scanned during root walk / SCAN_EDGES; the
 *      tracked object is kept alive only by ordinary references.
 *   2. After mark/forward, each backend's collect entry calls
 *      `aro_gc_finalize_walk(c)`.  Walk asks the backend
 *      `aro_gc_finalize_check(c, payload)` per entry:
 *        - returns payload (or new addr post-move) → alive → entry updated
 *        - returns NULL                              → dead  → ASTRO_GC_
 *                                                       FINALIZE invoked +
 *                                                       entry dropped
 *   3. Sample's `AROH_FINALIZE(payload)` macro (defined in context.h)
 *      reads payload's type tag and runs the cleanup (mpz_clear, etc.).
 *      MUST be defined by sample (= compile error otherwise — see below).
 *
 * Re-entrancy: register MUST NOT trigger GC (= a register that grows the
 * list invokes realloc; this is libc malloc, NOT aro_gc_alloc, so safe).
 * Finalize callback runs OUTSIDE any GC critical section's user-visible
 * effect window — payload backing memory has already been logically
 * reclaimed, the callback just releases external resources. */
void  aro_gc_finalize_register(CTX *c, void *payload);
void *aro_gc_finalize_check   (CTX *c, void *payload);
void  aro_gc_finalize_walk    (CTX *c);

/* Release the finalize_list backing storage.  Called from each backend's
 * aro_gc_fini.  Does NOT invoke AROH_FINALIZE on the still-live
 * entries — the process is exiting and the OS reclaims the inner buffers
 * anyway.  This matches the "fini == clean valgrind, not graceful
 * shutdown" contract of the rest of the framework. */
void  aro_gc_finalize_fini    (CTX *c);

#ifndef AROH_FINALIZE
#  error "Sample must define AROH_FINALIZE(payload) in its context.h. " \
         "Use `#define AROH_FINALIZE(payload) ((void)0)` when no " \
         "sample-managed external resource exists."
#endif

/* Stat readers — all take CTX so the data is sourced from the per-instance
 * common state (no global variable). */
static inline size_t aro_gc_total_bytes      (CTX *c) { return ARO_GC_COMMON(c)->stats.total_bytes;       }
static inline size_t aro_gc_heap_bytes       (CTX *c) { return ARO_GC_COMMON(c)->stats.heap_bytes;        }
static inline size_t aro_gc_count            (CTX *c) { return ARO_GC_COMMON(c)->stats.gc_count;          }
static inline size_t aro_gc_minor_count      (CTX *c) { return ARO_GC_COMMON(c)->stats.minor_count;       }
static inline size_t aro_gc_major_count      (CTX *c) { return ARO_GC_COMMON(c)->stats.major_count;       }
static inline double aro_gc_total_seconds    (CTX *c) { return ARO_GC_COMMON(c)->stats.total_seconds;     }
static inline double aro_gc_max_pause_seconds(CTX *c) { return ARO_GC_COMMON(c)->stats.max_pause_seconds; }
static inline double aro_gc_mark_seconds     (CTX *c) { return ARO_GC_COMMON(c)->stats.mark_seconds;      }
static inline double aro_gc_reclaim_seconds  (CTX *c) { return ARO_GC_COMMON(c)->stats.reclaim_seconds;   }

// Helper used inside each backend's collect entry point — accumulates wall
// time into c->astro_gc_stats.total_seconds.  Re-entrant: if a major calls
// minor (gc_mark_compact_gen), only the outermost begin/end pair times
// the work; inner pairs are no-ops via the depth counter (also in CTX).
//
// All timer state (depth + start timestamp + stats fields) lives inside
// CTX, so backends can't accidentally rely on hidden global mutable
// state.  The `struct timespec` return is kept only for API compat —
// the real depth/start tracking happens through `c`.

static inline struct timespec
aro_gc_time_begin(CTX *c)
{
    AroGcCommonState *cs = ARO_GC_COMMON(c);
    struct timespec t = {0, 0};
    if (cs->time_depth++ == 0) {
        clock_gettime(CLOCK_MONOTONIC, &cs->time_t0);
    }
    return t;
}

static inline void
aro_gc_time_end(CTX *c, struct timespec t0)
{
    (void)t0;
    AroGcCommonState *cs = ARO_GC_COMMON(c);
    if (--cs->time_depth == 0) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = (double)(t1.tv_sec  - cs->time_t0.tv_sec) +
                    (double)(t1.tv_nsec - cs->time_t0.tv_nsec) / 1e9;
        cs->stats.total_seconds += dt;
        if (dt > cs->stats.max_pause_seconds) {
            cs->stats.max_pause_seconds = dt;
        }
    }
}

// Phase-level timer: brackets a sub-phase (mark, sweep, slide, etc.) within
// a single collection and adds the elapsed time to *phase_field.  Unlike
// aro_gc_time_begin / _end this does NOT use a depth counter — phases are
// expected to be flat (caller wraps each non-overlapping section).
static inline struct timespec
aro_gc_phase_begin(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

static inline void
aro_gc_phase_end(struct timespec t0, double *phase_field)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec  - t0.tv_sec) +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    *phase_field += dt;
}

// Write barrier.
//
// `holder` is the heap object (payload pointer) that contains `slot`.  For
// item-array writes (`a->items->data[i] = v`) holder is the BaArrayItems
// payload, not the BaArray.  For pointer-field writes (`a->items = new_ptr`)
// holder is the BaArray.  Use NULL for stack-root writes (= no barrier).
//
// For non-WB backends the whole call collapses to `*slot = v` (= zero cost).
//
// For WB backends with the bit-in-head layout (= ARO_GC_WB_OLD_MASK
// defined in gc_types.h), `aro_gc_store` inlines the FAST PATH:
//
//   1. *slot = v
//   2. if (holder == NULL || !(OLD && !DIRTY)) return
//
// The COLD path (= set DIRTY + push to remset) is `aro_gc_remember`, an
// out-of-line extern function.  Splitting like this keeps the WB inline-able
// at every callsite — `arr.push` and similar hot writes get a few inline
// instructions plus a rarely-taken branch instead of a full function call.
//
// For WB backends without bit-in-head layout (mark_bitmap_gen / mark_card_gen
// use per-page bitmaps; mark_gen_inc has an extra SATB barrier), the whole
// aro_gc_store stays extern.  LTO may still inline it but there's no source-
// level guarantee.

#ifdef ARO_GC_HAS_WB

#ifdef ARO_GC_WB_OLD_MASK
/* Bit-in-head backends: inline fast-path check, out-of-line `remember`
 * does the actual work (= set DIRTY + push holder to remset).  Single
 * `remember` is shared by the slot and bulk WB — the action is
 * holder-centric, not slot-centric. */
void aro_gc_remember(CTX *c, AroObjectHeader *h);

static inline void
aro_gc_store(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (__builtin_expect(holder == NULL, 1)) return;
    AroObjectHeader *h = (AroObjectHeader *)holder;
    if (__builtin_expect(
        (h->gc_flags & (ARO_GC_WB_OLD_MASK | ARO_GC_WB_DIRTY_MASK))
            != ARO_GC_WB_OLD_MASK, 1)) return;
    aro_gc_remember(c, h);
}

static inline void
aro_gc_store_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (__builtin_expect(holder == NULL, 1)) return;
    AroObjectHeader *h = (AroObjectHeader *)holder;
    if (__builtin_expect(
        (h->gc_flags & (ARO_GC_WB_OLD_MASK | ARO_GC_WB_DIRTY_MASK))
            != ARO_GC_WB_OLD_MASK, 1)) return;
    aro_gc_remember(c, h);
}

#else  /* bitmap_gen / card_gen / mark_gen_inc: WB fully out-of-line */

void aro_gc_store     (CTX *c, void *holder, VALUE *slot, VALUE v);
void aro_gc_store_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n);

#endif

#else  /* non-WB backends: WB collapses to a plain store. */

static inline void
aro_gc_store(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    (void)c; (void)holder;
    *slot = v;
}

static inline void
aro_gc_store_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    (void)c; (void)holder;
    if (n) memcpy(dst, src, n * sizeof(VALUE));
}
#endif

#endif

/* ---------------------------------------------------------------------------
 * ARO_STORE / ARO_STORE_BULK — sample-facing store API, paired with ARO_LOAD.
 *
 * Forms the read/write pair sample code should use uniformly for heap-pointer
 * accesses:
 *   value  = (T *)ARO_LOAD(c, &slot);                         // read
 *   ARO_STORE(c, holder, &slot, (VALUE)value);                // write
 *
 * Underlying inline / extern implementation is `aro_gc_store` (= ex
 * `aro_gc_wb`).  For non-WB backends both fold to a plain store.
 * ------------------------------------------------------------------------- */
/* (uintptr_t) cast strips the `const` introduced by ARO_GC_EDGE in audit
 * builds.  Sample code that bypasses ARO_STORE (e.g. `a->items = x`)
 * cannot perform this cast at the assignment site, so the const enforces
 * routing through this macro.  Release builds have no const, the cast is
 * a no-op. */
#define ARO_STORE(c, holder, slot, v) \
    aro_gc_store((c), (holder), (VALUE *)(uintptr_t)(slot), (v))
#define ARO_STORE_BULK(c, holder, dst, src, n) \
    aro_gc_store_bulk((c), (holder), (VALUE *)(uintptr_t)(dst), (src), (n))

/* ARO_GC_RAW_STORE — direct write to an ARO_GC_EDGE-qualified slot that
 * bypasses BOTH the write barrier AND the audit const qualifier.  Use
 * ONLY for framework-internal writes that are exempt from WB:
 *   1) stack-allocated (alloca / on-stack) struct init — holder is not
 *      a GC object so reading head.gc_flags is undefined;
 *   2) SCAN_EDGES forwarding writeback — GC is running, mutator paused,
 *      remset doesn't apply;
 *   3) storing a value that is provably NOT a heap edge — NULL, or a tagged
 *      non-pointer sentinel (low bits set; the edge filter skips it) — so no
 *      old->young reference can be created.
 * Mutator writes that store a real heap pointer into an already-live heap
 * object MUST use ARO_STORE instead.
 *
 * The value's bytes are copied through the slot's element type (memcpy strips
 * the audit const cleanly; __typeof__ keeps the slot's size, so VALUE /
 * typed-ptr / char * slots all work).  Release builds (ARO_GC_EDGE empty) fold
 * the fixed-size memcpy to a plain store — identical to a direct assignment. */
#define ARO_GC_RAW_STORE(slot, val) \
    do { __typeof__(*(slot)) _aro_rs_ = (val); \
         __builtin_memcpy((void *)(uintptr_t)(slot), &_aro_rs_, sizeof(_aro_rs_)); } while (0)
