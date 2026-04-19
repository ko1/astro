# abruby — "a bit Ruby"

<p align="center">
  <img src="docs/logo.svg" alt="abruby logo" width="180">
</p>

A Ruby-subset interpreter built as a CRuby C extension on top of the
[ASTro](../../README.md) framework. It reuses CRuby's `VALUE`
representation and parser (Prism), so numbers, strings, arrays, hashes,
regexps and friends behave like their Ruby counterparts, while the
evaluator itself is a tree-walking interpreter generated from
[`node.def`](node.def) by ASTroGen.

> Looking for the language reference? See
> [`docs/abruby_spec.md`](docs/abruby_spec.md). For what is/isn't
> implemented, see [`docs/done.md`](docs/done.md) and
> [`docs/todo.md`](docs/todo.md).

## Build

abruby is a CRuby extension; you need a Ruby with development headers
(the repo is developed against Ruby 4.0.x under rbenv). From this
directory:

```sh
ruby extconf.rb && make     # builds abruby.so
make test                   # runs the full test suite (1000+ tests)
make run                    # runs exe/abruby on test.ab.rb
make clean                  # removes build artifacts
```

## Running programs

Use the `exe/abruby` launcher. It accepts a script path or a one-liner
with `-e`:

```sh
./exe/abruby fib.ab.rb                # run a script
./exe/abruby -e 'p(1 + 2)'            # evaluate an expression
./exe/abruby -v                       # print the version

# Inspect the AST / disassembly:
./exe/abruby --dump -e '1 + 2 * 3'    # pretty-print the AbRuby AST
./exe/abruby -d      -e '1 + 2 * 3'   # same as --dump=disasm
```

A minimal script (`fib.ab.rb`):

```ruby
def fib(n)
  if n < 2
    n
  else
    fib(n - 1) + fib(n - 2)
  end
end

p(fib(34))
```

```sh
$ ./exe/abruby fib.ab.rb
5702887
```

## Language at a glance

abruby covers a practical Ruby subset. Highlights:

- **Literals** — integers (Fixnum + Bignum), floats, strings with
  `"#{...}"` interpolation, symbols, arrays, hashes (`{a: 1}` /
  `{"k" => v}`), `1..10` / `1...10` ranges, `/regex/`, `%w(..)` /
  `%i(..)`, heredocs, `Rational` (`3r`), `Complex` (`2i`).
- **Variables** — locals, instance variables (`@x`), globals (`$x`),
  compound assign (`+=`, `||=`, `&&=`), multiple assignment
  (`a, b = 1, 2`).
- **Control flow** — `if`/`elsif`/`unless`, `while`/`until`, postfix
  modifiers, ternary, `&&`/`||`/`!`, `case`/`when` with `===`,
  `begin`/`rescue`/`ensure`, `raise`, backtraces.
- **Methods & classes** — `def` with default/splat args, endless
  `def f = expr`, `attr_reader`/`_writer`/`_accessor`, `self.foo`
  class methods, `super`, `method_missing`, `method(:x)`,
  `respond_to?`, single inheritance, `include` for modules, constants
  (`FOO = 1`, `A::B`).
- **Blocks, Proc, Fiber** — `{ |x| ... }` / `do |x| ... end`, `yield`,
  `block_given?`, non-local `return`, `next`/`break` with values,
  `Proc.new` / `lambda` / `->`, `Fiber.new { ... }` +
  `resume` / `yield` / `transfer`.
- **Built-in classes** — `Object`, `Integer`, `Float`, `String`,
  `Symbol`, `Array`, `Hash`, `Range`, `Regexp`, `Rational`, `Complex`,
  `Proc`, `Fiber`, `TrueClass` / `FalseClass` / `NilClass`, plus the
  `Kernel` module (`p`, `raise`, `Rational`, `Complex`, ...).

Example covering a few of these (`test.ab.rb`):

```ruby
class Point
  def initialize(x, y); @x = x; @y = y; end
  def x = @x
  def y = @y
  def dist = @x * @x + @y * @y
end

sum = 0
i = 0
while i < 500_000
  pt = Point.new(i, i + 1)
  sum += pt.dist
  i += 1
end
p(sum)
```

## Execution modes

By default abruby runs as a tree-walking interpreter. The same
`exe/abruby` front-end also drives ASTro's **code store** — a cache of
specialized C dispatchers compiled per AST — for AOT and profile-guided
compilation. Pick one:

```sh
./exe/abruby --plain       file.ab.rb   # interpreter only, no code store
./exe/abruby               file.ab.rb   # reuse previously-baked code if any (default)
./exe/abruby -c            file.ab.rb   # AOT-compile every entry before running
./exe/abruby --pg-compile  file.ab.rb   # run, then bake hot entries for next time
./exe/abruby --aot-compile a.rb b.rb --run main.rb   # batch AOT, then run
```

Useful flags:

| Flag | Purpose |
|---|---|
| `--verbose` | trace code-store operations |
| `--pg-threshold=N` | only PG-bake entries dispatched ≥ N times (default 100) |
| `--compiled-only` | abort if the default (interpreter) dispatcher is used |
| `--aot-only` | ignore PGC index, use AOT `SD_<Horg>` only |
| `--code-store=DIR` / `ABRUBY_CODE_STORE=DIR` | pick a custom store directory |
| `--clear-code-store` (`--ccs`) | wipe the store before starting |

Baked C files land under `code_store/` (`SD_<hash>.c` for AOT,
`PGSD_<hash>.c` for profile-guided) and are linked into a single
`all.so` that `cs_load` swaps each node's dispatcher to.

## REPL

`exe/iabrb` is an interactive REPL with the same code-store knobs:

```sh
./exe/iabrb                  # plain interpreter REPL
./exe/iabrb -c               # AOT-compile each input before executing it
./exe/iabrb --pg-compile     # bake hot entries after each input
./exe/iabrb -d               # disasm what was compiled
```

## Testing

```sh
make test           # full suite (default build)
make debug-test     # rebuild with ABRUBY_DEBUG=1, run the suite, restore
```

`ABRUBY_DEBUG=1` enables strict runtime checks (`ab_verify`) that catch
class/type-tag mismatches early when hacking on the evaluator.

## Where to look next

- [`node.def`](node.def) — ASTro node definitions and `EVAL_*` bodies;
  the "source of truth" for the interpreter.
- [`abruby.c`](abruby.c) — CRuby extension entry point, `T_DATA` type,
  and the ALLOC wrappers exposed to the parser.
- [`lib/abruby.rb`](lib/abruby.rb) — Prism AST → AbRuby AST translator
  plus `AbRuby.eval` / `AbRuby.dump`.
- [`builtin/`](builtin) — per-class method implementations
  (`integer.c`, `string.c`, `array.c`, ...).
- [`docs/runtime.md`](docs/runtime.md) — runtime data-structure tour
  (classes, frames, inline caches, VALUE stack, GC).
- [`benchmark/`](benchmark) — bm_fib, bm_ack, bm_nbody, etc.; the
  baseline/comparison scripts live under `benchmark/report/`.
