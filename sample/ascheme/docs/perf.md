# ascheme performance — what was tried, what worked

This document is a chronological log of the performance work on ascheme.
Each section names the optimization, the rationale, the implementation,
and the measured before/after on `bench/small/` (interp + aot-cached
seconds).  All numbers from a single Linux x86_64 host (gcc -O2).

The starting point was a textbook tree-walking interpreter with a single
fixnum-or-pointer tag and heap-allocated everything.  The end-point —
roughly the current code — outruns chibi-scheme 0.12 by 2–4× on
arithmetic / recursion / vector benchmarks while staying fully
R5RS-compliant (179/179 of chibi's `tests/r5rs-tests.scm` pass).

## 1 — `gref` inline cache  (commit 402d6af)

**Why.** `node_gref(name)` looked the symbol up in `c->globals` with a
linear `strcmp` scan on every read.  The most common operation in the
language (function-name resolution) was the most expensive.

**How.** Embed a `struct gref_cache @ref` (8 B: `int32_t cached`,
`uint32_t index`) on every `node_gref`.  First resolution stores the
slot index; subsequent reads load `c->globals[cache->index].value`
directly.  Indices are stable: globals are append-only, and `set!`
just overwrites the slot in place.

**Cost.** 8 B per gref.

**Win.** Hot-path gref drops from ~50 ns (strcmp scan over ~100 globals)
to ~3 ns (one indexed load).

## 2 — Specialized arith / pred / vec nodes  (commit 402d6af)

**Why.** `(+ a b)` went `node_call_2 → scm_apply_tail → scm_apply →
prim_plus → add2 → fixnum fast-path`.  Five C function calls plus an
argv array build for a single integer add.

**How.** Pattern-match `(<op> a b)` for `op ∈ {+, −, *, <, ≤, >, ≥, =}`
at parse time and lower to `node_arith_<op>`.  Same for
`null? / pair? / car / cdr / not` (1-arg) and `vector-ref /
vector-set!`.  Each specialized node embeds an `arith_cache @ref`
keyed off the install-time snapshot `PRIM_<op>_VAL`; the EVAL body
checks `c->globals[cache.index].value == PRIM_<op>_VAL` and falls
through to `arith_dispatch{1,3}` (a generic `scm_apply`) on rebinding.
This keeps R5RS `(set! + my+)` semantics intact while leaving the hot
path at a few instructions.

**Cost.** Eight new node kinds in `node.def`; `ascheme_gen.rb`
extension to teach ASTroGen how to hash / dump / specialize the
`@ref` cache structs (treats them as profile-style storage that
doesn't contribute to the structural hash).

**Win.** `(+ fix fix)` collapses to `__builtin_add_overflow` + range
check + `SCM_FIX(...)`.  After AOT specialization the SD function
body is ~5 instructions on the hot path.  Crucially this is **rebind-
safe** — `test/13_redefine_arith.scm` covers the regression cases.

## 3 — Inline flonum encoding  (commit 402d6af)

**Why.** `mandel` and `nbody` allocate a fresh heap `OBJ_DOUBLE` for
every multiplication / addition.  At ~10⁸ ops, that's gigabytes of
short-lived garbage.

**How.** Adopt CRuby's flonum trick verbatim.  Tag bits become:

```
xxxx_xxx1   fixnum
xxxx_xx10   flonum (IEEE-754 double, rotated left by 3)
xxxx_x000   pointer
```

Encoding rotates the bits so the sign + top exponent bits land in the
low 3, OR-ing with `0x02`.  Decoding rotates back.  Doubles in the
exponent range `[2^-255, 2^256]` (≈ `[1e-77, 1e+77]`) round-trip
exactly; `0.0`, NaN, ±inf, and out-of-range values fall through to a
heap `OBJ_DOUBLE` as before.

Add fast paths in `add2 / sub2 / mul2 / cmp2`:

```c
if (LIKELY(SCM_IS_FLONUM(a) & SCM_IS_FLONUM(b)))
    return scm_make_double(scm_flonum_to_double(a) + scm_flonum_to_double(b));
```

**Cost.** A bit of bookkeeping: every place that dereferenced
`SCM_PTR(v)->dbl` had to be audited and changed to `scm_get_double(v)`.

**Win.** mandel went **1.34 s → 0.22 s** (6.1×); nbody went
**1.85 s → 1.11 s** (1.7×).  Tight flonum loops no longer touch the
allocator.

## 4 — `--noinline` on call nodes was wrong  (commit 402d6af)

**Why.** Initially every `node_call_*` was marked `@noinline`, copying
naruby's choice.  The annotation tells ASTroGen's specializer to
emit `/* do nothing */` for the SD body; the call dispatch stayed as
a runtime function-pointer load through `n->head.dispatcher`.  AOT
buys you nothing if the call site doesn't get specialized.

**How.** Remove `@noinline` from `node_call_0 .. node_call_4` (kept on
`node_call_n` because its arg dispatch goes through a runtime read of
`ASCHEME_CALL_ARGS[i]->head.dispatcher`, which the specializer can't
bake).  Now the SD function for a call site fully inlines the
dispatcher chain for `fn` and the args; only the `scm_apply_tail`
boundary is a real call.

**Win.** loop **5.89 s → 4.92 s** (~1.2×); fib **1.60 s → 1.40 s**.
Modest, because most of the dispatch overhead has moved into
`scm_apply_tail` itself, but it set up later wins.

## 5 — PGO via abruby-style `--pg-compile`  (commit 402d6af)

**Why.** `-c` compiles every `@noinline=false` AST node, regardless of
whether it ever runs.  For programs with lots of dead-on-arrival
top-level forms (e.g., `(define foo (lambda () ...))` definitions
that the user never calls in this run), most of the gcc time is
wasted.

**How.** Modeled on `sample/abruby/abruby_gen.rb`'s `--pg-compile`:

1. `--pg-compile file.scm` parses, runs interpretively (so
   `body->head.dispatch_cnt` accumulates real counts), then walks
   `AOT_ENTRIES` and only specializes those above
   `AOT_PROFILE_THRESHOLD` (= 10).  Profile is dumped to
   `code_store/profile.txt` for inspection.
2. The next `-c` invocation auto-loads `profile.txt` and re-applies
   the threshold filter — cold entries stay on `DISPATCH_node_xxx`,
   the smaller `all.so` loads faster.

abruby goes further by emitting a separate `Hopt`-keyed
`PGSD_<Hopt>` variant; ascheme already inlines `PRIM_<op>_VAL` in
specialized arith nodes, so the same effect is achieved without the
parallel hash.

**Win.** For a program where 1/4 entries are hot, `--pg-compile` cuts
the cold-start gcc cost roughly in proportion.  See `pg-compile`
column of `make compare`.  For programs where every entry is hot,
no speedup over plain `-c` (and a small loss because of the extra
interp run).

## 6 — Frame reuse on self-tail-calls  (this commit)

**Why.** A 25 M-step `(let loop ((i 0)) (... (loop (+ i 1))))` was
GC_malloc-ing one `struct sframe` per iteration.  Even at ~30 ns per
malloc, that's ~750 ms of pure allocator work — half the run.

**How.** In `scm_apply_tail`'s tail-call path, recognize the
self-tail-call shape: same parent env, same nslot count, *and* the
target closure is `leaf` (its body has no nested `lambda`, so no
escaped sub-closure can reference the frame).  When all three hold,
overwrite the existing frame's slots in place and re-enter without
allocating.  `compile_lambda` tracks the `leaf` flag during
compilation via a single static `COMPILE_INNER_LAMBDA_SEEN` that
bubbles outward at each lambda boundary.

**Cost.** One `bool leaf` on the closure sobj; one `uint32_t leaf`
operand on `node_lambda` (so the parser can stamp it through ALLOC).

**Win.**

| bench | aot-cached before | aot-cached after | speed-up |
|---|---:|---:|---:|
| loop | 0.92 s | **0.29 s** | 3.2× |
| sum  | 1.29 s | **0.68 s** | 1.9× |
| sieve| 0.97 s | **0.54 s** | 1.8× |

## 7 — Stack-allocated frames for leaf closures  (this commit)

**Why.** Frame reuse handles tail loops, but every *non-tail* call
into a leaf closure still GC_malloc's a new frame.  fib(35) calls fib
~2 × 10⁷ times — most of which are non-tail and therefore allocate.

**How.** In `scm_apply`'s closure path, if the target is leaf,
allocate the frame via `alloca` instead of `scm_new_frame`.  Lifetime
is exactly the duration of this `scm_apply` call — perfect for the
non-tail recursive case.  On a recursive descent the alloca frames
stack like ordinary C locals; the trampoline still works because tail
calls *out* of a leaf closure either reuse the same frame (per §6) or
flip to a heap frame for non-leaf targets.  Either way, the alloca
memory only matters until this `scm_apply` returns, and that exit is
synchronous.

**Cost.** None at the language level — just an `alloca` instead of
`GC_malloc`.  Stack growth is bounded by recursion depth × frame
size, which on these benchmarks stays well under 1 MB.

**Win.**

| bench | aot-cached before | aot-cached after | speed-up |
|---|---:|---:|---:|
| ack  | 0.53 s | **0.22 s** | 2.4× |
| fib  | 1.27 s | **0.46 s** | 2.8× |
| tak  | 1.01 s | **0.43 s** | 2.3× |

## 8 — `cons`, `eq?`, `eqv?` specialized nodes  (this commit)

**Why.** Even after §2 the call sites for `cons`, `eq?`, `eqv?` still
went `node_call_2 → scm_apply_tail → scm_apply → prim_xxx`.  These
are extremely hot in cons-heavy code.

**How.** Add `node_cons_op`, `node_eq_op`, `node_eqv_op` with the same
`arith_cache @ref` rebind-detection pattern.  Hot path drops to
`scm_cons(a,b)` / `a == b` / a small union check.

**Win.** Modest by itself — most of the cons benchmark's cost was in
the allocator, not the dispatch.  But sets the stage for §9.

## 9 — Lift `jmp_buf` out of `struct sobj`  (this commit)

**Why.** Every `sobj` is a `union` of every payload type.  With
`jmp_buf` (~200 B on Linux x86_64) inline as the continuation variant,
**every cons cell, vector header, closure** allocated 200+ bytes —
and the allocator paid bucket / clear / scan costs for all of them.
A 3 M-cell list pinned roughly 600 MB of GC heap.

**How.**

```c
struct scont {           // continuation state, allocated lazily
    jmp_buf buf;
    VALUE   result;
    int     active, tag;
};

struct sobj {
    int type;
    union {
        ...
        struct scont *cont;   // pointer instead of inline struct
    };
};
```

`scm_callcc` now `GC_malloc`'s the `scont` separately and stores the
pointer.  Everything else gets ~5× smaller.  `sizeof(struct sobj)`
goes from ~208 B to **48 B**.

**Win.** `list` bench (3 M cons + traversal): 0.85 s → 0.36 s (2.4×).
Knock-on improvement of GC pressure on every other benchmark.

## 10 — Pair-sized allocation for `cons`  (this commit)

**Why.** Even at 48 B per `sobj`, a cons cell only *needs* 24 B
(`int type` + 4 B padding + `VALUE car` + `VALUE cdr`).  Boehm GC
bucket sizes (16 / 32 / 48 …) mean a 24 B request lands in a smaller
class than a 48 B one.

**How.** `scm_cons` calls `GC_malloc` with exactly
`offsetof(struct sobj, pair) + sizeof(pair)` = 24 B and casts the
result to `struct sobj *`.  Field access is sound because we never
read fields past `pair` for OBJ_PAIR objects.

**Win.** `list` bench: 0.36 s → 0.24 s (1.5× more, 3.5× cumulative
since §8).  No measurable change on non-list benchmarks (variance).

## 11 — Host build at `-O3`  (this commit)

**Why.** ascheme had been compiled at `-O2` to mirror the conservative
default in other ASTro samples.  With the inline flonum encoding,
specialized arith nodes, and frame-reuse / alloca paths now all
small + hot, `-O3`'s extra inlining heuristic should help.

**How.** `Makefile`: `optflags=-O3`.

**Win.**

| bench | -O2 | -O3 | speed-up |
|---|---:|---:|---:|
| loop | 0.32 s | 0.27 s | 1.19× |
| sum  | 0.75 s | 0.63 s | 1.19× |
| sieve| 0.56 s | 0.51 s | 1.10× |
| fib  | 0.53 s | 0.46 s | 1.15× |
| tak  | 0.47 s | 0.46 s | 1.02× |
| ack  | 0.24 s | 0.23 s | 1.04× |
| list | 0.24 s | 0.20 s | 1.20× |

A consistent ~10–20 % across the board, no regressions.  The SD-side
build is already at `-O3` — that hasn't changed.

## 12 — Inline `scm_apply_tail` hot path  (this commit)

**Why.** Every specialized call node ended in
`return scm_apply_tail(c, f, argc, argv, is_tail);`.  That's an
external call (PLT for SD `.so`, normal call for the host
interpreter) on every tail call, with the function body — argument
register setup, prologue, return path — all opaque to the inliner.

**How.** Split `scm_apply_tail` into two pieces:

```c
// node.h — visible to host + every SD .c
static inline VALUE
scm_apply_tail(...)
{
    if (is_tail && scm_is_closure(fn)) {
        struct sobj *cl = SCM_PTR(fn);
        if (LIKELY(!cl->closure.has_rest && cl->closure.leaf && ...shape ok...)) {
            for (i…) c->env->slots[i] = argv[i];
            c->next_body = cl->closure.body;
            c->next_env  = c->env;
            c->tail_call_pending = 1;
            return 0;
        }
    }
    return scm_apply_tail_slow(c, fn, argc, argv, is_tail);
}

// main.c — slow path stays out of line
VALUE scm_apply_tail_slow(...) { /* full body */ }
```

The hot path — self-tail-call to a leaf closure — is now fully
visible to gcc at the call site.  Combined with the `is_tail`
constant baked at parse time, the compiler folds the `is_tail`
branch entirely when the call is non-tail and the trampoline state
machine collapses to a few `mov`s in the SD body.

**Win.**

| bench | -O3 before | -O3 after | speed-up |
|---|---:|---:|---:|
| sum  | 0.63 s | 0.57 s | 1.11× |
| tak  | 0.46 s | 0.41 s | 1.12× |
| ack  | 0.23 s | 0.20 s | 1.15× |

For non-tail-call-heavy benchmarks (loop, sieve, fib) the difference
is in the noise.

## 13 — Serial-keyed inline caches  (this commit)

**Why.** Both `gref_cache` and `arith_cache` had a hot-path shape of
"load `cached` flag → load `c->globals[index].value` → compare to
`PRIM_<op>_VAL`".  The middle step is an *indirected* load: dereference
`c->globals` (a CTX field), index by `cache->index`, read the `value`
out of the `gentry` struct.  That's three memory accesses (globals
pointer, gentry array, value field).  In a tight loop the array
isn't always in L1.

**How.** Replace both caches with a `(uint64_t serial, VALUE value)`
pair, gated on a single `c->globals_serial` counter that's bumped
on every `set!` / `define`:

```c
struct gref_cache  { uint64_t serial; VALUE value; };
struct arith_cache { uint64_t serial; VALUE value; };

// CTX
uint64_t globals_serial;   // bumped by scm_global_define / scm_global_set

// Hot path (gref)
if (LIKELY(cache->serial == c->globals_serial)) return cache->value;

// Hot path (arith)
if (LIKELY(cache->serial == c->globals_serial && cache->value == PRIM_PLUS_VAL))
    fast_path();
```

The fast path now reads only the cache fields (one cache line) and
the CTX serial (also a hot field).  No globals indirection.  `set!` /
`define` invalidate every cache at once via the serial bump — for
benchmark code that touches set! only at init, the loop body never
takes the slow path.

**Cost.** `gref_cache` grows from 8 B to 16 B (`int32_t cached;
uint32_t index` → `uint64_t serial; VALUE value`).  Same for
`arith_cache`.  That's an extra 8 B per call site; for the bench
programs (a few hundred sites) it's nothing.

**Win.**

| bench | -O3 §12 | -O3 §13 | speed-up |
|---|---:|---:|---:|
| loop | 0.27 s | 0.23 s | 1.17× |
| sieve| 0.51 s | 0.44 s | 1.16× |
| fib  | 0.46 s | 0.42 s | 1.10× |
| tak  | 0.41 s | 0.37 s | 1.11× |
| ack  | 0.20 s | 0.15 s | 1.33× |
| sum  | 0.54 s | 0.54 s | — |
| list | 0.21 s | 0.20 s | 1.05× |

`sum` is unchanged because its hot path already does the `arith_cache`
load just twice (the rest of the time is spent in the tail-call
trampoline).

## 14 — Inline non-tail leaf-closure path  (this commit)

**Why.** §12 pulled `scm_apply_tail`'s tail-call hot path into a
`static inline` in node.h.  But the **non-tail** closure call still
went through the slow path: `scm_apply_tail` → `scm_apply_tail_slow`
(out of line) → `scm_apply` (out of line) → frame alloc + trampoline.
For `fib` and `tak` — pure non-tail recursion — every single call
paid the double-PLT hop.

**How.** Inline the non-tail leaf-closure path next to the tail
fast path inside the same `static inline scm_apply_tail`:

```c
// Tail position — frame reuse on self-tail-call to a leaf
if (is_tail && scm_is_closure(fn)) { ... }
// Non-tail leaf-closure call — alloca + trampoline inline
if (!is_tail && scm_is_closure(fn)) {
    struct sframe *new_env = alloca(...);   // stack frame
    ...
    for (;;) {
        VALUE v = EVAL(c, body);
        if (!c->tail_call_pending) { c->env = saved; return v; }
        ...
    }
}
return scm_apply_tail_slow(...);
```

`is_tail` is a parse-time constant, so each SD only emits one of the
two branches; the dead branch's body is dropped by the compiler.

The trampoline + alloca + frame-fill chunk inlined at every call
site is hefty (~30 instructions), but for non-tail recursive code it
removes two PLT calls per invocation.

**Subtle bit — `__attribute__((always_inline))` matters.**  Without
it gcc's heuristics decide the now-larger inline body isn't worth
expanding at every site, and silently emits an out-of-line copy
instead — losing the win.  Adding the attribute forced expansion
and got us back the speed we expected.  Confirmed by re-measuring
with vs without; sum was ~13 % slower without the attribute.

**Win.**

| bench | §13 | §14 | speed-up |
|---|---:|---:|---:|
| ack  | 0.15 s | **0.13 s** | 1.15× |
| fib  | 0.42 s | **0.25 s** | 1.68× |
| tak  | 0.37 s | **0.24 s** | 1.54× |
| list | 0.20 s | **0.18 s** | 1.11× |
| loop | 0.23 s | **0.21 s** | 1.10× |
| sum  | 0.54 s | 0.54 s | — (no non-tail closure calls) |
| sieve| 0.44 s | 0.45 s | — |

The tail-only benchmarks (sum, sieve, loop) don't gain — their hot
path was already inline.  fib / tak / ack / list, which all do
non-tail closure recursion or repeated cons calls, see 1.1-1.7×
speedups.

## 15 — `try_specialize_arith` no longer compiles args eagerly  (this commit)

Found via `perf record + perf report` on `bench/small/sum.scm`: 95 %
of cycles were attributed to host `DISPATCH_node_arith_add` /
`DISPATCH_node_call_2` / `DISPATCH_node_lref` from `ascheme`, and
**0 %** to `SD_*` from `all.so`.  But `nm code_store/all.so` showed
the SDs were exported, and `aot_compile_and_load` reported "loaded
4 / 4".  AOT was wired up, but the runtime wasn't using it.

Root cause: `try_specialize_arith` (the parser hook that lowers
`(+ a b)` etc. to specialized arith nodes) compiled its argument
forms **before** matching the function name:

```c
if (argc == 1) {
    NODE *a = compile(c, car(args), scope, false);   // compile FIRST
    if (strcmp(name, "null?") == 0) return ALLOC_node_pred_null(a);
    ...
    return NULL;        // name didn't match — `a` is now wasted
}
```

For a call like `(display X)`:

1. `try_specialize_arith(display, (X))` compiled `X` once (allocating
   AOT entries for any lambdas inside), then found "display" wasn't
   one of `null?` / `pair?` / `car` / `cdr` / `not`, returned NULL.
2. `compile_call` then compiled `X` **a second time**, allocating
   *fresh* NODE instances for every sub-form (lambdas, lrefs, ...).

Both the discarded and live trees registered themselves via
`aot_add_entry` on every embedded lambda body.  After the AOT step:

* The first (discarded) entry's `head.dispatcher` got patched to
  `SD_<hash>` by `astro_cs_load`.
* The second (live, runtime) entry's `head.dispatcher` was a freshly
  allocated NODE — **never touched by `astro_cs_load`** because of
  the dedup loop in `aot_compile_and_load`, which skipped any entry
  whose hash had already been seen.  It kept the slow host
  `DISPATCH_node_*` default that `ALLOC_*` installs.

So every SD compilation succeeded, every SD function was loaded into
`all.so`, and **none of them was ever called** by the running program.
`bench/small/sum.scm` was effectively running plain-interpreter code
even with `--clear-cs -c`.

The fix is two-part:

1. In `try_specialize_arith`, match the name **before** compiling the
   args.  Now non-matching names return NULL without side effects, so
   `compile_call` is the only place that allocates NODEs for them.
2. In `aot_compile_and_load`, drop the "skip duplicate hash" guard on
   the load loop.  The SD function in `all.so` is shared across same-
   hash entries, but each NODE has its own `head.dispatcher` slot
   that has to be patched individually.  (Without this, any future
   path that legitimately produces two same-hash entries — e.g. two
   structurally-identical lambdas in different files — would silently
   leave the second one running on the slow path.)

After the fix, `perf report` on the same workload shows 45 % of
cycles in `SD_5746ee42f49624cf` (the loop body's compiled SD) and
10 % in `scm_apply` (the trampoline) — the hot SD is finally getting
exercised.  Workloads where the body of a global call wraps
significant computation see the biggest jumps:

```
                 before    after    speedup
sum (small)      0.54 s    0.20 s    2.7×
sumloop (big)    2.15 s    0.88 s    2.4×
mandel           0.15 s    0.13 s    1.15×
nbody            0.59 s    0.51 s    1.16×
fib35            0.27 s    0.24 s    1.13×
others           ±5 %      ±5 %      ~ neutral
```

Sum and sumloop benefit most because their entire computation is the
argument of `(display ...)` — a 1-arg global call that didn't match
any specialized name and therefore double-compiled.  Microbenchmarks
that already routed through specialized 2-arg arith (`<`, `+`, etc.)
matched on the first try and were unaffected.  Wins on
mandel / nbody / fib35 come from secondary global-call sites in
their bodies (e.g. `(make-vector …)` arguments) that were similarly
double-compiled.

The lesson: when the parser performs a try-and-fallback on an
expensive operation, do the cheap match check first.  Otherwise the
fallback path silently doubles every recursive child too.

## 16 — Collapse arith-cache dual check to a single serial check  (this commit)

`perf annotate` on the loop SD showed each `+` / `-` / `=` dispatch
spent ~6 cycles on the cache-validity check:

```asm
cmp  %rax, 0x58(%rsi)         # cache->serial == globals_serial?
jne  slow
mov  0x4b9d(%rip), %rax       # GOT lookup PRIM_PLUS_VAL (1 load)
mov  (%rax), %rax             # dereference the GOT slot (1 load)
cmp  %rax, 0x60(%rsi)         # cache->value == PRIM_PLUS_VAL?
jne  slow
```

Two checks: (a) "is the cache fresh against current globals?" and
(b) "is `+` still bound to the original primitive (not user-rebound)?".
Both are needed for correctness — a `set!` on an unrelated global bumps
`globals_serial` without rebinding `+`, and a `(set! + my+)` rebinds `+`
without anything else changing.

But the second check (the `cache->value == PRIM_PLUS_VAL` part) costs a
GOT-relative load + a dereference + a compare — three extra instructions
per arith op, hot path.  The fix is to encode the rebind state into the
serial itself: `arith_refresh` only sets `cache->serial = globals_serial`
when the freshly-resolved global is still the original prim.  When
rebound, `cache->value` gets updated to the new closure but
`cache->serial` is left at its prior (stale) value.  The hot path's
single equality check then fails for rebound bindings (drives slow
dispatch through the user's closure) **and** for stale caches (drives
re-resolution).

After the change, the cache-validity gate is one compare:

```asm
cmp  %rax, 0x58(%rsi)         # cache->serial == globals_serial?
jne  slow
```

The slow path picks up an extra `expected` parameter so it knows what
the original primitive value was when deciding whether to bump the
serial.

Wall-time impact (best of 10, system under load):

```
                 baseline   collapsed   speedup
sumloop          1.03 s     0.96 s      1.07×
cps_loop         0.51 s     0.48 s      1.06×
sieve_big        1.04 s     1.01 s      1.03×
fib35            0.28 s     0.27 s      1.04×
```

Smaller wins than the saved-cycles math suggests (~6 cycles × 75M arith
ops ≈ 150 ms), because the GOT loads were already L1-cached and the
out-of-order CPU pipelined them with other work.  Still a clean
3–7 % gain across arith-heavy benches with no regressions.

`test/13_redefine_arith.scm` (the rebind/restore round-trip) keeps
passing — the new semantics correctly drives rebinds through the slow
path and restores the fast path once the original binding is in place.

## 17 — Lazy lref level cache for depth >= 2  (this commit)

`node_lref` (and `node_lset`) used a plain parent-chain walk:

```c
struct sframe *e = c->env;
for (uint32_t i = 0; i < depth; i++) e = e->parent;
return e->slots[idx];
```

Each `e = e->parent` is a chained dependent load.  ASTroGen specializes
`depth` as a literal so gcc unrolls the loop, but the loads are still
serial — for `depth=5` (sieve / fannkuch / nested let), that's ~5
chained loads = ~20 cycles latency on a typical OOO core.  A loop-body
that lref's the same outer slot 4–6 times per iteration pays the chain
walk every time.

Add a per-CTX lazy parent cache, keyed on a new `env_serial` counter
that bumps on every `c->env =` switch.  Hot path:

```c
if (depth == 0) return c->env;
if (depth == 1) return c->env->parent;
if (depth < ASCHEME_LREF_CACHE_SIZE) {       // 8
    if (UNLIKELY(c->env_cache_serial != c->env_serial)) {
        c->env_cache_serial = c->env_serial;
        c->env_chain[0] = c->env;
        c->env_chain_filled = 0;
    }
    while (c->env_chain_filled < depth) {
        c->env_chain[c->env_chain_filled+1] =
            c->env_chain[c->env_chain_filled]->parent;
        c->env_chain_filled++;
    }
    return c->env_chain[depth];
}
// fallback for depth >= 8: plain walk
```

`depth=0` and `depth=1` get the direct path because the cache check
+ array index is more work than 1 chained load — the cache is only a
win starting at `depth=2`.

Three subtleties matter:

1. **Pointer equality is not enough.**  alloca'd leaf-closure frames
   reuse stack memory: when the call returns, the `sframe` address
   becomes available again, and a later call may alloca an identical
   pointer holding a *different* parent chain.  A cache keyed on
   `c->env` pointer would silently return wrong data.  Hence the
   `env_serial` counter, bumped via `CTX_SET_ENV(c, e)` on every env
   switch.

2. **Self-tail-call frame reuse must NOT bump.**  The hot tail-loop
   `(loop (+ i 1) (+ s i))` in `sum.scm` rewrites slots in place but
   keeps `c->env` the same — the cache stays warm across iterations.
   The trampoline checks `if (c->next_env != c->env) CTX_SET_ENV(...)`
   so the bump only fires when env actually changes.

3. **Lazy fill, not eager.**  We don't precompute the whole chain on
   env switch; we extend `env_chain[]` only as deep as the program
   actually needs.  A workload using `lref(3, *)` doesn't pay for
   levels 4–7.

Best-of-10 wall time:

```
                 baseline   cached     speedup
sieve (small)    0.45 s     0.41 s     1.10×
fib              0.24 s     0.22 s     1.09×
list             0.19 s     0.17 s     1.12×
loop             0.21 s     0.19 s     1.10×
sumloop (big)    0.72 s     0.70 s     1.03×
sieve_big        0.90 s     0.83 s     1.08×
nqueens          1.09 s     1.04 s     1.05×
fannkuch         1.37 s     1.31 s     1.05×
fib35            0.24 s     0.23 s     1.04×
deriv            1.18 s     1.16 s     1.02×
mandel/cps_loop  ±0% (no deep lrefs in hot path)
ack / tak        ±0% (lref depth ≤ 1, direct path)
```

5–12 % across lref-depth-heavy benches; the wash on ack / tak is
expected (those bodies barely touch parent-frames).

## 18 — `node_loop` / `node_self_tail_call_K` for named-let  (this commit)

`(let loop ((i 0) (s 0)) (if cond then (loop ...)))` traditionally went
through the trampoline on every iteration: SD → `scm_apply_tail`'s
frame-reuse path → `tail_call_pending = 1` → return → `scm_apply`'s
trampoline → SD again.  ~10 % of `sum`'s wall time was the trampoline
itself (indirect call + `tail_call_pending` R/W per iter).

The framework principle is "ASTroGen / specializer don't look at
specific eval bodies" — so we can't have the SD generator emit a
goto-back-to-top.  But we CAN have the *parser* emit nodes that do
the loop, and ASTroGen specializes them like any other node.

Two new nodes:

* `node_loop(body, nparams)` — wraps a lambda body that contains
  patched self-tail-calls.  Runs `body` in a `for(;;)`; when the body
  sets `c->loop_continue = 1`, copies `c->loop_args[0..nparams]` back
  into the current frame's slots and re-iterates.  Otherwise returns.

* `node_self_tail_call_K(args[K])` — evaluates new args into
  `c->loop_args[]`, sets `c->loop_continue = 1`, returns.  Replaces
  `(call_K (lref D 0) args)` for tail-position self-recursive calls.

Parser detection (in `compile_let_named`): instead of desugaring to
`letrec → let → lambda call`, build the AST directly.  Push a
`CURRENT_SELF_CALL` context with the loop name + arity + the scope
that owns the slot, then compile the inner-lambda body.  `compile_call`
recognizes calls matching the context — same name, tail position,
matching arity, and `lex_lookup` resolves to *that exact scope* (so an
inner `(let ((loop …)) …)` shadow doesn't accidentally take the
fast path) — and emits a `node_self_tail_call_K`.  When at least one
patch happened, the inner body gets wrapped in `node_loop`.

`compile_lambda` saves/clears `CURRENT_SELF_CALL` on entry, so a
nested closure inside the body doesn't pick up the optimization
context — its escaping closure has a different env identity, and a
self-tail-call from inside it would be writing to the wrong frame.
That nested case falls back to the generic `scm_apply_tail` path.

Result: the named-let body's hot path becomes a tight C `for(;;)` —
one indirect dispatch on entry, then it stays inside the SD.  No
trampoline, no per-iteration `scm_apply` re-entry.

Best-of-10 wall time, opt-off vs opt-on, same build:

```
                 baseline  loop-opt   speedup
sum              0.19 s    0.07 s     2.71×
sumloop          0.71 s    0.27 s     2.63×
cps_loop         0.39 s    0.18 s     2.17×
sieve (small)    0.44 s    0.22 s     2.00×
sieve_big        0.92 s    0.46 s     2.00×
list             0.19 s    0.14 s     1.36×
fannkuch         1.41 s    1.36 s     1.04×
nqueens          1.04 s    0.99 s     1.05×
ack/fib/tak      ~wash (defined as global procedures, not named-let)
```

Benches that recurse via `(define (f …) …)` — fib, tak, ack — don't
benefit because they go through global call dispatch, not a named-let.
That's the next target (§19, planned: `node_call_known_global_K` with
a closure-pointer cache that bypasses scm_apply for hot global calls).

Two parser-side correctness landmines hit during implementation:

* **Init expressions are syntactically outer-scope but evaluated in
  the wrapper-lambda frame.**  `(let loop ((i v) …) …)` evaluates
  `v` after the wrapper lambda has been entered, so `lref` depths
  must be measured from `outer_scope` (the wrapper) — not from the
  caller `scope`.  Compiling inits in the wrong scope produces
  one-level-shallow loads at runtime (caught by the chibi tests).

* **Inner shadowing.**  `(let loop (…) (let ((loop …)) (loop …)))`
  rebinds `loop` inside the body.  The `(loop …)` call in the inner
  let now refers to the *inner* binding, not our named-let's.  Hence
  the `target_scope` check in `compile_call`: `lex_lookup_full`
  resolves the call's fn name and we only patch when the resolved
  scope matches the one we registered.

No framework (`lib/astrogen.rb`) changes — the new nodes are declared
in `node.def` and the parser change is local to `compile_let` /
`compile_call` / `compile_lambda`.

## 19 — `node_self_tail_call_global_K` for `(define (f …) … (f …))`  (this commit)

§18 collapsed the trampoline for *named-let* self-tail-calls.  Most
real Scheme code, though, defines recursive helpers as top-level
`(define (f params) body)` and recurses via global gref — and that
path was untouched.  loop / ack / tak / nqueens iterate through the
same trampoline as before.

Same trick, scoped to global mode.  Two changes:

1.  Five new nodes `node_self_tail_call_global_K` (K=0..4).  Same
    shape as §18's local `node_self_tail_call_K`, plus a `gref_cache`:
    the hot path checks `cache->serial == globals_serial` before
    setting `c->loop_args[]` / `c->loop_continue=1`.  If the global
    has been rebound (`(set! f g)`), `globals_serial` has bumped, the
    cache check fails, and the node falls through to
    `scm_apply_tail(c, current_fn, …, is_tail=1)` with the freshly-
    resolved closure.  This preserves R5RS dynamic-dispatch semantics
    even though the body looks like it's hard-wired to call itself.

2.  `compile_define` for top-level `(define (f params) body)` arms
    `SELF_CALL_FOR_NEXT_LAMBDA` with a global-mode ctx (target_scope
    = NULL); `compile_lambda` consumes that token on entry to the
    immediate next compile (so nested lambdas inside body still get
    the standard save+clear).  `compile_call` in global mode matches
    `(call_K (gref f-name) args)` patterns where the symbol is a
    non-shadowed global and the name matches.

Wraps body in `node_loop` if any patches fired.  Mutual recursion
(`(define f (lambda … (g …)))` calling `(g …)` calling `(f …)`)
isn't optimized — only direct self-tail.

Best-of-7 wall time, opt-off (just §1–§18) vs opt-on (§1–§19), same
build:

```
                 baseline  +§19      speedup
loop             0.20 s    0.05 s    4.00×
ack              0.13 s    0.10 s    1.30×
nbody            0.72 s    0.54 s    1.33×
nqueens          0.99 s    0.87 s    1.14×
tak (small)      0.22 s    0.21 s    1.05×
tak_big          12.73 s   11.73 s   1.09×
fib / fib35      ~wash (no tail-self-call; recursion is via `(+ (fib …) (fib …))`,
                       both calls non-tail)
sum / sumloop    already at the §18 floor (named-let path)
matmul / deriv   ±3 % (noise; bodies don't have tail-self-recursion to themselves —
                       inner named-lets handled by §18)
```

The fib non-improvement deserves a note: fib's body is `(+ (fib …)
(fib …))`, where each `(fib …)` is in *non-tail* position (it's an
argument to `+`).  Only the outer `+` is in tail position, and that's
not a recursive call.  §19 fires only on tail-position recursion, so
fib stays on the (already-inlined) `scm_apply_tail` non-tail leaf
path.  Truly speeding up fib would require either inlining the body
into the call site or memoization — both significantly more invasive.

The mechanism is the third (and probably last) major win on top of
§18: named-let, top-level define, and we'd need user-defined macros
or PGO-driven devirtualization for further structural wins.

No `lib/astrogen.rb` change — only node.def + main.c + context.h.

## Cumulative score vs chibi-scheme 0.12  (after §1–§19)

```
                aot-cached       chibi     ratio
