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

ASTro samples span a wide range of language families to exercise the framework against very different value representations, control-flow shapes, and runtime services. All share a uniform layout (`node.def`, `Makefile`, optional ASTroGen extension, per-sample `docs/`). Each sample's own README has the full language scope, build / run, benchmarks, and design notes; [`docs/samples.md`](./docs/samples.md) is the cross-sample analysis. The entries below are one-liners with the most distinctive flagship result.

**Tutorial.**
- [`calc`](./sample/calc/) — **toy 6-node calculator REPL** (`num` + `+`/`-`/`*`/`/`/`%`), the smallest end-to-end ASTroGen example.

**Ruby family.**
- [`naruby`](./sample/naruby/) — ***not a Ruby***: integer-only 21-node Ruby subset.
  The original ASTro paper's vehicle for evaluating all four execution modes, JIT included.
- [`abruby`](./sample/abruby/) — ***a bit Ruby***: larger Ruby subset as a CRuby C extension.
  Reuses CRuby's `VALUE`, Prism parser, and the full Bignum / Float / Rational / Complex numeric stack.
- [`koruby`](./sample/koruby/) — ***kind of Ruby***: standalone (non-CRuby) Ruby with Boehm GC + GMP + Prism + ucontext `Fiber`.
  **Runs optcarrot end-to-end**; AOT beats CRuby (no JIT) on `fib` by ~3.6×.

**Other dynamic scripting.**
- [`luastro`](./sample/luastro/) — **Lua 5.4** with tagged 8-byte `LuaValue`.
  Full pattern matcher, weak tables, `__gc` finalizers, ucontext coroutines, mark-sweep GC.
- [`jstro`](./sample/jstro/) — **JavaScript** (broad ES2023 subset) with V8-style hidden-class objects + shape-transition ICs.
  Profile-driven kind swap, longjmp `throw`, mark-sweep GC; beats node v18 by 2-50× on several benches.
- [`pystro`](./sample/pystro/) — **Python 3** subset (~80 nodes): GMP bignum + classes + dunder + `try`/`except` + generators + comprehensions + f-strings + ~37 builtins.
  **Beats CPython 3.12 on 8 / 9 benches** at ~1 s scale (`while_loop` 18×, `for_range` 11×).

**Functional / academic.**
- [`ascheme`](./sample/ascheme/) — **R5RS Scheme** with the full numeric tower (fixnum / bignum / rational / flonum / complex via GMP), `call/cc`, multiple values, ports.
  Passes 179/179 of chibi's `r5rs-tests.scm`.
- [`astocaml`](./sample/astocaml/) — **OCaml** subset (~80 nodes) with HM-lite type inference, ADTs, exceptions, single-inheritance classes, real functor instantiation, TCO.
  35/35 tests; with `-c`, fib / ack / tak beat `ocamlc` bytecode and `ocaml` toplevel.
- [`asom`](./sample/asom/) — **[SOM](https://som-st.github.io/)** Smalltalk dialect with type-specialized sends, control-flow inlining, Boehm GC + GMP.
  Passes the full SOM TestSuite (221/221).

**Statically-typed imperative.**
- [`pascalast`](./sample/pascalast/) — **Pascal** subset (~190 nodes), ISO 7185 + Free Pascal–style OO: variant records, sets, `with`, `array of T`, `virtual` / `override`, `try/except`, properties.
  **Wins outright on tight constant-folding loops vs `fpc -O3`** (collatz 0.4× / leibniz 0.6× / mandelbrot 0.8×).
- [`castro`](./sample/castro/) — **C** subset with tree-sitter-c front-end, 8-byte slot VALUE, structs / function pointers / `printf`, `gcc -E` preprocessing.
  AOT beats `gcc -O0` on tight loops.

**Stack-based.**
- [`aforth`](./sample/aforth/) — **Forth** subset where every word is an AST NODE (no traditional threaded code); calls indirect through a `word_id` → `NODE *` table.
  Wins 8/9 against gforth 0.7.3 (gcd 13.9×, factorial 8.1×, collatz 7.2×).

**Data / vector.**
- [`astr`](./sample/astr/) — **R** subset (~30 nodes) with tagged 64-bit `VALUE`, libgc, vector/scalar broadcast, `c()` / `paste` / `substr` / `1:n` ranges.
  AOT 4.1× on fib(36), 3.8× on ack(3,9).

**Non-source / DSL.**
- [`wastro`](./sample/wastro/) — **WebAssembly 1.0** (MVP) interpreter (~210 nodes).
  Reads both `.wat` and `.wasm`, runs the wasm spec-test `.wast` harness.
- [`astrogre`](./sample/astrogre/) — **Ruby-style regex engine** (~22 match nodes — the matcher itself is an AST) with a `grep`-style CLI; switchable between the astrogre backend and Onigmo.
  AOT fuses the for-each-start-position search loop and the regex chain into one SD function (7.2× over interp on long-buffer literal search).
- [`nuq`](./sample/nuq/) — **`jq` clone** (~50 nodes), full jq filter language modulo regex / assignment-style operators: pipe / comma, `if-elif-else`, `try-catch`, `reduce` / `foreach`, `label` / `break`, user `def`s, `@uri` / `@base64` / etc., 70+ builtins.
  338 / 338 tests pass; about half are differential against system `jq`.
- [`arjsv`](./sample/arjsv/) — **JSON Schema validator** (drafts 04 / 06 / 07 / 2020-12, 47 nodes), CRuby C extension with a `json_schemer`-compatible API.
  Each schema lowers to its own SD; property names, regexes, and `$defs` targets live in a Schema-side `consts` array so per-call validation does no allocation.  JSON Schema Test Suite: draft-04 98.8%, draft-06 98.9%, draft-07 95.5%, 2020-12 94.6% (within "no external dependency" scope — IDNA / HTTP `$ref` / dynamic-scope `$dynamicRef` excluded).  Drop-in for `json_schemer` (`valid?` / `validate` / `valid_schema?` / `formats:` / `insert_property_defaults:` 互換).  4–11× faster than `rj_schema` (Rust + RapidJSON via FFI) on the gateway-flow benchmark; 25–180× faster than `json_schemer` (pure Ruby).

## References

- VMIL 2025 — *ASTro: An AST-Based Reusable Optimization Framework*. [ACM DL](https://dl.acm.org/doi/10.1145/3759548.3763371)
- PPL 2026 — *ASTro による JIT コンパイラの試作*. [program](https://jssst-ppl.org/workshop/2026/program.html)
