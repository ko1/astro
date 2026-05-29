# AnLox runtime — value model, scope, GC

## Value representation (`context.h`)

`VALUE` is a tagged word:

| pattern | meaning |
|---|---|
| `2` / `4` / `6` | the constants `nil` / `false` / `true` |
| 8-aligned, ≠ 0 | heap object pointer (`struct lox_obj *`) |

Lox has **no integers** — every number is a `double`, boxed as a `LOX_NUM`
object.  Heap object kinds (`struct lox_obj`, a tagged union): `LOX_NUM`
(double), `LOX_STR`, `LOX_CLOSURE` (fundef + captured frame), `LOX_CLASS`
(name + superclass + method table), `LOX_INSTANCE` (class + field table),
`LOX_NATIVE`.

Boxing every number is the dominant runtime cost (see [perf.md](perf.md)); a
value-representation change (NaN-boxing / unboxed doubles) is the main perf
lever and is left as a TODO to keep the model simple and GC-trivial.

## Environment — de Bruijn frames + global table

A local scope is a flat slot vector with a parent pointer:

```c
struct lox_frame { struct lox_frame *parent; int nslots; VALUE slots[]; };
```

The resolver (in `parse.c`) maps every local occurrence to a `(depth, slot)`
coordinate at parse time — exactly the information *Crafting Interpreters*
computes in its separate Resolver pass — so `node_local(depth, slot)` is a
pointer-chase + index with no name hashing.

Scopes / frames:

| construct | frame |
|---|---|
| function call | `nslots` = params + the body's top-level locals (one frame) |
| `{ … }` block | a fresh frame sized to its own locals |
| `for (…)` | a scoped block around a desugared `while` |
| program top level | **no** frame — top-level `var` are globals |

**Globals** are late-bound: a name→VALUE hash table on the `CTX`, looked up by
name (`node_global`).  They can be defined and redefined at runtime; reading an
undefined global is a runtime error.

**`this` / `super`** are ordinary locals.  In a method, the resolver places a
`this` scope (and, for a subclass, a `super` scope) around the method's
parameter scope.  Accessing `obj.method` *binds* the method: it returns a fresh
closure whose captured env is a one-slot `this` frame over the method's
definition env.  `super.m` reads the `super` class and the `this` instance from
their frames, finds `m` up the superclass chain, and binds it.

## Function application & `return`

`lox_call` (in `value.c`) handles closures (push the param frame, run the body,
catch `return`), classes (construct an instance + run `init`, returning the
instance), and natives.  `return` unwinds cooperatively via `c->returning` /
`c->retval`, checked by `node_stmts` / `node_block` / the loops and consumed at
the call boundary — no `longjmp` per return.

## The side-tables (and GC)

Variable-arity children don't fit fixed node operands, so they live in
index-addressed side-tables filled by the parser: `LOX_BLOCK_STMTS`
(statement lists), `LOX_CALL_ARGS` (call arguments), `LOX_FUNDEFS` (function
descriptors), `LOX_CLASS_METHODS` (method fundef indices).  Each node reached
through one of these (and every function body) is registered as its own
code-store entry, since the dispatcher is read at runtime (see
`docs/usage.md`'s "Entry nodes").

**GC gotcha (learned here):** these arrays must be **GC-allocated**
(`GC_REALLOC`), not `malloc`'d.  The AST nodes are GC objects; if they were
reachable only through `malloc`'d arrays, a collection (triggered after ~1000
allocations — e.g. `fib(14)`) would free live nodes and the next dispatch would
crash.  The arrays are pointed to by static/global pointers, which libgc scans
as roots, so GC traces the whole AST through them.

## GC & errors

Heap objects, frames, tables, and the `CTX` come from **libgc** (`GC_MALLOC`;
strings via `GC_MALLOC_ATOMIC`).  `lox_runtime_error` prints to stderr and
`longjmp`s back to the driver (exit 70) when a handler is active.
