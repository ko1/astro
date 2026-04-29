# luastro — Lua 5.4 on ASTro

A Lua 5.4 interpreter built on the
[ASTro](../../docs/idea.md) framework.  Hand-written tokenizer +
recursive-descent parser, ASTroGen-driven evaluator, AOT specialization
via the shared code store.

luastro implements most of the Lua 5.4 reference manual surface: full
lexical syntax, full grammar, integer/float subtypes, closures with
captured-local boxing, metatables, the standard library (math /
string / table / io.write / os / coroutine), the full Lua pattern
matcher, weak tables, `__gc` finalizers, and ucontext-based
coroutines.  Multi-file `require` and the debug library are out of
scope.

For the implemented-feature inventory see
[`docs/done.md`](docs/done.md); known gaps are in
[`docs/todo.md`](docs/todo.md); the runtime architecture is in
[`docs/runtime.md`](docs/runtime.md).

## Build

```sh
make            # builds ./luastro (interpreter)
make test       # runs test/run_all.rb (8 test files, all passing)
make bench      # runs benchmark/run.rb (lua5.4 + luajit comparison)
```

### CLI options

| Flag                          | Effect                                                            |
|-------------------------------|-------------------------------------------------------------------|
| (none)                        | Pure interpreter; consults `code_store/` if present               |
| `-c` / `--aot-compile-first`  | Bake SDs into `code_store/`, then run with them active            |
| `--aot-compile`               | Bake SDs and exit (no run); used by bench `setup` / cache priming |
| `-p` / `--pg-compile`         | Run first (collect profile), then bake                            |
| `--no-compile`                | Don't consult / write the code store                              |
| `--dump-ast`                  | Print AST                                                         |
| `-e CODE`                     | Eval string instead of file                                       |

## Layout

```
sample/luastro/
├── Makefile             # build targets
├── lua_gen.rb           # ASTroGen extension (RESULT type, 3-arg prefix,
│                        # LuaString hashing, uint64_t literal emission)
├── node.def             # node definitions consumed by ASTroGen
├── node.h / context.h   # NodeHead / LuaValue / runtime structs / GC API
├── node.c               # main TU for generated dispatchers + glue
├── lua_token.h          # shared token type
├── lua_tokenizer.c      # hand-written Lua 5.4 lexer
├── lua_parser.c         # recursive-descent + Pratt parser, captured-local rewriter
├── lua_runtime.c        # strings (intern), tables (array+hash), closures,
│                        # errors, comparisons, metaop dispatch, fast-path
│                        # closure call, GC integration
├── lua_pattern.c        # Lua pattern matcher (full classes/sets/quantifiers)
├── lua_coroutine.c      # ucontext-based coroutines
├── lua_gc.c             # mark-sweep + weak tables + __gc finalizer
├── lua_stdlib.c         # standard library bindings
├── main.c               # driver; threads the chunk on a 4 GB worker stack
├── docs/
│   ├── runtime.md       # software architecture
│   ├── done.md          # implemented feature inventory
│   └── todo.md          # known gaps and future work
├── test/                # test_*.lua (compared against lua5.4 output)
├── benchmark/           # bm_*.lua + run.rb
└── examples/            # hello.lua, fib.lua
```

## Architecture summary

- **8-byte tagged LuaValue** (`uint64_t`).  Low 3 bits encode the tag:
  `000` = pointer to a heap object, `001` = 63-bit fixnum, `010` =
  inline flonum (reserved, see `todo.md`).  Whole-word singletons:
  `0x00` = nil, `0x14` = false, `0x24` = true.  Heap-object subtype
  read from `GCHead.type` (offset 0 of the object).
- **Captured locals** become heap `LuaBox` cells lazily on first
  access; `frame[]` slots hold a tagged pointer to the box.  Inner
  closures share the same `&box->value` so writes are visible across
  re-entry.
- **Inline closure call**: `luastro_inline_call()` (out-of-line in
  `lua_runtime.c`) bypasses `lua_call`'s metamethod / cfunc dispatch
  on the hot path of recursive Lua-to-Lua calls.
- **AOT via code store**: `-c` emits per-node `code_store/c/SD_<hash>.c`,
  links them into `all.so`, and re-resolves each AST node's dispatcher
  to its specialized SD entry.  The hash is structural, so the cache
  is reusable across runs and across programs that share AST shapes.
- **Branch state via globals**: control flow (`break`, `return`,
  `goto`, `continue`) propagates through the `LUASTRO_BR` /
  `LUASTRO_BR_VAL` globals, set by the corresponding `RESULT_*`
  macros.  An earlier draft folded `br` into the return value
  (`rax+rdx`); see `todo.md`.
- **Inline flonum** for the common magnitude range (b62 ∈ {3, 4},
  ≈ 2^-255..2^256, both signs).  Out-of-range doubles (zero,
  denormals, ±Inf, NaN, very tiny / very large) heap-box as
  `LuaHeapDouble`.  See [`docs/runtime.md`](docs/runtime.md) §3 for
  the encoding details.
- **4 GB worker stack** for the chunk pthread, so deeply recursive
  programs don't trip the kernel guard page (SD frames are ~1 KB).

See [`docs/runtime.md`](docs/runtime.md) for full detail.

## Benchmarks

Wall time, single run on a stable machine, `gcc -O3` build,
Lua 5.4.6 and LuaJIT 2.1, `N=3` (best of 3).

