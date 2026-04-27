# wastro — implemented features

Snapshot of what works as of v0.7.  See [`todo.md`](todo.md) for what
remains relative to the WebAssembly 1.0 spec.

## Numeric types

All four wasm 1.0 number types are carried in the dispatcher's `VALUE`
(= `uint64_t` raw bits) and reinterpreted at use sites via `AS_*` /
`FROM_*` helpers (`context.h`).

| Type | Width | Notes |
|---|---|---|
| `i32` | 32-bit | sign-extended on read, zero-extended in `VALUE` |
| `i64` | 64-bit | direct in `VALUE` |
| `f32` | IEEE 754 single | bit pattern in low 32 bits of `VALUE` |
| `f64` | IEEE 754 double | bit pattern in `VALUE` |

## Numeric operators

### Integer (per type, `T ∈ {i32, i64}`)

| Category | Instructions |
|---|---|
| Constants | `T.const N` |
| Arithmetic | `T.add`, `T.sub`, `T.mul`, `T.div_s`, `T.div_u`, `T.rem_s`, `T.rem_u` |
| Bitwise   | `T.and`, `T.or`, `T.xor`, `T.shl`, `T.shr_s`, `T.shr_u`, `T.rotl`, `T.rotr` |
| Comparison | `T.eq`, `T.ne`, `T.lt_s`, `T.lt_u`, `T.le_s`, `T.le_u`, `T.gt_s`, `T.gt_u`, `T.ge_s`, `T.ge_u` (result is i32) |
| Test      | `T.eqz` (result is i32) |
| Counts    | `T.clz`, `T.ctz`, `T.popcnt` |

`div_s` / `rem_s` trap on divide-by-zero or `INT_MIN / -1`; `div_u` /
`rem_u` trap on divide-by-zero only.  Shifts mask the count by 31 / 63
per spec.

### Float (per type, `T ∈ {f32, f64}`)

| Category | Instructions |
|---|---|
| Constants | `T.const X` |
| Arithmetic | `T.add`, `T.sub`, `T.mul`, `T.div`, `T.min`, `T.max`, `T.copysign` |
| Comparison | `T.eq`, `T.ne`, `T.lt`, `T.le`, `T.gt`, `T.ge` (result is i32) |
| Unary     | `T.abs`, `T.neg`, `T.sqrt`, `T.ceil`, `T.floor`, `T.trunc`, `T.nearest` |

`min` / `max` propagate NaN per wasm spec.

## Conversions

| Group | Instructions |
|---|---|
| Wrap / extend | `i32.wrap_i64`, `i64.extend_i32_s`, `i64.extend_i32_u`, `i32.extend8_s`, `i32.extend16_s`, `i64.extend8_s`, `i64.extend16_s`, `i64.extend32_s` |
| Float → int (trapping) | `i32.trunc_f32_s/u`, `i32.trunc_f64_s/u`, `i64.trunc_f32_s/u`, `i64.trunc_f64_s/u` |
| Int → float | `f32.convert_i32_s/u`, `f32.convert_i64_s/u`, `f64.convert_i32_s/u`, `f64.convert_i64_s/u` |
| Float ↔ float | `f32.demote_f64`, `f64.promote_f32` |
| Reinterpret | `i32.reinterpret_f32`, `i64.reinterpret_f64`, `f32.reinterpret_i32`, `f64.reinterpret_i64` |
| Saturating trunc | `i32.trunc_sat_f32_s/u`, `i32.trunc_sat_f64_s/u`, `i64.trunc_sat_f32_s/u`, `i64.trunc_sat_f64_s/u` |

`*.trunc_f*_*` traps on NaN, infinity, or out-of-range.
`*.trunc_sat_*` saturates instead of trapping (NaN→0, < min→min, > max→max).

## Locals & globals

- `(param $x T)` and `(param T T ...)` (anonymous, multi).
- `(local $y T)` and `(local T T ...)`.
- `local.get $name | N`, `local.set $name | N <expr>`.
- `(global $name (mut)? T (T.const N))` — module-level globals,
  initialized by a constant expression (`*.const` only for now).
- `global.get $name | N`, `global.set $name | N <expr>` (mut validated).

## Control flow

- `(if (result T)? <cond> (then <expr>+) (else <expr>+)?)` — typed
  if/else; `cond` must be `i32`.  `else` optional (then implicit void).
- `(block $L? (result T)? <expr>+)` — labelled scope.  `br N`/`br $L`
  exits past it.  Carrying value supported.
