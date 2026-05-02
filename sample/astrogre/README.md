# sample/astrogre — Ruby-style regex engine on ASTro

`astrogre` (pronounced *astrouga*) is an experimental Ruby/Onigmo-
compatible regex engine implemented as an ASTro sample.  This
directory holds the engine library; the user-facing binary,
[`are`](./are/), is a modern grep CLI built on top.

The matcher is itself an AST: `node.def` defines ~22 match-node kinds
(literal, char-class, dot, anchors, repetition, alternation, capture,
lookahead, backreference) and ASTroGen turns that into a tree-walking
interpreter.  Patterns enter through a small recursive-descent regex
parser (`parse.c`) that takes the bytes between the slashes plus a
flag bitmask and produces the AST.  No external regex parser is
required.

The grep CLI `./are/are` uses the astrogre engine by default.  An
[Onigmo](https://github.com/k-takata/Onigmo) backend can be swapped
in at runtime with `--engine=onigmo` for direct head-to-head
comparison.

## Status

- 118/118 self-tests pass: literals (`/i`), `.` (with `/m`), char
  classes, anchors, greedy/lazy quantifiers, capturing/non-capturing/
  named groups, back-references, lookahead, alternation, `/x`
  extended, inline flag groups, `search_from` enumeration, fixed-
  string mode.
- Standalone `/pat/flags` source-string syntax via
  `astrogre_parse_literal`.
- grep CLI (`are`): full `-i -n -c -v -w -F -l -L -H -h -o -A/-B/-C
  --color=auto`, plus `.gitignore`-aware recursive walk, type
  filters (`-t LANG`), and a parallel worker pool (`-j N`).  See
  [`are/README.md`](./are/README.md).
- Backend abstraction: `--engine=astrogre` (default) or
  `--engine=onigmo` (build with `WITH_ONIGMO=1`).
- `--encoding=utf-8|ascii` selects regex encoding (default UTF-8).
- AOT specialization: `are --aot` writes
  `code_store/c/SD_<hash>.c`, builds `all.so`, and patches every
  node's dispatcher.  Subsequent runs auto-load the cached build
  via `astro_cs_load`.
- The for-each-start-position search loop is itself a node
  (`node_grep_search`), so the specializer fuses the loop AND the
  inlined regex chain into one SD function — the in-engine bench
  shows up to **7.2× over interp on long-buffer searches**.
- Algorithmic prefilter nodes — `node_grep_search_memchr` /
  `_memmem` / `_byteset` / `_range` / `_class_scan` — emitted when
  the parser detects a literal first byte / prefix / byteset.

## Encoding

Two encoding modes, selectable per pattern:

| flag                | mode  | dot advances by | typical use             |
|---------------------|-------|-----------------|-------------------------|
| `--encoding=ascii`  | ASCII | 1 byte          | binary input, ASCII     |
| `--encoding=utf-8`  | UTF-8 | 1 codepoint     | default, modern Ruby    |
| (default)           | UTF-8 | 1 codepoint     | same as `--encoding=utf-8` |
| `/n` source string  | ASCII | 1 byte          | same as `--encoding=ascii` |
| `/u` source string  | UTF-8 | 1 codepoint     | (in `astrogre_parse_literal`) |

What works under either mode:

- ASCII literals, ranges (`[a-z]`, `\d`, `\w`, `\s`), anchors,
  alternation, repetition, captures, backrefs.
- `\b` / `\B` use the 7-bit ASCII word predicate (`[A-Za-z0-9_]`).
- `/i` lowercases ASCII letters at parse time.
- The SIMD prefilter ladder operates on raw bytes and composes
  cleanly with UTF-8 patterns (the body chain re-verifies the full
  codepoint at each candidate).

What's UTF-8-aware:

- `.` advances one full codepoint via the leading-byte cascade
  (`0xxxxxxx` / `110xxxxx` / `1110xxxx` / `11110xxx`); invalid
  leads don't match.
- Multi-byte UTF-8 literals are kept as one `IRE_LIT` token by
  the parser, so quantifiers bind to the codepoint
  (`/é+/` quantifies `é`, not its trailing 0xA9).

What's not supported (see [`docs/todo.md`](./docs/todo.md)):

- Multi-byte characters inside `[...]`.
- `\p{...}` Unicode property classes.
- Unicode case folding for `/i`.
- `\X` extended grapheme cluster.
- EUC-JP and Windows-31J.

`docs/runtime.md` has the full breakdown including how the SIMD
prefilter nodes interact with each encoding.

## Build and run

```sh
cd sample/astrogre

# Build the engine + are CLI
make

# With Onigmo as a switchable backend
make WITH_ONIGMO=1

# Grep usage (see are/README.md for the full flag set)
./are/are 'foo[0-9]+' file.txt
./are/are -n -i 'pattern' src/                    # recursive by default
./are/are -F 'literal_str' src/
./are/are --engine=onigmo --color=always 'pat' file.txt
./are/are --aot 'pat' file.txt                    # AOT-specialise pattern
./are/are --encoding=ascii '\xff+' binary.bin

# Engine-level (no grep CLI in the loop)
make self-test                              # 118-case engine self-test
make bench                                  # in-engine microbench
make bench-file FILE=file PATTERN=/pat/     # bench-file harness
make bench-rg                               # cross-tool grep comparison
make bench-tree                             # recursive-walk comparison
make bench-aot                              # AOT-favourable engine bench

# Pattern AST inspection (dev flag, not in are --help)
./are/are --dump '/(a|b)*c/'
./are/are --verbose -e static file.txt      # phase-by-phase wall-clock
./are/are --aot --cs-verbose -e foo file.txt
```

## Layout

```
node.def              22 match-node kinds (continuation-passing form)
parse.c / parse.h     recursive-descent regex parser
match.c               astrogre_search / astrogre_search_from + rep_cont singleton
backend.h             abstract ops table (compile/search/free)
backend_astrogre.c    in-house engine bound to backend_ops_t
backend_onigmo.c      Onigmo engine bound to backend_ops_t (WITH_ONIGMO=1)
node.c                ASTro framework glue (hash funcs, EVAL, OPTIMIZE)
selftest.c            engine self-test + microbench harnesses
selftest_runner.c     standalone driver behind `make {self-test,bench,bench-file}`
aho_corasick.c / .h   AC trie + branchless scanner used by alt-of-LIT prefilter
are/                  user-facing grep CLI (own README, own Makefile)
bench/grep_bench.rb   cross-tool single-file comparison
bench/aot_bench.rb    engine-internal AOT-favourable bench
bench/tree_bench.rb   recursive walk vs ripgrep / grep -r
onigmo/               cloned, locally-built Onigmo (build_local.mk)
```

## Speed: prefilter ladder + AOT specialization

Two layers of optimisation, both expressed as ASTro nodes:

1. **`node_grep_search`** — the for-each-start-position loop is
   itself a node, so the specialiser fuses the loop AND the
   inlined regex chain into one SD function.
2. **Algorithmic prefilter nodes** — emitted in place of plain
   `node_grep_search` when the parser sees something that lets us
   skip non-candidate positions:

   | node                          | when emitted                        | algorithm |
   |-------------------------------|-------------------------------------|-----------|
   | `node_grep_search_memmem`     | ≥ 4-byte literal prefix             | glibc memmem (two-way) |
   | `node_grep_search_memchr`     | ≥ 1-byte literal prefix             | glibc memchr (AVX2) |
   | `node_grep_search_byteset`    | ≤ 8 distinct first bytes (alt)      | N × `vpcmpeqb` + OR |
   | `node_grep_search_range`      | single contiguous-range first class | `vpsubusb / vpminub / vpcmpeqb` |
   | `node_grep_search_class_scan` | arbitrary 256-bit first class       | Hyperscan-style Truffle (PSHUFB ×2 + AND) |
   | `node_grep_search_ac`         | ≥ 9 leading literal alternatives    | Aho-Corasick with branchless inner loop |

   The bake then composes — first byte / range bounds / packed
   bytes / 16-byte nibble tables become AVX2 `vpbroadcastb` / `set1`
   immediates inside the SD; the inner loop has no indirect call.

### Bench A — grep CLI (118 MB C-source corpus, best-of-7 ms)

`bench/grep_bench.rb` — re-runnable with `make bench-rg`.  All
`are` rows use `-j 1` so the bench measures the engine, not the
parallel walker.  Output to a regular file (NOT `/dev/null` — see
the pitfall note below).  Bold = row minimum.

| pattern                              | are interp | are aot/cached | are +onigmo | grep | ripgrep |
|--------------------------------------|---:|---:|---:|---:|---:|
| `/static/`                           | **38** | 41 | 111 | 74 | 41 |
| `/specialized_dispatcher/`           | **21** | 24 | 39 | 38 | **21** |
| `/^static/` anchored                 | 77 | 80 | 114 | 76 | **41** |
| `/VALUE/i`                           | 85 | 76 | 141 | 98 | **56** |
| `/static\|extern\|inline/`           | 376 | 121 | 1027 | 205 | **63** |
| 12-way alt (AC prefilter)            | 444 | 409 | 2609 | 276 | **139** |
| `/[0-9]{4,}/`                        | 362 | 168 | 601 | 97 | **55** |
| `/[a-z_]+_[a-z]+\(/`                 | 1858 | 1202 | 3400 | 2214 | **203** |
| `-c /static/`                        | **22** | 24 | 71 | 64 | 27 |

`+onigmo` requires `make WITH_ONIGMO=1`; if onigmo isn't linked the
column comes back ERR.  See [`are/README.md`](./are/README.md) for
a fuller analysis of these numbers including which rows ripgrep
takes home and why.

#### Pitfall — DON'T pipe to `/dev/null` while benching grep

GNU grep since
[commit `af6af28`](https://cgit.git.savannah.gnu.org/cgit/grep.git/commit/?id=af6af288)
(Paul Eggert, May 2016, "grep: /dev/null output speedup")
`fstat`s stdout, recognises `/dev/null`, and switches to first-
match-and-exit mode — `-q`-equivalent — because the output won't
be visible anyway.  Benches that redirect to `/dev/null` look like
grep is doing 2 ms over a 118 MB file when in fact it's short-
circuiting after the first match.  `bench/grep_bench.rb`
explicitly avoids this.

### Bench B — engine-level whole-file scan (ms/iter)

`bench/aot_bench.rb` runs the in-engine `bench-file` path: the
118 MB buffer is loaded once, then `astrogre_search` is called N
times.  Other tools run via their CLI in count mode.  Patterns are
deliberately chosen for an AOT-favourable shape (long chain, no
trivial libc memmem available).  Best of 3 ms/iter; bold = row
winner.

| pattern                                 | interp | +AOT | +onigmo | grep | rg    |
|-----------------------------------------|---:|---:|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/`                       | 25 | **14** | 590 | 104 | 27 |
| `/(QQQX\|RRRX\|SSSX)+/`                  | 57 | **26** | 608 | 29 | 27 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/`   | 1126 | 517 | 611 | 778 | **223** |
| `/[A-Z]{50,}/`                           | 472 | **206** | 1026 | 1722 | 306 |
| `/\b(if\|else\|for\|while\|return)\b/`   | 595 | **203** | 2226 | 2241 | 227 |
| `/[a-z][0-9][a-z][0-9][a-z]/`           | 1141 | 506 | 616 | 1114 | **209** |
| `/(\d+\.\d+\.\d+\.\d+)/`                | 467 | 229 | 634 | **93** | 274 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/`    | 7694 | 5314 | 13081 | 6170 | **237** |

#### AOT effect

AOT cuts interp time by **1.4×–2.95×** on every row.  Win count:

- **vs Onigmo: 8/8 wins** — usually 3-15× faster.  Onigmo is a
  bytecode VM dispatching every alt branch, every quantifier
  iter; AOT folds those into a flat SD with no indirect call.
- **vs GNU grep: 4/8 wins**.  Notably:
  - `[A-Z]{50,}`: AOT 206 ms vs grep 1722 ms (8.4×).  The DFA
    explodes on long uppercase runs; `greedy_class` fast-path
    walks them in SIMD.
  - `\b(if|else|for|while|return)\b`: AOT 203 ms vs grep 2241 ms
    (11×).  Long alt-of-LIT body chain compiled into one SD.
  - `(QQQ|RRR)+\d+`, `(QQQX|RRRX|SSSX)+`: byteset prefilter +
    AOT-folded body match grep / ripgrep.
- **vs ripgrep: 2/8 wins** (`[A-Z]{50,}`, `\b(if|else|...)\b`).
  Ripgrep wins or ties the rest 1.05×–25× thanks to lazy DFA +
  the Aho-Corasick / Teddy multi-pattern literal prefilter in the
  [`regex-automata`](https://github.com/rust-lang/regex/) crate.
  The `(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)` row (5314 ms vs rg
  237 ms) is the worst gap: rg extracts mid-pattern literal
  anchors (`(`, `,`, `)`) and runs SIMD AC on them; we only
  extract leading-edge alts of literals.  Tracked in
  [`docs/todo.md`](./docs/todo.md).

#### What grep still wins

`(\d+\.\d+\.\d+\.\d+)` (93 ms) — the leading `\d` class is rare
in C source, so grep's `\d`-led DFA + Boyer-Moore-Horspool skips
ahead by needle length.  Our `greedy_class` walks every byte via
SIMD — same throughput, but the byte budget is fundamentally
larger.  Algorithmic miss, not a SIMD one.

See [`docs/perf.md`](./docs/perf.md) and
[`docs/runtime.md`](./docs/runtime.md) for the architectural
lesson ("wrap algorithms as nodes; the framework's bake / hash /
code-store sharing then composes with them for free").

## What it does NOT do (yet)

- Real PG profile signal (`HOPT == HORG` for now — there's no
  profile signal to bake yet).
- Unicode case folding for non-ASCII, `\p{...}` properties, multi-
  byte characters inside `[...]` (multi-byte `\u` outside `[]`
  works fine via UTF-8 byte expansion).
- EUC-JP / Windows-31J encodings.
- The `(?~e)` absence operator uses the simple `(?:(?!e).)*`
  semantics; Onigmo's stricter "no contiguous substring matches"
  length can differ for unanchored cases.
- `Regexp.new(string)` from the CLI front; the Ruby C extension
  `ASTrogre.compile(string)` already exposes the runtime-string
  compile path.
- Mid-pattern literal extraction (Teddy / multi-pattern AC across
  arbitrary positions).  Today's AC prefilter only handles the
  leading-edge alternation case.

What landed recently and ISN'T in this list anymore:
- Lookbehind `(?<=...)` / `(?<!...)` — fixed-width, alt-of-fixed,
  and a variable-width fallback.
- Atomic groups `(?>...)` and the possessive `*+ ++ ?+` forms.
- Conditional `(?(N)yes|no)` and `\g<>` subroutine calls
  (recursive, with a dynamic stack guard).
- Inline comments `(?#...)`, `\u` escapes, character-class set
  intersection `[a-z&&[^aeiou]]`, the absence operator `(?~e)`.
- Onigmo-style MatchCache memoization for ReDoS-prone patterns.
- `MatchData` equivalent + `match` / `match?` / `=~` / `===` /
  `scan` / `match_all` on the Ruby extension side.

See [`docs/done.md`](./docs/done.md) and
[`docs/todo.md`](./docs/todo.md) for the full status.

## References

- ASTro framework: top-level [README](../../README.md) and
  [`docs/idea.md`](../../docs/idea.md).
- Onigmo: <https://github.com/k-takata/Onigmo> — cloned at build
  time with `make WITH_ONIGMO=1`.
