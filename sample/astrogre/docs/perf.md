# astrogre performance notes

This is the v1 sample.  ASTro's specialization / code-store machinery
is now wired up via the abruby-style modes
(`--aot-compile` / cached / `--pg-compile` / `--plain`); the
performance gain on grep-shaped workloads is small for the reasons
documented under "AOT specialization" below.  The doc records what
*did* and *did not* help, so the next pass (literal-prefix prefilter,
real PG profile signal) can pick up where this one left off.

## Cross-tool grep comparison

`bench/grep_bench.sh` runs each tool on a 118 MB corpus (C / header
source replicated to ~3.3 M lines) and reports best-of-5 wall-clock
seconds.

```
                         grep   ripgrep  +onigmo  interp  aot-cached
literal    /static/      0.002   0.035    0.221   0.884   0.872
rare       /specialized_dispatcher/
                         0.035   0.020    0.190   0.855   0.866
anchored   /^static/     0.002   0.035    0.227   0.641   0.643
case-i     /VALUE/i      0.002   0.051    0.273   0.724   0.735
alt-3      /static|extern|inline/
                         0.002   0.048    1.131   2.010   1.950
class-rep  /[0-9]{4,}/   0.002   0.054    0.715   1.284   1.296
ident-call /[a-z_]+_[a-z]+\(/
                         0.002   0.183    3.253   3.814   3.792
count -c   /static/      0.002   0.027    0.218   0.882   0.867
```

Reading this:

- **ugrep** is essentially memory-bandwidth-bound: SIMD memchr for
  literals + lazy DFA / literal-prefix prefilter for the rest.  Stays
  under 5 ms even on the regex cases.
- **ripgrep** is in a similar class.
- **Onigmo** is the right comparison for v1 — both engines are
  backtracking matchers without a literal-prefix prefilter, both walk
  the input position-by-position.  Onigmo is currently 2–4 × faster.
- **astrogre interp vs aot-cached**: AOT shaves 0–4 % depending on the
  pattern.  The reason is below.

## AOT specialization: why the gain is small for grep

`astrogre_pattern_aot_compile` runs the standard ASTro AOT loop —
`astro_cs_compile` writes one C function per AST node, `make` builds
`code_store/all.so`, `astro_cs_load` flips each node's
`head.dispatcher` to the corresponding `SD_<hash>` symbol.  Inner SDs
are renamed `_INL` and exposed via thin externally-visible wrappers
(borrowed verbatim from luastro's
`luastro_export_sd_wrappers`); a side array tracks every allocated
NODE so the post-build re-resolve patches the whole chain rather than
just the root.

What the SD ends up containing for `/static/` (the pattern's root
function, decompiled):

```
  bounds-check c->pos + 6 vs c->str_len
  cmpl  $0x74617473, (str+pos)        ; "stat"
  jne   fail
  cmpw  $0x6369,    4(str+pos)         ; "ic"
  jne   fail
  set ends[0], valid[0]; ret 1
fail:
  ret 0
```

Roughly 16 instructions for the entire chain — `cap_start → lit("static") →
cap_end → succ` — and the inner `_INL` functions all inline cleanly.
That's about as tight as we can get without folding the search loop
itself into the SD.

So why does AOT only save ~1 % per search?  Because the indirect
dispatch chain that the SD removes is *cheap*: 3 indirect calls per
position, all to the same target as the previous position, all
predicted correctly by the BTB.  The cost goes from ~3 ns of
dispatch + memcmp to ~0 ns of dispatch + memcmp — and memcmp is the
larger half.  At 100 M positions, savings work out to about 100 ms,
which matches what the bench shows.

The real win for AOT in our shape would be **fusing the search loop
into the SD**, so the SD becomes "scan the input and report match"
rather than "match at one position".  That's planned but not done —
it requires a custom specialize hook that recognises the search
context, since ASTroGen's specialiser only knows about the per-node
chain.

## In-engine microbench reference

`./astrogre --bench` against a 16 KiB synthetic input:

```
literal-tail       /match/         103 µs / search
literal-i          /MATCH/i         77 µs
class-digit        /\d+/           166 ns      (matches very early)
class-word         /\w+/           208 µs      (matches late)
alt-3              /cat|dog|match/ 296 µs
rep-greedy         /a.*z/          198 µs
group-alt          /(a|b|c)+m/     418 µs      (no match — exhaustive)
anchored           /\Amatch/       118 ns      (one-position search)
dot-star           /.*match/       222 µs
```

