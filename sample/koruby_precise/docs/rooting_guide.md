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

## 0. THE ABSOLUTE RULE — never write `c->sp_top`

**Writing `c->sp_top` is forbidden.** Not "discouraged", not "only in alloc
helpers" — **forbidden everywhere**: cfunc bodies, AST dispatchers, *and* the
alloc helpers themselves. Any `c->sp_top = …` is debt to delete, not a pattern
to copy. The field itself is being eliminated.

**Why:** the live root-stack top is *threaded as the `sp` parameter* down the
call chain (register-resident). Storing it back into the CTX (`c->sp_top = …`)
creates a second source of truth that goes out of sync — a caller bumps it to
hold a slot, an inner alloc helper rewinds it with `c->sp_top = sp`, and the
caller's slot silently falls out of the scanned range → stale handle → SEGV.
This exact class burned us repeatedly. The fix is structural: **the GC reads the
root boundary from the threaded `sp`, never from a stored field.**

**What this means when you write code:**
- The **one** function that writes `c->sp_top` is the wrapper
  `korb_alloc(c, top, size)` (object.c): it sets `c->sp_top = top` and calls
  `aro_gc_alloc`. Every real allocation goes through it; nothing else writes the
  field (Fiber switch aside). An alloc helper takes `(CTX *c, VALUE *sp, …)` and
  calls `korb_alloc(c, sp + N, size)` — it never writes `c->sp_top` itself.
- To hold scratch across an allocation, pass `sp + N` to the helper — never
  reserve by writing `c->sp_top`.
- To root a handle across a non-alloc GC point (funcall / yield / intern), use a
  **frame park** (IDIOM C) or `yield_self_chain` (IDIOM C-variant). Those write
  `c->current_frame` / `c->yield_self_chain`, **never** `c->sp_top`.
- If a handle already sits in a scanned slot (`self`, an arg), just **re-read** it
  (IDIOM A).

