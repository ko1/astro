# luastro — gaps and future work

This is what's NOT done.  The implemented surface is in
[`done.md`](done.md); the runtime architecture is in
[`runtime.md`](runtime.md).

## Performance — primary blockers

| Item | Current state | Why it matters |
|---|---|---|
| ~~**Inline flonum**~~ | **Done.**  Shift-based scheme (`bits 4..63 = orig bits 0..59`, `bit 3 = b62=3 vs 4 disc`, `bit 2 = sign`, `bits 0..1 = 10` tag) covers `b62 ∈ {3, 4}` (magnitudes 2^-255..2^256, both signs) inline, lossless.  Out-of-range doubles still heap-box. |
| ~~**Mixed int+float in arith / compare**~~ | **Done.**  `node_int_add/_sub/_mul` and `node_lt/_le` now handle int+float / float+int directly via promote-to-double, without falling through to `lua_arith` / `lua_lt`.  `2 * x` and `x*x + y*y < 4` no longer go through the slow path. |
| ~~**Pinned +0.0 cell**~~ | **Done.**  A single shared `LuaHeapDouble` represents `0.0` instead of `calloc`-ing a fresh cell on every accumulator-hits-zero / loop-init.  Mandelbrot AOT-c: 108 → 88 ms (-19%). |
| ~~**`node_local_decl` 1-LHS/1-RHS fast path**~~ | **Done.**  The common `local x = expr` form now skips the `staged[]` array and `ret_info` plumbing entirely.  Mandelbrot 88 → 81 ms (-8%). |
| ~~**`always_inline` on `luav_to_double` / `luav_from_double`**~~ | **Done.**  Without `always_inline`, gcc emitted out-of-line copies of these helpers and called them via the symbol; with it, the encode/decode is fused into every arith SD's hot path.  Mandelbrot 81 → 73 ms (-10%). |
| ~~**Lazy array-part promotion for sparse-start integer keys**~~ | **Done.**  `lua_table_seti` now accepts ≤ 2× arr_cap + 4 as "modestly close" and routes to the array part instead of dropping every entry into the open-addressing hash.  This unblocks the common `for i=2,N do t[i] = ... end` pattern (sieve, etc.) that previously bypassed the array entirely because index 1 was never set.  Sieve AOT-c: 12 → 6 ms — **tied with lua5.4**. |
| ~~**Pre-interned metamethod / library names**~~ | **Done.**  `__index`, `__newindex`, `__call`, `__add`, ... `__gc`, `string` are interned once at `luastro_init_globals` and stored in `LUASTRO_S_*` globals.  Hot-path metatable touches no longer call `lua_str_intern("__index")` per access.  Modest impact on the bundled benchmarks (no metatable use) but removes the surprise factor for code that does. |
| ~~**Inner SDs externally visible via wrappers**~~ | **Done.**  `luastro_specialize_all` now post-processes each generated `code_store/c/SD_<hash>.c`: every `SD_<hash>` reference inside the file is renamed to `SD_<hash>_INL` (so the in-source function-pointer chain still inlines through `static inline`) and a tiny `__attribute__((weak)) RESULT SD_<hash>(...) { return SD_<hash>_INL(...); }` wrapper is appended for each.  `astro_cs_load`'s `dlsym(SD_<hash>)` now finds the wrapper for **every** node, so `cs hit` jumps from 2 → ~80 per mandelbrot run; the prior fallback to the host binary's `DISPATCH_*` collapses.  Mandelbrot AOT-c: 76 → 65 ms (-14%). |
| ~~**Variadic-child SD baking (`LUASTRO_NODE_ARR` walk)**~~ | **Done.**  `@noinline` nodes (`node_local_decl`, multi-arg calls, table constructors) hide their children inside `LUASTRO_NODE_ARR` (the parser's side array), and ASTroGen's specializer walks only typed `NODE *` operands so those children stayed unbaked.  `luastro_specialize_all` now also calls `astro_cs_compile` on each entry of `LUASTRO_NODE_ARR` so every node ends up addressable by `dlsym`.  cs-hit rate goes from ~80% → ~95% on mandelbrot.  Mandelbrot AOT-c: 65 → 60 ms (-8%). |
| ~~**`node_local_decl_one` for `local x = expr`**~~ | **Done.**  The hot inner-loop pattern `local xn = x*x - y*y + x0` was being routed through `@noinline` `node_local_decl`'s side-array path even after the `nlhs==1 && nrhs==1` runtime fast path landed.  Added a dedicated parser-emitted node `node_local_decl_one(slot, NODE *rhs)` with `@always_inline`; ASTroGen now recurses into the typed `rhs` operand and bakes an SD that inlines the rhs evaluation directly.  `cs hit` jumps from 99/106 → 104/106; perf-attribution `DISPATCH_node_local_decl` drops from 9% → 1%.  Mandelbrot AOT-c: 60 → 51 ms (-15%). |
| ~~**Inlined `luav_unbox_double`**~~ | **Done.**  Heap-boxed double decode is now `static inline __attribute__((always_inline))` in `context.h` (was an out-of-line call); the heap-fallback path in `luav_to_double` becomes a direct struct-field load.  Mandelbrot AOT-c: 51 → 47 ms (-8%). |
| ~~**Stop re-interning string operands on every dispatch**~~ | **Done.**  `lua_gen.rb`'s specializer was emitting `lua_str_intern("x")` literally into SD bodies — the comment said "Re-intern by C string at SD load time so the SD_ source can be cached on disk and reloaded across runs", but that meant **every dispatch re-ran the intern** on the same C string.  perf showed `lua_str_intern_n` at 21.5% of nbody AOT-c cycles.  The parser already interns the string and stores the `LuaString *` on the AST node, so the SD just reads `n->u.<kind>.<field>` — fresh-per-run because the AST is rebuilt by the parser before any SD runs.  Nbody AOT-c: 33 → 22 ms (-33%). |
| **Residual `DISPATCH_*` indirect calls** | A handful of nodes (~25 / ~106 in mandelbrot) still miss the AOT load because their hash-derived SD wasn't generated by `astro_cs_compile` — cycle-break in the specializer (`is_specializing` already set) is one trigger, dedup on already-emitted hashes is another.  These nodes' `head.dispatcher` stays at `&DISPATCH_<name>` in the host binary, so the SD module still bounces through the host on those (~15% of cycles in mandelbrot).  Closing this requires either (a) emitting an SD per *every* node (no dedup short-circuit at compile time, just rely on the linker / string-equality of `static inline` bodies), or (b) post-processing to add wrappers at the host side too.  Framework-level. |
| **Type-speculating SDs** | Every SD inherits the `if(LV_IS_INT(a) && LV_IS_INT(b))` guard from the source EVAL | At AOT bake time we have profile info on which type a node sees; emitting a guard-free SD with a deopt path would let `int_add` SDs become a single `add` instruction.  Closes much of the gap to LuaJIT's interpreter mode. |
| **2-value RESULT (`rax+rdx`)** | `RESULT` is a typedef for `LuaValue`; control flow goes through `LUASTRO_BR` / `LUASTRO_BR_VAL` globals | Theoretical perf win in tight loops — the global memory load/store on every iteration becomes a register check.  Earlier draft was abandoned mid-port; a full re-port is needed. |
| **Float-only fused loops** | Only `node_numfor_int_sum` exists | A `node_numfor_flt_sum` (and more generally, a way to detect type-stable accumulator loops at parse time and emit scalar bodies) would help float-heavy benchmarks even before inline flonum lands. |

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
