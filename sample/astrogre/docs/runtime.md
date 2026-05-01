# astrogre runtime

This document explains how astrogre actually matches a regex against an
input — i.e. the runtime semantics of the AST nodes, with particular
emphasis on how repetition and continuation flow are wired up.

## High-level shape

```
Ruby src ──prism──▶ pm_regular_expression_node_t.unescaped + flags
                      │
                      ▼
                ┌────────────┐
                │ regex      │  hand-written recursive-descent parser
                │ parser     │  (parse.c) — reads /.../ body, builds IR
                └─────┬──────┘
                      │
                      ▼
                ┌────────────┐
                │ IR (ire_*) │  little tree of unions: LIT, CONCAT, ALT,
                │            │  REP, GROUP, CLASS, ...
                └─────┬──────┘
                      │  lower(..., tail=succ)        right-to-left:
                      ▼                                each node's
                ┌────────────┐                         `next` is the
                │ ASTro AST  │  node_re_*              already-built
                │ (ALLOC_*)  │  generated from         remainder
                └─────┬──────┘  node.def
                      │
                      ▼ EVAL(c, root)
                  match / no match + captures
```

The "lowering" step is what makes the AST itself a directed chain in
*continuation-passing* form: each match node carries a `next` operand,
and dispatching `next` is how it tells "the rest of the pattern" to try
to match. A failed `next` returns 0 to its caller, which is how
backtracking is expressed without an explicit thread list or stack
machine.

## Continuation-passing convention

Every match-node has the same calling shape:

```c
NODE_DEF
node_re_xxx(CTX *c, NODE *n, ..., NODE *next)
{
    /* try to consume something */
    if (this node's local check fails) return 0;

    /* save state we'll need to undo */
    size_t saved_pos = c->pos;
    c->pos += how_much_we_consumed;

    /* let the rest of the pattern try */
    VALUE r = EVAL_ARG(c, next);

    if (!r) c->pos = saved_pos;     /* backtrack */
    return r;
}
```

This shape generalises to anchors (no `c->pos` change), captures (save
the slot, restore on tail-fail), and lookaround (always restore `c->pos`
even on success). The terminator is `node_re_succ`, which sets
`ends[0] = c->pos` and returns 1 — that's how we record where the whole
match ended.

The chain ends in `node_re_succ`. The chain *starts* at `cap_start(0,
...)` (added implicitly by the parser) so capture group 0 records the
whole match span without special-casing the entry point.

## The repetition mechanism

Repetition is the only construct where continuation passing alone isn't
enough: `a*b` lets the body match zero or more times, with the outer
`b` competing for the input. We use a small runtime stack:

```c
struct rep_frame {
    NODE *body;           /* what's iterating */
    NODE *outer_next;     /* what comes after the rep */
    int32_t min, max;     /* remaining counts; max == -1 means ∞ */
    uint32_t greedy;
    struct rep_frame *prev;
};
```

`node_re_rep` pushes a fresh frame and dispatches a single, shared
sentinel node — `node_re_rep_cont`, allocated once at startup — which
reads `c->rep_top` to decide what to do next. The body's `next` operand
is wired to the same sentinel, so each successful body iteration lands
back in `rep_cont`.

```c
NODE_DEF
node_re_rep(CTX *c, NODE *n, NODE *body, NODE *outer_next, ...)
{
    struct rep_frame f = { body, outer_next, min, max, greedy, c->rep_top };
    c->rep_top = &f;
    VALUE r = (*c->rep_cont_sentinel->head.dispatcher)(c, c->rep_cont_sentinel);
    c->rep_top = f.prev;
    return r;
}
```

`node_re_rep_cont` then implements the greedy / lazy contract:

* **Greedy** — try to take one more iteration first (recursing through
  body → rep_cont → body → ...); if any depth eventually fails to reach
  `outer_next` successfully, fall back to "min satisfied? try
  outer_next" at this level.
* **Lazy** — try `outer_next` first (if `min == 0`); only if that fails
  do we try one more body iteration.

The pop-on-recurse pattern is important: when we dispatch
`outer_next`, we temporarily pop our own frame off `c->rep_top` so any
*nested* rep_cont triggered from inside the rest of the pattern reads
the right frame. We restore on backtrack.

