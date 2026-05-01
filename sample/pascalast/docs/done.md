# done.md — what's implemented

Running list of language features and performance work that has
landed.

## Language features

### Round 1 — initial subset

`program` / `const` / `var` / top-level `procedure` / `function`,
direct recursion, function return by name, full operator precedence
with short-circuit `and`/`or`, `if/then/else`, `while/do`,
`repeat/until`, `for/to/downto/do`, `begin/end`, comments
`{ } (* *) // `, case-insensitivity (lex-time lowercase), built-ins
`abs sqr sqrt succ pred ord chr odd inc dec halt write writeln read
readln`, `write(x:w)` width spec.

### Round 2 — post-MVP

`forward;` and mutual recursion, `case`, `var` parameters,
2D arrays, `real` with int↔real promotion.

### Round 3 — control flow + composite types

`break` / `continue` / `exit` / `exit(value)`, `Result` alias,
char literals `'A'`, `readln(...)` line consumption, token-name
diagnostics, `type` blocks (enum + subrange + alias), local arrays,
array-as-`var`-parameter, `record` + `with` + var-record parameter,
`set` (64-bit bitset) + literal + `+ - *` + `in`, `string` type with
concat / compare / `length` / indexing, char→string promotion,
pointers (`^Type`, `nil`, `new`, `dispose`, `p^.field`).

### Round 4 — ISO 7185 ライン到達

| Feature                                  | Test                       |
|---|---|
| Nested procedures with display vector     | `test/29_nested_proc.pas`  |
| Procedure / function values, `@func`      | `test/30_proc_value.pas`   |
| Text-file I/O (`assign / reset / rewrite / close / read / readln / write / writeln / eof`) | `test/31_file_io.pas` |
| Variant records (`case selector of`)      | `test/32_variant_record.pas` |
| Source-line tracking in run-time errors   | (every error printer)      |
| Nested record / record-of-record          | `test/33_nested_record.pas` |
| String mutation `s[i] := c` (copy-on-write) | `test/36_string_mut.pas`   |
| Inline-cache for procedure calls          | (transparent)              |
| `try/except`, `try/finally`, `raise`, `ExceptionMessage` | `test/34_exceptions.pas`     |
| `for x in arr do …`                        | `test/35_for_in_packed.pas` |
| `packed` keyword (parsed and ignored)     | (same)                     |
| libgc-backed heap                          | (transparent)              |
| `unit` / `uses` / `interface` / `implementation` | `test/37_uses.pas` (+`test/myunit.pas`) |
| OOP: `class`, single inheritance, fields, constructor, methods, `Self`, static dispatch | `test/38_class.pas` |

### Round 5 — true OOP

