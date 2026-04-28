# wastro — gaps relative to wasm 1.0 (and beyond)

As of v1.0 the implementation covers the WebAssembly 1.0 (MVP) text
and binary surface: full numeric / control / memory / global /
function-call / table / call_indirect / start / module-level export
support, with a `.wast` spec-test harness.  See [`done.md`](done.md)
for the full inventory.

The remaining gaps below are deliberate choices, post-1.0 features,
or small cleanups.

## Known wasm 1.0 quirks not yet covered

| Item | Notes |
|---|---|
| Stack-style let-floating | When a void instr is encountered while pure-and-impure values coexist on the operand stack, side-effect ordering across the void is preserved only if the carried values have no side effects.  Real-world compiler output (rustc / emcc / clang) follows "consume what you push" so this hasn't surfaced.  A future pass could spill stack values to fresh temp locals to make this fully spec-conformant. |
| `nan:canonical` / `nan:arithmetic` matchers | The spec testsuite uses `(assert_return ... (f32.const nan:canonical))` for non-deterministic NaN bit patterns.  We currently treat any NaN==NaN as a match, which over-accepts.  Tighten when running f32/f64.wast. |
| Multi-result `(result T1 T2)` blocks | Wasm multi-value extension (post-1.0).  Block / function single-result only. |
| Higher call arities (>4) | `node_call_N` and `node_call_indirect_N` cover N=0..4 (and host_call N=0..3).  Most testsuite functions stay within this range; bump if needed. |

## Post-1.0 features (out of scope)

These are explicitly not aimed at and would each require their own
design pass:

- **Bulk memory** (`memory.fill`, `memory.copy`, `memory.init`,
  `data.drop`, passive `(data $name)` segments).
- **Reference types** (`ref.func`, `ref.null`, `ref.is_null`,
  `table.get/set/grow/size/fill/copy/init`, `elem.drop`).
- **Multi-value** (multiple block / call results).
- **Multi-memory**.
- **Tail calls** (`return_call`, `return_call_indirect`).
- **GC types** (`(struct ...)` / `(array ...)` / `(ref ...)`).
- **SIMD** `v128` and the 128-bit operation set.
- **Threads / atomics** (`atomic.add`, shared memory).
- **Exception handling** (`try` / `catch` / `throw` / `delegate`).

## Tooling and front-end

| Item | Notes |
|---|---|
| Cross-module `(register "name")` linkage | Spec testsuite uses this to import functions from a previously-loaded module.  We accept the form syntactically and skip — single-module per `.wast` works fine. |
| Better error messages | `head.line` is reserved on every node but unused; column tracking would help the harness output. |
| `--dump-ast` / `--dump-c` | abruby has both.  Useful for debugging the binary decoder and the stack-style parser. |
| Programmatic host-import binding | The `env.*` registry is hardcoded.  A flag / callback to register custom host functions (`--import env.foo=./libfoo.so:foo`) would let users wire arbitrary C symbols. |

## ASTro framework extensions (out of scope for wastro itself)

These touch ASTro's framework (`lib/astrogen.rb` and the runtime).
Not wastro work, but they cap how fast wastro can run on call-heavy
workloads (`fib`, recursive code).  Iterative workloads (`sum`,
`sieve`) are already within 3× of native and not bottlenecked by
these.

| Item | Notes |
|---|---|
| `node_call_N` body as `@noinline_child` | Embed body NODE pointer in the call node, but tell the specializer not to recursively inline through it.  Phase 2 / prerequisite for Phase 3. |
| Depth-bounded recursive specializer | `lib/astrogen.rb` build_specialize would unroll recursive calls N levels.  Phase 3 in the wastro README. |
| Typed dispatcher signatures | Allow specialized dispatchers to take typed args (register passing).  Phase 4. |

## Smaller cleanups specific to wastro

| Item | Notes |
|---|---|
| Higher-arity `node_call_N` / `node_host_call_N` / `node_call_indirect_N` | Add `_5` through `_8` if any module needs them (parser already errors out cleanly). |
| Stricter typing on `if` without explicit result | Today a result-less `if` defaults to void unless both branches happen to produce the same non-void type.  Tighten when this matters. |
| Custom hash for `node_call_N` and `node_call_indirect_N` | Today the node hash combines indices and arg children, which suffices for distinct call sites; verify there are no hash collisions across modules with the same shape. |

## ASTro framework — future work (added 2026-04-28)

### SCC-aware hashing for cycle inlining

When the call graph has a cycle (mutual or self-recursion), the current
visited-set / `is_specializing` cycle-break leaves the cycle-closing edge as
an indirect dispatch.  The N-1 inlined hops within the cycle are good, but
the closing edge falls back to runtime dispatcher.

A stronger ASTro framework feature: detect strongly-connected components in
the AST/call graph at HASH time, compute a fixpoint hash for the entire SCC
(treat the whole loop as a single specialization unit), and emit one SD\_
that handles every node in the component with internal direct calls.  fib
becomes one self-referential SD\_ that calls itself directly; even/odd
becomes a paired SD\_ with two entry points.

This is an ASTro framework change (not a wastro-specific tweak) and likely
non-trivial — needs careful interaction with the existing `is_specializing`
flag, dedup keying, and incremental compilation.  Tracked as a future
priority for the framework.