This handles the case `(a|ab)*c` matching `abc` correctly: after `a`
matches in iteration 1 and the outer `c` fails at position 1, control
returns up through the body's continuation (rep_cont). With the body's
`next` wired to rep_cont, the *inner* `alt` gets a chance to retry
its other branch (`ab`) on the way back, which is what makes the
composite match `ab` then `c`. A "body returns one success per call"
short-cut would miss this.

## Captures

Captures live in `c->starts[]`, `c->ends[]`, `c->valid[]`. There are
two nodes:

* `node_re_cap_start(idx, next)` — saves the slot, writes
  `starts[idx] = c->pos`, dispatches `next`. On tail failure the slot
  is restored.
* `node_re_cap_end(idx, next)` — symmetric on the end side; sets
  `valid[idx] = true` so a backreference / output reader knows the slot
  has data.

Group 0 is wrapped around the whole AST by the parser, so callers
always get `starts[0]` / `ends[0]` for the overall match span without a
special success path. `node_re_succ` also writes `ends[0]` for the
match-found case (defensive — it's redundant when group 0 is wrapped,
but cheap and means a future "don't wrap" optimisation won't break).

## Anchors

`\A`, `\z`, `\Z`, `^`, `$`, `\b`, `\B` are zero-width: they look at
`c->pos` (and possibly the surrounding bytes) and either dispatch
`next` or return 0. `\b` / `\B` use a 7-bit ASCII word-character
predicate (`[A-Za-z0-9_]`), matching Ruby's default for `/n` and `/u`
on ASCII letters.

## Encoding

`c->encoding` is set from prism's flag bits (or from the literal CLI
syntax). Two paths affect actual byte traversal:

* **`.` (dot)** — there are four node kinds: `node_re_dot`,
  `node_re_dot_m`, `node_re_dot_utf8`, `node_re_dot_utf8_m`. The UTF-8
  variants advance one full codepoint by sniffing the leading byte
  (`0xxxxxxx` → 1, `110xxxxx` → 2, `1110xxxx` → 3, `11110xxx` → 4) and
  refusing to match an invalid lead byte.
* **Literals** — bytes are bytes. UTF-8 multi-byte sequences are
  emitted by the parser as a single literal token so a quantifier
  binds to the whole codepoint, not to its trailing byte.

Character classes are an ASCII-only 256-bit bitmap (`uint64_t bm[4]`).
That's enough for `\d \w \s` and ASCII ranges; for non-ASCII inside
`[...]` v1 simply matches byte-wise, which is wrong for any pattern
where a multi-byte codepoint should be one class element. See
[`todo.md`](./todo.md).

## Top-level search

The for-each-start-position loop is itself an AST node:
`node_grep_search`. Its EVAL is the loop, its `body` operand is the
regex AST, its `anchored_bos` operand short-circuits to one
position when the pattern starts with `\A`. `astrogre_search` (in
match.c) just sets up CTX and calls `EVAL(c, root)` once.

```c
NODE_DEF
node_grep_search(CTX *c, NODE *n, NODE *body, uint32_t anchored_bos)
{
    size_t start = c->pos;                  /* caller-set */
    size_t start_max = anchored_bos ? (start == 0 ? 1 : 0) : c->str_len + 1;
    for (size_t s = start; s < start_max; s++) {
        c->pos = s;
        for (int i = 0; i < ASTROGRE_MAX_GROUPS; i++) c->valid[i] = false;
        c->rep_top = NULL;
        if (EVAL_ARG(c, body)) return 1;
    }
    return 0;
}
```

Putting the loop in node.def is the key trick. When the specialiser
recurses into `body` and inlines `body_dispatcher` as a direct
function pointer, gcc fuses the loop and the regex chain into one
SD function — no indirect calls, no DISPATCH chain, capture-state
reset hoisted to a single `vmovdqu`. See
[`perf.md`](./perf.md) for the disassembly and the 7.22×
literal-tail microbench number.

## Memory

* AST nodes are heap-allocated via the framework's `node_allocate`
  (calloc). They live for the life of the pattern; freeing happens
  in `astrogre_pattern_free`.
* The intermediate IR (`ire_*`) is freed right after lowering — only
  the AST persists.
