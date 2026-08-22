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
2. **AOT compilation** — specialize the whole AST offline, link, and run.  Several samples also build **standalone executables** (`--build`): the AST is re-emitted as C (`_embed.c`) with dispatchers pre-bound to the specialized code, so the binary starts without parsing anything — and this path cross-compiles (e.g. `koruby_precise` → wasm32-wasip1).
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

**At a glance — 29 samples by paradigm:**

| Group | Count | Samples |
|---|---:|---|
| Tutorial / calculators | 2 | `calc`, `abc` |
| Ruby family | 6 | `naruby`, `baruby`, `baruby_precise`, `abruby`, `koruby`, `koruby_precise` |
| Other dynamic scripting | 4 | `luastro`, `jstro`, `pystro`, `anlox` |
| Functional / OO academic | 6 | `ascheme`, `ascheme_precise`, `astocaml`, `asml`, `ancaml`, `asom` |
| Statically-typed imperative | 3 | `pascalast`, `castro`, `anpy` |
| Stack-based | 1 | `aforth` |
| Data processing | 2 | `astr`, `arawk` |
| Non-source / DSL | 5 | `wastro`, `astrogre`, `nuq`, `arjsv`, `arcel` |

**Tutorial / calculators.**
- [`calc`](./sample/calc/) — **toy 6-node calculator REPL** (`num` + `+`/`-`/`*`/`/`/`%`), the smallest end-to-end ASTroGen example.
- [`abc`](./sample/abc/) — **POSIX/GNU `bc`-compatible arbitrary-precision calculator** (40 nodes): GMP-mantissa + `scale` fixed-point, variables / arrays / user functions (dynamic scope) / `if`-`while`-`for` / `ibase`-`obase` bases, with **LSB-tagged fixnum immediates** so scale-0 small integers skip GMP/GC; Boehm GC throughout.
  Verified by **5,500+ differential tests vs system `bc`** (curated + fixtures + seeded fuzzing); **faster than `bc` on every benchmark (geomean ~6×)** — factorial **42×** / sqrt **25×** via GMP, integer loops 2–5×.

**Ruby family.**
- [`naruby`](./sample/naruby/) — ***not a Ruby***: tiny integer-only Ruby subset (32 nodes).
  The original ASTro paper's vehicle and **the only sample exercising all four execution modes** (plain / AOT / PG / JIT) from a single binary.
- [`baruby`](./sample/baruby/) — ***barely a Ruby***: naruby fork extended with **Array + String + libgc** (52 nodes); LSB-tagged VALUE, parse-time method desugar (no OO machinery).
  Built as the **first testbed for the unified GC framework** ([`docs/gc_design.md`](./docs/gc_design.md)) — minimal value representation, ships with `binary_trees` / `list_alloc` / `string_concat` benches at ~1 s scale that report wall time + libgc collection counts.
- [`baruby_precise`](./sample/baruby_precise/) — **precise GC testbed** forked from `baruby`: same language surface, but **14 hand-written GC backends** (`none` / `mark` / `mark_gen` / `mark_gen_inc` / `copy` / `copy_gen` / `mark_compact` / `mark_compact_gen` / `bump` / `mark_bump_gen` / `immix` / `immix_gen` / `mark_bitmap_gen` / `mark_card_gen`) selectable by `make GC=<name>`.
  Precise rooting via shared `sp[]` spill (Lua/Rust-style; see [`docs/gc_design.md`](./docs/gc_design.md)). Ships a 14-bench × 14-backend perf matrix and per-collector wall time / pause / collection-count breakdown.
- [`abruby`](./sample/abruby/) — ***a bit Ruby***: larger Ruby subset as a CRuby C extension (107 nodes), reusing CRuby's `VALUE` / Prism / GC.
  PGC-baked optcarrot **86.5 fps** vs CRuby (no-JIT) 45.6 fps; integer-loop microbenches 4–8×.
- [`koruby`](./sample/koruby/) — ***kind of Ruby***: standalone (non-CRuby) Ruby with Boehm GC + GMP + Prism (119 nodes).
  **Runs optcarrot end-to-end at ~100 fps** (gcc-15 -O3, vs CRuby 42 fps ≈ 2.4×; YJIT 175 fps).
