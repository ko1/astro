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

ASTro samples deliberately span a wide range of language families so the framework gets exercised against very different value representations, control-flow shapes, and runtime services. The current set covers a tiny calculator (`calc`); three Ruby subsets at different scales (`naruby`, `abruby`, `koruby`); three other dynamic scripting languages (`luastro` for Lua 5.4, `jstro` for an ES2023 subset of JavaScript, `pystro` for a Python 3 subset); three academic / functional languages (`ascheme` for R5RS Scheme, `astocaml` for an OCaml subset, `asom` for the SOM Smalltalk dialect); two statically-typed imperative languages (`pascalast` for Pascal, `castro` for a C subset); one stack-based concatenative language (`aforth` for a Forth subset); two specialized non-source-language samples (`wastro` for WebAssembly 1.0, `astrogre` for a Ruby-style regex engine); and a JSON query DSL (`nuq` — a jq clone).

All samples share a uniform layout (`node.def`, `Makefile`, optional language-specific ASTroGen extension, per-sample `docs/`). Each sample's own README has the full language scope, build / run, benchmarks, and design notes — the entries below are one-liners.

- [`sample/calc/`](./sample/calc/) — Toy calculator REPL. The smallest end-to-end ASTroGen example.
- [`sample/naruby/`](./sample/naruby/) — *Not a Ruby*: minimal integer-only Ruby subset (~21 nodes). Used in the original ASTro paper to evaluate all four execution modes including JIT.
- [`sample/abruby/`](./sample/abruby/) — *a bit Ruby*: larger Ruby subset built as a CRuby C extension. Reuses CRuby's `VALUE`, Prism parser, and numeric stack (Bignum / Float / Rational / Complex).
- [`sample/koruby/`](./sample/koruby/) — *kind of Ruby*: standalone (non-CRuby) Ruby with Boehm GC + GMP + Prism + ucontext `Fiber`. **Runs optcarrot end-to-end**; AOT beats CRuby (no JIT) on `fib` by ~3.6×.
- [`sample/luastro/`](./sample/luastro/) — Lua 5.4. Tagged 8-byte `LuaValue`, full pattern matcher, weak tables, `__gc` finalizers, ucontext coroutines, mark-sweep GC.
- [`sample/jstro/`](./sample/jstro/) — JavaScript (broad ES2023 subset). V8-style hidden-class objects, shape-transition ICs, longjmp `throw`, mark-sweep GC, profile-driven kind swap, AOT/PG specialization, open-addressing hash Map.  Beats node v18 on `try_catch` (45×), `cold` (53×), `state` (Redux spread, 2.3×), `sieve_big` (40 M, 2.5×); 3-14× behind on TurboFan numeric loops.
- [`sample/pystro/`](./sample/pystro/) — Python 3 subset (~80 nodes).  GMP bignum + inline flonum, classes with `super()` and dunder methods (`__add__` / `__eq__` / `__repr__` / `__getitem__` / etc.), `try`/`except`/`else`/`finally`/`raise`, `lambda` + default args + `*args` + `**kwargs` + keyword-only params, list / tuple / dict / set + slicing + slice assignment, list / dict / set comprehensions, f-strings with format spec (`{x:.2f}`), `for x in iter` / `while` with `else` clauses, `with ... as`, generators (eager `yield`), `nonlocal` closures + decorators, `staticmethod` / `classmethod` / `property`, walrus `:=`, multi-assign, ~37 built-ins.  Inline-cached gref / method-resolve / for-loop targets, `py_apply` inlined into SD bodies, leaf-func alloca frames, dict identity-equal fast path, share-buffer string slices, inline flonum + flonum-flonum arithmetic fast paths.  **Beats CPython 3.12 on 8 / 9 benches** at ~1 s scale: `while_loop` 18×, `for_range` 11×, `list` 4.7×, `fib(35)` 1.9×, `recursive` (tak) 1.6×, `string.split` 1.2×, `mandel` 1.1×, `nqueens` 1.1×; `dict_bench` is 0.94× (CPython's hand-tuned dict).
- [`sample/ascheme/`](./sample/ascheme/) — R5RS Scheme. Full numeric tower (fixnum / bignum / rational / flonum / complex via GMP), tail calls, `call/cc`, multiple values, ports. Passes 179/179 of chibi's `r5rs-tests.scm`.
- [`sample/astocaml/`](./sample/astocaml/) — OCaml subset (~80 nodes). HM-lite type inference, ADTs, exceptions, single-inheritance classes, real functor instantiation, TCO, AOT specialization. 35/35 tests; with `-c`, fib/ack/tak beat `ocamlc` bytecode and `ocaml` toplevel — 3.5× behind `ocamlopt` native on fib, 15-20× on list-heavy nqueens / sieve.
- [`sample/asom/`](./sample/asom/) — Smalltalk dialect ([SOM](https://som-st.github.io/)). Type-specialized sends, control-flow inlining, Boehm GC + GMP. Passes the full SOM TestSuite (221/221).
- [`sample/pascalast/`](./sample/pascalast/) — Pascal subset (~190 nodes), ISO 7185 + Free Pascal–style OO. Variant records, sets, `with`, dynamic arrays (`array of T`), subrange range-checking, `virtual` / `override` / `inherited` / `abstract` / `class procedure`, properties, `is` / `as`, catchable `try/except/finally`, hand-written parser. 49/49 tests; AOT bakes callee SD + per-proc metadata + fp threading + IC for vcall so call-heavy benches reach within ~2× of `fpc -O3` (fib 1.7× / tarai 1.9× / gcd 1.2×) and **win outright on tight constant-folding loops** (collatz 0.4× / leibniz 0.6× / mandelbrot 0.8× of fpc -O3). See [`docs/compare_fpc.md`](./sample/pascalast/docs/compare_fpc.md).
- [`sample/castro/`](./sample/castro/) — C subset. tree-sitter-c front-end, slot-based 8-byte VALUE, structs / function pointers / `printf`, `gcc -E` preprocessing. AOT beats `gcc -O0` on tight loops.
- [`sample/aforth/`](./sample/aforth/) — Forth subset. Each word is a NODE, `: word ;` bodies + toplevel are AOT entries; word calls indirect through a `word_id` → `NODE *` table so recursion and mutual recursion need no cycle break. AOT speedups: 2.6× on deep `RECURSE` (fib / ack — call-floor bound) up to 33× on inner-loop benches (gcd) where the body folds into a single SD that gcc unrolls. Wins 8/9 against gforth 0.7.3 — best 13.9× on gcd, 8.1× on factorial, 7.2× on collatz.
- [`sample/astr/`](./sample/astr/) — R subset (~30 nodes). Tagged 64-bit `VALUE` (1-bit fixnum / heap-boxed `astr_obj`), Boehm GC, fixnum-fixnum arithmetic fast paths with vector/scalar slow path, numeric / integer / list / string heap types, `c()` / `paste` / `substr` / `sum` / `1:n` ranges, R-flavoured `[1] ...` print. AOT 4.1× on fib(36), 3.8× on ack(3,9); tight `while` loop already near-optimal so flat there.
- [`sample/wastro/`](./sample/wastro/) — WebAssembly 1.0 (MVP) interpreter (~210 nodes). Reads both `.wat` and `.wasm`, runs the wasm spec-test `.wast` harness.
- [`sample/astrogre/`](./sample/astrogre/) — Ruby-style regex engine (~22 match nodes — the matcher itself is an AST) plus a grep CLI. Switchable at runtime between the astrogre backend and Onigmo. The for-each-start-position search loop is itself a node, so AOT specialization fuses the loop + inlined regex chain into one SD function (7.2× over interp on long-buffer literal search).
- [`sample/nuq/`](./sample/nuq/) — `jq` clone (~50 nodes).  Full jq filter language modulo regex / assignment-style operators: pipe / comma fan-out, `if-elif-else-end`, `try-catch`, `reduce` / `foreach`, `label` / `break`, `as $x` bindings, user `def`s with both value (`$x`) and filter parameters, string interpolation, `@text` / `@json` / `@csv` / `@tsv` / `@uri` / `@html` / `@sh` / `@base64[d]` formats, and 70+ built-ins (`length`, `keys`, `map`, `select`, `range`, `sort_by`, `group_by`, `unique_by`, `to_entries` / `from_entries` / `with_entries`, `paths`, `limit`, `first` / `last` / `nth`, `getpath`, `indices`, …).  Tree-walker over a tagged 1-bit fixnum + heap-`nuq_obj` (null / bool / double / string / array / object) value model with Boehm GC.  338 / 338 tests pass; about half are differential against system `jq`.

## References

- VMIL 2025 — *ASTro: An AST-Based Reusable Optimization Framework*. [ACM DL](https://dl.acm.org/doi/10.1145/3759548.3763371)
- PPL 2026 — *ASTro による JIT コンパイラの試作*. [program](https://jssst-ppl.org/workshop/2026/program.html)
