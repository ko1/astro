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

- Values: `int`, `real`, `string`, `bool`, `unit`, `'a list`, tuples,
  records `{f1 = v1, f2 = v2}`, `'a ref`, ADT constructors
- Top-level decls: `val pat = e`, `fun f p1 ... pN = e [and ...]`,
  `datatype 'a id = Ctor | Ctor of <type> | ...` (type vars + record /
  list / ref / tycon types in `of` allowed)
- Local: `let val/fun/datatype ... in e [; e]* end`
- Expressions: `if / then / else`, `case e of pat => e | ...`,
  `fn pat => e`, `e1 e2` (curried application), `e1 ; e2`,
  `andalso`, `orelse`, `not`,
  `+ - * div mod` for `int`, `/` for `real`,
  `< <= > >= = <>` polymorphic (specialised to `_int` / `_real` /
  `_string` / `_poly` at lower-time),
  `^` string concat, `::` cons, `@` list append,
  `ref e`, `!e`, `e1 := e2`,
  `raise e`, `e handle pat => e | ...`,
  `op +` etc. to get an operator as a value,
  `#field e` field selector (curried selector value `#field` also works)
- Patterns: `_`, identifier (variable), int literal, `~int`, string literal,
  `true / false`, `()`, `[]`, list literal `[a,b,c]`, cons `p :: p`,
  tuple `(p, p, ...)`, constructor `Ctor` / `Ctor pat`,
  record `{f1 = p1, f2 = p2}` / `{f1, f2}` (short)
- Types in datatype `of`: `'a` / `int` / `real` / `string` / `bool` /
  `unit` / `exn` / `T list` / `T ref` / `T tycon` / `(T, ...) tycon` /
  `T1 * T2` / `{f : T, ...}` (records)

## Builtins

`print`, `println`, `Int.toString`, `Real.toString`, `String.size` (`size`),
`List.length`, `List.null`, `List.hd`, `List.tl`, `List.rev`,
`real`, `floor`, `ref`, and the operators above as `op +`, `op =`, etc.

Built-in datatypes: `'a option = NONE | SOME of 'a` (registered as ctors).

## Install

### Prerequisites (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby libreadline-dev   # libreadline-dev is optional
```

ASTroGen runs from `make` and needs Ruby 3.x.  asml itself links only
`-ldl -lm` (no GMP / GC).  `libreadline-dev` is auto-detected and gives
the REPL line-editing; the build still works without it.

## Building / running

```sh
make
./asml test/fib.sml
./asml -e 'fun fact n = if n <= 1 then 1 else n * fact (n - 1); println (Int.toString (fact 10))'
make test               # run the test/*.sml suite
./asml -c file          # AOT-compile each top-level form before evaluating
bench/run.sh            # asml interp / AOT-cold / AOT-warm 比較
bench/compare.sh        # 上に加え `sml` (SML/NJ) があれば横並び比較
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
- Comparisons (`< <= > >= = <>`) dispatch on inferred type at lower-time:
  `_int` / `_real` / `_string` / `_poly` (the last for lists, tuples,
  variants, records).  No dynamic IS_INT fast-path remains — generic
  `node_lt` etc. were removed from `node.def`.
- Exceptions: `Match`, `Div`, `Empty`, `Fail of string` and any nullary
  constructors registered via `datatype` can appear in `handle` arms.
  All exceptions share the single type `exn`.
- Datatype: parametric (`'a list`, `'a option`) and nullary user
  datatypes are supported.  `datatype 'a t = ...` registers each
  constructor with a properly generalised scheme.

## Performance

Vs Standard ML of New Jersey v110.79 (`bench/compare.sh`, best-of-3):

| ベンチ      | asml-int | asml-AOT | sml/NJ  |
|-------------|---------:|---------:|--------:|
| fib (35)    |   0.45 s |   0.16 s |  0.09 s |
| ack (3, 9)  |   1.76 s |   1.52 s |  0.05 s |
| tak ×5      |   2.09 s |   2.01 s |  0.07 s |
| nqueens ×3  |   1.60 s |   1.46 s |  0.05 s |
| sumlist     |   0.49 s |   0.46 s |    FAIL¹|
| refloop     |   3.66 s |   2.63 s |    FAIL¹|
| recordsum   |   0.18 s |   0.19 s |    FAIL¹|
| strcat      |   0.28 s |   0.29 s |  0.79 s |

¹ SML/NJ has 31-bit `Int.int` (max 2^30 − 1); these benches overflow.
asml uses 63-bit fixnums.

asml AOT is ~1.8× SML/NJ on `fib` and **2.7× faster on `strcat`**.  The
remaining gap on multi-arg `ack` / `tak` / `nqueens` is the partial-state
malloc on every curried call (`f x y` lowers to `app1(app1(f, x), y)`);
folding these into `app2` / `app3` at lower-time is the next perf target
(see `docs/todo.md` §C).

The AOT pipeline applies astocaml-style aggressive cflags
(`-fno-stack-clash-protection -flto -finline-limit=10000 ...`), `is_leaf`
detection so closure frames live on the C stack via alloca, and a
post-lower `mark_tail_calls` pass that rewrites tail-position `app1` /
`app2` to `_tail_app*` for trampoline-driven constant-stack tail
recursion.  See `docs/perf.md` for the full investigation.

## Limitations

This is a teaching-grade subset.  Notable absent features: signatures /
modules / structures, functors, exception declarations, mutually
recursive datatypes, `local in end`, `where`, `withtype`, `abstype`,
infix/infixr declarations, character literals (`#"a"`), substring /
slice operations.  Type annotations on patterns / decls
(`fun f (x: int) = ...`) aren't parsed (the type checker infers
everything).  Record types are not row-polymorphic — `#field e` requires
that `e`'s type be **fully resolved** to a concrete record type at the
selector site (use a function-arg pattern `fun f {x, y} = ...` if you'd
otherwise hit "ambiguous record selector").  Only ASCII source is
handled.

The parser also has a few pragmatic shortcuts:

- `fun` clauses accept simple-variable, tuple, or any single-ply pattern
  parameters; deeper destructuring should use `case`.
- The body-skipping during the `fun ... and ...` two-pass parse uses
  paren / bracket / `let..end` nesting to find `and`-boundaries; it does
  not understand `if .. then .. else` or `case .. of` nesting (those
  forms have no unambiguous terminator), so an `and` inside an arm
  expression on the same nesting level would confuse it.  In practice
  this hasn't bitten any of the tests.
