# asml — Standard ML subset on ASTro

A small SML interpreter / type-checker, hand-written recursive-descent
parser, Algorithm-W Hindley–Milner inference, tree-walking evaluator.
Built with the same ASTroGen + code-store framework used by every other
`sample/`.

## Pipeline

```
source ─▶ parse ─▶ expr (IR) ─▶ infer (HM) ─▶ lower (with type-driven
                                                  specialisation) ─▶ NODE ─▶ EVAL
```

Type checking runs on every top-level form: well-typed code proceeds to
lowering, which uses the inferred types to drop dynamic IS_INT / IS_BOOL /
IS_REF guards (`node_add` → `node_add_int`, `node_lt` → `node_lt_int`,
`node_if` → `node_if_bool`, `node_assign` → `node_assign_unchecked`, etc.).
Ill-typed code is rejected at compile time with a line-numbered diagnostic
and exit status 2.

Polymorphism is true HM: `fun id x = x` infers `'a -> 'a`, both `id 5` and
`id "hi"` work; `'a list` and `'a option` work as expected; the value
restriction prevents unsound generalisation of side-effecting RHS (e.g.
`val r = ref []` is `'_ list ref`-style monomorphic).

## Supported subset

- Values: `int`, `real`, `string`, `bool`, `unit`, `'a list`, tuples, `'a ref`
- Top-level decls: `val pat = e`, `fun f p1 ... pN = e [and ...]`,
  `datatype id = Ctor | Ctor of <type> | ...`
- Local: `let val/fun ... in e [; e]* end`
- Expressions: `if / then / else`, `case e of pat => e | ...`,
  `fn pat => e`, `e1 e2` (curried application), `e1 ; e2`,
  `andalso`, `orelse`, `not`,
  `+ - * div mod` for `int`,
  `+ - * /` for `real`,
  `< <= > >= = <>` polymorphic,
  `^` string concat, `::` cons, `@` list append,
  `ref e`, `!e`, `e1 := e2`,
  `raise e`, `e handle pat => e | ...`,
  `op +` etc. to get an operator as a value
- Patterns: `_`, identifier (variable), int literal, `~int`, string literal,
  `true / false`, `()`, `[]`, list literal `[a,b,c]`, cons `p :: p`,
  tuple `(p, p, ...)`, constructor `Ctor` / `Ctor pat`
- Types: a type variable (`'a`) and parametric types are accepted but
  ignored — no type checker

## Builtins

`print`, `println`, `Int.toString`, `Real.toString`, `String.size` (`size`),
`List.length`, `List.null`, `List.hd`, `List.tl`, `List.rev`,
`real`, `floor`, `ref`, and the operators above as `op +`, `op =`, etc.

Built-in datatypes: `'a option = NONE | SOME of 'a` (registered as ctors).

## Building / running

```sh
make
./asml test/fib.sml
./asml -e 'fun fact n = if n <= 1 then 1 else n * fact (n - 1); println (Int.toString (fact 10))'
make test       # run the test/*.sml suite
./asml -c file  # AOT-compile each top-level form before evaluating
```

## Files

| File          | Purpose                                                         |
|---------------|-----------------------------------------------------------------|
| `node.def`    | AST node definitions (interpreter logic)                        |
| `context.h`   | `VALUE` representation, `CTX`, `mlobj`, options                 |
| `node.h`      | Type declarations + `NodeHead`                                  |
| `node.c`      | Glue: `EVAL`, `OPTIMIZE`, `INIT` + generated includes           |
| `main.c`      | Lexer / parser / runtime helpers / prelude / driver             |
| `asml_gen.rb` | ASTroGen subclass for `@ref` operands of host struct types      |
| `Makefile`    | Build + tests                                                   |
| `test/*.sml`  | Smoke tests with matching `*.expected`                          |

## Type system specifics

- HM full with let-polymorphism via level-based generalisation (Remy's
  algorithm).  Strict value restriction: only syntactic values
  (literals, lambdas, ctor-of-values, tuple-of-values, lref/gref) are
  generalised at let.
- `+ - *` are int-only; real arithmetic uses `/` (returns real) — this
  is a deviation from real SML's overloaded numeric operators.  `div`
  and `mod` are int (SML floor semantics).
- Comparisons (`< <= > >= = <>`) are polymorphic; lowering picks
  `node_*_int` when both operands are statically known to be `int`,
  otherwise the polymorphic `node_*` (which falls back to `ml_compare`).
- Exceptions: `Match`, `Div`, `Empty`, `Fail of string` and any nullary
  constructors registered via `datatype` can appear in `handle` arms.
  All exceptions share the single type `exn`.
- Datatype: parametric (`'a list`, `'a option`) and nullary user
  datatypes are supported.  `datatype 'a t = ...` registers each
  constructor with a properly generalised scheme.

## Limitations

This is a teaching-grade subset.  Notable absent features: signatures /
modules / structures, functors, records, exception declarations,
mutually recursive datatypes, `local in end`, `where`, `withtype`,
`abstype`, infix/infixr declarations, character literals (`#"a"`),
substring / slice operations.  Type annotations on patterns / decls
(`fun f (x: int) = ...`) aren't parsed (the type checker infers
everything).  Only ASCII source is handled.

The parser also has a few pragmatic shortcuts:

- `fun` clauses accept simple-variable, tuple, or any single-ply pattern
  parameters; deeper destructuring should use `case`.
- The body-skipping during the `fun ... and ...` two-pass parse uses
  paren / bracket / `let..end` nesting to find `and`-boundaries; it does
  not understand `if .. then .. else` or `case .. of` nesting (those
  forms have no unambiguous terminator), so an `and` inside an arm
  expression on the same nesting level would confuse it.  In practice
  this hasn't bitten any of the tests.
