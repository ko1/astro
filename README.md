<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo-dark.svg">
    <img alt="ASTro: AST-based Reusable Optimization Framework" src="docs/logo-light.svg" width="360">
  </picture>
</p>

# ASTro: AST-based Reusable Optimization Framework

> Note: This project is still experimental, and the API is subject to significant changes.

ASTro is an optimization framework based on **Abstract Syntax Trees (ASTs)**.
It provides a reusable infrastructure for generating optimized code fragments through partial evaluation of AST interpreters, emitting C source as the output of specialization and delegating native code generation to a mature C compiler (gcc/clang).

The companion tool **ASTroGen** (`lib/astrogen.rb`) automatically generates an interpreter (evaluator), a dispatcher, allocators, Merkle-tree hash functions, a partial evaluator (specializer), a dumper, and node-replacement helpers from a `node.def` file that defines node types and their behaviors in C code.

## Key ideas

- **Dispatcher / Evaluator separation.** Evaluators (`EVAL_xxx`) hold the user-written semantics; dispatchers (`DISPATCH_xxx`) are thin wrappers that unpack node fields and call the evaluator. Partial evaluation specializes only the dispatchers, so evaluator code is reused unchanged and the C compiler is free to inline the resulting call chain.
- **Merkle-tree hashing of AST nodes.** Each node carries a hash derived from its kind and children, enabling content-addressable sharing of compiled code across processes and machines (functions are named `SD_<hash>`).
- **C source as the IR.** Specialized code is emitted as ordinary C, compiled with the host toolchain, and loaded via shared objects (`dlopen` + `dlsym`). No custom backend.
- **Code Store** (`runtime/astro_code_store.{h,c}`). A small runtime library that manages `<store>/SD_<hash>.{c,o}` files plus an aggregated `all.so`, and swaps a node's dispatcher to the specialized one on `astro_cs_load`.

## Four execution modes

The same `node.def` and the same generated infrastructure support:

1. **Plain interpreter** — pure tree-walking via `DISPATCH` → `EVAL`.
2. **AOT compilation** — specialize the whole AST offline, link, and run.
3. **Profile-guided compilation** — collect profile on the first run, specialize before the second.
4. **JIT compilation** — specialize and load `.so` files at run time. A tiered design (in-process L0 thread, local L1 daemon, remote L2 compile farm) shares compiled code by hash.

See [`docs/idea.md`](./docs/idea.md) for the design rationale and [`docs/usage.md`](./docs/usage.md) for an ASTroGen tutorial.

## Repository layout

```
lib/astrogen.rb         ASTroGen core (Ruby) — generates C from node.def
runtime/                Reusable C runtime (Code Store)
sample/calc/            Minimal calculator (3 nodes)
sample/naruby/          "Not a Ruby" — Ruby subset, JIT enabled
sample/abruby/          "a bit Ruby" — larger Ruby subset, CRuby C extension
sample/wastro/          WebAssembly 1.0 interpreter (.wat + .wasm + .wast)
sample/asom/            ASTro SOM — Smalltalk dialect; SOM-st/SOM as submodule
sample/ascheme/         R5RS Scheme — full numeric tower (GMP), Boehm GC, PGO
sample/luastro/         Lua 5.4 interpreter — tagged 8-byte values, full pattern matcher
sample/castro/          C subset — tree-sitter-c front-end, slot-based VALUE, beats gcc -O0 on tight loops
sample/pascalast/       Pascal subset — typed int/bool/real, var params, 2D arrays, forward, case
docs/                   Design notes and papers
```

## Samples

The samples share a uniform layout (`node.def`, `Makefile`, optional language-specific ASTroGen extension, per-sample `docs/`). Each sample's README has the language scope, build / run, benchmarks, and design notes.

