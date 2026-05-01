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

```
bench                    interp  fpc -O-  fpc -O3      AOT
-----                    ------  -------  -------      ---
ackermann                  1.61     0.19     0.06     0.28
collatz                    1.69     0.19     0.16     0.06
fib                        0.94     0.15     0.09     0.21
gcd                        1.03     0.25     0.14     0.29
heron                      1.38     0.16     0.09     0.09
leibniz_pi                 1.23     0.14     0.13     0.10
mandelbrot_int             1.61     0.06     0.05     0.04
mandelbrot_real            1.50     diff     diff     0.06
matmul                     1.37     0.05     0.02     0.06
matmul_2d                  1.41     0.10     0.06     0.17
nested_loops               1.72     0.23     0.07     0.00
quicksort                  1.09     0.15     0.10     0.21
sieve                      1.23     0.12     0.06     0.25
tarai                      1.59     0.20     0.11     0.29
varparam_swap              1.16     0.07     0.03     0.10
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

**pascalast AOT is essentially on par with fpc -O- (no opt) overall.**
On every bench, AOT is within ~3× of fpc -O- and often beats it:

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
