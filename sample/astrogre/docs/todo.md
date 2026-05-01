# astrogre — todo

Backlog of features and performance work.  Companion to
[`done.md`](./done.md).  Ordered roughly by impact / pain level.

## Performance — next up

### Multi-pattern / Hyperscan-Teddy literal anchor scan
The single biggest miss vs ugrep on the bench.  Patterns like
`/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` carry forced literals (`(`,
`,`, `)`) somewhere inside; ugrep extracts them and runs a
multi-pattern Teddy / FDR scan, then verifies.  We only look at
the *start* of the pattern today.

Plan:
- Walk the IR collecting "must-appear" literal byte sequences from
  any position, with their min-distance from match start.
- Pick the rarest (or longest) as the anchor.
- New node: `node_grep_search_teddy(body, anchors, max_back)` —
  AVX2 multi-pattern scan; for each anchor hit, run the body from
  positions in `[hit - max_back, hit]`.

This would close the `(\w+)\s*\(...\)` and `(\d+\.\d+\.\d+\.\d+)`
gaps with grep.  Implementation cost: ~500-1000 lines (parser
extraction + AVX2 multi-pattern scanner + back-up logic).

### Line iteration as an AST node
The CLI's whole-file mmap path (`process_buffer` in main.c) gets
us most of the way; for prefilter-eligible patterns it's already
within ripgrep speed.  Putting the line-iteration logic itself in
the AST as `node_grep_lines(body)` would let the specialiser bake
the line-bound search loop with the inlined body — useful when
the pattern is simple enough that even the memrchr/memchr per
match becomes a meaningful share of wall time.  Lower priority
now that the C-level mmap loop landed.

### Real PG signal
`--pg-compile` is wired but currently aliases to `--aot-compile`
because `HOPT == HORG`.  Real signals to bake:
- **Hot-alternative reordering**: count branch hits at each
  `node_re_alt`, emit alternation with the hot one tested first.
- **Capture elision**: if no backreference reads a group during
  the profile run, drop its save/restore.
- **Iteration-count specialisation**: bake unrolled fixed-N
  variants for repetitions whose observed count is concentrated.

### Twin-memchr scan for /i case-fold
`/foo/i` — both 'f' / 'F' could start a match.  Either:
- Two memchrs per input chunk + min-position pick.
- Or 2-byte byteset entry: pre-compute {'f', 'F'} and use the
  existing `node_grep_search_byteset`.
The second is trivial — just extend `ire_collect_first_byte_set`
to handle `/i` literals by pushing both cases.

### `node_grep_search_bmh` for `-F` mode
glibc memmem is two-way; classic Boyer-Moore-Horspool with a
bake-time bad-character table can be faster on short needles.

### Inline small char-classes as comparisons
For `[abc]`, `b == 'a' || b == 'b' || b == 'c'` may fold to a
switch table or SIMD compare faster than the bitmap test.
Specializer candidate.

### Capture state on the stack instead of CTX
`c->starts[]` / `c->ends[]` / `c->valid[]` are 32 entries each, ~750
bytes per CTX.  Most patterns have ≤ 4 groups.

### Code-store mtime invalidation
When `node.def` changes, every cached `SD_*.c` is stale; pass the
binary mtime to `astro_cs_init` so a recompile forces a rebuild.

### JIT
Once the Teddy / line-iteration nodes land, plug the standard
ASTro JIT path: code-store sharing applies because the AST is a
DAG.

## Language gaps

### Lookbehind
- `(?<=...)` and `(?<!...)`.  Parser explicitly errors out.
- Fixed-length form: compute body length at parse, jump back.
- Variable-length: continuation-passing in reverse.

### Atomic groups and possessive quantifiers
- `(?>...)` and `*+`, `++`, `?+`.
- Possessive *parsed* but degraded to plain greedy.
- Needs a "commit barrier" inside the rep_frame protocol.

### `\k<name>` actually following the name table
Named captures are recognised but stored only by index.  `\k<name>`
currently matches group 1 unconditionally.

### Conditional groups, recursion
- `(?(cond)yes|no)`.
- `\g<name>` / `\g<n>` recursive subroutine calls.
- `(?#...)` comments.

### Unicode
- `\p{...}` / `\P{...}` property classes.
- Case folding for `/i` (currently ASCII only).
- `\X` extended grapheme cluster.

### Multi-byte chars in classes
`[äé]` currently builds an ASCII bitmap and writes high bytes
byte-by-byte, which doesn't match a single codepoint.  Want a
hybrid class — ASCII bitmap + sorted codepoint range list.

### Encodings
EUC-JP (`/e`), Windows-31J (`/s`).  Gated on demand.

### Anchors / boundaries
- `\G` "anchor at last match end".
- `\R` line break.

### `Regexp.new(str)` / `Regexp.compile(str)`
The `--via-prism` path only catches literal `/.../` regexes from
prism source.  A separate runtime-string + flags entry would let
astrogre be used as a regex *library* from a host program.

## API / driver

### grep CLI gaps vs GNU grep
- `-A` / `-B` / `-C` context lines.
- `-Z` / `-z` NUL-delimited output / input.
- `--include` / `--exclude` glob filters during `-r`.
- `-q` quiet (exit-code-only).
- Binary-file detection.
- `--mmap` for large files (currently `getline`-based).

### Engine API
- `MatchData`-like result struct exposing every group with line /
  column info.
- `gsub`-equivalent: walk input, repeatedly match, splice
  replacements.
- `scan`-equivalent: enumerate non-overlapping matches via callback.

### Diagnostics
- Parse-error line / column from the prism source span.
- Trace mode (per dispatch + position).
- Feed astrogre through `re_test` corpora (PCRE / Onigmo / re2-tests).

## Tests

- More UTF-8 coverage when multi-byte char-class lands.
- Cross-check the grep CLI output against GNU grep on a battery
  of patterns + corpora to catch drift in match positions.
- Pathological backtracking (`/(a+)+b/`) — currently exposes
  exponential blowup; documented limitation.
