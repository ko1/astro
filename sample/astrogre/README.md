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

## Speed: prefilter + AOT specialization

Two layers of optimisation, both expressed as ASTro nodes:

1. **`node_grep_search`** — the for-each-start-position loop.
   Putting it in the AST means the specializer fuses the loop AND
   the inlined regex chain into one SD function.  In-engine
   microbench: 7.22× over interp on `literal-tail` (16 KiB single
   buffer, repeated calls).
2. **`node_grep_search_memchr` / `_memmem`** — algorithmic
   prefilters.  When the parser detects a fixed first byte / multi-
   byte literal prefix, emit one of these instead.  glibc's AVX2
   memchr / two-way memmem skips entire KB of input per library
   call; the body chain runs only at candidate positions.

Grep-CLI bench (`bench/grep_bench.sh`, 118 MB corpus, best-of-5 s):

```
                          grep   ripgrep   +onigmo  interp  aot-cached
literal    /static/      0.002    0.037     0.240   0.285    0.287
rare       /specialized_dispatcher/
                         0.039    0.022     0.200   0.269    0.267
anchored   /^static/     0.003    0.042     0.249   0.284    0.281
case-i     /VALUE/i      0.002    0.053     0.285   0.746    0.712
alt-3      /static|extern|inline/
                         0.003    0.053     1.183   2.199    2.133
class-rep  /[0-9]{4,}/   0.002    0.059     0.749   1.384    1.392
ident-call /[a-z_]+_[a-z]+\(/
                         0.002    0.196     3.626   4.238    4.294
count -c   /static/      0.002    0.030     0.244   0.293    0.286
```

What this says:

- **prefilter-eligible patterns** (literal-led, anchored, count) are
  now **within 20 %** of Onigmo — 3-4× faster than the no-prefilter
  baseline, finally in the same ballpark as a serious backtracking
  matcher.
- **patterns the current prefilter doesn't trigger on** (case-
  insensitive, alternation, class-led) still match the no-prefilter
  baseline.  Each gets its own SIMD-y node next: twin memchr for
  `/i`, first-byte bitmap for alternation, PSHUFB for classes.
  Documented under [`docs/todo.md`](./docs/todo.md).
- **ugrep / ripgrep stay an order of magnitude ahead**.  The
  remaining gap is process startup + per-line `getline` / CTX-init
  overhead, addressable by folding line iteration into the AST too
  (`node_grep_lines`).

See [`docs/perf.md`](./docs/perf.md) and
[`docs/runtime.md`](./docs/runtime.md) for the architectural lesson
("wrap algorithms as nodes; the framework's bake / hash / code-store
sharing then compose with them for free") and the disassembly of
the fused SD.

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
