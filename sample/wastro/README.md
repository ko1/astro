# wastro — WebAssembly subset on ASTro

ASTro framework's typed sample: a tiny WebAssembly text-format
(WAT) interpreter that runs as an AST-walking interpreter generated
from `node.def`.

For an exhaustive coverage map see:

- **[`docs/done.md`](docs/done.md)** — what's implemented today
- **[`docs/todo.md`](docs/todo.md)** — what's missing for full wasm 1.0

## v0.8 scope at a glance

Most of MVP wasm 1.0 plus saturating truncation (wasm 2.0):

- **Types**: `i32`, `i64`, `f32`, `f64`. `VALUE` is `uint64_t` raw
  bits, reinterpreted at use sites via `AS_*`/`FROM_*` helpers in
  `context.h`. No SIMD, no GC types.
- **Numeric ops**: full set of arithmetic, bitwise, shifts, rotations,
  comparisons, and unary ops (`eqz`, `clz`, `ctz`, `popcnt`, `abs`,
  `neg`, `sqrt`, `floor`, `ceil`, `trunc`, `nearest`, etc.) per type.
- **Conversions**: `wrap`, `extend` (signed/unsigned, 8/16/32-bit),
  trapping and saturating `trunc`, `convert`, `demote`/`promote`,
  `reinterpret`.
- **Locals & globals**: typed `(param ...)`, `(local ...)`,
  `(global $g (mut)? T (T.const N))`. `local.get/set`, `global.get/set`
  with mutability validation.
- **Control flow**: `if/then/else` (typed), `block`, `loop`, `br`,
  `br_if`, `br_table`, `return`, `unreachable`, `nop`. All via
  CTX-side branch state (no `setjmp`).
- **Functions**: direct `(call ...)` with fixed-arity nodes (0..4
  args). No `call_indirect` / tables yet.
- **Linear memory**: `(memory N M?)`, `(data ...)` static init,
  full `*.load*` / `*.store*` (all width / sign variants),
  `memory.size`, `memory.grow`, bounds-check trap.
- **Imports**: `(import "module" "field" (func ...))` with a built-in
  `env.*` host registry — `log_i32/i64/f32/f64`, `putchar`,
  `print_bytes`. Unknown imports raise a parse error.
- **Front-end**: folded S-expression WAT only. Stack-style WAT must
  be folded with `wabt`'s `wat-desugar -f` before being fed to wastro.

For the full feature inventory see [`docs/done.md`](docs/done.md);
remaining gaps (tables/`call_indirect`, bulk memory ops, multi-result
blocks, binary `.wasm` decoder, ...) are listed in
[`docs/todo.md`](docs/todo.md).

## Build

```sh
make
```

The build invokes ASTroGen (`ruby ../../lib/astrogen.rb`) to generate
the dispatcher / hash / specialize / etc. C files from `node.def`,
then compiles `main.c` + `node.c` with `-O3`.

## Usage

```sh
./wastro [options] <module.wat> [<export-name> [arg ...]]

  -q, --quiet         suppress code-store miss/hit messages
  -v, --verbose       trace cs_compile/build/load steps
  --no-compile        disable code-store consultation entirely
  -c                  AOT-compile all functions before running
  --aot               AOT-compile only, then exit (no <export> needed)
  --clear-cs          delete code_store/ before starting
```

Examples:

```sh
# plain interpreter, no code store
./wastro --no-compile -q examples/fib.wat fib 30
# 832040

# AOT-compile only — produces code_store/all.so
./wastro --aot -v examples/fib.wat
# cs_compile: $fib
# cs_build

# AOT-compile and run in one shot
./wastro -c -q examples/fib.wat fib 30
# 832040

# Second run picks up the cached all.so automatically
./wastro -q examples/fib.wat fib 30
# 832040
```

Without `--no-compile`, ASTro's code store is consulted on each
sub-AST during allocation (`OPTIMIZE` is called from
`ALLOC_node_xxx`).  Each node's Merkle hash (`Horg`) becomes the
`SD_<hash>` symbol name in `code_store/all.so`; loading swaps the
node's dispatcher to the specialized version.

