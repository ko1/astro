# wastro — WebAssembly on ASTro

ASTro framework's typed sample: a WebAssembly 1.0 interpreter that
runs as an AST-walking interpreter generated from `node.def`.  Reads
both `.wat` (text) and `.wasm` (binary) modules, runs the wasm
spec-test `.wast` format, and feeds its tree-walked dispatch through
ASTro's per-node specialization.

For an exhaustive coverage map see:

- **[`docs/done.md`](docs/done.md)** — what's implemented today
- **[`docs/todo.md`](docs/todo.md)** — what's missing for full wasm 1.0+

## v1.0 scope

Full WebAssembly 1.0 (MVP) plus saturating truncation (wasm 2.0):

- **Types**: `i32`, `i64`, `f32`, `f64`. `VALUE` is `uint64_t` raw
  bits, reinterpreted at use sites via `AS_*`/`FROM_*` helpers.
- **Numeric ops**: full set of arithmetic, bitwise, shifts, rotations,
  comparisons, and unary ops per type.
- **Conversions**: `wrap`, `extend` (signed/unsigned, 8/16/32-bit),
  trapping and saturating `trunc`, `convert`, `demote`/`promote`,
  `reinterpret`.
- **Locals & globals**: `local.get/set/tee`, `global.get/set` with
  mutability validation.
- **Control flow**: `if/then/else`, `block`, `loop`, `br`, `br_if`,
  `br_table`, `return`, `unreachable`, `nop`, `drop`, `select`. All
  via CTX-side branch state (no `setjmp` in hot paths).
- **Functions**: direct `(call ...)` (arity 0..4), and indirect
  `call_indirect (type $sig)` via a single funcref `(table ...)` and
  `(elem ...)`. Runtime structural type check; traps on mismatch.
- **Linear memory**: `(memory N M?)`, `(data ...)` static init, full
  `*.load*` / `*.store*` (all width / sign variants), `memory.size`,
  `memory.grow`, bounds-check trap.
- **Imports**: `(import "module" "field" (func | memory | global | table) ...)`
  with a built-in `env.*` host registry — `log_i32/i64/f32/f64`,
  `putchar`, `print_bytes`. Unbound imports install a stub that traps
  if invoked.
- **Module form**: `(type ...)`, `(import ...)`, `(func ...)`,
  `(table ...)`, `(memory ...)`, `(global ...)`, `(export ...)`,
  `(start ...)`, `(elem ...)`, `(data ...)` — accepted in any order.
- **Front-ends**:
  - **WAT** — folded *and* stack-style, freely mixed.
  - **Binary `.wasm`** — full opcode coverage of wasm 1.0.
- **Spec test harness** — `--test foo.wast` runs the standard
  spec-testsuite assertion forms (`assert_return`, `assert_trap`,
  `invoke`, `register`, ...).

## Build

```sh
make
```

The build invokes ASTroGen (`ruby ../../lib/astrogen.rb`) to generate
the dispatcher / hash / specialize / etc. C files from `node.def`,
then compiles `main.c` + `node.c` with `-O3`.

## Usage

```sh
./wastro [options] <module.wat | module.wasm> [<export-name> [arg ...]]
./wastro --test <foo.wast>

  -q, --quiet         suppress code-store miss/hit messages
  -v, --verbose       trace cs_compile/build/load steps
  --no-compile        disable code-store consultation entirely
  -c                  AOT-compile all functions before running
  --aot               AOT-compile only, then exit (no <export> needed)
  --clear-cs          delete code_store/ before starting
  --test <foo.wast>   run a wasm spec-test file
```

If only `<module>` is given and the module has a `(start ...)`
function, wastro instantiates the module and runs `(start)`, then
exits.  Otherwise an `<export-name>` is required.

Examples:

```sh
# WAT, plain interpreter
./wastro --no-compile -q examples/fib.wat fib 30
# 832040

# Stack-style WAT (non-folded)
./wastro -q examples/stack_style.wat fact 6
# 720

# Binary .wasm
./wastro -q examples/fib.wasm fib 20
# 6765

# AOT-compile and run
./wastro -c -q examples/fib.wat fib 30
# 832040

# Spec test harness
./wastro --test examples/control.wast
# examples/control.wast: 16 passed, 0 failed, 0 skipped

# Module with (start) + top-level (export)
./wastro -q examples/start.wat get_counter
# 100
```

Without `--no-compile`, ASTro's code store is consulted on each
sub-AST during allocation (`OPTIMIZE` is called from
`ALLOC_node_xxx`).  Each node's Merkle hash (`Horg`) becomes the
`SD_<hash>` symbol name in `code_store/all.so`; loading swaps the
node's dispatcher to the specialized version.

## Performance

A few representative numbers vs. native gcc -O2 on Linux x86_64.
See `docs/done.md` for the full table.

| Workload (cached SD\_) | wastro | native | ratio |
|---|---|---|---|
| `fib(34)` — recursive | 54 ms | 9 ms | **6×** |
| `sum(1_000_000)` — i32 loop | 3 ms | 1 ms | **3×** |
| `sieve(1_000_000)` — i32 loop + linear memory | 9 ms | 3 ms | **3×** |

