# astrogre performance notes

This is the v1 sample.  ASTro's specialization / code-store machinery
is wired into the build (every node has a generated
`SPECIALIZE_node_re_xxx`) but `OPTIMIZE` is currently a no-op — all
numbers below are for the tree-walking interpreter alone.  The purpose
of this doc is to record what *did* and *did not* help on the way to
those numbers, so the next pass (specialization, then JIT) can pick up
where this one left off.

## Cross-tool grep comparison

`bench/grep_bench.sh` runs each tool on a 118 MB corpus (C / header
source replicated to ~3.3 M lines) and reports best-of-3 wall-clock
seconds.

```
                          grep   ripgrep  astrogre  +onigmo
  literal    /static/    0.002    0.034    0.839    0.217
  rare       /specialized_dispatcher/
                         0.035    0.020    0.830    0.178
  anchored   /^static/   0.002    0.035    0.603    0.220
  case-i     /VALUE/i    0.002    0.049    0.699    0.254
  alt-3      /static|extern|inline/
                         0.002    0.049    1.857    1.096
  class-rep  /[0-9]{4,}/ 0.003    0.056    1.293    0.713
  ident-call /[a-z_]+_[a-z]+\(/
                         0.002    0.188    3.801    3.366
  count -c   /static/    0.002    0.027    0.841    0.218
```

Reading this:

- **ugrep** is essentially memory-bandwidth-bound here — for plain
  literals it's a SIMD memchr scan, and even for the alt and class
  cases it stays under 5 ms.  Not a fair comparison target without
  a prefilter on our side.
- **ripgrep** is in a similar class with its lazy-DFA + literal-prefix
  prefilter.
- **Onigmo** is the right comparison for v1: both engines are
  backtracking matchers without a literal prefilter, and both walk
  the input position-by-position.  Onigmo is currently 2–4 × faster.
- **astrogre vs Onigmo** narrows on heavy-regex patterns
  (`ident-call`: 12 % gap) and widens on literal / anchored cases.
  The gap on `literal-rare` (0.83 vs 0.18) is almost entirely
  Onigmo's Boyer-Moore prefilter firing — the matcher core spends
  most of the time in `memcmp` retries, not the regex engine itself.

## In-engine microbench reference

`./astrogre --bench` against a 16 KiB synthetic input:

```
literal-tail       /match/         103 µs / search
literal-i          /MATCH/i         77 µs
class-digit        /\d+/           166 ns      (matches early)
class-word         /\w+/           208 µs      (matches late)
alt-3              /cat|dog|match/ 296 µs
rep-greedy         /a.*z/          198 µs
group-alt          /(a|b|c)+m/     418 µs      (no match — exhaustive)
anchored           /\Amatch/       118 ns      (one-position search)
dot-star           /.*match/       222 µs
```

`anchored` is the engine's *cost floor*: roughly 120 ns is what it
takes to walk the chain `cap_start → lit → cap_end → succ` once.  The
unanchored cases are dominated by the C-level outer loop at
`astrogre_search`, which retries each starting position.

## What helped

### Coalesce adjacent literals at parse time
`parse_concat` checks the previous node before pushing: if the new
atom is `IRE_LIT` and the previous is too (and the case-fold flag
matches), the bytes are concatenated into the previous literal.  This
turns `\AHello, World/` from 13 lit nodes into 1.

### Pre-fold case at parse time
`/Hello/i` becomes `node_re_lit_ci("hello", 5, ...)` — the pattern
side is lowercased once, and the matcher does an asymmetric fold of
the input byte during the byte-by-byte compare.

### Anchored-start short-circuit
The parser sets `pattern->anchored_bos` when it sees a leading `\A`,
and the search loop short-circuits to one starting position.  On the
`\Amatch` bench this is the difference between 118 ns and ~100 µs.

### `astrogre_search_from` for grep enumeration
Letting the caller pick the resume position avoids re-allocating the
`CTX` per match when listing all hits on a line (`-o` / `--color`).

