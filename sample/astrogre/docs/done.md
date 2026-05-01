# astrogre — implemented

What's in v1, by category.  Companion to [`todo.md`](./todo.md).

## Engine — language surface

### Atoms
- Literal byte sequence.  Adjacent literals coalesce at parse time.
- `.` (dot).  Four variants: ASCII, ASCII-`/m`, UTF-8, UTF-8-`/m`.
- Character class `[...]`, negated `[^...]`.
- ASCII ranges in classes (`[a-z]`).
- Escaped char-class shortcuts inside `[...]`: `\d \D \w \W \s \S`.
- Numeric escapes `\xHH`, `\0`, `\n \t \r \f \v \a \e`.
- Anchors: `\A` `\z` `\Z` `^` `$` `\b` `\B`.
- Plain-literal escapes `\\ \/ \. \^ \$ \( \) \[ \] \{ \} \| \* \+ \? \-`.

### Quantifiers
- `*` `+` `?`, greedy and lazy (`*?` `+?` `??`).
- `{n}` / `{n,}` / `{n,m}`, greedy and lazy.
- Possessive (`*+` `++` `?+`) parsed; falls through to greedy.

### Groups
- Capturing groups `(...)`.
- Non-capturing `(?:...)`.
- Named capture `(?<name>...)` — captured by left-to-right index.
- Inline-flag groups `(?ixm-ixm:...)` and `(?ixm)`.
- Positive lookahead `(?=...)` and negative `(?!...)`.

### Backreferences
- `\1`–`\9`.

### Flags / encoding
- `/i` (ASCII case-fold).  `/m` (dot matches newline).  `/x` (extended).
  `/n` (ASCII byte mode).  `/u` (UTF-8 default).

### Front-end
- prism integration (`astrogre_parse_via_prism`): walks any Ruby
  source's AST, picks the first `PM_REGULAR_EXPRESSION_NODE`.
- `astrogre_parse_literal`: `/pat/flags` syntax for tests / CLI.
- `astrogre_parse_fixed`: `-F` mode, no regex parser.

## Engine — runtime

- AST in continuation-passing form: every match-node carries a
  `next` operand; chain ends in `node_re_succ`.
- Repetition via shared `node_re_rep_cont` sentinel + per-call
  `rep_frame` on `c->rep_top` (no AST cycles).
- Capture group 0 wrapped around the whole AST so the matcher records
  the overall match span the same way as user-numbered groups.
- `astrogre_search` and `astrogre_search_from` for caller-resumable
  enumeration of hits.

## Engine — performance work that landed

- Adjacent-literal coalescing at the IR level.
- Pre-folding pattern literals to lowercase under `/i`.
- Anchored-`\A` short-circuit in the search loop.
- Single global rep_cont sentinel.
- 4× `uint64_t` inline class bitmap.
- `restrict` on `CTX *` and `NODE *` parameters across `node.def`.

## Drivers / tooling

- **grep CLI**: `./astrogre PATTERN [FILE...]` with the standard
  options `-i -n -c -v -w -F -l -L -H -h -o -r -e --color=auto`.
  Recurses into directories with `-r`, skips dotfiles.  Multi-pattern
  support via repeated `-e PATTERN`.  Filename-only output (`-l`,
  `-L`).  `-o` prints just the matched span(s).
- **`--color`**: red on match, green on line numbers, magenta on
  filenames, matching GNU grep.  `--color=auto` honours `isatty`.
- **`--via-prism`**: parse the pattern argument as Ruby source via
  prism, take the first `/.../`'s body as the search pattern.
- **`--backend=astrogre|onigmo`**: switchable matcher backend at
  runtime.  Onigmo is built locally with `make WITH_ONIGMO=1` (a
  hand-rolled `build_local.mk` skips the autoconf / libtool dance).
- **`--self-test`** (44 cases) and **`--bench`** (in-engine
  microbench) preserved as flags.
- **`bench/grep_bench.sh`**: cross-tool comparison harness; runs
  grep / ripgrep / astrogre / astrogre+onigmo on the same corpus
  and patterns and reports best-of-N seconds per tool.

## Backend abstraction

- `backend.h` declares an ops table (`compile / search / search_from
  / free / aot_compile`) that the grep CLI talks to.
- `backend_astrogre.c` (~80 lines) wraps the in-house engine.
- `backend_onigmo.c` (~110 lines) wraps Onigmo's `onig_new` /
  `onig_search` / `onig_region`.  Both implement `-F` (Onigmo by
  escaping metacharacters at compile time).

## AOT / cached / PG

- **`--aot-compile` (`-C`)**: compile every pattern to
  `code_store/c/SD_<hash>.c`, build `code_store/all.so`, swap each
  node's dispatcher, then run.  Subsequent runs (default mode)
  pick up the cached `all.so` automatically via `astro_cs_load` in
  `OPTIMIZE`.
- **default (cached)**: at pattern allocation time, `OPTIMIZE` calls
  `astro_cs_load` for every node.  Hits use the specialized
  dispatcher; misses fall back to the interpreter.
- **`--pg-compile` (`-P`)**: accepted for CLI parity with abruby;
  currently routes through the same path as `--aot-compile`.  No PG
  profile signal exists yet (HOPT == HORG).
- **`--plain` (`--no-cs`)**: bypass the code store entirely.

Inner SDs are made externally visible via the
`astrogre_export_sd_wrappers` post-process (borrowed from luastro):
each generated SD gets renamed to `SD_<hash>_INL` plus a thin
externally-visible wrapper, so `dlsym` finds every node, not just
the root.  A side array tracks every allocated NODE so the post-build
re-resolve patches the whole chain.

## Search loop folded into the AST

