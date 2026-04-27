# wastro — what's missing for a full WebAssembly 1.0 interpreter

What is implemented today is in [`done.md`](done.md).  v0.8 covers
nearly all of MVP wasm 1.0 in functional form: the full numeric and
control-flow surface, linear memory with `(data ...)` initialization,
globals, imports (with a small `env.*` registry), and saturating
truncation (wasm 2.0 carry-over).

The remaining gaps below are listed in roughly the order they would
matter for running real-world `.wasm` modules.

## Tables and indirect calls

| Missing | Notes |
|---|---|
| `(table N M? funcref)` | Function table declaration. |
| `(elem ...)` | Initializes table with function references. |
| `call_indirect (type $sig)` | Indirect call with type-sig check; trap on mismatch. |
| `table.size`, `table.grow`, `table.fill`, `table.copy`, `table.init`, `table.get`, `table.set`, `elem.drop` | Table ops. |
| `ref.func`, `ref.null`, `ref.is_null` | Reference types. |

This is the next big chunk and is needed by anything emitted from a
real source language compiler (Rust, C with vtables, etc.).

## Imports — secondary

| Missing | Notes |
|---|---|
| Command-line / programmatic import binding | Currently the host registry is hardcoded.  A flag or callback to register custom host functions (e.g. `--import env.foo=./libfoo.so:foo`) would let users wire arbitrary C symbols. |
| `(import "env" "memory" (memory N))` | Imported memory. |
| `(import ... (global ...))` | Imported global. |
| `(import ... (table ...))` | Imported table. |
| Top-level `(export ...)` form | Currently only inline `(export "name")` on `(func ...)` is parsed. |

## Memory — bulk operations

| Missing | Notes |
|---|---|
| `memory.fill`, `memory.copy`, `memory.init`, `data.drop` | Bulk memory ops (post-1.0 / threads proposal). |
| Passive data segments (`(data $name "bytes")` with `memory.init`) | Companion to bulk ops. |

## Control flow — leftovers

| Missing | Notes |
|---|---|
| Multi-result blocks (`(result T1 T2 ...)`) | Wasm multi-value extension. The current parser only accepts a single `(result T)`. |
| Stricter typing on `if` without explicit result | Today a result-less `if` defaults to void unless both branches happen to produce the same non-void type.  Tighten when this matters. |

## Reference types and other proposals (out of scope)

Tracked here to make explicit they are not aimed at:

- **GC types** (`(struct ...)` / `(array ...)` / `(ref ...)` etc.).
- **SIMD** `v128` and the entire 128-bit operation set.
- **Threads / atomics** (`atomic.add`, shared memory).
- **Exception handling** (`try` / `catch` / `throw` / `delegate`).
- **Tail calls** (`return_call`, `return_call_indirect`).
- **Multi-memory** (multiple linear memories per module).

## Tooling and front-end

| Missing | Notes |
|---|---|
| Binary `.wasm` decoder | Currently only WAT text is read.  Pipe through `wabt`'s `wasm2wat` for now. |
| Stack-style WAT (non-folded) | The parser only accepts folded S-expressions.  Either fold via `wabt`'s `wat-desugar -f` or add an operand-stack tracker to the parser. |
| Spec-test harness | The wasm spec `testsuite/*.wast` — load `(assert_return ...)` / `(assert_trap ...)` and run against wastro. |
| Better error messages | Line / column info on parse errors (today only the offending token text is reported). |
| `--dump` / `--disasm` | Show the AST or generated specialized C.  abruby has both. |

## ASTro framework extensions (out of scope for wastro itself)

These touch ASTro's framework (`lib/astrogen.rb` and the runtime).
Not wastro work, but they cap how fast wastro can run on call-heavy
workloads (`fib`, recursive code).  Iterative workloads (`sum`,
`sieve`) are already within 3× of native and not bottlenecked by
these.

| Missing | Notes |
|---|---|
| `node_call_N` body as `@noinline_child` | Embed body NODE pointer in the call node, but tell the specializer not to recursively inline through it.  Phase 2 / prerequisite for Phase 3. |
| Depth-bounded recursive specializer | `lib/astrogen.rb` build_specialize would unroll recursive calls N levels.  Phase 3 in the wastro README. |
| Typed dispatcher signatures | Allow specialized dispatchers to take typed args (register passing).  Phase 4. |

## Smaller cleanups specific to wastro

| Item | Notes |
|---|---|
| `local.set` returning `WT_VOID` cleanly | Today `local.set` is allowed in expression position; in real wasm it is a statement.  Distinguish "value-producing" vs "void" expressions cleanly. |
| Better diagnostics | Track source line / column in nodes (`head.line` reserved but unused). |
| Higher-arity `node_call_N` / `node_host_call_N` | Add `_5` through `_8` if any module needs them (parser already errors out cleanly). |
| Custom hash for `node_call_N` | Today the node hash combines `func_index` / `local_cnt` / arg children, which suffices for distinct call sites; verify there are no hash collisions across modules with the same shape. |
