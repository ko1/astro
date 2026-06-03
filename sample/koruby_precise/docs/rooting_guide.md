# koruby_precise — moving-GC rooting guide (MUST READ before writing builtins)

koruby_precise uses a **precise copying/moving GC**. Heap objects are
**relocated** on every collection (and under `ASTRO_GC_STRESS=1` a collection
fires on *every* allocation). Therefore a C-local that holds a heap **handle**
across any **GC point** becomes a dangling pointer the instant the object
moves. Under `ASTRO_GC_PURGE=1` the old location is `mprotect(PROT_NONE)`'d, so
the next deref is an immediate SEGV — which is exactly how we find these bugs.

This document is normative. **Any builtin / node that touches a moving handle
across a GC point MUST follow one of the idioms below**, and MUST pass the
STRESS+PURGE gate (see §6). Logic changes are never needed — this is pure
rooting hygiene.

---

## 1. What is a *moving handle*?

Every object allocated through `aro_gc_alloc` moves. In koruby_precise that is:

| Type | Handle struct | Moves? | Notes |
|---|---|---|---|
| Array | `struct korb_array` | **yes** | + a separate moving `T_ARY_BACKING` payload |
| String | `struct korb_string` | **yes** | the handle moves; its `->ptr` buffer is libc-atomic (stable) |
| Range | `struct korb_range` | **yes** | |
| Class / Module | `struct korb_class` | **yes** | this is why ~all class methods need rooting |
| Object (T_OBJECT) | `struct korb_object` | **yes** | |
| Bignum | `struct korb_bignum` | **yes** | |
| Float (boxed T_FLOAT) | | **yes** | flonums are immediate (safe) |

**NOT moving (safe to hold in a C-local across GC):**
- Immediates: Fixnum, Symbol, flonum, `true`/`false`/`nil`, `Qundef`.
- **Hash** (`struct korb_hash`) — libc-allocated, address-stable.
- **Proc / Method / Binding / Fiber** — libc-allocated (but their *contents*,
  e.g. `p->env` slots, hold moving handles the GC forwards separately).
- **korb_method / korb_cref** — libc (but `def_cref->klass` is a moving class).
- AST `NODE` — HEAP_IMMORTAL, never moves.
- libc-atomic byte buffers (`->ptr` of a String) — stable even when the handle moves.

> Rule of thumb: if `BUILTIN_TYPE(v)` of a value can be Array / String / Range /
> Class / Module / Object / Bignum / boxed-Float, it is a moving handle.

## 2. What is a *GC point*?

Anything that can allocate or run Ruby code. Treat **all** of these as GC points
— a handle in a bare C-local is stale on the line *after*:

- any `korb_*_new` / `korb_*_new_capa` / `korb_str_new*` / `korb_float_new` /
  `korb_hash_new` / `korb_class_new` / `korb_object_new` …
- `korb_intern` (may grow the symbol table), `korb_id2sym` of a fresh name.
- `korb_ary_push` / `korb_ary_aset` **when they grow** (re-alloc the backing).
- `korb_str_concat` when it allocs a buffer (libc — usually no GC, but assume yes).
- `korb_funcall` / `korb_funcall_r` / `korb_yield` / `korb_yield_r` / `proc_call`
  (run arbitrary Ruby → arbitrary allocation).
- `korb_class_add_method_ast` / `korb_const_set` / `korb_module_include` /
  `korb_singleton_class_of*` (class mutation allocates).
- `korb_inspect` / `korb_to_s` / `korb_eql` / `korb_to_int_or_raise` /
  `str_coerce_arg` (they dispatch user code).
- `korb_raise` / `korb_const_get` of a not-yet-interned name.

`korb_xmalloc*` / `korb_xrealloc` / `memcpy` are **libc** → not GC points.

## 3. The idioms

### IDIOM A — re-read from the receiver/arg slot (default for `self` and args)
`self = sp[-argc-1]` and `argv[i] = (sp-argc)[i]` live in the **value stack**,
which `visit_roots` scans and forwards. So after a GC point, **don't trust the
C-local — re-read the slot**:

