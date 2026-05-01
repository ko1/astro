# perf.md — performance log

A list of the deliberate performance choices that landed, plus the
ones we tried (or considered and chose not to ship) and why.  Numbers
are wall time at `-O3` on the development machine; absolute values
vary, but the *ratios* against each baseline are reproducible.

## Worked

### AOT specialization (round 3, still in)

`pascalast -c FILE.pas` parses the program, runs the
ASTroGen-generated `SPECIALIZE` pass on every procedure body and the
main body, and dumps a self-contained translation unit
(`node_specialized.c`) with one `SD_<hash>` function per unique AST
sub-tree.  The next build picks up that file via `#include` in
`node.c`; at run time `OPTIMIZE` looks up each freshly allocated
node's hash in `sc_repo` and swaps the dispatcher to the specialized
one before evaluation begins.

| benchmark              | interp   | AOT       | speedup  |
|---|---|---|---|
| `fib.pas`              | 0.89 s   | 0.40 s    | 2.2 ×    |
| `tarai.pas`            | 1.58 s   | ~0.8 s    | ~2 ×     |
| `ackermann.pas`        | 1.36 s   | ~0.6 s    | ~2 ×     |
| `collatz.pas`          | 1.24 s   | ~0.05 s   | ~25 ×    |

Tight numeric loops collapse spectacularly because gcc constant-folds
across the SD chain.  Recursive call-heavy benchmarks plateau at ~2 ×
— the remaining cost is the proc-call indirection.

### Specialised for-loop nodes (round 1)

Four variants — `node_lfor_to`, `node_lfor_downto`, `node_gfor_to`,
`node_gfor_downto` — pick the right loop-var slot at parse time.  At
`-O3` the loop variable register-promotes inside the dispatcher.

### Static type tracking, type-erased values (round 2)

The parser carries `TE { NODE *n; int t; }` through every expression
rule.  Operator nodes are picked by static type (`node_add` vs
`node_radd`); int → real promotion is handled by an `node_i2r` cast
inserted at the parse-time mismatch.  No runtime tag check, no
boxing.  Real arithmetic is one union pun per op.

### `case` → if-else lowering (round 2)

`case x of …` becomes a chain of `if`-`else if` nodes with a fresh
hidden temp slot for the controlling expression.  Single evaluation
of `x`, branch prediction works on `node_if`, no extra dispatch.

### Address-of nodes for var parameters (round 2)

`var` parameters pass the storage's int64-cast address through the
same call channel as a regular value.  GCC inlines the
pointer-arithmetic in `node_addr_aref` and the deref in
`node_var_lref` at -O3.

### Pass-through of already-indirect var-params (round 2)

Forwarding a `var x: integer` to another `var` parameter reuses the
existing pointer — no double indirection, no fresh address-take.

### Inline cache for proc calls (round 4)

`@ref struct pcall_cache { body; nslots; return_slot; lexical_depth;
is_function; }` is embedded in each `node_pcall_K`.  First call
populates from `c->procs[pidx]`; subsequent calls skip the
proc-table lookup.

Saves one indirection per call.  Measured fib +7 % vs the
non-cached baseline; AOT fib improves a further ~12 %.  This is
*not* enough to inline the callee body across the SD boundary —
that's the next round's job (bake `body` as a NODE\* operand visible
to SPECIALIZE).

### Display vector for nested-proc access (round 4)

Reading a non-local in a nested proc is one indirect index
`c->stack[c->display[depth] + idx]` — same cost as a local read after
gcc loads `display[depth]` into a register.  Saving / restoring
`display[depth]` around each call costs two integer stores.

### libgc-backed heap (round 4)

Strings, file objects, pointer targets, and readln buffers go
through libgc.  No measured perf delta on the bench suite (allocation
volumes are modest); the win is operational — programs no longer leak
on every string concat.  Preserves the GC-as-default policy that
matches asom / ascheme.

## Considered but didn't ship

### Stack-allocated frames via `alloca`

Replacing the flat `c->stack[fp..]` with `alloca`-allocated per-call
frames is a real win in other ASTro samples (naruby, asom).  We
passed for now because:

- `var`-parameter addresses live across the call boundary, so
  alloca-frame lifetimes need careful escape analysis.
- The current flat-stack form is fast enough that the relative cost
  shifts after AOT.

Revisit once the per-call indirection is folded.

### NaN-boxing or tagged values

Out of scope per project policy.  The current type-erased int64
with parser-side typing avoids the issue entirely.

### Inline-cache that bakes the body NODE\*

The `pcall_cache` we shipped saves the proc-table lookup but not the
indirect call to `body->head.dispatcher`.  For AOT to inline the
callee body into the caller's SD, the body would need to be a NODE\*
operand of the call (visible to `SPECIALIZE_node_pcall_K`).
Conceptually doable; needs a post-parse fixup pass to populate the
operand for forward-declared procs, plus a node-DEF tweak.  Best
single remaining performance lever.

### Perfect-hash for keywords

Negligible at our source sizes.  Worth doing only when we ship a
multi-kLOC bench.

### Two-cell VALUE (separate int and double slots)

Doubles `c->stack`'s memory pressure.  GCC handles the union pun
cleanly.  Not worth the ABI change.

### Hand-tuned arithmetic with `__builtin_*_overflow`

Pascal's `integer` is bounded; we let it wrap.  Overflow trapping
would be a debugging mode, not a performance one.

## Round-trip

```sh
make aot-bench BENCH=fib   # builds, specializes, rebuilds, times both
```

The make rule:
1. Build pascalast (interpreter only).
2. `./pascalast -c bench/fib.pas` — emit `node_specialized.c`.
3. Rebuild pascalast — now linked against the specialized SDs.
4. Run with and without `--no-compile`, print wall time.

## What we learned (round 4)

- The display-vector approach for nested procs is essentially free
  at run time — the cost is concentrated in the call save / restore,
  not in variable access.
- `setjmp` and force-inlined EVAL bodies don't mix.  Pascal's
  `try/except/finally` had to live in a regular helper in main.c.
  Same constraint will apply to any future feature that needs
  setjmp/longjmp (longjmp-based goto, generators, …).
- Records-of-records flattens cleanly to a chain of static offset
  additions at parse time, which means there's zero runtime cost
  vs a flat record.  Keep this insight when designing
  array-of-record / record-of-array.
- Constructors got cleaner once we treated "return Self" as an
  implicit body prelude.  No special call-site protocol — every
  constructor is just a function whose return slot starts as the new
  object.