| benchmark   | luastro | AOT-1st | AOT-c   | lua5.4  | luajit  | vs lua5.4   |
|-------------|--------:|--------:|--------:|--------:|--------:|------------:|
| ack         | 0.138s  | 0.182s  | 0.057s  | 0.050s  | 0.008s  | 1.14× behind |
| factorial   | 0.032s  | 0.140s  | 0.010s  | 0.026s  | 0.004s  | **2.6× faster** |
| fib         | 0.065s  | 0.148s  | 0.027s  | 0.051s  | 0.010s  | **1.9× faster** |
| loop        | 0.006s  | 0.091s  | 0.003s  | 0.038s  | 0.009s  | **12.7× faster** |
| mandelbrot  | 0.125s  | 0.440s  | 0.076s  | 0.049s  | 0.005s  | 1.55× behind |
| nbody       | 0.030s  | 0.743s  | 0.033s  | 0.012s  | 0.003s  | 2.75× behind |
| sieve       | 0.008s  | 0.304s  | 0.006s  | 0.007s  | 0.003s  | **tied** |
| tak         | 0.004s  | 0.133s  | 0.004s  | 0.003s  | 0.002s  | tied |

Columns:

- **luastro** — pure interpreter (no `code_store/`).
- **luastro-AOT-1st** — `-c`: bake + run in one process; `code_store/`
  cleared per iteration so timing **includes** gcc compile time.
- **luastro-AOT-c** — bake once in `setup` (not timed), then time
  pure execution against the warmed `all.so`.

### What the numbers say

**Integer-dominant** workloads (`loop`, `factorial`, `fib`,
`tak`, `ack`): with AOT-c, luastro is **at parity or faster than
lua5.4**.

- `loop`: 3 ms vs 39 ms — **13× faster** than lua5.4.  This benchmark
  hits the `node_numfor_int_sum` whole-loop fused node.
- `factorial`: 9 ms vs 24 ms — **2.7× faster**.
- `fib`: 26 ms vs 46 ms — **1.8× faster** via SD inlining.
- `ack`: 55 ms vs 46 ms — 1.2× behind (within noise).
- `tak`: 4 ms vs 3 ms — matched.

**Float-dominant** workloads (`mandelbrot`, `nbody`): a sequence of
five changes landed against the always-heap-box baseline.  Cumulative
effect on `mandelbrot` AOT-c:

| Step                                              | mandelbrot AOT-c |
|---------------------------------------------------|-----------------:|
| Baseline (16-byte LuaValue, every double on heap) |           737 ms |
| 8-byte LuaValue with heap-boxed doubles           |           126 ms |
| **+ inline flonum** (b62 ∈ {3, 4} doubles inline) |           108 ms |
| **+ mixed int+float in arith / compare**          |           108 ms |
| **+ pinned +0.0 cell**                            |            88 ms |
| **+ `node_local_decl` 1-LHS/1-RHS fast path**     |            81 ms |
| **+ `always_inline` on `luav_to_double` / `_from_double`** |   73 ms |
| **+ pre-interned metamethod names**               |            76 ms |

mandelbrot AOT-c: 76 ms vs lua5.4 49 ms — **1.55× behind** (was 15.7×).
nbody AOT-c: 33 ms vs lua5.4 12 ms — **2.75× behind** (was 9.3×).

The remaining gap is mostly per-node `DISPATCH_*` indirect calls in
the AOT-cached SD's outermost handler (children that didn't get baked
SDs at compile time fall back to runtime dispatch).  Type-speculating
SDs that drop the per-call guard are the next item; see
[`docs/todo.md`](docs/todo.md).

**vs LuaJIT**: not competitive on any benchmark (LuaJIT is a tracing
JIT to native code).  Beating LuaJIT in pure speed would require a
similar tracing JIT layer; out of scope.

## Status

| Layer            | Coverage                                                                        |
|------------------|---------------------------------------------------------------------------------|
| Tokenizer        | Full Lua 5.4 lexical syntax (keywords, ints/floats, hex/binary, long strings)   |
| Parser           | Full Lua 5.4 grammar — statements, expressions, table constructors, generic-for |
| Runtime          | nil / bool / int / float / string / table / closure / cfunction / thread / box |
| Tables           | Array+hash hybrid, metatables (`__index` / `__add` / `__lt` / `__call` / ...)   |
| Functions        | Closures with upvalues (auto-promoted heap cells), multi-return spread          |
| Coroutines       | `coroutine.create/resume/yield/status/isyieldable/running` (ucontext-backed)    |
| Errors           | `error` / `pcall` / `xpcall` (handler ignored), assert                          |
| GC               | Stop-the-world mark-sweep, weak tables (`__mode = "k"/"v"/"kv"`), `__gc` finalizer |
| Standard library | base, `math.*`, `string.*` (full pattern matcher), `table.*`, `io.write`, `os.*`, `coroutine.*` |
| Specialization   | `node_int_*` / `node_flt_*` / `node_call_argN` / `node_numfor_int_sum`          |
| AOT              | `astro_cs_compile` + `astro_cs_build`; `make compiled_luastro`, `make pg_luastro` |
| Tests            | 8/8 passing (compared byte-for-byte against `lua5.4`)                           |

See [`docs/done.md`](docs/done.md) for the detailed inventory and
[`docs/todo.md`](docs/todo.md) for the gaps.
