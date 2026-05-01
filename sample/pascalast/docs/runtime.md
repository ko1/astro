# runtime.md — how the interpreter actually runs

This doc walks through how a parsed program executes, with extra
attention on the mechanisms that aren't obvious from reading
`node.def` in isolation.  Updated for round 4 (ISO 7185 line: nested
procs, proc values, file I/O, variant records, exceptions, unit
system, minimal OOP).

## Big picture

The same `node.def` produces:

- `node_alloc.c`  — `ALLOC_node_X(args)` constructors
- `node_dispatch.c` — `DISPATCH_node_X(c, n)` thin wrappers
- `node_eval.c`  — inlined `EVAL_node_X(c, n, args…)` bodies
- `node_hash.c`  — Merkle-tree hashing
- `node_dump.c`  — debug dumper
- `node_replace.c` / `node_specialize.c` — used by AOT specialization

The host-side `node.c` provides `EVAL`, `HASH`, `DUMP`, `OPTIMIZE`,
`SPECIALIZE`, `dispatch_info`, the `hash_*` helpers, the `sc_repo`
specialized-code repository, and (this round) `pascal_try_run` for
exception unwinding.

`OPTIMIZE` is called by every `ALLOC_node_X`.  It stamps
`n->head.line = g_alloc_line` (parser-side global, updated in
`next_token`) and looks up `hash_node(n)` in `sc_repo`; on a hit the
node's dispatcher is swapped to the AOT-compiled SD.

## Storage model

```
CTX:
  globals[PASCAL_MAX_GLOBALS]   — flat int64 cells
  arrays[i], array_lo[i],
  array_size[i], array_lo2[i],
  array_size2[i]                — heap buffer + bounds for global array `i`
  stack[PASCAL_STACK_SIZE]      — flat call stack
  fp                             — index of slot 0 of the current frame
  sp                             — first free slot above the current frame
  display[PASCAL_MAX_DEPTH]     — fp at each lexical depth (nested-proc support)
  procs[]                        — proc table; see below
  loop_action, exit_pending     — non-local control flow flags
  exc_top, exc_msg              — setjmp handler chain + last raised string
```

Locals at slot `idx`: `c->stack[c->fp + idx]`.  Globals: `c->globals[idx]`.

## Functions and procedures

The proc table `c->procs[i]` records, per subprogram:

```c
struct pascal_proc {
    const char *name;
    NODE       *body;
    int         nparams, nslots;
    bool        is_function;
    int         return_slot;
    int         lexical_depth;        // 0=main, 1=top-level proc, 2+=nested
    bool        param_by_ref[16];
    bool        param_is_array[16];
    int32_t     param_arr_lo[16];
    char        param_type[16];       // PT_INT / PT_BOOL / PT_REAL ...
    char        return_type;
};
```

### `pascal_call_cached` — the call trampoline

Each `node_pcall_K` carries an `@ref pcall_cache` that's lazily
populated on the first dispatch.  After populate it skips the
`procs[pidx]` lookup:

```c
if (UNLIKELY(!cache->body)) {
    struct pascal_proc *p = &c->procs[pidx];
    cache->body          = p->body;
    cache->nslots        = p->nslots;
    cache->return_slot   = p->return_slot;
    cache->lexical_depth = p->lexical_depth;
    cache->is_function   = p->is_function;
}
new_fp = c->sp;
new_sp = new_fp + cache->nslots;
copy av into stack[new_fp..]
saved_display = c->display[cache->lexical_depth];
c->display[cache->lexical_depth] = new_fp;
c->fp = new_fp; c->sp = new_sp;
EVAL(c, cache->body);
rv = cache->is_function ? c->stack[new_fp + cache->return_slot] : 0;
c->display[cache->lexical_depth] = saved_display;
c->fp = saved_fp; c->sp = saved_sp;
c->exit_pending = 0;
c->loop_action  = 0;
return rv;
```

Pascal procs are not redefinable, so the cache never invalidates.

### `var` parameters

Slot stores a `VALUE *` (the address of the source storage cast
through `int64`).  Reads/writes deref:

| Source            | Address-of node                  |
|---|---|
| local scalar      | `node_addr_lvar(idx)`           |
| local var-param   | `node_addr_passthru(idx)`       |
| global scalar     | `node_addr_gvar(idx)`           |
| global 1D elem    | `node_addr_aref(arr_idx, i)`    |
| global 2D elem    | `node_addr_aref2(arr_idx, i, j)`|
| local 1D elem     | `node_addr_aref_local(slot, lo, i)` |
| global array base | `node_addr_garr_base(arr_idx)` (var-array params) |
| local array base  | `node_addr_larr_base(slot)`     |
| global record     | `node_addr_gvar(s->idx)` (passes the base of the contiguous slot run) |

Inside the callee, `node_var_lref` / `node_var_lset` deref through
the slot; `node_var_aref(slot, lo, i)` indexes through the base.

## Nested procedures — the display vector

A nested proc's body can read its enclosing proc's locals.  We
implement this with a display vector.  Each pascal_proc records its
`lexical_depth`; a non-local variable access at `(depth, idx)` reads
`c->stack[c->display[depth] + idx]`.

`pascal_call_cached` saves and restores `c->display[lexical_depth]`
around the call so siblings and recursion rebind freely.  Pascal's
lexical visibility rule (you can only call procs in scope) means the
display vector always holds the right binding for any reachable
non-local variable.