ack             0.13 s           0.74 s    5.7×
fib             0.25 s           1.40 s    5.6×
list            0.18 s           0.87 s    4.8×
loop            0.20 s           0.91 s    4.6×
sieve           0.44 s           1.48 s    3.4×
sum             0.20 s           1.27 s    6.4×    ← was 2.4× before §15
tak             0.24 s           1.59 s    6.6×
```

vs guile 3.0 (JIT):

```
                aot-cached       guile     ratio
ack             0.13 s           2.49 s    19×
fib             0.25 s           4.70 s    19×
list            0.18 s           1.30 s    7×
loop            0.20 s           3.31 s    17×
sieve           0.44 s           5.14 s    12×
sum             0.20 s           5.39 s    27×    ← was 10× before §15
tak             0.24 s           6.39 s    27×
```

The guile gap is partly its bytecode-VM dispatch + closure-call
overhead, partly the interpreted-mode it falls into when the JIT
counter hasn't tripped yet.  Either way, ascheme's specialized AST
+ AOT-folded SD chain runs ~10× faster on these workloads.

## Cumulative score on bench/big  (multi-second workloads)

For longer benchmarks the same patterns hold — ascheme aot-cached
beats chibi-scheme on every test, and beats guile-3.0 on every test
except matmul (where guile's JIT produces tight inner-loop code for
nested `vector-ref` chains that we don't yet match):

```
                 ascheme       chibi      guile     vs chibi   vs guile
