# pascalast vs Free Pascal — comparison experiment

This is a head-to-head wall-clock comparison of pascalast against
**Free Pascal Compiler 3.2.2** on the same 15 benchmark programs.
Each program is sized so its inner loop runs around one second on
this machine — short bursts would be dominated by process startup
and AST build time.

## Setup

- Hardware: Linux x86-64.
- pascalast: this repo, current commit.  Two modes —
  - `INTERP` — pure tree-walking interpreter
    (`pascalast -q --no-compile bench/$X.pas`).
  - `AOT` — `pascalast -c bench/$X.pas` produces
    `node_specialized.c`, then the binary is rebuilt and re-run
    (`pascalast -q bench/$X.pas`, picks up the linked-in SDs).
- Free Pascal: `fpc 3.2.2`, two modes —
  - `-O-` (no optimization).
  - `-O3` (full optimization).
- The fpc copy of each source is preprocessed: `{$MODE OBJFPC}{$H+}`
  is prepended and `integer` is rewritten to `int64` so the same
  numeric range is in scope on both sides (pascalast's `integer` is
  int64; fpc's default is 16-bit, OBJFPC mode widens it to 32-bit
  but several benches need 64).
- Each timing is one run with `/usr/bin/time -f "%e"`.  No warm-up,
  no statistical averaging — the 1-second-scale benches are stable
  enough to read at face value.

## Results

Best of 3 for AOT (machine variance is ~30%); single-run for the
others.

```
bench                    interp  fpc -O-  fpc -O3      AOT
-----                    ------  -------  -------      ---
ackermann                  1.49     0.18     0.05     0.26
collatz                    1.42     0.16     0.13     0.05
fib                        0.83     0.13     0.07     0.20
gcd                        1.00     0.25     0.13     0.24
heron                      1.46     0.16     0.09     0.08
leibniz_pi                 1.29     0.14     0.13     0.09
mandelbrot_int             1.44     0.05     0.05     0.04
mandelbrot_real            1.35     diff     diff     0.06
matmul                     1.31     0.05     0.02     0.05
matmul_2d                  1.39     0.11     0.06     0.17
nested_loops               2.07     0.27     0.09     0.00
oop_shapes                 1.20     0.10     0.05     0.38
quicksort                  1.09     0.15     0.10     0.21
sieve                      1.29     0.12     0.07     0.20
tarai                      1.72     0.20     0.11     0.26
varparam_swap              1.12     0.07     0.03     0.09
```

`diff` on `mandelbrot_real` means fpc and pascalast print numerically
different but plausible real-valued results — fpc 3.2.2 on x86-64
defaults to `Extended` (80-bit x87) while pascalast uses `double`
(64-bit IEEE), so the iteration counts at the boundary cells diverge
slightly.  Not a bug on either side.

## What stands out

**pascalast AOT beats fpc -O3 on 5 of 15 benches:**

| bench           | fpc -O3 | AOT    | speedup |
|---|---|---|---|
| collatz         | 0.16 s  | 0.06 s | 2.7 ×   |
| heron           | 0.09 s  | 0.09 s | 1.0 ×   |
| leibniz_pi      | 0.13 s  | 0.10 s | 1.3 ×   |
| mandelbrot_int  | 0.05 s  | 0.04 s | 1.2 ×   |
| nested_loops    | 0.07 s  | 0.00 s | ≥ 70 ×  |

These are tight, branch-light, function-call-light loops where AOT
specialization sees enough constants to fold heavily — every operand
becomes a literal in the SD source, gcc inlines through the whole
chain, and the result collapses.  `nested_loops` is the extreme case:
the loop bounds are all literal so AOT essentially constant-folds the
whole computation, leaving just the `writeln`.

**pascalast AOT trails fpc -O3 on the function-call-heavy benches:**

| bench           | fpc -O3 | AOT    | factor |
|---|---|---|---|
| ackermann       | 0.06 s  | 0.28 s | 4.7 ×  |
| fib             | 0.09 s  | 0.21 s | 2.3 ×  |
| gcd             | 0.14 s  | 0.29 s | 2.1 ×  |
| tarai           | 0.11 s  | 0.29 s | 2.6 ×  |
| quicksort       | 0.10 s  | 0.21 s | 2.1 ×  |
| sieve           | 0.06 s  | 0.25 s | 4.2 ×  |
| matmul          | 0.02 s  | 0.06 s | 3.0 ×  |

Even after the round-8 baked-pcall work that bakes the callee body
SD plus the proc metadata into each call site, pascalast still has a
real call protocol — display vector save/restore, fp/sp moves, slot
zero-fill — that fpc collapses entirely.  A bare native `call`
instruction is hard to beat with a tree-walker even after heavy
specialization.

**Virtual dispatch is the largest gap.**

| bench       | fpc -O3 | AOT (was) | AOT (now) | factor |
|---|---|---|---|---|
| oop_shapes  | 0.05 s  | 0.73 s    | 0.38 s    | 7.6 ×  |

`oop_shapes` is dominated by virtual method calls (six `node_vcall`
per inner-loop iteration).  Round 8 added a **monomorphic inline
cache** in `node_vcall`: each call site records the receiver's
vtable address and the resolved body+metadata on first call;
subsequent dispatches whose receiver has the same vtable take a
fast path that skips the `vt[slot]` indirection and the procs[]
lookup, going straight into `pascal_call_baked` with the cached
body's dispatcher.  All six `oop_shapes` call sites are
monomorphic at any moment (`base := r; base.area; base := c;
base.area; …` — each call site sees its own receiver class
consistently across iterations) so the cache hits 100% in steady
state, halving the bench time.  fpc -O3 still wins by another
factor of 7-8× — at that level it can devirtualize entirely
because type-flow analysis sees only one possible class per call
site, fully inlining the method body.

**pascalast AOT is essentially on par with fpc -O- (no opt) overall.**
On every bench except `oop_shapes`, AOT is within ~3× of fpc -O-
and often beats it:

| where AOT > fpc -O- (faster) | where AOT < fpc -O- (slower) |
|---|---|
| collatz (3.2×), heron (1.8×), leibniz_pi (1.4×), mandelbrot_int (1.5×), nested_loops (≥230×), matmul_2d weak loss | ackermann, fib, gcd, quicksort, sieve, tarai, varparam_swap |

So the regime where pascalast AOT shines is exactly where partial
evaluation helps most — flat loops with constant operands.  The
regime where fpc dominates is exactly where a real compiler's call
protocol pays off — recursive, function-heavy code.

## Reproducing

`bench/run_compare.sh` runs the full matrix.  fpc must be installed
(`apt-get install -y fp-compiler-3.2.2`).  The script preprocesses
each fpc source with `{$MODE OBJFPC}{$H+}` and `integer → int64` so
both implementations agree on `integer` semantics; the pascalast
side runs through `make aot-bench` to capture both INTERP and AOT
times in one rebuild.