`node_uref` / `node_uset` / `node_uvar_ref` / `node_uvar_set` /
`node_uaref_local` / `node_uaset_local` are the up-level access
nodes; the parser picks them when `sym->depth < current_depth`.

## Procedure values

`var p: procedure(x: integer)` is a slot that stores a proc index.
`@func` evaluates to that index.  `p(args)` is `node_pcall_ind(fn,
args_idx, argc)`: read the index from `fn`, look up
`c->procs[idx].body`, run via `pascal_call`.  No var-param support
through proc values (they always pass by value).

## File I/O

A `text` slot stores a `pascal_file *` (heap-allocated under libgc):

```c
struct pascal_file {
    FILE *fp;
    char  name[256];
};
```

`assign(f, name)` lazily allocates the struct on the first call and
stores the filename.  `reset` / `rewrite` `fopen` for read / write.
`close` `fclose`s.  `eof` peeks via `fgetc` + `ungetc`.  `read` /
`write` and friends route to `node_fread_int / fread_line /
fwrite_int / fwrite_str / fwrite_real / fwriteln / freadln_eat`.

The parser sniffs the first arg of `read` / `readln` / `write` /
`writeln`; if it's a file variable, the file-targeted nodes are
emitted, else stdout/stdin nodes.

## Variant records

`record fixed-fields ; case [tag :] type of v1: (fields); v2:
(fields) … end` — parser flattens both fixed and variant fields into
a single `record_type.fields[]` array, with variant fields starting
at the same base offset (so they overlap).  At runtime there's no
discriminator check; field names happen to be unique by convention.

## Exceptions

`raise STR` sets `c->exc_msg` to STR and `longjmp`s to
`c->exc_top->buf` (or, if no handler, calls `pascal_error_at`).

`try-except`:

```c
node_try_except(body, handler) {
    if (pascal_try_run(c, body) != 0) EVAL(c, handler);
}
```

`pascal_try_run` lives in `main.c` (not in node.def — `EVAL_node_*`
is force-inlined and `setjmp` can't be inlined).  It pushes a
handler, snapshots `fp/sp/display`, runs `body`; on raise it pops
+ restores and returns 1.

`try-finally`:

```c
node_try_finally(body, fin) {
    raised = pascal_try_run(c, body);
    EVAL(c, fin);
    if (raised) longjmp(c->exc_top->buf, 1);   // re-raise
}
```

Cleanup runs on both paths; an uncaught raise propagates after the
finally.

## Classes (minimal OOP, this round)

A class is a `record_type` (layout for fields, with slot 0 reserved
for a future vtable pointer) plus a `class_type` entry that holds
the class name, parent index, and method table.

Inheritance:
- Child's `record_type` starts with a copy of parent's fields at
  parent's offsets.  Child's own fields start at
  `parent.total_slots`.
- Child's method table starts as a copy of parent's; same-name
  child methods later overwrite the entry (static dispatch — no
  vtable yet).

Method body declaration `procedure T.method(...);` registers a fresh
proc with name `"T.method"` and patches the matching method-table
entry's `proc_idx`.

`Self`:
- The first parameter (slot 0) of every method body is automatically
  named `self`, with type `PT_POINTER` and the class's record-type
  index.  Field access through `Self.field` goes through the
  `obj.field` path (auto-deref through the pointer).

Construction:
- `T.Create(args)` is parser-recognized — if the resolved method is
  marked `is_constructor`, the parser emits a fresh
  `node_new_record(slots)` as the first arg (the new Self), then a
  regular pcall.
- The constructor body is augmented at parse time with `Result :=
  Self;` prepended, so the ctor's return slot holds the new object
  pointer regardless of what the user wrote.

`obj.field` / `obj.method` (no `^`): parser sees a class-typed
PT_POINTER slot and special-cases the dot to route through
`node_ptr_field_ref / set` (for fields) or `mk_pcall` with self as
first arg (for methods).

## Source-line tracking

`g_alloc_line` is updated in `next_token` to the lexer's current
line.  `OPTIMIZE` (called by every `ALLOC_node_X`) stamps
`n->head.line = g_alloc_line`.  Run-time errors call
`pascal_error_at(n->head.line, ...)`, which prefixes `[line N]` to
the message.

## libgc backing

Boehm-Demers-Weiser GC is linked at `/usr/lib/x86_64-linux-gnu/libgc.so.1`.
`INIT()` calls `GC_init()` once.  Used for: string concat results,
char→string promotion buffers, string literals, file objects, pointer
targets (`new`), and `readln` line buffers.  Long-lived parser-side
allocations (sym tables, procs, record_types) stay on plain `malloc`
because they live for the whole program and tracking them costs
without benefit.

## What this interpreter still doesn't do

- No virtual dispatch — `virtual` / `override` are accepted as
  syntax but methods bind statically.  `node_vcall` is in node.def
  ready to wire up.
- No PGC.  The plain AOT path emits SDs; PGC would extend that with
  Hopt-keyed variants.
- No JIT.  `pascalast -c` is the AOT round-trip; there's no L0/L1/L2
  daemon yet.
- No range checking on subrange types.  Bounds are parsed; runtime
  doesn't enforce them.
- No goto / label.  break / continue / exit / raise cover most cases.