* CTX and rep frames are stack-allocated, so no malloc on the hot
  path.

## Threading model

None. CTX is per-call and the rep_cont sentinel is shared globally,
but everything mutable (rep stack, captures) lives on the calling CTX.
Two threads can match concurrently with separate CTX instances even
though they share the AST.

## Backend abstraction

The grep CLI (main.c) talks only to `backend.h`.  Two backends are
plugged in:

* `backend_astrogre.c` — the in-house engine (this whole file).
* `backend_onigmo.c`   — Onigmo (`onig_new` + `onig_search` + region
                         object), built only when WITH_ONIGMO=1.

The ops table is `compile / search / search_from / free`.  Each
backend implements `-F` (fixed-string) at the compile call: in our
engine via `astrogre_parse_fixed`, in Onigmo by escaping
metacharacters before passing to `onig_new` (Onigmo doesn't have a
fixed-string mode).  Pattern objects are opaque on either side, so
the CLI never has to look inside them.

This is plumbing, not optimisation — but it's what made the
side-by-side comparison in `bench/grep_bench.sh` cheap to write.

## Where ASTro's specialization helps (and where it doesn't)

A bench-driven note on what we learned writing this sample, kept here
because the answer turns out to depend on the *shape* of the workload
in a way that might surprise someone coming from "AOT bake good for
everything".

### Bake helps when per-iter work is non-trivial
When the inner work per dispatch is meaningful — a method send, a
type-check, a frame push — the bake's job (eliminating the indirect
call + constant-folding child operands) is a real share of the wall
time:

- koruby `fib`: interp → AOT, 3.6×.
- pascalast typical bench: 2–25× across the table.
- Our own `literal-tail` microbench (16 KiB single buffer, repeated
  search): **22.75 s → 3.15 s, 7.22×**. The fused SD has no
  indirect call left at all.

### Bake stops helping when an algorithmic optimization eats the dispatch
For the grep CLI the picture flips. Per-position inner work is *one
or two compares*, not a method send. The dispatch chain bake removes
is already cheap (3–4 indirect calls per position, all to the same
hot BTB target, ~1 ns each). And once the literal-prefix prefilter
nodes landed (`node_grep_search_memchr` / `_memmem`), the verify
chain only runs on candidate positions — a handful per kilobyte —
so the total dispatch overhead the bake could eliminate is in the
µs range:

```
bench: 118 MB corpus, post-prefilter
                                interp     aot-cached
literal /static/                0.285 s    0.287 s    (essentially noise)
```

ugrep does the same search in 2 ms via mmap + memchr-spanning-the-
whole-file. The 100× gap astrogre still shows vs ugrep is **not**
something specialization can close — it's process startup +
per-line getline + CTX init dominating, addressable by folding line
iteration into the AST too.

### The right pattern: wrap algorithms as nodes
This is the architectural lesson the sample makes obvious.  ASTro's
specializer can't *invent* algorithmic optimizations; what it gives
you is a free composition mechanism — once an optimization is
expressed as a node, the framework hashes it, code-store-shares it,
and inlines its `body` operand.  Engineering shape:

> *Identify an algorithmic optimization. Wrap it in a node. Have the
> parser emit it under the right precondition. The bake handles the
> rest.*

Five prefilter nodes have landed, each fitting the same shape and
each directly exercising AVX2 / glibc-SIMD where applicable:

| node                            | algorithm                                 | parser trigger                         |
|---------------------------------|-------------------------------------------|----------------------------------------|
| `node_grep_search_memmem`       | glibc memmem (two-way string match)       | ≥ 4-byte literal prefix                |
| `node_grep_search_memchr`       | glibc memchr (AVX2 PCMPEQB)               | ≥ 1-byte literal prefix                |
| `node_grep_search_byteset`      | N × `vpcmpeqb` + OR (≤ 8 bytes)           | small first-byte set (alt of literals) |
| `node_grep_search_range`        | `vpsubusb / vpminub / vpcmpeqb`           | single contiguous-range first class    |
| `node_grep_search_class_scan`   | Hyperscan-style Truffle (PSHUFB × 2 + AND) | arbitrary 256-bit first class          |

