# sample/astrogre — Ruby-style regex engine on ASTro

`astrogre` (pronounced *astrouga*) is an experimental Ruby-style
regex engine implemented as an ASTro sample, plus a small grep front-end
on top.  The matcher itself is the "engine"; the binary is the grep CLI.

The matcher is itself an AST: `node.def` defines ~22 match-node kinds
(literal, char-class, dot, anchors, repetition, alternation, capture,
lookahead, backreference) and ASTroGen turns that into a tree-walking
interpreter.  The front-end uses [prism](https://github.com/ruby/prism)
to parse Ruby source, find a `PM_REGULAR_EXPRESSION_NODE`, and feed
its unescaped body + flag bits into a small recursive-descent regex
parser that produces our AST.

The binary `./astrogre` is a grep tool that uses the `astrogre`
engine by default.  An [Onigmo](https://github.com/k-takata/Onigmo)
backend can be swapped in at runtime with `--backend=onigmo` for
direct head-to-head comparison.

## Status

- 44/44 self-tests pass: literals (`/i`), `.` (with `/m`), char classes,
  anchors, greedy/lazy quantifiers, capturing/non-capturing/named groups,
  back-references, lookahead, alternation, `/x` extended, inline flag
  groups, `search_from` enumeration, fixed-string mode.
- prism integration + standalone `/pat/flags` syntax.
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
./astrogre --via-prism 'p /\d+/i' input.txt   # extract regex via prism
bench/grep_bench.sh 3                # cross-tool grep speed comparison
```

## Layout

```
node.def              22 match-node kinds (continuation-passing form)
parse.c / parse.h     prism integration + recursive-descent regex parser
match.c               astrogre_search / astrogre_search_from + rep_cont singleton
backend.h             abstract ops table (compile/search/free)
backend_astrogre.c    in-house engine bound to backend_ops_t
backend_onigmo.c      Onigmo engine bound to backend_ops_t (WITH_ONIGMO=1)
node.c                ASTro framework glue (hash funcs, EVAL, OPTIMIZE)
selftest.c            engine self-test + microbench
main.c                grep CLI (file walk, options, color, --backend)
bench/grep_bench.sh   cross-tool comparison harness
prism                 symlink to ../naruby/prism (Ruby parser)
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

### Bench A — grep CLI (line-by-line, 118 MB corpus, best-of-5 ms)

| pattern | astrogre interp | astrogre +AOT | astrogre +onigmo | grep | ripgrep |
|---|---:|---:|---:|---:|---:|
| `/static/` literal | 66 | 64 | 98 | **2** | 34 |
| `/specialized_dispatcher/` rare | 25 | 25 | 37 | 35 | **20** |
| `/^static/` anchored | 68 | 65 | 98 | **2** | 35 |
| `/VALUE/i` case-i | 597 | 579 | 134 | **2** | 48 |
| `/static\|extern\|inline/` alt-3 | 290 | 294 | 920 | **2** | 49 |
| `/[0-9]{4,}/` class-rep | 470 | 472 | 558 | **2** | 54 |
| `/[a-z_]+_[a-z]+\(/` ident-call | 3283 | 3297 | 3159 | **2** | 182 |
| `-c /static/` count | **24** | **25** | 72 | 2 | 27 |

The whole-file mmap path (used when the pattern has a SIMD/libc
prefilter) drops literal-led astrogre by 3-10× and **astrogre is
within ripgrep speed on rare-literal patterns**
(`literal-rare` 25 ms vs ripgrep's 20 ms; even slightly faster
than ugrep's 35 ms there).  ugrep stays an order of magnitude
ahead on common literals (memchr at memory-bandwidth).

The `-c /static/` row is the **only one where astrogre beats
ripgrep** (24 ms vs 27 ms): the per-line counting loop itself was
folded into a dedicated AST node `node_grep_count_lines_lit`
(Hyperscan-style dual-byte filter + 64-byte stride + AOT-baked
needle), bringing it within ~10× of grep's 2 ms. Run with
`--verbose` to see the wall-clock split — most of the remaining
gap is mmap+munmap of a 118 MB file (PTE bookkeeping), not the scan
itself. See [`docs/done.md`](./docs/done.md) for details.

The `/i`, `class-rep`, `ident-call` rows fall back to per-line
streaming because the engine has no fast scan for the leading
`\w` / `[0-9]` / `/i` shape — see Bench B for what AOT does once
the per-line overhead is gone.

### Bench B — engine-level whole-file scan (ms/iter)

`bench/aot_bench.sh` runs the in-engine `--bench-file` path: full
buffer in memory, full-sweep count to mirror grep `-c` semantics.
Patterns chosen for the AOT-favourable shape (long chain, prefilter
applies).  Lower is better; bold = winner per row, ★ = astrogre+AOT
beats grep AND Onigmo.

| pattern | astrogre interp | astrogre +AOT | astrogre +onigmo | grep | ripgrep |
|---|---:|---:|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/` | 21 | **13** ★ | 568 | 86 | 25 |
| `/(QQQX\|RRRX\|SSSX)+/` | 45 | **39** ★ | 977 | 54 | 51 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | 1717 | **455** ★ | 562 | 572 | 209 |
| `/[A-Z]{50,}/` | 793 | **658** ★ | 920 | 1526 | 184 |
| `/\b(if\|else\|for\|while\|return)\b/` | 241 | 79 | 985 | **2** | 119 |
| `/[a-z][0-9][a-z][0-9][a-z]/` | 1127 | 476 | 596 | **4** | 214 |
| `/(\d+\.\d+\.\d+\.\d+)/` | 596 | 421 | 566 | **4** | 50 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 13381 | 11475 | 14260 | **2** | 216 |

★ = astrogre + AOT beats grep AND Onigmo.  **4/8 vs grep, 8/8 vs
Onigmo.**  AOT typically lands a 1.5-3.8× speedup over interp on
this set (best: 3.77× on `[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]`).
The losses to grep are patterns where the leading char / first-byte
set is common in source code, so SIMD scan finds candidates
everywhere and ugrep's Hyperscan-style multi-pattern literal
anchor extraction (Teddy / FDR) is the only way to win.  That's
the next addition (see [`docs/todo.md`](./docs/todo.md)).

ripgrep stays a step ahead on most patterns thanks to lazy DFA +
literal-prefix prefilter; closing that gap would need DFA-style
NFA simulation, a bigger refactor.

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
- Literal-prefix prefilter (Boyer–Moore or memchr-on-first-byte).
  Single biggest miss vs ripgrep.
- Lookbehind `(?<=...)` / `(?<!...)`.
- Atomic groups `(?>...)` / possessive quantifiers — parsed, degraded
  to plain greedy.
- Unicode case folding for non-ASCII, `\p{...}` properties, multi-byte
  characters inside `[...]`.
- EUC-JP / Windows-31J encodings.
- `Regexp.new(string)` — only literal `/.../` regexes via prism.
- `gsub` / `scan` / `MatchData`-equivalent API.

See [`docs/todo.md`](./docs/todo.md) for the full backlog.

## References

- ASTro framework: top-level [README](../../README.md) and
  [`docs/idea.md`](../../docs/idea.md).
- prism `pm_regular_expression_node_t` —
  `prism/include/prism/ast.h`.  Note that prism gives us only the
  regex *source bytes* and the `/imxnu` flag bits; it does **not**
  parse the regex syntax itself (`[a-z]`, `\d`, etc.).  That's why
  this sample ships its own recursive-descent regex parser.
- Onigmo: <https://github.com/k-takata/Onigmo> — cloned at build
  time with `make WITH_ONIGMO=1`.