`anchored` is the engine's *cost floor*: roughly 120 ns to walk the
chain `cap_start → lit → cap_end → succ` once.  The unanchored cases
are dominated by the C-level outer loop in `astrogre_search`, which
retries each starting position.

## What helped

### Coalesce adjacent literals at parse time
`parse_concat` checks the previous node before pushing: if the new
atom is `IRE_LIT` and the previous is too (and the case-fold flag
matches), the bytes are concatenated.  Turns `\AHello, World/` from
13 lit nodes into 1.

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
under a capture-0 wrap.

### Single rep_cont sentinel
There is exactly one `node_re_rep_cont` allocation, shared by every
rep node in every compiled pattern.  Keeps the AST acyclic (so
Merkle hashes survive) and avoids per-rep continuation duplication.

### Backend abstraction
`backend.h` decouples the grep CLI from the matcher.  Adding the
Onigmo backend was ~150 lines of wrapper + a 60-line `build_local.mk`
that compiles Onigmo without the autoconf / libtool dance.

### `restrict` on `c` and `n`
Following the rest of the codebase.

### 256-bit bitmap class as `uint64_t × 4`
Inline storage in the union; the matcher does
`bm[b >> 6] >> (b & 63)` with no indirection.

### `_INL` rename + extern wrapper post-process
Without it, `astro_cs_load` only sees the root SD; inner nodes' SDs
stay hidden behind `static inline` and the runtime chain bounces
through host-side `DISPATCH_*` for every per-node touch.  With the
post-process, every inner SD is reachable via dlsym, the side-array
re-resolve loop patches every node, and the chain stays inside
`all.so`.

## What did not help / was abandoned

### Per-rep "static" continuation node
Earlier draft allocated a `rep_cont` *per repeat node* so the
continuation could carry the rep-frame pointer inline and avoid the
`c->rep_top` indirection.  Made the AST cyclic, broke Merkle hashing
and code-store sharing.

### "Body returns one success per call" repetition model
Simpler design where the rep node iterates body locally without
continuation passing through body's tail.  Failed `(a|ab)*c` on
`abc` (the inner alt has to be retryable across iteration backtracks).

### UTF-8 multi-byte char-class
Tried storing class as a 256-bit ASCII bitmap *plus* a sorted list of
`(lo, hi)` codepoint ranges.  Binary search dominated the dispatch
on classes with non-ASCII content.

### Inlining the search loop into `EVAL`
Tried generating a specialized search function per pattern that
inlines the start-position loop and the AST entry point.  The win on
`anchored` is zero (already 1-pos) and on `dot-star` is in the noise.

### Storing `bytes` for class as `const char *`
Saves 24 bytes per node struct vs four `uint64_t`s, but the matcher
then has to load the bitmap pointer; inline won by ~12 % on
`class-word`.

### Possessive quantifiers
Parsed but degraded to plain greedy.  Possessive would forbid
backtracking across the rep boundary.

### PG (profile-guided) bake
`--pg-compile` is accepted as a CLI flag for parity with abruby but
currently behaves as `--aot-compile` because we have no profile
signal: `HOPT == HORG` in node.h, so the baked SD is the same as
AOT's.  A real PG signal for regex (hot-alternative reordering, hot
iteration counts, capture-elision when never read) is on the runway.

## Things on the runway, not started

* **Search-loop fused SD.**  The biggest performance lever still on
  the table.  Generate a wrapping `SD_<hash>_search(c, str, len,
  starts, ends)` that does the for-each-start-position loop plus the
  match attempt as one inlined function.  Should drop dispatch +
  loop overhead per attempt to single instructions, closing most of
  the Onigmo gap on literal/anchored cases.
* **Literal-prefix prefilter.**  Independent of the AST and a pure
  C-level win.  For unanchored patterns with a fixed-byte prefix,
  use Boyer-Moore-style scan to find candidate positions and verify
  with the AST.  Should drop `literal-rare` from 880 ms to memchr-
  bound (~20 ms).
* **First-byte bitmap.**  At compile time, build a 256-bit bitmap
  of allowed first bytes; skip ahead using a vectorised scan.
* **Real PG signal.**  Hot-alternative reordering: count which alt
  branch wins at each `node_re_alt`, bake the hot one as the first
  branch.  Capture elision: if no backreference uses a given group
  during the profile run, the bake can drop its save/restore.
* **JIT.**  Once specialization is fused with the search loop, plug
  the standard ASTro JIT path: hash-keyed code-store caching applies
  because the AST is a DAG.

These are listed in [`todo.md`](./todo.md).
