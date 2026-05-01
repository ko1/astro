# pascalast — Pascal subset on ASTro

A tree-walking interpreter for a useful subset of Pascal, built on the
ASTro framework.

## What this is

`pascalast` is the project's "second-rung" sample, sitting between the
`calc/` minimum (6 arithmetic nodes) and the larger language samples
(`naruby`, `ascheme`, `luastro`, …).  It is meant to demonstrate that
ASTro's `node.def` mechanism scales to a real, statically typed source
language — variables and frames, recursion, arrays, control flow,
type-checked I/O, call-by-reference — without straying into specialized
runtime infrastructure (no GC, no closures, no objects).

The implementation is contained in this directory:

```
sample/pascalast/
├── README.md             this document
├── Makefile              ASTroGen + build + test + bench
├── node.def              ~50 nodes (int + real + var-param + 1D/2D array + I/O)
├── node.h / context.h    NODE / CTX / VALUE
├── node.c                runtime wiring (hash, alloc, EVAL, OPTIMIZE no-op)
├── main.c                lexer + recursive-descent parser + driver + helpers
├── docs/                 design notes, perf log, todo list
├── test/*.pas + .expected   correctness suite (19 tests)
└── bench/*.pas               sustained ~1 s benchmarks (15 programs)
```

The lexer + parser is hand-written; there is no external parser
dependency.  Pascal is case-insensitive for keywords and identifiers,
which the lexer handles by lowercasing every ID token at lex time.

## Status

- **45 / 45** correctness tests pass.
- ISO 7185 + Free Pascal 風 OO 完全(ish): ネスト手続き / 手続き値 /
  file I/O / variant record + 例外 (catchable raise) + unit/uses +
  **真の virtual dispatch (vtable)** + `inherited` + destructor +
  **`is` / `as`** + **properties** + **abstract methods** +
  **class methods** + 動的配列 + subrange range-check.
- 言語機能: ティア1〜2のフル + 例外 (`try/except/finally/raise`) +
  `unit/uses` + OOP (`class`/単一継承/virtual/override/abstract/
  class methods/properties/`is`/`as`/`inherited`/destructor) +
  文字列ミューテーション + AnsiString フル機能 (`copy / pos /
  insert / delete / setlength / IntToStr / StrToInt / …`) +
  動的配列 (`array of T`) + subrange `var x: 1..100` で代入時の
  範囲チェック + for-in + libgc-backed heap.
- 詳細リスト: [`docs/done.md`](./docs/done.md).
- 残: visibility enforcement, `goto` runtime, open array param,
  N-D 配列、`{$R+/-}` ディレクティブ — [`docs/todo.md`](./docs/todo.md).
- **AOT specialization** が動く。`make aot-bench BENCH=<name>` で
  parse → SPECIALIZE → 再ビルド → 比較。baked pcall (Round 8) で
  recursion benches も伸びるようになり fib 4.2× / tarai 5.5× /
  matmul 19× / collatz 26× / mandelbrot 26-37×。

For the full breakdown of what's done and what's outstanding:

- [`docs/done.md`](./docs/done.md) — language features and perf work that landed.
- [`docs/todo.md`](./docs/todo.md) — what's missing from a "real" Pascal and the next perf experiments to try.
- [`docs/runtime.md`](./docs/runtime.md) — how everything works at run time, with a focus on the call protocol and var-param indirection.
- [`docs/perf.md`](./docs/perf.md) — successful and unsuccessful tuning attempts.

## Build & run

```sh
make            # generates node_*.{c,h} via ASTroGen, builds ./pascalast
./pascalast test/01_hello.pas
```

CLI:

```
-q, --quiet      suppress non-output diagnostics
--dump-ast       print the program AST to stderr before running
--no-run         parse only
--no-compile     run pure interpreter even if specialized SDs are linked
-c FILE.pas      AOT mode — parse, dump node_specialized.c, exit
```

Round-trip:

```sh
make aot-bench BENCH=fib   # builds, specializes, rebuilds, times both
```

## Language subset

Numeric / boolean types

| Type      | Storage                                | Notes                                    |
|---|---|---|
| `integer` | int64                                  | aliases: `longint`, `int64`, `word`      |
| `boolean` | int64 (0 / 1)                          |                                          |
| `real`    | int64 holding the bit pattern of a double | aliases: `double`, `single`           |

Static type tracking is done in the parser; values themselves are
type-erased (uniformly 8 bytes), so every node returns/consumes a
`VALUE` and the right operator nodes are emitted at parse time.
Promotion `int → real` is automatic where Pascal allows it; the other
direction needs `trunc(x)` or `round(x)`.

Arrays (globals only):

```pascal
a: array[1..100] of integer;
m: array[1..N, 1..N] of integer;
m2: array[1..N] of array[1..N] of integer;   { equivalent }
```

Statements: `:=`, `if/then/else`, `while/do`, `repeat/until`,
`for/to/downto/do`, `case … of … else … end`, `begin … end`,
procedure / function call.

Subprograms: top-level `procedure name(params); var locals; begin …
end;` and `function name(params): T; …`, with `forward;` for the
header-only declaration.  Parameters can be `var` (call-by-reference);
recursion and mutual recursion both work.  Function return is by
assigning to the function name inside the body.

Built-ins (recognised by the parser, not first-class subprograms):
`abs sqr sqrt succ pred ord chr odd inc dec halt write writeln read
readln trunc round`.  `write` / `writeln` accept integers, reals
(with `:width:precision`), strings (literal only), and bool values.

Comments: `{ … }`, `(* … *)`, `// …`.

## Tests

`make test` runs every `test/*.pas`, diffs against `test/*.expected`,
and reports pass/fail.

