# sample/astrogre — Ruby-style regex engine on ASTro

`astrogre` (pronounced *astrouga*) is an experimental Ruby-style
regex engine implemented as an ASTro sample, plus a small grep front-end
on top.  The matcher itself is the "engine"; the binary is the grep CLI.

The matcher is itself an AST: `node.def` defines ~22 match-node kinds
(literal, char-class, dot, anchors, repetition, alternation, capture,
lookahead, backreference) and ASTroGen turns that into a tree-walking
interpreter.  Patterns enter through a small recursive-descent regex
parser (`parse.c`) that takes the bytes between the slashes plus a
flag bitmask and produces the AST.  No external regex parser is
required.

The binary `./astrogre` is a grep tool that uses the `astrogre`
engine by default.  An [Onigmo](https://github.com/k-takata/Onigmo)
backend can be swapped in at runtime with `--backend=onigmo` for
direct head-to-head comparison.

Two CLIs ship in this directory:

- **`./astrogre`** — the original grep front-end on top of the
  engine, kept close to GNU grep semantics for benching: explicit
  PATH required, no recursion default, `-r` to descend, all flags
  including the lib-internal ones (`--self-test`, `--bench`,
  `--bench-file`, `--dump`, `--plain`, `--aot-compile`,
  `--backend=`, `--verbose`).  This is the binary used in the
  benches below.
- **`./are/are`** — production CLI with modern defaults (recursive
  by default, `.gitignore`-aware, parallel walking via `-j N`,
  binary skip, type filter `-t LANG`).  Same regex engine, just a
  user-facing front end with rg/ag-style ergonomics — see
  [`are/README.md`](./are/README.md) for its own benches and
  options.

## Status

- 44/44 self-tests pass: literals (`/i`), `.` (with `/m`), char classes,
  anchors, greedy/lazy quantifiers, capturing/non-capturing/named groups,
  back-references, lookahead, alternation, `/x` extended, inline flag
  groups, `search_from` enumeration, fixed-string mode.
- Standalone `/pat/flags` source-string syntax via `astrogre_parse_literal`.
- grep CLI: `-i -n -c -v -w -F -l -L -H -h -o -r -e --color=auto`.
- Backend abstraction: `--backend=astrogre` (default) or `--backend=onigmo`.
- AOT specialization wired up: `--aot-compile` (`-C`) writes
  `code_store/c/SD_<hash>.c`, builds `all.so`, and patches every node's
  dispatcher.  Subsequent runs (default mode) auto-load the cached
  build via `astro_cs_load` in `OPTIMIZE`.  `--pg-compile` (`-P`) is
  accepted for parity with abruby but currently aliases to AOT —
  there's no profile signal to bake yet.
- The for-each-start-position search loop is itself a node
  (`node_grep_search`), so the specializer fuses the loop AND the
  inlined regex chain into one SD function — the in-engine bench
  shows up to **7.2× over interp on long-buffer searches**.
- Algorithmic prefilter nodes — `node_grep_search_memchr` /
  `_memmem` — emitted when the parser detects a literal first byte
  / prefix.  3-4× win on literal-led grep patterns, bringing
  astrogre to within 20 % of Onigmo on those cases.

## Encoding

Two encoding modes, set per-pattern via the regex flag:

| flag      | mode  | dot advances by | typical use                       |
|-----------|-------|-----------------|-----------------------------------|
| `/n`      | ASCII | 1 byte          | binary input / ASCII-only         |
| `/u`      | UTF-8 | 1 codepoint     | default in modern Ruby            |
| (default) | UTF-8 | 1 codepoint     | same as `/u`                      |

What works under either mode:

- ASCII literals, ranges (`[a-z]`, `\d`, `\w`, `\s`), anchors,
  alternation, repetition, captures, backrefs.
- `\b` / `\B` use the 7-bit ASCII word predicate (`[A-Za-z0-9_]`).
- `/i` lowercases ASCII letters at parse time.
- The SIMD prefilter ladder operates on raw bytes and composes
  cleanly with UTF-8 patterns (the body chain re-verifies the
  full codepoint at each candidate).

What's UTF-8-aware:

- `.` advances one full codepoint via the leading-byte cascade
  (`0xxxxxxx` / `110xxxxx` / `1110xxxx` / `11110xxx`); invalid
  leads don't match.
- Multi-byte UTF-8 literals are kept as one `IRE_LIT` token by
  the parser, so quantifiers bind to the codepoint
  (`/é+/` quantifies `é`, not its trailing 0xA9).

What's not supported (see `docs/todo.md`):

- Multi-byte characters inside `[...]`.
- `\p{...}` Unicode property classes.
- Unicode case folding for `/i`.
- `\X` extended grapheme cluster.
- EUC-JP (`/e`) and Windows-31J (`/s`).

`docs/runtime.md` has the full breakdown including how the SIMD
prefilter nodes interact with each encoding.

## Build and run

```sh
cd sample/astrogre

# In-house engine only (default)
make

# With Onigmo as a switchable backend
make WITH_ONIGMO=1

# grep usage
./astrogre 'foo[0-9]+' file.txt
./astrogre -n -i 'pattern' *.c
./astrogre -r -F 'literal_str' src/
./astrogre --backend=onigmo --color=always 'pat' file.txt
./astrogre -C 'pat' file.txt          # AOT-compile pattern, then run
./astrogre --plain 'pat' file.txt     # bypass code store entirely

# Engine-level
./astrogre --self-test               # 44-case self-test
./astrogre --bench                   # in-engine microbench
./astrogre --dump '/(a|b)*c/'        # show compiled AST
make bench-rg                        # cross-tool grep comparison
make bench-tree                      # recursive-walk comparison
make bench-aot                       # AOT-favourable engine bench
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
selftest.c            engine self-test + microbench
main.c                grep CLI (file walk, options, color, --backend)
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

   The bake then composes — first byte / range bounds / packed
   bytes / 16-byte nibble tables become AVX2 set1 immediates inside
   the SD; the inner loop has no indirect call.

### Bench A — grep CLI (118 MB C-source corpus, best-of-3 ms)

`bench/grep_bench.rb` — see that script for the full setup
(re-runnable with `make bench-rg`).  Each tool's stdout is
redirected to a regular file (NOT `/dev/null` — see the pitfall
note at the bottom of this section).

| pattern                                | astrogre interp | astrogre +AOT/cached | astrogre +onigmo | grep | ripgrep |
|----------------------------------------|---:|---:|---:|---:|---:|
| `/static/` literal                     | 71 | **40** | n/a | 153 | 86 |
| `/specialized_dispatcher/` rare        | **22** | 25 | n/a | 37 | **21** |
| `/^static/` anchored                   | 77 | 83 | n/a | **71** | 41 |
| `/VALUE/i` case-insens                 | 82 | **76** | n/a | 105 | 60 |
| `/static\|extern\|inline/` alt-3       | 389 | **128** | n/a | 204 | 64 |
| 12-way alt (AC prefilter)              | 418 | 419 | n/a | 281 | **147** |
| `/[0-9]{4,}/` class-rep                | 373 | 177 | n/a | **92** | 59 |
| `/[a-z_]+_[a-z]+\(/` ident-call        | 1911 | 1341 | n/a | 2421 | **220** |
| `-c /static/` count                    | 27 | 26 | n/a | 71 | **30** |

`+onigmo` rows are `n/a` here because this build was made without
`WITH_ONIGMO=1`; the harness records ERR when the backend isn't
linked.

#### Headline rows

- **`/static/`**: AOT is 1.8× faster than interp here (71 → 40 ms),
  closing the gap to ripgrep's 86 ms and beating GNU grep's
  150 ms.  AOT folds the per-byte alt-of-LIT body chain into the
  scanner SD; the inner loop has zero indirect calls.
- **`-c /static/`**: 26 ms — fastest tool on this row, ahead of
  ripgrep (30 ms) and well ahead of GNU grep (71 ms).
- **`/[a-z_]+_[a-z]+\(/`** (ident-call): 1.9 s for us, 220 ms for
  ripgrep, 2.4 s for GNU grep.  Both we and grep miss ripgrep's
  lazy-DFA optimisation here; AOT shaves ~30 % off interp but the
  algorithmic gap dominates.

#### Pitfall — DON'T pipe to `/dev/null` while benching grep

GNU grep since
[commit `af6af28`](https://cgit.git.savannah.gnu.org/cgit/grep.git/commit/?id=af6af288)
(Paul Eggert, May 2016, "grep: /dev/null output speedup")
`fstat`s stdout, recognises `/dev/null`, and switches to
first-match-and-exit mode — `-q`-equivalent — because the output
won't be visible anyway.  Benches that redirect to `/dev/null`
look like grep is doing 2 ms over a 118 MB file when in fact it's
short-circuiting after the first match.  `bench/grep_bench.rb`
explicitly avoids this.

### Bench B — engine-level whole-file scan (ms/iter)

`bench/aot_bench.rb` runs the in-engine `--bench-file` path: the
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
    explodes on long uppercase runs; greedy_class fast-path
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
in C source, so grep's `\d`-led DFA + Boyer-Moore-Horspool skip
ahead by needle length.  Our `greedy_class` walks every byte via
SIMD — same throughput, but the byte budget is fundamentally
larger.  Algorithmic miss, not a SIMD one.

See [`docs/perf.md`](./docs/perf.md) and
[`docs/runtime.md`](./docs/runtime.md) for the architectural lesson
("wrap algorithms as nodes; the framework's bake / hash / code-store
sharing then composes with them for free").

## What it does NOT do (yet)

- Search-loop fused SD.  Per-position AOT dispatch is already cheap;
  the bigger lever is folding the start-position loop into the
  specialized C function so the whole "scan + match" lives in one
  basic block.  Without it, AOT-cached barely beats interp on grep.
- Real PG profile signal (`HOPT == HORG` for now — `--pg-compile`
  bakes the same bytes as `--aot-compile`).
- Unicode case folding for non-ASCII, `\p{...}` properties, multi-byte
  characters inside `[...]` (multi-byte `\u` outside `[]` works fine
  via UTF-8 byte expansion).
- EUC-JP / Windows-31J encodings.
- The `(?~e)` absence operator uses the simple `(?:(?!e).)*` semantics;
  Onigmo's stricter "no contiguous substring matches" length can
  differ for unanchored cases.
- `Regexp.new(string)` from the CLI front; the Ruby C extension
  `ASTrogre.compile(string)` already exposes the runtime-string
  compile path.

What landed recently and ISN'T in this list anymore:
- Lookbehind `(?<=...)` / `(?<!...)` — fixed-width, alt-of-fixed,
  and a variable-width fallback.
- Atomic groups `(?>...)` and the possessive `*+ ++ ?+` forms.
- Conditional `(?(N)yes|no)` and `\g<>` subroutine calls (recursive,
  with a dynamic stack guard).
- Inline comments `(?#...)`, `\u` escapes, character-class set
  intersection `[a-z&&[^aeiou]]`, the absence operator `(?~e)`.
- Onigmo-style MatchCache memoization for ReDoS-prone patterns.
- `MatchData` equivalent + `match` / `match?` / `=~` / `===` / `scan`
  / `match_all` on the Ruby extension side.

See [`docs/done.md`](./docs/done.md) and
[`docs/todo.md`](./docs/todo.md) for the full status.

## References

- ASTro framework: top-level [README](../../README.md) and
  [`docs/idea.md`](../../docs/idea.md).
- Onigmo: <https://github.com/k-takata/Onigmo> — cloned at build
  time with `make WITH_ONIGMO=1`.