```c
VALUE iv = UNWRAP(korb_to_int_or_raise(c, argv[0]));   // GC point
self = sp[-argc - 1];                                  // ← re-read; old `self` is stale
struct korb_array *a = (struct korb_array *)self;
```

Always **return the re-read handle**, never the entry-time C-local:
```c
return RESULT_OK(sp[-argc - 1]);   // NOT RESULT_OK(self)
```

When a loop both allocates and reads the array, re-derive `a` **inside the
loop / in the condition**:
```c
while (((struct korb_array *)sp[-argc-1])->len < need)   // re-derive in the condition!
    korb_ary_push(c, c->sp_top, sp[-argc-1], Qnil);      // push to the re-read handle
```

### IDIOM B — value-stack park (for things with no slot to re-read)
A handle held only in a C-local — e.g. a **by-value function argument**, a
freshly-coerced value, a per-element temporary — has no receiver/arg slot.
Park it on the value stack (it then sits in the scan range) and read it back:

```c
VALUE *const root = c->sp_top;
root[0] = x; root[1] = y;
c->sp_top = root + 2;                 // publish: now [stack_base, sp_top) covers root[0..1]
RESULT r = korb_funcall(c, root[0], id, 1, &root[1]);   // GC point
... use root[0] / root[1] (forwarded) ...
c->sp_top = root;                     // pop on EVERY exit path
```
- Use **explicit `RESULT` checks, not `UNWRAP`**, on the parked region — `UNWRAP`
  early-returns on raise and would **leak the park** (and skip the pop).
- Canonical: `korb_to_int_or_raise`, `str_coerce_arg`, `ary_sort_compare`.

