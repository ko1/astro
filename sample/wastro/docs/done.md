# wastro — implemented features

Snapshot of what works as of v1.0.  See [`todo.md`](todo.md) for what
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

## Locals & globals

- `(param $x T)` and `(param T T ...)` (anonymous, multi).
- `(local $y T)` and `(local T T ...)`.
- `local.get $name | N`, `local.set $name | N <expr>`, `local.tee $name | N <expr>`.
- `(global $name (mut)? T (T.const N))` — module-level globals.
- `global.get $name | N`, `global.set $name | N <expr>` (mut validated).

## Control flow

- `(if (result T)? <cond> (then <expr>+) (else <expr>+)?)` and the
  stack-style `cond / if / ... / else / ... / end` form.
- `(block $L? (result T)? <expr>+)` and `block ... end`.
- `(loop $L? (result T)? <expr>+)` and `loop ... end`.
- `br`, `br_if`, `br_table`, `return`, `unreachable`, `nop`.
- `drop`, `select` (with optional typed `(result T)` annotation).

All implemented via CTX-side branch state (`c->br_depth`, `c->br_value`),
no `setjmp` in hot paths (setjmp is only used by the spec-test harness
to recover from traps).

## Functions

- `(func $name (export "...")? (param ...)* (result T)? (local ...)* <body>)`.
- Direct `(call $name | N <args>...)`.
- `call_indirect (type $sig)` with a single funcref `(table ...)` and
  `(elem ...)` initialization.  Runtime structural type check; traps
  on type mismatch or OOB index.
- Fixed-arity call nodes `node_call_0..4`, `node_call_indirect_0..4`,
  `node_host_call_0..3` for specializer-friendly inlining.

## Tables

- `(table N M? funcref)` — single funcref table per wasm 1.0.
- `(elem (offset (i32.const N)) $f0 $f1 ...)`, `(elem (i32.const N) ...)`,
  `(elem N ...)`, `(elem ... (ref.func $f) ...)`.
- Table is indexed via `WASTRO_TABLE[]`; uninitialized slots trap as
  `undefined element`.

## Linear memory

- `(memory $name? <min-pages> <max-pages>?)`.
- Loads / stores: `i32.load`, `i32.load8_s/u`, `i32.load16_s/u`,
  `i64.load`, `i64.load8_s/u`, `i64.load16_s/u`, `i64.load32_s/u`,
  `f32.load`, `f64.load`, and the matching stores.
- `memory.size`, `memory.grow`, bounds-check trap on OOB.
- `(data ...)` static initialization in the standard forms.

## Imports

- `(import "module" "field" (func ...))` with built-in `env.*` host
  registry (`log_i32/i64/f32/f64`, `putchar`, `print_bytes`).
- `(import "m" "f" (memory N M?))`, `(import ... (global ...))`,
  `(import ... (table ...))` — declarations parsed; memory/table
  imports allocate the local instance, global imports default to 0.
- Imports with no host binding install a stub that traps on call,
  letting modules with placeholder imports load.

## Module form

- `(module
     (type ...)*  (import ...)*  (func ...)*  (table ...)?
     (memory ...)?  (global ...)*  (export ...)*  (start ...)?
     (elem ...)*  (data ...)* )`
- Top-level `(export "name" (func | memory | global | table) ...)` —
  func exports installed; the rest are accepted (single instance).
- `(start $f)` invoked at module instantiation.

## WAT front-end

- Folded S-expression form **and** stack-style (bare-keyword) form,
  freely mixed.  Stack-style is the format used by the spec testsuite
  and by `wasm2wat`.
- Tokenizer: parens, `$identifiers`, `.`-keywords, integer / float
  literals, double-quoted strings (with `\xx` / `\n` / `\t` / etc.
  escapes), `;;` line and `(;...;)` block comments,
  `offset=N` / `align=N` keyword-attribute style.

## Binary `.wasm` decoder

- Magic + version (must be 1).
- Sections: Type, Import, Function, Table, Memory, Global, Export,
  Start, Element, Code, Data, DataCount, Custom (skipped).
- LEB128 (signed / unsigned) decoders.
- Full opcode coverage (0x00–0xC4) plus the 0xFC sub-opcodes for
  saturating truncation.
- Auto-detection by magic in `wastro_load_module` — `.wat` and `.wasm`
  use the same CLI entry.

## Spec test harness (`.wast`)

