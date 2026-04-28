# wastro runtime — software architecture

This document describes how wastro represents and executes wasm
modules.  It is the longer-form companion to the README's *Design
notes* section: every point made there is expanded here, and each
implementation detail is anchored at a concrete file/symbol so the
reader can read on.

For the implemented-feature inventory see [`done.md`](done.md);
for known gaps see [`todo.md`](todo.md).

## 1. Pipeline at a glance

```
   .wat / .wasm
        │
        ▼
   parser            (main.c — tokenizer, S-expr / stack-style WAT,
        │                       binary section decoder)
        │   ALLOC_node_xxx  per AST node
        │   ───────────────►  OPTIMIZE(n)
        │                        │
        │                        ▼
        │                    astro_cs_load(n)   ← dlsym("SD_<hash>")
        │                        │              ← runtime/astro_code_store.c
        │                        ▼
        │                    n->head.dispatcher = SD_…  (if specialized)
        ▼
   AST in heap (linked NODE *)
        │
        ▼
   wastro_invoke(c, fn_idx, args)   ← main.c:5276
        │
        ▼
   EVAL(c, fn->body, frame)          ← node.h:74 (`static inline`)
        │
        ▼   per-node dispatch chain  (node_call_N → node_loop → …)
   RESULT { value, br_depth }
```

Two textual front-ends share one backend.  Once the AST exists, the
WAT parser and the binary `.wasm` decoder are interchangeable —
`wastro_load_module` (`main.c:4622`) inspects the magic and routes
accordingly.  Both paths populate the same module-global tables
(`WASTRO_FUNCS`, `WASTRO_GLOBALS`, `WASTRO_TABLE`, `WASTRO_TYPES`,
`MOD_DATA_SEGS`, `WASTRO_BR_TABLE`) and emit the same node kinds.

## 2. The AST

Every node is a `struct Node` with two parts:

```
NODE
├── struct NodeHead       (always — defined in node.h)
│   ├── flags             { has_hash_value, is_specialized,
│   │                       is_specializing, is_dumping, no_inline, … }
│   ├── kind              → const struct NodeKind *  (vtable-ish)
│   ├── parent            → enclosing NODE (optional, for diagnostics)
│   ├── hash_value        node_hash_t — Merkle structural hash, lazy
│   ├── dispatcher_name   "DISPATCH_node_i32_add" or "SD_<hash>"
│   ├── dispatcher        node_dispatcher_func_t — the actual fn ptr
│   └── …
└── union body            (kind-specific — generated from node.def)
    ├── struct node_i32_add_struct { NODE *l; NODE *r; }
    ├── struct node_call_2_struct  { uint32_t func_index;
    │                                uint32_t local_cnt;
    │                                NODE *a0; NODE *a1; NODE *body; }
    └── …
```

The `kind` is a singleton const struct emitted by ASTroGen
(`node_alloc.c`); among its fields are pointers to the default
dispatcher, the specializer, the dumper, and the per-operand layout
descriptors.

`hash_value` is a *Merkle* hash: each node's hash combines its kind id
with the hashes of all its operand children.  Computed lazily on
first call to `HASH(n)` and cached in the node head.  This is the key
to the code store — two distinct call sites for the same `(i32.add
(local.get 0) (i32.const 1))` produce identical hashes and share the
specialized SD\_ symbol.

### Node creation

Every `ALLOC_node_xxx` (generated from `node.def` by ASTroGen) does:

1. `malloc` a NODE big enough for `sizeof(NodeHead) + sizeof(<kind body>)`
2. Wire `head.kind`, `head.dispatcher = DISPATCH_<name>` (the default
   interpreter dispatcher), `head.flags = …`
3. Copy operands into the kind-specific body
4. Call `OPTIMIZE(_n)` — defined in `node.c:45`, which calls
   `astro_cs_load(n, NULL)`.  If a specialized version exists in
   `code_store/all.so`, the dispatcher is patched in place to point
   at `SD_<Horg>` *before the parser ever returns the node*.

So by the time `wastro_load_module` finishes, every node already has
its specialized dispatcher installed if one was on disk.  No JIT pass
is needed at run time.  When `--no-compile` is given, `OPTIMIZE` is a
no-op and every node stays on its default dispatcher.

