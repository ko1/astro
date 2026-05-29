# Testing AnLox

Lox has no system interpreter to diff against, so AnLox uses *Crafting
Interpreters*' **canonical self-contained test format**: each `.lox` fixture
annotates its own expected output in comments, and the runner checks them.

```sh
make check                       # = ruby test/run_tests.rb
ANLOX=/path/to/impl ruby test/run_tests.rb       # test another implementation
ANLOX_REF=/path/to/jlox ruby test/run_tests.rb   # also diff stdout vs a reference Lox
```

## Annotation markers (matching the book's suite)

```lox
print 1 + 2;                 // expect: 3
print x;                     // expect runtime error: Undefined variable 'x'.
var a = ;                    // [compile]   (any syntax/resolve error → exit 65)
```

- `// expect: TEXT` — the next stdout line must equal `TEXT` (in source order).
- `// expect runtime error: MSG` — the program exits `70` and `MSG` appears on
  stderr; stdout up to that point must still match the `// expect:` lines.
- `// [compile]` or a `// Error…` comment — the program exits `65`.

The runner (`test/run_tests.rb`) reads the markers from each
`test/cases/*.lox`, runs `anlox`, and compares stdout + exit code.

## Optional differential testing

Set `ANLOX_REF` to a reference Lox (the book's `jlox` or `clox`) and the runner
additionally diffs `anlox`'s stdout against the reference for every
non-error fixture — true differential testing on top of the embedded
expectations.

## Writing fixtures

- Keep expected output deterministic.  For numbers, prefer integer-valued
  results (AnLox prints `3`, not `3.0`); avoid many-digit irrational decimals
  where AnLox's `printf` formatting may differ from the book's.
- A runtime error halts execution, so put at most one per fixture (with any
  preceding `// expect:` output before it).
- Add a file under `test/cases/` and it's picked up automatically.

## Coverage

`test/cases/` covers expressions/precedence, control flow + logical
short-circuit, closures (counter / recursion / capture), classes
(fields / methods / `this` / `init` / bound methods), inheritance (`super`,
override, inherited `init`), lexical scope/shadowing, and the three runtime
errors (undefined variable, `+` type error, arity).
