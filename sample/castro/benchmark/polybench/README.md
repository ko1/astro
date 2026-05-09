# PolyBench/C — castro adaptation

PolyBench/C 4.2.1 (Pouchet et al.) is the standard numerical kernel
suite used in compiler / autotuning research.  This directory
contains:

- `fetch.sh` — pull the upstream source into `upstream/` (one-time).
- `polybench_stub.h` — castro-friendly replacement for the upstream
  `polybench.h` (no libc-typedef-heavy includes; just the structural
  macros each kernel uses).  Currently unused by the in-tree kernels
  below, kept for future direct-from-upstream compile.
- `kernels/` — 12 in-tree adapted kernels (gemm, syrk, 2mm, atax,
  bicg, mvt, gesummv, jacobi-1d, jacobi-2d, seidel-2d, lu,
  floyd-warshall).  Each is a self-contained `.c` derived from the
  upstream kernel with arrays moved to static globals, polybench
  timing infrastructure stripped, and a `main()` wrapper that runs
  init → kernel × REPS → checksum-return.  Workload sizes tuned so
  gcc -O3 lands at ≥100 ms (= measurement noise floor).
- `run.sh` — bench harness: per kernel, builds gcc -O0 / gcc -O3
  references, AOT-compiles castro into `bin/<kernel>_code_store/`,
  then times `RUNS` iterations of each (default 7) and prints a
  median table.

## Setup

```sh
make bench-polybench   # from sample/castro/
```

(or to fetch the upstream source for reference:)

```sh
bash benchmark/polybench/fetch.sh
```

## Why not just compile the upstream kernels?

Two friction points with the unmodified upstream:

1. `polybench.h` pulls in `<time.h>` / `<sys/time.h>` / `<sched.h>`
   for its instrumentation API — those headers transitively bring
   `__builtin_va_list` and a forest of glibc internal typedefs that
   castro's parser stumbles on.  We replace `polybench.h` with a
   castro-friendly stub; the stub keeps just the structural macros
   (`POLYBENCH_2D` etc.) and stubs the timing entry points.
2. The upstream `main()` calls `print_array(…)` which uses
   `fprintf(stderr, …)` — castro doesn't implement `fprintf`, only
   `printf` to stdout.  We replace each kernel's `main` with a tiny
   `init / kernel × REPS / checksum-return` wrapper.

## Result format

`run.sh` prints a table like:

```
bench                 castro_ms      O0_ms      O3_ms      vs-O3 castro_rc    O3_rc
2mm                        1120       5690        580      1.93x        0        0
atax                        370       1460        270      1.37x       41       41
bicg                       1020       1010        210      4.86x      163      163
floyd-warshall              300        760        180      1.67x      224      224
gemm                        320       4800        280      1.14x        0        0
gesummv                     790        790        170      4.65x      239      239
jacobi-1d                   170        520         80      2.12x      193      193
jacobi-2d                   320       2410        280      1.14x       32       32
lu                         2230       3320        570      3.91x      247      247
mvt                         450       1460        240      1.88x      128      128
seidel-2d                  1270       1650       1170      1.09x      147      147
syrk                        350       2650        290      1.21x        0        0
```

`castro_rc` and `O3_rc` are the per-kernel checksum return codes; they
should match.

## Adding a kernel

1. Pick a kernel under `upstream/`.
2. Copy its body into a new `kernels/<NAME>.c`.
3. Replace `POLYBENCH_2D(A,M,N,m,n)` style array decls (in the file's
   own static-global declarations) with `DATA_TYPE A[M][N];`.
4. Replace function-arg `POLYBENCH_2D(A,...)` with the matching
   array signature (e.g. `DATA_TYPE A[M][N]`), or — preferred — drop
   the array params altogether and read the static globals directly.
5. Strip the upstream `main()` and replace with a tiny wrapper that
   calls `init_array()` once and `kernel_<NAME>()` REPS times, then
   computes a checksum (`s += A[i][j]`) and returns it `& 0xff`.
6. Tune `REPS` (or array dimensions) so `gcc -O3` lands at ≥100 ms.

The 12 kernels in `kernels/` follow this template — copy one as a
starting point.

## License

The kernels are derived from PolyBench/C, which is dual-licensed under
GPL-3+ and the Polylib license.  The adapted versions follow the same
license as upstream.