- `(loop $L? (result T)? <expr>+)` — labelled scope.  `br N`/`br $L`
  re-enters the loop.  Falling through exits normally.
- `(br $L | N (<value>)?)` — branch with optional carried value.
- `(br_if $L | N <cond>)` and `(br_if $L | N <value> <cond>)`.
- `(br_table $L0 $L1 ... $Ldefault (<value>)? <idx>)` — computed
  branch into a table of labels.
- `(return)` and `(return <value>)` — early function exit, propagated
  via the `WASTRO_BR_RETURN` sentinel and consumed at the function
  boundary.
- `(unreachable)` — trap.
- `(nop)` — no-op.
- Multi-statement function bodies — folded into right-leaning
  `node_seq` trees.  `seq` short-circuits if a branch is in flight.

All implemented via CTX-side branch state (`c->br_depth`, `c->br_value`),
no `setjmp`.

## Functions

- `(func $name (export "...")? (param ...)* (result T)? (local ...)* <body>)`
- Direct `(call $name | N <args>...)`.
- Fixed-arity call nodes `node_call_0` .. `node_call_4` for
  specializer-friendly inlining of arg evaluation.  Higher arities
  raise a parse error.

## Linear memory

- `(memory $name? <min-pages> <max-pages>?)` — single linear memory
  declaration.  Memory is `min × 64KB`, growable up to `max`.
- Loads (with optional `offset=N` / `align=N` immediates):
  - `i32.load`, `i32.load8_s/u`, `i32.load16_s/u`
  - `i64.load`, `i64.load8_s/u`, `i64.load16_s/u`, `i64.load32_s/u`
  - `f32.load`, `f64.load`
- Stores (mirror of loads, no sign variants):
  - `i32.store`, `i32.store8`, `i32.store16`
  - `i64.store`, `i64.store8`, `i64.store16`, `i64.store32`
  - `f32.store`, `f64.store`
- `memory.size` — current page count.
- `memory.grow <delta>` — grow by `delta` pages, returns previous
  size or `-1` on failure.
- Bounds-check trap (`out of bounds memory access`) on every access.

`(data ...)` static memory initialization is supported in three forms:

- `(data (offset (i32.const N)) "bytes...")`
- `(data (i32.const N) "bytes...")` (sugar)
- `(data N "bytes...")` (legacy bare offset)

String operands accept `\n`, `\t`, `\r`, `\\`, `\'`, `\"`, `\0`, and
`\xx` (hex byte) escapes.  Multiple consecutive string literals are
concatenated.

## Imports (host functions)

- `(import "module" "field" (func $name? (param T)* (result T)?))` —
  declares an imported function.  The `(func ...)` shape after
  `(import "mod" "field" ...)` is parsed but ignored: signatures come
  from the host registry.
- Built-in `env.*` host registry (no command-line bindings yet):

  | Import | Signature | Effect |
  |---|---|---|
  | `env.log_i32`     | `(i32) → ()`        | `printf("%d\n", x)` |
  | `env.log_i64`     | `(i64) → ()`        | `printf("%lld\n", x)` |
  | `env.log_f32`     | `(f32) → ()`        | `printf("%g\n", x)` |
  | `env.log_f64`     | `(f64) → ()`        | `printf("%g\n", x)` |
  | `env.putchar`     | `(i32) → ()`        | `putchar(x & 0xFF)` |
  | `env.print_bytes` | `(i32 ptr, i32 len) → ()` | `fwrite(memory+ptr, 1, len, stdout)`; bounds-checked |

  Unknown `(import ...)` keys raise a parse error listing the registry.

  Imports use a separate `node_host_call_N` family (N = 0..3) so that
  the dispatcher does direct C function-pointer calls without setting
  up a wasm frame.

## Module form

- `(module (import ...)* (memory ...)? (global ...)* (data ...)* (func ...)*)`

## Validation (parser-side, AST-by-construction)

- Every operator validates operand types at parse time.
- `local.get` / `local.set` / `global.get` / `global.set` resolve
  types from their respective environments.
- `if`, `block`, `loop`, `br`, `br_if`, `br_table` validate carried
  / branch / branch-target types.
- `call` validates argument count and per-argument types.
- Function body's last expression checked against `(result T)`.
- Mutability check on `global.set` (immutable globals raise an error).

## Execution / code-store

- AST-walking interpreter generated from `node.def` by ASTroGen.
- `astro_cs_compile` + `astro_cs_build` + `astro_cs_reload` flow:
  per-function specialized C is generated, linked into
  `code_store/all.so`, and `dlopen`'d.