## 3. Dispatch model

ASTroGen splits each node-kind's runtime behaviour across three
distinct C functions, all named after the node kind.  Mixing them up
is easy, so this section spells the split out before §4.

| Symbol                | Where written                  | Signature (`local.get $i` example)                                              | Role |
|-----------------------|--------------------------------|----------------------------------------------------------------------------------|------|
| `EVAL_node_xxx`       | `node.def` (user)              | `RESULT EVAL_node_local_get_i32(CTX *c, NODE *n, slot *frame, uint32_t index)`  | The *evaluator* — the actual wasm semantics.  Operands are normal C parameters.  `static inline __attribute__((always_inline))` so it folds into whatever calls it. |
| `DISPATCH_node_xxx`   | generated (`node_dispatch.c`)  | `RESULT DISPATCH_node_local_get_i32(CTX *c, NODE *n, slot *frame)`              | Thin wrapper.  Unpacks operands from the node body and forwards to `EVAL_node_xxx`.  This is the function whose pointer initially lives in `n->head.dispatcher`. |
| `SD_<hash>`           | generated, AOT-compiled        | `RESULT SD_<hash>(CTX *c, NODE *n, slot *frame)`                                 | Specialized dispatcher.  Replaces `DISPATCH_node_xxx` after `astro_cs_load`; inlines the operand unpacking *and* the entire sub-tree of child evaluators. |

So the indirection through `n->head.dispatcher` always points at one
of `DISPATCH_node_xxx` or `SD_<hash>`, both of which have the
*three-arg* signature.  Operand reading happens inside.  See
`runtime/astro_node.c` and the `build_eval_dispatch` block of
`lib/astrogen.rb` for the codegen.

The function-pointer type is:

```c
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n,
                                         union wastro_slot *frame);
```

`EVAL` (the macro/function name in `node.h:74`, distinct from
`EVAL_node_xxx`) is the single trampoline that calls through that
pointer.  Defined as `static inline` so specialized SD\_ functions
loaded from `all.so` can call it directly without a PLT bounce back
into the host binary:

```c
static inline RESULT
EVAL(CTX *c, NODE *n, union wastro_slot *frame)
{
    return (*n->head.dispatcher)(c, n, frame);
}
```

The naming convention is deliberate but unforgiving:

- `EVAL(...)` — the trampoline (3 args, calls through head.dispatcher)
- `EVAL_node_xxx(...)` — the user-written evaluator (3 args + operands)
- `DISPATCH_node_xxx(...)` — the generated wrapper (3 args, reads
  operands from `n->u.node_xxx.<field>` and calls `EVAL_node_xxx`)

Concretely, for `local.get $i`:

```c
// User-written, in node.def:
static inline __attribute__((always_inline)) RESULT
EVAL_node_local_get_i32(CTX *c, NODE *n, slot *frame, uint32_t index)
{
    return RESULT_OK(FROM_I32(frame[index].i32));
}

// Generated, in node_dispatch.c:
static __attribute__((no_stack_protector)) RESULT
DISPATCH_node_local_get_i32(CTX *c, NODE *n, slot *frame)
{
    dispatch_info(c, n, 0);
    RESULT v = EVAL_node_local_get_i32(c, n, frame,
                                       n->u.node_local_get_i32.index);
    dispatch_info(c, n, 1);
    return v;
}
```

Under default (un-specialized) execution, every node hop costs one
indirect call (`EVAL` trampoline → `DISPATCH_node_xxx`) plus an
inline-then-folded `EVAL_node_xxx` call inside.  Under specialization,
the SD\_ function for an enclosing node inlines its children's
evaluators (via the same `EVAL_node_xxx` symbols, which are
always-inline), eliminating both the wrapper and most of the indirect
calls along the way.

### `RESULT` — branch-aware return value

```c
typedef struct { VALUE value; uint32_t br_depth; } RESULT;
```

12 bytes, returned in `rax`/`rdx` under SysV x86-64.  This means
neither the produced wasm value *nor* the branch state ever has to
hit memory between dispatcher calls in the hot path.