cps_loop          0.43 s        2.91       10.13       6.8×        24×
deriv             1.16 s        1.71        1.87       1.5×       1.6×
fannkuch          1.37 s        4.08        6.85       3.0×       5.0×
fib35             0.24 s        1.21        4.89       5.0×        20×
mandel            0.13 s        0.97        1.00       7.5×       7.7×
matmul           11.45 s       22.63        9.50       2.0×    0.83× (loss)
nbody             0.51 s        2.62        2.49       5.1×       4.9×
nqueens           1.16 s        4.27       14.71       3.7×        13×
sieve_big         0.90 s        2.34        9.62       2.6×        11×
sumloop           0.88 s        3.52       17.74       4.0×        20×    ← §15: was 1.6× / 8.2×
tak_big          14.90 s       76.02      348.62       5.1×        23×
```

mandel (6.5×) and nbody (4.4×) are won by §3's inline flonum
encoding; matmul's nested `vector-ref` access pattern in a tight
inner loop is the one place guile's JIT pulls clearly ahead.

## What didn't move the needle (yet)

- **Specializing `node_lref0` / `node_lset0`** — the depth=0 case is
  the common one, but with AOT specialization gcc unrolls the
  zero-trip `for` loop already.  No measurable speedup.
- **CCACHE during AOT** — explicitly disabled (`CCACHE_DISABLE=1`)
  during cold-start measurement so timings reflect the real gcc cost
  rather than a cache hit.  See `bench/compare.sh`.

## What's next

- **Direct call inlining via PGO** — bake the resolved closure body
  pointer into the SD code at hot call sites (the abruby
  `method_prologue_t` analogue).  Should remove the remaining
  `scm_apply_tail` overhead for hot calls.
- **`call_known_prim`** — at AOT time, sites that always resolve
  through `gref` to a primitive could skip the closure-vs-prim
  branch entirely.
- **Sub-typed `cons` cells** — list/cons-heavy benchmarks (`list`)
  still pay full `struct sobj` (~24 B + GC header) per pair.  A
  cons-only sub-allocator with header-less cells would close the gap
  to chibi.
