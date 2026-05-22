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
OBJ_CLOSURE    { Node *body; sframe *env; int nparams, has_rest; bool leaf; ... }
OBJ_PRIM       { scm_prim_fn fn; const char *name; int min/max_argc }
OBJ_DOUBLE     { double dbl }                  (heap path; flonum-encoding miss)
OBJ_BIGNUM     { mpz_t mpz }                   (GMP)
OBJ_RATIONAL   { mpq_t mpq }                   (GMP)
OBJ_COMPLEX    { double re, im }
OBJ_PROMISE    { VALUE thunk, value; bool forced }
OBJ_PORT       { FILE *fp; bool input, closed, owned }
OBJ_CONT       struct scont *cont               (out-of-line — see below)
OBJ_MVALUES    { VALUE *items; size_t len }    (return value of `(values …)`)
OBJ_BOOL,OBJ_NIL,OBJ_UNSPEC,OBJ_EOF — singleton sobj's
```

`sizeof(struct sobj) == 48` bytes.  Continuation state — a `jmp_buf`
plus three small fields — lives behind a pointer in `struct scont`,
not inline.  Without that split, the union would pad every cons cell
and vector header to ~208 bytes (the size of `jmp_buf`); see
[`docs/perf.md`](perf.md) §9.  `scm_cons` further allocates only
`offsetof(sobj, pair) + sizeof(pair)` = 24 bytes, fitting the
smallest Boehm bucket (§10).

GMP allocations are routed through `GC_malloc` via
`mp_set_memory_functions`, so bignums and rationals are reclaimed by
the conservative GC like everything else.

The closure variant carries a `bool leaf` set at parse time iff the
lambda's body contains no inner `lambda` form.  `leaf` enables two
hot-path optimizations in §4 below: stack frames via `alloca` for
non-tail-recursive calls, and in-place frame reuse for self-tail-calls.

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
| **Control** | `node_if`, `node_seq`, `node_lambda` | branches, sequencing, closure construction.  `node_lambda` carries a `leaf` operand stamped at parse time. |
| **Calls** | `node_call_0`…`node_call_4`, `node_call_n`, `node_callcc` | fixed-arity / variadic / `call/cc` (escape only).  Tail position bakes an `is_tail` flag at parse time. |
| **Specialized arith** | `node_arith_add/sub/mul/lt/le/gt/ge/eq` | folded at parse time when the head symbol matches `+ − * < <= > >= =` and isn't lex-shadowed |
| **Specialized preds** | `node_pred_null/pair/car/cdr/not` | folded for `null? / pair? / car / cdr / not` |
| **Specialized vec** | `node_vec_ref`, `node_vec_set` | folded for `vector-ref / vector-set!` |
| **Specialized list / eq** | `node_cons_op`, `node_eq_op`, `node_eqv_op` | folded for `cons / eq? / eqv?` |

### Specialization safety under R5RS rebinding

R5RS allows arithmetic operators to be rebound — globally
(`(set! + my+)` / `(define + my+)`) or lexically
(`(let ((+ -)) …)` / `(lambda (+) …)`).  ascheme handles each at the
right phase:

**Lexical shadowing — caught at parse time.**  `try_specialize_arith`
runs `lex_lookup` on the head symbol before emitting a specialized
node; if the name is bound in any enclosing scope it returns `NULL`
and the parser falls back to a generic `node_call_K` with an `lref`
in function position.  Examples that *don't* specialize:

```scheme
(let ((+ -)) (+ 5 3))                  ; emits node_call_2(lref +, …) → 2
((lambda (+) (+ 1 2)) *)               ; same idea — `+` is a parameter → 2
(let ((car cdr)) (car (cons 1 2)))     ; `car` is local → 2
```

**Global rebinding — caught at runtime.**  When the head symbol
isn't lex-bound the specialized node is emitted with an
`@ref`-stored cache:

```c
struct arith_cache { int32_t resolved; uint32_t index; };
```

The hot path checks
`c->globals[cache->index].value == PRIM_<op>_VAL` (the snapshot
captured at `install_prims` time) before running the inline fixnum
/ flonum fast path.  Any subsequent
`(set! + my+)` / `(define + new+)` mutates the global slot's value,
the equality fails, and we route through `arith_dispatch{1,3}` —
a regular `scm_apply` against whatever `+` is now bound to.  See
[`test/13_redefine_arith.scm`](../test/13_redefine_arith.scm) for
the runtime cases.

### Tail-call trampoline + frame reuse

`node_call_K` carries an `is_tail` field stamped during compilation
(propagated through `if` / `begin` / `let` / etc.).  `scm_apply_tail`
is split between an inline header version (hot path) and an out-of-
line slow-path (`scm_apply_tail_slow` in main.c).  See
[`docs/perf.md`](perf.md) §6, §7, §12 for the speed wins.

Hot path (inlined into every dispatcher and SD function via
`node.h`'s `static inline`):

- **Tail position + leaf closure + same shape** — overwrite the
  current frame's slots in place and re-enter without allocating.
  The "same shape" check (`c->env->parent == cl->closure.env &&
  c->env->nslots == total`) means a tight tail loop reuses one
  `sframe` for its entire run.
- **Otherwise** — call `scm_apply_tail_slow`, which falls back to
  `build_frame_for` and the heap.  For non-leaf closures the slow
  path also runs (the `leaf` gate prevents reuse when an inner
  lambda might have escaped a captured frame).

The trampoline lives in `scm_apply` (closure path):

- For *leaf* closures it `alloca`'s the frame on the C stack,
  skipping `GC_malloc`.  Lifetime equals the duration of this
  `scm_apply` call — non-tail recursion stacks alloca frames
  naturally.
- The `for(;;)` loop catches `tail_call_pending` and re-runs the
  body with the new (body, env), which may either be the reused
  alloca/heap frame or a fresh heap frame produced by the slow
  path for a different-shape target.

So the trampoline:

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