`br_depth` encodes one of three things:

| value                         | meaning                            |
|-------------------------------|------------------------------------|
| `0`                           | normal flow, `value` is the result |
| `1..N`                        | a `br N` (or fallout) is in flight; pop one label per scope |
| `WASTRO_BR_RETURN` (`0xFFFFFFFF`) | function-level `return` / `unreachable` propagation |

`br_depth` propagation through expression nodes is uniform.  Every
multi-operand `NODE_DEF` evaluates each child via the `UNWRAP` macro:

```c
#define UNWRAP(r) ({                                  \
    RESULT _r = (r);                                  \
    if (__builtin_expect(_r.br_depth != 0, 0))        \
        return _r;          /* propagate to caller */ \
    _r.value;                                         \
})
```

So a `(br N <value>)` deep inside a nested `i32.add` immediately
unwinds the C stack one EVAL frame at a time, carrying `value` and
`br_depth` upward.  `node_block` / `node_loop` / `node_if`
(`node.def:1273-1297`) decrement `br_depth` on the way out and stop
the propagation at depth 0.

The point: structured control flow is implemented entirely as
*return values*.  No `setjmp` is used on the hot path.  (One use of
`setjmp` exists, but only in `--test` mode for `assert_trap`
recovery — see §11.)

## 4. Frames & locals

Wasm locals are caller-allocated:

```c
union wastro_slot {
    int32_t  i32;
    int64_t  i64;
    float    f32;
    double   f64;
    uint64_t raw;
};
```

`frame` (the third positional arg) is threaded through every
dispatcher and evaluator, all the way to the leaves — the
`local.get_<T>` / `local.set_<T>` evaluators in §3 take it directly
and access `frame[index].<T>`.

Why `union` rather than a typed struct?

- *Uniform memory layout.*  An array of 8-byte cells is the same
  regardless of which wasm types the function declares.
- *Per-slot type recovery.*  Each `local.get_<T>` node accesses
  exactly `frame[i].<T>`, giving gcc per-access type information so
  it can SROA the slot back into a register at its real C type when
  the SD chain inlines.

`node_call_N` (`node.def:1672-1762`) allocates the callee's frame as
a C VLA on its own stack:

```c
NODE_DEF
node_call_2(CTX *c, NODE *n, …, NODE *a0, NODE *a1, NODE *body)
{
    union wastro_slot F[local_cnt];
    F[0].raw = UNWRAP(EVAL_ARG(c, a0));
    F[1].raw = UNWRAP(EVAL_ARG(c, a1));
    for (uint32_t i = 2; i < local_cnt; i++) F[i].raw = 0;
    RESULT r = body_dispatcher(c, body, F);
    return RESULT_OK(r.value);
}
```