The for-each-start-position search loop is itself a node
(`node_grep_search`) — its EVAL is the loop, and the specializer
treats `body` as a regular NODE * operand, so the SD bakes the
loop AND the inlined regex chain into a single C function.

For `/static/` against a 16 KiB string, the result is ~30 instructions:
loop, `cmpl + cmpw` for the literal, `vmovdqu` for the per-iter
capture-state reset, no indirect calls, no DISPATCH chain.  In-engine
microbench: **22.75 s → 3.15 s on `literal-tail` (7.2× speedup)**.

For the grep CLI to *see* the fusion gain, the per-line `getline`
+ `CTX_struct` zero-init overhead has to go.  The whole-file
mmap path (below) is what unlocks that.

## Whole-file mmap path in the grep CLI

`process_buffer` (main.c) replaces per-line `getline` for regular
files when the pattern has a SIMD/libc prefilter (memchr / memmem
/ byteset / range / class_scan).  The file is mmap'd once, the
backend's `search_from` runs in a loop over the whole buffer, and
each match's containing line is identified via memrchr/memchr.
For plain backtracking patterns (no prefilter), the per-line
streaming loop wins because each line is short.

Bench impact, line-by-line grep CLI on the 118 MB corpus
(post-mmap, post-prefilter):

| pattern             | prior interp | mmap interp | grep | ripgrep |
|---------------------|-------------:|------------:|-----:|--------:|
| `/static/` literal  | 0.285 s      | **0.077** s | 0.002 | 0.034 |
| literal-rare        | 0.266        | **0.026**   | 0.035 | 0.020 |
| `/^static/`         | 0.273        | **0.076**   | 0.002 | 0.036 |
| `-c /static/`       | 0.279        | **0.048**   | 0.002 | 0.027 |

3-10× over the per-line baseline.  The `literal-rare` row is the
headline: 26 ms vs ripgrep's 20 ms, even faster than ugrep's
35 ms — the SIMD memmem in our `node_grep_search_memmem` is at
ripgrep speed, the mmap path lets it actually fire.

Gating: `backend.h` exposes a `has_fast_scan` op; the CLI
queries it before taking the mmap path.  Onigmo backend leaves
the op NULL ("always yes" — Onigmo has its own internal
prefilter).  -v invert mode skips the mmap path because it needs
to enumerate every line, including non-matching.

## Prefilter ladder — algorithms as nodes

Five algorithmic prefilter nodes, all sharing `node_grep_search`'s
shape: the EVAL is the SIMD / libc scan, the body operand is the
regex chain that verifies at each candidate.  The specialiser
inlines the body and bakes the algorithmic constants (first byte,
needle bytes, range bounds, nibble tables, packed byte-set) as
immediates inside the SD.

The parser's analysis ladder picks the most specific that fits:

| node                            | when emitted                              | inner algorithm |
|---------------------------------|-------------------------------------------|-----------------|
| `node_grep_search_memmem`       | ≥ 4-byte literal prefix, no `/i`          | glibc memmem (two-way) |
| `node_grep_search_memchr`       | ≥ 1-byte literal prefix, no `/i`          | glibc memchr (AVX2) |
| `node_grep_search_byteset`      | ≤ 8 distinct first bytes (alt of literals) | N × `vpcmpeqb` + OR |
| `node_grep_search_range`        | single contiguous-range first class       | `vpsubusb / vpminub / vpcmpeqb` |
| `node_grep_search_class_scan`   | arbitrary 256-bit first class (\w, etc.) | Truffle (PSHUFB nibble lookup) |
| `node_grep_search`              | none of the above                         | plain start-position loop |

Detection helpers (in parse.c):

- `ire_collect_prefix` — longest fixed literal prefix
- `ire_first_class` — first class node walking past zero-width
- `bm_is_single_range` — recognises `[a-z]`-style classes
- `ire_collect_first_byte_set` — collects distinct first bytes from
  alt branches; bails if > 8
- `build_truffle_tables` — Hyperscan-style nibble encoding
  for arbitrary 256-bit classes

`/i` disables the prefilter for now (would need twin memchr for
case-fold).  All scans flagged behind `__AVX2__` with scalar
fallback.

## Cross-engine bench

Latest results, 118 MB corpus, full-sweep count (`-c` semantics
mirrored in `--bench-file`), best-of-3 ms/iter:

| pattern | astrogre interp | astrogre +AOT | astrogre +onigmo | grep | ripgrep |
|---|---:|---:|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/` | 19 | **12** ★ | 488 | 74 | 23 |
| `/(QQQX\|RRRX\|SSSX)+/` | 40 | **23** ★ | 535 | 27 | 25 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | 926 | **444** ★ | 548 | 507 | 185 |
| `/[A-Z]{50,}/` | 741 | **640** ★ | 919 | 1525 | 185 |
| `/\b(if\|else\|for\|while\|return)\b/` | 252 | 90 | 894 | **2.5** | 118 |
| `/[a-z][0-9][a-z][0-9][a-z]/` | 1008 | 429 | 535 | **4** | 186 |
| `/(\d+\.\d+\.\d+\.\d+)/` | 566 | 397 | 554 | **4** | 48 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 13061 | 10096 | 14532 | **5** | 351 |

★ = astrogre + AOT beats grep AND Onigmo.  Bold = winner per row.

**4/8 vs grep (ugrep 7.5 + PCRE2-JIT), 8/8 vs Onigmo** on this set.
The losing patterns all need multi-pattern literal extraction
(Hyperscan Teddy / FDR), which would be the next big addition.

See [`perf.md`](./perf.md) for the bench analysis and
[`runtime.md`](./runtime.md) for the architectural lesson —
ASTro's bake composes with each prefilter node uniformly,
turning each algorithm into one specialised C function with
constant operands inlined.