- `--test foo.wast` runs each top-level form in sequence:
  - `(module ...)` resets state and loads the new module.
  - `(assert_return (invoke "name" args...) result)` — invokes and
    compares value (NaN-aware for floats).
  - `(assert_trap (invoke ...) "msg")` — invokes under setjmp; pass
    if trap fires.
  - `(invoke ...)` — bare invocation.
  - `(register "name")` — accepted (no cross-module link).
  - `assert_invalid` / `assert_malformed` / `assert_exhaustion` /
    `assert_return_canonical_nan` / `assert_return_arithmetic_nan` —
    parsed and reported as skipped.
- Final summary: `<path>: N passed, N failed, N skipped`.

## Validation (parser-side, AST-by-construction)

- Every operator validates operand types.
- `local.get/set/tee` and `global.get/set` resolve types.
- `if`, `block`, `loop`, `br`, `br_if`, `br_table`, `return` validate
  carried / branch / branch-target types.
- `call` and `call_indirect` validate argument count and per-argument
  types; `global.set` checks mutability.
- Function body's tail value checked against `(result T)`.

## Execution / code-store

- AST-walking interpreter generated from `node.def` by ASTroGen.
- Same `astro_cs_compile` + `astro_cs_build` + `astro_cs_reload` flow
  as before; `node_call_indirect_N` participates in specialization.
- Test harness (`--test`) bypasses the code store — it runs in pure
  interpreter mode and uses `setjmp`/`longjmp` to recover from traps.

## Traps (run-time)

| Source | Message |
|---|---|
| `*.div_s`, `INT_MIN / -1` | `integer overflow` |
| `*.div_*`, `*.rem_*` (zero divisor) | `integer divide by zero` |
| `*.trunc_*` of NaN | `invalid conversion to integer` |
| `*.trunc_*` out-of-range | `integer overflow` |
| Memory out-of-bounds | `out of bounds memory access` |
| `unreachable` | `unreachable` |
| `call_indirect`, OOB or null slot | `undefined element` / `uninitialized element` |
| `call_indirect`, sig mismatch | `indirect call type mismatch` |
| Imported func with no host binding | `call to unbound host import` |

## CLI flags (`./wastro`)

- `--no-compile` — disable code-store consultation (pure interpreter).
- `-c` / `--aot-compile-first` — AOT compile every function before run.
- `--aot` / `--aot-compile` — compile only, do not run.
- `--clear-cs` / `--ccs` — wipe `code_store/` before starting.
- `-v` / `--verbose` — trace `cs_compile` / `cs_build` / `cs_load`.
- `-q` / `--quiet` — suppress code-store hit/miss messages.
- `--test <foo.wast>` — run a wasm spec-test file.

If the module has a `(start ...)` function it is invoked at
instantiation time, before the user-named export.  If no `<export>`
is given and the module has `(start)`, wastro instantiates and runs
it, then exits.

## Examples shipped (`examples/`)

| File | Demonstrates |
|---|---|
| `fib.wat`             | i32 recursion, `if`, `call_1` |
| `fib_i64.wat`         | i64 arithmetic |
| `fib_f64.wat`         | f64 arithmetic |
| `tak.wat`             | i32 with 3-arg recursion (`call_3`) |
| `sum.wat`             | `block` + `loop` + `br` / `br_if` |
| `sum_loop.wat`        | `loop` + `if` + `br` |
| `early_return.wat`    | `return` for early function exit |
| `bits.wat`            | `i32.popcnt`, bitwise ops, shifts, `eqz` |
| `global_counter.wat`  | mutable `(global ...)` |
| `memory.wat`          | `(memory ...)`, `i32.store`, `i32.load` |
| `sieve.wat`           | Sieve of Eratosthenes |
| `hello.wat`           | `(import env.print_bytes)` + `(data ...)` |
| `log_fib.wat`         | `(import env.log_i32)` |
| `sat.wat`             | `i32.trunc_sat_f64_s` clamping |
| `indirect.wat`        | `(table)` + `(elem)` + `call_indirect` |
| `start.wat`           | `(start $f)` + top-level `(export ...)` |
| `stack_style.wat`     | non-folded WAT (spec-testsuite style) |
| `add.wasm`, `fib.wasm` | minimal hand-encoded `.wasm` binaries |
| `smoke.wast`          | spec-test harness sanity check |
| `control.wast`        | larger spec-test exercise (memory, table, control flow) |
