# luastro runtime — software architecture

This document describes how luastro represents and executes Lua 5.4
source.  It is the longer-form companion to the README's *Architecture
notes* section: every point made there is expanded here, with concrete
file/symbol anchors so the reader can dive into the source.

## 1. Pipeline at a glance

```
   foo.lua
       │
       ▼
   tokenizer        (lua_tokenizer.c — full Lua 5.4 lexical syntax)
       │
       ▼
   parser           (lua_parser.c — recursive-descent + Pratt;
       │                            captured-local rewriter)
       │   ALLOC_node_xxx per AST node
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
   lua_call(c, chunk, NULL, 0)          ← lua_runtime.c
       │
       ▼
   EVAL(c, body, frame)                  ← node.c:60
       │
       ▼   per-node dispatch chain  (node_seq → node_call_N → …)
   RESULT (== LuaValue)   +   LUASTRO_BR (global control-flow flag)
```

Once the AST exists, every node already has either its default
`DISPATCH_<name>` dispatcher or, if a matching SD was found in the code
store, an `SD_<hash>` dispatcher.  The interpreter then walks the tree
by calling `n->head.dispatcher(c, n, frame)` at every step.

## 2. The AST

Every node is a `struct Node` with two parts:

```
NODE
├── struct NodeHead       (always — defined in node.h)
│   ├── flags             { has_hash_value, is_specialized, … }
│   ├── kind              → const struct NodeKind * (vtable-ish)
│   ├── parent            → enclosing NODE (optional, for diagnostics)
│   ├── hash_value        node_hash_t — Merkle structural hash, lazy
│   ├── dispatcher_name   "DISPATCH_node_int_add" or "SD_<hash>"
│   ├── dispatcher        node_dispatcher_func_t — the actual fn ptr
│   └── …
└── union body            (kind-specific — generated from node.def)
    ├── struct node_int_add_struct  { NODE *l; NODE *r; }
    ├── struct node_call_arg2_struct{ NODE *fn; NODE *a0; NODE *a1; }
    ├── struct node_numfor_struct   { uint32_t var_idx; NODE *start;
    │                                 NODE *limit; NODE *step;
    │                                 NODE *body; }
    └── …
```

The kind set lives in `node.def`.  Highlights:

- **Arithmetic**: `node_int_add/_sub/_mul`, `node_flt_add/_sub/_mul/_div`,
  generic `node_arith` (variable op).  The `int_*` and `flt_*` variants
  are speculative — they assume the hot path type and fall back to
  `lua_arith` on type mismatch.
- **Control flow**: `node_seq`, `node_if`, `node_while`, `node_repeat`,
  `node_numfor`, `node_genfor`, `node_break`, `node_return`,
  `node_goto`, `node_label`.
- **Locals and captures**: `node_local_get/_set`, `node_box_get/_set`
  (for captured locals), `node_upval_get/_set`.
- **Calls**: `node_call_arg{0,1,2,3}` (fixed-arity fast paths) and
  `node_call_argN` (variable arity).  All ultimately go through
  `luastro_inline_call` / `lua_call`.
- **Specialized fused nodes**: `node_numfor_int_sum` recognises the
  classic `for i=1,N do sum = sum + i end` pattern at parse time and
  emits a tight scalar-only loop body.

### Node creation

Every `ALLOC_node_xxx` (generated from `node.def` by ASTroGen) does:

1. `calloc` a NODE big enough for `sizeof(NodeHead) + sizeof(<kind body>)`
2. Wire `head.kind`, `head.dispatcher = DISPATCH_<name>`, `head.flags`
3. Copy operands into the kind-specific body
4. Call `OPTIMIZE(_n)` (`node.c`), which calls `astro_cs_load(n, NULL)`
   to patch the dispatcher to a specialized SD if one is on disk.

So when the parser returns, every node already has its specialized
dispatcher installed if a matching one exists in `code_store/all.so`.

## 3. LuaValue — 8-byte tagged word

`LuaValue` is a single `uint64_t`.  Encoding is CRuby-inspired:

```
  bits 2..0 :
    000   → pointer to a heap object (LuaString / LuaTable / LuaClosure /
                                      LuaCFunction / LuaCoroutine /
                                      LuaBox / LuaHeapDouble).
            Object kind read from GCHead.type at offset 0 of the object.
    001   → fixnum.  Bits 63..1 are the signed 63-bit integer value.
    010   → inline flonum (RESERVED — not yet emitted; see §10).
  Whole-word singletons:
    0x00  → nil           (so calloc-zeroed memory is naturally nil)
    0x14  → false
    0x24  → true
  Special:
    0x8000000000000002    → +0.0 sentinel for inline flonum (TBD)
```