The remaining gap to native (largely on call-heavy workloads) is
discussed in `docs/todo.md` under "ASTro framework extensions" —
depth-bounded recursive specialization and typed dispatcher
signatures (Phase 3 / Phase 4) would close it further but require
ASTroGen-side extensions.

## Files

```
wastro/
├── README.md         this file
├── Makefile          build rules + ASTroGen invocation
├── node.def          ASTro node definitions (~210 nodes)
├── node.h            NodeHead struct + extern decls + HOPT/HORG macros
├── node.c            INIT, OPTIMIZE; includes generated *.c
├── context.h         CTX, wastro_function, wtype_t, AS_*/FROM_* helpers
├── main.c            tokenizer + WAT parser + binary decoder + .wast harness + driver
├── docs/
│   ├── done.md       inventory of implemented features
│   └── todo.md       remaining gaps
└── examples/         sample .wat / .wasm / .wast programs
```

## Design notes

### Fixed-arity `node_call_N` / `node_call_indirect_N` / `node_host_call_N`

Wasm `call` takes a variable number of operands.  ASTro's
`NODE_DEF` produces a fixed-shape struct per node, so we provide
arity-specific variants (0..4 for direct/indirect, 0..3 for host
imports), and the parser dispatches by argc.  Args become declared
`NODE *` children, which lets ASTro's specializer inline them via
`EVAL_ARG`.

### Frames / locals

`CTX` holds a fixed `VALUE stack[]`.  `c->fp` is the current
frame's locals base, `c->sp` is the top of stack.  `node_call_N`
allocates a new frame at `c->sp`, evaluates args into it, zeros out
body-only locals, then `EVAL`s the callee's body.  After the callee
returns, `c->fp` and `c->sp` are restored.

### Branch state

Structured control flow uses `c->br_depth` / `c->br_value`.
`block` / `loop` decrement on the way out; `seq` short-circuits.
The function boundary consumes `WASTRO_BR_RETURN`.  No `setjmp` in
hot paths.

### Tables and indirect calls

Single funcref table per wasm 1.0.  `WASTRO_TABLE[]` stores function
indices (or -1 for uninitialized).  `node_call_indirect_N` fetches
the slot, structurally compares the resolved function's signature
against the declared `(type $sig)`, and dispatches.  Imported
functions are dispatched via the host_fn pointer; defined functions
go through the standard frame setup.

### Stack-style WAT

The body parser tracks an operand stack of `TypedExpr`.  Folded
`(...)` sub-expressions push their result onto the same stack, so
folded and stack-style instructions can be freely mixed.  Void
instructions go into a "pending statements" list that's wrapped
around the final stack value as a right-leaning `node_seq` tree.

### Binary `.wasm` decoder

Section-driven decoder using LEB128 / fixed-width readers.  Each
section directly populates the same global state used by the WAT
parser (`WASTRO_TYPES`, `WASTRO_FUNCS`, `WASTRO_GLOBALS`,
`MOD_DATA_SEGS`, `WASTRO_TABLE`).  The Code section parses opcodes
into AST via the same `OpStack` / `StmtList` machinery the WAT
parser uses.  Magic detection in `wastro_load_module` dispatches
WAT vs. binary automatically.

### Spec test harness

`--test foo.wast` parses each top-level form, resets module state
on each `(module ...)`, and runs assertion forms in sequence.
`assert_trap` uses `setjmp` / `longjmp` to recover from `wastro_trap`
calls — which become recoverable when `wastro_trap_active` is set.
Pure-interpreter mode is forced for tests (no AOT specialization
overhead per assertion).

### Specializer-friendly `EVAL`

`EVAL` is `static inline` in `node.h` so that specialized SD\_
functions in `code_store/all.so` do not pay a PLT call back into
the host binary on each dispatch.  The host binary is linked with
`-rdynamic` so that globals (`WASTRO_FUNCS`, `WASTRO_GLOBALS`,
`WASTRO_BR_TABLE`, `WASTRO_TABLE`, `WASTRO_TYPES`) resolve when
`all.so` is `dlopen`'d.

## What's next

The full coverage gap against wasm 1.0 is in [`docs/todo.md`](docs/todo.md).
The biggest known item is stack-style let-floating (rare in
real-world wasm) and `nan:canonical` / `nan:arithmetic` matchers in
the spec-test harness.

Post-1.0 proposals (bulk memory, reference types, multi-value, GC,
SIMD, threads, exceptions, tail calls) are out of scope.

ASTro framework-level extensions that would substantially improve
performance (tracked separately in `todo.md`):

- **Phase 3** — depth-bounded recursive specializer that unrolls N
  levels of recursion into specialized SD\_, eliminating the indirect
  call at the recursion boundary.  Mainly helps call-heavy workloads
  (`fib`).
- **Phase 4** — typed dispatcher signatures so that specialized
  dispatchers receive arguments in registers instead of via `c->fp`.
  Helps loop-heavy workloads (`sum`, `sieve`).
