# luastro — implemented features

Snapshot of what works as of the current revision.  See
[`todo.md`](todo.md) for known gaps relative to the Lua 5.4 reference
manual.

## Lexical syntax (`lua_tokenizer.c`)

Full Lua 5.4 lexical surface:

| Category | Coverage |
|---|---|
| Keywords | All 22 reserved words including `goto`, `break`, `repeat`, `until` |
| Identifiers | UTF-8 friendly (any byte ≥ 0x80 treated as ident-continuation) |
| Numerics | Decimal int / float, `0x...` hex (including hex floats `0x1p10`), exponent `e±N` |
| Integer overflow | Lexer falls back to float if literal exceeds `int64_t` |
| Strings | Single / double quoted, all `\` escapes (`\a \b \f \n \r \t \v \\ \" \' \xNN \ddd \z`) |
| Long strings | `[[...]]`, `[=[ ... ]=]` (any number of `=`), opening newline stripped |
| Comments | `--` line comment, `--[[ ... ]]` long comment with `=` levels |

## Grammar (`lua_parser.c`)

Recursive-descent for statements, Pratt for expressions.

| Construct | Notes |
|---|---|
| `local` declarations | Multi-LHS, multi-RHS with adjustment, `<const>` / `<close>` attribs accepted (no enforcement) |
| `local function f` | Parser captures `f` as a captured-local box at definition site for self-reference |
| Function definitions | `function`, `local function`, method form `function t:m()`, varargs `...` |
| Calls | Positional, method calls `obj:m(...)`, table call `t{...}`, string call `f"x"` |
| Statements | `if/elseif/else`, `while`, `repeat/until`, `for i=...`, `for k,v in ...`, `do/end`, `break`, `return`, `goto`/`::label::` |
| Multi-assign | `a, b, c = e1, e2, e3` with adjust-to-N and call-spread on last RHS |
| Tables | `{ k=v, [expr]=v, e1, e2, ... }`, mixed sequence + hash |
| Operators | All Lua 5.4 ops including `//`, `%`, `^`, `..`, bitwise `& | ~ << >> ~=`, length `#`, unary `not -` |
| Operator precedence | Lua 5.4 reference manual table |
| Captured-local rewriting | Pre-pass identifies which locals escape into inner closures; their accesses become `node_box_get/set` |

## Runtime types

| Type | Encoding | Notes |
|---|---|---|
| `nil` | `LuaValue == 0` | calloc-zeroed memory is naturally nil |
| `boolean` | `0x14` (false), `0x24` (true) | |
| `integer` | `(i << 1) | 1` (63-bit signed) | sign-extending right shift recovers value |
| `float` | inline (`(v & 3) == 2`, low60 + sign + disc layout) for `b62 ∈ {3,4}` magnitudes; else heap `LuaHeapDouble { GCHead; double }` | inline encoding is lossless |
| `string` | heap `LuaString { GCHead; hash; len; bytes[] }`, interned | pointer equality = value equality |
| `table` | heap `LuaTable { GCHead; array; hash; metatable }` | hybrid array+hash |
| `function` | heap `LuaClosure { GCHead; body; nparams; nlocals; nupvals; upvals; }` | upvals point into frames or boxes |
| `cfunction` | heap `LuaCFunction { GCHead; name; fn; }` | host-callable |
| `thread` | heap `LuaCoroutine` | ucontext + 256 KiB mmap'd stack |
| boxed local | heap `LuaBox { GCHead; value; }` | created lazily on capture |

## Standard library

### Base

`print`, `tostring`, `tonumber`, `type`, `ipairs`, `pairs`, `error`,
`pcall`, `xpcall`*, `assert`, `select`, `setmetatable`, `getmetatable`,
`rawget`, `rawset`, `rawequal`, `rawlen`, `unpack`, `collectgarbage`.

\* `xpcall` catches the error but the message handler is not invoked.

### `math`

`abs`, `floor`, `ceil`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `exp`, `log`, `max`, `min`, `random`, `fmod`, `modf`, `pow`,
`tointeger`, `type`, plus constants `pi`, `huge`, `maxinteger`,
`mininteger`.

### `string`