The tag is examined with predicates only — there is no runtime tag
field to load.  Common predicates:

```c
LV_IS_PTR(v)      ((v) & 7) == 0 && (v) != 0
LV_IS_INT(v)      ((v) & 1) != 0
LV_IS_NIL(v)      (v) == 0
LV_IS_BOOL(v)     (v) == LV_FALSE_BITS || (v) == LV_TRUE_BITS

// Heap-object subtype: a single byte load through the pointer.
lv_heap_type(v)   (*(const uint8_t *)(uintptr_t)(v))
LV_IS_HEAP_OF(v, T) (LV_IS_PTR(v) && lv_heap_type(v) == (T))

LV_IS_STR(v)      LV_IS_HEAP_OF(v, LUA_TSTRING)
LV_IS_TBL(v)      LV_IS_HEAP_OF(v, LUA_TTABLE)
LV_IS_FN(v)       LV_IS_HEAP_OF(v, LUA_TFUNC)
LV_IS_CF(v)       LV_IS_HEAP_OF(v, LUA_TCFUNC)
LV_IS_FLOAT(v)    LV_IS_FLONUM(v) || LV_IS_HEAP_OF(v, LUA_TFLOAT)
LV_IS_NUM(v)      LV_IS_INT(v) || LV_IS_FLOAT(v)
LV_IS_CALL(v)     pointer to LUA_TFUNC or LUA_TCFUNC
```

### Why this layout

- **Nil at zero** lets `calloc` initialise hash-table slots and frame
  arrays correctly without an explicit fill pass.
- **Heap-type byte at object offset 0** means subtype dispatch is one
  cache-line load through the pointer — no need to consult a tag in the
  LuaValue itself.
- **Fixnum bit 0 = 1** means addition / subtraction can use plain
  signed integer ops on the encoded form (then re-tag); sign extension
  via arithmetic right shift gives the original int back.

### Doubles — currently always heap-boxed

For v1 every double goes through a `struct LuaHeapDouble { GCHead;
double }` allocation.  `LUAV_FLOAT(d)` calls `luav_from_double` (out of
line in `lua_runtime.c`), which `calloc`s the cell, registers it with
the GC as `LUA_TFLOAT`, and returns the pointer-as-LuaValue.

This is **the dominant performance bottleneck** for float-heavy code
(mandelbrot / nbody).  The intended replacement is CRuby-style inline
flonum: doubles whose exponent bits sit in a moderate range encode
inline with a 0x02 tag, with `LuaHeapDouble` as fallback for denormals
/ extreme magnitudes.  Listed under §10 future work.

## 4. Branch state — `LUASTRO_BR`

Every EVAL function returns a `RESULT`, which is currently just a
typedef for `LuaValue` (a single 8-byte register return).  Control flow
is propagated through two thread-global variables:

```c
extern uint32_t LUASTRO_BR;       // LUA_BR_{NORMAL,BREAK,RETURN,CONTINUE,GOTO}
extern LuaValue LUASTRO_BR_VAL;   // payload for return / goto target
```

`RESULT_RETURN_(v)`, `RESULT_BREAK_()`, `RESULT_CONTINUE_()` are macros
that set the globals via the comma operator and also return the value
in `rax` for callers that consume it.

Loops, `node_seq`, `node_call_*` check `LUASTRO_BR` after each child
EVAL and propagate the branch upwards (returning early).  At the
function-call boundary (`lua_call` / `luastro_inline_call`) a `RETURN`
is converted back to `NORMAL`, since it's been consumed.

### Why globals instead of a struct return

An earlier draft returned `struct { LuaValue value; uint32_t br; }` —
SysV x86_64 puts that in `rax+rdx`, theoretically removing the global
load/store on every loop iteration.  The current code falls back to
globals because the partial port of that change introduced too many
churn points across `node.def` and the generated dispatcher to land
cleanly under a single iteration.  Re-doing it is on the roadmap.

## 5. Frames and captured locals