The architectural point is that **the prefilter and the bake
compose**.  The SD for `node_grep_search_memchr(/static/)` contains
the memchr call AND the inlined chain that verifies `"static"` at
each candidate position — both inside the same SD, both visible to
gcc's optimiser, both addressable via dlsym.  Adding each prefilter
required no change to the bake / hash / code-store machinery; the
framework just did the right thing.

Bench impact, 118 MB corpus, full-sweep count, ms/iter (★ = AOT
beats grep AND Onigmo):

| pattern | astrogre +AOT | grep | onigmo | prefilter node fired |
|---|---:|---:|---:|---|
| `/(QQQ\|RRR)+\d+/` | **16** ★ | 85 | 726 | byteset over {Q,R} |
| `/(QQQX\|RRRX\|SSSX)+/` | **24** ★ | 26 | 700 | byteset over {Q,R,S} |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | **503** ★ | 533 | 717 | range `[a-z]` |
| `/[A-Z]{50,}/` | **678** ★ | 1570 | 1099 | range `[A-Z]` |
| `/\b(if\|else\|for\|while\|return)\b/` | 90 | **2.3** | 1060 | byteset over {i,e,f,w,r} |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 10824 | **2.7** | 9353 | Truffle on `\w` (common) |

**4/8 vs grep, 8/8 vs Onigmo** on this set.  The losing patterns
all need multi-pattern literal extraction (Hyperscan Teddy / FDR)
to pull the rare literal `(`, `,`, `)` out of the middle of the
pattern; that's the next big addition (see `todo.md`).

Nodes still on the runway (same shape, would extend the ladder):

| node                          | algorithm                  | parser trigger                                    |
|-------------------------------|----------------------------|---------------------------------------------------|
| `node_grep_search_teddy`      | multi-pattern AVX2 scan    | pattern has ≥ 1 fixed literal at any position     |
| `node_grep_search_bmh`        | Boyer-Moore-Horspool       | `-F` mode short fixed pattern                     |
| `node_grep_lines`             | newline scan + per-line    | grep CLI driver                                   |
| `node_grep_search_ci2`        | twin memchr for /i         | case-insensitive literal-led pattern              |

### Where bake (specifically) might still pay
For grep-shaped workloads the most plausible bake-only wins are
small and around encoding / flag specialization:

- **UTF-8 dot leading-byte cascade.** `node_re_dot_utf8`'s 4-way
  branch on `b < 0x80` / `0xC0` / `0xE0` / `0xF0` could collapse for
  inputs known to be ASCII-only. But that's a *runtime* property of
  the input, not a parse-time property of the pattern — bake can't
  see it without a profile signal.
- **Case-fold backref.** `node_re_backref` has a `c->case_insensitive`
  branch. Splitting into ci / non-ci variants at parse time fixes
  this without bake — the parser knows the flag.
- **Class bitmap as constants.** Bake commits `bm0..3` as immediates.
  gcc *might* turn small classes (`[abc]`) into a switch table. We
  see ~1.11× on `class-word`; small.

### TL;DR
- Algorithmic optimizations live in *new node types*; bake then
  composes them with the rest of the AST for free.
- Bake's contribution shows up cleanly when the inner per-iter work
  is non-trivial. For grep-shaped workloads where prefilter does
  most of the heavy lifting, bake is in the noise.
- "ASTro fast" needs both: per-node algorithmic care (memchr,
  PSHUFB, BMH, …) AND specialization (so the algorithmic shell can
  compose with the regex verify it wraps).

## Driver: grep on top

`main.c` is the grep front-end.  It does no regex work itself;
everything goes through the backend ops.  The interesting parts:

* `getline` per file / stdin, then `backend->search` per line — one
  pattern compile per pattern, reused across the whole input.
* `--color` / `-o`: drive `backend->search_from` in a loop to
  enumerate every match on a line.  Zero-width matches advance the
  cursor by one byte to avoid spinning.
* `-w` (whole-word): wraps the pattern in `\b...\b` at the regex
  level (with `-F` escaping the literal first).
* `-r`: `opendir` + recursive descent; skips dotfiles by default.
* `--via-prism`: replaces each `-e PATTERN` with the body of the
  first `/.../` found inside it (parsed as Ruby source by prism).
  Useful for piping snippets straight from Ruby code.
