# astrogre — todo

Backlog of features and performance work.  Companion to
[`done.md`](./done.md).  Ordered roughly by impact / pain level.

## Performance — next up

### Search-loop fused SD
The biggest lever still on the table.  Right now AOT bakes one C
function per AST node and the search loop calls it once per starting
position; per-iter dispatch is already cheap (single indirect call
to a hot BTB target), so the AOT win on grep is small.  A wrapping
SD that takes `(str, len, &match)` and runs the for-each-start
loop + the match in one inlined body would drop the per-iter
overhead to near zero — and we'd finally have a single C function
the C compiler can vectorise over.

### Real PG signal
`--pg-compile` is wired but currently aliases to `--aot-compile`
because `HOPT == HORG`.  Real signals to bake:
- **Hot-alternative reordering**: count branch hits at each
  `node_re_alt`, emit alternation with the hot one tested first.
- **Capture elision**: if no backreference reads a group during the
  profile run, drop its save/restore.
- **Iteration-count specialisation**: bake unrolled fixed-N
  variants for repetitions whose observed count is concentrated.

### Literal-prefix prefilter
Independent of the AST and a pure C-level win.  For unanchored
patterns with a fixed-byte prefix, use Boyer-Moore-style scan to
find candidate positions and verify with the AST.  Should drop
`literal-rare` from 880 ms to memchr-bound (~20 ms).

### First-byte bitmap
Even simpler than full BMH: at compile time, build a 256-bit bitmap
of allowed first bytes; skip ahead using a vectorised scan.

### Inline small char-classes as comparisons
For a class like `[abc]`, `b == 'a' || b == 'b' || b == 'c'` is
faster than the bitmap test because gcc folds it to a switch table
or SIMD comparison.  Specializer candidate.

### JIT
Once the search-loop fused SD lands, plug the standard ASTro JIT
path: the rep_cont sentinel keeps the AST a DAG, so hash-keyed
code-store caching applies.

### Capture state on the stack instead of CTX
`c->starts[]` / `c->ends[]` / `c->valid[]` are 32 entries each, ~750
bytes per CTX.  Most patterns have ≤ 4 groups.

### Literal-prefix prefilter
The single biggest miss vs ripgrep / ugrep.  For unanchored patterns
with a fixed-byte prefix, use Boyer-Moore-style scan to find
candidate positions and verify with the AST.  Independent of the AST
and a pure C-level win.  Cheap version: `memchr` for the first
literal byte; full version: BMH on a longer required substring.

### First-byte bitmap
Even simpler than full BMH: at compile time, build a 256-bit bitmap
of allowed first bytes; skip ahead using a vectorised scan.

### Inline small char-classes as comparisons
For a class like `[abc]`, `b == 'a' || b == 'b' || b == 'c'` is
faster than the bitmap test because gcc folds it to a switch table
or SIMD comparison.  Specializer candidate.

### JIT
Once specialization works, plug the standard ASTro JIT path: the
rep_cont sentinel keeps the AST a DAG, so hash-keyed code-store
caching applies.

### Capture state on the stack instead of CTX
`c->starts[]` / `c->ends[]` / `c->valid[]` are 32 entries each, ~750
bytes per CTX.  Most patterns have ≤ 4 groups.

### Code-store mtime invalidation
When `node.def` changes, every cached `SD_*.c` is stale, but
`astro_cs_init`'s version arg is currently `0`.  Pass the binary
mtime so a recompile forces a full rebuild.

## Language gaps

### Lookbehind
- `(?<=...)` and `(?<!...)`.  Parser explicitly errors out.
- Fixed-length form: compute body length at parse, jump back.
- Variable-length: needs the same continuation-passing machinery
  that lookahead uses but in reverse.

### Atomic groups and possessive quantifiers
- `(?>...)` and `*+`, `++`, `?+`.
- Possessive *parsed* but degraded to plain greedy.
- Needs a "commit barrier" inside the rep_frame protocol.

### `\k<name>` actually following the name table
- Named captures are recognised but stored only by index.
- `\k<name>` currently matches group 1 unconditionally.

### Conditional groups, recursion, options
- `(?(cond)yes|no)`.
- `\g<name>` / `\g<n>` recursive subroutine calls.
- `(?#...)` comments — easy.
- `(?adlux-imsx)` Ruby's "options around" — partially landed.

### Unicode
- `\p{...}` / `\P{...}` Unicode property classes.
- Unicode case folding for `/i` (currently ASCII only).
- `\X` extended grapheme cluster.

### Multi-byte chars in classes
- `[äé]` currently builds an ASCII bitmap and writes the high bytes
  byte-by-byte, which doesn't match a single codepoint.
- Want: hybrid class — ASCII bitmap + sorted codepoint range list.

### Encodings
- EUC-JP (`/e`).
- Windows-31J (`/s`).
- Mostly relevant for legacy Ruby code; gated on demand.

### Anchors / boundaries
- `\G` "anchor at last match end".
- `\R` line break.

### `Regexp.new(str)` / `Regexp.compile(str)`
- The `--via-prism` path only catches *literal* `/.../` regexes
  from prism.  A separate path that takes a runtime string and a
  flags arg would let astrogre be used as a regex *library* from a
  host program.

## API / driver

### grep CLI gaps vs GNU grep
- `-A`/`-B`/`-C` context lines.
- `-Z` / `-z` NUL-delimited output / input.
- `--include` / `--exclude` glob filters during `-r`.
- `--include-dir` / `--exclude-dir`.
- `-q` quiet (exit-code-only).
- Binary-file detection (currently we always treat input as text).
- `--mmap` for large files (currently `getline`-based).

### Engine API
- `MatchData`-like result struct exposing every group, named or
  numbered, with line / column information.
- A `gsub`-equivalent: walk the input, repeatedly match, splice in
  a replacement.
- A `scan`-equivalent: enumerate non-overlapping matches into a
  caller-supplied callback.

### Diagnostics
- Better parse error reporting (line / column from the prism source
  span, not the regex offset).
- Optional trace mode that prints each dispatch + position.
- Feed astrogre through `re_test` corpora (PCRE / Onigmo /
  re2-tests) to find behaviour gaps systematically.

## Tests

- More UTF-8 coverage once `[multi-byte char-class]` lands.
- Cross-check the grep CLI's output against GNU grep on a battery
  of patterns + corpora to catch drift in match positions.
- Pathological backtracking inputs (`/(a+)+b/`) — currently exposes
  exponential blowup; documented limitation.
