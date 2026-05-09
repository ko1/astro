# Computer Language Benchmarks Game (CLBG) — non-numeric kernels for castro

PolyBench/C (sibling directory) covers the *numerical* side — pure
linear algebra and stencils, where castro's AOT path lands within
2× of gcc -O3 on most kernels.  These CLBG-derived kernels probe
the *non-numerical* corners: malloc-intensive trees, control-flow
permutations, bit-twiddling hashes, FP simulation.

## Kernels

- `binary-trees.c` — recursive `make_tree` / `check_tree` / `free_tree`.
  Stresses the malloc / free / pointer-chase path.
- `fannkuch-redux.c` — permutation generator + flip count.  Pure int,
  no array allocation, control-flow heavy.
- `md5.c` — RFC 1321 MD5, single block × N reps.  Bit-twiddling
  benchmark (rotates, XOR, AND, OR, table lookups).
- `nbody.c` — 5-body solar-system simulation, FP-arith inner loop.
  Includes a hand-written Newton-Raphson sqrt (since castro doesn't
  link `<math.h>`'s `sqrt`); that's an artefact of the missing builtin,
  not a fair comparison point with gcc.

## Setup

```sh
make bench-clbg   # from sample/castro/
```

(Runs all kernels, median-of-7, prints comparison table vs gcc -O0/-O3.)

## Result format

```
bench                 castro_ms      O0_ms      O3_ms      vs-O3 castro_rc    O3_rc
binary-trees                490        300        210      2.33x      174      174
fannkuch-redux              330        410        210      1.57x       92       92
md5                       10140        320         90    112.67x      178      178
nbody                      5100       1550        560      9.11x      203      203
```

## Caveats specific to non-numeric kernels

**`md5` is brutal** (~110× of gcc -O3): castro's slot model stores
every `unsigned int` in an 8-byte slot, so each `& 0xffffffff` mask
that the C source needs (because castro doesn't truncate on assignment
to a narrower integer type) becomes a real `and` instruction in the
SD chain.  gcc's u32 arithmetic auto-truncates for free.  Bit-heavy
codes pay this tax on every op.

**`nbody` over-counts** the "vs gcc -O3" gap because of a
hand-written 20-iteration Newton-Raphson `my_sqrt` (castro has no
`sqrt` builtin / libm link).  gcc -O3 gets a single `sqrtsd` instruction
per call; castro runs 20 multiplies + adds.  For an apples-to-apples
benchmark, castro would need a `node_call_sqrt` builtin (or runtime
helper that calls libm).  Documented as a future TODO.

**`binary-trees` slower than gcc -O0** because every `malloc(sizeof Tree)`
goes through `node_call_malloc` (= a real function call out of the
SD chain) + the resulting `Tree *` is dereferenced via slot-stride
pointer arithmetic.  gcc inlines malloc / free for small allocations
and dereferences via native pointer ops.

**`fannkuch-redux` is the bright spot**: pure int + array indexing,
castro AOT lands at `1.57×` of gcc -O3 and *beats* gcc -O0.  This is
the same shape as the polybench wins.

## License

Each kernel is derived from the CLBG entry of the same name (see the
[CLBG website](https://benchmarksgame-team.pages.debian.net/benchmarksgame/)).
The CLBG entries are typically MIT-or-equivalent; the adapted versions
here track the same license.

## Adding a kernel

CLBG is a much looser collection than PolyBench (each problem has many
language-specific variants).  When adapting, prefer the simplest C
version and:

1. Strip threads / OpenMP / vector intrinsics.
2. Replace stdin / argv parsing with hardcoded sizes.
3. Replace stdout output (`printf("%s\n", …)`) with a checksum-return
   `int main()` so timing focuses on the kernel.
4. Add explicit `& 0xff` / `& 0xffff` masks where the C source assigns
   to a narrower integer type — castro's slot model doesn't truncate.