- Specialized dispatcher swap via `OPTIMIZE` called from
  `ALLOC_node_xxx` — code-store is consulted at allocation time and
  matching `SD_<hash>` symbols replace the default dispatcher.
- Fixed-arity `node_call_N` exposes argument sub-trees as declared
  `NODE *` children, so the specializer can pass dispatcher pointers
  via `EVAL_ARG` and gcc inlines through the call.
- `EVAL` is `static inline` so specialized SD\_ functions in `all.so`
  do not pay a PLT call back into the host on each dispatch.
- Host binary linked with `-rdynamic` so that globals like
  `WASTRO_FUNCS`, `WASTRO_GLOBALS`, `WASTRO_BR_TABLE` resolve when
  `all.so` is `dlopen`'d.

## Traps (run-time)

| Source | Message |
|---|---|
| `i32.div_s` / `i64.div_s`, `INT_MIN / -1` | `integer overflow` |
| `*.div_*`, `*.rem_*` (zero divisor) | `integer divide by zero` |
| `*.trunc_*` of NaN | `invalid conversion to integer` |
| `*.trunc_*` out-of-range | `integer overflow` |
| Memory out-of-bounds | `out of bounds memory access` |
| `unreachable` | `unreachable` |

All traps print `wastro: trap: <message>` to stderr and `exit(1)`.

## CLI flags (`./wastro`)

- `--no-compile` — disable code-store consultation (pure interpreter).
- `-c` / `--aot-compile-first` — AOT compile every function before run.
- `--aot` / `--aot-compile` — compile only, do not run.
- `--clear-cs` / `--ccs` — wipe `code_store/` before starting.
- `-v` / `--verbose` — trace `cs_compile` / `cs_build` / `cs_load`.
- `-q` / `--quiet` — suppress code-store hit/miss messages.

## WAT front-end

- Tokenizer: parens, `$identifiers`, keywords (with `.`), integer and
  floating literals, double-quoted strings, `;;` line and `(;...;)`
  block comments, `offset=N` / `align=N` keyword-attribute style.
- Folded S-expression form only.  Stack-style WAT must be folded with
  `wabt`'s `wat-desugar -f` before being fed to wastro.

## Performance (typical Linux x86_64)

### `fib(34)` — recursive, call-overhead dominated

| Mode | Time | vs native |
|---|---|---|
| `--no-compile` (interpreter) | ~158 ms | 18× |
| Cached (specialized) | ~54 ms | **6×** |
| `gcc -O2 fib.c` | ~9 ms | 1× |

### `sum(1_000_000)` — iterative i32 loop

| Mode | Time | vs native |
|---|---|---|
| `--no-compile` (interpreter) | ~17 ms | 17× |
| Cached (specialized) | ~3 ms | **3×** |
| `gcc -O2 sum.c` | ~1 ms | 1× |

### `sieve(1_000_000)` — iterative + linear memory

| Mode | Time | vs native |
|---|---|---|
| `--no-compile` (interpreter) | ~45 ms | 15× |
| Cached (specialized) | ~9 ms | **3×** |
| `gcc -O2 sieve.c` | ~3 ms | 1× |

## Examples shipped (`examples/`)

| File | Demonstrates |
|---|---|
| `fib.wat`             | i32 recursion, `if`, `call_1` |
| `fib_i64.wat`         | i64 arithmetic |
| `fib_f64.wat`         | f64 arithmetic |
| `tak.wat`             | i32 with 3-arg recursion (`call_3`) |
| `sum.wat`             | `block` + `loop` + `br` / `br_if` (typical structured loop) |
| `sum_loop.wat`        | `loop` + `if` + `br` (idiomatic while-loop shape) |
| `early_return.wat`    | `return` for early function exit |
| `bits.wat`            | `i32.popcnt`, bitwise ops, shifts, `eqz` |
| `global_counter.wat`  | mutable `(global ...)` + `global.get` / `global.set` |
| `memory.wat`          | `(memory ...)`, `i32.store`, `i32.load` round-trip |
| `sieve.wat`           | Sieve of Eratosthenes via i32 memory bytes — primes < N |
| `hello.wat`           | `(import env.print_bytes)` + `(data ...)` — prints `Hello, world!` |
| `log_fib.wat`         | `(import env.log_i32)` — prints fib(0..n-1) iteratively |
| `sat.wat`             | `i32.trunc_sat_f64_s` clamping behavior (NaN→0, ±∞ → INT_MIN/MAX) |
