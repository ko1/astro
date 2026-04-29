# ascheme runtime — software architecture

ascheme is an R5RS Scheme implementation built on the ASTro framework.
This document describes how a `.scm` source file becomes a running
program — from reader to AST, through the dispatcher chain, into the
optional AOT (and PGO) code-store, and back to the evaluator's
trampoline.

For the user-facing tour see [`../README.md`](../README.md); the
ASTro framework as a whole is described in [`../../../docs/idea.md`](../../../docs/idea.md).

## 1. Pipeline at a glance

```
   .scm source
        │
        ▼
   reader (main.c)            S-expression → tagged scheme VALUEs
        │
        ▼
   compile() (main.c)         scheme VALUE → AST
        │   ALLOC_node_xxx         node_lambda, node_call_K, node_arith_*, …
        │   ───────────────►   OPTIMIZE(n)    ← opportunistic SD_<hash> lookup
        │                                        in code_store/all.so
        ▼
   AST in heap (linked NODE *, GC-managed)
        │
        ▼
   eval_top(c, ast)            EVAL(c, n) = (*n->head.dispatcher)(c, n)
        │
        ▼   per-node dispatch chain (node_seq → node_arith_add → …)
   VALUE  (return value of the top-level form)

   Closure calls go through `scm_apply`, which trampolines TCO via
   c->tail_call_pending / next_body / next_env.
```

Three execution modes share the same AST and the same generated
dispatchers:

| Mode | What `code_store/` contains | Hot-path dispatch |
|---|---|---|
| **interp** | empty | `DISPATCH_node_xxx` (function pointer per AST edge) |
| **AOT** (`-c`) | `SD_<hash>.so` for every entry | `SD_<hash>` (children inlined as static SD calls within the same .so) |
| **PGO** (`--pg-compile`) | `SD_<hash>.so` only for entries above threshold + `profile.txt` | hot entries: `SD_<hash>`; cold: default `DISPATCH_node_xxx` |

`make compare` (and `make compare-big`) tabulates wall-clock for each
mode plus chibi-scheme and guile.

## 2. Value representation

`VALUE` is `int64_t` with three tag classes — Ruby-style:

```
xxxx_xxx1  fixnum (signed 62-bit, value = (int64_t)v >> 1)
xxxx_xx10  flonum (IEEE-754 double encoded inline; bit-rotation; CRuby's scheme)
xxxx_x000  pointer to heap-allocated `struct sobj` (8-byte aligned)
```

Inline flonums round-trip every IEEE double whose top three exponent
bits are `0b011` or `0b100` — roughly `[1e-77, 1e+77]` in magnitude.
0.0, NaN, ±inf, and out-of-range magnitudes fall back to a heap
`OBJ_DOUBLE`.  See [`mandel`](../bench/big/mandel.scm) and
[`nbody`](../bench/big/nbody.scm) for the impact: removing the flonum
allocation in their inner loops gave 6×/1.7× speedups respectively
(see the benchmark table in the README).

Heap objects (`struct sobj`) carry a `int type` tag and a union:

```
type           variant
────────────   ────────────────────────────────────────────
OBJ_PAIR       { VALUE car, cdr }
OBJ_SYMBOL     { char *name }                  (interned)
OBJ_STRING     { char *chars; size_t len }
OBJ_CHAR       { uint32_t cp }
OBJ_VECTOR     { VALUE *items; size_t len }
OBJ_CLOSURE    { Node *body; sframe *env; int nparams, has_rest }
OBJ_PRIM       { scm_prim_fn fn; const char *name; int min/max_argc }
OBJ_DOUBLE     { double dbl }                  (heap path; flonum-encoding miss)
OBJ_BIGNUM     { mpz_t mpz }                   (GMP)
OBJ_RATIONAL   { mpq_t mpq }                   (GMP)
OBJ_COMPLEX    { double re, im }
OBJ_PROMISE    { VALUE thunk, value; bool forced }
OBJ_PORT       { FILE *fp; bool input, closed, owned }
OBJ_CONT       { jmp_buf buf; VALUE result; int active, tag }
OBJ_MVALUES    { VALUE *items; size_t len }    (return value of `(values …)`)
OBJ_BOOL,OBJ_NIL,OBJ_UNSPEC,OBJ_EOF — singleton sobj's
```

GMP allocations are routed through `GC_malloc` via
`mp_set_memory_functions`, so bignums and rationals are reclaimed by
the conservative GC like everything else.

