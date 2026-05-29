# AnCaml — implemented

## Language (faithful to MinCaml)

- Values: `unit`, `bool`, `int` (63-bit, tagged), `float` (boxed), heap
  closures / tuples / arrays.  No strings, lists, variants, or polymorphism —
  matching MinCaml.
- Literals: int, float (with optional exponent), `true`/`false`, `()`.
- Integer `+ - (unary -)`; **no** integer `* /` (MinCaml restriction).
- Float `+. -. *. /. (unary -.)`.
- Comparison `= <> < > <= >=` (polymorphic, structural), desugared to
  `Eq` / `LE` / `Not` in the parser exactly as MinCaml does.
- `not`, `if … then … else …`.
- `let x = e in e`, monomorphic.
- `let rec f a b … = e in e` — self-recursive, ≥1 parameter, first-class
  closures, higher-order use, lexical capture.
- `let (a, b, …) = e in e` tuple destructuring; tuple construction `e, e, …`.
- `Array.create` / `Array.make`, `a.(i)`, `a.(i) <- v` (bounds-checked).
- Sequencing `e1; e2` (`e1` must be `unit`).
- Nestable `(* … *)` comments.
- External functions: `print_int`, `print_char`, `print_newline`,
  `print_float`, `read_int`, `read_float`, `float_of_int`, `int_of_float`,
  `truncate`, `abs_float`, `sqrt`, `sin`, `cos`, `atan`, `floor`.

## Type system

- Monomorphic Hindley–Milner inference (`type.c`): destructive unification
  over `Type.Var` cells, occurs-check, first-error reporting with line number.
- External-function signatures; unresolved type variables default to `int`
  (display only) as in MinCaml.
- `--dump-types` prints the inferred program type; `--no-typecheck` skips the
  pass (debug).

- Integer literals are exact at any width: `int32`-range values use a compact
  `node_int`, wider ones a 64-bit `node_int64`; runtime arithmetic is 63-bit.

## Performance features

- **Tail-call elimination.** A parse-time pass (`ac_mark_tail`) rewrites
  tail-position applications into `node_tail_*` variants; `ac_apply` runs a
  trampoline, so tail recursion uses O(1) C stack (multi-million-iteration
  loops run fine).  The rewrite is an in-place kind+dispatcher swap, so it
  survives `--build` AST embedding and AOT compilation.
- **Leaf-frame `alloca`.** A function whose body creates no closures
  (`is_leaf`, computed during parsing) has its parameter frame stack-allocated
  in the caller rather than `GC_MALLOC`-ed — see [perf.md](perf.md) for the
  resulting ~3× speedup on integer recursion.

## ASTro framework integration

- 38 AST nodes; children evaluated via `EVAL_ARG` so the SD specializer folds
  the dispatch chain.
- de Bruijn `(depth, idx)` variable resolution (`node_lref`) done at parse time.
- Code-store wiring: `--aot-compile` bakes the top-level expression and every
  function body (each its own entry, since bodies are reached through a runtime
  dispatcher read in `ac_apply`).
- Standalone executables via the framework CLI: `ancaml --build OUT [--aot-compile] file.ml`.

## Tooling

- `test/run_tests.rb` — differential vs `ocaml` (positive / type-error /
  inferred-type golden / fixtures), 42 checks.
- `benchmark/run_bench.rb` — interp / AOT / ocaml-bytecode / ocamlopt-native.
- `docs/` — spec, full reimplementation reference, testing contract, runtime,
  perf, done/todo.