| Feature                                  | Test                       |
|---|---|
| **Real virtual dispatch via vtable**     | `test/39_virtual.pas`      |
| `virtual` / `override` actually mean what they say (vtable slot assignment, override reuses parent's slot) | (same) |
| `inherited [methodname](args)` — static call into parent | `test/40_inherited.pas` |
| `destructor T.Done; … end;` (parsed and called like a regular method) | (same) |

### Round 6 — OOP polish + string lib + parsing

| Feature                                  | Test                       |
|---|---|
| **Properties** (`property X: T read R [write W]`) — read/write via field offset or method idx | `test/41_property_isas.pas` |
| **`is` / `as`** — runtime class hierarchy check via vtable lookup | (same) |
| **Bare-name auto-resolve** — inside a method, unqualified `field` / `Property` becomes `Self.X` (locals shadow) | (same) |
| **`(p as T).field` / `(p as T).method(args)`** — postfix dispatch on parenthesised class expressions | (same) |
| Visibility keywords (`private`, `public`, `protected`, `published`) — accepted, no enforcement | (same) |
| **AnsiString full helpers**: `copy`, `pos`, `insert`, `delete`, `setlength`, `IntToStr`, `StrToInt`, `FloatToStr`, `StrToFloat` (auto-promotes char→string) | `test/42_string_funcs.pas` |
| `label N1, N2, …;` declarations + `goto N` / `N: stmt` syntax accepted (runtime stubbed — see todo.md) | — |

### Round 7 — class polish + dynamic arrays + range checks

| Feature                                  | Test                       |
|---|---|
| **Dynamic arrays** (`array of T` value type) — slot stores `int64_t *` (length at index 0); `setlength` / `length` / `a[i]` | `test/43_dynarr.pas` |
| **Subrange range checks** — `var x: 1..100` raises a *catchable* exception on out-of-range assignment | `test/44_subrange.pas` |
| **Abstract methods** (`virtual; abstract;`) — vtable slot 0 / -1; calling raises a catchable runtime exception | `test/45_abstract_class.pas` |
| **Class methods** (`class procedure foo` / `class function bar`) — no implicit `Self`; called as `T.foo(args)` | (same) |
| **Polymorphic return-type** — virtual-call result type is captured from the class header, so `s.name` typed `string` even when proc_idx is still -1 (abstract base) | (same) |
| **`pascal_raise`** — runtime helper that longjmps to the active try/except; range checks and abstract-method calls now go through it | (same) |

Total tests: **45 / 45** passing.

### Knowingly skipped (deferred to later rounds)

- `goto` / labels — covered functionally by break/continue/exit/raise.
- Range-checking on subrange types at assignment.
- Open-array params (`array of T` without bounds) — needs base+length pair.
- N-dimensional (≥ 3) arrays.
- True virtual dispatch (vtable) — `virtual` / `override` are accepted by
  the parser but methods bind statically right now.
- `inherited` keyword (Free Pascal style) — accepted as a token but
  not yet implemented.
- Properties, class methods, abstract, generic classes, exception
  classes proper.

## Runtime / interpreter work

### Round 4 highlights

- **Display vector** in CTX (`int display[PASCAL_MAX_DEPTH]`) for
  nested-procedure variable access.  `pascal_call_cached` saves and
  restores `display[lexical_depth]` around each call so siblings and
  recursion rebind freely; uref/uset reads `c->stack[c->display[d] +
  idx]`.
- **Procedure values** as int proc-indexes; `node_pcall_ind` reads
  the slot, looks up the proc, runs the body via the standard
  pascal_call.
- **Text files** as a `pascal_file *` per slot; `assign` lazily
  allocates, `reset` / `rewrite` open the FILE *, write/read variants
  per type (`fwrite_int / fwrite_str / fwrite_real / fread_int /
  fread_line`).
- **Variant records** flatten to a single record_type whose fields
  may share offsets — variants overlap at the variant base offset.
- **Run-time error lines** stamped via `g_alloc_line` (set by the
  parser in `next_token`) into `n->head.line` at OPTIMIZE; node-side
  errors call `pascal_error_at(n->head.line, ...)`.
- **Nested record fields** (e.g. `r.inner.x`) walk a static
  field-offset chain at parse time, emitting one read/write at the
  flattened slot.
- **Inline cache for proc calls** (`@ref struct pcall_cache`):
  per-call-site cache populated on first dispatch; subsequent calls
  skip the procs[] lookup.  Saves one indirection per call.
- **Exceptions via setjmp** in a CTX-threaded handler stack; raise
  longjmps to the innermost handler.  fp/sp/display are snapshotted
  at try entry so unwinding through nested frames is clean.
- **for-in** lowered to a regular for-to over the array's bounds with
  an auto-allocated hidden index slot.
- **libgc-backed heap** for strings, file objects, pointer targets,
  and readln buffers.  Long-lived parser tables stay on plain
  malloc — there's no value in tracking them.
- **Unit / uses** are file-include style: `parse_unit_file` saves and
  restores the lexer state, parses the unit's decls into the same
  global symbol table.  `interface` headers are body-less (auto
  forward); `implementation` provides the bodies.
- **Classes with virtual dispatch (round 5).**  A class is a record_type
  with slot 0 reserved for the vtable pointer + a method table.
  `T.Create(args)` emits `node_new_object(slots, class_idx)` which
  allocates and stamps the per-class vtable address into slot 0; the
  constructor body then runs with the new pointer as `Self`.
  `obj.field` auto-derefs through the pointer.  `obj.method` for a
  virtual method emits `node_vcall(self, vtable_slot, args…)` which
  reads vtable from `obj[0]` and calls vtable[slot] — actual
  polymorphism: `a: TAnimal` holding a `TDog` calls `TDog.speak`.
  `inherited [methodname](args)` is always a *static* call into the
  parent's same-named method, regardless of virtuality.

### AOT specialization (still in)

`pascalast -c FILE.pas` parses the program, runs the SPECIALIZE
pass on every procedure body and the main body, dumps a
self-contained translation unit to `node_specialized.c`.  The next
build picks up that file via `#include` in `node.c` and links every
`SD_<hash>` function into the binary.

`make aot-bench BENCH=<name>` does the round-trip.  Recent numbers
(post Round 8 baked-pcall — see below):

| benchmark               | interp  | AOT    | speedup |
|---|---|---|---|
| fib.pas                 | 0.84 s  | 0.20 s | 4.2 ×   |
| tarai.pas               | 1.49 s  | 0.27 s | 5.5 ×   |
| ackermann.pas           | 1.56 s  | 0.27 s | 5.8 ×   |
| gcd.pas                 | 0.90 s  | 0.26 s | 3.5 ×   |
| quicksort.pas           | 1.21 s  | 0.22 s | 5.5 ×   |
| collatz.pas             | 1.32 s  | 0.05 s | 26 ×    |
| heron.pas               | 1.33 s  | 0.08 s | 17 ×    |
| mandelbrot_int.pas      | 1.47 s  | 0.04 s | 37 ×    |
| mandelbrot_real.pas     | 1.30 s  | 0.05 s | 26 ×    |
| leibniz_pi.pas          | 1.15 s  | 0.09 s | 13 ×    |
| matmul.pas              | 1.33 s  | 0.07 s | 19 ×    |
| matmul_2d.pas           | 1.51 s  | 0.19 s | 8 ×     |
| varparam_swap.pas       | 1.17 s  | 0.10 s | 12 ×    |

Constant-folded tight loops (collatz, mandelbrot) collapse
spectacularly.  The recursive call-heavy benchmarks (fib / tarai /
ack) used to gain only ~2 × under the original pcall_K_cached path
because the proc-call edge stayed indirect across the SD boundary.
Round 8's baked pcall variant (see below) bakes body's SD plus the
per-proc metadata into the call NODE so the AOT'd parent SD calls
the callee SD directly, lifting recursion-heavy benches to 4-6 ×.

### Round 8 — AOT baked pcall + compiler directives

| Feature                                  | Test / Bench           |
|---|---|
| **`node_pcall_K_baked`** (K=0..3, plus N) — body NODE * is a `@nohash` operand visible to SPECIALIZE; per-proc metadata (nslots / return_slot / lexical_depth / is_function) baked as uint32 operands so AOT folds them into literals | (transparent — every call site uses these now) |
| **`@nohash` operand modifier** in `pascalast_gen.rb` — NODE * operand that doesn't auto-recurse during SPECIALIZE and contributes 0 to HASH; needed to break the body↔pcall hash cycle while still emitting body's `dispatcher_name` as a literal in the SD source | (`pascalast_gen.rb`) |
| **Eager `dispatcher_name` set in SPECIALIZE** — `node.c::SPECIALIZE` stamps `n->head.dispatcher_name = SD_<hash>` *before* recursing into children, so a child that cycles back to the parent (recursive proc) sees the right name and the recursive-edge SD call is emitted as a direct `SD_X` reference instead of a generic `DISPATCH_node_<kind>` | fib / tarai recursive call sites |
| **Post-parse fixup pass** — `pcall_fixups[]` records every baked-pcall NODE at parse time; a single sweep after `parse_program()` patches body + nslots + return_slot + lexical_depth + is_function from the resolved `c->procs[pidx]` | mk_pcall + parse_program tail |
| **Proc-body / main-body roots emitted `static inline`** — matches the forward-decl that baked specializers emit; gcc still lays out a callable copy because `sc_entries[]` takes the address | (`SPECIALIZED_SRC` in node.c) |
| **`{$R+/-}` / `(*$R+/-*)` directives** — toggle subrange range-check emission in the parser.  Unknown one-letter directives (`{$H+}`, `{$MODE OBJFPC}`, …) are accepted and silently ignored so real-world Pascal programs don't trip the parser | `test/46_directive.pas` |

## Bug fixes

- **Stale `by_ref` flag** — `lsyms[]` re-use across procedures
  leaked the previous proc's var-param flag into a fresh local.
  `sym_add_local` now `memset`s the slot first.
- **Char-vs-string concat** crashing because of `(const char*)33`.
  `+` between `string` and `int` (char) now inserts `chr_to_str`.
- **Nested string-readln** consuming an extra line.  `node_fread_line`
  now leaves the trailing newline for `freadln_eat` to consume.
- **Constructor return value** — body-less `b := TBox.Create(42)` was
  yielding 0 because the ctor never wrote `Result`.  Now the parser
  prepends `return_slot := Self` to every constructor body.