### Fixed-string parser entry
`-F` skips the regex parser entirely and builds a single `node_re_lit`
under a capture-0 wrap.  Saves the overhead of building an IR tree for
patterns that have no metacharacters anyway.

### Single rep_cont sentinel
There is exactly one `node_re_rep_cont` allocation, shared by every
rep node in every compiled pattern.  Keeps the AST acyclic and avoids
per-rep continuation duplication.

### Backend abstraction
`backend.h` decouples the grep CLI from the matcher.  Adding the
Onigmo backend was ~150 lines of wrapper + a 60-line `build_local.mk`
that compiles Onigmo without the autoconf / libtool dance — useful as
a head-to-head reference and as a way to keep the in-house engine
honest about regressions.

### `restrict` on `c` and `n`
Following the rest of the codebase.  Lets gcc keep `c->str`,
`c->str_len`, etc. in registers across the contiguous prefix of a
literal compare.

### 256-bit bitmap class as `uint64_t × 4`
Inline storage in the union, the matcher does
`bm[b >> 6] >> (b & 63)` with no indirection.

## What did not help / was abandoned

### Per-rep "static" continuation node
Earlier draft allocated a `rep_cont` *per repeat node* so the
continuation could carry the rep-frame pointer inline and avoid the
`c->rep_top` indirection.  The catch: the AST then had cycles
(body → rep_cont → body), which broke Merkle hashing and made
code-store sharing impossible.

### "Body returns one success per call" repetition model
A simpler design where the rep node iterates body locally (without
continuation passing through body's tail) made the matcher faster on
straight-line cases but failed `(a|ab)*c` on `abc`.

### UTF-8 multi-byte char-class
Tried storing class as a 256-bit ASCII bitmap *plus* a sorted list of
`(lo, hi)` codepoint ranges.  Binary search dominated the dispatch
on classes with non-ASCII content; pulled before merging.

### Inlining the search loop into `EVAL`
Tried generating a specialized search function per pattern that
inlines the start-position loop and the AST entry point.  The win on
`anchored` is zero (already 1-pos) and on `dot-star` is in the noise.

### Storing `bytes` for class as `const char *` to 32 bytes
Saves 24 bytes per node struct vs four `uint64_t`s, but the matcher
then has to load the bitmap pointer; inline won by ~12 % on
`class-word`.  Reverted.

### Possessive quantifiers
Parsed but degraded to plain greedy.  Possessive would forbid
backtracking across the rep boundary — implementable as a separate
`node_re_rep_pos` with a `setjmp`-style commit barrier.

## Things on the runway, not started

* **AOT / PG specialization (next up).**  The framework already
  generates `SPECIALIZE_node_re_*` from `node.def` and the
  `astro_code_store` runtime is in tree.  Wiring it up gives
  the abruby-style "compile / cached / pg-compile / pg-cached"
  loop: first run specializes the pattern body to one C function,
  subsequent runs `dlopen` it.  The literal prefix would collapse
  into a single basic block; the dispatch overhead disappears.
* **Literal-prefix prefilter.**  The biggest single miss vs ripgrep.
  For unanchored patterns with a fixed-byte prefix (`literal-tail`,
  `dot-star`'s embedded `match`), use a Boyer-Moore-style scan to
  find candidate positions and verify with the AST.  Independent of
  the AST and a pure C-level win — should drop `literal-rare` from
  830 ms to memchr-bound (~20 ms).
* **First-byte bitmap.**  Even simpler: at compile time, build a
  256-bit bitmap of the first possible byte at each starting
  position; skip ahead on mismatch.  Useful for class-led patterns.
* **JIT.**  Once specialization works, plug the standard ASTro JIT
  path: hash-keyed code-store caching applies because the AST is a
  DAG.

These are listed in [`todo.md`](./todo.md) under "performance".