`len`, `sub`, `upper`, `lower`, `rep`, `format`, `byte`, `char`,
`reverse`, `find`, `match`, `gmatch`, `gsub`.

The pattern matcher (`lua_pattern.c`) is a full Lua-pattern
implementation: character classes (`%a`, `%d`, `%s`, `%w`, `%p`, `%l`,
`%u`, `%c`, `%x` and uppercase complements), sets `[...]` with ranges
and complements, quantifiers `?`, `*`, `+`, `-`, `%b()` balanced
match, `%f[set]` frontier, anchors `^` / `$`, captures `()` (position)
and `(...)` (substring), `%0` whole match, `%1..%9` capture
references in `gsub` replacement strings.

### `table`

`insert`, `remove`, `concat`, `pack`, `unpack`.

### `io`

`write` (stdout only).

### `os`

`clock`, `time`, `exit`, `getenv`.

### `coroutine`

`create`, `resume`, `yield`, `status`, `isyieldable`, `running`.
(`coroutine.wrap` raises — see todo.)

## Metatables

`__index` (table or function), `__newindex` (table or function),
`__add`, `__sub`, `__mul`, `__div`, `__mod`, `__pow`, `__unm`,
`__idiv`, `__band`, `__bor`, `__bxor`, `__bnot`, `__shl`, `__shr`,
`__concat`, `__len`, `__eq`, `__lt`, `__le`, `__call`, `__gc`.

`__index` chain is followed across multiple levels (table → metatable
→ table's metatable → ...).  Function metamethods are invoked with
the standard `(self, key)` / `(a, b)` signatures.

## Garbage collector

Stop-the-world mark-sweep (`lua_gc.c`):

- Walks the frame stack, globals, multi-return scratch, varargs,
  `last_error`, captured-local cells.
- Recurses through tables (with weak-mode handling for `__mode = "k"`,
  `"v"`, `"kv"`).
- Invokes `__gc` finalizers for unmarked tables before freeing.
- Triggered by `collectgarbage("collect")`.

Strings are kept alive by the intern pool (not freed by sweep).

## Coroutines

ucontext-based; each coroutine has its own 256 KiB C stack.  Resume
swaps in, yield swaps back, errors inside a coroutine propagate as
`(false, msg)` from the resume call.

## Specialization fast paths

| Node | Behaviour |
|---|---|
| `node_int_add/_sub/_mul` | int+int direct; float+float fall through; mixed → `lua_arith` |
| `node_flt_add/_sub/_mul/_div` | float+float direct; otherwise → `lua_arith` |
| `node_arith` | Generic op-typed arith with full coercion / metatable fallback |
| `node_call_arg{0,1,2,3}` | Fixed-arity Lua-to-Lua calls without metamethod / cfunc dispatch |
| `node_call_argN` | Variable arity (≥4 args, or method calls) |
| `node_numfor_int_sum` | Whole-loop fused node for `for i=...do sum = sum + i end` (15× faster than lua5.4) |

## Build / driver

| Target | Effect |
|---|---|
| `make` | Builds `./luastro` |
| `make test` | Runs `test/run_all.rb` (8 .lua test files compared against lua5.4 output) |
| `make bench` | Runs `benchmark/run.rb` (lua5.4 + luajit comparison) |
| `make compiled_luastro` | Bakes SDs from `examples/hello.lua` and rebuilds with the snapshot |
| `make pg_luastro` | Profile-guided variant: run, then bake |

CLI flags:

| Flag | Effect |
|---|---|
| (none) | Pure interpreter; auto-loads `code_store/` if present |
| `-c` / `--aot-compile-first` | Bake SDs, then run with them active |
| `--aot-compile` | Bake and exit (used by bench setup) |
| `-p` / `--pg-compile` | Run first (collect profile), then bake |
| `--no-compile` | Skip code-store load entirely |
| `--dump-ast` | Print AST to stdout |
| `-e CODE` | Eval string instead of file |
| `-q` / `-v` | Quiet / verbose |

## Test suite

`test/test_*.lua` (8 files): arith, compare, control, functions, math,
pcall, strings, tables.  All passing — output is byte-for-byte
compared against `lua5.4`'s output.
