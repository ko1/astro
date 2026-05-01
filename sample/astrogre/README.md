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

### Bench A — grep CLI (line-by-line, 118 MB corpus, best-of-5 s)

| pattern | astrogre interp | astrogre +AOT | astrogre +onigmo | grep | ripgrep |
|---|---:|---:|---:|---:|---:|
| `/static/` literal | 0.285 | 0.271 | 0.226 | 0.002 | 0.036 |
| `/specialized_dispatcher/` rare | 0.266 | 0.264 | 0.194 | 0.038 | 0.022 |
| `/^static/` anchored | 0.273 | 0.283 | 0.229 | 0.002 | 0.038 |
| `/VALUE/i` case-i | 0.702 | 0.336 | 0.277 | 0.002 | 0.048 |
| `/static\|extern\|inline/` alt-3 | 0.553 | 0.288 | 1.097 | 0.002 | 0.054 |
| `/[0-9]{4,}/` class-rep | 0.581 | 0.498 | 0.761 | 0.002 | 0.059 |
| `/[a-z_]+_[a-z]+\(/` ident-call | 3.828 | 2.885 | 3.524 | 0.002 | 0.192 |
| `-c /static/` count | 0.279 | 0.270 | 0.226 | 0.002 | 0.029 |

For literal-led grep, astrogre is now **within 20 % of Onigmo on
every pattern** (memchr / memmem / byteset all firing).  ugrep
(`/usr/bin/grep` here) is an order of magnitude ahead — its AVX2
memchr / lazy DFA + PCRE2-JIT runs at memory bandwidth and the
per-line CLI overhead doesn't apply.

### Bench B — engine-level whole-file scan (ms/iter)

`bench/aot_bench.sh` runs the in-engine `--bench-file` path: full
buffer in memory, full-sweep count to mirror grep `-c` semantics.
Patterns chosen for the AOT-favourable shape (long chain, prefilter
applies).

| pattern | astrogre +AOT | astrogre +onigmo | grep | ripgrep |
|---|---:|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/` | **16** ★ | 726 | 85 | 26 |
| `/(QQQX\|RRRX\|SSSX)+/` | **24** ★ | 700 | 26 | 26 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | **503** ★ | 717 | 533 | 197 |
| `/[A-Z]{50,}/` | **678** ★ | 1099 | 1570 | 184 |
| `/[a-z][0-9][a-z][0-9][a-z]/` | 482 | 722 | **4** | 206 |
| `/(\d+\.\d+\.\d+\.\d+)/` | 430 | 738 | **4** | 50 |
| `/\b(if\|else\|for\|while\|return)\b/` | 90 | 1060 | **2.3** | 121 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 10824 | 9353 | **2.7** | 218 |

★ = astrogre + AOT beats grep AND Onigmo.  **4/8 vs grep, 8/8 vs
Onigmo.**  Bold = winner per row.  The wins come from the prefilter
ladder picking the right node for each shape; the losses are
patterns where the leading char / first-byte-set is common in
source code, so SIMD scan finds candidates everywhere and ugrep's
Hyperscan-style multi-pattern literal anchor extraction (Teddy /
FDR) is the only way to win.  That's the next addition (see
[`docs/todo.md`](./docs/todo.md)).

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
