# AnCaml runtime — value model, scope, GC

## Value representation (`context.h`)

`VALUE` is a tagged 64-bit word:

| pattern | meaning |
|---|---|
| `…1` (LSB = 1) | immediate `int`, value = `v >> 1` (63-bit signed) |
| `2` / `4` / `6` | the constants `unit` / `false` / `true` |
| 8-aligned, ≠ 0 | heap object pointer (`struct ac_obj *`) |

Immediates and the three small constants carry no pointers, so the conservative
GC ignores them; heap pointers are 8-aligned and traced normally.  Macros:
`AC_INT` / `AC_INT_VAL` / `AC_IS_INT`, `AC_BOOL`, `AC_IS_PTR`, `AC_PTR`,
`AC_OBJ`, and the constants `AC_UNIT` / `AC_TRUE` / `AC_FALSE`.

Heap objects (`struct ac_obj`, a tagged union):

| kind | payload |
|---|---|
| `AC_FLOAT` | a boxed `double` |
| `AC_CLOSURE` | `(body node, captured frame, nparams)` |
| `AC_TUPLE` | `(n, items[])` immutable |
| `AC_ARRAY` | `(n, items[])` mutable |
| `AC_PRIM` | `(name, C fn, arity)` — the external functions |

Floats are boxed (no NaN-boxing).  This keeps the GC trivial and the tagging
uniform; the cost is an allocation per float result, which dominates
float-heavy loops (see [perf.md](perf.md)).

## Environment — de Bruijn frames

A lexical scope is a flat slot vector with a parent pointer:

```c
struct ac_frame { struct ac_frame *parent; int nslots; VALUE slots[]; };
```

The parser resolves every variable occurrence to a `(depth, idx)` coordinate
at parse time (see `parse.c`'s scope stack), so the runtime never hashes a
name: `node_lref(depth, idx)` walks `depth` parents and reads `slots[idx]`.

Which constructs push a frame, and with how many slots:

| construct | frame |
|---|---|
| `let x = v in body` | 1 slot (`x`) over the body |
| `let rec f … = … in body` | 1 slot (`f`); the function value captures it so `f` can recurse |
| `let (a,b,…) = v in body` | one slot per pattern variable |
| applying an n-ary closure | one slot per parameter, parented on the closure's captured frame |
| `e1; e2` | **no** frame (kept as a dedicated `node_seq` so the slot numbering stays simple) |

A closure stores the `body` NODE and the frame it was created in.  `ac_apply`
(in `value.c`) builds the parameter frame over that captured frame, swaps
`c->env`, dispatches the body through its runtime dispatcher, and restores
`c->env`.  Recursion works because a `let rec`'s closure captures the frame
holding `f` itself.

## Function application & the AOT entry boundary

Because `ac_apply` invokes a closure body through
`(*body->head.dispatcher)(c, body)` — a dispatcher read off a runtime
value — the SD specializer cannot constant-fold across the call.  Each
function body is therefore registered as its own code-store **entry** (the
parser collects them in `ac_entries`), exactly as `docs/usage.md`'s
"Entry nodes" section prescribes.  The same applies to the variable-arity
children kept in the `AC_CALL_ARGS` / `AC_TUPLE_ITEMS` side-tables
(`node_appn` arguments and `node_tuple` elements).

## GC

Heap objects, frames, and the `CTX` come from **libgc** (`GC_MALLOC`).  The
collector is conservative; the tagging above guarantees that non-pointer
words are never mistaken for pointers.  Type metadata (`type.c`) uses plain
`malloc` and leaks — the checker runs once per program.

## Errors

`ac_runtime_error` flushes stdout, prints `ancaml: runtime error: …` to
stderr, and `longjmp`s back to the driver's handler (`c->err_jmp`) when one
is active (so the process exits cleanly with status 1), else `exit(1)`.
Type errors are reported earlier, before any evaluation, by `type.c`.
