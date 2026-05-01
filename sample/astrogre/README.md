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
- The bake is structurally correct (every inner node's SD is exposed
  via dlsym through a thin extern wrapper, all chain dispatchers are
  re-resolved post-build) but the speed gain on grep-shaped workloads
  is small.  See `docs/perf.md` for the analysis.

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

## Speed comparison

`bench/grep_bench.sh` runs each tool on a 118 MB corpus
(C/header source from a few in-tree samples, replicated) and reports
best-of-5 wall-clock seconds.  All times in seconds.

```
                         grep   ripgrep  +onigmo  interp  aot-cached
literal    /static/      0.002   0.035    0.221   0.884   0.872
rare       /specialized_dispatcher/
                         0.035   0.020    0.190   0.855   0.866
anchored   /^static/     0.002   0.035    0.227   0.641   0.643
case-i     /VALUE/i      0.002   0.051    0.273   0.724   0.735
alt-3      /static|extern|inline/
                         0.002   0.048    1.131   2.010   1.950
class-rep  /[0-9]{4,}/   0.002   0.054    0.715   1.284   1.296
ident-call /[a-z_]+_[a-z]+\(/
                         0.002   0.183    3.253   3.814   3.792
count -c   /static/      0.002   0.027    0.218   0.882   0.867
```

What this says:

- **ugrep / ripgrep are not really competing**: SIMD memchr +
  literal-prefix prefilters / lazy DFA put them an order of
  magnitude ahead of v1 work.
- **astrogre vs Onigmo** is the apples-to-apples comparison (both
  are tree/bytecode backtracking engines without a literal
  prefilter).  Onigmo is currently 2–4× faster on the bulk of cases.
- **interp vs aot-cached** is essentially noise.  The dispatch chain
  AOT removes is already cheap (3 indirect calls per position, all
  predicted by the BTB).  The big lever for our shape — fusing the
  search loop into the SD itself — is on the runway, not landed.

See [`docs/perf.md`](./docs/perf.md) for what's tried + landed and
the runway items (search-loop fusion, literal-prefix prefilter, real
PG signal, JIT) that should close the Onigmo gap.

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