| # | Test                | Coverage                                                         |
|---|---|---|
| 01 | hello              | output                                                            |
| 02 | arith              | int arithmetic, abs/sqr/succ/pred                                 |
| 03 | control            | for to/downto, while, repeat, if-else chains                      |
| 04 | proc               | recursive functions (fact, fib, ack, tarai)                       |
| 05 | array              | 1D array + insertion sort                                          |
| 06 | sieve              | bool array sieve                                                   |
| 07 | bool               | short-circuit and / or, odd                                        |
| 08 | recursion          | gcd, fast-pow, tarai                                               |
| 09 | inc / dec          | scalar + array element                                              |
| 10 | const              | integer constants                                                  |
| 11 | fizzbuzz           | nested if/else                                                     |
| 12 | quicksort          | recursive sort                                                     |
| 13 | string lit         | escape `'`, width spec                                             |
| 14 | nested             | 2-deep loops + early-ish exit via flag                              |
| 15 | forward            | `forward;` mutual recursion (even/odd, Hofstadter F/M)              |
| 16 | case               | constant labels, label list, range labels, `else`                  |
| 17 | varparam           | `var` swap + var-arg pass-through                                  |
| 18 | 2D array           | both `array[m,n]` and `array[m] of array[n]` syntaxes              |
| 19 | real               | mixed-type promotion, sqrt, trunc, round, real const, write width  |

## Benchmarks

`make bench` runs each `bench/*.pas` under `time` and prints
`<name>  <wall>  <maxRSS>`.  Each program is sized so the inner loop
runs around one second on this machine — short bursts are dominated by
AST build and process startup, so they would be useless as
comparisons.

| benchmark              | what it stresses                                         |
|---|---|
| `fib.pas`              | naive recursive fib(36) — exponential call count         |
| `tarai.pas`            | Takeuchi function — extreme recursion                    |
| `ackermann.pas`        | deep two-arg recursion                                   |
| `gcd.pas`              | recursive 2-arg call across a 2-D index space            |
| `quicksort.pas`        | recursive partition + global-array swap                  |
| `varparam_swap.pas`    | insertion sort using `swap(var, var)`                    |
| `sieve.pas`            | bool-array sieve to 2 M, repeated 12 ×                   |
| `collatz.pas`          | tight `mod`/`div` loop, no calls                         |
| `heron.pas`            | integer sqrt — boolean flag controls inner loop          |
| `nested_loops.pas`     | 4-deep `for` loops — pure for-overhead                   |
| `matmul.pas`           | N=128 integer matmul, flat-index storage                 |
| `matmul_2d.pas`        | same, native 2D `a[i, j]` indexing                       |
| `mandelbrot_int.pas`   | scaled-integer Mandelbrot                                |
| `mandelbrot_real.pas`  | floating-point Mandelbrot                                |
| `leibniz_pi.pas`       | Leibniz-series π — pure real arithmetic                  |

Recent `make bench` snapshot:

```
ackermann.pas         1.61 s   3712 KB
collatz.pas           1.40 s   2048 KB
fib.pas               1.01 s   2048 KB
gcd.pas               1.31 s   2048 KB
heron.pas             1.26 s   2048 KB
leibniz_pi.pas        1.05 s   2048 KB
mandelbrot_int.pas    1.38 s   2048 KB
mandelbrot_real.pas   1.40 s   2176 KB
matmul.pas            1.21 s   2432 KB
matmul_2d.pas         1.30 s   2304 KB
nested_loops.pas      1.66 s   2048 KB
quicksort.pas         1.13 s   2048 KB
sieve.pas             1.29 s  17664 KB
tarai.pas             1.77 s   2048 KB
varparam_swap.pas     ≈1.0 s    2048 KB
```

## What this sample is not

- **A standards-conformant Pascal compiler.**  Still missing:
  nested procedures, procedure values, file I/O, variant records,
  `goto`, packed arrays, units.  See [`docs/todo.md`](./docs/todo.md)
  for the full catalogue.
- **A profile-guided / JIT showcase.**  AOT works; PGC and JIT are not
  yet wired up.

## Key design choices (one-liners)

- **Statically typed at parse time, type-erased at run time.**  Every
  expression in the parser carries a `TE { NODE *n; int t; }`; the
  parser inserts `node_i2r` / picks `node_radd` vs `node_add` etc.
  At run time a `VALUE` is just an `int64_t`.
- **Two storage pools:** `c->globals[]` for globals, `c->stack[]` for
  call frames (one flat array, fp/sp pointers).  All variable access
  is index-only — no name lookup at run time.
- **Arrays live alongside globals.**  `c->arrays[idx]` is the heap
  buffer; `c->array_lo[idx]` and `c->array_size[idx]` (plus `*_2`
  for 2D) are stamped into the node so the runtime sees a 0-based
  index.
- **`var` parameters are addresses passed through the same VALUE
  channel.**  `node_addr_lvar` / `node_addr_gvar` /
  `node_addr_aref(2)` cast the storage address to `int64`; the
  callee uses `node_var_lref` / `node_var_lset` to deref.  When a
  var-param itself is forwarded to another var-param, the address is
  passed through unchanged via `node_addr_passthru`.
- **`case` lowers to an if-else chain** with a fresh hidden temp
  (local slot inside a procedure, fresh global at the main-block
  level).  Labels, label-lists, and ranges all share the same emit
  path.
- **`forward;` is reuse-by-name** in `parse_subprogram`: the proc
  table entry is allocated when the header is first seen with body =
  NULL; when the matching definition arrives later we reuse the slot
  and fill in `body`.  A final pass after `parse_program` rejects any
  forward without a matching body.

For the deeper version of all of this, see
[`docs/runtime.md`](./docs/runtime.md).