`CTX::stack` is a flat `LuaValue *` arena (4 MiB by default).  When
`lua_call` invokes a closure it pushes `cl->nlocals` slots onto `sp`,
zero-fills them (so unused slots read as nil), copies args into the
first `cl->nparams`, and runs `EVAL(c, cl->body, frame)`.

### Captured locals — `LuaBox`

If the parser sees that an inner closure references a local of an outer
function, every read/write of that local is rewritten from
`node_local_get/_set` to `node_box_get/_set`.  At first access the
slot's value is moved into a heap-allocated `LuaBox`:

```c
struct LuaBox {
    struct GCHead gc;     // gc.type == LUA_TBOXED
    LuaValue      value;
};
```

The frame slot is rewritten to store a tagged pointer to that box;
`LV_IS_BOX(slot)` true thereafter.  All inner closures that captured
this slot reference the same `&box->value`, so writes from any of them
are observed by the others.  See `luastro_ensure_box` in `node.c`.

### Closures

```c
struct LuaClosure {
    struct GCHead gc;
    struct Node *body;
    uint32_t nparams, nlocals, nupvals;
    bool     is_vararg;
    const char *name;
    LuaValue **upvals;    // each entry points into some enclosing frame
                          // OR into a LuaBox::value (after escape).
};
```

`upvals[i]` is a pointer-to-LuaValue, so reads and writes go through
the box automatically.  `lua_closure_with_upvals` snapshots the upval
pointer array when the closure is constructed; subsequent writes to
the parent's locals are visible because everyone shares the same box.

## 6. Function call

`lua_call(c, fn, args, argc)` is the general entry point.  It handles:

- `__call` metamethod chain (table → metatable lookup, prepend table
  as first arg, retry).
- C functions (`LV_IS_CF(fn)` → invoke `cf->fn(c, args, argc)`).
- Lua closures: allocate frame on `c->sp`, copy args, set varargs
  pointer if applicable, set `LUASTRO_CUR_UPVALS = cl->upvals`,
  `EVAL` the body.

`luastro_inline_call(c, fn, nargs, argv)` is the fast path used by
`node_call_arg{0,1,2,3}` for the common case "Lua closure called with
the right fixed-arity args, no varargs, no metamethod".  Lives in
`lua_runtime.c` (out of line, single shared symbol across the host
binary and SD shared object — earlier inline-everywhere variants caused
sp leaks across SD↔host boundaries).

Multi-return is staged in `c->ret_info.results[]`; the primary value
(first return) is also returned in `rax` via `RESULT`.

## 7. Tables — hybrid array + hash

```c
struct LuaTable {
    struct GCHead       gc;         // gc.type == LUA_TTABLE
    LuaValue           *array;
    uint32_t            arr_cnt, arr_cap;
    struct LuaTabEntry *hash;       // open-addressing
    uint32_t            hash_cap, hash_cnt;
    struct LuaTable    *metatable;
};
```

Integer keys 1..arr_cnt go directly into the array part.  An integer
key beyond `arr_cnt + 1` (or non-integer keys) goes to the hash part;
hash lookups use `lua_value_hash` (FNV-mixed for strings, multiplied
by a Knuth constant for ints / pointers).

`lua_table_seti` promotes a hash entry back to the array part if the
new key is contiguous with the current array tail.  Shrinking happens
implicitly on nil-at-boundary writes.

Metatables are looked up at the call site: `lua_table_get` does NOT
consult `__index` automatically — that's the responsibility of
`node_field_get`, which handles the Lua-spec chain (raw → metatable
`__index` is table? recurse : function? call).

## 8. Strings — interned

Every distinct byte sequence has exactly one `LuaString *`.  Equality
is pointer identity (`a == b`); hashing is precomputed at intern time.
The pool is an open-addressing set keyed by `(hash, len, bytes)` —
`lua_str_intern_n` looks the bytes up, allocating only on miss.

The intern pool is a process-wide global (no per-context isolation)
which simplifies cross-CTX string handoff (e.g. coroutines).

## 9. GC — stop-the-world mark-sweep

`luastro_gc_collect(c)` runs:

1. **Mark roots** — walk `c->stack` (boxed slots are followed via the
   pointer in the slot), globals, `ret_info`, varargs, last_error,
   current closure's upvalue pointers.  Recursive mark via
   `mark_value` switches on `lv_heap_type(v)`.
2. **Sweep table weak entries** — for tables with `gc.weak_mode != 0`,
   drop hash entries whose weak side became unreachable.