- [`koruby_precise`](./sample/koruby_precise/) — **the flagship sample**: koruby rebuilt from scratch (v2) on a slots ABI with **precise rooting + moving/copy GC** (16 build-time selectable backends) and `RESULT`-based error propagation — no `setjmp`/`longjmp`, no machine-stack scanning, every alloc path audited under GC-stress + page-purge.  Aims to be a **CRuby drop-in**: core classes + regex ([`astrogre`](./sample/astrogre/)-backed) + stdlib subset; **optcarrot runs checksum-identical** (AOT 77 fps = 1.8× plain CRuby; YJIT still wins on optcarrot, while on microbenches AOT beats YJIT except recursion / object-heavy), pure-Ruby **DOOM and a Game Boy emulator render pixel-perfect**, and unmodified **rubyspec core via real mspec passes 80.2%**.
  `--build` emits **standalone executables** (program + prelude ASTs and all specialized code embedded — zero parsing at startup) and cross-compiles to **wasm32-wasip1**: the AOT .wasm starts in 0.014 s (ruby.wasm 0.09 s) and runs 2–37× faster than ruby.wasm on loop/alloc benches; a browser demo (WASI shim, main-thread vs Worker) is included ([`wasi/`](./sample/koruby_precise/wasi/)).

**Other dynamic scripting.**
- [`luastro`](./sample/luastro/) — **Lua 5.4** (74 nodes) with tagged 8-byte `LuaValue`.
  Full pattern matcher, weak tables, `__gc` finalizers, ucontext coroutines, self-written mark-sweep GC.
- [`jstro`](./sample/jstro/) — **JavaScript** (broad ES2023 subset, 101 nodes) with V8-style hidden-class objects + shape-transition ICs.
  Profile-driven kind swap, longjmp `throw`, mark-sweep GC; beats node v18 by 2–50× on several benches.
- [`pystro`](./sample/pystro/) — **Python 3** subset (95 nodes): GMP bignum + classes + dunder + `try`/`except` + generators + comprehensions + f-strings + ~37 builtins.
  **Beats CPython 3.12 on 13 / 14 benches** (4 macro + 9/10 micro) at ~1 s scale (`while_loop` 17×); also beats CPython 3.14+JIT on 12/14.

**Functional / academic.**
- [`ascheme`](./sample/ascheme/) — **R5RS Scheme** (54 nodes) with the full numeric tower (fixnum / bignum / rational / flonum / complex via GMP), `call/cc`, multiple values, ports.
  Passes 179/179 of chibi's `r5rs-tests.scm`; vs chibi 18/18 wins, vs guile 17/18 (up to 27×).
- [`ascheme_precise`](./sample/ascheme_precise/) — ascheme fork migrated from Boehm libgc to the **precise GC framework** (`runtime/precise_gc/`, 17 build-time selectable backends): precise rooting via `sframe` + `sp` scratch, GMP finalizers, GC-stress / page-purge audit knobs.  Same R5RS surface and AOT pipeline; serves as the framework's Scheme testbed (with a libgc baseline in its `docs/perf.md`).
- [`astocaml`](./sample/astocaml/) — **OCaml** subset (91 nodes) with HM-lite type inference, ADTs, exceptions, single-inheritance classes, real functor instantiation, TCO.
  35/35 tests; with `-c`, fib / ack / tak beat `ocamlc` bytecode and `ocaml` toplevel.
