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
docs/                   Design notes and papers
```

## Samples

- [`sample/calc/`](./sample/calc/) — A toy "Calc" language with three node types (`num`, `add`, `mul`, ...). The smallest end-to-end example for understanding ASTroGen.
- [`sample/naruby/`](./sample/naruby/) — "Not a Ruby": a minimal Ruby subset (~21 nodes, integer-only) used for evaluating all four execution modes including JIT.
- [`sample/abruby/`](./sample/abruby/) — "a bit Ruby": a larger Ruby subset implemented as a CRuby C extension. Supports classes, blocks, exceptions, strings/arrays/hashes, and Bignum/Float/Rational/Complex via the CRuby numerics. See [`sample/abruby/docs/abruby_spec.md`](./sample/abruby/docs/abruby_spec.md) for the language spec.
- [`sample/wastro/`](./sample/wastro/) — WebAssembly on ASTro: a wasm 1.0 (MVP) interpreter (~210 nodes, four numeric types, full memory / table / call_indirect / spec-test harness). Reads both `.wat` and `.wasm`. AOT-cached runs are competitive with Cranelift JIT (`wasmtime`) across the bundled benchmark suite (`bench.rb`). See [`sample/wastro/docs/runtime.md`](./sample/wastro/docs/runtime.md) for the runtime architecture.
- [`sample/asom/`](./sample/asom/) — ASTro SOM: a port of the [Simple Object Machine](https://som-st.github.io/) Smalltalk dialect. Bundles the upstream SOM-st/SOM repository as a submodule for the standard library, TestSuite, and AreWeFastYet benchmark suite. Hand-written lexer/parser, tagged-int object model, per-class metaclasses, lexical block scope with `escapedBlock:` recovery, `doesNotUnderstand:`, ~190 C-level primitives, sends up to 8 args, class-side fields, `#(...)` literal arrays. Passes **211/221 (95.5%) assertions** of the SOM-st/SOM TestSuite (19/24 files clean). On the AreWeFastYet benchmark suite it is roughly on par with **SOM++** (the optimised C++ reference), 14–250× faster than **CSOM**, and 30–250× faster than **PySOM** (plain CPython).
- [`sample/ascheme/`](./sample/ascheme/) — **R5RS Scheme on ASTro**.  40 node kinds covering tail-call trampoline, escape `call/cc`, `delay`/`force`, multiple values, file ports, the full R5RS numeric tower (fixnum / bignum / rational / flonum / complex via GMP), and Ruby-style inline flonum encoding.  Specialized nodes for the common arith / predicate / vector ops with R5RS-faithful runtime rebinding detection (`arith_cache @ref` snapshot of `PRIM_<op>_VAL` at install_prims time).  Three execution modes — interpreter, AOT, and abruby-style `--pg-compile` PGO — wired into a `make compare` table that pits ascheme against chibi-scheme 0.12 and guile 3.0 (JIT).  AOT/PGO-cached beats chibi on 5/7 micro-benchmarks and beats guile-JIT on 7/7.  Passes 16 self-tests + 179/179 of chibi's `tests/r5rs-tests.scm` (machine-converted).  See [`sample/ascheme/docs/runtime.md`](./sample/ascheme/docs/runtime.md) for the runtime architecture.
- [`sample/luastro/`](./sample/luastro/) — **Lua 5.4 on ASTro**.  Hand-written tokenizer + recursive-descent parser, ~60 node kinds covering full Lua 5.4 syntax (statements, expressions, multi-assign, table constructors, generic-for, `goto`/labels, varargs).  CRuby-style 8-byte tagged `LuaValue` (`uint64_t`): low 3 bits = tag (`000` ptr / `001` 63-bit fixnum / `010` reserved for inline flonum), heap-object subtype read from a `GCHead.type` byte at offset 0 of the object.  Full standard library — `math`, `string` (including the complete Lua pattern matcher: `%a`/`%d`/sets/`%b()`/`%f[]`/captures/`gsub`), `table`, `io.write`, `os`, `coroutine` (ucontext-backed).  Stop-the-world mark-sweep GC with weak tables (`__mode`) and `__gc` finalizers.  AOT specialization via the shared code store (`-c` / `--pg-compile`).  On AOT-cached integer-dominant benchmarks luastro beats Lua 5.4 (`loop` 13×, `factorial` 2.3×, `fib` 1.6×); float-dominant benchmarks (`mandelbrot`, `nbody`) currently lag because every double heap-allocates — inline flonum fixes that and is the next item.  See [`sample/luastro/docs/runtime.md`](./sample/luastro/docs/runtime.md) for the runtime architecture and [`sample/luastro/docs/done.md`](./sample/luastro/docs/done.md) / [`todo.md`](./sample/luastro/docs/todo.md) for the implemented-feature inventory and gaps.

## References

- VMIL 2025 — *ASTro: An AST-Based Reusable Optimization Framework*. [ACM DL](https://dl.acm.org/doi/10.1145/3759548.3763371)
- PPL 2026 — *ASTro による JIT コンパイラの試作*. [program](https://jssst-ppl.org/workshop/2026/program.html)