- [`sample/calc/`](./sample/calc/) — Toy calculator REPL (6 arithmetic nodes). The smallest end-to-end example for understanding ASTroGen.
- [`sample/naruby/`](./sample/naruby/) — *Not a Ruby*: minimal Ruby subset (~21 nodes, integer-only). Used in the original ASTro paper to evaluate all four execution modes including JIT.
- [`sample/abruby/`](./sample/abruby/) — *a bit Ruby*: larger Ruby subset implemented as a CRuby C extension. Reuses CRuby's `VALUE` / Prism parser / numerics (Bignum, Float, Rational, Complex).
- [`sample/koruby/`](./sample/koruby/) — *kind of Ruby*: standalone (non-CRuby) Ruby implementation. Boehm GC + GMP Bignum + Prism parser. CRuby-compatible `VALUE` encoding so heap layouts and code can be ported. Closures (yield-shared-fp), classes/modules/include, attr_*, lexical constants captured per method (cref), `case/when`, multi-assign, `super`, `ensure`, optional + rest parameters, `require/require_relative`, basic `Struct`/`File`/`STDOUT`. Beats CRuby (no JIT) on fib by ~1.6× via interp + 3.6× with AOT specialization; ~1.6× off `--yjit` on tight benches. optcarrot loads all 12 files cleanly + reaches Config parsing; full execution still in progress (needs real Regexp + more splat/destructure).
- [`sample/wastro/`](./sample/wastro/) — WebAssembly 1.0 (MVP) interpreter, ~210 nodes. Reads both `.wat` and `.wasm`, runs the wasm spec-test `.wast` harness, AOT-cached comparable to Cranelift on the bundled benchmarks.
- [`sample/asom/`](./sample/asom/) — Smalltalk dialect ([SOM](https://som-st.github.io/)). Type-specialized sends, control-flow inlining, flonum-tagged Double, Boehm GC, GMP Bignum. SOM-st/SOM submodule for stdlib + TestSuite + AreWeFastYet; passes 221/221 TestSuite, IntegerTest 25/25 + DoubleTest 27/27. AOT beats SOM++ on 11/12 AWFY (Sieve **10×**, QuickSort 5.3×, Fannkuch 4.3×); on cold-start wall-clock often beats TruffleSOM (asom-aot 0.65s vs Truffle 1.74s on Sieve). TruffleSOM warm-peak still 4-28× faster — the gap is method-level PE + escape analysis, not interpretation overhead.
- [`sample/ascheme/`](./sample/ascheme/) — R5RS Scheme. Full numeric tower (fixnum / bignum / rational / flonum / complex via GMP), tail calls, `call/cc`, multiple values, ports. Passes 179/179 of chibi's `r5rs-tests.scm`; AOT-cached beats chibi-scheme on 5/7 micro-benchmarks, guile-3.0 (JIT) on 7/7.
- [`sample/luastro/`](./sample/luastro/) — Lua 5.4. Tagged 8-byte `LuaValue` (63-bit fixnum + inline flonum), full pattern matcher, weak tables, `__gc` finalizers, ucontext coroutines, mark-sweep GC. AOT-cached beats lua5.4 on `loop` (12.7×), `factorial` (2.6×), `fib` (1.9×).
- [`sample/jstro/`](./sample/jstro/) — JavaScript (subset of ECMAScript). Tagged 8-byte `JsValue` (CRuby-style fixnum/flonum/heap-pointer), V8-style hidden-class objects with shape-transition ICs, monomorphic call ICs that inline the body, longjmp-based throw, function-declaration hoisting via textual pre-scan. Covers ES2015+ syntax: classes, arrow functions, default/rest/spread, optional chaining (`?.`), nullish coalescing, computed keys, template literals, `Map`/`Set`, iterator protocol via `@@iterator`. Pure tree-walking interpreter (no JIT yet); nbody runs in ~0.32 s — about 16× off node.js (V8) for a code base of ~7 K lines.
- [`sample/castro/`](./sample/castro/) — C subset. tree-sitter-c front-end + Ruby parse driver, slot-based 8-byte VALUE, structs / function pointers / `printf` / libc-ish builtins, `gcc -E` preprocessing. RESULT-state non-local exits (no setjmp), wastro-style caller-allocated stack VLA frame so loop variables register-promote, leaf-helper inlining via NODE-child operand on `node_call_static`. AOT-cached beats `gcc -O0` on `loop_sum` / `nqueens` / `quicksort`, ties `gcc -O1` on `crc32`; passes 30/30 feature tests + 184/220 of c-testsuite.
- [`sample/pascalast/`](./sample/pascalast/) — Pascal subset, ~50 nodes. Hand-written recursive-descent parser (no external dependency), int64 / boolean / real (double bit pattern in int64) with parser-level type tracking, 1D/2D global arrays, `var` parameters (call-by-reference via address-of nodes + indirect lref/lset), `case`, `forward;` for mutual recursion, `write`/`writeln` with width/precision specifiers. Currently a pure tree-walking interpreter — `OPTIMIZE` / `SPECIALIZE` are stubbed; ASTro's code-store is not exercised yet. 19/19 tests pass; 15 sustained-~1 s benchmarks (fib, tarai, ack, sieve, qsort, mandelbrot int + real, matmul flat + 2D, leibniz π, varparam-swap, …).
- [`sample/astocaml/`](./sample/astocaml/) — OCaml subset interpreter, ~50 nodes. Hand-written recursive-descent parser, tagged 8-byte VALUE (62-bit fixnum + heap pointer), boxed float / cons / string / closure / tuple / ref / variant / record / array. Covers Phase 1 + most of Phase 2 of OCaml: `let / let rec / and`, `fun / function`, `match` with nested patterns + `when` guards + `as`, tuples / refs (`ref`/`!`/`:=`) / boxed floats with `+. -. *. /.`, polymorphic structural compare, full algebraic data types (variants + records), exceptions (`try / with / raise`, predefined `Failure` / `Not_found` / `Division_by_zero`), `List.*` / `String.*` / `Array.*` stdlib basics. Phase 3 (modules/functors/objects/GADT) and labeled args are deferred. 23/23 tests pass; 5 sustained-~1 s benchmarks (fib, tak, ack, nqueens, sieve).

## References

- VMIL 2025 — *ASTro: An AST-Based Reusable Optimization Framework*. [ACM DL](https://dl.acm.org/doi/10.1145/3759548.3763371)
- PPL 2026 — *ASTro による JIT コンパイラの試作*. [program](https://jssst-ppl.org/workshop/2026/program.html)