- [`asml`](./sample/asml/) — **Standard ML** subset (85 nodes) with **full Hindley–Milner type inference** (Algorithm W + level-based generalisation + value restriction), ADTs with type parameters, **records** (`{x=1, y=2}` / `#field` / record patterns), pattern match, exceptions, refs.  Type errors halt at compile time; the type-checker drives lower-time dispatcher specialisation, and **generic dynamic-typecheck nodes are removed from `node.def` entirely** — only `_int` / `_real` / `_string` / `_poly` / `_bool` / `_unchecked` variants remain.  AOT pipeline includes `is_leaf`-driven C-stack alloca, `mark_tail_calls` post-pass, and astocaml-style aggressive cflags (`-fno-stack-clash-protection -flto`).  vs SML/NJ v110.79: AOT **1.8×** of SML/NJ on `fib` and **2.7× faster** on `strcat`; 30× gap on multi-arg curried recursion (next target: `app2`/`app3` lower-time folding).
- [`ancaml`](./sample/ancaml/) — **[MinCaml](https://esumii.github.io/min-caml/)** (the monomorphic ML subset from Tohoku University's compiler course, 38 nodes): a from-scratch **Hindley–Milner inferer** (destructive unification, defaults free vars to `int`), **de Bruijn frame** variables resolved at parse time, boxed-float / closure / tuple / array values, and **tail-call elimination** (trampoline) + leaf-frame `alloca`.  Differential-tested vs the system `ocaml` (a two-line prelude reconciles `print_char`'s arg type and the removed `Array.create`); AOT `fib` beats OCaml bytecode (only native `ocamlopt` is ahead).  Ships a **self-contained reimplementation reference** ([`docs/mincaml_impl_spec.md`](./sample/ancaml/docs/mincaml_impl_spec.md)) and an implementation-agnostic harness (`ANCAML=/path ruby test/run_tests.rb`).  Part of the **An\*** series (*ASTro Nutshell* — bite-sized pedagogical "subject" languages; cf. [`anpy`](./sample/anpy/)).
- [`asom`](./sample/asom/) — **[SOM](https://som-st.github.io/)** Smalltalk dialect (80 nodes) with type-specialized sends, control-flow inlining, Boehm GC + GMP.
  Passes the full SOM TestSuite (221/221); PG mode beats SOM++ on all 12 AreWeFastYet benches.

**Statically-typed imperative.**
- [`pascalast`](./sample/pascalast/) — **Pascal** subset (159 nodes), ISO 7185 + Free Pascal–style OO: variant records, sets, `with`, `array of T`, `virtual` / `override`, `try/except`, properties.
  **Wins outright on tight constant-folding loops vs `fpc -O3`** (collatz 0.4× / leibniz 0.6× / mandelbrot 0.8×).
- [`castro`](./sample/castro/) — **C** subset (101 nodes) with tree-sitter-c front-end, 8-byte slot VALUE, structs / function pointers / `printf`, `gcc -E` preprocessing.
  AOT beats `gcc -O0` on tight loops; `crc32` ties `gcc -O1`.
- [`anpy`](./sample/anpy/) — **[ChocoPy](https://chocopy.org/)** (statically-typed Python 3.6 subset, 41 nodes): indentation lexer, a **static type checker** (conformance / assignment-compat / join + §5.2 rules), single-inheritance classes with dynamic dispatch, nested functions with `global`/`nonlocal` closures, lists/strings; tagged-immediate ints + Boehm GC.
  Verified by **differential testing vs `python3`** (valid ChocoPy == valid Python 3.6) plus runtime-error and type-error-rejection cases.  Beats CPython 3.12 on loop-heavy code (loop/collatz/list 0.4–0.5×), trails on call-heavy code (per-call env-frame cost); geomean 1.26×.  Ships a **self-contained implementation reference** ([`docs/chocopy_impl_spec.md`](./sample/anpy/docs/chocopy_impl_spec.md)) and an **implementation-agnostic harness** (`ANPY=/path/to/impl ruby test/run_tests.rb`) so a from-scratch ChocoPy in any language can be checked against the same suite.  First of the **An\*** series (*ASTro Nutshell* — bite-sized pedagogical "subject" languages; cf. [`ancaml`](./sample/ancaml/), [`anlox`](./sample/anlox/)).
- [`anlox`](./sample/anlox/) — **[Lox](https://craftinginterpreters.com/)** (Robert Nystrom's *Crafting Interpreters* teaching language, 38 nodes): a dynamic language with closures and single-inheritance classes (`this`/`super`/dynamic dispatch).  Demonstrates a parse-time **resolver** assigning `(depth, slot)` to locals (globals stay by-name), bound methods via a `this`-frame, and the canonical self-contained **`// expect:` test format** (no system Lox exists; set `ANLOX_REF` for a reference diff).  `--build` is unsupported (side-table indirection); interpreter + `--aot-compile` work.  Part of the **An\*** series.

**Stack-based.**
- [`aforth`](./sample/aforth/) — **Forth** subset (68 nodes) where every word is an AST NODE (no traditional threaded code); calls indirect through a `word_id` → `NODE *` table.
  Wins 8/9 against gforth 0.7.3 (gcd 13.9×, factorial 8.1×, collatz 7.2×; ack 0.95× the only loss).

**Data processing.**
- [`astr`](./sample/astr/) — **R** subset (46 nodes) with tagged 64-bit `VALUE`, libgc, vector/scalar broadcast, `c()` / `paste` / `substr` / `1:n` ranges.
  AOT 4.1× on fib(36), 3.8× on ack(3,9).
- [`arawk`](./sample/arawk/) — **POSIX awk** subset (115 nodes, regex excluded) — BEGIN/END + pattern-action rules, `$N`/`NR`/`NF`, associative arrays, `for-in`, user-defined functions, full `printf`/`sprintf`, six `getline` forms, pipe + redirect I/O, UTF-8 codepoint mode (gawk-style `LC_CTYPE` detect; `--byte`/`--posix` for mawk-compatible byte mode).
  AOT geomean **0.93× of gawk** on the bench set (beats goawk 1.07×, behind mawk 1.93×). Phase 2 plans an **AST-interpreter merge with [`astrogre`](./sample/astrogre/)** so awk + regex run under a single ASTroGen instance.

**Non-source / DSL.**
- [`wastro`](./sample/wastro/) — **WebAssembly 1.0+ (MVP)** interpreter (212 nodes).  Reads both `.wat` and `.wasm`, runs the wasm spec-test `.wast` harness; AOT-cached within **3–6× of `gcc -O2`** on tight loops.
- [`astrogre`](./sample/astrogre/) — **Ruby/Onigmo-compatible regex engine** (53 nodes — the matcher itself is an AST) with a `grep`-style CLI (`are`); switchable backend (astrogre / Onigmo).  AOT fuses the start-position scan loop and the regex chain into one SD (8/8 wins vs Onigmo, 3–15×).
- [`nuq`](./sample/nuq/) — **`jq` 1.7-compatible clone** (209 nodes) with full pipeline / `try-catch` / `reduce` / `foreach` / module / call-by-name / 70+ builtins.  Passes **524/526** of jq 1.7's official test suite; real-world 11/11 wins **2.6–5.0× vs jq** (micro `upto` 50×, `reverse` 16×).
- [`arjsv`](./sample/arjsv/) — **JSON Schema validator** (drafts 04 / 06 / 07 / 2020-12, 47 nodes), CRuby C extension, `json_schemer`-compatible API, per-schema SD with alloc-free `valid?`.
  Test Suite (excl. IDNA / HTTP `$ref` / dynamic `$dynamicRef`): 04 98.8% / 06 98.9% / 07 95.5% / 2020-12 94.6%.  **4–11× vs `rj_schema`** (Rust+RapidJSON FFI), **25–180× vs `json_schemer`** (pure Ruby).
- [`arcel`](./sample/arcel/) — **CEL (Common Expression Language)** evaluator (57 nodes) — drop-in replacement for `cel-go` / `cel-cpp` aimed at K8s admission / Envoy authz / IAM Conditions hot paths.
  Passes **808/808** of Google's `cel-spec/tests/simple/testdata` (vs cel-go 89.7% on the same harness).  AOT vs cel-cpp on 11 bench cases: geomean **14×**, realistic K8s ValidatingAdmissionPolicy **52 ns/op (22.4×)**, `bool_ladder` constant-folds to 5 ns/op (**42.6×**).

## References

- VMIL 2025 — *ASTro: An AST-Based Reusable Optimization Framework*. [ACM DL](https://dl.acm.org/doi/10.1145/3759548.3763371)
- PPL 2026 — *ASTro による JIT コンパイラの試作*. [program](https://jssst-ppl.org/workshop/2026/program.html)
