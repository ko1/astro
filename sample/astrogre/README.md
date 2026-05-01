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
- Pure tree-walking interpreter for both backends; AOT specialization
  (the abruby-style "compile / cached" loop) is on the next-up list.

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
best-of-3 wall-clock seconds.  All times in seconds.

```
                          grep   ripgrep  astrogre  +onigmo
  literal    /static/    0.002    0.034    0.839    0.217
  rare       /specialized_dispatcher/
                         0.035    0.020    0.830    0.178
  anchored   /^static/   0.002    0.035    0.603    0.220
  case-i     /VALUE/i    0.002    0.049    0.699    0.254
  alt-3      /static|extern|inline/
                         0.002    0.049    1.857    1.096
  class-rep  /[0-9]{4,}/ 0.003    0.056    1.293    0.713
  ident-call /[a-z_]+_[a-z]+\(/
                         0.002    0.188    3.801    3.366
  count -c   /static/    0.002    0.027    0.841    0.218
```

What this says:

- **ugrep / ripgrep are not really competing**: they have SIMD memchr
  for plain literals + lazy DFA / Hyperscan-like prefilters for
  regex-shaped patterns.  These are an order of magnitude beyond the
  scope of v1.
- **astrogre vs Onigmo** is the apples-to-apples comparison (both are
  tree/bytecode backtracking engines without a literal prefilter).
  Onigmo is currently 2–4× faster on the bulk of cases.  The gap is
  narrowest on heavy-regex patterns (`ident-call`: 12%) and widest on
  literal / anchored cases where Onigmo's BM-class prefilter still
  fires while astrogre's plain search loop has to retry every position.

See [`docs/perf.md`](./docs/perf.md) for what's tried + landed and the
runway items (prefilter, AOT specialization, JIT) that should close
the Onigmo gap.

## What it does NOT do (yet)

- AOT / PG specialization through ASTro's code-store.  The framework
  hooks (`SPECIALIZE_node_re_*`) are generated already; the cache
  driver is not wired into `OPTIMIZE` yet.  Adding it in the
  abruby-style "compile first / cached / pg-compile / pg-cached"
  shape is the next item.
- Literal-prefix prefilter (Boyer–Moore or memchr-on-first-byte).
  This is the single biggest miss vs ripgrep.
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
