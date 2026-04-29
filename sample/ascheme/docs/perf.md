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

## Cumulative score vs chibi-scheme 0.12  (after §1–§12)

```
                aot-cached       chibi     ratio
ack             0.20 s           0.74 s    3.7×
fib             0.46 s           1.40 s    3.0×
list            0.21 s           0.87 s    4.1×
loop            0.27 s           0.91 s    3.4×
sieve           0.51 s           1.48 s    2.9×
sum             0.57 s           1.27 s    2.2×
tak             0.41 s           1.59 s    3.9×
```

vs guile 3.0 (JIT):

```
                aot-cached       guile     ratio
ack             0.20 s           2.49 s    12×
fib             0.46 s           4.70 s    10×
list            0.21 s           1.30 s    6×
loop            0.27 s           3.31 s    12×
sieve           0.51 s           5.14 s    10×
sum             0.57 s           5.39 s    9×
tak             0.41 s           6.39 s    16×
```

The guile gap is partly its bytecode-VM dispatch + closure-call
overhead, partly the interpreted-mode it falls into when the JIT
counter hasn't tripped yet.  Either way, ascheme's specialized AST
+ AOT-folded SD chain runs ~10× faster on these workloads.

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
