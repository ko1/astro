# astrogre — todo

Backlog of features and performance work.  Companion to
[`done.md`](./done.md).  Ordered roughly by impact / pain level.

## Performance — next up

### AOT / PG specialization (abruby-style)
Wire the framework's `astro_code_store` into `OPTIMIZE` / pattern
compile so we get the four-mode loop the rest of ASTro already has:

| mode             | first run                              | subsequent runs                |
|------------------|----------------------------------------|--------------------------------|
| AOT compile      | parse → `astro_cs_compile(root)` → build all.so | -                     |
| AOT cached       | parse → `astro_cs_load(root)` → run    | swapped dispatcher fires       |
| PG compile       | parse → run-and-record → `astro_cs_compile_hopt` | -                    |
| PG cached        | parse → `astro_cs_load(root)` → run with profile baked in     |        |

For a long-running grep on a single pattern that's the difference
between "interpret the chain N million times" and "run one inlined
C function N million times".  Should close most of the
astrogre-vs-Onigmo gap on literal/anchored cases.

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