### IDIOM C — synthetic frame park (for handles that must survive a yield)
`korb_yield` / `proc_call` run the block body at a **lower** `sp_top` (the
block's env), so value-stack parks above that level are **NOT scanned during the
body**. Park such handles in a **synthetic frame** instead — the frame chain is
always walked regardless of `sp_top`:

```c
struct korb_frame fr = {
    .prev = c->current_frame, .self = c->current_frame->self,
    .fp = c->current_frame->fp, .cref = c->current_frame->cref,
    .current_class = c->current_frame->current_class,
    .current_file = c->current_frame->current_file,
    .last_line = arr,          // park #1 (the array)
    .last_match = sp[-argc-1], // park #2 (self)
};
c->current_frame = &fr;
for (long i = 0; i < n; i++) {
    VALUE el = korb_ary_items((struct korb_array *)fr.last_line)[i];  // re-derive each iter
    RESULT y = korb_yield_r(c, 1, &el);
    if (y.state != KORB_NORMAL) { c->current_frame = fr.prev; return y; }  // restore on raise!
}
VALUE selfr = fr.last_match;
c->current_frame = fr.prev;            // restore on EVERY exit
return RESULT_OK(selfr);
```
- A frame gives you **two** parking slots (`last_line`, `last_match`). Need more?
  re-fetch from a stable source each iteration instead (see IDIOM E).
- `.self` MUST inherit the caller's self (not the parked value) or ivar lookups
  inside the body break.
- You MUST restore `c->current_frame = fr.prev` on **every** exit, including raise.
- There are `KORB_ARY_YIELD_FRAME` / `KORB_HASH_YIELD_FRAME` macros (array.c /
  hash.c) for the common single-park case.
- Alternative to mid-loop yields: **build the full result first (no yield → the
  value-stack parks survive), then yield each element afterward** from a frame
  (see `str_split`, `struct_each`).

### IDIOM D — walk cursor (class hierarchy / linked walks)
Walking `k = k->super` (or includes/prepends) while each step is a GC point: the
cursor moves. Park the cursor on the value stack and re-derive the class from
the slot before every use, advance via the re-read super:

```c
sp[0] = korb_ary_new(c, sp + 1);   // result
sp[1] = self;                      // walk cursor
c->sp_top = sp + 2;
while (sp[1] != 0 && !SPECIAL_CONST_P(sp[1])) {
    struct korb_class *k = (struct korb_class *)sp[1];
    for (int i = k->includes_cnt-1; i >= 0; i--)
        push_module(c, sp[0], ((struct korb_class *)sp[1])->includes[i]);  // re-derive per push
    sp[1] = (VALUE)((struct korb_class *)sp[1])->super;                    // advance via re-read
}
c->sp_top = sp;
```
Canonical: `class_ancestors`.

### IDIOM E — re-fetch from a stable container (when slot pressure is tight)
If you can't spare parking slots, re-derive a handle from a **stable** place each
iteration: `self` from `sp[-argc-1]`; a class from `((korb_object *)sp[-argc-1])->basic.klass`;
a Struct's members via `korb_const_get_inherited(re-derived-class, …)`. The const
table / receiver slot are scanned, so a fresh fetch is always live.
Canonical: `_struct_inspect`, `struct_to_h`.

## 4. Special hazards (do NOT do these)

- **libc shadow buffers for moving handles.** Copying moving VALUEs into a
  `korb_xmalloc` array and holding it across a GC point: the heap pointers inside
  the libc buffer are *outside* the scan range → stale. Park the sources on the
  **value stack** instead. (Old `ary_concat` / `str_prepend` bug.)
- **Returning the entry-time `self`** after an internal GC. Re-read it.
- **`a->len` / `members->len` in a loop condition** read from a stale cached
  handle. Re-derive in the condition.
- **A second `korb_funcall` on the same by-value arg** after a `respond_to?`
  funcall — the arg moved. Re-read it (IDIOM A/B) before the second call.
- **`korb_yield` with value-stack parks above the block env** — use IDIOM C.

## 5. The `c->sp_top` convention (strict)
- **Allowed:** a builtin reserves its own park slots before a loop
  (`c->sp_top = sp + K`) and restores after (`c->sp_top = sp`). IDIOM B/C/D do this.
- **Forbidden:** writing `c->sp_top = sp + N` at a `korb_ary_push` / `korb_ary_aset`
  *call site* — the push wrapper handles its own staging. Alloc helpers
  (`korb_ary_new_capa`, …) set `c->sp_top` internally; the caller only passes the
  staging base. See `[[sp_top_design_rule]]`.

## 6. Verification discipline (REQUIRED for every change)

1. Force rebuild: `rm -f <changed>.o koruby_precise && CCACHE_DISABLE=1 make` —
   no `error:` / `undefined reference`. (Build is single-shot LTO; removing
   `koruby_precise` is enough, but also `rm` the `.o` of the file you changed.)
2. Reproduce the specific crash before/after under STRESS+PURGE:
   `ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1 timeout 40 ./koruby_precise test/cruby_runner/run_rubyspec.rb <spec>`
   — `rc=139` is the SEGV, `rc=124` is *just slowness* (not a bug), `rc=1` is
   assertion fails (feature gaps, not our concern).
3. **Gate must stay green:** `bash tools/gc_harness.sh all` → `DEFAULT 28/28`,
   `STRESS 27/28` (test_fiber is a known pre-existing flake).
4. Per-directory sweep to confirm no regressions and measure progress:
   STRESS+PURGE over the directory, count `rc>=132` (SEGV).

The STRESS+PURGE sweep **is** the enforcement mechanism — a stale handle that
escapes review is caught as a deterministic SEGV. Keep the gate in CI-spirit:
no change lands that increases the SEGV count.

## 7. Writing-a-builtin checklist
- [ ] Is `self`/each arg a moving handle (per §1)?
- [ ] Does the body cross a GC point (per §2) before its last use / return?
- [ ] If yes: pick an idiom — A (re-read slot) by default; B (park) for by-value
      temporaries; C (frame) if a yield is involved; D (cursor) for walks; E
      (re-fetch) under slot pressure.
- [ ] Re-derive array/length in loop conditions, not from a cached handle.
- [ ] Return the **re-read** handle.
- [ ] Pop `c->sp_top` / restore `c->current_frame` on **every** exit, incl. raise.
- [ ] Run §6.

Cross-refs: `docs/array_moving_gc.md` (campaign log), `docs/stress_rooting_spec.md`
(iterator P1/P2 origin), memory `[[project_koruby_precise_gc_bump_migration]]`.