`body_dispatcher` looks magic but follows a general ASTroGen
convention: for every `NODE *foo` operand declared on a `NODE_DEF`,
the generator quietly adds a sibling `node_dispatcher_func_t
foo_dispatcher` parameter to the `EVAL_node_xxx` signature, and
`DISPATCH_node_xxx` passes `n->u.node_xxx.foo->head.dispatcher` for
it.  The `EVAL_ARG(c, foo)` macro inside `node.def` expands to
`(*foo_dispatcher)(c, foo, frame)` — a direct call through the
sibling parameter, not through `foo->head.dispatcher`.  Most
operators (e.g. `node_i32_add`'s `l` / `r`) use `EVAL_ARG`;
`node_call_N` uses the raw `body_dispatcher` instead because it has
to pass a *new* frame to the callee instead of the current one.

Why this rather than just dispatching through `foo->head.dispatcher`?
Because when the parent's `SD_<hash>` is specialized, the operand's
sibling dispatcher parameter is materialized at the call site as a
known, named symbol (`SD_<child_hash>`).  gcc sees a direct call and
can inline transitively across nodes — the wrapper hop and most of
the indirect calls collapse into one SD\_ function.  At default
(unspecialized) execution the same parameter just carries
`DISPATCH_node_xxx`, an indirect call.

`CTX::stack` is reserved (64K `VALUE` slots) for the spec-test
harness path and as a safety net, but in the hot interpreter path it
is *not* used as the primary value stack.  Real wasm operands are
passed via the C call stack as `RESULT` return values.

## 5. Function call dispatch

Wasm `call` takes a variable number of operands.  ASTro's `NODE_DEF`
emits a fixed-shape struct per node, so wastro splits the dispatch
across two tiers:

| Node                       | Arity  | Notes                                         |
|----------------------------|--------|-----------------------------------------------|
| `node_call_0..4`           | 0–4    | direct call, declared `NODE *` operands → sibling-dispatcher specialization |
| `node_call_indirect_0..4`  | 0–4    | `call_indirect` via `WASTRO_TABLE[idx]`       |
| `node_host_call_0..3`      | 0–3    | host-imported function                        |
| `node_call_var`            | 5–1024 | direct, operand sub-trees in `WASTRO_CALL_ARGS[args_index..+args_cnt]` |
| `node_call_indirect_var`   | 5–1024 | `call_indirect`, same args store              |
| `node_host_call_var`       | 4–1024 | host import                                   |

The fast tier (0..4) is what the typical wasm module hits — the
arguments live as direct `NODE *` operands of the call node, so under
specialization the per-operand dispatcher pointers materialize as
direct calls and the chain inlines into the caller's `SD_<hash>`.

The var tier handles the long tail of large-arity functions (1024-param
forms, autogenerated bridges) without exploding the node-kind set
into 1024 fixed shapes.  Operand sub-trees go into a module-global
`NODE **WASTRO_CALL_ARGS` flat array, addressed at run time by
`(args_index, args_cnt)` — the same pattern `WASTRO_BR_TABLE` uses for
`br_table`.  Each operand is dispatched via its own
`head.dispatcher`, so the var tier loses transitive sibling inlining
but accepts arbitrary arity.

The parsers select between the two by `argc <= 4` (or `<= 3` for host).
The hard cap on per-function param count is `WASTRO_MAX_PARAMS = 1024`
(see `context.h`), enforced at parse time; the per-function
`param_types` / `local_types` storage itself is heap-allocated to
exact size, no fixed-worst-case array.

### Indirect calls

`call_indirect (type $sig) <args>... <idx>` performs:

1. `WASTRO_TABLE[idx]` lookup → callee function index, or trap on
   OOB / `-1` slot
2. Resolve `&WASTRO_FUNCS[fn_idx]`
3. Structural sig compare against `WASTRO_TYPES[type_index]` —
   trap on mismatch
4. If `fn->is_import`, pack args + call `fn->host_fn`
5. Otherwise allocate a frame VLA, copy args, call body dispatcher

Step 3 is unavoidable per spec — the runtime sig check is the
documented behavior of `call_indirect`.

### Host imports

Imports use a simple host registry: `env.log_i32`, `env.putchar`,
`env.print_bytes`, etc.  Bindings are wired by name at module-load
time.  An import with no host binding installs a stub that traps
when called — this lets a module with placeholder imports load
successfully (you only fail if you actually invoke the unbound
import).

## 6. Linear memory

A wasm module owns at most one linear memory (1.0 limit).  At
instantiation (`wastro_instantiate`, `main.c:5222`) wastro:

1. `mmap` an 8 GB virtual reservation as `PROT_NONE` —
   addressable but inaccessible.
2. `mprotect` the initial pages (`MOD_MEM_INITIAL_PAGES * 64KB`) as
   `PROT_READ|PROT_WRITE`.  These are zero-filled lazily by the
   kernel on first touch.
3. Copy `(data ...)` segments into the active region.
4. Install a SIGSEGV handler that converts a fault inside the
   reservation into `wastro_trap("out of bounds memory access")`.

Why not bounds-check explicitly in every load/store node?  Because
SIGSEGV is free in the common (in-bounds) case and the load/store
generated code becomes a single `mov` after specialization.  The
slow path (out-of-bounds) is rare and already exceptional.

`memory.grow` extends the mprotect window in place — the virtual
reservation already covers up to 8 GB, so growth is just a syscall
and a cached-size update.  No realloc, no relocation of pointers
held by host imports.

```c
typedef struct CTX_struct {
    VALUE   stack[64*1024];          // (rarely used in hot path)
    VALUE  *fp, *sp;                 // (rarely used in hot path)
    uint8_t *memory;                 // wasm linear memory
    uint32_t memory_pages;
    uint32_t memory_max_pages;
    uint64_t memory_size_bytes;      // cached pages*64K — bounds check is one cmp
} CTX;
```

## 7. Module-global state

The parser populates several module-global arrays.  Each one is
referenced from generated dispatchers via `extern` so that
specialized SD\_ symbols can resolve them at `dlopen` time (the host
binary is linked `-rdynamic`):

| Array                  | Used by                                 |
|------------------------|-----------------------------------------|
| `WASTRO_FUNCS[]`       | every direct/indirect/host call         |
| `WASTRO_TABLE[]`       | `call_indirect` slot lookup             |
| `WASTRO_TYPES[]`       | `call_indirect` runtime sig compare     |
| `WASTRO_GLOBALS[]`     | `global.get` / `global.set`             |
| `WASTRO_BR_TABLE[]`    | `br_table` — flat target-depth array    |
| `MOD_DATA_SEGS[]`      | `(data ...)` initialisation             |

`WASTRO_BR_TABLE` deserves a note.  A `(br_table L0 L1 … LN_default)`
instruction would naively need a per-node array of `uint32_t` depths.
Instead, we store all targets contiguously in one module-global
array; each `node_br_table` records `(target_index, target_cnt,
default_depth)` only.  This keeps the `NodeHead`-plus-body struct
small and uniform for the specializer.

## 8. Module instantiation

```
INIT()                          ; node.c:91 — astro_cs_init("code_store", ".", 0)
   loads code_store/all.so via dlopen if present

wastro_load_module(path)        ; main.c:4622
   ├── magic detection           (first 4 bytes 0x00 0x61 0x73 0x6D ⇒ binary)
   ├── parser populates globals  (WASTRO_FUNCS / WASTRO_TYPES / WASTRO_BR_TABLE / …)
   └── per node: ALLOC_node_xxx → OPTIMIZE → astro_cs_load patches dispatcher

wastro_instantiate(initial_locals)  ; main.c:5222
   ├── alloc CTX
   ├── mmap 8 GB PROT_NONE; mprotect initial pages R/W
   ├── install SIGSEGV handler (OOB → wastro_trap)
   └── memcpy (data ...) segments

if MOD_HAS_START: wastro_invoke(c, MOD_START_FUNC, NULL, 0)

if user gave <export>:           ; main.c:5402
   parse args by fn->param_types
   wastro_invoke(c, fn_idx, args, argc) → printf result
```

`wastro_invoke` (main.c:5276) is the single entry into the AST.  It
allocates the entry frame (params plus zero-initialised body locals),
calls `EVAL(c, fn->body, F)`, and unwraps the returned RESULT.
Because `WASTRO_BR_RETURN` is a sentinel `br_depth` value (not a
label depth), it's only consumed at this boundary — `block` / `loop`
/ `if` propagate it upward unchanged.

## 9. Code store / specialization

This is the part that makes wastro fast.  See also the README's
*Specializer-friendly EVAL* note.

### Names and files

```
code_store/
├── c/
│   ├── SD_<hash1>.c          ← generated specialized dispatchers
│   └── SD_<hash2>.c
├── o/SD_<hash>.o             ← built artefacts
├── all.so                    ← dlopen'd by astro_cs_init / astro_cs_reload
└── Makefile                  ← runtime/Makefile.cs, parallel `make -j`
```

Each SD\_ name is `"SD_" + hex(HASH(node))`.  The hash is structural:
two distinct call sites for the same sub-tree share one SD\_.

### The flow

```
astro_cs_compile(entry_node, NULL)
   ├── walks the AST
   ├── for each unspecialized node: writes SD_<hash>.c that
   │      inlines the full sub-tree of dispatchers
   └── astro_spec_dedup keeps one emission per hash per session

astro_cs_build(NULL)
   ├── Make builds every SD_*.c → o/SD_*.o
   └── links into all.so

astro_cs_reload()
   ├── dlclose(old all.so)
   └── dlopen(new all.so)        — symbols become callable

astro_cs_load(node, NULL)
   ├── HASH(node) → "SD_<hash>"
   ├── dlsym(all.so, "SD_<hash>")
   ├── if found: node->head.dispatcher = dlsym_result
   │            node->head.flags.is_specialized = true
   │            return true
   └── else: leave dispatcher = DISPATCH_<kind>; return false
```

### When does specialization happen?

| Mode             | When                                                         |
|------------------|--------------------------------------------------------------|
| Default (lazy)   | `OPTIMIZE` is called from every `ALLOC_node_xxx` during parse — picks up SD\_ symbols already in `all.so` |
| `-c` / `--aot-compile-first` | `compile_all_funcs` runs `astro_cs_compile` on every function body, then `astro_cs_build` + `astro_cs_reload`, then `load_all_funcs` patches every dispatcher.  See `main.c:5191` |
| `--aot`          | Compile step only; exits without running |
| `--no-compile`   | `OPTIMIZE` becomes a no-op; everything runs on default dispatchers |

In default mode, a re-run after a previous AOT compile reads SD\_
symbols transparently — the parser-time `OPTIMIZE` finds them and
patches.  This is why a cold `--clear-cs -c` run pays compile cost
once, but every later `./wastro` invocation runs at SD\_ speed
without recompiling.

### Why `static inline EVAL`?

The chain `SD_<outer> → EVAL → <child dispatcher>` is what gets
inlined.  If `EVAL` were out of line in `node.h` and lived in the
host binary, every dispatch from inside `all.so` would have to go
through the PLT.  With `static inline`, gcc inlines `EVAL` directly
into each generated SD\_, so a chain like

```
SD_for_loop_body
  → SD_local_set_i32
       → SD_i32_add
            → SD_local_get_i32
            → SD_i32_const
```

collapses into a single function with no inter-`.so` calls at all.
The frame slots SROA into registers across the entire collapsed
chain.

## 10. Trap handling

`wastro_trap(msg)` (`__attribute__((noreturn))`) is called by:

- explicit `unreachable`
- bounds violations (load/store, `memory.grow`, `data` init OOB)
- `i32.div_s` / `i64.div_s` `INT_MIN / -1` and any divide by zero
- non-finite-to-int conversions (`trunc_*`)
- `call_indirect` OOB index, null slot, or sig mismatch
- unbound host imports

Default behaviour: print `msg` to stderr and `exit(1)`.

In `--test` mode, the harness installs a `setjmp`-based recovery
handler (`wastro_trap_active`).  Each `(assert_trap (invoke …) "msg")`:

1. `setjmp(env)`; mark `wastro_trap_active = true`
2. Run the invocation through `wastro_invoke`
3. If it returns normally → assertion *failed*
4. If `wastro_trap` calls `longjmp(env)` → assertion *passed*

This is the only place `setjmp` appears in the runtime.  It's
guarded by the `--test` flag and disabled in production runs so the
hot path stays branch-prediction-friendly.

## 11. End-to-end: tracing `fib(2)`

`examples/fib.wat`:

```wat
(func $fib (param $n i32) (result i32)
  (if (result i32) (i32.lt_s (local.get $n) (i32.const 2))
    (then (local.get $n))
    (else (i32.add
            (call $fib (i32.sub (local.get $n) (i32.const 1)))
            (call $fib (i32.sub (local.get $n) (i32.const 2)))))))
```

Run: `./wastro --no-compile -q examples/fib.wat fib 2`

Steps:

1. `INIT()` calls `astro_cs_init` — no `code_store/` exists yet
   (we cleared it in this thought experiment), so no `all.so`.
2. `wastro_load_module("examples/fib.wat")`:
    - Tokenize.  Recognise `(func $fib ...)`.
    - Parse the body folded-style; build:
      ```
      node_if(
        cond = node_i32_lt_s(node_local_get_i32(0), node_i32_const(2)),
        then = node_local_get_i32(0),
        else = node_i32_add(
                 node_call_1(fib_idx, locals=1, a0=node_i32_sub(…), body=fib_body),
                 node_call_1(fib_idx, locals=1, a0=node_i32_sub(…), body=fib_body)))
      ```
    - Each `ALLOC_node_xxx` calls `OPTIMIZE`, which calls
      `astro_cs_load`.  No `all.so`, every node keeps its default
      dispatcher.
3. `wastro_instantiate(1)` allocates `CTX *c`.  No `(memory)` in
   `fib.wat`, so `c->memory = NULL`.
4. `wastro_invoke(c, fib_idx, [FROM_I32(2)], 1)`:
    - Allocate a 1-slot frame `F[1]`; `F[0].raw = 2`.
    - `EVAL(c, fib_body, F)` calls `DISPATCH_node_if`:
        - cond → `EVAL` `node_i32_lt_s` →
          `EVAL_ARG(node_local_get_i32(0))` returns `RESULT_OK(2)`,
          `EVAL_ARG(node_i32_const(2))` returns `RESULT_OK(2)`.
          Result: `2 < 2` is false, `cv = 0`.
        - Take else branch: `EVAL` `node_i32_add`.
            - First operand: `node_call_1` with `a0 = (i32.sub (n) 1)`.
                - Frame VLA of 1 slot, `F'[0].raw = AS_I32(2 - 1) = 1`
                - `EVAL(c, fib_body, F')` recursively → returns `1`.
            - Second operand: another `node_call_1` with arg `0` →
              recursively → returns `0`.
            - Add: `1 + 0 = 1`.
        - `node_if` sees no in-flight branch (`r.br_depth == 0`),
          returns the value directly.
    - `wastro_invoke` reads `r.value`, `printf("%d\n", 1)`.
5. Output: `1` (correct: `fib(2) = 1`).

If we re-ran with `-c`, the parser-time `OPTIMIZE` would patch the
top-level `node_if`'s dispatcher to `SD_<hash_of_if>`, and the entire
sub-tree of EVAL calls would collapse into one specialized C
function: the `union wastro_slot F[local_cnt]` allocations vanish,
the `node->u.…` field reads vanish, the recursive call boundary
becomes a direct C call to `SD_<hash_of_fib_body>` (which is itself).
That's how wastro reaches its measured fib performance.

## 12. Where to look in the source

| Concern                          | File                                   |
|----------------------------------|----------------------------------------|
| Per-node `EVAL_node_xxx` bodies (semantics) | `node.def`                  |
| Generated `DISPATCH_node_xxx` wrappers      | `node_dispatch.c` (built)   |
| AST struct + `EVAL` trampoline + `UNWRAP`   | `node.h`                    |
| Driver, traps, linear memory, module-state arrays | `main.c`             |
| WAT lexer + token-to-bits / decode-string helpers | `wat_tokenizer.c`    |
| WAT folded + stack-style parser, `(func ...)` driver | `wat_parser.c`    |
| Inline `(export ...)` / `(import ...)` helpers | `wat_parser.c`             |
| `env.*` / `spectest.*` host import table   | `host_imports.c`             |
| Binary `.wasm` decoder           | `wasm_decoder.c`                       |
| Spec-test (`.wast`) harness      | `wast_runner.c`                        |
| Code store (specialize/build/load) | `runtime/astro_code_store.{c,h}`     |
| Hash + DUMP                       | `runtime/astro_node.c`                 |
| Code-generation logic             | `lib/astrogen.rb`                      |
| Wastro's per-language ASTroGen ext| `wastro_gen.rb`                        |

The five front-end TUs (`wat_tokenizer.c`, `host_imports.c`,
`wat_parser.c`, `wasm_decoder.c`, `wast_runner.c`) are `#include`'d
from `main.c` rather than compiled separately — same single-TU
pattern that node.c uses for the ASTroGen-generated dispatchers.
This keeps the build command a one-liner and lets every front-end
file see all module-state arrays without forward declarations.

The `Makefile` invokes ASTroGen via `wastro_gen.rb` to produce the
`node_*.c` / `node_head.h` files (dispatchers, hashers, dumpers,
specializers, allocators).  Those generated files are #include'd
from `node.c` so all per-kind behaviour lands in one TU and the
default dispatchers are visible to ASTroGen's specializer when it
emits SD\_ source.
