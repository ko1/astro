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
docs/                   Design notes and papers
```

## Samples

The samples share a uniform layout (`node.def`, `Makefile`, optional language-specific ASTroGen extension, per-sample `docs/`). Each sample's README has the language scope, build / run, benchmarks, and design notes.

- [`sample/calc/`](./sample/calc/) — Toy calculator REPL (6 arithmetic nodes). The smallest end-to-end example for understanding ASTroGen.
- [`sample/naruby/`](./sample/naruby/) — *Not a Ruby*: minimal Ruby subset (~21 nodes, integer-only). Used in the original ASTro paper to evaluate all four execution modes including JIT.
- [`sample/abruby/`](./sample/abruby/) — *a bit Ruby*: larger Ruby subset implemented as a CRuby C extension. Reuses CRuby's `VALUE` / Prism parser / numerics (Bignum, Float, Rational, Complex).
- [`sample/wastro/`](./sample/wastro/) — WebAssembly 1.0 (MVP) interpreter, ~210 nodes. Reads both `.wat` and `.wasm`, runs the wasm spec-test `.wast` harness, AOT-cached comparable to Cranelift on the bundled benchmarks.
- [`sample/asom/`](./sample/asom/) — Smalltalk dialect ([SOM](https://som-st.github.io/)). Bundles SOM-st/SOM as a submodule for stdlib + TestSuite + AreWeFastYet. Passes 216/221 (97.7%) TestSuite assertions; 16 AWFY benchmarks pass. Type-specialized int/array sends, control-flow inlining (`ifTrue:` / `whileTrue:` / `to:do:` etc., stack- and pool-frame variants for N-locals), per-bucket frame pool, double / block bump arenas, parse-time-detected no-NLR methods skipping `setjmp`, plus a fix wave that made AOT actually pay off (deterministic structural hash for string/array literals, body-subtree as direct NODE * operand on every inlined control-flow node, block bodies registered as cs_compile entries so the SD chain doesn't break across asom_block_invoke). Result at sustained workloads (~1 s runs): **asom-aot is faster than SOM++ (`USE_TAGGING` + COPYING GC) on every one of the 12 AreWeFastYet benchmarks** — Sieve **18.2×**, Queens 8.3×, QuickSort / Fannkuch 4×, BubbleSort / Permute / Bounce / Storage 3×, TreeSort / Towers 2.3×, List 2×, Mandelbrot 1.1×.
- [`sample/ascheme/`](./sample/ascheme/) — R5RS Scheme. Full numeric tower (fixnum / bignum / rational / flonum / complex via GMP), tail calls, `call/cc`, multiple values, ports. Passes 179/179 of chibi's `r5rs-tests.scm`; AOT-cached beats chibi-scheme on 5/7 micro-benchmarks, guile-3.0 (JIT) on 7/7.
- [`sample/luastro/`](./sample/luastro/) — Lua 5.4. Tagged 8-byte `LuaValue` (63-bit fixnum + inline flonum), full pattern matcher, weak tables, `__gc` finalizers, ucontext coroutines, mark-sweep GC. AOT-cached beats lua5.4 on `loop` (12.7×), `factorial` (2.6×), `fib` (1.9×).
- [`sample/castro/`](./sample/castro/) — C subset. tree-sitter-c front-end + Ruby parse driver, slot-based 8-byte VALUE, structs / function pointers / `printf` / libc-ish builtins, `gcc -E` preprocessing. AOT-cached beats `gcc -O0` on `loop_sum`; passes 30/30 feature tests + 173/220 of c-testsuite.

## References

- VMIL 2025 — *ASTro: An AST-Based Reusable Optimization Framework*. [ACM DL](https://dl.acm.org/doi/10.1145/3759548.3763371)
- PPL 2026 — *ASTro による JIT コンパイラの試作*. [program](https://jssst-ppl.org/workshop/2026/program.html)