3. **Finalize** — for unmarked tables with `__gc` metamethod, invoke
   the finalizer before freeing.
4. **Sweep** — walk the all-objects linked list (`G_GC_HEAD`), free
   every object whose `gc.mark == 0`, and clear marks on the rest.

Triggered manually via `collectgarbage("collect")`; an automatic
threshold-based trigger is sketched (`G_GC_TRIGGER`) but not wired up
in v1.

Strings are not freed even when unmarked (the intern pool retains
them).  All other heap objects are reclaimed.

### Weak tables

`setmetatable(t, { __mode = "k" | "v" | "kv" })` sets `t->gc.weak_mode`
to 1 / 2 / 3.  The mark phase skips weak sides; `sweep_table_weak_entries`
nulls entries whose weak side became unreachable.

## 10. Coroutines — ucontext-based

Each `struct LuaCoroutine` owns:

- A `ucontext_t` and a 256 KiB `mmap`'d C stack.
- A `transfer[16]` slot pair for yield/resume payload.
- A `prev` pointer chaining resumers (for status="normal").
- `status` ∈ {SUSPENDED, RUNNING, NORMAL, DEAD}.

`luaco_resume(c, co, args, argc)` `swapcontext`s into the coroutine.
`luaco_yield(c, args, argc)` swaps back to the resumer.  The coroutine
body runs inside a `setjmp`'d pcall frame so exceptions raised inside
become a `(false, msg)` resume return.

The current implementation has a known wart: `coroutine.wrap` raises
an error rather than returning a callable proxy.  Use
`coroutine.resume` directly.

## 11. AOT / code store

The AOT path mirrors naruby's:

```
./luastro -c foo.lua
   │
   ├──▶ astro_cs_compile(chunk_root)        — emit code_store/c/SD_<h>.c
   ├──▶ for each LuaClosure body (registered at construction time):
   │       astro_cs_compile(body)
   ├──▶ astro_cs_build()                    — gcc -shared → all.so
   ├──▶ astro_cs_reload()                   — dlopen(all.so)
   ├──▶ astro_cs_load(chunk_root)           — re-resolve dispatchers
   └──▶ run as usual; every specialized node now jumps to its SD_<h>.
```

The Merkle hash on every node makes the SD lookup content-addressable:
two distinct call sites for the same `(int_add (local_get 0) (int 1))`
produce the same hash and share the SD.  This means:

- A baked `code_store/` is reusable across runs of the same program.
- It's also reusable across *different* programs that happen to share
  AST shapes — the global cache builds up over time.

`--no-compile` skips the load entirely (every node stays on its
default `DISPATCH_<name>`).  `-p / --pg-compile` runs first (so
profile-conditional `swap_dispatcher` choices get baked) and then
emits SDs.

## 12. Threading model

The chunk runs on a `pthread` with a **4 GB virtual stack** (set via
`pthread_attr_setstacksize`).  Reason: SD frames can be ~1 KB each for
non-trivial closures; deeply recursive programs (Ackermann etc.) blow
through the default 8 MB easily.  The 4 GB allocation is virtual, so
unused pages are never touched.  If `pthread_create` fails the driver
falls back to running on the main stack.

`main.c::luastro_run_on_big_stack` plumbs this together.

## 13. Limitations and future work

| Item                                | Status                                                   |
|-------------------------------------|----------------------------------------------------------|
| Inline flonum                       | Reserved tag (`010`); always heap-boxes today            |
| 2-value RESULT (`rax+rdx`)          | Globals used instead; refactor pending                   |
| `goto` / labels                     | Parsed; `BR_GOTO` propagates up; no label scanner        |
| `xpcall` message handler            | Caught but handler ignored                               |
| `coroutine.wrap`                    | Raises; use `coroutine.resume` directly                  |
| Local-scope shrink on `do…end` exit | Not implemented; conservative w.r.t. GC                  |
| Type-speculating SDs                | All SDs include the type guard from the source EVAL      |
| Float-only specialized loops        | Only `node_numfor_int_sum` exists; `_flt_sum` would help |

The two highest-leverage items are **inline flonum** (mandelbrot /
nbody depend entirely on this) and **type-speculating specialization
at SD bake time** (would let `int_add` SDs drop the `if(LV_IS_INT)`
guard, bringing tight numeric loops within striking distance of
LuaJIT's interpreter mode).