For fib's body, the entire `if`/`else` tree (the function's body
node) becomes a single `SD_<hash>` function in `all.so`.  Recursive
`call $fib` goes through the default `node_call` dispatcher (which
is `@noinline`), then re-EVALs the body — and that body's
dispatcher is the specialized one.

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
├── node.def          ASTro node definitions (~190 nodes)
├── node.h            NodeHead struct + extern decls + HOPT/HORG macros
├── node.c            INIT, OPTIMIZE; includes generated *.c
├── context.h         CTX, wastro_function, wtype_t, AS_*/FROM_* helpers
├── main.c            WAT tokenizer + parser + driver + host registry
├── docs/
│   ├── done.md       full inventory of implemented features
│   └── todo.md       remaining gaps vs. wasm 1.0
└── examples/         14 .wat sample programs
```

## Design notes

### Fixed-arity `node_call_N` / `node_host_call_N`

Wasm `call` takes a variable number of operands.  ASTro's
`NODE_DEF` produces a fixed-shape struct per node, so variable-arity
args cannot live as `NODE *` fields directly.  We provide
`node_call_0` .. `node_call_4` (and `node_host_call_0` .. `_3` for
imports), and the parser dispatches to the right arity by argc.
Args become declared `NODE *` children, which lets ASTro's
specializer inline them via `EVAL_ARG`.

### Frames / locals

`CTX` holds a fixed `VALUE stack[]`.  `c->fp` is the current
frame's locals base, `c->sp` is the top of stack (= next frame's
`fp`).  `node_call_N` allocates a new frame at `c->sp`, evaluates
args into it (so args become locals 0..N-1), zeros out body-only
locals, then `EVAL`s the callee's body.  After the callee returns,
`c->fp` and `c->sp` are restored.

### Branch state

Structured control flow uses `c->br_depth` / `c->br_value`
(`br_depth = 0` = no branch, `br_depth > 0` = branching out N
labels, `WASTRO_BR_RETURN` = function return).  `block` / `loop`
decrement on the way out; `seq` short-circuits.  No `setjmp`.

### Imports

`(import "module" "field" (func ...))` resolves against a built-in
host registry hard-coded in `main.c` (`env.log_*`, `env.putchar`,
`env.print_bytes`).  Imported funcs occupy the same `WASTRO_FUNCS`
table as defined funcs but are dispatched via separate
`node_host_call_N` nodes that call `host_fn` directly.

### Specializer-friendly `EVAL`

`EVAL` is `static inline` in `node.h` so that specialized SD\_
functions in `code_store/all.so` do not pay a PLT call back into
the host binary on each dispatch.  The host binary is linked with
`-rdynamic` so that globals (`WASTRO_FUNCS`, `WASTRO_GLOBALS`,
`WASTRO_BR_TABLE`) resolve when `all.so` is `dlopen`'d.

## What's next

The full coverage gap against wasm 1.0 is in [`docs/todo.md`](docs/todo.md).
By rough impact:

1. **Tables + `call_indirect`** — the biggest remaining feature.
   Required by anything emitted from a real source-language compiler
   (Rust, C with vtables, etc.).  ~1 day of work.
2. **Bulk memory ops** — `memory.fill`, `memory.copy`, `memory.init`,
   `data.drop`.  Half a day.
3. **Spec test harness** — run `testsuite/*.wast` against wastro
   for spec compliance.  ~1 day.
4. **Stack-style WAT** — currently only folded form is parsed; the
   spec-test corpus is mostly stack-style.  ~1 day.
5. **Binary `.wasm` decoder** — read `.wasm` directly without going
   through `wasm2wat`.  Larger.

ASTro framework-level extensions that would substantially improve
performance (tracked separately in `todo.md`):

- **Phase 3** — depth-bounded recursive specializer that unrolls N
  levels of recursion into specialized SD\_, eliminating the indirect
  call at the recursion boundary.  Mainly helps call-heavy workloads
  (`fib`).
- **Phase 4** — typed dispatcher signatures so that specialized
  dispatchers receive arguments in registers instead of via `c->fp`.
  Helps loop-heavy workloads (`sum`, `sieve`).
