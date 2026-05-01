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
sample/astrogre/        Ruby-style regex engine — prism front-end, ~22 match nodes
docs/                   Design notes and papers
```

## Samples

The samples share a uniform layout (`node.def`, `Makefile`, optional language-specific ASTroGen extension, per-sample `docs/`). Each sample's README has the language scope, build / run, benchmarks, and design notes.

- [`sample/calc/`](./sample/calc/) — Toy calculator REPL (6 arithmetic nodes). The smallest end-to-end example for understanding ASTroGen.
- [`sample/naruby/`](./sample/naruby/) — *Not a Ruby*: minimal Ruby subset (~21 nodes, integer-only). Used in the original ASTro paper to evaluate all four execution modes including JIT.
- [`sample/abruby/`](./sample/abruby/) — *a bit Ruby*: larger Ruby subset implemented as a CRuby C extension. Reuses CRuby's `VALUE` / Prism parser / numerics (Bignum, Float, Rational, Complex).
- [`sample/koruby/`](./sample/koruby/) — *kind of Ruby*: standalone (non-CRuby) Ruby implementation. Boehm GC + GMP Bignum + Prism parser + ucontext `Fiber`. CRuby-compatible `VALUE` encoding so heap layouts and code can be ported. Closures (yield-shared-fp), classes/modules/include, attr_*, lexical constants per method (cref), `case/when`, multi-assign, splat in args / array / range, block destructure, `super`, `ensure`, optional + rest parameters, singleton methods (`def self.foo`), `Struct`/`File`/`STDOUT`/`Process`/`Time`/`Fiber`. Beats CRuby (no JIT) on fib by ~1.6× via interp + 3.6× with AOT specialization. **Runs optcarrot end-to-end** (NES.new → 30 frames → fps + checksum); ~11× off CRuby on optcarrot wall-clock with interp only — checksum doesn't match CRuby yet (subtle emulation drift), but the full pipeline executes without errors.
- [`sample/wastro/`](./sample/wastro/) — WebAssembly 1.0 (MVP) interpreter, ~210 nodes. Reads both `.wat` and `.wasm`, runs the wasm spec-test `.wast` harness, AOT-cached comparable to Cranelift on the bundled benchmarks.
- [`sample/asom/`](./sample/asom/) — Smalltalk dialect ([SOM](https://som-st.github.io/)). Type-specialized sends, control-flow inlining, flonum-tagged Double, Boehm GC, GMP Bignum. SOM-st/SOM submodule for stdlib + TestSuite + AreWeFastYet; passes 221/221 TestSuite, IntegerTest 25/25 + DoubleTest 27/27. AOT beats SOM++ on 11/12 AWFY (Sieve **10×**, QuickSort 5.3×, Fannkuch 4.3×); on cold-start wall-clock often beats TruffleSOM (asom-aot 0.65s vs Truffle 1.74s on Sieve). TruffleSOM warm-peak still 4-28× faster — the gap is method-level PE + escape analysis, not interpretation overhead.
- [`sample/ascheme/`](./sample/ascheme/) — R5RS Scheme. Full numeric tower (fixnum / bignum / rational / flonum / complex via GMP), tail calls, `call/cc`, multiple values, ports. Passes 179/179 of chibi's `r5rs-tests.scm`; AOT-cached beats chibi-scheme on 5/7 micro-benchmarks, guile-3.0 (JIT) on 7/7.
- [`sample/luastro/`](./sample/luastro/) — Lua 5.4. Tagged 8-byte `LuaValue` (63-bit fixnum + inline flonum), full pattern matcher, weak tables, `__gc` finalizers, ucontext coroutines, mark-sweep GC. AOT-cached beats lua5.4 on `loop` (12.7×), `factorial` (2.6×), `fib` (1.9×).
- [`sample/jstro/`](./sample/jstro/) — JavaScript (broad ES2023 subset). Tagged 8-byte `JsValue` (CRuby-style fixnum / flonum / heap pointer), V8-style hidden-class objects with shape-transition ICs, monomorphic call ICs that inline the body, longjmp-based `throw`. Covers `class extends Base { super(...); super.method(); get/set; static {...}; #priv }`, full destructuring (incl. nested + default + rest in function params), spread (call / array / object), `??` / `?.` / `?.()` / `??=`, computed keys, tagged templates, `arguments` / `new.target`, labeled `break`, hoisting of function decls and `let`/`const` (with TDZ), per-iteration `let` binding in `for` loops. Stdlib has the common `Array.prototype` / `Object` / `String` / `Math` / `Number.prototype` / `Function.prototype` methods, `Map` / `Set` / `WeakMap` / `WeakSet` / TypedArrays, `Symbol(desc)` (unique heap value), `JSON.{parse,stringify}`, `RegExp` with regex literals (`/pat/flags` — minimal NFA backtracker), `Promise.{resolve,reject,all,then}`, `async function` / `await` (synchronous resolution), `function*` / `yield` (syntax accepted; suspend not implemented), `Proxy(target, handler)` with get/set traps, `Reflect.*`, `eval(str)`, `new Function(...)`, **`require()`** (CommonJS) and ES `import` / `export`. BigInt is **not** implemented (the `123n` literal is accepted but treated as a regular Number). Pure tree-walking interpreter (no JIT yet); nbody runs in ~0.38 s — ~21× off node.js (V8). ~7 K lines hand-written.
- [`sample/castro/`](./sample/castro/) — C subset. tree-sitter-c front-end + Ruby parse driver, slot-based 8-byte VALUE, structs / function pointers / `printf` / libc-ish builtins, `gcc -E` preprocessing. RESULT-state non-local exits (no setjmp), wastro-style caller-allocated stack VLA frame so loop variables register-promote, leaf-helper inlining via NODE-child operand on `node_call_static`. AOT-cached beats `gcc -O0` on `loop_sum` / `nqueens` / `quicksort`, ties `gcc -O1` on `crc32`; passes 30/30 feature tests + 184/220 of c-testsuite.
- [`sample/pascalast/`](./sample/pascalast/) — Pascal subset, ~150 nodes, ISO 7185 + OO ライン到達。Hand-written recursive-descent parser (no external dependency), int64 / boolean / real with parser-side static typing, libgc-backed heap. Covers nested procedures (display vector), procedure values (`@func`), text-file I/O (`assign / reset / rewrite / eof`), variant records (`case … of` in record), records + `with`, var-record params, sets (`+ - *` / `in`), strings with mutation (`s[i] := c`), pointers / `new` / `dispose`, `try/except/finally` + `raise` + `ExceptionMessage`, `for-in`, `unit/uses` + `interface/implementation`, **OOP with true virtual dispatch** (per-class vtable, `virtual` / `override` / `inherited` / destructor — `a: TAnimal` holding a `TDog` calls `TDog.speak`), source-line tracking on runtime errors. AOT specialization works — `make aot-bench BENCH=<name>` shows 2 × (recursive) to ~25 × (tight numeric loops). 40/40 tests pass.
- [`sample/astrogre/`](./sample/astrogre/) — Ruby-style regex engine (~22 match-node kinds) plus a grep front-end on top. The matcher itself is an AST: prism front-end (drives `pm_parse` + `pm_visit_node`, picks the first `PM_REGULAR_EXPRESSION_NODE`, threads its `unescaped` body + flag bits into a small recursive-descent regex parser) feeds an AST in continuation-passing form; repetition is driven by a singleton `node_re_rep_cont` sentinel + `c->rep_top` frame stack so the AST stays acyclic for hashing. Covers `[...]`/`[^...]` (256-bit ASCII bitmap), `\d \w \s \D \W \S`, `* + ? {n,m}` greedy/lazy, capturing/non-capturing/named groups, `\1`–`\9`, `^ $ \A \z \Z \b \B`, lookahead `(?=)` / `(?!)`, alternation, inline-flag groups `(?ixm-ixm:...)`, `/i /m /x /n /u`. ASCII + UTF-8 (proper codepoint advance for `.`); pure tree-walking interpreter for v1, the framework's specialize / code-store hooks are wired but not driven yet. The binary `./astrogre` is a grep CLI (`-i -n -c -v -w -F -l -L -H -h -o -r -e --color=auto`) using the engine; backend is switchable at runtime with `--backend=astrogre|onigmo` (Onigmo cloned + locally built via `make WITH_ONIGMO=1`). 44/44 self-tests pass; on a 118 MB grep corpus astrogre is currently 2–4× behind Onigmo and 10–100× behind ripgrep / ugrep — gap is the literal-prefix prefilter and AOT specialization that aren't wired yet.
- [`sample/astocaml/`](./sample/astocaml/) — OCaml subset interpreter, ~75 nodes. Hand-written recursive-descent parser, tagged 8-byte VALUE (62-bit fixnum + heap pointer), boxed float / cons / string / closure / tuple / ref / variant / record / array / lazy / bytes / object. Covers Phase 1 + Phase 2 + most of Phase 3: `let / let rec / and` (top + nested mutual rec), `fun / function`, `match` with full nested patterns + `when` guards + `as` + or-patterns + match-exception + range patterns (`'a' .. 'z'`), tuples / refs / boxed floats with `+. -. *. /.`, polymorphic structural compare with fixnum inline fast-path, full algebraic data types (variants + records + polymorphic variants `` `Foo ``), exceptions (`try / with / raise`, predefined + user-defined `exception E of T`), labeled `~x` / optional `?x` args (positional simplification), `lazy` / `Lazy.force`, `module M = struct ... end` (nested + `open` + alias + `let open M in body`), single-inheritance `class` with `inherit` + `initializer` + bare-field access in method bodies, custom infix operator definition (single + multi-char like `(+!)` `(<*>)`), `Printf.printf`, `Bytes`, `Hashtbl`, `Stack`, `Queue`, `Buffer`, `List.{sort,assoc,partition,combine,split,find,find_opt,exists,for_all,flatten,init,iter2,map2}`, AOT specialize via `--compile`, **tail-call optimization** (1M-step recursion runs in constant stack), and a **gref inline cache** (per-callsite `@ref` cache keyed on `globals_serial`) that gave 3-5× speed-up over the post-Phase-2 baseline. 30/30 tests pass; 5 sustained-~1 s benchmarks (fib 1.4 s, ack 0.8 s, nqueens 1.2 s, sieve 1.2 s, tak 0.7 s). GADT / first-class module / true functor instantiation / multi-inherit / `ppx` / Effect handlers deferred (see `sample/astocaml/docs/todo.md`).

## References

- VMIL 2025 — *ASTro: An AST-Based Reusable Optimization Framework*. [ACM DL](https://dl.acm.org/doi/10.1145/3759548.3763371)
- PPL 2026 — *ASTro による JIT コンパイラの試作*. [program](https://jssst-ppl.org/workshop/2026/program.html)
