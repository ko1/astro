# AnCaml performance

Run with `make bench` (`ruby benchmark/run_bench.rb`).  Each program is run
four ways and outputs are verified equal before timing.  All three of
*interp* / *AOT* / *ocaml(bytecode)* are interpreters; *ocamlopt* is the
native OCaml compiler, included only as an absolute reference.

## Numbers

Machine: this dev box, `gcc -O2`, libgc, OCaml 4.14.1.  Programs are sized for
~0.5 s in the interpreter; all are tree-recursive (`fib`/`ack`) or a float
tree-recursion (`ffib`, i.e. `fib` over boxed floats with `+.`).

| program | interp (s) | AOT (s) | ocaml bc (s) | ocamlopt (s) | AOT vs interp |
|---|---|---|---|---|---|
| `fib 35`  | 0.468 | **0.151** | 0.398 | 0.042 | 3.1× |
| `ffib 34` | 0.680 | 0.516 | 0.176 | 0.028 | 1.3× |
| `ack 3 9` | 0.459 | 0.274 | 0.198 | 0.011 | 1.7× |

On the integer workloads the AOT build is the fastest interpreter here — `fib`
AOT (0.151 s) beats OCaml's bytecode (0.398 s); only native `ocamlopt` is
ahead.  The float workload gains least (see the profile below).

## What made the difference

Two changes moved these numbers a lot versus the first cut:

- **Leaf-frame `alloca`.** A function whose body creates no closures (`is_leaf`)
  has its parameter frame put on the C stack in the calling `node_app*`
  (`APP_LEAF` in `node.def`) instead of `GC_MALLOC`-ing it.  `fib`/`ack`/`ffib`
  are all leaves, so the per-call heap allocation disappears — this roughly
  halved interp time and tripled AOT time on `fib`.
- **Tail-call trampoline.** Tail calls no longer recurse on the C stack
  (`ac_apply`'s loop), so the win above doesn't trade away unbounded loops.

## Profile (real `perf record`)

Flat self-time of the **interpreter** on `ffib 34` (the float workload, where
the most overhead remains):

```
12.6%  DISPATCH_node_app1     (call boundary: arg eval + leaf-alloca + dispatch)
 9.3%  GC_malloc_kind  ┐
 1.7%  GC_malloc       ┘     float boxing (one alloc per `+.`) + the closure
 7.3%  DISPATCH_node_sub
 6.2%  DISPATCH_node_if
 5.5%  DISPATCH_node_le
 5.0%  DISPATCH_node_fadd
 4.8%  DISPATCH_node_lref
 2.7%  ac_make_float / 2.2% ac_get_float
 ...
```

Reading it:

- **AOT removes the per-node `DISPATCH_*` rows.** `astro_cs_compile` folds an
  entire function body into one specialized dispatcher, so the `node_sub` /
  `node_if` / `node_le` / `node_lref` dispatch overhead (≈24% here) collapses
  into straight-line code — that is the AOT speedup.
- **What AOT cannot remove is the ~11% GC**: each `+.` boxes a fresh float
  (`ac_make_float`), and the specializer does not unbox.  This is exactly why
  `ffib` (float) gains 1.3× from AOT while `fib` (int, no boxing) gains 3.1×.
- The generic arithmetic/compare nodes already carry **no type-error checks**
  (the HM checker guarantees operand types), so unlike `astocaml` there is no
  separate "`_int` specialization" win to be had on `+`/`-`/`if` — the tags are
  already trusted.  `node_le` keeps one int-fast-path branch; `node_eq` is the
  only structural one.

## Remaining headroom

- **Unboxed floats** (the biggest remaining `ffib` cost) would need a value
  representation change (e.g. NaN-boxing or an int/float-split) — out of scope
  for the current tagged model.
- **An application inline cache** (cache the resolved closure/body per call
  site) would trim the `node_app1` type/arity check; small, and it needs a
  `@ref` operand (custom `*_gen.rb`).  See [todo.md](todo.md).
