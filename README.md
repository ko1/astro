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
sample/                 Sample languages — see § Samples below
docs/                   Design notes and papers
```

## Samples

ASTro samples deliberately span a wide range of language families so the framework gets exercised against very different value representations, control-flow shapes, and runtime services. The current set covers a tiny calculator (`calc`); three Ruby subsets at different scales (`naruby`, `abruby`, `koruby`); two other dynamic scripting languages (`luastro` for Lua 5.4, `jstro` for an ES2023 subset of JavaScript); three academic / functional languages (`ascheme` for R5RS Scheme, `astocaml` for an OCaml subset, `asom` for the SOM Smalltalk dialect); two statically-typed imperative languages (`pascalast` for Pascal, `castro` for a C subset); and two specialized non-source-language samples (`wastro` for WebAssembly 1.0, `astrogre` for a Ruby-style regex engine).

All samples share a uniform layout (`node.def`, `Makefile`, optional language-specific ASTroGen extension, per-sample `docs/`). Each sample's own README has the full language scope, build / run, benchmarks, and design notes — the entries below are one-liners.

- [`sample/calc/`](./sample/calc/) — Toy calculator REPL. The smallest end-to-end ASTroGen example.
- [`sample/naruby/`](./sample/naruby/) — *Not a Ruby*: minimal integer-only Ruby subset (~21 nodes). Used in the original ASTro paper to evaluate all four execution modes including JIT.
- [`sample/abruby/`](./sample/abruby/) — *a bit Ruby*: larger Ruby subset built as a CRuby C extension. Reuses CRuby's `VALUE`, Prism parser, and numeric stack (Bignum / Float / Rational / Complex).
- [`sample/koruby/`](./sample/koruby/) — *kind of Ruby*: standalone (non-CRuby) Ruby with Boehm GC + GMP + Prism + ucontext `Fiber`. **Runs optcarrot end-to-end**; AOT beats CRuby (no JIT) on `fib` by ~3.6×.
- [`sample/luastro/`](./sample/luastro/) — Lua 5.4. Tagged 8-byte `LuaValue`, full pattern matcher, weak tables, `__gc` finalizers, ucontext coroutines, mark-sweep GC.
- [`sample/jstro/`](./sample/jstro/) — JavaScript (broad ES2023 subset). V8-style hidden-class objects with shape-transition ICs, monomorphic call ICs, longjmp `throw`, safepoint-driven mark-sweep GC, AOT/PG specialization via `astro_code_store` (geo-mean ~2× over the plain interpreter). ~7 K hand-written lines.
- [`sample/ascheme/`](./sample/ascheme/) — R5RS Scheme. Full numeric tower (fixnum / bignum / rational / flonum / complex via GMP), tail calls, `call/cc`, multiple values, ports. Passes 179/179 of chibi's `r5rs-tests.scm`.
- [`sample/astocaml/`](./sample/astocaml/) — OCaml subset (~80 nodes). Full ADTs, exceptions, labeled / optional args, real functor instantiation, single-inheritance classes, HM-lite type inference with let-polymorphism, TCO, gref + method-send inline caches, closure-leaf alloca frames. 35/35 tests; fib(35) in 0.57 s (4.6× over baseline).
- [`sample/asom/`](./sample/asom/) — Smalltalk dialect ([SOM](https://som-st.github.io/)). Type-specialized sends, control-flow inlining, Boehm GC + GMP. Passes the full SOM TestSuite (221/221).
- [`sample/pascalast/`](./sample/pascalast/) — Pascal subset (~190 nodes), ISO 7185 + Free Pascal–style OO. Variant records, sets, `with`, dynamic arrays (`array of T`), subrange range-checking, `virtual` / `override` / `inherited` / `abstract` / `class procedure`, properties, `is` / `as`, catchable `try/except/finally`, hand-written parser. 45/45 tests, AOT 2-25× over interp.
- [`sample/castro/`](./sample/castro/) — C subset. tree-sitter-c front-end, slot-based 8-byte VALUE, structs / function pointers / `printf`, `gcc -E` preprocessing. AOT beats `gcc -O0` on tight loops.
- [`sample/wastro/`](./sample/wastro/) — WebAssembly 1.0 (MVP) interpreter (~210 nodes). Reads both `.wat` and `.wasm`, runs the wasm spec-test `.wast` harness.
- [`sample/astrogre/`](./sample/astrogre/) — Ruby-style regex engine (~22 match nodes — the matcher itself is an AST) plus a grep CLI. Switchable at runtime between the astrogre backend and Onigmo. The for-each-start-position search loop is itself a node, so AOT specialization fuses the loop + inlined regex chain into one SD function (7.2× over interp on long-buffer literal search).

## References

- VMIL 2025 — *ASTro: An AST-Based Reusable Optimization Framework*. [ACM DL](https://dl.acm.org/doi/10.1145/3759548.3763371)
- PPL 2026 — *ASTro による JIT コンパイラの試作*. [program](https://jssst-ppl.org/workshop/2026/program.html)
