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

## Speed: AOT specialization

The for-each-start-position search loop is itself an AST node
(`node_grep_search`), so AOT specialization fuses the loop AND the
inlined regex chain into one SD function.  The in-engine
microbench (`./astrogre --bench`) shows the gain:

```
                                  interp     aot-cached   speedup
literal-tail   /match/            22.75 s      3.15 s      7.22×
class-word     /\w+/              11.60 s     10.43 s      1.11×
alt-3          /cat|dog|match/    15.67 s     12.86 s      1.22×
dot-star       /.*match/          11.04 s      9.16 s      1.20×
```

The grep CLI bench (`bench/grep_bench.sh`, 118 MB corpus, line by
line) shows essentially **no fusion gain** — per-line `getline` /
CTX init / fwrite overhead dominates the wall clock when each line
is ~36 bytes.  Folding the line iteration into the AST too is on
the runway.

```
                          grep   ripgrep   +onigmo  interp  aot-cached
literal    /static/      0.002    0.036     0.238   0.934    0.974
rare       /specialized_dispatcher/
                         0.036    0.020     0.215   1.004    1.024
anchored   /^static/     0.003    0.041     0.358   0.720    0.672
case-i     /VALUE/i      0.002    0.050     0.278   0.773    0.784
alt-3      /static|extern|inline/
                         0.003    0.055     1.199   2.122    2.348
class-rep  /[0-9]{4,}/   0.006    0.103     0.861   1.381    1.330
ident-call /[a-z_]+_[a-z]+\(/
                         0.002    0.192     3.500   3.990    3.938
count -c   /static/      0.002    0.029     0.241   0.957    0.933
```

What this says:

- **ugrep / ripgrep are not really competing**: SIMD memchr +
  literal-prefix prefilters / lazy DFA put them an order of
  magnitude ahead of v1 work.
- **astrogre vs Onigmo** is the apples-to-apples comparison (both
  are tree/bytecode backtracking engines without a literal
  prefilter).  Onigmo is currently 2–4× faster at the grep-CLI
  level; the gap is the literal-prefix prefilter Onigmo runs
  internally, which we don't have yet.

See [`docs/perf.md`](./docs/perf.md) for what's tried + landed,
including the disassembly of the fused SD and the runway items
(line iteration in the AST, literal-prefix prefilter, real PG
signal, JIT).

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