## 3. AST nodes

Generated from [`node.def`](../node.def) by ASTroGen + the small
[`ascheme_gen.rb`](../ascheme_gen.rb) extension (the latter teaches
the framework how to hash / dump / specialize `@ref`-stored cache
structs).

### Categories

| Group | Nodes | Purpose |
|---|---|---|
| **Literals** | `node_const_int`, `node_const_int64`, `node_const_double`, `node_const_str`, `node_const_sym`, `node_const_char`, `node_const_bool`, `node_const_nil`, `node_const_unspec`, `node_quote` | self-evaluating constants and quoted scheme values |
| **Variables** | `node_lref`, `node_lset`, `node_gref`, `node_gset`, `node_gdef` | `(set! x v)` etc. — `lref/lset` walk the lexical frame chain by `(depth, idx)`; `gref` looks up by name with an `@ref` inline cache |
| **Control** | `node_if`, `node_seq`, `node_lambda` | branches, sequencing, closure construction |
| **Calls** | `node_call_0`…`node_call_4`, `node_call_n`, `node_callcc` | fixed-arity / variadic / `call/cc` (escape only) |
| **Specialized arith** | `node_arith_add/sub/mul/lt/le/gt/ge/eq` | folded at parse time when the head symbol matches `+ − * < <= > >= =` and isn't lex-shadowed |
| **Specialized preds + vec** | `node_pred_null/pair/car/cdr/not`, `node_vec_ref`, `node_vec_set` | folded at parse time for `null? / pair? / car / cdr / not / vector-ref / vector-set!` |

### Specialization safety under R5RS rebinding

R5RS allows `(set! + my+)`.  Each specialized node carries a
`struct arith_cache @ref` (just `{ int32_t resolved; uint32_t index; }`):

```
hot path:  cache->resolved && c->globals[cache->index].value == PRIM_<op>_VAL
            ↓
            inline fixnum / flonum fast-path
            ↓
            else fall through to add2 / cmp2 / scm_apply via arith_dispatch{1,3}
```

`PRIM_PLUS_VAL` etc. are snapshotted at `install_prims` time; if the
user later rebinds `+`, the global slot's value changes, the equality
check fails, and we go through the slow path (`scm_apply` against
whatever `+` is now bound to).  See [`test/13_redefine_arith.scm`](../test/13_redefine_arith.scm)
for the regression cases.

### Tail-call trampoline

`node_call_K` carries an `is_tail` field stamped during compilation
(propagated through `if` / `begin` / `let` / etc.).  The eval body
calls `scm_apply_tail` which:

- **Tail position + closure target** — sets
  `c->next_body / c->next_env / c->tail_call_pending` and returns
  immediately with a bogus value.
