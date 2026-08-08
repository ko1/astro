# koruby_precise — CodeQL GC-safety / encapsulation gate

Static checks that keep koruby's **moving-GC pointer rules** enforced.  koruby's
raw payload buffers (`KorbStrBuf` / `KorbArrayItems`) move when the GC runs, so a
raw pointer into them is only valid until the next allocation.  These queries,
plus the `ARO_BORROW` accessor discipline, make that safe by construction.

Design & rationale: [`../docs/c_ext_api_design.md`](../docs/c_ext_api_design.md) §4.1.

## Run it after changes

```sh
make codeql-check        # = sh codeql/run.sh   (~2–3 min; full DB rebuild)
```

Each rule is **self-tested on a fixture** (so a broken query can't silently pass)
and then **required clean on the real koruby build**.  Non-zero exit on any
failure.  Needs the CodeQL CLI (`gh extension install github/gh-codeql`);
`codeql pack install` and the DBs (under `codeql/.db/`, gitignored) are handled
automatically.

## The `ARO_*` attributes (`runtime/aro_gc_effect.h`)

Markers that expand to nothing under gcc (the queries key on the *macro
invocation*, so there is no unknown-attribute warning):

| macro | meaning |
|---|---|
| `ARO_BORROW` | this function hands out / returns a raw pointer into a movable GC object.  **Only** `ARO_BORROW` functions may reach into the raw payload. |
| `ARO_MAYGC` | this function may trigger a GC (reserved for the may-gc effect layer). |
| `ARO_NOGC`  | this function must not trigger a GC (reserved). |

The payload fields are named `data_priv` so that any direct access **outside an
accessor is a compile error** — the compiler is the primary spatial guard, and
the `interior_encapsulation` query is the CodeQL backstop.

## The rules (all currently clean)

| query | kind | what it enforces |
|---|---|---|
| `borrow_after_gc.ql` | **temporal (pointer)** | A raw borrow (an `ARO_BORROW` accessor's return, or a `->data`/`->data_priv` field on `KorbStrBuf`/`KorbArrayItems`) held in a local and **used after a may-GC call** is stale under the moving GC → error.  SSA-precise: re-deriving the pointer (a new SSA def) is treated as safe, so the re-derive-each-iteration idiom does not false-positive.  Follows the borrow through conversions, pointer arithmetic, `&elem`, and local aliasing. |
| `value_after_gc.ql` | **temporal (VALUE)** | The VALUE companion to `borrow_after_gc`: a `VALUE` produced by a may-GC call (a potentially movable heap object) held in a plain C local and **used after another may-GC call** is stale — the moving GC updates rooted slots (`slots[]`, `VALUE_REF` cells) but not a bare local → error.  The safe idiom stages into `slots[]` (an array element, not a `StackVariable`) and re-reads it, which is not flagged; a re-read into a local (`v = slots[i]` again) is a fresh SSA def, also safe.  Follows the VALUE through conversions and local aliasing. |
| `interior_encapsulation.ql` | **spatial** | Direct access to a raw payload field (`KorbStrBuf`/`KorbArrayItems` `::data`/`::data_priv`) **outside an `ARO_BORROW` function** → warning.  Backstops the compiler (field rename): all interior access must go through the accessor chokepoint, so the representation can change by editing accessors alone. |
| `borrow_escape.ql` | **escape** | A raw borrow that **escapes a non-`ARO_BORROW` function** — returned from it, or stored into a struct field / global — hands the caller a borrow without its lifetime → warning.  Fix: mark the function `ARO_BORROW` (if it is deliberately an accessor) or copy the bytes out. |
| `aro_borrow_unused.ql` | **hygiene** | A function marked `ARO_BORROW` whose body **touches no raw payload and calls no accessor** — the annotation is a lie that needlessly exempts it from `interior_encapsulation` and makes `borrow_after_gc` treat its return as a borrow (false positives on callers) → warning.  Fix: remove `ARO_BORROW`. |
| `maygc.ql` | helper | Infers the may-gc effect of every function by transitive closure over direct calls from the single seed `korb_alloc` (the only GC publish point).  Used by `borrow_after_gc` to decide what a "may-GC call" is; not a pass/fail gate. |

## Fixtures (query self-tests)

- `test/borrow_cases.c` — 5 true positives (linear hold / loop-carried hold /
  `&data[i]` / alias / via-accessor) + 3 true negatives (use-before-gc /
  no-gc-between / re-derive-loop) for `borrow_after_gc`.
- `test/value_cases.c` — 3 true positives (local held across may-GC / held
  across a second producer / alias) + 4 true negatives (no-gc-between /
  staged-in-`slots[]` / re-read-from-slot / consumed-as-argument) for
  `value_after_gc`.
- `test/annotation_cases.c` — one escape + one unused-annotation case for
  `borrow_escape` / `aro_borrow_unused`.
- `test/encapsulation_cases.c` — direct-access-outside-accessor case for
  `interior_encapsulation`.

## Files

```
qlpack.yml                  CodeQL pack (deps: codeql/cpp-all)
run.sh                      the gate (invoked by `make codeql-check`)
cqbuild.sh                  clean, ccache-disabled build for DB extraction
borrow_after_gc.ql          temporal check (raw pointer)
value_after_gc.ql           temporal check (bare VALUE)
interior_encapsulation.ql   spatial check
borrow_escape.ql            escape check
aro_borrow_unused.ql        annotation-hygiene check
maygc.ql                    may-gc inference helper
test/                       fixtures
```
