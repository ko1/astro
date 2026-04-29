# luastro — gaps and future work

This is what's NOT done.  The implemented surface is in
[`done.md`](done.md); the runtime architecture is in
[`runtime.md`](runtime.md).

## Performance — primary blockers

| Item | Current state | Why it matters |
|---|---|---|
| **Inline flonum** | Reserved `010` tag; `LUAV_FLOAT(d)` always heap-boxes via `LuaHeapDouble` | `mandelbrot`, `nbody` run ~10–15× slower than `lua5.4`.  CRuby-style rotate-by-3 encoding for the common-magnitude range (with `LuaHeapDouble` fallback for denormals) is the obvious fix. |
| **2-value RESULT (`rax+rdx`)** | `RESULT` is a typedef for `LuaValue`; control flow goes through `LUASTRO_BR` / `LUASTRO_BR_VAL` globals | Theoretical perf win in tight loops — the global memory load/store on every iteration becomes a register check.  Earlier draft was abandoned mid-port; a full re-port is needed. |
| **Type-speculating SDs** | Every SD inherits the `if(LV_IS_INT(a) && LV_IS_INT(b))` guard from the source EVAL | At AOT bake time we have profile info on which type a node sees; emitting a guard-free SD with a deopt path would let `int_add` SDs become a single `add` instruction.  Closes much of the gap to LuaJIT's interpreter mode. |
| **Float-only fused loops** | Only `node_numfor_int_sum` exists | A `node_numfor_flt_sum` (and more generally, a way to detect type-stable accumulator loops at parse time and emit scalar bodies) would help float-heavy benchmarks even before inline flonum lands. |
| **Header-inline value helpers** | `luav_from_double` / `luav_to_double` are out-of-line in `lua_runtime.c` | Making them `static inline` (in `context.h`) lets gcc SROA the heap-box away inside SDs when the round-trip can be elided.  Cheap to do. |

## Lua 5.4 features not implemented / partial

| Feature | Status |
|---|---|
| `goto` / `::label::` | Parsed; `BR_GOTO` propagates up the EVAL chain but **no label scanner** binds `goto` to its target.  Most Lua programs don't use cross-block goto, so this rarely surfaces. |
| `coroutine.wrap` | Raises an error — use `coroutine.resume` directly. |
| `xpcall` message handler | Error is caught but the handler arg is ignored. |
| `<const>` / `<close>` local attribs | Parsed but not enforced (no const-write check, no close-on-scope-exit). |
| `do...end`-scoped local cleanup | Locals declared inside a `do…end` block stay reachable until the enclosing function returns.  GC-conservative, not unsound. |
| Integer-for step semantics | We coerce step to int when start/limit/step all fit; mixed cases use float-for.  Edge cases around `math.mininteger` overflow are not exhaustively handled. |
| `string.format` | Implements common specifiers (`%d %i %u %o %x %X %f %g %e %s %c %%`, width / precision / `-` / `+` / `#` / `0` flags).  Missing: `%q`, `%a` / `%A`. |
| `string.pack` / `string.unpack` / `string.packsize` | Not implemented. |
| `utf8.*` | Not implemented. |
| `debug.*` | Not implemented. |
| `io.*` (other than `io.write`) | No `io.read`, `io.open`, file handles, etc. |
| `os.date` / `os.difftime` / `os.tmpname` | Not implemented. |
| `require` / `package` | Not implemented (single-file programs only). |
| Long strings with embedded `]=]` | Tokenizer handles a single level; nesting works but pathologically deep nesting (`[==========[`) is untested. |
| Bitwise op overflow on shift counts | `1 << 64` yields 0; matches Lua 5.4 spec but only sparsely tested. |

## Standard library inventory gaps

Items present in Lua 5.4 reference manual but absent here:

- **`math`**: `math.atan(y, x)` two-arg form, `math.deg`, `math.rad`,
  `math.ult`, `math.randomseed`, `math.huge` is present but
  `math.maxinteger == 2^63-1` precision around `tointeger` boundary
  cases not exhaustively tested.
- **`string`**: `string.pack`, `string.unpack`, `string.packsize`,
  `%q` and `%a` format specifiers.
- **`table`**: `table.move`, `table.sort`.
- **`io`**: everything except `write`.
- **`os`**: `date`, `difftime`, `tmpname`, `setlocale`,
  `os.execute` returns differently from spec.
- **`debug`**: entire library.
- **`utf8`**: entire library.
- **`package` / `require` / `loadfile` / `dofile` / `load`**: not
  wired up.

## Correctness — known wrinkles

| Item | Notes |
|---|---|
| `__metatable` field | Not honoured by `getmetatable` (which always returns the actual metatable). |
| `__name` field | Not used in error messages. |
| `__pairs` metamethod | Not invoked by `pairs(t)`; we always iterate raw. |
| Numeric overflow in `//` and `%` | Integer cases follow Lua semantics; float cases propagate ±inf / NaN.  No special handling for `INT_MIN // -1` (treats as int_min, not raise). |
| Coroutine error-stack | Errors thrown inside a coroutine produce `(false, msg)` but lose the source-position hint Lua adds. |

## Testing gaps

- `test/test_*.lua` covers core arithmetic / control flow / strings /
  tables / pcall / functions / math / compare.  Missing:
  coroutines, weak tables, GC cycles, `__gc`, multi-assign with call
  spread, `string.format` edge cases.
- No fuzz / random-program tester.
- AOT path is exercised by `make bench` but not by the test suite.

## Tooling

| Item | Notes |
|---|---|
| `--dump-ast` | Implemented but output is for the AOT C-source emit format, not human-friendly.  A `naruby`-style indented dumper would help. |
| `--dump-c` | Not present.  Would be useful for debugging SD generation. |
| Source-position tracking | Tokenizer tracks line; parser threads it into `head.line` (reserved) but EVAL doesn't surface it in error messages. |
| GC stats | `collectgarbage("count")` returns 0 — not wired up. |
| Disassembly | None.  AOT path emits `code_store/c/SD_*.c` which can be inspected directly. |

## ASTro framework — items that would help luastro

Not luastro work, but limit how fast luastro can run:

- **Speculation + deopt at SD level** — would let SDs assume types and
  hot-swap on miss.  See abruby's `swap_dispatcher` for a similar
  pattern.
- **Cross-SD inlining of small EVAL bodies** — currently each SD is a
  separate `dlsym` symbol; `__attribute__((always_inline))` only
  reaches across the EVAL/DISPATCH boundary within one SD.
- **Profile-driven loop fusion** — generalise `node_numfor_int_sum` to
  any type-stable accumulator pattern, ideally observed at runtime
  rather than parser-time pattern-matched.