- **Non-tail or non-closure** — runs `scm_apply` (which builds a
  frame and enters the body's dispatcher chain).

`scm_apply` for closures wraps `EVAL(c, body)` in a `for(;;)` that
re-enters whenever `tail_call_pending` is set; one C frame, any TCO
depth.  See [`test/09_tco.scm`](../test/09_tco.scm) for the 10⁶ /
mutually-recursive cases.

## 4. Evaluator (interp mode)

```c
static inline VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}
```

`n->head.dispatcher` starts as the per-kind `DISPATCH_node_xxx`
generated by ASTroGen.  Each `DISPATCH` reads the node's fields and
calls the user-written `EVAL_node_xxx` (force-inlined into its single
caller, the `DISPATCH` wrapper itself).

`EVAL_ARG(c, child)` expands to a function-pointer call through the
child's own `dispatcher`.  Children dispatchers can later be patched
to specialized `SD_<hash>` functions without touching the parent's
code — the link is the `dispatcher` field, not a static call.

## 5. AOT (`-c`) mode

After parsing, `aot_compile_and_load` walks `AOT_ENTRIES` (every
non-`@noinline` AST node registered during compile):

1. **`astro_cs_compile(entry, NULL)`** for each entry → writes
   `code_store/c/SD_<Horg>.c`.  Children are emitted as `static inline`
   helpers within the same `.c` file, so their dispatchers fold into
   the entry's body when gcc inlines.
2. **`astro_cs_build(NULL)`** runs `make -j` (regenerated Makefile)
   to compile every `.c` to `.o` and link `code_store/all.so`.
   `CCACHE_DISABLE=1` is set so a `--clear-cs` rebuild is honestly
   cold rather than ccache-warm.
3. **`astro_cs_reload()`** rehardlinks `all.so` to `all.<gen>.so` and
   `dlopen`s the new path (glibc caches by pathname, so a fresh inode
   forces a re-read).
4. **`astro_cs_load(entry, NULL)`** for each entry → `dlsym("SD_<hash>")`
   and patches `entry->head.dispatcher` to the SD function.

Subsequent runs with `-c` find every `.c` already on disk; only the
re-link and `dlopen` happen.  That's "aot-cached" in the bench.

## 6. PGO (`--pg-compile`) mode

Modeled on `sample/abruby`'s `--pg-compile`.  A single ascheme
invocation:

1. Parse + compile to AST.
2. Run **interpretively** (no AOT applied during this run).
   `scm_apply`'s closure branch increments `body->head.dispatch_cnt`
   on each entry, so by the time the program exits we have a true
   execution count per body.
3. Walk `AOT_ENTRIES`, filter to those above
   `AOT_PROFILE_THRESHOLD` (= 10), and run the same compile / build
   / load sequence as `-c` — but only on the hot subset.
4. Persist `(Horg, count)` tuples to `code_store/profile.txt`.

The next invocation with `-c` automatically picks up `profile.txt`
and applies the same threshold filter — i.e. cold entries stay on
`DISPATCH_node_xxx`, the smaller `all.so` loads faster, and the
hot path is unchanged.  That's "pg-cached" in the bench.

abruby goes a step further by emitting a separate `Hopt`-keyed
`PGSD_<Hopt>` variant that bakes profile-derived constants
(method prologues, etc.) into the generated C; ascheme's specialized
nodes already inline their hot-path constants via `PRIM_*_VAL`, so we
get most of that benefit without a parallel hash.

## 7. Garbage collection

ascheme uses **Boehm-Demers-Weiser GC** (`libgc`) — conservative,
non-moving, and easy to integrate.  Every ascheme allocation goes
through `GC_malloc` / `GC_malloc_atomic`; we never `free`.  This
includes:

- `struct sobj` payloads
- `struct sframe` (closure environments)
- AST nodes (`node_allocate` in `node.c`)
- Scheme strings and vectors
- Globals / symbol table buffers (re-allocated via `GC_realloc`)
- GMP internals (via `mp_set_memory_functions`)

Conservative scanning of the C stack lets us hold values in C locals
across allocations without explicit roots — the same pattern the
specialized node bodies rely on.

## 8. Repository layout

```
sample/ascheme/
├── README.md             user guide
├── docs/runtime.md       this file
├── node.def              AST node definitions (40 kinds)
├── ascheme_gen.rb        ASTroGen extension (handles `@ref` cache structs)
├── context.h             VALUE / sobj / CTX / GMP+GC prototypes
├── node.h                NodeHead, EVAL, extern decl for arith helpers
├── node.c                runtime wiring (alloc, OPTIMIZE, includes generated)
├── main.c                reader, compiler, primitives, drivers (interp / AOT / PGO)
├── Makefile              build, test, bench, bench-big targets
├── test/                 16 self-tests + chibi r5rs-tests adapter
├── bench/small/          quick micro-benchmarks (~1 s interp)
├── bench/big/            substantial workloads (multi-second interp)
├── bench/compare.sh      runs benches across ascheme / chibi / guile
├── code_store/           AOT artefacts (gitignored)
└── .chibi/               chibi-scheme 0.12 cache for `make compare` (gitignored)
```

## 9. Limitations

- **No `dynamic-wind`** — `call/cc` is escape-only (one-shot, downward).
  Re-invocation of an expired continuation raises an explicit error.
- **No `syntax-rules`** — no user-defined macros.  The compiler
  recognises `quasiquote` / `unquote` / `unquote-splicing` and
  expands them to `cons` / `list` / `append`.
- **Immutable `eqv?` of redefined operators** — see §3 above for the
  arith cache trade-off.  Affects `+ − * < = …` only.
- **No source-level line tracking** — `NodeHead.line` is reserved
  but unused; PGC's `(file, line) → Hopt` index is therefore the
  Horg only.
- **Boehm GC quirks** — pointer-free buffers (string/symbol char data)
  use `GC_malloc_atomic`; everything else uses `GC_malloc` so internal
  references are scanned.