The idioms below are written to this rule. IDIOM B (the old "bump `c->sp_top` to
park") is **retired and forbidden**; it is listed only so you recognise and
remove it.

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
    korb_ary_push(c, sp, sp[-argc-1], Qnil);             // pass threaded sp as staging base — NOT c->sp_top
```

### IDIOM B — value-stack park ⚠️ FORBIDDEN (writes `c->sp_top` — being removed)
> **Do NOT use this.** Writing `c->sp_top` from a cfunc / dispatcher / alloc
> helper is **forbidden** (see §0). The old "park by bumping `c->sp_top`" pattern
> is the single biggest source of remaining writes and is being eliminated
> wholesale. If you find it in the code, it is debt to migrate, not an example to
> copy. Replacements:
> - **≤2 handles across a GC point** → frame park (IDIOM C): the frame's
>   `last_line` / `last_match` slots are scanned with no `c->sp_top` write.
> - **a handle that already lives in a scanned slot** (`self`, an arg) → just
>   **re-read** it (IDIOM A) — no park needed.
> - **>2 handles** → re-fetch from a stable container each use (IDIOM E), or push
>   one frame per pair.
>
> The historical form was `VALUE *root = c->sp_top; …; c->sp_top = root + N; …;
> c->sp_top = root;`. Every such site must go.

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

#### IDIOM C-variant — `yield_self_chain` park (one handle, across arbitrary eval)
When you need to root **a single handle across a whole sub-evaluation** (not a
loop) — e.g. a node evaluator that must preserve a saved value across `body` +
`ensure` + `rescue` clauses — pushing a full synthetic `korb_frame` is overkill.
`c->yield_self_chain` is a lighter LIFO of `struct korb_yield_self_save {VALUE
self; struct korb_yield_self_save *prev;}` that `visit_roots` also walks
regardless of `sp_top`, so a slot parked there survives a raise-unwind too:

```c
struct korb_yield_self_save save = { .self = x, .prev = c->yield_self_chain };
c->yield_self_chain = &save;          // park
RESULT r = EVAL_ARG(c, some_node);    // GC points, may raise/unwind
x = save.self;                        // re-read forwarded handle
c->yield_self_chain = save.prev;      // unpark on EVERY exit path
```
- **LIFO discipline:** nest saves so the inner one pops first (`save.prev`
  captures the chain *at push time*; unpark = restore to `save.prev`). Re-park
  fresh after a `goto retry`.
- **Idempotent unpark:** writing `c->yield_self_chain = save.prev` twice is safe
  *iff* nothing else pushed since — handy when several exit paths share a tail.
- Canonical: `node_ensure` / `node_rescue` / `node_rescue_else` parking the saved
  `$!` (`prev_bang`) across the body/rescue/ensure clauses — a raw C-local there
  goes stale and gets written back into `$!`, so a later bare `raise` re-raises a
  moved-out exception (commit `b89a68b9`).

### IDIOM D — walk cursor (class hierarchy / linked walks)
Walking `k = k->super` (or includes/prepends) while each step is a GC point: the
cursor moves. Park the result **and** the cursor in a **frame** (two slots —
`last_line` / `last_match`), re-derive the class from the slot before every use,
and advance via the re-read super. **No `c->sp_top` write:**

```c
struct korb_frame fr = {
    .prev = c->current_frame, .self = c->current_frame->self,
    .fp = c->current_frame->fp, .cref = c->current_frame->cref,
    .current_class = c->current_frame->current_class,
    .current_file = c->current_frame->current_file,
    .last_line  = korb_ary_new(c, sp),   // result (sp = the threaded staging base)
    .last_match = self,                  // walk cursor
};
c->current_frame = &fr;
while (fr.last_match != 0 && !SPECIAL_CONST_P(fr.last_match)) {
    struct korb_class *k = (struct korb_class *)fr.last_match;
    for (int i = k->includes_cnt-1; i >= 0; i--)
        push_module(c, fr.last_line, ((struct korb_class *)fr.last_match)->includes[i]);
    fr.last_match = (VALUE)((struct korb_class *)fr.last_match)->super;   // advance via re-read
}
VALUE result = fr.last_line;
c->current_frame = fr.prev;              // restore on EVERY exit
return RESULT_OK(result);
```
Canonical: `class_ancestors` (frame-park form).

### IDIOM E — re-fetch from a stable container (when slot pressure is tight)
If you can't spare parking slots, re-derive a handle from a **stable** place each
iteration: `self` from `sp[-argc-1]`; a class from `((korb_object *)sp[-argc-1])->basic.klass`;
a Struct's members via `korb_const_get_inherited(re-derived-class, …)`. The const
table / receiver slot are scanned, so a fresh fetch is always live.
Canonical: `_struct_inspect`, `struct_to_h`.

## 4. Special hazards (do NOT do these)

- **Writing `c->sp_top` anywhere.** The cardinal sin — see §0.
- **libc shadow buffers for moving handles.** Copying moving VALUEs into a
  `korb_xmalloc` array and holding it across a GC point: the heap pointers inside
  the libc buffer are *outside* the scan range → stale. Park the sources in a
  **frame** (IDIOM C) instead. (Old `ary_concat` / `str_prepend` bug.)
- **Returning the entry-time `self`** after an internal GC. Re-read it.
- **`a->len` / `members->len` in a loop condition** read from a stale cached
  handle. Re-derive in the condition.
- **A second `korb_funcall` on the same by-value arg** after a `respond_to?`
  funcall — the arg moved. Re-read it (IDIOM A) before the second call.
- **`korb_yield` with handles held in C-locals across the body** — use IDIOM C.

## 5. The `c->sp_top` rule
**Never write `c->sp_top`.** This section used to sanction "a builtin reserves
park slots via `c->sp_top = sp + K`" — that is now **withdrawn and forbidden**
(see §0). `sp` is threaded as a parameter and carried to the GC by the alloc
helper; nothing stores it back into the CTX. To hold scratch across an
allocation, pass `sp + N` to the helper. To park across a non-alloc GC point,
use a frame (IDIOM C) or `yield_self_chain` (IDIOM C-variant). Migration of the
remaining writes is the top-priority campaign (`[[project_koruby_precise_sp_threading]]`).

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

### 6.1 RESULT-audit — pinpoint a stale handle to `file:line` (debugging aid)
The STRESS+PURGE SEGV tells you a stale handle *exists* but crashes at the
*deref*, often far from where the handle went stale. The **RESULT-audit** build
(`-DKORB_RESULT_AUDIT`, fully `#ifdef`-gated → zero cost in release) names the
exact site. It has three detectors, all keyed off the copy GC's to-space bounds
(`aro_gc_addr_stale`) and a logical "GC-could-have-fired" clock
(`korb_g_gc_clock`, ticked at every alloc via the `AROH_GC_SAFEPOINT()` contract
hook — deterministic, independent of whether STRESS actually collected):

1. **construction-time** (`RESULT_MK`): fires when a *stale* raw VALUE is wrapped
   into a RESULT → catches "I returned an already-moved handle".
2. **held-across-GC** (`gc_clock` stamped in RESULT, checked by `RESULT_VAL` /
   `UNWRAP`): fires when a RESULT's `.value` is read after the clock advanced →
   catches "I held a RESULT across a GC point then used `.value`".
3. **store-time** (`KORB_AUDIT_OBJECT(v, "what")` — general object audit, "living"
   is its first check; `SP_SET(sp,i,v)` runs it on every value-stack store):
   fires when a bad value is *written* into a sink (`korb_gvar_set`,
   `korb_ivar_set`, the value stack) → catches the save/restore-of-a-raw-snapshot
   bug at the store, the closest point to the root.

Usage:
```
rm -f *.o koruby_precise && CCACHE_DISABLE=1 make CC="gcc -DKORB_RESULT_AUDIT"
ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1 gdb -batch -ex run -ex 'bt 12' \
    --args ./koruby_precise test/cruby_runner/run_rubyspec.rb <spec>
```
The `*** RESULT-AUDIT(...)` line gives `file:line`; the `bt` gives the callsite.
After the fix, the audit run must be silent **and** the release SEGV must be
gone. **Restore the release binary** (`rm -f *.o koruby_precise && make`) before
normal use. Add a fresh `KORB_AUDIT_OBJECT` at any new store sink you suspect
(or use `SP_SET` for value-stack stores). Full design: memory
`[[project_koruby_result_audit]]`.

## 7. Writing-a-builtin checklist
- [ ] **No `c->sp_top = …` anywhere** (per §0). Pass `sp` / `sp + N` to alloc
      helpers; never store the root top in the CTX.
- [ ] Is `self`/each arg a moving handle (per §1)?
- [ ] Does the body cross a GC point (per §2) before its last use / return?
- [ ] If yes: pick an idiom — A (re-read slot) by default; C (frame) for handles
      with no slot / across a yield; D (frame cursor) for walks; E (re-fetch)
      under slot pressure. **Never B** (retired/forbidden).
- [ ] Re-derive array/length in loop conditions, not from a cached handle.
- [ ] Return the **re-read** handle.
- [ ] Restore `c->current_frame` on **every** exit, incl. raise.
- [ ] Run §6.

## 8. Migrating an existing `c->sp_top =` write (the elimination campaign)

Goal: `grep -rn 'c->sp_top *=' builtins/ node.def object.c` → **0** (Fiber
switch aside). Classify each write and apply the matching move; verify per file
against the §6 gate (a load-bearing park removed wrong = STRESS+PURGE SEGV).

1. **Alloc-preceding** — `c->sp_top = TOP; X = aro_gc_alloc(c, SIZE);`
   → `X = korb_alloc(c, TOP, SIZE);`. The write moves into the single wrapper.
   These live only in object.c's handle constructors.
2. **Staging-publish before a self-publishing helper** — `c->sp_top = sp + N;`
   right before `korb_ary_new` / `korb_ary_push` / `korb_str_new` / … (any helper
   that takes an `sp` and allocates). **Delete the write** and make sure the
   helper is passed `sp + N` (or higher) as its staging base: the helper's
   `korb_alloc` republishes a top ≥ `sp + N`, so the caller's `sp[0..N-1]` stay
   scanned. No explicit write needed.
3. **Park across a non-alloc GC point** — `c->sp_top = sp + N; korb_funcall(…)`
   / `korb_yield(…)` / `korb_intern(…)`, holding `sp[0..N-1]` live across it.
   This is the retired IDIOM B. → **frame park** (IDIOM C): move the live values
   into a synthetic frame's `last_line`/`last_match` (or re-fetch, IDIOM E, for
   >2). The frame chain is scanned regardless of `c->sp_top`.

**The case-3 caveat — verify before deleting.** Case 2's safety relies on the
callee threading `sp` down to its `korb_alloc`. A bare `korb_funcall` does NOT
take a staging `sp` from you — its callee runs at its own stack level — so a
value you staged below it is kept live only as long as `c->sp_top` covers it.
That is exactly why these were IDIOM B. **Do not just delete a case-3 write** —
convert it to a frame park, or the staged slot silently leaves the scan range.
When unsure which case you have, treat it as case 3 (frame park is always safe).

**Same pass: adopt `SP_SET`.** While editing a file, convert its `sp[i] = v`
value-stack stores to `SP_SET(sp, i, v)` (§6.1) so the object audit covers them.
Touch each site once.

Cross-refs: `docs/array_moving_gc.md` (campaign log), `docs/stress_rooting_spec.md`
(iterator P1/P2 origin), memory `[[project_koruby_precise_gc_bump_migration]]`,
`[[sp_top-design-rule]]`, `[[project_koruby_precise_sp_threading]]`.
